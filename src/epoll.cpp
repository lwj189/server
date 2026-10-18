// ===========================================================================
// src/epoll.cpp：单线程 Reactor 版 HTTP 服务器
//
// 并发模型：一个线程 + 一个 epoll 实例照看所有连接。
// HTTP 的解析 / 拼响应 / 每条连接的状态都在 src/http.hpp 里 ——
// 那部分和并发模型无关，所以和 src/thread.cpp（线程池版）共用一份。
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

// ClientContext（每条连接的状态：fd / inbuf / want_close）定义在 src/http.hpp，
// 三个纯函数（toLowerAscii / findHeader / isAllDigits）也在那里 —— 线程池版共用。

// ===========================================================================
// 处理一个完整请求：解析（借 src/http.hpp）→ 发响应 → 消费掉 inbuf 里的字节
//
// 返回值：true  = 已经有结果了（响应发了 / 或这条连接该被丢弃）→ 调用方收连接
//         false = 还不够一个完整请求 → 等下次 EPOLLIN 再进来接着算
//
// ⚠ 这个返回值【不是】"成功 / 失败"：请求非法（Rejected）同样返回 true，
//   因为那条连接也"有结果了" —— 该被收掉。
// ===========================================================================
static bool tryHandleRequest(ClientContext& ctx) {
    const ParsedRequest pr = parseRequest(ctx);

    if (pr.result == ParseResult::NeedMore) {
        return false;   // 还不够，等下次事件
    }
    if (pr.result == ParseResult::Rejected) {
        // 请求非法（头过大 / Content-Length 非法…）：ctx.want_close 已被置 true。
        // 【什么都不回】，直接让调用方收掉连接。
        return true;
    }

    // ---- 收齐了：记录 + 响应 ----
    logLine("收到完整请求:\n" + pr.raw);

    const std::string body = "<h1>Hello from my own server!</h1>";
    const std::string response = buildResponse(body, ctx.want_close);

    if (send(ctx.fd, response.c_str(), response.size(), 0) < 0) {
        perror("send");
    }

    // ---- 把已经处理掉的字节从 inbuf 里删掉 ----
    // 不删的话，下次事件进来 find("\r\n\r\n") 又会找到同一个请求 → 又回一遍，
    // 排在后面的请求永远轮不到。删完剩下的才是"还没处理的字节"。
    ctx.inbuf.erase(0, pr.total);

    return true;   // 我处理掉了一个请求（后面还有没有，调用方接着问）
}

int main() {
    // 忽略 SIGPIPE：客户端提前断开时 send 不会杀掉整个进程，而是返回 -1 (EPIPE)
    signal(SIGPIPE, SIG_IGN);

    // ---- 步骤 1：创建 socket ----
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // ---- 步骤 2：SO_REUSEADDR（必须在 bind 之前）----
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        return 1;
    }

    // ---- 步骤 3：填写监听地址 ----
    struct sockaddr_in address{};
    address.sin_family = AF_INET;           // IPv4 地址族
    address.sin_addr.s_addr = INADDR_ANY;   // 0.0.0.0：监听本机所有网卡
    address.sin_port = htons(PORT);         // 端口必须转成网络字节序

    // ---- 步骤 4：bind ----
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    // ---- 步骤 5：listen ----
    // backlog = 已完成三次握手、等着被 accept 取走的连接队列上限。
    // 队列太短（比如照 man 手册的示例写 3）会成为吞吐天花板：
    // 高并发时客户端连 ConnectTimeout，而服务器 CPU 根本没吃满。
    // SOMAXCONN 是内核允许的最大值（超过上限的值会被静默截断成上限）。
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        return 1;
    }

    // ---- 步骤 6：把 server_fd 设为非阻塞（读-改-写）----
    // 之后 accept 在没有新连接时立刻返回 -1 + EAGAIN，而不是挂起线程
    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return 1;
    }
    if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return 1;
    }

    // ---- 步骤 7：创建 epoll 实例（它也返回一个 fd）----
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    // ---- 步骤 8：把 server_fd 登记进 epoll ----
    // ev.data 是"你写给内核的标记"，epoll_wait 事件回来时原样带回，
    // 用来分辨"这个事件属于哪个 fd"（总机还是分机）。
    struct epoll_event ev{};
    ev.events = EPOLLIN;   // 关心"可读"：对监听 socket 就是"有新连接排队"
    ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl: server_fd");
        return 1;
    }   // ← 这个花括号必须在这！少一个就会把下面整段关进 if 的错误分支（死代码）

    std::cout << "epoll ready, listening on port " << PORT << " ..." << std::endl;

    // ---- 连接账本：fd → 连接状态 ----
    // 这是"现在到底有哪些连接活着"的唯一真相来源。
    //
    // ⚠ 两本账要配平：
    //     · fd 号     —— 靠 close() 销账
    //     · 账本条目  —— 靠 erase() 销账
    //   只 close 不 erase = 内存泄漏；只 erase 不 close = fd 泄漏。
    std::unordered_map<int, ClientContext> clients;

    // ---- EMFILE 暂停状态 ----
    // accept 撞到 EMFILE（本进程 fd 用光）后如果只是 break，
    // LT 模式下监听 fd 仍然"可读" → epoll_wait 立刻又返回 → 死循环烧 CPU。
    // 办法：暂停时把 server_fd 从 epoll 里摘掉；等有连接关闭、fd 腾出来，再加回去。
    bool accept_paused = false;
    size_t paused_at = 0;   // 暂停那一刻账本里有几条（后面拿它判断"有没有腾出 fd"）

    // ---- 步骤 9：事件循环（Reactor 的心脏）----
    constexpr int MAX_EVENTS = 16;
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        // ---- 先看看能不能恢复接收 ----
        // 判据：暂停时账本有 paused_at 条，现在少了 → 至少腾出了一个 fd。
        // 这一步必须在 epoll_wait 之前：先问能不能恢复，再决定睡不睡。
        if (accept_paused && clients.size() < paused_at) {
            // 加回去。用的还是当初登记 server_fd 时那个 event（它还在作用域里）
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == 0) {
                accept_paused = false;
                std::cout << "fd 腾出来了 —— 恢复接收新连接" << std::endl;
            }
        }

        // 返回值三态：>0 就绪的 fd 个数 / 0 超时（不是错误）/ -1 出错
        // timeout = -1：没有事件就一直睡，不占 CPU
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;   // 被信号打断：重试，不是错误
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            // 分流：data.fd 是登记时写进去的标记，现在有两种可能 ——
            //   == server_fd → 总机：有新连接在排队
            //   != server_fd → 分机：这条连接上有数据到达，或者对端关闭了
            if (events[i].data.fd == server_fd) {

                // 一次事件可能对应队列里【多个】刚到达的连接，所以循环接到 EAGAIN
                // （ET 模式下不循环会丢连接；LT 模式下是效率问题）
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 接完了
                        if (errno == EMFILE) {   // 本【进程】的 fd 用光了（ENFILE 才是整机）
                            std::cerr << "fd 用光了 —— 暂停接收，等连接释放" << std::endl;
                            // 把它从 epoll 摘掉，别再收它的事件 —— 摘了才不会空转。
                            // （EPOLL_CTL_DEL 的 event 参数会被内核忽略，传什么都不会读）
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, &ev);
                            accept_paused = true;
                            paused_at = clients.size();   // 记下暂停时的账本大小（恢复判据比它小）
                            break;
                        }
                        perror("accept");
                        break;
                    }

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip << ":"
                              << ntohs(client_addr.sin_port) << std::endl;

                    // ---- conn_fd 也要设成非阻塞（同样是读-改-写）----
                    // 事件循环是一个线程轮流照顾所有连接；如果这条连接的 read 阻塞住，
                    // 整个线程就停在这一条连接上，其它连接和监听 fd 的事件全都处理不了。
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

                    // ---- 把 conn_fd 登记进 epoll ----
                    // 登记之后，这条连接上的数据到达时，epoll_wait 才会把它报给我们。
                    // data.fd 写 conn_fd：事件回来时，就用这个标记分辨"是哪条连接"。
                    struct epoll_event cev{};
                    cev.events = EPOLLIN;    // 关心"可读"：对连接就是"对端发数据来了"
                    cev.data.fd = conn_fd;   // 标记写 conn_fd：事件回来时靠它认出是哪条连接

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &cev) < 0) {
                        perror("epoll_ctl: conn_fd");
                        close(conn_fd);   // 没登记成功就没人管它了，必须自己关掉
                        continue;
                    }

                    // ---- 把这条连接记进账本 ----
                    // 放在 epoll_ctl 成功【之后】：前面任何一步失败都已经 close + continue 了，
                    // 走到这儿才动手，就不用再回头擦屁股。
                    ClientContext ctx;
                    ctx.fd = conn_fd;
                    // emplace 返回 pair<iterator, bool>，那个 bool = "到底插进去了没有"。
                    // key 已存在时它【静默不插】—— 而 fd 号会被复用，
                    // 所以 false 就意味着：上一个用这个号码的连接没销账。
                    //
                    // ⚠ 为什么哨兵必须放这儿、不能只放在 erase 那儿：
                    //   漏了 erase 之后，这个 fd 的旧条目【还躺在账本里】，
                    //   下一个连接复用同一个号码时，erase 照样能删掉它（返回 1）——
                    //   所以 erase 那边永远等不到"删不到"的情况，喊不出来。
                    //   只有 emplace 这一侧才知道"这个号码不该有人占着"。
                    if (!clients.emplace(conn_fd, ctx).second) {
                        std::cerr << "账本里已经有 fd=" << conn_fd << " —— 上一个连接漏了 erase"
                                  << std::endl;
                    }
                }
            } else {
                // ---- 分机的事件（data.fd != server_fd）----
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
                // 为什么要循环：一次事件只保证"现在有数据可读"，
                // 不保证"读一次就读完了"。非阻塞 fd 的标准用法就是"一直操作，直到 EAGAIN"。
                while (true) {
                    ssize_t n = read(fd, buf, sizeof(buf));

                    if (n > 0) {
                        // ---- 攒进这条连接【自己】的缓冲区 ----
                        // 是 append 不是 = ：一段一段往上接，这就是半包/粘包的落脚点。
                        ctx.inbuf.append(buf, static_cast<size_t>(n));

                        // ---- 一个连接可能有好几个请求：循环解析 ----
                        // tryHandleRequest 的意思：
                        //     true  = 我处理掉了一个完整请求（已经 erase 掉了）
                        //     false = 还不够一个完整请求
                        // 所以要【循环】问它，直到问不出请求为止 ——
                        // 一次 read 可能把两个请求一起收进来（粘包），
                        // 不循环的话第二个就永远躺在 inbuf 里没人管。
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
                        // 只 close 就够了，不必显式 EPOLL_CTL_DEL：
                        // man 7 epoll 说 fd 关闭时会自动从所有 interest list 摘除；
                        // 显式 DEL 反而有风险 —— fd 号可能已被新连接复用，会误删别人的登记。
                        close(fd);
                        // fd 销了，账本也要销 —— 两本账必须一起平。
                        //
                        // 为什么要写成 if：erase 返回"删掉了几个"（只能是 0 或 1）。
                        // 返回 0 = 账本里【本来就没有】这条 —— 那必然是别的地方漏销账了。
                        // 这是【故意加的哨兵】：漏 erase 时 fd 数是平的、测试也看不出来，
                        // 只有这里会喊出来。
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
                        // 这条路径同样两本都要销（哨兵作用和上面那个一样，见上）
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
