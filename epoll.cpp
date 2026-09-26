#include <iostream>     // std::cout / std::endl
#include <string>       // std::string / std::stoull
#include <cstdint>      // uint16_t
#include <cstddef>      // size_t
#include <cctype>       // std::tolower
#include <exception>    // std::exception
#include <csignal>      // signal / SIGPIPE / SIG_IGN
#include <sys/socket.h> // socket / bind / listen / setsockopt / sockaddr
#include <netinet/in.h> // sockaddr_in / INADDR_ANY / htons
#include <cstdio>       // perror
#include <unistd.h>     // close / read
#include <arpa/inet.h>  // inet_ntop
#include <fcntl.h>


constexpr uint16_t PORT = 8888; // 监听端口


constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;


static std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

static bool findHeader(const std::string& buf, size_t header_end,
                       const std::string& name, std::string& value) {
    const std::string want = toLowerAscii(name);
    size_t pos = 0;

    while (pos < header_end) {
        size_t line_end = buf.find("\r\n", pos);
        if (line_end == std::string::npos || line_end > header_end) {
            line_end = header_end;
        }

        const std::string line = buf.substr(pos, line_end - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && toLowerAscii(line.substr(0, colon)) == want) {
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
                ++v; // 跳过 OWS
            value = line.substr(v);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back(); // 去掉尾部空白
            }
            return true;
        }

        if (line_end >= header_end) break;
        pos = line_end + 2; // 跳过 CRLF，看下一行
    }
    return false;
}


static bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

int main() {
    signal(SIGPIPE, SIG_IGN);

   
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

    std::cout << "listening on port " << PORT << " ..." << std::endl;
    //非阻塞
    int flag = fcntl(server_fd, F_GETFD, -1);
    fcntl(server_fd,F_SETFL, O_NONBLOCK);


    // ---- 步骤 6：接受连接 → 处理请求 ----
    while (true) {
        struct sockaddr_in client_addr{};

        socklen_t client_len = sizeof(client_addr);

        int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        // sin_port 是网络字节序，打印前要用 ntohs 转回本机字节序
        std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        
        std::string read_buffer;
        bool header_done = false;

        // ---- 步骤 7.1：循环读，直到请求头收全（解决半包）----
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

           
            if (!header_done && read_buffer.size() > MAX_HEADER_SIZE) {
                std::cout << "请求头过大（超过 " << MAX_HEADER_SIZE
                          << " 字节）—— 主动断开" << std::endl;
                
                break;
            }
        }

        if (header_done) {
            const size_t header_end = read_buffer.find("\r\n\r\n");

            size_t content_len = 0;
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
                        std::cout << "body 过大（Content-Length=" << v << " > "
                                  << MAX_BODY_SIZE << "）—— 丢弃这条连接" << std::endl;
                        close(conn_fd);
                        continue;
                    }
                    content_len = static_cast<size_t>(v);
                } catch (const std::exception&) {
                    std::cout << "Content-Length 溢出 —— 丢弃这条连接" << std::endl;
                    close(conn_fd);
                    continue;
                }
            }

            const size_t total = header_end + 4 + content_len;

            // ---- 步骤 7.3：body 没到齐就继续读 ----
            while (read_buffer.size() < total) {
                char chunk[1024];
                ssize_t n = read(conn_fd, chunk, sizeof(chunk));
                if (n <= 0) {
                    std::cout << "body 没收完对端就断了" << std::endl;
                    break;
                }
                read_buffer.append(chunk, static_cast<size_t>(n));
            }

            // ---- 步骤 7.4：收齐了才处理并响应 ----
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

        // 这条连接处理完了，关掉分机；server_fd（总机）要一直留着
        close(conn_fd);
    }

    close(server_fd);
    return 0;
}
