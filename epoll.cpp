// ===========================================================================
// day4 / epoll 学习 —— 纯 IO 事件通知，不含任何 HTTP 解析
//
// 为什么这里没有 HTTP 解析？
//   epoll 回答的是"哪个 fd 现在有新连接 / 有数据"（IO 事件层）；
//   HTTP 解析回答的是"这堆字节里哪一段是一个完整请求"（应用层协议）。
//   两者正交。先把 epoll 本身玩透，解析留到 CP6d 再搬进来
//   （完整且验证过的版本在 p2.cpp 里，13 个回归用例全过）。
//
// 当前进度：CP6a —— 事件循环 + 非阻塞 server_fd + accept 循环到 EAGAIN
// ===========================================================================

#include <iostream>     // std::cout / std::endl
#include <cstdint>      // uint16_t
#include <cerrno>       // errno / EAGAIN / EWOULDBLOCK / EINTR
#include <csignal>      // signal / SIGPIPE / SIG_IGN
#include <sys/socket.h> // socket / bind / listen / setsockopt / accept / sockaddr
#include <netinet/in.h> // sockaddr_in / INADDR_ANY / htons
#include <cstdio>       // perror
#include <unistd.h>     // close
#include <arpa/inet.h>  // inet_ntop
#include <fcntl.h>      // fcntl / F_GETFL / F_SETFL / O_NONBLOCK
#include <sys/epoll.h>  // epoll_create1 / epoll_ctl / epoll_wait / struct epoll_event

constexpr uint16_t PORT = 8888; // 监听端口

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
    address.sin_family = AF_INET;         // IPv4 地址族
    address.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0：监听本机所有网卡
    address.sin_port = htons(PORT);       // 端口必须转成网络字节序

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
    ev.events = EPOLLIN; // 关心"可读"：对监听 socket 就是"有新连接排队"
    ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl: server_fd");
        return 1;
    } // ← 这个花括号必须在这！少一个就会把下面整段关进 if 的错误分支（死代码）

    std::cout << "epoll ready, listening on port " << PORT << " ..." << std::endl;

    // ---- 步骤 9：事件循环（Reactor 的心脏）----
    constexpr int MAX_EVENTS = 16;
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        // 返回值三态：>0 就绪的 fd 个数 / 0 超时（不是错误）/ -1 出错
        // timeout = -1：没有事件就一直睡（实测 0% CPU，wchan=ep_poll）
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue; // 被信号打断：重试，不是错误
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            // data.fd == server_fd 说明这是"监听套接字上的事件"，也就是有新连接
            if (events[i].data.fd == server_fd) {

                // 一次事件可能对应队列里【多个】刚到达的连接，所以循环接到 EAGAIN
                // （ET 模式下不循环会丢连接；LT 模式下是效率问题）
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 接完了
                        perror("accept");
                        break;
                    }

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip << ":"
                              << ntohs(client_addr.sin_port) << std::endl;

                    // CP6a 只验证"能接到"，先立刻关掉
                    // CP6b：改成"设非阻塞 + 登记进 epoll"，交给事件循环处理
                    close(conn_fd);
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
