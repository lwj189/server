// http.hpp -- HTTP 协议层：解析请求、拼响应。不涉及 epoll，也不涉及线程。
// epoll.cpp 和 thread.cpp 共用这一份。
#pragma once

#include <cctype>
#include <cstddef>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>

// 头 / 体上限，防"只发头不发完"的连接把内存吊住。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// 日志必须整行原子：cout << a << b << c 是三次调用，两个线程同时写会交错成
// "新连接 fd=新连接 fd=57"，而测试是按行读日志的。
// 必须 inline（C++17）不能用 static：static 在头文件里是内部链接，两个 TU
// 链进同一个程序会变成两把独立的锁、两份 g_log_requests。
inline std::mutex g_log_mutex;

inline void logLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << line << std::endl;   // endl 要 flush，重定向到文件时测试才读得到
}

// --quiet 关掉逐请求日志（压测用）。
// [[maybe_unused]] 不能省：epoll.cpp 不用这个开关，而 CI 有警告即失败。
[[maybe_unused]] inline bool g_log_requests = true;

// 每条连接的状态。
// 一次 read 可能只拿到半个请求，半个请求得存下来等下次事件 —— 所以缓冲区
// 必须挂在连接对象上，不能是 read 函数的局部变量。
struct ClientContext {
    int fd = -1;

    // 这条连接自己的缓冲区。多条连接共用会互相污染。
    std::string inbuf;

    // 回完当前请求后要不要关：客户端要 close，或这条连接已被判非法。
    bool want_close = false;

    // 只在 thread.cpp 用：true = fd 已从 epoll 摘掉、正在工人手里。
    // 主线程看到 busy 就不能再碰这个 fd（不 read / 不解析 / 不 close），
    // 否则就是两个线程操作同一个 fd。epoll.cpp 单线程，永远是 false。
    bool busy = false;
};

// 以下三个纯函数从 p2.cpp 原样搬来，不依赖 socket 和 epoll。

// 只转 ASCII 小写。man 3 tolower 要求参数是 unsigned char 的值或 EOF，
// 直接传 char（负数）是未定义行为。
static std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 在头部区域 [0, header_end) 内按字段名取值，返回时去掉两端空白。
// 三处容易漏：字段名大小写不敏感；冒号后的 OWS 可以是 0 个；只能在这段
// 区域内找 —— 否则 body 里的同名文本会被当成头字段。
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
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;   // 跳过 OWS
            value = line.substr(v);
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

// 整串都是十进制数字？（空串不算）
// 不能用 stoull 代替：stoull("10abc") 返回 10，非法值会被当成合法值收下。
// 也不用 std::isdigit：同样是负数入参的 UB；'0' <= c <= '9' 明确，且不受 locale 影响。
// 这里的 for 没有 &，对比上面的 toLowerAscii：只读不改。
static bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// 从 ctx.inbuf 里取一个完整请求。每次 EPOLLIN 调一次，不够就原地返回，
// 进度留在 ctx.inbuf 里（p2.cpp 那版是 while 循环堵到收全为止）。
//
// 三种结果，调用方必须区别对待：
//   NeedMore  还不够一个请求，等下次事件
//   Ready     out.raw / out.total 有效，去发响应并消费这些字节
//   Rejected  请求非法，want_close 已置 true，直接收连接、不回任何东西
enum class ParseResult { NeedMore, Ready, Rejected };

struct ParsedRequest {
    ParseResult result = ParseResult::NeedMore;
    std::string raw;     // Ready 时有效：完整请求（头部 + body）
    size_t total = 0;    // Ready 时有效：这个请求占多少字节
};

static ParsedRequest parseRequest(ClientContext& ctx) {
    ParsedRequest out;

    // ① 头部定界 \r\n\r\n
    const size_t header_end = ctx.inbuf.find("\r\n\r\n");
    if (header_end == std::string::npos) {                  // 还没收全头部
        // 对方可能永远不发定界符。判断顺序不能反：先看收全没有，再看超限 ——
        // 反过来会把"头很小但 body 很大"的合法请求误杀。
        if (ctx.inbuf.size() > MAX_HEADER_SIZE) {
            logLine("请求头过大 —— 丢弃 fd=" + std::to_string(ctx.fd));
            ctx.want_close = true;   // 判死刑的连接一律要关
            out.result = ParseResult::Rejected;
            return out;
        }
        return out;   // NeedMore：等下次事件
    }

    // ② Content-Length
    size_t content_len = 0;   // 没这个头字段就按 0 处理（普通 GET 就是这种）
    std::string cl_value;
    if (findHeader(ctx.inbuf, header_end, "Content-Length", cl_value)) {
        if (!isAllDigits(cl_value)) {
            logLine("Content-Length 非法（不是纯数字）—— 丢弃 fd=" + std::to_string(ctx.fd));
            ctx.want_close = true;
            out.result = ParseResult::Rejected;
            return out;
        }
        try {
            const unsigned long long v = std::stoull(cl_value);
            if (v > MAX_BODY_SIZE) {
                // 声明一个巨大 body 却不发，同样能吊住服务器
                logLine("body 过大（Content-Length=" + std::to_string(v) + "）—— 丢弃 fd=" +
                        std::to_string(ctx.fd));
                ctx.want_close = true;
                out.result = ParseResult::Rejected;
                return out;
            }
            content_len = static_cast<size_t>(v);
        } catch (const std::exception&) {
            // 只可能是数字长得离谱，stoull 溢出
            logLine("Content-Length 溢出 —— 丢弃 fd=" + std::to_string(ctx.fd));
            ctx.want_close = true;
            out.result = ParseResult::Rejected;
            return out;
        }
    }

    // ③ 客户端要不要关连接。必须在拼响应之前问出来，响应里的 Connection 头跟着它走。
    // 这一段和上面找 Content-Length 只是形状像：那个要把数字转整数，这个只比字符串。
    std::string conn_value;
    if (findHeader(ctx.inbuf, header_end, "Connection", conn_value) &&
        toLowerAscii(conn_value) == "close") {
        ctx.want_close = true;
    }

    // ④ 一个完整请求 = 头部 + 结尾空行 + body。header_end 指向 "\r\n\r\n" 的第一个 \r。
    const size_t total = header_end + 4 + content_len;

    // 没齐就原地返回，进度留在 ctx.inbuf 里，下一个 EPOLLIN 进来重算
    if (ctx.inbuf.size() < total) {
        return out;   // NeedMore
    }

    out.result = ParseResult::Ready;
    out.raw = ctx.inbuf.substr(0, total);
    out.total = total;
    return out;
}

// 只拼字符串，不发。want_close 决定 Connection 头：客户端说要关，我们也回 close。
static std::string buildResponse(const std::string& body, bool want_close) {
    const std::string conn_header = want_close ? "Connection: close\r\n"
                                               : "Connection: keep-alive\r\n";
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/html\r\n"
           "Content-Length: " +
           std::to_string(body.size()) + "\r\n" + conn_header + "\r\n" + body;
}
