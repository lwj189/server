// ===========================================================================
// src/p2.cpp：阻塞串行基线 —— 不用 epoll、不开线程，一次只服务一条连接。
// tests/p2_test.py 的 23 项断言就是按这一版写的，另外三个版本拿它当参照。
// 编译运行：g++ -std=c++17 -O2 src/p2.cpp -o build/p2 && build/p2
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
//   客户端可以一直发数据、永远不发 "\r\n\r\n"，让 read_buffer 无限增长把内存撑爆。
//   8 KB 与 nginx 的 large_client_header_buffers（默认 4 个 × 8k，单个头字段不能超过一个 buffer）同源；
//   serverai/day4.cpp ~ day6.cpp 的参考实现同样是 #define MAX_HEADER_SIZE 8192。
//   MAX_BODY_SIZE 1 MB，对齐 nginx 的 client_max_body_size 默认值。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// ---------------------------------------------------------------------------
// HTTP 头字段解析工具：三个【纯函数】，不依赖全局变量、不做 IO，输入相同输出相同，
// 所以能脱离服务器单独测试（不起进程、不连端口，0.01 秒跑完）。
// 解析 Content-Length 有三个容易踩的坑，正好和这三个函数一一对应——
//   · 头字段名大小写敏感      → toLowerAscii + findHeader 里的比较
//   · 冒号后必须有空格        → findHeader 里跳过 OWS
//   · stoul 把 "10abc" 当 10  → isAllDigits 先把值卡成纯数字
// 文件作用域的 static = internal linkage：只在本文件可见，不会和别的 .cpp 里的
// 同名函数在链接期撞车。
// ---------------------------------------------------------------------------

// 把字符串转成小写（只处理 ASCII：HTTP 头字段名限定为 ASCII token，不需要真正的
// Unicode 小写转换，不需要处理 ß、土耳其语 İ 之类的特例；Ascii 就是这条能力边界）。
// 两个能编译、能跑、却静默出错的坑：
//   · for (char& c : out) 的 & 不能省 —— 写成 for (char c : out) 时 c 只是拷贝，
//     改它对 out 毫无影响，函数会【静默地什么都不做】，最难查。
//   · static_cast 两连转 —— char 在 x86 Linux 默认【有符号】（0xE4 这种 UTF-8 字节读出来是 -28），
//     而 man 3 tolower 要求参数是 unsigned char 的值或 EOF，传负数进去是【未定义行为】：
//     内层 static_cast<unsigned char> 把 -28 变成 228 交给 tolower，外层再转回 char。
static std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 在"请求头区域"[0, header_end) 内查找字段 name，找到就返回值（已去掉两端空白）。
// 参数：const std::string& 是只读参数；std::string& value 是【输出参数】——
// C++ 只能返回一个值，所以用 bool 表示"找到没"、用引用参数带回"值是多少"。
//
// 为什么逐行扫描而不是直接 buf.find("content-length")：要保证字段名【从行首开始】，
// 否则 "X-Content-Length-Foo: 1" 会被误匹配。
//
// 三个必须处理的协议细节，少一个都会被真实客户端或恶意请求绕过：
//   1. 头字段名【大小写不敏感】（RFC 9110 §5.1）—— content-length / CONTENT-LENGTH
//      是同一个字段；旧实现 find("Content-Length: ") 遇到小写就会漏掉。
//   2. 冒号后的空白是【可选】的（OWS = 0 个或多个空格/制表符，RFC 9112 §5 的
//      field-line = field-name ":" OWS field-value OWS），所以 "Content-Length:10"
//      也合法；旧实现要求冒号后恰好一个空格，会漏掉前者。
//   3. 只在【头部区域内】查找：否则 body 里恰好含有 "Content-Length: ..." 文本时，
//      解析器会被 body 内容骗到（请求走私 / 参数污染的入门级防范）。
static bool findHeader(const std::string& buf, size_t header_end,
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

            // 跳过 OWS；line[v] 不做越界检查，所以循环条件必须先比 v < line.size()
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
// 为什么需要：stoul / stoull 的行为是"解析到非数字就停"，stoul("10abc") 返回 10 ——
// 非法值被当成合法值接受了。先卡成纯数字，后面 stoull 就只可能因【溢出】抛异常
// （RFC 9112 §6.3 要求：无效的 Content-Length 必须以 400 拒绝）。
// 不用 std::isdigit：同样是负数入参的 UB；'0' <= c <= '9' 更明确、不受 locale 影响。
// 注意这里 for 没有 &：只读不改，不需要引用（对比 toLowerAscii）。
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

    // 建监听 socket（IPv4 + TCP）
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // SO_REUSEADDR 必须在 bind 之前：允许 bind 覆盖处于 TIME_WAIT 的端口，否则重启会 EADDRINUSE
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt");
        return 1;
    }

    // 监听地址：IPv4、INADDR_ANY（本机所有网卡）、端口转网络字节序
    struct sockaddr_in address{};
    address.sin_family = AF_INET;           // IPv4 地址族
    address.sin_addr.s_addr = INADDR_ANY;   // 0.0.0.0：监听本机所有网卡
    address.sin_port = htons(PORT);         // 端口必须转成网络字节序

    // 把地址"挂"到 socket 上（地址要强转成通用地址 struct sockaddr*）
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    // listen：主动套接字 → 被动套接字；backlog = 已完成握手、等 accept 取走的队列上限
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "listening on port " << PORT << " ..." << std::endl;

    // 主循环：串行 —— 一条连接从 read 走到 send 完才回来 accept 下一条，
    // 这就是"阻塞基线"的含义（并发能力 = 1）。
    while (true) {
        // accept 会覆盖地址和长度，所以每轮都用新变量（addrlen 是"值-结果"参数，内核会改写它）
        struct sockaddr_in client_addr{};

        socklen_t client_len = sizeof(client_addr);

        // accept 返回这条连接的新 fd；失败多为可恢复错误，continue 继续等
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

        // read_buffer 必须每条连接一份：放到 while(true) 外面会让多个客户端共用缓冲区，
        // 上一条的残留数据会污染下一个请求 —— 极难查的 bug。
        std::string read_buffer;
        bool header_done = false;

        // 循环读到缓冲区里出现 "\r\n\r\n" 才算头收全（半包 / 粘包，RFC 9112 §2.1）：
        // 判据是缓冲区的【内容】，而不是 read 了几次。
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

            // 【超限保护】判断顺序很关键：先看收全没有（收全了 size 大是后续 body/粘包的事，
            // 不算头部超限），没收全再看超限（超了说明对方在灌垃圾，直接断开）。反过来判，
            // 一个"头很小但 body 很大"的合法请求会被误杀。
            if (!header_done && read_buffer.size() > MAX_HEADER_SIZE) {
                std::cout << "请求头过大（超过 " << MAX_HEADER_SIZE
                          << " 字节）—— 主动断开" << std::endl;
                // 这里直接断开；更规范是先 send 431 (Request Header Fields Too Large) 再断，
                // serverai/day5.cpp 就是这么做的。
                break;
            }
        }

        if (header_done) {
            const size_t header_end = read_buffer.find("\r\n\r\n");

            // 解析 Content-Length：用 findHeader 而不是 find("Content-Length: ")，
            // 一次解决大小写不敏感 / 冒号后空白可选 / 只在头部区域内查找三件事。
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
                        // DoS 防护：声明一个巨大的 body 却不发，就能吊住服务器
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

            // 收齐了才处理并响应
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

        // 每条连接处理完就 close；监听 fd（server_fd）要一直留着
        close(conn_fd);
    }

    close(server_fd);
    return 0;
}
