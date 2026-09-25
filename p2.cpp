#include <iostream>       // std::cout / std::endl
#include <string>         // std::string / std::stoull
#include <cstdint>        // uint16_t
#include <cstddef>        // size_t
#include <cctype>         // std::tolower
#include <exception>      // std::exception
#include <csignal>        // signal / SIGPIPE / SIG_IGN
#include <sys/socket.h>   // socket / bind / listen / setsockopt / sockaddr
#include <netinet/in.h>   // sockaddr_in / INADDR_ANY / htons
#include <cstdio>         // perror
#include <unistd.h>       // close / read
#include <arpa/inet.h>    // inet_ntop

constexpr uint16_t PORT = 8888;   // 监听端口

// ★【DoS 保护】请求头 / 请求体大小上限
//   客户端可能一直发数据、永远不发 "\r\n\r\n"，让 read_buffer 无限增长把内存撑爆。
//   8 KB 这个量级和 nginx 的默认值同源：nginx 的 client_header_buffer_size 默认 1k，
//   装不下时才分配 large_client_header_buffers（默认 4 个 × 8k，单个头字段不能超过一个 buffer）。
//   本项目 serverai/day4.cpp ~ day6.cpp 的参考实现用的是 #define MAX_HEADER_SIZE 8192。
//   MAX_BODY_SIZE 取 1 MB，与 nginx 的 client_max_body_size 默认值一致。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// ---------------------------------------------------------------------------
// HTTP 头字段的解析工具
// ---------------------------------------------------------------------------

// 转成小写。HTTP 头字段名限定为 ASCII token，所以只处理 ASCII 就够。
static std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 在"请求头区域"[0, header_end) 内查找字段 name，找到就返回值（已去掉两端空白）。
//
// 这里有三个必须处理的协议细节，少一个都会被真实客户端或恶意请求绕过：
//   1. 头字段名【大小写不敏感】—— RFC 9110 §5.1 "Field names are case-insensitive"。
//      所以 content-length / Content-Length / CONTENT-LENGTH 是同一个字段。
//      旧实现用 read_buffer.find("Content-Length: ") 直接匹配，遇到小写就会漏掉。
//   2. 冒号后面的空白是【可选】的（OWS = 0 个或多个空格/制表符）—— RFC 9112 §5 的
//      field-line = field-name ":" OWS field-value OWS。
//      所以 "Content-Length:10" 和 "Content-Length:  10" 都是合法的。
//      旧实现要求冒号后恰好一个空格，会漏掉前者。
//   3. 只在【头部区域内】查找：否则 body 里恰好含有 "Content-Length: ..." 文本时，
//      解析器会被 body 内容骗到（请求走私 / 参数污染的入门级防范）。
static bool findHeader(const std::string& buf, size_t header_end,
                       const std::string& name, std::string& value) {
    const std::string want = toLowerAscii(name);
    size_t pos = 0;

    while (pos < header_end) {
        // 取出一行（到 CRLF 为止；最后一行可能正好顶到 header_end）
        size_t line_end = buf.find("\r\n", pos);
        if (line_end == std::string::npos || line_end > header_end) {
            line_end = header_end;
        }

        const std::string line = buf.substr(pos, line_end - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && toLowerAscii(line.substr(0, colon)) == want) {
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;   // 跳过 OWS
            value = line.substr(v);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();                                                 // 去掉尾部空白
            }
            return true;
        }

        if (line_end >= header_end) break;
        pos = line_end + 2;   // 跳过 CRLF，看下一行
    }
    return false;
}

// 整个字符串都是十进制数字？（空串不算数字）
// 用它先把值卡成"纯数字"，后面的 std::stoull 就只会因为数值太大而抛异常。
// 旧实现直接把 substr 丢给 stoul，靠"遇到非数字就停"侥幸工作——
// 那意味着 "Content-Length: 10abc" 也会被当成 10 接受。
static bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
int main() {
    // 忽略 SIGPIPE：客户端提前断开时 send 不会杀掉整个进程，而是返回 -1 (EPIPE)
    signal(SIGPIPE, SIG_IGN);

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

    // ---- 步骤 6：接受连接 → 处理请求 ----
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

        // 每一条连接的数据都攒在【这一层】的缓冲区里。
        // 放在 while(true) 外面会让多个客户端共用同一个缓冲区 —— 上一条的残留数据
        // 会污染下一个请求，那是极难查的 bug。
        std::string read_buffer;
        bool header_done = false;

        // ---- 步骤 7.1：循环读，直到请求头收全（解决半包）----
        // TCP 是字节流：一次 read 可能只拿到半个请求，也可能一次拿到两个请求。
        // 所以判据不是"read 了一次"，而是"缓冲区里出现了 \r\n\r\n"（RFC 9112 §2.1）。
        while (!header_done) {
            char buffer[1024];
            ssize_t n = read(conn_fd, buffer, sizeof(buffer));

            if (n == 0) {
                // read 返回 0 = 对端正常关闭（EOF）。请求没读完就断了，这条连接没戏了。
                std::cout << "对端关闭，请求不完整" << std::endl;
                break;
            }
            if (n < 0) {
                // n < 0 才是错误。EINTR（被信号打断）严格来说应该重试，这里先当错误处理。
                perror("read");
                break;
            }

            read_buffer.append(buffer, static_cast<size_t>(n));

            if (read_buffer.find("\r\n\r\n") != std::string::npos) {
                header_done = true;
            }

            // 【超限保护】判断顺序很关键：必须放在"找判据"之后。
            //   先看收全没有：收全了就是正常请求，size 大一点是后续 body/粘包的事，不算头部超限；
            //   没收全再看超限：超了说明对方在灌垃圾，直接断开。
            // 如果反过来（先判超限），一个"头很小但 body 很大"的合法请求会被误杀。
            if (!header_done && read_buffer.size() > MAX_HEADER_SIZE) {
                std::cout << "请求头过大（超过 " << MAX_HEADER_SIZE
                          << " 字节）—— 主动断开" << std::endl;
                // 这里选择"直接断开"；更规范的做法是先 send 一个 431
                // (Request Header Fields Too Large) 再断开，serverai/day5.cpp 就是这么做的。
                break;
            }
        }

        if (header_done) {
            const size_t header_end = read_buffer.find("\r\n\r\n");

            // ---- 步骤 7.2：解析 Content-Length ----
            // 用 findHeader 而不是 find("Content-Length: ")，一次解决三件事：
            //   大小写不敏感 / 冒号后空白可选 / 只在头部区域内查找（详见 findHeader 注释）
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
                        // 也是 DoS 防护：声明一个巨大的 body 却不发，就能吊住服务器
                        std::cout << "body 过大（Content-Length=" << v << " > "
                                  << MAX_BODY_SIZE << "）—— 丢弃这条连接" << std::endl;
                        close(conn_fd);
                        continue;
                    }
                    content_len = static_cast<size_t>(v);
                } catch (const std::exception&) {
                    // 走到这里只可能是数字长得离谱（stoull 溢出）
                    std::cout << "Content-Length 溢出 —— 丢弃这条连接" << std::endl;
                    close(conn_fd);
                    continue;
                }
            }
            // 没有 Content-Length 就按 0 处理（GET 请求就是这种）

            // 一个完整请求 = 头部（含结尾的空行）+ body
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
                std::cout << "收到完整请求:\n" << request << std::endl;

                std::string body = "<h1>Hello from my own server!</h1>";
                std::string response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
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
