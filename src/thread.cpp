// ===========================================================================
// src/thread.cpp：单 Reactor + 线程池
//
// 和 src/epoll.cpp 的唯一区别是【谁来干活】：
//
//   src/epoll.cpp       主线程：收连接 → 读 → 解析 → 算业务 → 发响应   （全程一个线程）
//   src/thread.cpp 主线程：收连接 → 读 → 解析 ──┐
//                                              └→ 线程池：算业务 → 发响应
//
// 为什么要这么拆？
//   IO 和 CPU 是两件事。主线程不该为了"算一个请求"停下来 ——
//   它一停，所有别的连接都得等。把计算丢给工人，主线程立刻回去照看别的连接。
//
//   ⚠ 但这个收益【只在业务真的吃 CPU 时才存在】。如果业务就是返回一个固定
//     字符串，那线程池只会带来队列 + 锁 + 唤醒的开销，吞吐反而更低。
//     所以本文件带了一个真的吃 CPU 的路由：/work?n=NNNN
//     —— 用它才能把"线程池有用"这件事测出来。（对照数据见 notes/）
//
// fd 的交接协议（这是本文件最需要小心的地方）：
//   主线程解析出完整请求后：
//     ① ctx.busy = true      标记"这条连接现在归工人管"
//     ② EPOLL_CTL_DEL        从 epoll 摘掉 —— 否则 LT 模式下它会继续通知主线程，
//                            主线程就会和工人同时操作同一个 fd（数据竞争）
//     ③ pool.submit(...)     把 (fd, 请求) 打包丢进队列
//   工人干完：
//     ④ send 响应
//     ⑤ 要关就 close + erase；要留就 ctx.busy = false + EPOLL_CTL_ADD 还回去
//
//   ★ 第 ⑤ 步的"还回去"是本文件比 day5 参考实现多出来的一步 ——
//     没有它，keep-alive 就没了（一条连接只能服务一个请求）。
//
// 编译运行（-pthread 必须加）：
//     g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/thread.cpp -o build/thread -pthread
//     build/thread                       # 4 个工人 + 每请求日志
//     build/thread --workers 1           # 只开 1 个工人（用来证明"多核真的有用"）
//     build/thread --quiet               # 关掉每请求日志（压测时用）
// ===========================================================================

#include "http.hpp"          // ClientContext / parseRequest / buildResponse / logLine
#include "thread_pool.hpp"   // ThreadPool

#include <arpa/inet.h>     // inet_ntop
#include <cerrno>          // errno / EAGAIN / EWOULDBLOCK / EINTR / EMFILE
#include <csignal>         // signal / SIGPIPE / SIG_IGN
#include <cstdint>         // uint16_t / uint64_t
#include <cstdio>          // perror
#include <cstdlib>         // std::strtoull
#include <cstring>         // std::strlen
#include <fcntl.h>         // fcntl / F_GETFL / F_SETFL / O_NONBLOCK
#include <iostream>        // std::cout / std::endl
#include <mutex>           // std::mutex / std::lock_guard
#include <netinet/in.h>    // sockaddr_in / INADDR_ANY / htons
#include <string>          // std::string / std::to_string
#include <sys/epoll.h>     // epoll_create1 / epoll_ctl / epoll_wait
#include <sys/socket.h>    // socket / bind / listen / setsockopt / accept
#include <unistd.h>        // close / read / send
#include <unordered_map>   // std::unordered_map
#include <utility>         // std::move

constexpr uint16_t PORT = 8888;        // 监听端口
constexpr size_t DEFAULT_WORKERS = 4;  // 默认工人数
constexpr size_t MAX_EVENTS = 16;

// /work 的 n 上限 —— 业务参数是客户端给的，不设上限就是让人一个请求打满 CPU（DoS）
constexpr unsigned long long MAX_WORK_UNITS = 20'000'000ULL;
constexpr unsigned long long DEFAULT_WORK_UNITS = 200'000ULL;

// ===========================================================================
// 跨线程共享的东西
//
// ⚠ 这个文件里【唯一】需要保护的是 g_clients 这张表：
//     · 主线程会 emplace（新连接）/ erase（连接结束）
//     · 工人在还得回去的时候要改 ctx.busy
//   两张线程都会碰 → 必须有一把锁。
//
//   为什么不在读/写 inbuf 时也加锁：因为 fd 交出去之前已经 EPOLL_CTL_DEL 了，
//   主线程不会再收到这条连接的事件，也就不会再碰它的 inbuf —— 分工是清楚的。
// ===========================================================================
static ThreadPool* g_pool = nullptr;
static int g_epoll_fd = -1;
static std::mutex g_clients_mutex;
static std::unordered_map<int, ClientContext> g_clients;

// ===========================================================================
// 业务：真的吃 CPU
//
// xorshift64 —— 每一步都用上一步的结果，所以编译器【没法】把它优化掉。
// 如果写成"空循环 n 次"，-O2 会直接把整个循环删掉，那测出来的就是假的。
// ===========================================================================
static uint64_t burnCpu(unsigned long long n) {
    uint64_t x = 0x9e3779b97f4a7c15ULL;   // 随便一个非零种子
    for (unsigned long long i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x += i;
    }
    return x;
}

// 从 "GET /work?n=100 HTTP/1.1\r\n..." 里取出请求目标（第二段）
static std::string requestTarget(const std::string& raw) {
    const size_t sp1 = raw.find(' ');
    if (sp1 == std::string::npos) return "/";
    const size_t sp2 = raw.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return "/";
    return raw.substr(sp1 + 1, sp2 - sp1 - 1);
}

// 从 "/work?n=100" 里取出 n；没有就用默认值，超上限就截到上限
static unsigned long long workUnits(const std::string& target) {
    const size_t q = target.find("n=");
    if (q == std::string::npos) return DEFAULT_WORK_UNITS;

    const char* p = target.c_str() + q + 2;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(p, &end, 10);
    if (end == p) return DEFAULT_WORK_UNITS;   // "n=" 后面没有数字
    return v > MAX_WORK_UNITS ? MAX_WORK_UNITS : v;
}

// 根据请求算出响应体 —— 这就是"业务处理"，工人线程执行的就是它
static std::string doBusiness(const std::string& raw) {
    const std::string target = requestTarget(raw);

    if (target.rfind("/work", 0) != 0) {
        // 普通请求：和 src/epoll.cpp 返回完全一样的东西
        // （这样 p2_test.py 那 23 项协议测试对两个服务器都成立）
        return "<h1>Hello from my own server!</h1>";
    }

    const unsigned long long n = workUnits(target);
    const uint64_t sum = burnCpu(n);
    return "<h1>CPU work done</h1><p>units=" + std::to_string(n) +
           "</p><p>checksum=" + std::to_string(sum) + "</p>";
}

// ===========================================================================
// 账本操作（都要拿锁）
// ===========================================================================
static bool ctxBusy(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    const auto it = g_clients.find(fd);
    return it != g_clients.end() && it->second.busy;
}

// 返回的指针只在"下一次往表里插入/删除之前"有效。
// 主线程处理单个事件期间不会插入/删除别的连接，所以够用。
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

// ===========================================================================
// ⚠⚠ 本文件最要命的一条规矩：收连接时必须【先 eraseCtx，再 close】
//
// 反过来写会怎样（实测踩到过，而且是哨兵先喊出来的）：
//
//     工人： close(fd=5)          ← 号码 5 立刻可以被内核复用
//     主线程：accept(...) 拿到 fd=5 → g_clients.emplace(5) 失败
//             → 哨兵喊"账本里已经有 fd=5 —— 上一个连接漏了 erase"  ← 冤枉！
//     工人： eraseCtx(5)          ← 把【新连接】的账销掉了
//
//   结果是：新连接不在账本里 → 主线程 findCtx 返回 nullptr → continue
//   → 那条连接【再也没人读、没人关】，fd 一直泄漏。
//
//   顺序调过来就没事：eraseCtx 先做，等 close 真正释放号码时，
//   账本里已经没有它了 —— 所以 close 之后复用这个号码的新连接一定能 emplace 成功。
//
//   一句话：**fd 是会被复用的号码，所以"销账"必须发生在"归还号码"之前。**
// ===========================================================================

// 工人处理【一个】已经解析出来的请求。返回 false = 这条连接已经收掉了。
static bool workerHandleOne(int fd, const std::string& raw, bool want_close) {
    const std::string body = doBusiness(raw);
    const std::string response = buildResponse(body, want_close);

    // ⚠ 非阻塞 fd 上的 send：小响应不会 EAGAIN；部分写（0 < sent < size）
    //   要等响应变大才撞得到，那时候要加输出缓冲 + EPOLLOUT（暂未做）
    const ssize_t sent = send(fd, response.c_str(), response.size(), 0);
    if (sent < 0) {
        perror("send");
    }

    if (want_close) {
        // 客户端要求关（或这条连接被判非法）→ 两本账一起销
        eraseCtx(fd);   // ← 先销账
        close(fd);      // ← 再归还号码（顺序见上面的长注释）
        return false;
    }
    return true;
}

// ===========================================================================
// 【工人线程】算业务 → 发响应 → 把 fd 还回去（或收掉）
//
// 注意参数全是【按值】捕获的：fd / 请求文本 / 要不要关。
// 不能传 ClientContext 的引用 —— 那条连接的上下文归主线程管，
// 工人只在需要碰 inbuf / busy 时短暂地进去改一下（而且要拿锁）。
//
// ★ 中间那个 while 是踩出来的，不是想出来的：
//   客户端可能一次发了两个请求（粘包）。主线程只解析出第一个就交出来了，
//   第二个还在 ctx.inbuf 里 —— 而那是【用户态】缓冲区，不在内核里。
//   所以就算把 fd 还回去，epoll 也不会报 EPOLLIN（内核没有新数据），
//   第二个请求就永远没人管了（判据：keepalive_test 的 [2]，第一版就是挂在这）。
//   → 工人必须【自己】把 inbuf 里排队的请求处理干净，再还 fd。
// ===========================================================================
static void handleInPool(int fd, std::string raw, bool want_close) {
    if (!workerHandleOne(fd, raw, want_close)) {
        return;   // 已经关了
    }

    // ---- 把 inbuf 里已经到齐的请求接着处理完 ----
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
            eraseCtx(fd);   // 先销账（同上）
            close(fd);
            return;
        }
        if (!workerHandleOne(fd, next_raw, next_close)) return;
    }

    // ---- 把 fd 还回去，让主线程继续照看这条连接（keep-alive 靠这一步）----
    // 顺序讲究：先清 busy，再 EPOLL_CTL_ADD。
    //   反过来的话，主线程可能在 busy 还是 true 的时候收到事件 → 被 continue 掉 → 丢事件。
    setBusy(fd, false);

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl: ADD back");
    }
}

// ===========================================================================
// 【主线程】把一个完整请求交给线程池
// ===========================================================================
static void handOffToPool(ClientContext& ctx, size_t total) {
    const std::string raw = ctx.inbuf.substr(0, total);
    const bool want_close = ctx.want_close;

    if (g_log_requests) {
        logLine("收到完整请求（交给工人）:\n" + raw);
    }

    // ① 标记 + 消费字节（inbuf 归主线程管，趁现在处理干净）
    ctx.busy = true;
    ctx.inbuf.erase(0, total);

    // ② 从 epoll 摘掉 —— 不摘的话 LT 会继续通知主线程，两边同时操作同一个 fd
    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, ctx.fd, nullptr);

    // ③ 丢进队列（按值捕获，别传引用）
    const int fd = ctx.fd;
    g_pool->submit([fd, raw, want_close] { handleInPool(fd, raw, want_close); });
}

// ===========================================================================
int main(int argc, char** argv) {
    size_t workers = DEFAULT_WORKERS;
    uint16_t port = PORT;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--workers" && i + 1 < argc) {
            workers = std::stoul(argv[++i]);
        } else if (a == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoul(argv[++i]));
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
    if (listen(server_fd, SOMAXCONN) < 0) {   // backlog 拉满（和 src/epoll.cpp 同一个教训）
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

    ThreadPool pool(workers);
    g_pool = &pool;

    logLine("thread ready: workers=" + std::to_string(workers) + ", port=" +
            std::to_string(port) + "（单 Reactor + 线程池）");

    struct epoll_event events[MAX_EVENTS];
    bool accept_paused = false;
    size_t paused_at = 0;

    while (true) {
        // EMFILE 撞墙后的恢复判断（和 src/epoll.cpp 同一套）
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
                // ---------- 总机：新连接 ----------
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
                    std::lock_guard<std::mutex> lock(g_clients_mutex);
                    if (!g_clients.emplace(conn_fd, ctx).second) {
                        logLine("账本里已经有 fd=" + std::to_string(conn_fd) +
                                " —— 上一个连接漏了 erase");
                    }
                }
            } else {
                // ---------- 分机：有数据 / 对端关了 ----------
                const int fd = events[i].data.fd;

                // 这条连接正在工人的手里 → 主线程不许碰（也别 read）
                if (ctxBusy(fd)) continue;

                ClientContext* ctx = findCtx(fd);
                if (ctx == nullptr) continue;   // 账本里没这条，不该发生

                char buf[1024];
                bool gone = false;
                while (true) {
                    const ssize_t n = read(fd, buf, sizeof(buf));

                    if (n > 0) {
                        ctx->inbuf.append(buf, static_cast<size_t>(n));

                        // 一个连接可能一次到好几个请求 → 循环解析
                        // 但每交出去一个就得停：这条连接已经归工人了，别再动它
                        while (true) {
                            const ParsedRequest pr = parseRequest(*ctx);
                            if (pr.result == ParseResult::NeedMore) break;
                            if (pr.result == ParseResult::Rejected) {
                                logLine("请求非法 —— 丢弃 fd=" + std::to_string(fd));
                                eraseCtx(fd);   // 先销账（同上）
                                close(fd);
                                gone = true;
                                break;
                            }
                            handOffToPool(*ctx, pr.total);
                            gone = true;   // 已经交出去了，这条连接本轮不再处理
                            break;
                        }
                        if (gone) break;
                    } else if (n == 0) {
                        eraseCtx(fd);   // 先销账（同上）
                        close(fd);
                        gone = true;
                        break;
                    } else {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("read");
                        eraseCtx(fd);   // 先销账（同上）
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
