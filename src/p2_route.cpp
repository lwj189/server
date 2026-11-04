// p2_route.cpp -- p2.cpp + 一层薄路由：从"收齐字节流"到"知道你要哪个资源"。
//   g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion p2_route.cpp -o p2_route
//
// 并发模型与 p2.cpp 完全相同（阻塞、一次一条连接），加的只有三样：
//   1. 请求行解析：拆 method / target / version，target 再切成 path + query
//   2. 路由：GET /、/hello、/first、/second、/search?q=、POST /echo，其余 404；
//      方法不认识 -> 405 + Allow；请求行畸形（非 origin-form / 版本不对 / 路径带裸空格）-> 400
//   3. 响应构造：状态码、Content-Type、body 都由路由决定
//
// 不进 CI 测试矩阵：tests/p2_test.py 的 23 项断言是按 p2.cpp 写的（任何路径都回 Hello）。
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

// 上限：客户端可以一直发数据、永远不发空行，没有上限缓冲区会一直涨到 OOM。
// 8 KB / 1 MB 与 nginx 的 large_client_header_buffers、client_max_body_size 同量级。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// 协议层三个纯函数：不依赖全局变量、不做 IO。匿名 namespace 是内部链接，不会和别的 .cpp 撞名。
namespace {
    // 参数必须先转 unsigned char：char 在 x86 Linux 上有符号，0xE4 这类 UTF-8 字节读出来是负数，
    // tolower 收到负数是未定义行为。
    std::string toLowerAscii(const std::string& s) {
        std::string out = s;
        for (char& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    // 在头部区域 [0, header_end) 内按行找字段，命中就把值（去掉两端空白）写进 value。
    // 逐行比到冒号为止，不能全文 find("content-length")：那样 "X-Content-Length-Foo: 1" 会被误匹配。
    // 三件事都得管：① 字段名大小写不敏感 ② 冒号后 OWS 可为 0 个或多个
    //             ③ 只在头部区域内找 —— 否则 body 里恰好出现同名文本会骗到解析器
    bool findHeader(const std::string& buf, size_t header_end,
                        const std::string& name, std::string& value) {
        const std::string want = toLowerAscii(name);   // 预先转好，循环里不用反复转
        size_t pos = 0;

        while (pos < header_end) {
            size_t line_end = buf.find("\r\n", pos);
            if (line_end == std::string::npos || line_end > header_end) {
                line_end = header_end;   // 绝不允许读越过头部区域
            }

            const std::string line = buf.substr(pos, line_end - pos);
            const size_t colon = line.find(':');
            if (colon != std::string::npos && toLowerAscii(line.substr(0, colon)) == want) {
                // 跳过 OWS（0 个或多个空格/制表符）
                size_t v = colon + 1;
                while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
                    ++v;

                value = line.substr(v);   // 只取本行的值，不能一路取到 CRLF CRLF
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                    value.pop_back();
                }
                return true;
            }

            if (line_end >= header_end) break;
            pos = line_end + 2;   // +2 跳过 CRLF
        }
        return false;
    }

    // 必须整串都是十进制数字（空串不算）。
    // stoull 是"解析到非数字就停"：stoull("10abc") 返回 10，非法值会被当成合法值收下。
    bool isAllDigits(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;   // ASCII 区间判断，不用 isdigit
        }
        return true;
    }

    struct RequestLine {
        std::string method;    // "GET"
        std::string target;    // "/hello?x=1"  原样的 request-target
        std::string path;      // "/hello"      路由用它
        std::string query;     // "x=1"         原样保留，不做 %XX 解码
        std::string version;   // "HTTP/1.1"
    };

    // 请求行 = method SP request-target SP HTTP-version。
    // 只做三条边界检查，少一条都会被构造出来的请求打穿：
    //   ① 必须正好两个空格 —— 多一个会被算进 version 导致版本不符；
    //      "GET /a b HTTP/1.1"（路径带裸空格）因此在这死掉，真要空格得写 %20
    //   ② method 非空且有长度上限，否则对方能发一个 8 KB 的"方法名"
    //   ③ target 必须以 '/' 开头（origin-form），absolute-form / CONNECT 不支持
    bool parseRequestLine(const std::string& buf, size_t header_end, RequestLine& out) {
        // 请求行是头部的第一行：到第一个 CRLF 为止
        const size_t line_end = buf.find("\r\n");
        if (line_end == std::string::npos || line_end > header_end) return false;

        const std::string line = buf.substr(0, line_end);

        const size_t sp1 = line.find(' ');
        if (sp1 == std::string::npos) return false;            // 只有一段
        const size_t sp2 = line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return false;            // 只有两段

        out.method  = line.substr(0, sp1);
        out.target  = line.substr(sp1 + 1, sp2 - sp1 - 1);
        out.version = line.substr(sp2 + 1);                    // 带多余空格时长度会不符

        if (out.method.empty() || out.method.size() > 16) return false;
        if (out.target.empty() || out.target[0] != '/') return false;
        if (out.version != "HTTP/1.1" && out.version != "HTTP/1.0") return false;

        // 只在第一个 '?' 处切，后面的 '?' 属于 query 内容
        const size_t q = out.target.find('?');
        if (q == std::string::npos) {
            out.path = out.target;
            out.query.clear();
        } else {
            out.path  = out.target.substr(0, q);
            out.query = out.target.substr(q + 1);
        }
        return true;
    }

    struct Response {
        int code = 200;
        std::string reason = "OK";
        std::string content_type = "text/html; charset=utf-8";
        std::string body;
        std::string extra_header;   // 附加响应头（405 的 Allow: ...）
    };

    // 用 if/else 不用路由表：这个文件的主题是解析，三五条路径用容器管反而干扰。
    Response routeRequest(const RequestLine& req, const std::string& body) {
        Response r;

        // 只实现 GET / POST，其余 405 并告诉对方能用什么
        if (req.method != "GET" && req.method != "POST") {
            r.code = 405;
            r.reason = "Method Not Allowed";
            r.extra_header = "Allow: GET, POST\r\n";
            r.body = "<h1>405 Method Not Allowed</h1>";
            return r;
        }

        // path 一律整串比，不做前缀匹配：前缀匹配会让 /worker、/workshop 也命中 /work。
        if (req.method == "POST" && req.path == "/echo") {
            // text/plain 而不是 text/html：浏览器就不会把用户输入当标签渲染（XSS）
            r.content_type = "text/plain; charset=utf-8";
            r.body = body.empty() ? std::string("(body is empty)") : body;
            return r;
        }
        if (req.method == "GET" && req.path == "/search") {
            r.content_type = "text/plain; charset=utf-8";
            r.body = "query (raw) = " + req.query + "\n";
            return r;
        }

        if (req.path == "/")            r.body = "<h1>index</h1>";
        else if (req.path == "/hello")  r.body = "hello";
        else if (req.path == "/first")  r.body = "first";
        else if (req.path == "/second") r.body = "second";
        else {
            r.code = 404;
            r.reason = "Not Found";
            r.body = "<h1>404 Not Found</h1>";
        }
        return r;
    }

    // 只拼字节串不 send —— 发不完怎么办是并发模型的事
    std::string buildResponse(const Response& r) {
        return "HTTP/1.1 " + std::to_string(r.code) + " " + r.reason + "\r\n" +
               "Content-Type: " + r.content_type + "\r\n" +
               "Content-Length: " + std::to_string(r.body.size()) + "\r\n" +
               r.extra_header +
               "Connection: close\r\n" +
               "\r\n" + r.body;
    }
}

int main() {
    // 客户端提前断开时 send 返回 -1 (EPIPE)，SIGPIPE 会直接杀掉整个进程
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // 必须在 bind 之前：允许覆盖处于 TIME_WAIT 的端口，否则重启撞 EADDRINUSE
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        return 1;
    }

    struct sockaddr_in address{};
    address.sin_family = AF_INET;           // IPv4
    address.sin_addr.s_addr = INADDR_ANY;   // 0.0.0.0：本机所有网卡
    address.sin_port = htons(PORT);         // 端口要转网络字节序

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    // backlog=3 是故意留小的，配合 100 并发压测能看到建连超时（SYN 重传 1s->2s）。
    // epoll.cpp / thread.cpp 已经改成 SOMAXCONN，这里保留小值当对照。
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "listening on port " << PORT << " ..." << std::endl;

    while (true) {
        // 新变量装客户端地址（accept 会覆盖它，别复用 bind 用的 address）；
        // addrlen 是"值-结果"参数，内核会改写它，所以每轮都要重置
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        // sin_port 是网络字节序，打印前用 ntohs 转回来
        std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        // 缓冲区必须在【连接这一层】：放到 while(true) 外面会被多个客户端共用，
        // 上一条的残留数据污染下一个请求 —— 极难查的 bug。
        std::string read_buffer;
        bool header_done = false;

        // 循环读到出现 CRLF CRLF 为止：判据不是"read 了一次"（半包）
        while (!header_done) {
            char buffer[1024];
            ssize_t n = read(conn_fd, buffer, sizeof(buffer));

            if (n == 0) {
                // 0 = 对端正常关闭（EOF），请求没读完就断了
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

            // 顺序很关键：必须在"找判据"之后。反过来会把"头很小但 body 很大"的合法请求误杀。
            if (!header_done && read_buffer.size() > MAX_HEADER_SIZE) {
                std::cout << "请求头过大（超过 " << MAX_HEADER_SIZE
                          << " 字节）—— 主动断开" << std::endl;
                // TODO: 更规范的做法是先回 431 (Request Header Fields Too Large) 再断开
                break;
            }
        }

        if (header_done) {
            const size_t header_end = read_buffer.find("\r\n\r\n");

            // 用 findHeader 而不是 find("Content-Length: ")：一次解决大小写 / OWS / 只在头部内找
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

            // 一个完整请求 = 头部（含结尾空行）+ body
            const size_t total = header_end + 4 + content_len;

            // body 没到齐就继续读
            while (read_buffer.size() < total) {
                char chunk[1024];
                ssize_t n = read(conn_fd, chunk, sizeof(chunk));
                if (n <= 0) {
                    std::cout << "body 没收完对端就断了" << std::endl;
                    break;
                }
                read_buffer.append(chunk, static_cast<size_t>(n));
            }

            // 收齐了才解析
            if (read_buffer.size() >= total) {
                std::cout << "收到完整请求（" << total << " 字节：头 " << (header_end + 4)
                          << " B + body " << content_len << " B）" << std::endl;

                // 头部区域 = [0, header_end)，body = 空行之后 content_len 个字节
                const std::string body = read_buffer.substr(header_end + 4, content_len);

                RequestLine line;
                Response resp;
                if (!parseRequestLine(read_buffer, header_end, line)) {
                    // 连 method/path 都没解析出来，没什么可路由的
                    resp.code = 400;
                    resp.reason = "Bad Request";
                    resp.body = "<h1>400 Bad Request</h1>";
                    std::cout << "  请求行非法 → 400" << std::endl;
                } else {
                    std::cout << "  method=" << line.method
                              << "  path=" << line.path
                              << "  query=" << (line.query.empty() ? "(none)" : line.query)
                              << "  version=" << line.version << std::endl;
                    resp = routeRequest(line, body);
                }

                const std::string response = buildResponse(resp);
                ssize_t sent = send(conn_fd, response.c_str(), response.size(), 0);
                if (sent < 0) { perror("send"); }
                std::cout << "  → " << resp.code << " " << resp.reason
                          << "（body " << resp.body.size() << " B）" << std::endl;
            }
        }

        // 这条连接处理完了，关掉分机；server_fd（总机）要一直留着
        close(conn_fd);
    }

    close(server_fd);
    return 0;
}
