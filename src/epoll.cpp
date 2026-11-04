// epoll.cpp -- 单线程 Reactor 版 HTTP 服务器。
//   g++ -std=c++17 epoll.cpp -o epoll
// 本文件只管并发模型：一个线程 + 一个 epoll 实例照看所有连接。
// HTTP 的解析和拼响应在 http.hpp 里，和 thread.cpp 共用一份。

#include "http.hpp"        // ClientContext / parseRequest / buildResponse / logLine

#include <iostream>
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdio>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unordered_map>
#include <string>

constexpr uint16_t PORT = 8888;

// 处理一个完整请求：解析 → 发响应 → 消费掉 inbuf 里的字节。
//
// 返回值不是"成功 / 失败"：true = 这条连接已经有结果（响应发了，或者该丢弃），
// 调用方去收连接；false = 还不够一个完整请求，等下次 EPOLLIN。
// 请求非法（Rejected）也返回 true —— 那条连接同样该被收掉。
static bool tryHandleRequest(ClientContext& ctx) {
    const ParsedRequest pr = parseRequest(ctx);

    if (pr.result == ParseResult::NeedMore) {
        return false;
    }
    if (pr.result == ParseResult::Rejected) {
        // want_close 已被置 true，什么都不回，直接收连接
        return true;
    }

    logLine("收到完整请求:\n" + pr.raw);

    const std::string body = "<h1>Hello from my own server!</h1>";
    const std::string response = buildResponse(body, ctx.want_close);

    if (send(ctx.fd, response.c_str(), response.size(), 0) < 0) {
        perror("send");
    }

    // 必须消费已处理掉的字节。不删的话下次进来又找到同一个请求、再回一遍，
    // 后面的请求永远轮不到。
    ctx.inbuf.erase(0, pr.total);

    return true;   // 处理掉了一个，后面还有没有由调用方接着问
}

int main() {
    // 客户端提前断开时 send 返回 EPIPE，不忽略 SIGPIPE 进程会直接被杀
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // 必须在 bind 之前，否则重启时会被 TIME_WAIT 残留的端口挡住
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        return 1;
    }

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    // backlog 是已完成握手、等着被 accept 取走的队列长度。
    // 原来照 man 抄成 3，实测这就是吞吐天花板：100 并发只有 3 个位置，
    // 客户端 ConnectTimeout，吞吐被压到 ~4.5k req/s 而 CPU 没吃满。
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        return 1;
    }

    // 监听 fd 设非阻塞，没新连接时 accept 会立刻返回 EAGAIN。
    // F_SETFL 是整体替换不是追加，必须先 F_GETFL 再 or 上 O_NONBLOCK。
    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return 1;
    }
    if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return 1;
    }

    int epoll_fd = epoll_create1(0);   // epoll 实例本身也是一个 fd
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    // data.fd 是写给内核的标记，事件回来时原样带回，用来区分总机和分机
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl: server_fd");
        return 1;
    }

    std::cout << "epoll ready, listening on port " << PORT << " ..." << std::endl;

    // 连接账本：fd → 状态，是"现在有哪些连接活着"的唯一真相来源。
    // 从这里开始有两本账要配平：fd 靠 close() 销，条目靠 erase() 销。
    // 只 close 不 erase 是内存泄漏，只 erase 不 close 是 fd 泄漏。
    std::unordered_map<int, ClientContext> clients;

    // EMFILE 暂停状态。accept 撞到 EMFILE 后如果只是 break，LT 模式下监听 fd
    // 仍然可读，epoll_wait 立刻又返回 → 死循环烧 CPU（实测 3 秒刷了 161 万行
    // "accept: Too many open files"）。办法是暂停时把 server_fd 从 epoll 里摘掉，
    // 等有连接关闭、fd 腾出来再加回去。
    bool accept_paused = false;
    size_t paused_at = 0;   // 暂停那一刻账本里有几条

    constexpr int MAX_EVENTS = 16;
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        // 必须在 epoll_wait 之前问，所以放在循环里。
        // 判据：暂停时账本有 paused_at 条，现在少了 → 至少腾出了一个 fd。
        if (accept_paused && clients.size() < paused_at) {
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == 0) {
                accept_paused = false;
                std::cout << "fd 腾出来了 —— 恢复接收新连接" << std::endl;
            }
        }

        // > 0 就绪个数 / 0 超时 / -1 出错。timeout = -1：没事件就一直睡。
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;   // 被信号打断，重试
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            // 分流：server_fd 是总机（有新连接），其它是分机（有数据或对端关了）
            if (events[i].data.fd == server_fd) {

                // 一次事件可能对应队列里多个刚到的连接，所以循环接到 EAGAIN 为止
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 接完了
                        if (errno == EMFILE) {   // 本进程的 fd 用光（ENFILE 才是整机）
                            std::cerr << "fd 用光了 —— 暂停接收，等连接释放" << std::endl;
                            // 摘掉监听 fd 才不会再收到它的事件。event 传 nullptr，
                            // 内核会忽略；传 &ev 只会让人以为它有用。
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, nullptr);
                            accept_paused = true;
                            paused_at = clients.size();
                            break;
                        }
                        perror("accept");
                        break;
                    }

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip << ":"
                              << ntohs(client_addr.sin_port) << std::endl;

                    // 分机也要非阻塞：这条连接一阻塞，整个线程就停在这儿，
                    // 其它 fd 的事件全处理不了。accept 出来的 socket 不继承监听
                    // fd 的非阻塞属性，必须自己设。
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

                    // 登记了这条分机，它的数据到达时 epoll_wait 才会报给我们
                    struct epoll_event cev{};
                    cev.events = EPOLLIN;
                    cev.data.fd = conn_fd;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &cev) < 0) {
                        perror("epoll_ctl: conn_fd");
                        close(conn_fd);   // 没登记成功就没人管它了，得自己关
                        continue;
                    }

                    // 记账放在 epoll_ctl 成功之后：前面任何一步失败都已经 close 了
                    ClientContext ctx;
                    ctx.fd = conn_fd;
                    // 哨兵。emplace 的返回值 = 到底插进去了没有，key 已存在时它静默不插。
                    // fd 号会被复用，所以 false 意味着上一个用这个号码的连接没销账。
                    // 只放 erase 那边没用：漏 erase 后旧条目还在，下一个连接复用同号时
                    // erase 照样能删掉它，那边永远等不到"删不到"的情况。
                    if (!clients.emplace(conn_fd, ctx).second) {
                        std::cerr << "账本里已经有 fd=" << conn_fd << " —— 上一个连接漏了 erase"
                                  << std::endl;
                    }
                }
            } else {
                int fd = events[i].data.fd;

                // 账本里找不到说明登记和销账没配平，跳过即可，不能拿空 ctx 硬来
                auto it = clients.find(fd);
                if (it == clients.end()) {
                    continue;
                }

                // 必须引用：拷贝一份改的是副本，inbuf 里攒的数据全丢
                ClientContext& ctx = it->second;

                char buf[1024];
                // 一次事件只保证"现在有数据可读"，不保证读一次就读完
                while (true) {
                    ssize_t n = read(fd, buf, sizeof(buf));

                    if (n > 0) {
                        // 是 append 不是 = ，半包和粘包就在这里接上
                        ctx.inbuf.append(buf, static_cast<size_t>(n));

                        // 一次 read 可能收进来两个请求，所以要循环问到底，
                        // 不循环的话第二个就永远躺在 inbuf 里没人管
                        bool gone = false;
                        while (tryHandleRequest(ctx)) {
                            if (ctx.want_close) {
                                // 两本账一起销
                                close(fd);
                                if (clients.erase(fd) == 0) {
                                    std::cerr << "账本里没有 fd=" << fd << " —— 有地方漏销账"
                                              << std::endl;
                                }
                                gone = true;
                                break;
                            }
                        }
                        if (gone) break;
                    } else if (n == 0) {
                        // 对端正常关闭。不用显式 EPOLL_CTL_DEL：close 会自动从所有
                        // interest list 摘除；显式 DEL 反而可能误删复用了同一号码的新连接。
                        close(fd);
                        // 哨兵：erase 返回 0 = 账本里本来就没这条。漏 erase 时 fd 数是
                        // 平的、测试全绿、编译零警告，只有这里会喊。
                        if (clients.erase(fd) == 0) {
                            std::cerr << "账本里没有 fd=" << fd << " —— 有地方漏销账" << std::endl;
                        }
                        break;
                    } else {
                        if (errno == EINTR) continue;   // 被信号打断，重试
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;   // 数据读干净了，这是正常出口
                        }
                        perror("read");   // 其它错误，比如对端 RST（ECONNRESET）
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
