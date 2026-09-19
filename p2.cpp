#include <iostream>       // std::cout / std::endl
#include <string>         // std::string
#include <cstdint>        // uint16_t
#include <sys/socket.h>   // socket / bind / listen / setsockopt / sockaddr
#include <netinet/in.h>   // sockaddr_in / INADDR_ANY / htons
#include <cstdio>         // perror
#include <unistd.h>       // close / pause
#include <arpa/inet.h>    // inet_ntop

constexpr uint16_t PORT = 8888;   // 监听端口

int main() {
    // ---- 步骤 1：创建 socket ----
    // AF_INET = IPv4；SOCK_STREAM = 可靠字节流(TCP)；0 = 默认协议
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    // ---- 步骤 2：SO_REUSEADDR（必须在 bind 之前）----
    // 允许 bind 覆盖处于 TIME_WAIT 的端口，否则重启会 EADDRINUSE
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt"); return 1;
    }

    // ---- 步骤 3：填写监听地址 ----
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

    // ---- CP2 / CP3：接受连接 → 读取请求 ----
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
        // 注意：一次 read 不保证是一个完整请求（半包），day3 用读缓冲区解决
        char buffer[1024] = {0};
        ssize_t n = read(conn_fd, buffer, sizeof(buffer));
        if (n > 0) {
            std::string req(buffer, n);     // 用 n 当长度，不依赖 '\0'
            std::cout << "收到请求:\n"<< req << std::endl;
            
            // ---- CP4：返回 HTTP 响应 ----
            // 消息结构（RFC 9112 §2.1）：
            //   HTTP-message = start-line CRLF *( field-line CRLF ) CRLF [ message-body ]
            //
            // 需要你自己填的两个参数：
            //   ① Content-Length 的值：它要表达"消息体有多少字节"，消息体是哪个变量？
            //   ② send 的第一个参数：要发给哪条连接？（总机还是分机？）
            std::string body = "<h1>Hello from my own server!</h1>";
            std::string response =
                "HTTP/1.1 200 OK\r\n"         // start-line
                "Content-Type: text/html\r\n" // field-line
                "Content-Length: " +
                std::to_string(body.size()) + "\r\n"                  // field-line（①）
                                              "Connection: close\r\n" // field-line
                                              "\r\n" +                // 单独一个 CRLF = 空行
                body;                                                 // message-body

        } else if (n == 0) {
            std::cout << "客户端主动关闭了连接" << std::endl;
        } else {
            perror("read");
        }

        
        // flags 填 0：man 2 send 说 "with a zero flags argument, send() is equivalent to write(2)"
        ssize_t sent = send(conn_fd, response.c_str(), response.size(), 0);   // （②）
        if (sent < 0) {
            perror("send");
        }

        // 想一想：n <= 0（没收到请求）时，还需要发这个响应吗？

        // 这条连接处理完了，关掉分机；server_fd（总机）要一直留着
        close(conn_fd);
    }

    close(server_fd);
    return 0;
}
