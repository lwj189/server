// thread.cpp -- 单 Reactor + 线程池。
//   g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion thread.cpp -o thread -pthread
//   ./thread --workers 1 --quiet
//
// 和 epoll.cpp 的区别只在谁来干活：
//   epoll.cpp   主线程：收连接 → 读 → 解析 → 算业务 → 发响应
//   thread.cpp  主线程：收连接 → 读 → 解析 ──┐
//                                             └→ 线程池：算业务 → 发响应
// IO 和 CPU 分开，主线程不用为算一个请求停下来。但这个收益只在业务真吃 CPU 时才有：
// 业务只是返回固定字符串的话，线程池只带来队列和锁的开销，吞吐反而更低。
// 所以这里带了一个真吃 CPU 的路由 /work?n=NNNN，用它才测得出区别。
//
// fd 交接协议（本文件最容易出错的地方）：
//   主线程 ① setBusy(fd, true)   标记这条连接归工人了
//          ② EPOLL_CTL_DEL       摘掉，否则 LT 下主线程和工人会同时操作同一个 fd
//          ③ pool.submit(...)    打包丢进队列
//   工人   ④ send 响应
//          ⑤ 要关就 eraseCtx + close；要留就 setBusy(false) + EPOLL_CTL_ADD
// 第 ⑤ 步的"还回去"是 keep-alive 的基础，没有它一条连接只能服务一个请求。

#include "http.hpp"          // ClientContext / parseRequest / buildResponse / logLine
#include "thread_pool.hpp"   // ThreadPool

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

constexpr uint16_t PORT = 8888;
constexpr size_t DEFAULT_WORKERS = 4;
constexpr size_t MAX_EVENTS = 16;

// /work 的 n 上限：业务参数由客户端给，不设上限就是一个请求打满 CPU
constexpr unsigned long long MAX_WORK_UNITS = 20'000'000ULL;
constexpr unsigned long long DEFAULT_WORK_UNITS = 200'000ULL;

// 跨线程共享的状态。真正需要锁的只有 g_clients 这张表：
// 主线程 emplace / erase，工人要改 ctx.busy。
// inbuf 不用锁：fd 交出去之前已经 EPOLL_CTL_DEL 了，主线程不会再碰它的 inbuf。
static ThreadPool* g_pool = nullptr;
static int g_epoll_fd = -1;
static std::mutex g_clients_mutex;
static std::unordered_map<int, ClientContext> g_clients;

// 业务：真的吃 CPU。
// 用 xorshift64 而不是空循环，因为每一步都依赖上一步的结果，编译器没法优化掉；
// 空循环在 -O2 下会被整个删掉，测出来是假的。
static uint64_t burnCpu(unsigned long long n) {
    uint64_t x = 0x9e3779b97f4a7c15ULL;
    for (unsigned long long i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x += i;
    }
    return x;
}

// 从 "GET /work?n=100 HTTP/1.1\r\n..." 里取第二段
static std::string requestTarget(const std::string& raw) {
    const size_t sp1 = raw.find(' ');
    if (sp1 == std::string::npos) return "/";
    const size_t sp2 = raw.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return "/";
    return raw.substr(sp1 + 1, sp2 - sp1 - 1);
}

// 切出 path："/work?n=100" -> "/work"
// 必须切开再拿整串比。以前是 target.rfind("/work", 0) == 0 的前缀匹配，
// 它把 "/worker"、"/workshop" 也吃进来 —— 一个和业务无关的路径
// 只要起手是这五个字符就能让服务器烧 CPU。
static std::string pathOf(const std::string& target) {
    const size_t q = target.find('?');
    return target.substr(0, q == std::string::npos ? target.size() : q);
}

// 从 "n=100" / "a=1&n=100" 里取 n，没有就用默认值，超上限截到上限。
// 必须按 & 切参数而不是直接 find("n=")："?foo=n=5" 里也含 "n="，
// 直接找会把它当成 n=5。只有整个参数名恰好是 n 才算数。
static unsigned long long workUnits(const std::string& query) {
    size_t pos = 0;
    while (pos < query.size()) {
        const size_t amp = query.find('&', pos);
        const size_t end = (amp == std::string::npos) ? query.size() : amp;
        if (end - pos > 2 && query.compare(pos, 2, "n=") == 0) {
            const char* p = query.c_str() + pos + 2;
            char* stop = nullptr;
            const unsigned long long v = std::strtoull(p, &stop, 10);
            // 值必须是纯数字，空的、后面跟着非法字符都算没给
            if (stop != p && (*stop == '\0' || *stop == '&')) {
                return v > MAX_WORK_UNITS ? MAX_WORK_UNITS : v;
            }
            return DEFAULT_WORK_UNITS;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return DEFAULT_WORK_UNITS;
}

// 算响应体，就是"业务处理"
static std::string doBusiness(const std::string& raw) {
    const std::string target = requestTarget(raw);
    const std::string path = pathOf(target);

    if (path != "/work") {
        // 普通请求返回和 epoll.cpp 一样的内容，p2_test 的 23 项才对两个版本都成立
        return "<h1>Hello from my own server!</h1>";
    }

    const size_t q = target.find('?');
    const std::string query = (q == std::string::npos) ? std::string() : target.substr(q + 1);
    const unsigned long long n = workUnits(query);
    const uint64_t sum = burnCpu(n);
    return "<h1>CPU work done</h1><p>units=" + std::to_string(n) +
           "</p><p>checksum=" + std::to_string(sum) + "</p>";
}

// 账本操作，都要拿锁
static bool ctxBusy(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    const auto it = g_clients.find(fd);
    return it != g_clients.end() && it->second.busy;
}

// 返回的指针只在下次往表里插入/删除之前有效。主线程处理单个事件期间不会
// 插删别的连接，所以够用。
static ClientContext* findCtx(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    const auto it = g_clients.find(fd);
    return it == g_clients.end() ? nullptr : &it->second;
}

static void eraseCtx(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    if (g_clients.erase(fd) == 0) {
        logLine("账本里没有 fd=" + std::to_string(fd) + " —— 有地方漏销账");
    }
}

static void setBusy(int fd, bool busy) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    const auto it = g_clients.find(fd);
    if (it != g_clients.end()) {
        it->second.busy = busy;
    }
}

// 收连接时必须先 eraseCtx 再 close，反过来要出事（实测踩到过）：
//   工人   close(fd=5)                号码 5 立刻可以被内核复用
//   主线程 accept 拿到 fd=5 → emplace 失败 → 哨兵冤枉地喊漏 erase
//   工人   eraseCtx(5)                把新连接的账销掉了
// 新连接不在账本里 → findCtx 返回 nullptr → 没人读没人关，fd 一直漏。
// 一句话：fd 号会被复用，销账必须发生在归还号码之前。

// 工人处理一个已经解析好的请求。返回 false = 这条连接已经收掉了。
// send 失败必须当断线处理。最初只 perror 一句然后照样还回去，结果是主线程
// 捧着一条死连接等一个永远不来的 EPOLLIN，最后撞 EMFILE。
// 部分写（sent < size）同样收掉：没有输出缓冲和 EPOLLOUT，宁可丢一个响应，
// 也不能留一条状态对不上的连接。
static bool workerHandleOne(int fd, const std::string& raw, bool want_close) {
    const std::string body = doBusiness(raw);
    const std::string response = buildResponse(body, want_close);

    bool broken = false;
    ssize_t sent = send(fd, response.c_str(), response.size(), 0);
    while (sent < 0 && errno == EINTR) {   // 被信号打断，这次没发出去，重来
        sent = send(fd, response.c_str(), response.size(), 0);
    }
    if (sent < 0) {
        perror("send");   // EPIPE / ECONNRESET / EAGAIN 都是"这条连接不可用了"
        broken = true;
    } else if (static_cast<size_t>(sent) < response.size()) {
        logLine("send 只发出一部分 fd=" + std::to_string(fd) + "（" +
                std::to_string(sent) + "/" + std::to_string(response.size()) +
                "）—— 没有输出缓冲，只能丢弃这条连接");
        broken = true;
    }

    if (want_close || broken) {
        // 客户端要求关 / 请求非法 / 已经断了 → 两本账一起销
        eraseCtx(fd);   // 先销账
        close(fd);      // 再归还号码
        return false;
    }
    return true;
}

// 工人线程：算业务 → 发响应 → 把 fd 还回去或收掉。
// 参数全按值捕获。不能传 ClientContext 的引用：那条连接的上下文归主线程管。
//
// 中间那个 while 是踩出来的：客户端一次发两个请求（粘包），主线程只解析出第一个
// 就交出去了，第二个还在 ctx.inbuf 里 —— 那是用户态缓冲区，内核里没有新数据，
// 所以就算把 fd 还回去 epoll 也不会报 EPOLLIN，第二个请求永远没人管。
// 工人必须自己把 inbuf 里排队的请求处理干净。
static void handleInPool(int fd, std::string raw, bool want_close) {
    if (!workerHandleOne(fd, raw, want_close)) {
        return;   // 已经关了
    }

    while (true) {
        std::string next_raw;
        bool next_close = false;
        bool bad = false;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            const auto it = g_clients.find(fd);
            if (it == g_clients.end()) return;   // 主线程先收掉了

            const ParsedRequest pr = parseRequest(it->second);
            if (pr.result == ParseResult::NeedMore) break;
            if (pr.result == ParseResult::Rejected) {
                bad = true;
            } else {
                next_raw = pr.raw;
                next_close = it->second.want_close;
                it->second.inbuf.erase(0, pr.total);
            }
        }
        if (bad) {
            eraseCtx(fd);   // 先销账
            close(fd);
            return;
        }
        if (!workerHandleOne(fd, next_raw, next_close)) return;
    }

    // 把 fd 还回去，让主线程继续照看这条连接，keep-alive 靠这一步。
    // 顺序讲究：先清 busy 再 ADD。反过来主线程可能在 busy 还是 true 时收到事件，
    // 被 continue 掉，事件就丢了。
    setBusy(fd, false);

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        // 这是双泄漏：ADD 失败后主线程再也收不到这条连接的事件，
        // 而 fd 还开着、账本条目也还在。epoll 实例满（ENOSPC）会走到这。
        perror("epoll_ctl: ADD back");
        eraseCtx(fd);
        close(fd);
    }
}

// 主线程把一个完整请求交给线程池
static void handOffToPool(ClientContext& ctx, size_t total) {
    const std::string raw = ctx.inbuf.substr(0, total);
    const bool want_close = ctx.want_close;

    if (g_log_requests) {
        logLine("收到完整请求（交给工人）:\n" + raw);
    }

    // ① 标记 + 消费字节。inbuf 归主线程管，趁现在处理干净。
    // busy 是跨线程的，必须走 setBusy，不能裸写字段。
    setBusy(ctx.fd, true);
    ctx.inbuf.erase(0, total);

    // ② 从 epoll 摘掉，不摘的话 LT 会继续通知主线程，两边同时操作同一个 fd。
    //    DEL 时 event 参数传 nullptr，内核会忽略它；传 &ev 只会让人以为有用。
    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, ctx.fd, nullptr);

    // ③ 丢进队列。按值捕获，别传引用。
    const int fd = ctx.fd;
    g_pool->submit([fd, raw, want_close] { handleInPool(fd, raw, want_close); });
}

// 命令行参数解析，失败不抛异常。
// 不能用 std::stoul：它遇到 "abc" 会抛 invalid_argument 直接 terminate，
// "-1" 会静默回绕成 18446744073709551615，"12x" 会返回 12 把尾巴吞掉。
// 换成 strtoull + 全串校验，范围再由调用方卡。
static bool parseUnsigned(const char* s, unsigned long long& out) {
    if (s == nullptr || *s == '\0' || *s == '-') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;   // 溢出 / 没读到数 / 尾巴有垃圾
    out = v;
    return true;
}

int main(int argc, char** argv) {
    size_t workers = DEFAULT_WORKERS;
    uint16_t port = PORT;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        unsigned long long v = 0;
        if (a == "--workers" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], v) || v == 0 || v > 1024) {
                std::cerr << "--workers 需要 1..1024 的整数，收到: " << argv[i] << "\n";
                return 2;
            }
            workers = static_cast<size_t>(v);
        } else if (a == "--port" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], v) || v == 0 || v > 65535) {
                std::cerr << "--port 需要 1..65535 的整数，收到: " << argv[i] << "\n";
                return 2;
            }
            port = static_cast<uint16_t>(v);
        } else if (a == "--quiet") {
            g_log_requests = false;
        } else {
            std::cout << "用法: " << argv[0] << " [--workers N] [--port N] [--quiet]\n";
            return (a == "--help" || a == "-h") ? 0 : 2;
        }
    }
    if (workers == 0) workers = 1;

    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt"); return 1;
    }

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, SOMAXCONN) < 0) {   // backlog 拉满，同 epoll.cpp 的教训
        perror("listen"); return 1;
    }

    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1 || fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl"); return 1;
    }

    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl: server_fd"); return 1;
    }

    ThreadPool pool(workers);   // 析构顺序：pool 必须比 g_pool 活得久
    g_pool = &pool;

    logLine("thread ready: workers=" + std::to_string(workers) + ", port=" +
            std::to_string(port) + "（单 Reactor + 线程池）");

    struct epoll_event events[MAX_EVENTS];
    bool accept_paused = false;
    size_t paused_at = 0;

    while (true) {
        // EMFILE 撞墙后的恢复判断，和 epoll.cpp 同一套
        if (accept_paused) {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            if (g_clients.size() < paused_at) {
                if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == 0) {
                    accept_paused = false;
                    logLine("fd 腾出来了 —— 恢复接收新连接");
                }
            }
        }

        const int nfds = epoll_wait(g_epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server_fd) {
                // 总机：新连接
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    const int conn_fd =
                        accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EMFILE) {
                            logLine("fd 用光了 —— 暂停接收，等连接释放");
                            epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, server_fd, &ev);
                            accept_paused = true;
                            {
                                std::lock_guard<std::mutex> lock(g_clients_mutex);
                                paused_at = g_clients.size();
                            }
                            break;
                        }
                        perror("accept");
                        break;
                    }

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    if (g_log_requests) {
                        logLine("新连接 fd=" + std::to_string(conn_fd) + " 来自 " + ip + ":" +
                                std::to_string(ntohs(client_addr.sin_port)));
                    }

                    int cflags = fcntl(conn_fd, F_GETFL, 0);
                    if (cflags == -1 ||
                        fcntl(conn_fd, F_SETFL, cflags | O_NONBLOCK) == -1) {
                        perror("fcntl conn");
                        close(conn_fd);
                        continue;
                    }

                    struct epoll_event cev{};
                    cev.events = EPOLLIN;
                    cev.data.fd = conn_fd;
                    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, conn_fd, &cev) < 0) {
                        perror("epoll_ctl: conn_fd");
                        close(conn_fd);
                        continue;
                    }

                    ClientContext ctx;
                    ctx.fd = conn_fd;
                    bool dup = false;
                    {
                        std::lock_guard<std::mutex> lock(g_clients_mutex);
                        dup = !g_clients.emplace(conn_fd, ctx).second;
                    }
                    if (dup) {
                        // 账本里那条旧条目还占着这个号码，之后 findCtx 拿到的是旧连接的
                        // ctx，新连接的数据会写进旧连接的缓冲。哨兵既然喊了漏 erase，
                        // 这条连接就当毒药处理，当场关掉。
                        logLine("账本里已经有 fd=" + std::to_string(conn_fd) +
                                " —— 上一个连接漏了 erase，丢弃这条新连接");
                        close(conn_fd);   // close 会自动从 epoll 摘除，不必再 DEL
                        continue;         // 回去 accept 下一条
                    }
                }
            } else {
                // 分机：有数据 / 对端关了
                const int fd = events[i].data.fd;

                // 这条连接在工人手里，主线程不许碰，也别 read
                if (ctxBusy(fd)) continue;

                ClientContext* ctx = findCtx(fd);
                if (ctx == nullptr) continue;   // 账本里没这条，不该发生

                char buf[1024];
                bool gone = false;
                while (true) {
                    const ssize_t n = read(fd, buf, sizeof(buf));

                    if (n > 0) {
                        ctx->inbuf.append(buf, static_cast<size_t>(n));

                        // 一次可能到好几个请求，逐个解析，但交出去一个就得停：
                        // 这条连接已经归工人了，主线程不能再动它
                        while (true) {
                            const ParsedRequest pr = parseRequest(*ctx);
                            if (pr.result == ParseResult::NeedMore) break;
                            if (pr.result == ParseResult::Rejected) {
                                logLine("请求非法 —— 丢弃 fd=" + std::to_string(fd));
                                eraseCtx(fd);   // 先销账
                                close(fd);
                                gone = true;
                                break;
                            }
                            handOffToPool(*ctx, pr.total);
                            gone = true;   // 已经交出去了，这一轮不再处理这条连接
                            break;
                        }
                        if (gone) break;
                    } else if (n == 0) {
                        eraseCtx(fd);   // 先销账
                        close(fd);
                        gone = true;
                        break;
                    } else {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("read");
                        eraseCtx(fd);   // 先销账
                        close(fd);
                        gone = true;
                        break;
                    }
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
