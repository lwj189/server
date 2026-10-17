// ===========================================================================
// http.hpp：HTTP 解析 / 响应构造 —— 与"用什么并发模型"无关的部分
//
// 为什么要把这些抽出来？
//   项目里现在有两个服务器：
//     · epoll.cpp       单线程 Reactor          （基线，36 项测试守着它）
//     · thread.cpp 单 Reactor + 线程池     （简历主角）
//   两者的【并发模型】完全不同，但【HTTP 怎么解析、响应怎么拼】一模一样。
//   抄两份的话，改一处忘一处，两份代码迟早漂移 —— 所以抽到这个头文件。
//
//   判断标准很简单：这里的所有东西都【不依赖 epoll，也不依赖线程】——
//   输入一段字符串，输出解析结果。所以谁都能用。
//
// 注意 buildResponse 只负责"拼字符串"，不负责 send ——
// 谁发、什么时候发、发不完怎么办，是各个服务器的并发模型决定的。
// ===========================================================================

#pragma once   // 同一个 .cpp 里 include 两次也只展开一次

#include <cctype>     // std::tolower
#include <cstddef>    // size_t
#include <exception>  // std::exception（stoull 溢出时抛的那个）
#include <iostream>   // std::cout / std::endl
#include <mutex>      // std::mutex / std::lock_guard（多线程日志用）
#include <string>     // std::string

// ★【DoS 保护】请求头 / 请求体大小上限（和 p2.cpp 同一套值）
//   没有上限的实现会被"一直发数据、永远不发 \r\n\r\n"的客户端吊死（内存涨爆）。
//   MAX_HEADER_SIZE = 8 KB 与 nginx 的 large_client_header_buffers 同量级；
//   MAX_BODY_SIZE = 1 MB 与 nginx 的 client_max_body_size 默认值一致。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// ===========================================================================
// 日志：多线程下必须"整行原子"
//
// 为什么：std::cout << a << b << c 不是一次调用，是【三次】——
//   两个线程同时写，输出会交错成 "新连接 fd=新连接 fd=57" 这种东西。
//   而测试里有【按行读服务器日志】的（keepalive_test 找 'GET /first'、
//   balance_test 找哨兵报警），日志一乱就变成假红。
//
// ⚠ std::endl 会 flush：日志重定向到文件时测试才能及时读到，不能换成 '\n'。
// ===========================================================================
static std::mutex g_log_mutex;

static void logLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << line << std::endl;
}

// 压测开关：每个请求都打日志（还带 flush）会成为瓶颈。
// thread.cpp 用 --quiet 关掉它。
// ⚠ [[maybe_unused]] 不能省：epoll.cpp 是单线程的、没解析 argv，用不到这个开关，
//   而 CI 是"有警告即失败" —— 没有这个属性就会报 -Wunused-variable。
[[maybe_unused]] static bool g_log_requests = true;

// ===========================================================================
// 每条连接的状态
//
// 为什么需要它？
//   "一条连接"不能只是一个 fd 号 —— 连接相关的所有东西如果活在事件处理的
//   【栈帧】里，事件一处理完就烟消云散。
//   一旦要处理半包 / 粘包，就必须把"这条连接已经收到哪儿了"存下来，
//   而栈帧留不住它 → 状态必须从栈上搬到外面的容器里。
//
//   这就是"状态机切片"：把"一个连续执行的函数"改造成"每次事件恢复一点进度"，
//   所以"进度"得有个地方放。
// ===========================================================================
struct ClientContext {
    int fd = -1;

    // 这条连接自己的读缓冲区。
    // 为什么必须"每条连接一个"：TCP 是字节流，一次 read 拿到的可能只是半个请求。
    // 半个请求得先存着，等剩下的到了再拼起来 —— 而"等"意味着函数要返回，
    // 栈上的局部变量活不到下一次事件，所以只能存在这里。
    std::string inbuf;

    // 这条连接【回完当前请求之后】要不要关？
    // 谁说了算：客户端在请求头里说（Connection: close）。
    // 出错的连接也必须关（头过大 / Content-Length 非法 …），也是靠这个成员。
    //     true  = 该关了（客户端要求，或这条连接被判为非法）
    //     false = 留着，继续服务后面的请求（HTTP/1.1 的默认行为）
    bool want_close = false;

    // ---- 只在 thread.cpp（单 Reactor + 线程池）里用得到 ----
    // true = 这条连接的 fd 已经从 epoll 摘掉、正躺在工人手里。
    // 主线程看到 busy 就必须【完全别碰它】—— 不 read、不解析、不关，
    // 否则就是两个线程同时操作同一个 fd（数据竞争 / use-after-close）。
    //
    // epoll.cpp 是单线程的，这个字段永远是 false，不影响它任何行为。
    bool busy = false;
};

// ===========================================================================
// 下面三个纯函数是从 p2.cpp 原样搬过来的
//
// 它们不依赖 socket、不依赖 epoll —— 输入一段字符串，输出解析结果。
// 正因为是纯函数，"搬家"才这么省事：换个文件照样能用、一个字都不用改。
// ===========================================================================

// 只转 ASCII 小写。为什么不用真 Unicode 小写：HTTP 头字段名限定为 ASCII token，
// 而且 std::tolower 的参数必须是 unsigned char 的值（负数进去是未定义行为）。
static std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 在"头部区域"[0, header_end) 内查找字段 name，找到就返回值（已去掉两端空白）。
// 三个协议细节：① 字段名大小写不敏感（RFC 9110 §5.1）
//              ② 冒号后的 OWS 可为 0 个（RFC 9112 §5）
//              ③ 只在头部区域内找 —— 否则 body 里的同名文本会把解析器骗到
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

// 整个字符串都是十进制数字？（空串不算）
// 为什么要它：std::stoull 是"解析到非数字就停"，stoull("10abc") 会返回 10 ——
// 非法值被当成合法值接受了（RFC 9112 §6.3 要求这种情况以 400 拒绝）。
static bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// ===========================================================================
// 解析：从 ctx.inbuf 里取出【一个完整请求】
//
// 这是"状态机切片"最直观的一处：
//   p2.cpp：一个 while 循环堵在那里读到收全为止（连续执行，栈上有 read_buffer）
//   这里  ：每次事件调用一次，不够就【原地返回】，进度留在 ctx.inbuf 里
//
// 三种结果，调用方必须区别对待：
//   NeedMore —— 还不够一个完整请求，等下次事件（什么都不做）
//   Ready    —— out.raw / out.total 有效，调用方去发响应 + 消费字节
//   Rejected —— 请求非法（头过大 / Content-Length 非法…），ctx.want_close 已置 true；
//               调用方【直接收掉连接，不要回任何东西】（和 p2.cpp 的行为一致）
// ===========================================================================
enum class ParseResult { NeedMore, Ready, Rejected };

struct ParsedRequest {
    ParseResult result = ParseResult::NeedMore;
    std::string raw;     // Ready 时有效：完整请求文本（头部 + body）
    size_t total = 0;    // Ready 时有效：这个请求一共占多少字节
};

static ParsedRequest parseRequest(ClientContext& ctx) {
    ParsedRequest out;

    // ---- ① 头部定界：找 \r\n\r\n（RFC 9112 §2.1）----
    const size_t header_end = ctx.inbuf.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        // 头部还没收全。但别无限等 —— 对方可能永远不发定界符（DoS）。
        // ⚠ 判断顺序和 p2.cpp 一致：先看"收全没有"，再看"超限"。
        //   反过来会把"头很小但 body 很大"的合法请求误杀。
        if (ctx.inbuf.size() > MAX_HEADER_SIZE) {
            logLine("请求头过大 —— 丢弃 fd=" + std::to_string(ctx.fd));
            ctx.want_close = true;   // 判死刑的连接一律要关（下面 3 处错误分支同理）
            out.result = ParseResult::Rejected;
            return out;
        }
        return out;   // NeedMore：等下次事件
    }

    // ---- ② 解析 Content-Length（CP5 的三个坑由上面两个纯函数解决）----
    size_t content_len = 0;   // 没有这个头字段就按 0 处理（普通 GET 就是这种）
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
                // 也是 DoS 防护：声明一个巨大的 body 却不发，就能吊住服务器
                logLine("body 过大（Content-Length=" + std::to_string(v) + "）—— 丢弃 fd=" +
                        std::to_string(ctx.fd));
                ctx.want_close = true;
                out.result = ParseResult::Rejected;
                return out;
            }
            content_len = static_cast<size_t>(v);
        } catch (const std::exception&) {
            // 走到这里只可能是数字长得离谱（stoull 溢出）
            logLine("Content-Length 溢出 —— 丢弃 fd=" + std::to_string(ctx.fd));
            ctx.want_close = true;
            out.result = ParseResult::Rejected;
            return out;
        }
    }

    // ---- 客户端要不要关连接？（HTTP/1.1 默认复用，客户端说 close 才关）----
    // 位置讲究：必须在【拼响应之前】问出来，因为响应里的 Connection 头要跟着它走。
    //
    // ⚠ 和上面找 Content-Length 只有【前半截】像（都是 findHeader 取值），后半截完全不同：
    //   那个要"把数字串转成整数"（isAllDigits / stoull / MAX_BODY_SIZE），
    //   这个只要"比字符串"。照抄形状可以，抄进来的每一行都得知道在干嘛。
    //
    // 值可能是 "close" / "keep-alive" / "Close"… → 先 toLowerAscii 再比（RFC 9110 §5.1）
    std::string conn_value;
    if (findHeader(ctx.inbuf, header_end, "Connection", conn_value) &&
        toLowerAscii(conn_value) == "close") {
        ctx.want_close = true;
    }

    // ---- ③ 一个完整请求 = 头部 + 结尾空行 + body，一共多少字节？----
    //     header_end 指向 "\r\n\r\n" 的【第一个 \r】，头部内容本身不含这 4 个字节。
    const size_t total = header_end + 4 + content_len;

    // ---- ④ 收齐了没有？----
    // 没齐就原地返回 —— 这一行就是"切片"：
    // 函数退出，进度留在 ctx.inbuf 里，等下一个 EPOLLIN 再进来重新算一遍。
    if (ctx.inbuf.size() < total) {
        return out;   // NeedMore
    }

    out.result = ParseResult::Ready;
    out.raw = ctx.inbuf.substr(0, total);
    out.total = total;
    return out;
}

// ===========================================================================
// 拼 HTTP 响应（只拼字符串，不发）
//
// want_close 决定 Connection 头：客户端说要关，我们也回 close。
// ===========================================================================
static std::string buildResponse(const std::string& body, bool want_close) {
    const std::string conn_header = want_close ? "Connection: close\r\n"
                                               : "Connection: keep-alive\r\n";
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/html\r\n"
           "Content-Length: " +
           std::to_string(body.size()) + "\r\n" + conn_header + "\r\n" + body;
}
