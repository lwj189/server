// ===========================================================================
// epoll.cpp：单线程 Reactor 版 HTTP 服务器（基线实现）。
// 解析 / 拼响应 / 连接状态都在 http.hpp —— 与并发模型无关，和 thread.cpp（线程池版）共用；
// 本文件只负责【并发模型】：一个线程 + 一个 epoll 实例照看所有连接。
// ===========================================================================
#include "http.hpp"        // ClientContext / parseRequest / buildResponse / logLine

#include <iostream>        // std::cout / std::endl
#include <cstdint>         // uint16_t
#include <cerrno>          // errno / EAGAIN / EWOULDBLOCK / EINTR
#include <csignal>         // signal / SIGPIPE / SIG_IGN
#include <sys/socket.h>    // socket / bind / listen / setsockopt / accept / sockaddr
#include <netinet/in.h>    // sockaddr_in / INADDR_ANY / htons
#include <cstdio>          // perror
#include <unistd.h>        // close
#include <arpa/inet.h>     // inet_ntop
#include <fcntl.h>         // fcntl / F_GETFL / F_SETFL / O_NONBLOCK
#include <sys/epoll.h>     // epoll_create1 / epoll_ctl / epoll_wait / struct epoll_event
#include <unordered_map>   // std::unordered_map（fd → 连接状态）
#include <string>          // std::string / std::to_string

constexpr uint16_t PORT = 8888;   // 监听端口

// ClientContext（每条连接的状态：fd / inbuf / want_close）已搬到 http.hpp ——
// 线程池版也要用，不能只留在这一个文件里。

// 三个纯函数（toLowerAscii / findHeader / isAllDigits）也一块搬过去了。

// ===========================================================================
// tryHandleRequest：解析 → 发响应 → 消费掉 inbuf 里已处理的字节。
// 返回 true = 已经有结果了（响应发了 / 或这条连接该被丢弃）→ 调用方收连接；
//       false = 还不够一个完整请求 → 等下次 EPOLLIN 再进来接着算。
// ⚠ true 【不是】成功/失败：请求非法（Rejected）同样返回 true，因为那条连接也该被收掉。
// ===========================================================================
static bool tryHandleRequest(ClientContext& ctx) {
    const ParsedRequest pr = parseRequest(ctx);

    if (pr.result == ParseResult::NeedMore) {
        return false;   // 还不够，等下次事件
    }
    if (pr.result == ParseResult::Rejected) {
        // 请求非法（头过大 / Content-Length 非法…）：ctx.want_close 已被置 true，
        // 【什么都不回】，直接让调用方收掉 —— 和 p2.cpp 的行为一致。
        return true;
    }

    // ---- 收齐了：记录 + 响应 ----
    logLine("收到完整请求:\n" + pr.raw);

    const std::string body = "<h1>Hello from my own server!</h1>";
    const std::string response = buildResponse(body, ctx.want_close);

    if (send(ctx.fd, response.c_str(), response.size(), 0) < 0) {
        perror("send");
    }

    // ---- 把【已经处理掉】的字节从 inbuf 里删掉（消费）----
    // 不删的话下次 find(CRLFCRLF) 又找到同一个请求 → 又回一遍，后面的请求永远轮不到
    // （判据：keepalive_test 的 [2]）。
    ctx.inbuf.erase(0, pr.total);

    return true;   // 处理掉了一个；还有没有下一个，由调用方再问
}

int main() {
    // 忽略 SIGPIPE：客户端提前断开时 send 返回 -1 (EPIPE)，而不是杀掉整个进程。
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // SO_REUSEADDR 必须在 bind 之前：否则重启时端口还在 TIME_WAIT 会 EADDRINUSE。
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        return 1;
    }

    struct sockaddr_in address{};
    address.sin_family = AF_INET;           // IPv4 地址族
    address.sin_addr.s_addr = INADDR_ANY;   // 0.0.0.0：监听本机所有网卡
    address.sin_port = htons(PORT);         // 端口必须转成网络字节序

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    // ---- listen：backlog = 已完成三次握手、等着被 accept 取走的连接队列上限 ----
    // 原来是 3（照 man 手册的示例抄的），实测这就是吞吐天花板：100 并发压上来时队列只有 3 个位置 →
    //   客户端 ConnectTimeout，短连接吞吐被压到 ~4.5k req/s，而服务器 CPU 根本没吃满。
    // SOMAXCONN 是内核允许的最大值（Linux 上通常 4096）——
    //   man 2 listen 明说：超过上限的值会被【静默截断】成上限。
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        return 1;
    }

    // ---- 把 server_fd 设为非阻塞（读-改-写）----
    // 之后 accept 在没有新连接时立刻返回 -1 + EAGAIN，而不是挂起线程。
    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return 1;
    }
    if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return 1;
    }

    // ---- 创建 epoll 实例（它本身也是一个 fd）----
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    // ---- 把 server_fd 登记进 epoll ----
    // data.fd 是你写给内核的标记，epoll_wait 事件回来时原样带回，用来分辨这个事件属于哪个 fd。
    struct epoll_event ev{};
    ev.events = EPOLLIN;   // 监听 socket 可读 = 有新连接排队
    ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl: server_fd");
        return 1;
    }   // ← 少这个花括号，下面整段会变成 if 的错误分支（死代码）

    std::cout << "epoll ready, listening on port " << PORT << " ..." << std::endl;

    // ---- CP6c：连接账本：fd → 该连接的状态，是现在到底有哪些连接活着的唯一真相来源 ----
    // ⚠ 从这里开始有【两本账】要配平：fd 号靠 close() 销账，账本条目靠 erase() 销账 ——
    //   只 close 不 erase = 内存泄漏；只 erase 不 close = fd 泄漏，两本都得平。
    //   （HANDOFF §8.3 的第二阶段：close / erase 配对）
    std::unordered_map<int, ClientContext> clients;

    // ---- CP6d：EMFILE 暂停状态 ----
    // accept 撞到 EMFILE（fd 用光）后如果只是 break，LT 模式下监听 fd 仍然可读 →
    //   epoll_wait 立刻又返回 → 死循环烧 CPU（实测 3 秒刷了 161 万行 accept: Too many open files，CPU 100%）。
    // 办法：暂停时把 server_fd 从 epoll 里【摘掉】；等有连接关闭、fd 腾出来，再【加回去】。
    bool accept_paused = false;
    size_t paused_at = 0;   // 暂停那一刻账本里有几条（恢复判据：比它小）

    // ---- 事件循环：MAX_EVENTS 是单次 epoll_wait 最多取回多少个事件 ----
    constexpr int MAX_EVENTS = 16;
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        // ---- CP6d：先看看能不能恢复接收 ----
        // 判据：暂停时账本有 paused_at 条，现在少了 → 至少腾出了一个 fd。
        // ⚠ 位置必须在 while 循环【里面】：放循环外则永不执行，摘掉的监听 fd 再也放不回去（实测重试 2/2）；
        //   放这里 vs 放循环体【末尾】实测【等价】，留在这里只因语义更清楚：睡前先问能不能恢复。
        if (accept_paused && clients.size() < paused_at) {
            // 加回去：复用当初登记 server_fd 时那个 event（还在作用域里）
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == 0) {
                accept_paused = false;
                std::cout << "fd 腾出来了 —— 恢复接收新连接" << std::endl;
            }
        }

        // 返回值三态：>0 就绪的 fd 个数 / 0 超时（不是错误）/ -1 出错。
        // timeout = -1：没有事件就一直睡（实测 0% CPU，wchan=ep_poll）。
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;   // 被信号打断：重试，不是错误
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            // 分流：data.fd == server_fd → 监听 socket（有新连接排队）；否则 → 某条连接可读 / 对端关闭了。
            if (events[i].data.fd == server_fd) {

                // 一次事件可能对应队列里【多个】刚到达的连接，所以循环接到 EAGAIN
                // （ET 模式下不循环会丢连接；LT 模式下是效率问题）。
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 接完了
                        if (errno == EMFILE) {   // 本【进程】的 fd 用光了（ENFILE 才是整机）
                            std::cerr << "fd 用光了 —— 暂停接收，等连接释放" << std::endl;
                            // 把它从 epoll 摘掉，别再收它的事件 —— 摘了才不会空转。
                            // event 传 nullptr：man 2 epoll_ctl 说 EPOLL_CTL_DEL 时这个参数【会被内核忽略】
                            // （Linux 2.6.9 之后允许传 NULL）；thread.cpp 里同一条调用也写的 nullptr。
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, nullptr);
                            accept_paused = true;
                            paused_at = clients.size();   // 记下账本大小（恢复判据比它小）
                            break;
                        }
                        perror("accept");
                        break;
                    }

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip << ":"
                              << ntohs(client_addr.sin_port) << std::endl;

                    // ---- CP6b ①：conn_fd 也要设成非阻塞 ----
                    // 事件循环是一个线程轮流照顾所有连接：这条分机的 read 一旦阻塞住，整个线程就停在
                    // 这一条连接上，其它分机和监听 fd 的事件全处理不了 —— 那就退化回阻塞版了。
                    int cflags = fcntl(conn_fd, F_GETFL, 0);
                    if (cflags == -1) {
                        perror("fcntl F_GETFL conn");
                        close(conn_fd);   // 失败也要配平，否则就是一条 fd 泄漏
                        continue;
                    }
                    if (fcntl(conn_fd, F_SETFL, cflags | O_NONBLOCK) == -1) {
                        perror("fcntl F_SETFL conn");
                        close(conn_fd);
                        continue;
                    }

                    // ---- CP6b ②：把 conn_fd 登记进 epoll ----
                    // 登记之后，这条分机上的数据到达时，epoll_wait 才会把它报给我们。
                    struct epoll_event cev{};
                    cev.events = EPOLLIN;    // 对分机就是对端发数据来了
                    cev.data.fd = conn_fd;   // 事件回来时靠它认出是哪条分机

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &cev) < 0) {
                        perror("epoll_ctl: conn_fd");
                        close(conn_fd);   // 没登记成功就没人管它了，必须自己关掉
                        continue;
                    }

                    // ---- CP6c：记进账本（放在 epoll_ctl 成功【之后】：前面任一步失败都已 close + continue）----
                    ClientContext ctx;
                    ctx.fd = conn_fd;
                    // ---- CP6c：存进账本（这里才是第一道哨兵）----
                    // emplace 返回 pair<iterator, bool>，那个 bool = 到底插进去了没有；key 已存在时它
                    // 【静默不插】—— 而 fd 号会被复用，所以 false 就意味着上一个同号连接没销账。
                    // ⚠ 为什么哨兵必须放这儿、不能只放 erase 那儿：漏了 erase 之后旧条目【还躺在账本里】，
                    //   下一个连接复用同一号码时，erase 照样删得掉（返回 1），那侧永远等不到删不到，
                    //   喊不出来；只有 emplace 这一侧才知道这个号码不该有人占着。
                    if (!clients.emplace(conn_fd, ctx).second) {
                        std::cerr << "账本里已经有 fd=" << conn_fd << " —— 上一个连接漏了 erase"
                                  << std::endl;
                    }
                }
            } else {
                // ---- CP6b ③ / CP6c：分机的事件（data.fd != server_fd）----
                int fd = events[i].data.fd;

                // ---- 先从账本里把这条连接找出来 ----
                // 账本 = fd → 这条连接的状态。找不到说明不该发生（登记和销账没配平），
                // 这时 continue 兜住即可 —— 绝不能拿着一个空的 ctx 硬来。
                auto it = clients.find(fd);
                if (it == clients.end()) {
                    continue;                 // 账本里没这条 —— 不该发生，跳过别硬来
                }

                // ⚠ 用【引用】而不是拷贝：拷贝一份改的是副本，inbuf 攒的东西全丢了。
                //   （副本会静默地什么都不做 —— 和 for (char c : out) 少个 & 是同一类 bug）
                ClientContext& ctx = it->second;

                char buf[1024];
                // 为什么要循环：一次事件只保证现在有数据可读，不保证读一次就读完了。
                // 非阻塞 fd 的标准用法就是一直操作，直到 EAGAIN 为止（和上面 accept 同形）。
                while (true) {
                    ssize_t n = read(fd, buf, sizeof(buf));

                    if (n > 0) {
                        // ---- CP6c-2：append 不是 = ：一段一段往上接，这就是半包/粘包的落脚点 ----
                        ctx.inbuf.append(buf, static_cast<size_t>(n));

                        // ---- 一个连接可能有好几个请求：循环解析 ----
                        // 一次 read 可能把两个请求一起收进来（粘包）：第二个请求的字节已经在【用户态】，
                        // 不会再触发 EPOLLIN —— 不在这里循环问完，它就永远躺在 inbuf 里没人管。
                        bool gone = false;
                        while (tryHandleRequest(ctx)) {
                            // 每处理完一个就问一句：这条连接该收了吗？
                            if (ctx.want_close) {
                                // 两本账一起销：fd 靠 close，账本条目靠 erase
                                close(fd);
                                if (clients.erase(fd) == 0) {
                                    std::cerr << "账本里没有 fd=" << fd << " —— 有地方漏销账"
                                              << std::endl;
                                }
                                gone = true;
                                break;
                            }
                            // 不用关 → 回到 while 顶部，看 inbuf 里还有没有下一个请求
                        }
                        if (gone) break;   // 连接已经收了 → 跳出 read 循环
                    } else if (n == 0) {
                        // read 返回 0 = 对端【正常】关闭（有序关闭，不是 RST）。
                        // 只 close 就够了，不必显式 EPOLL_CTL_DEL：man 7 epoll 说 fd 关闭时会自动从所有
                        // interest list 摘除；显式 DEL 反而有风险 —— fd 号可能已被新连接复用，会误删别人的登记。
                        close(fd);
                        // fd 销了，账本也要销 —— 两本账必须一起平。
                        // 为什么写成 if：erase 返回删掉了几个（只能是 0 或 1）。返回 0 = 账本里【本来就没有】
                        // 这条 —— 那必然是别的地方漏销账了。这是【故意加的哨兵】：漏 erase 时 fd 数是平的、
                        // cp6b_test.py 4/4 全绿、编译零警告 —— 只有这里会喊出来。
                        if (clients.erase(fd) == 0) {
                            std::cerr << "账本里没有 fd=" << fd << " —— 有地方漏销账" << std::endl;
                        }
                        break;
                    } else {
                        if (errno == EINTR) continue;   // 被信号打断：重试，不是错误
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;   // 数据读干净了 —— 这是【正常】出口，不是错误
                        }
                        perror("read");   // 其它错误（如 ECONNRESET：对端 RST 强断）
                        close(fd);
                        if (clients.erase(fd) == 0) {
                            std::cerr << "账本里没有 fd=" << fd << " —— 有地方漏销账" << std::endl;
                        }
                        break;
                    }
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
