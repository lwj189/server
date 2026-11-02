// ===========================================================================
// p2_route.cpp —— p2.cpp + 一层薄路由：看清"服务器怎么知道你要的是哪个资源"。
// p2.cpp 只回固定 117 字节 Hello；本文件在上面只加三件事（仍是阻塞 + 一连接一线程）：
//   1. 请求行解析：拆 method / target / version，target 再切 path + query
//   2. 路由：GET /、/hello、/first、/second、/search?q=…、POST /echo，其余 404；
//      方法不认识 → 405 + Allow；请求行畸形（非 origin-form、版本不对、裸空格）→ 400
//   3. 响应构造：状态码 / Content-Type / body 由路由决定（p2.cpp 写死 200 + text/plain）
// 编译：g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/p2_route.cpp -o build/p2_route
// ⚠ 不进 CI：tests/p2_test.py 的 23 项断言按 p2.cpp 的"任何路径都返回 Hello"写，与本文件分支冲突。
// ===========================================================================

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

// ★【DoS 保护】头/体大小上限：客户端可以一直发、永不发 "\r\n\r\n"，让 read_buffer 无限涨撑爆内存。
//   8 KB 这个量级与 nginx 同源（client_header_buffer_size 默认 1k，装不下才给 large_client_header_buffers
//   默认 4×8k，单个头字段不能超过一个 buffer）；本项目 serverai/day4~day6.cpp 参考实现也是 #define … 8192。
//   MAX_BODY_SIZE 取 1 MB，与 nginx 的 client_max_body_size 默认值一致。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// ---------------------------------------------------------------------------
// HTTP 头字段解析工具。抽成三个函数对应 CP5 解析 Content-Length 踩的三个坑：
//   字段名大小写敏感 → toLowerAscii + findHeader 比较；冒号后必须有空格 → 跳过 OWS；
//   stoul 把 "10abc" 当 10 → isAllDigits 先把值卡成纯数字。
// 三个都是纯函数（无全局状态、无 IO），能脱离服务器单独测（不起进程、不连端口，0.01 秒跑完）。
// namespace { } 是匿名命名空间：internal linkage，不与别的 .cpp 同名函数撞车；等价于逐个加 static，
// 一个块包住全部更省事（以后加函数不用再补）。
// ---------------------------------------------------------------------------

// 把字符串转成小写（只处理 ASCII）。
// 为什么只做 ASCII：HTTP 头字段名限定为 ASCII token，不需要 Unicode 小写（ß、土耳其语 İ 等特例），
// 函数名里的 Ascii 就是在声明这个能力边界。
// const std::string& s 既不拷贝也不修改原串；std::string out = s 才拷贝出可改的副本。
// for (char& c : out) 的 & 至关重要：c 是元素的别名，改 c 才改得到 out；
// 写成 for (char c : out) 会静默地什么都不做 —— 能编译能运行，最难查。
// static_cast 两连转：char 在 x86 Linux 默认有符号（0xE4 读出来是 -28），而 tolower 要求
// unsigned char 的值或 EOF，传负数进去是未定义行为 —— 内层转成 228，外层再转回 char。
namespace {
    std::string toLowerAscii(const std::string& s) {
        std::string out = s;
        for (char& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    // 在"请求头区域"[0, header_end) 内查找字段 name，找到就返回值（已去掉两端空白）。
    // const std::string& buf / name 是只读参数；std::string& value 是【输出参数】：
    //   函数要同时返回"找到没"和"值是多少"，而 C++ 只能返回一个值 → bool 返"找到没"，引用带出值。
    // 调用方：std::string cl_value; if (findHeader(buf, header_end, "Content-Length", cl_value)) {…}
    //
    // 为什么必须逐行扫描，而不是直接 buf.find("content-length")：要保证字段名从【行首】开始，
    //   否则 "X-Content-Length-Foo: 1"（名字里含有 Content-Length）会被误匹配。
    //
    // 三个必须处理的协议细节，少一个都会被真实客户端或恶意请求绕过：
    //   1. 头字段名大小写不敏感 —— RFC 9110 §5.1；旧实现 find("Content-Length: ") 遇到小写就漏。
    //   2. 冒号后的空白是【可选】的（OWS = 0 个或多个空格/制表符，RFC 9112 §5）；
    //      旧实现要求恰好一个空格，会漏掉 "Content-Length:10"。
    //   3. 只在头部区域内查找：否则 body 里恰好含 "Content-Length: ..." 文本时解析器会被骗
    //      （请求走私 / 参数污染的入门级防范）。
    bool findHeader(const std::string& buf, size_t header_end,
                        const std::string& name, std::string& value) {
        const std::string want = toLowerAscii(name);   // 预先转好小写，循环里不用反复转
        size_t pos = 0;

        while (pos < header_end) {
            size_t line_end = buf.find("\r\n", pos);
            if (line_end == std::string::npos || line_end > header_end) {
                line_end = header_end;   // 防御：绝不允许读越过头部区域
            }

            const std::string line = buf.substr(pos, line_end - pos);
            const size_t colon = line.find(':');   // find 找单个字符，同样返回下标或 npos
            if (colon != std::string::npos && toLowerAscii(line.substr(0, colon)) == want) {

                // ★ 修复点 2：跳过 OWS（0 个或多个空格/制表符）；line[v] 不做越界检查，所以先比 v < line.size()
                size_t v = colon + 1;
                while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
                    ++v;

                value = line.substr(v);   // 只取【本行】的值（旧实现会一路取到 \r\n\r\n，把后续头字段也圈进来）
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                    value.pop_back();   // 去掉值尾部的空白，比如 "5  " → "5"
                }
                return true;
            }

            if (line_end >= header_end) break;   // 已经到头部区域末尾
            pos = line_end + 2;                  // +2 跳过 CRLF，继续看下一行
        }
        return false;   // 整个头部区域都没有这个字段
    }

    // 整个字符串都是十进制数字？（空串不算数字）
    //
    // 为什么需要它：stoul / stoull 的行为是"解析到非数字就停"，stoul("10abc") 返回 10 ——
    //   非法值被当成合法值接受了。先用它把值卡成"纯数字"，后面 stoull 就只可能因【数值溢出】抛异常。
    //   （RFC 9112 §6.3 要求：无效的 Content-Length 必须以 400 拒绝）
    //   旧实现直接把 substr 丢给 stoul，靠"遇到非数字就停"侥幸工作，就是这么漏的。
    // 不用 std::isdigit：参数同样有负数问题；'0' <= c <= '9' 更明确、不受 locale 影响、也更快。
    // 注意这里的 for 没有 &，对比 toLowerAscii：只读不改。
    bool isAllDigits(const std::string& s) {
        if (s.empty()) return false;   // 空串不是数字
        for (char c : s) {
            if (c < '0' || c > '9') return false;   // 用 ASCII 区间判断，而不是 isdigit
        }
        return true;
    }

    // =======================================================================
    // 请求行解析。请求行格式（RFC 9112 §3）：method SP request-target SP HTTP-version CRLF
    // 三段各有一个"必须拒绝"的边界，这里只做最必要的三条：
    //   1. 必须正好两个空格。少一个 → 请求行残缺；多出来的被算进 version，version 对不上 → 一样被拒。
    //      所以 "GET /a b HTTP/1.1"（路径里有裸空格）在这里就死了 —— URI 里要写 %20。
    //   2. method 非空且长度有上限：不加这条，对方可以发一个 8 KB 的"方法名"。
    //   3. request-target 必须以 '/' 开头（origin-form）；absolute-form（GET http://host/path）
    //      和 authority-form（CONNECT）不支持。
    // 和上面三个函数的区别：这里开始有业务含义 —— 结果不止一样东西，所以返回结构体而不是输出参数。
    // =======================================================================
    struct RequestLine {
        std::string method;    // "GET"
        std::string target;    // "/hello?x=1"  原样的 request-target
        std::string path;      // "/hello"      路由用它
        std::string query;     // "x=1"         原样保留，未解码
        std::string version;   // "HTTP/1.1"
    };

    // 输入是整块读缓冲区 + 头部结束位置（不拷贝，够用就行）
    bool parseRequestLine(const std::string& buf, size_t header_end, RequestLine& out) {
        // 请求行是头部的第一行：到第一个 "\r\n" 为止
        const size_t line_end = buf.find("\r\n");
        if (line_end == std::string::npos || line_end > header_end) return false;

        const std::string line = buf.substr(0, line_end);

        const size_t sp1 = line.find(' ');
        if (sp1 == std::string::npos) return false;            // 只有一段 → 非法
        const size_t sp2 = line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return false;            // 只有两段 → 非法

        out.method  = line.substr(0, sp1);
        out.target  = line.substr(sp1 + 1, sp2 - sp1 - 1);
        out.version = line.substr(sp2 + 1);                    // 含多余空格时长度会不符

        if (out.method.empty() || out.method.size() > 16) return false;
        if (out.target.empty() || out.target[0] != '/') return false;
        if (out.version != "HTTP/1.1" && out.version != "HTTP/1.0") return false;

        // 切 ?query：只在【第一个】'?' 处切，后面的 '?' 属于 query 内容
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

    // 路由：从"解析结果"到"回什么"。用 if/else 而不是路由表 —— 这是【解析课】，
    // 路由只是拿去验证解析结果对不对，三五条路径用容器管反而是干扰。
    struct Response {
        int code = 200;
        std::string reason = "OK";
        std::string content_type = "text/html; charset=utf-8";
        std::string body;
        std::string extra_header;   // 附加响应头（405 的 Allow: …）
    };

    Response routeRequest(const RequestLine& req, const std::string& body) {
        Response r;

        // ① 方法：本服务只实现 GET / POST，其余回 405 并告诉对方能用什么（先判方法再判路径，
        //    所以 PUT /nope 是 405 而不是 404）
        if (req.method != "GET" && req.method != "POST") {
            r.code = 405;
            r.reason = "Method Not Allowed";
            r.extra_header = "Allow: GET, POST\r\n";
            r.body = "<h1>405 Method Not Allowed</h1>";
            return r;
        }

        // ② 按 path 【整串】比较，不做前缀匹配：
        //    前缀匹配会让 /worker、/workshop 也命中 /work（这个坑在 thread 版踩过）
        if (req.method == "POST" && req.path == "/echo") {
            // body 现在终于有用了：原样回显。真服务要防 XSS，
            // 这里用 text/plain 而不是 text/html，浏览器就不会把它当标签渲染。
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

    // 拼响应字节串（只拼字符串，不 send —— 发不发、发不完怎么办是并发模型的事）
    std::string buildResponse(const Response& r) {
        return "HTTP/1.1 " + std::to_string(r.code) + " " + r.reason + "\r\n" +
               "Content-Type: " + r.content_type + "\r\n" +
               "Content-Length: " + std::to_string(r.body.size()) + "\r\n" +
               r.extra_header +
               "Connection: close\r\n" +
               "\r\n" + r.body;
    }
}

// ---------------------------------------------------------------------------
int main() {
    // 忽略 SIGPIPE：客户端提前断开时 send 不会杀掉整个进程，而是返回 -1 (EPIPE)
    signal(SIGPIPE, SIG_IGN);

    // ---- 创建监听 socket ----
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // SO_REUSEADDR 必须在 bind 之前：否则重启时端口还在 TIME_WAIT 会 EADDRINUSE
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        return 1;
    }

    // ---- 监听地址（三个字段名见 man 3type sockaddr_in）----
    struct sockaddr_in address{};
    address.sin_family = AF_INET;           // IPv4 地址族
    address.sin_addr.s_addr = INADDR_ANY;   // 0.0.0.0：监听本机所有网卡
    address.sin_port = htons(PORT);         // 端口必须转成网络字节序

    // ---- bind ----
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    // ---- listen：backlog = 已完成握手、等着被 accept 取走的队列上限（这里取 3，照 man 手册示例抄的）----
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "listening on port " << PORT << " ..." << std::endl;

    // ---- 接受连接 → 处理请求 ----
    while (true) {
        // 用新变量装客户端地址（accept 会覆盖它，别复用 bind 用的 address）
        struct sockaddr_in client_addr{};

        // addrlen 是"值-结果"参数：内核会改写它，所以每轮都要重置
        socklen_t client_len = sizeof(client_addr);

        int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (conn_fd < 0) {
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        // read_buffer 必须每条连接一份：放到 while(true) 外面会让多个客户端共用缓冲区，
        // 上一条的残留数据污染下一个请求 —— 极难查的 bug。
        std::string read_buffer;
        bool header_done = false;

        // ---- 循环读，直到请求头收全（解决半包）----
        // TCP 是字节流：一次 read 可能只拿到半个请求，也可能一次拿到两个请求，所以判据不是
        // "read 了一次"，而是"缓冲区里出现了 \r\n\r\n"（RFC 9112 §2.1）。
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

            // 【超限保护】判序很关键：必须在"找判据"之后。先看收全没有：收全了就是正常请求，size 大
            //   一点是后续 body/粘包的事，不算头部超限；没收全再看超限，超了说明对方在灌垃圾，直接断开。
            // 反过来（先判超限），一个"头很小但 body 很大"的合法请求会被误杀。
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

            // 解析 Content-Length：用 findHeader 而不是 find("Content-Length: ")，
            // 一次解决三件事：大小写不敏感 / 冒号后空白可选 / 只在头部区域内查找（详见 findHeader）。
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

            // ---- body 没到齐就继续读（判据是字节数，不是 read 次数）----
            while (read_buffer.size() < total) {
                char chunk[1024];
                ssize_t n = read(conn_fd, chunk, sizeof(chunk));
                if (n <= 0) {
                    std::cout << "body 没收完对端就断了" << std::endl;
                    break;
                }
                read_buffer.append(chunk, static_cast<size_t>(n));
            }

            // ---- 收齐了才解析：请求行 → 路由 → 拼响应 → send ----
            if (read_buffer.size() >= total) {
                // header_end 是 "\r\n\r\n" 的起点，头部本身（含那个空行）要再 +4
                std::cout << "收到完整请求（" << total << " 字节：头 " << (header_end + 4)
                          << " B + body " << content_len << " B）" << std::endl;

                // 头部区域 = [0, header_end)：不含结尾那 4 个字节的空行。
                // body = 空行之后的 content_len 个字节。两者都不越界，因为上面
                // 已经确认 read_buffer.size() >= total。
                const std::string body = read_buffer.substr(header_end + 4, content_len);

                RequestLine line;
                Response resp;
                if (!parseRequestLine(read_buffer, header_end, line)) {
                    // 请求行非法：连 method/path 都没解析出来，没什么可路由的
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

        // 这条连接处理完了就关；server_fd 要一直留着
        close(conn_fd);
    }

    close(server_fd);
    return 0;
}
