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
// 关于开头的 static：函数在文件作用域加 static = internal linkage，
// 意思是"这个函数只在本文件可见"，不会和别的 .cpp 里的同名函数在链接期撞车。
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
static std::string toLowerAscii(const std::string& s) {
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
static bool findHeader(const std::string& buf, size_t header_end,
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
static bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;   // 空串不是数字
    for (char c : s) {
        if (c < '0' || c > '9') return false;   // 用 ASCII 区间判断，而不是 isdigit
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
