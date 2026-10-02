#include <iostream>       // std::cout / std::endl
#include <cstdint>        // uint16_t
#include <cerrno>         // errno / EAGAIN / EWOULDBLOCK / EINTR
#include <csignal>        // signal / SIGPIPE / SIG_IGN
#include <sys/socket.h>   // socket / bind / listen / setsockopt / accept / sockaddr
#include <netinet/in.h>   // sockaddr_in / INADDR_ANY / htons
#include <cstdio>         // perror
#include <unistd.h>       // close
#include <arpa/inet.h>    // inet_ntop
#include <fcntl.h>        // fcntl / F_GETFL / F_SETFL / O_NONBLOCK
#include <sys/epoll.h>    // epoll_create1 / epoll_ctl / epoll_wait / struct epoll_event

constexpr uint16_t PORT = 8888;   // 监听端口

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
    if (listen(server_fd, 3) < 0) {
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

    // ---- 步骤 9：事件循环（Reactor 的心脏）----
    constexpr int MAX_EVENTS = 16;
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        // 返回值三态：>0 就绪的 fd 个数 / 0 超时（不是错误）/ -1 出错
        // timeout = -1：没有事件就一直睡（实测 0% CPU，wchan=ep_poll）
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
                        perror("accept");
                        break;
                    }

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip << ":"
                              << ntohs(client_addr.sin_port) << std::endl;

                    // ---- CP6b ①：conn_fd 也要设成非阻塞 ----
                    // 和第 53~61 行处理 server_fd 时完全同形（都是"读-改-写"），只是换了 fd。
                    // 为什么必须非阻塞：事件循环是"一个线程轮流照顾所有连接"。
                    // 如果这条分机的 read 阻塞住，整个线程就停在这一条连接上，
                    // 其它分机和总机的事件全都处理不了 —— 那就退化回 CP2 的阻塞版了。
                    int cflags = fcntl(conn_fd, F_GETFL, 0);
                    if (cflags == -1) {
                        perror("fcntl F_GETFL conn");
                        close(conn_fd);   // 失败也要配平，否则就是一条 fd 泄漏
                        continue;
                    }
                    if (fcntl(conn_fd, F_SETFL, cflags | O_NONBLOCK) == -1) {   // ← 空 1
                        perror("fcntl F_SETFL conn");
                        close(conn_fd);
                        continue;
                    }

                    // ---- CP6b ②：把 conn_fd 登记进 epoll ----
                    // 登记之后，这条分机上的数据到达时，epoll_wait 才会把它报给我们。
                    // data.fd 写 conn_fd：事件回来时，就用这个标记分辨"是哪条分机"。
                    struct epoll_event cev{};
                    cev.events = EPOLLIN;    // ← 空 2：这条分机，你关心它的什么？
                    cev.data.fd = conn_fd;   // ← 空 3：标记写谁？

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &cev) < 0) {   // ← 空 4
                        perror("epoll_ctl: conn_fd");
                        close(conn_fd);   // 没登记成功就没人管它了，必须自己关掉
                        continue;
                    }
                }
            } else {
                // ---- CP6b ③：分机的事件（data.fd != server_fd）----
                int fd = events[i].data.fd;

                char buf[1024];
                // 为什么要循环：一次事件只保证"现在有数据可读"，
                // 不保证"读一次就读完了"。和 CP6a 里 accept 循环到 EAGAIN 是同一个形状 ——
                // 非阻塞 fd 的标准用法就是"一直操作，直到 EAGAIN 为止"。
                while (true) {
                    ssize_t n = read(fd, buf, sizeof(buf));

                    if (n > 0) {
                        send(fd, buf, n, 0);   // 回显：CP4 的 send 在这里第一次被复用
                    } else if (n == 0) {
                        close(fd);   // ← 空 5：对端【正常关闭】。这里要做两件事，写出来
                        break;
                    } else {
                        if (errno == EINTR) continue;   // 被信号打断：重试，不是错误
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;   // 数据读干净了 —— 这是【正常】出口，不是错误
                        }
                        perror("read");   // 其它错误（如 ECONNRESET：对端 RST 强断）
                        close(fd);        // ← 空 6：出错也要配平
                        break;
                    }
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
