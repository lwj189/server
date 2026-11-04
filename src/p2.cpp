// p2.cpp -- 阻塞式 HTTP 服务器：一次只服务一条连接。
//   g++ -std=c++17 p2.cpp -o p2
// socket → bind → listen → accept → read → 收齐头部 → 回 Hello → close。
// 不看路径也不看方法，并发问题一个都没解决，但 TCP 字节怎么变成一次 HTTP 响应是完整的。

#include <iostream>
#include <string>
#include <cstdint>
#include <cstddef>
#include <cctype>
#include <exception>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdio>
#include <unistd.h>
#include <arpa/inet.h>

constexpr uint16_t PORT = 8888;

// 没有上限的话，一直发数据、永远不发 \r\n\r\n 的客户端能把内存撑爆
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// 下面三个是纯函数：不读全局状态、不做 IO，可以脱离服务器单独测。

// 只转 ASCII 小写。内层 static_cast 必须有：char 在有符号平台上可能是负数，
// 传给 tolower 是未定义行为。
static std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 在头部区域 [0, header_end) 内查字段 name，找到就返回值（已去掉两端空白）。
// 必须逐行从行首比到冒号，不能直接 buf.find("content-length")：
// X-Content-Length-Foo 这种字段会被误匹配。
// 另外：字段名大小写不敏感；冒号后的 OWS 可以是 0 个；只在头部区域内找，
// 否则 body 里的同名文本能骗到解析器。
static bool findHeader(const std::string& buf, size_t header_end,
                       const std::string& name, std::string& value) {
    const std::string want = toLowerAscii(name);
    size_t pos = 0;

    while (pos < header_end) {
        size_t line_end = buf.find("\r\n", pos);
        if (line_end == std::string::npos || line_end > header_end) {
            line_end = header_end;   // 防御：绝不读越过头部区域
        }

        const std::string line = buf.substr(pos, line_end - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && toLowerAscii(line.substr(0, colon)) == want) {
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
                ++v;

            value = line.substr(v);   // 只取本行的值
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            return true;
        }

        if (line_end >= header_end) break;
        pos = line_end + 2;   // 跳过 CRLF
    }
    return false;
}

// 整串都是十进制数字？（空串不算）
// 不能省这层检查：stoull 遇到非数字就停，stoull("10abc") 返回 10，
// 非法值会被当成合法值收下。
static bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

int main() {
    // 客户端提前断开时 send 会拿到 EPIPE，不忽略的话 SIGPIPE 直接杀掉进程
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // 必须在 bind 之前：让 bind 能覆盖 TIME_WAIT 状态的端口，否则重启就 EADDRINUSE
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        return 1;
    }

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;   // 0.0.0.0
    address.sin_port = htons(PORT);         // 端口要转网络字节序

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    // backlog = 3 是故意写小的，用来复现 backlog 溢出；生产用 SOMAXCONN
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "listening on port " << PORT << " ..." << std::endl;

    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);   // 值-结果参数，每轮都要重置

        int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        // 缓冲区必须在连接这一层：放到循环外会被多条连接共用，残留数据污染下一个请求
        std::string read_buffer;
        bool header_done = false;

        // 判据不是"读了一次"，而是缓冲区里出现了 \r\n\r\n。一次 read 可能只有半个请求。
        while (!header_done) {
            char buffer[1024];
            ssize_t n = read(conn_fd, buffer, sizeof(buffer));

            if (n == 0) {
                std::cout << "对端关闭，请求不完整" << std::endl;
                break;
            }
            if (n < 0) {
                perror("read");
                break;
            }

            read_buffer.append(buffer, static_cast<size_t>(n));

            if (read_buffer.find("\r\n\r\n") != std::string::npos) {
                header_done = true;
            }

            // 顺序很关键：先判收全没有，再判超没超限。反过来会把
            // "头很小但 body 很大"的合法请求误杀。
            if (!header_done && read_buffer.size() > MAX_HEADER_SIZE) {
                std::cout << "请求头过大（超过 " << MAX_HEADER_SIZE
                          << " 字节）—— 主动断开" << std::endl;
                break;
            }
        }

        if (header_done) {
            const size_t header_end = read_buffer.find("\r\n\r\n");

            size_t content_len = 0;   // 没有这个头字段就按 0 处理（GET 就是这样）
            std::string cl_value;
            if (findHeader(read_buffer, header_end, "Content-Length", cl_value)) {
                if (!isAllDigits(cl_value)) {
                    std::cout << "Content-Length 非法（不是纯数字）—— 丢弃这条连接" << std::endl;
                    close(conn_fd);
                    continue;
                }
                try {
                    const unsigned long long v = std::stoull(cl_value);
                    if (v > MAX_BODY_SIZE) {
                        // 声明一个巨大的 body 却不发，同样能吊住服务器
                        std::cout << "body 过大（Content-Length=" << v << " > "
                                  << MAX_BODY_SIZE << "）—— 丢弃这条连接" << std::endl;
                        close(conn_fd);
                        continue;
                    }
                    content_len = static_cast<size_t>(v);
                } catch (const std::exception&) {
                    // 只可能是数字长得离谱，stoull 溢出
                    std::cout << "Content-Length 溢出 —— 丢弃这条连接" << std::endl;
                    close(conn_fd);
                    continue;
                }
            }

            // 一个完整请求 = 头部（含结尾空行）+ body
            const size_t total = header_end + 4 + content_len;

            while (read_buffer.size() < total) {
                char chunk[1024];
                ssize_t n = read(conn_fd, chunk, sizeof(chunk));
                if (n <= 0) {
                    std::cout << "body 没收完对端就断了" << std::endl;
                    break;
                }
                read_buffer.append(chunk, static_cast<size_t>(n));
            }

            if (read_buffer.size() >= total) {
                std::string request = read_buffer.substr(0, total);
                std::cout << "收到完整请求:\n"
                          << request << std::endl;

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
        }

        // 只关这条连接，server_fd 一直留着
        close(conn_fd);
    }

    close(server_fd);
    return 0;
}
