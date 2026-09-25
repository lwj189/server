#include <iostream>       // std::cout / std::endl
#include <string>         // std::string
#include <cstdint>        // uint16_t
#include <cstddef>        // size_t
#include <sys/socket.h>   // socket / bind / listen / setsockopt / sockaddr
#include <netinet/in.h>   // sockaddr_in / INADDR_ANY / htons
#include <cstdio>         // perror
#include <unistd.h>       // close / pause
#include <arpa/inet.h>    // inet_ntop
#include <csignal>

constexpr uint16_t PORT = 8888;   // 监听端口

// ★【DoS 保护】请求头大小上限
//   客户端可能一直发数据、永远不发 "\r\n\r\n"，让 read_buffer 无限增长把内存撑爆。
//   8 KB 是业界常见的量级（nginx 的 header buffer 默认也是 8k；本项目 day2.cpp 用的同样是 8 KB）。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

int main() {

    signal(SIGPIPE, SIG_IGN);

    // ---- 步骤 1：创建 socket ----
    // AF_INET = IPv4；SOCK_STREAM = 可靠字节流(TCP)；0 = 默认协议
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    // ----SO_REUSEADDR（必须在 bind 之前）----
    // 允许 bind 覆盖处于 TIME_WAIT 的端口，否则重启会 EADDRINUSE
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt"); return 1;
    }

    // ---- 填写监听地址 ----
    // 三个字段名见 man 3type sockaddr_in
    struct sockaddr_in address{};
    address.sin_family      = AF_INET;         // IPv4 地址族
    address.sin_addr.s_addr = INADDR_ANY;      // 0.0.0.0：监听本机所有网卡
    address.sin_port        = htons(PORT);     // 端口必须转成网络字节序

    // ---- 步骤 4：bind ----
    // 把地址"挂"到 socket 上；第二参数要强转成通用地址 struct sockaddr*
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind"); return 1;
    }

    // ---- 步骤 5：listen ----
    // 主动套接字 → 被动套接字；backlog = 已完成握手、等 accept 取走的队列上限
    if (listen(server_fd, 3) < 0) { perror("listen"); return 1; }

    std::cout << "listening on port " << PORT << " ..." << std::endl;

    // ----接受连接 → 读取请求 ----
    while (true) {
        // 用新变量装客户端地址（accept 会覆盖它，别复用 bind 用的 address）
        struct sockaddr_in client_addr{};

        // addrlen 是"值-结果"参数：内核会改写它，所以每轮都要重置
        socklen_t client_len = sizeof(client_addr);

        // accept 返回一个新的 fd（这条连接）；失败多为可恢复错误，continue 继续等
        int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept");
            continue;
        }

        // 把二进制的 IPv4 地址转成 "127.0.0.1" 这样的字符串
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        // sin_port 是网络字节序，打印前要用 ntohs 转回本机字节序
        std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        // read 的三态：>0 数据 / 0 对端正常关闭 / -1 出错
        // 注意：一次 read 不保证是一个完整请求（半包）

                // ---- 循环读，直到请求头收全 ----
                std::string read_buffer; 
                bool header_done = false;

                while (!header_done) {
                    char buffer[1024];
                    ssize_t n = read(conn_fd, buffer, sizeof(buffer));

                    if (n == 0) {
                        std::cout << "对端关闭，请求不完整\n";
                        break;
                    }
                    if (n < 0) {
                        perror("read");
                        break;
                    }

                    read_buffer.append(buffer, n);                       
                    if (read_buffer.find("\r\n\r\n") != std::string::npos) 
                        header_done = true;

                    // 超限保护】位置很关键：必须放在"找判据"之后
                    //   先看收全没有：收全了就是正常请求，size 大一点是后续 body/粘包的事，不算头部超限
                    //   没收全再看超限：超了说明对方在灌垃圾，直接断开
                    if (!header_done && read_buffer.size() > MAX_HEADER_SIZE) {
                        std::cout << "请求头过大（超过 " << MAX_HEADER_SIZE
                                  << " 字节）—— 主动断开" << std::endl;
                        break;   // 这里选择"直接断开"；想更规范可改为先 send 一个 431 响应再断开
                    }
                }
                if (header_done) {
                    size_t header_end = read_buffer.find("\r\n\r\n");

                    // ----  解析 Content-Length ----
                    size_t content_len = 0;
                    size_t cl_pos = read_buffer.find("Content-Length: ");
                    if (cl_pos != std::string::npos && cl_pos < header_end) {
                        size_t value_start = cl_pos + std::string("Content-Length: ").size();
                        try {
                            content_len = std::stoul(read_buffer.substr(value_start, header_end - value_start));
                        } catch (const std::exception&) {
                            std::cout << "Content-Length 非法 —— 丢弃这条连接\n";
                            close(conn_fd); // ← 必须先关再 continue，否则 fd 泄漏！
                            continue;
                        }
                    }

                    // ----总长度 ----
                    size_t total = header_end + 4 + content_len;

                    // ----  body 上限（DoS 保护）----
                    if (content_len > MAX_BODY_SIZE) {
                        std::cout << "body 过大 —— 丢弃这条连接\n";
                        close(conn_fd); // ← 同上
                        continue;
                    }

                    // ----  body 没齐就继续读 ----
                    while (read_buffer.size() < total) {
                        char chunk[1024];
                        ssize_t n = read(conn_fd, chunk, sizeof(chunk));
                        if (n <= 0) {
                            std::cout << "body 没收完对端就断了\n";
                            break;
                        }
                        read_buffer.append(chunk, n);
                    }

                    // ---- 齐了才处理 ----
                    if (read_buffer.size() >= total) {
                        std::string request = read_buffer.substr(0, total);
                        std::cout << "收到完整请求:\n"
                                  << request << std::endl;

                        // ↓↓↓ CP4 那段别删！从 git diff 里找回来 ↓↓↓
                        std::string body = "<h1>Hello from my own server!</h1>";
                        std::string response =
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/html\r\n"
                            "Content-Length: " +
                            std::to_string(body.size()) + "\r\n"
                                                          "Connection: close\r\n"
                                                          "\r\n" +
                            body;

                        ssize_t sent = send(conn_fd, response.c_str(), response.size(), 0);
                        if (sent < 0) { perror("send"); }
                    }
                } // ← 别忘了闭合 if (header_done)！

                close(conn_fd);
    }

    close(server_fd);
    return 0;
}
