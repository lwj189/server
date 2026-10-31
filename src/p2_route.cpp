// ===========================================================================
// p2_route.cpp —— p2.cpp + 一层薄路由（看清"服务器怎么知道你要的是哪个资源"）
//
// p2.cpp 只做协议最小闭环：收齐请求头 → 判 \r\n\r\n → 回 117 字节 Hello。
// 它不关心你请求的是 /xxx 还是 /yyy，一律回同一个字符串 —— 看清楚 socket 这一层够用，
// 但回答不了"你的服务器怎么区分 path"。
//
// 本文件在 p2.cpp 上加了三个薄层（其余逐行相同，仍是阻塞 + 一连接一线程的模型）：
//   1. 请求行解析：拆 method / target / version，target 再切成 path + query
//   2. 路由：GET /、/hello、/first、/second、/search?q=...、POST /echo，其余 404；
//      方法不认识 → 405 + Allow 头；请求行畸形（非 origin-form、版本不对、路径带裸空格）→ 400
//   3. 响应构造：状态码 / Content-Type / body 由路由决定（p2.cpp 是写死 200 + text/plain）
//
// ⚠ 它和 p2.cpp 是两个独立目标，编哪个都行：
//     g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/p2_route.cpp -o build/p2_route
//
// ⚠ 它【不】进 CI 的测试矩阵：tests/p2_test.py 的 23 项断言是按 p2.cpp 写的
//   （"任何路径都返回 Hello"），和这里的分支行为冲突。要让它也跑那套测试，得先改断言。
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

// ★【DoS 保护】请求头 / 请求体大小上限
//   客户端可能一直发数据、永远不发 "\r\n\r\n"，让 read_buffer 无限增长把内存撑爆。
//   8 KB 这个量级和 nginx 的默认值同源：nginx 的 client_header_buffer_size 默认 1k，
//   装不下时才分配 large_client_header_buffers（默认 4 个 × 8k，单个头字段不能超过一个 buffer）。
//   本项目 serverai/day4.cpp ~ day6.cpp 的参考实现用的是 #define MAX_HEADER_SIZE 8192。
//   MAX_BODY_SIZE 取 1 MB，与 nginx 的 client_max_body_size 默认值一致。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// ---------------------------------------------------------------------------
// HTTP 头字段解析工具
//
// 为什么单独抽成三个函数：CP5 解析 Content-Length 时踩了三个坑，它们正好一一对应——
//   · 头字段名大小写敏感      → toLowerAscii + findHeader 里的比较
//   · 冒号后必须有空格        → findHeader 里跳过 OWS
//   · stoul 把 "10abc" 当 10  → isAllDigits 先把值卡成纯数字
//
// 三个都是【纯函数】：不依赖全局变量、不做 IO，输入相同输出就相同。
// 好处是可以脱离服务器单独测试（不用起进程、不用连端口，0.01 秒跑完）。
//
// 关于外面那个 namespace { }（匿名命名空间）：里面的东西只在本文件可见（internal
// linkage），不会和别的 .cpp 里的同名函数在链接期撞车。老写法是给每个函数单独加
// static，C++ 里两者等价；namespace 版更省事 —— 一个块包住全部，以后加函数不用再补。
// ---------------------------------------------------------------------------

// 把字符串转成小写（只处理 ASCII）。
//
// 为什么只做 ASCII：HTTP 的【头字段名】限定为 ASCII token，不需要真正的 Unicode 小写转换
// （那个还要处理 ß、土耳其语 İ 之类的特例）。函数名里的 Ascii 就是在声明这个能力边界。
//
// 语法点：
//   · const std::string& s —— const 引用：既不拷贝原串（省一次内存分配），也承诺不修改它。
//   · std::string out = s; —— 拷贝一份副本，因为接下来要改它。
//   · for (char& c : out)  —— 范围 for（C++11）。这里的 & 至关重要：
//                             c 是 out 中那个字符的【别名】，改 c 就是改 out。
//                             如果写成 for (char c : out)，c 只是拷贝，改它对 out 毫无影响，
//                             函数会【静默地什么都不做】—— 这类 bug 能编译能运行，最难查。
//                             口诀：要改原容器的元素才加 &，只读不加。
//   · static_cast 两连转 —— char 在 x86 Linux 上默认【有符号】（0xE4 这种 UTF-8 字节读出来是 -28），
//                             而 man 3 tolower 要求参数是 unsigned char 的值或 EOF，
//                             传负数进去是【未定义行为】。所以：
//                               内层 static_cast<unsigned char> 先把 -28 变成 228，安全地交给 tolower；
//                               外层 static_cast<char> 再把结果转回来存进字符串。
namespace {
    std::string toLowerAscii(const std::string& s) {
        std::string out = s;
        for (char& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    // 在"请求头区域"[0, header_end) 内查找字段 name，找到就返回值（已去掉两端空白）。
    //
    // 【参数里的两个 C++ 惯用法】
    //   · const std::string& buf / name —— 只读参数。const 是在告诉调用者"我不会改它们"。
    //   · std::string& value            —— 【输出参数】：不加 const，函数会把结果写进去。
    //   函数要同时返回两样东西（"找到没" + "值是多少"），而 C++ 只能返回一个值，
    //   于是：用返回值表示"找到没"(bool)，用引用参数表示"值是多少"。调用方长这样：
    //       std::string cl_value;
    //       if (findHeader(read_buffer, header_end, "Content-Length", cl_value)) { ... }
    //
    // 【为什么必须逐行扫描，而不是直接 buf.find("content-length")】
    //   要保证字段名是【从行首开始】的。否则这个字段会被误匹配：
    //       X-Content-Length-Foo: 1        ← 名字里含有 "Content-Length"
    //   逐行 + 从行首比到冒号，才是正确的字段名匹配方式。
    //
    // 【这里有三个必须处理的协议细节，少一个都会被真实客户端或恶意请求绕过】
    //   1. 头字段名【大小写不敏感】—— RFC 9110 §5.1 "Field names are case-insensitive"。
    //      所以 content-length / Content-Length / CONTENT-LENGTH 是同一个字段。
    //      旧实现用 read_buffer.find("Content-Length: ") 直接匹配，遇到小写就会漏掉。
    //   2. 冒号后面的空白是【可选】的（OWS = 0 个或多个空格/制表符）—— RFC 9112 §5 的
    //      field-line = field-name ":" OWS field-value OWS。
    //      所以 "Content-Length:10" 和 "Content-Length:  10" 都是合法的。
    //      旧实现要求冒号后恰好一个空格，会漏掉前者。
    //   3. 只在【头部区域内】查找：否则 body 里恰好含有 "Content-Length: ..." 文本时，
    //      解析器会被 body 内容骗到（请求走私 / 参数污染的入门级防范）。
    bool findHeader(const std::string& buf, size_t header_end,
                        const std::string& name, std::string& value) {
        const std::string want = toLowerAscii(name);   // 预先转好小写，循环里不用反复转
        size_t pos = 0;

        while (pos < header_end) {
            // buf.find("\r\n", pos) 返回 "\r\n" 的下标；找不到时返回 std::string::npos
            // （npos 是 size_t 的最大值，所以必须单独判断，不能拿它当普通下标用）
            size_t line_end = buf.find("\r\n", pos);
            if (line_end == std::string::npos || line_end > header_end) {
                line_end = header_end;   // 防御：绝不允许读越过头部区域
            }

            // line.substr(0, colon) = 从下标 0 开始取 colon 个字符 → 冒号【前面】的字段名
            const std::string line = buf.substr(pos, line_end - pos);
            const size_t colon = line.find(':');   // find 找单个字符，同样返回下标或 npos
            if (colon != std::string::npos && toLowerAscii(line.substr(0, colon)) == want) {
                // ★ 修复点 1：两边都转小写再比较，所以 content-length 也能认出来

                // ★ 修复点 2：跳过 OWS（0 个或多个空格/制表符）。
                //   line[v] 按下标取字符，不做越界检查，所以循环条件必须先比 v < line.size()
                size_t v = colon + 1;
                while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
                    ++v;

                value = line.substr(v);   // 只取【本行】的值（旧实现会一路取到 \r\n\r\n，把后续头字段也圈进来）
                // back() = 最后一个字符；pop_back() = 删掉最后一个字符
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
    // 【为什么需要它】std::stoul / std::stoull 的行为是"解析到非数字就停"，
    //   所以 stoul("10abc") 会返回 10 —— 非法值被当成合法值接受了。
    //   先用这个函数把值卡成"纯数字"，后面 stoull 就只可能因为【数值太大溢出】而抛异常。
    //   （RFC 9112 §6.3 要求：无效的 Content-Length 必须以 400 拒绝）
    //   旧实现直接把 substr 丢给 stoul，靠"遇到非数字就停"侥幸工作，就是这么漏的。
    //
    // 【为什么不用 std::isdigit(c)】
    //   1. 又是那个负数问题 —— isdigit 的参数同样必须是 unsigned char 的值或 EOF；
    //   2. '0' <= c <= '9' 更明确：只接受 ASCII 数字，不受 locale 影响，也更快。
    //
    // 【注意这里的 for 没有 &】对比上面的 toLowerAscii：这里只读不改，所以不需要引用。
    bool isAllDigits(const std::string& s) {
        if (s.empty()) return false;   // 空串不是数字
        for (char c : s) {
            if (c < '0' || c > '9') return false;   // 用 ASCII 区间判断，而不是 isdigit
        }
        return true;
    }

    // =======================================================================
    // 请求行解析
    //
    // 请求行长这样（RFC 9112 §3）：
    //       method SP request-target SP HTTP-version CRLF
    //       GET    /hello?x=1      HTTP/1.1
    //
    // 三段各有一个"必须拒绝"的边界，这里只做最必要的三条：
    //   1. 必须正好两个空格。少一个 → 请求行残缺；
    //      多出来的会被算进 version，于是 version 对不上 → 一样被拒。
    //      所以 "GET /a b HTTP/1.1"（路径里有裸空格）在这里就死了 ——
    //      URI 里不允许裸空格，真要有空格必须写成 %20。
    //   2. method 非空且长度有上限：不加这条，对方可以发一个 8 KB 的"方法名"。
    //   3. request-target 必须以 '/' 开头（origin-form）。
    //      absolute-form（GET http://host/path）和 authority-form（CONNECT）不支持。
    //
    // 和上面三个函数的区别：这里开始【有业务含义】了 —— 解析结果不是数字或
    // "找到没"，而是"这个请求想干什么"。所以它返回一个结构体，而不是输出参数。
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

    // =======================================================================
    // 路由：从"解析结果"到"回什么"
    //
    // 用 if/else 而不是路由表：这个文件是【解析课】，路由只是拿去验证解析结果对不对，
    // 三五条路径用容器管反而是干扰。
    // =======================================================================
    struct Response {
        int code = 200;
        std::string reason = "OK";
        std::string content_type = "text/html; charset=utf-8";
        std::string body;
        std::string extra_header;   // 附加响应头（405 的 Allow: …）
    };

    Response routeRequest(const RequestLine& req, const std::string& body) {
        Response r;

        // ① 方法：本服务只实现 GET / POST，其余回 405 并告诉对方能用什么
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

    // ---- 步骤 1：创建 socket ----
    // AF_INET = IPv4；SOCK_STREAM = 可靠字节流(TCP)；0 = 默认协议
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // ---- 步骤 2：SO_REUSEADDR（必须在 bind 之前）----
    // 允许 bind 覆盖处于 TIME_WAIT 的端口，否则重启会 EADDRINUSE
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        return 1;
    }

    // ---- 步骤 3：填写监听地址 ----
    // 三个字段名见 man 3type sockaddr_in
    struct sockaddr_in address{};
    address.sin_family = AF_INET;           // IPv4 地址族
    address.sin_addr.s_addr = INADDR_ANY;   // 0.0.0.0：监听本机所有网卡
    address.sin_port = htons(PORT);         // 端口必须转成网络字节序

    // ---- 步骤 4：bind ----
    // 把地址"挂"到 socket 上；第二参数要强转成通用地址 struct sockaddr*
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    // ---- 步骤 5：listen ----
    // 主动套接字 → 被动套接字；backlog = 已完成握手、等 accept 取走的队列上限
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        return 1;
    }

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

            // ---- 步骤 7.4：收齐了才解析 ----
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

        // 这条连接处理完了，关掉分机；server_fd（总机）要一直留着
        close(conn_fd);
    }

    close(server_fd);
    return 0;
}
