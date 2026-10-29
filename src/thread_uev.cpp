// ===========================================================================
// src/thread_uev.cpp：单 Reactor + 线程池 + 统一事件源（自包含单文件）
//
// 这个文件是什么？
//   src/epoll.cpp  —— 单 Reactor：一个线程既收连接又算业务
//   src/thread.cpp —— 单 Reactor + 线程池：主线程只碰 IO，业务丢给工人
//   本文件 = 把两边的"内容"合进一个文件，再加一样 thread.cpp 没有的东西：
//   【统一事件源】—— 信号不再用 signal handler 处理，而是变成 epoll 里的一个 fd。
//
// 为什么单开一个文件，而不是继续改 thread.cpp？
//   1. 自包含：HTTP 解析（原 http.hpp）、线程池（原 thread_pool.hpp）、
//      epoll 主循环全在同一份里 —— 从 accept 到 send 一条线读下来，不用来回跳文件。
//   2. 它多了 thread.cpp 没有的行为（signalfd + 优雅停机），
//      混进 thread.cpp 会让"36 项测试守着的那份基线"变样。
//   3. 两个文件可以对照着读：先看 thread.cpp 懂模型，再看这里看统一事件源。
//
// ===========================================================================
// 【统一事件源】到底是什么
//
// 朴素写法（很多教程）：
//     volatile sig_atomic_t g_stop = 0;
//     void onSignal(int) { g_stop = 1; }          // handler 里只置标志
//     signal(SIGINT, onSignal);
//     while (!g_stop) { epoll_wait(...); ... }
//   三个毛病：
//     · handler 里只能调 async-signal-safe 的函数（不能 printf/cout/malloc，
//       甚至不能 epoll_ctl）—— 真要"立刻处理"，能做的事几乎没有；
//     · 信号随时可能打断主线程正在做的事，标志位什么时候被看到不确定；
//     · 在 epoll_wait 里被打断只会返回 EINTR，你还得专门处理它。
//
// 统一事件源（本文件）：
//     ① sigprocmask 先把 SIGINT/SIGTERM 【屏蔽】（信号只挂起，不投递）
//     ② signalfd(-1, &mask, ...) 把"挂起的信号"变成一个可读的 fd
//     ③ 把这个 fd 注册进 epoll
//     ④ epoll_wait 返回它 → read 出 struct signalfd_siginfo → 和连接事件一样处理
//
//   好处：信号处理变成普通 IO，主循环只有一处等待（epoll_wait），
//   不用 handler、不用全局 volatile 标志、不用处理 EINTR 之外的"异步打断"。
//   书上的 socketpair/self-pipe 是同一招的前身：handler 里 write 一个字节，
//   主循环读那个管道 —— 还是得在 handler 里干活（write 是 async-signal-safe 的）。
//   signalfd 把这根管道和 handler 全省了。
//
// ⚠ 屏蔽必须发生在【创建线程之前】：线程会继承创建者的信号掩码。
//   否则 SIGTERM 可能被某个工人线程领走，主线程的 epoll_wait 永远等不到它。
//
// ===========================================================================
// 【优雅停机】为什么不是收到信号就 return 0
//
// 收到 SIGTERM 时，可能有：
//   · 工人手里正在算 /work 的任务（响应还没发）
//   · 队列里排着的任务
//   · 一堆 keep-alive 的空闲连接
// 直接 return 0 / exit() 会让前两种的响应永远发不出去，客户端看到半截或 RST。
//
// 本文件的顺序：
//   ① 信号事件里：置 g_stopping、从 epoll 摘掉并 close listen fd（不再接新连接）
//   ② 退出事件循环
//   ③ 关掉"空闲"连接（busy == false 的那些）
//   ④ 线程池 drain()：把队列排空、等在途任务全部干完（工人会把响应发完）
//   ⑤ 关 epoll/signalfd，退出码 0
//   ★ 第 ③ 步只关 busy == false 的：busy == true 的 fd 归工人管，
//     主线程碰它就是 use-after-close。
//
// ===========================================================================
// 【fd 的交接协议】（和 src/thread.cpp 一个字都不差的协议）
//   主线程：解析出完整请求 → busy=true → EPOLL_CTL_DEL → 打包任务丢进队列
//   工人  ：send → 要关就 eraseCtx+close；要留就 busy=false + EPOLL_CTL_ADD 还回去
//   ★ 工人还 fd 前必须把 inbuf 里已经到齐的请求处理干净 —— 那些字节在用户态，
//     内核不会因为它们再报 EPOLLIN（keepalive_test 的 [2] 就挂在这条上）。
//
// 【和 src/thread.cpp 的差异】（都是有意改的，不是两份代码在漂移）
//   1. 信号 → signalfd，和 listen fd / 连接 fd 一起进 epoll（本文件新增）
//   2. 优雅停机：drain 线程池 + 关空闲连接（thread.cpp 收到 SIGTERM 直接死）
//   3. /work 精确匹配路径（thread.cpp 用 rfind("/work",0)，/worker 会被当成 /work）
//   4. workUnits 按 & 切参数、参数名必须恰为 n（thread.cpp 用 find("n=")，
//      "?foo=n=5" 这种会被误判成 n=5）
//   5. send 做 EINTR 重试 + 全量校验；发失败/只发一半就收连接
//      （thread.cpp 只 perror，然后把半条响应留在连接上继续复用）
//   6. epoll_ctl ADD back 失败时收连接（thread.cpp 只 perror → fd 和账本双泄漏）
//   7. --workers / --port 严格解析（thread.cpp 用 stoul，非法值会抛异常崩在 main）
//
// 编译运行（-pthread 必须加）：
//   g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion -pthread src/thread_uev.cpp -o build/thread_uev
//   build/thread_uev                    # 4 个工人 + 每请求日志
//   build/thread_uev --workers 1        # 单工人（对照多核收益）
//   build/thread_uev --quiet            # 压测时关每请求日志
//
// 验收（和 thread.cpp 用同一套测试，一行都不用改）：
//   python3 tests/p2_test.py
//   python3 tests/balance_test.py
//   python3 tests/keepalive_test.py --bin build/thread_uev
//   python3 tests/emfile_test.py   --bin build/thread_uev
// 优雅停机手测：
//   build/thread_uev --quiet &
//   curl -s 'http://127.0.0.1:8888/work?n=20000000' &   # 在途请求
//   kill -TERM %1                                       # 服务器等它答完再退
// ===========================================================================

#include <arpa/inet.h>     // inet_ntop
#include <atomic>          // std::atomic（跨线程的停机标志）
#include <cctype>          // std::tolower
#include <cerrno>          // errno / EAGAIN / EINTR / EMFILE / ERANGE
#include <condition_variable>  // std::condition_variable
#include <csignal>         // sigprocmask / signal / SIGPIPE / SIG_IGN
#include <cstddef>         // size_t
#include <cstdint>         // uint16_t / uint32_t / uint64_t
#include <cstdio>          // perror
#include <cstdlib>         // std::strtoull
#include <cstring>         // std::strlen（本文件没直接用，保留给 strerror 类扩展）
#include <exception>       // std::exception（stoull 溢出时抛的那个）
#include <fcntl.h>         // fcntl / F_GETFL / F_SETFL / O_NONBLOCK
#include <functional>      // std::function（任务）
#include <iostream>        // std::cout / std::endl
#include <mutex>           // std::mutex / std::lock_guard / std::unique_lock
#include <netinet/in.h>    // sockaddr_in / INADDR_ANY / htons
#include <queue>           // std::queue
#include <string>          // std::string / std::to_string
#include <sys/epoll.h>     // epoll_create1 / epoll_ctl / epoll_wait
#include <sys/signalfd.h>  // signalfd / struct signalfd_siginfo
#include <sys/socket.h>    // socket / bind / listen / setsockopt / accept / send
#include <thread>          // std::thread
#include <unistd.h>        // close / read / send
#include <unordered_map>   // std::unordered_map
#include <utility>         // std::move
#include <vector>          // std::vector

// ===========================================================================
// 常量
// ===========================================================================
constexpr uint16_t PORT = 8888;        // 监听端口
constexpr size_t DEFAULT_WORKERS = 4;  // 默认工人数
constexpr size_t MAX_EVENTS = 16;      // 一次 epoll_wait 最多取几个事件

// ★ DoS 保护：请求头 / 请求体上限（和 src/p2.cpp、src/http.hpp 同一套值）
//   没有上限的实现会被"一直发数据、永远不发 CRLF CRLF"的客户端吊死。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// /work 的 n 上限 —— 业务参数是客户端给的，不设上限就是让人一个请求打满 CPU
constexpr unsigned long long MAX_WORK_UNITS = 20'000'000ULL;
constexpr unsigned long long DEFAULT_WORK_UNITS = 200'000ULL;

// ===========================================================================
// 日志：多线程下必须"整行原子"
//
// std::cout << a << b << c 是三次调用，两个线程同时写会交错成
// "新连接 fd=新连接 fd=57"。而测试里有按行读日志的（keepalive_test 找
// 'GET /first'、balance_test 找"账本里"哨兵）—— 日志一乱就变成假红。
//
// ⚠ std::endl 会 flush：日志重定向到文件时测试才能及时读到，不能换成 '\n'。
// ===========================================================================
inline std::mutex g_log_mutex;

inline void logLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << line << std::endl;
}

// 压测开关：每请求都打日志（还带 flush）会成为瓶颈。--quiet 关掉它。
inline bool g_log_requests = true;

// ===========================================================================
// 每条连接的状态
//
// "一条连接"不能只是一个 fd 号：半包/粘包要求把"已经收到哪儿了"存下来，
// 而事件处理的栈帧活不到下一次事件 —— 状态必须搬到外面的容器里。
// ===========================================================================
struct ClientContext {
    int fd = -1;

    // 这条连接自己的读缓冲区（一次 read 可能只是半个请求）
    std::string inbuf;

    // 回完【当前】请求之后要不要关？
    //   true  = 客户端在头里说了 Connection: close，或这条连接被判非法
    //   false = 留着继续服务（HTTP/1.1 默认行为）
    bool want_close = false;

    // true = 这条连接的 fd 已经 EPOLL_CTL_DEL 掉、正躺在工人手里。
    // 主线程看到 busy 就必须完全别碰它（不 read、不解析、不 close），
    // 否则就是两个线程同时操作同一个 fd（数据竞争 / use-after-close）。
    bool busy = false;
};

// ===========================================================================
// 纯函数：HTTP 解析 / 响应构造（原 src/http.hpp，逻辑一个字没改）
// ===========================================================================

// 只转 ASCII 小写。std::tolower 的参数必须是 unsigned char 的值，
// 负数进去是未定义行为，所以先 cast。
static std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 在"头部区域"[0, header_end) 内查找字段 name，找到就返回值（去掉两端空白）。
// 三个协议细节：① 字段名大小写不敏感 ② 冒号后的 OWS 可为 0 个
//              ③ 只在头部区域内找 —— 否则 body 里的同名文本会骗到解析器
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
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
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
// 为什么要它：stoull 是"解析到非数字就停"，stoull("10abc") 返回 10 ——
// 非法值被当成合法值接受（RFC 9112 §6.3 要求以 400 拒绝）。
static bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// 字符串 → unsigned long long，整串必须是数字（给命令行参数用）。
static bool parseUnsigned(const std::string& s, unsigned long long& out) {
    if (!isAllDigits(s)) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0') return false;
    out = v;
    return true;
}

// 解析：从 ctx.inbuf 里取出【一个完整请求】。
//   NeedMore —— 还不够一个完整请求，等下次事件
//   Ready    —— out.raw / out.total 有效
//   Rejected —— 请求非法；ctx.want_close 已置 true，调用方直接收连接
enum class ParseResult { NeedMore, Ready, Rejected };

struct ParsedRequest {
    ParseResult result = ParseResult::NeedMore;
    std::string raw;     // Ready 时有效：完整请求文本（头部 + body）
    size_t total = 0;    // Ready 时有效：这个请求一共占多少字节
};

static ParsedRequest parseRequest(ClientContext& ctx) {
    ParsedRequest out;

    // ---- ① 头部定界：找 CRLF CRLF ----
    // 注意判断顺序：先看"收全没有"，再看"超限"。
    // 反过来会把"头很小但 body 很大"的合法请求误杀。
    const size_t header_end = ctx.inbuf.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        if (ctx.inbuf.size() > MAX_HEADER_SIZE) {
            logLine("请求头过大 —— 丢弃 fd=" + std::to_string(ctx.fd));
            ctx.want_close = true;
            out.result = ParseResult::Rejected;
            return out;
        }
        return out;   // NeedMore：等下次事件
    }

    // ---- ② 解析 Content-Length ----
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
                logLine("body 过大（Content-Length=" + std::to_string(v) + "）—— 丢弃 fd=" +
                        std::to_string(ctx.fd));
                ctx.want_close = true;
                out.result = ParseResult::Rejected;
                return out;
            }
            content_len = static_cast<size_t>(v);
        } catch (const std::exception&) {
            // 只可能是数字长得离谱（stoull 溢出）
            logLine("Content-Length 溢出 —— 丢弃 fd=" + std::to_string(ctx.fd));
            ctx.want_close = true;
            out.result = ParseResult::Rejected;
            return out;
        }
    }

    // ---- ③ 客户端要不要关连接？----
    // 必须在拼响应之前问出来，因为响应里的 Connection 头要跟着它走。
    std::string conn_value;
    if (findHeader(ctx.inbuf, header_end, "Connection", conn_value) &&
        toLowerAscii(conn_value) == "close") {
        ctx.want_close = true;
    }

    // ---- ④ 一个完整请求 = 头部 + 结尾空行 + body ----
    // header_end 指向 CRLFCRLF 的第一个 \r，头部内容本身不含这 4 个字节。
    const size_t total = header_end + 4 + content_len;

    // ---- ⑤ 收齐了没有？没齐就原地返回，进度留在 ctx.inbuf 里 ----
    if (ctx.inbuf.size() < total) {
        return out;   // NeedMore
    }

    out.result = ParseResult::Ready;
    out.raw = ctx.inbuf.substr(0, total);
    out.total = total;
    return out;
}

// 拼 HTTP 响应（只拼字符串，不发）。want_close 决定 Connection 头。
static std::string buildResponse(const std::string& body, bool want_close) {
    const std::string conn_header = want_close ? "Connection: close\r\n"
                                               : "Connection: keep-alive\r\n";
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/html\r\n"
           "Content-Length: " +
           std::to_string(body.size()) + "\r\n" + conn_header + "\r\n" + body;
}

// ===========================================================================
// 线程池（原 src/thread_pool.hpp，加了一个 drain()）
//
// 三个成员各管一件事：
//   tasks_ —— 待办清单
//   mtx_   —— 清单的锁
//   cv_    —— 门铃（"来活了" / "下班了"）
//
// 和原版唯一的区别：析构前可以显式 drain()，把"排空队列"这一步
// 从析构函数里拿出来，方便在 main 里先 log 一句"开始等在途任务"。
// ===========================================================================
class ThreadPool {
public:
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
        logLine("线程池启动，工人数 = " + std::to_string(n));
    }

    ~ThreadPool() {
        drain();   // 幂等：drain 过就直接返回
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务：调用方（主线程）丢进来就走，不等它做完。
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.push(std::move(task));
        }
        // 解锁之后才按门铃（否则被唤醒的工人会卡在锁上）
        cv_.notify_one();
    }

    // 排空队列 + 等所有工人收工。
    // stop_ 置 true 之后，工人会【先把剩下的活干完】再退休
    //（见 workerLoop 里 if (stop_ && tasks_.empty()) return;）。
    void drain() {
        if (joined_) return;   // 析构还会再调一次，别 join 两次
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }
        joined_ = true;
        logLine("线程池已排空并关闭");
    }

    size_t workerCount() const { return workers_.size(); }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                // unique_lock 才能中途解锁；condition_variable::wait 需要这个能力
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                // 收到下班通知，而且清单空了 → 退休
                if (stop_ && tasks_.empty()) return;

                task = std::move(tasks_.front());
                tasks_.pop();
            }   // ← 干活时不该占着锁，否则池子退化成单线程

            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool joined_ = false;
};

// ===========================================================================
// 跨线程共享的东西
//
// 这个文件里【唯一】需要保护的是 g_clients 这张表：
//   · 主线程会 emplace（新连接）/ erase（连接结束）/ 读 inbuf
//   · 工人要改 ctx.busy，并在还 fd 前处理 inbuf 里排队的请求
// 读/写 inbuf 不用额外锁：fd 交出去之前已经 EPOLL_CTL_DEL 了，
// 主线程不会再收到这条连接的事件 —— 分工是清楚的。
// ===========================================================================
static ThreadPool* g_pool = nullptr;
static int g_epoll_fd = -1;
static int g_signal_fd = -1;
static std::mutex g_clients_mutex;
static std::unordered_map<int, ClientContext> g_clients;

// 停机标志：信号事件里置 true；工人还 fd 前要看它 ——
// 停机中就不再把 fd 还回 epoll，直接收掉（否则主循环已经退出，
// 还回去的 fd 会一直挂着，还白打一条 epoll_ctl 失败的日志）。
static std::atomic<bool> g_stopping{false};

// ===========================================================================
// 业务：真的吃 CPU
//
// xorshift64 —— 每一步都用上一步的结果，编译器没法把它优化掉。
// 如果写成"空循环 n 次"，-O2 会直接把整个循环删掉，测出来的就是假的。
// ===========================================================================
static uint64_t burnCpu(unsigned long long n) {
    uint64_t x = 0x9e3779b97f4a7c15ULL;   // 随便一个非零种子
    for (unsigned long long i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x += i;
    }
    return x;
}

// 从 "GET /work?n=100 HTTP/1.1 CRLF..." 里取出请求目标（第二段）
static std::string requestTarget(const std::string& raw) {
    const size_t sp1 = raw.find(' ');
    if (sp1 == std::string::npos) return "/";
    const size_t sp2 = raw.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return "/";
    return raw.substr(sp1 + 1, sp2 - sp1 - 1);
}

// 只取路径部分（问号之前）。为什么：/work 匹配必须是【精确路径】，
// rfind("/work", 0) 会把 /worker、/workshop 也当成 /work（白送一个 DoS 面）。
static std::string pathOf(const std::string& target) {
    const size_t q = target.find('?');
    return q == std::string::npos ? target : target.substr(0, q);
}

// 从 "/work?n=100" 里取 n；没有就用默认值，超上限截到上限。
// 按 & 切参数、参数名必须恰为 "n" —— 不能全文 find("n=")：
// "?foo=n=5" 里也有 "n="，会被误判成 n=5。
static unsigned long long workUnits(const std::string& target) {
    const size_t q = target.find('?');
    if (q == std::string::npos) return DEFAULT_WORK_UNITS;

    size_t pos = q + 1;
    while (pos <= target.size()) {
        size_t amp = target.find('&', pos);
        if (amp == std::string::npos) amp = target.size();

        const std::string kv = target.substr(pos, amp - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == "n") {
            const std::string value = kv.substr(eq + 1);
            if (!isAllDigits(value)) return DEFAULT_WORK_UNITS;   // "n=" 后面不是数字
            try {
                const unsigned long long v = std::stoull(value);
                return v > MAX_WORK_UNITS ? MAX_WORK_UNITS : v;
            } catch (const std::exception&) {
                return MAX_WORK_UNITS;   // 数字长得离谱 → 按上限算
            }
        }

        if (amp == target.size()) break;
        pos = amp + 1;
    }
    return DEFAULT_WORK_UNITS;
}

// 根据请求算出响应体 —— 工人线程执行的就是它
static std::string doBusiness(const std::string& raw) {
    const std::string target = requestTarget(raw);

    if (pathOf(target) != "/work") {
        // 普通请求：和 src/epoll.cpp 返回完全一样的东西
        // （这样 p2_test.py 那 23 项协议测试对三个服务器都成立）
        return "<h1>Hello from my own server!</h1>";
    }

    const unsigned long long n = workUnits(target);
    const uint64_t sum = burnCpu(n);
    return "<h1>CPU work done</h1><p>units=" + std::to_string(n) +
           "</p><p>checksum=" + std::to_string(sum) + "</p>";
}

// ===========================================================================
// 账本操作（都要拿锁）
// ===========================================================================
static bool ctxBusy(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    const auto it = g_clients.find(fd);
    return it != g_clients.end() && it->second.busy;
}

// 返回的指针只在"下一次往表里插入/删除之前"有效。
// 主线程处理单个事件期间不会插入/删除别的连接，所以够用。
static ClientContext* findCtx(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    const auto it = g_clients.find(fd);
    return it == g_clients.end() ? nullptr : &it->second;
}

static void eraseCtx(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    if (g_clients.erase(fd) == 0) {
        logLine("账本里没有 fd=" + std::to_string(fd) + " —— 有地方漏销账");
    }
}

// ===========================================================================
// ⚠⚠ 本文件最要命的一条规矩：收连接时必须【先 eraseCtx，再 close】
//
// 反过来写会怎样（哨兵会先喊出来，而且喊的是冤枉话）：
//   工人： close(fd=5)          ← 号码 5 立刻可以被内核复用
//   主线程：accept 拿到 fd=5 → g_clients.emplace(5) 失败
//           → 哨兵喊"账本里已经有 fd=5" ← 冤枉！
//   工人： eraseCtx(5)          ← 把【新连接】的账销掉了
//   结果：新连接不在账本里 → 主线程 findCtx 返回 nullptr → 那条连接再也没人管，
//   fd 一直泄漏。
//
// 一句话：fd 是会被复用的号码，所以"销账"必须发生在"归还号码"之前。
// ===========================================================================
static void dropClient(int fd) {
    eraseCtx(fd);   // ← 先销账
    close(fd);      // ← 再归还号码
}

// ===========================================================================
// 工人：发响应
//
// ⚠ 没有输出缓冲 + EPOLLOUT，所以"发不完"不能等 —— 只能判定这条连接坏了。
//   小响应（几十字节）不会撞到，但代码不能假设 send 一定一次发完：
//   非阻塞 fd 上 send 可能返回部分写入（EAGAIN），剩下的字节必须有人保管，
//   而这里没有保管的地方（thread.cpp 的已知妥协，见 notes/BRANCH 讨论）。
//   所以：要么一次发完，要么收掉连接 —— 绝不留半条响应在连接上继续复用。
// ===========================================================================
static bool sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;   // 被信号打断：重试
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            logLine("响应只发了一半（无输出缓冲）—— 只能收掉 fd=" + std::to_string(fd));
            return false;
        }
        return false;   // EPIPE / ECONNRESET …
    }
    return true;
}

// 工人处理【一个】已经解析出来的请求。返回 false = 这条连接已经收掉了。
static bool workerHandleOne(int fd, const std::string& raw, bool want_close) {
    const std::string body = doBusiness(raw);
    const std::string response = buildResponse(body, want_close);

    if (!sendAll(fd, response)) {
        dropClient(fd);
        return false;
    }

    if (want_close) {
        dropClient(fd);   // 客户端要求关（或这条连接被判非法）→ 两本账一起销
        return false;
    }
    return true;
}

// ===========================================================================
// 【工人线程】算业务 → 发响应 → 把 fd 还回去（或收掉）
//
// 参数全是按值捕获：fd / 请求文本 / 要不要关。
// 不能传 ClientContext 的引用 —— 那条连接的上下文归主线程管，
// 工人只在需要碰 inbuf / busy 时短暂进去改一下（而且要拿锁）。
//
// ★ 中间那个 while 是踩出来的：
//   客户端可能一次发了两个请求（粘包）。主线程只解析出第一个就交出来了，
//   第二个还在 ctx.inbuf 里 —— 那是用户态缓冲区，不在内核里。
//   所以就算把 fd 还回去，epoll 也不会报 EPOLLIN，第二个请求永远没人管。
//   → 工人必须自己把 inbuf 里排队的请求处理干净，再还 fd。
// ===========================================================================
static void handleInPool(int fd, std::string raw, bool want_close) {
    if (!workerHandleOne(fd, raw, want_close)) {
        return;   // 已经关了
    }

    // ---- 把 inbuf 里已经到齐的请求接着处理完 ----
    while (true) {
        std::string next_raw;
        bool next_close = false;
        bool bad = false;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            const auto it = g_clients.find(fd);
            if (it == g_clients.end()) return;   // 主线程先收掉了

            const ParsedRequest pr = parseRequest(it->second);
            if (pr.result == ParseResult::NeedMore) break;
            if (pr.result == ParseResult::Rejected) {
                bad = true;
            } else {
                next_raw = pr.raw;
                next_close = it->second.want_close;
                it->second.inbuf.erase(0, pr.total);
            }
        }
        if (bad) {
            dropClient(fd);
            return;
        }
        if (!workerHandleOne(fd, next_raw, next_close)) return;
    }

    // ---- 把 fd 还回去，让主线程继续照看这条连接（keep-alive 靠这一步）----
    // 顺序讲究：先清 busy，再 EPOLL_CTL_ADD —— 两步必须在同一个临界区里，
    // 否则主线程的停机扫描可能看到 busy==false 就把 fd 关了，工人随后
    // 对着一个已经关闭（甚至已被复用的）号码做 ADD。
    //
    // 停机中（g_stopping）则不再还回去：主循环已经/即将退出，还回去没人管。
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        const auto it = g_clients.find(fd);
        if (it == g_clients.end()) return;

        if (g_stopping.load()) {
            g_clients.erase(it);
            close(fd);   // 响应已经发完，可以安全关
            return;
        }

        it->second.busy = false;

        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            const int saved = errno;
            // ADD 失败 = 这条连接以后不会再有事件 → 没人读、没人关。
            // 只 perror 就会 fd + 账本双泄漏，所以必须收掉。
            g_clients.erase(fd);
            close(fd);
            logLine("epoll_ctl ADD back 失败 fd=" + std::to_string(fd) +
                    " errno=" + std::to_string(saved) + " —— 已收掉");
        }
    }
}

// ===========================================================================
// 【主线程】把一个完整请求交给线程池
// ===========================================================================
static void handOffToPool(ClientContext& ctx, size_t total) {
    const std::string raw = ctx.inbuf.substr(0, total);
    const bool want_close = ctx.want_close;

    if (g_log_requests) {
        logLine("收到完整请求（交给工人）:\n" + raw);
    }

    // ① 标记 + 消费字节（inbuf 归主线程管，趁现在处理干净）
    ctx.busy = true;
    ctx.inbuf.erase(0, total);

    // ② 从 epoll 摘掉 —— 不摘的话 LT 会继续通知主线程，
    //    两边同时操作同一个 fd（数据竞争 / use-after-close）
    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, ctx.fd, nullptr);

    // ③ 丢进队列（按值捕获，别传引用）
    const int fd = ctx.fd;
    g_pool->submit([fd, raw, want_close] { handleInPool(fd, raw, want_close); });
}

// ===========================================================================
// 统一事件源：屏蔽信号 → signalfd
// ===========================================================================
static int setupSignalFd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    // ① 先屏蔽（此时信号只会挂起，不会投递、不会打断任何线程）
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        perror("sigprocmask");
        return -1;
    }

    // ② 把"挂起的信号"变成一个可读 fd（非阻塞，读不到就是 EAGAIN）
    const int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) {
        perror("signalfd");
    }
    return fd;
}

// 处理一次信号事件：读光 signalfd 里的 siginfo，然后停止接收新连接。
static void handleSignalEvent(int& server_fd) {
    struct signalfd_siginfo si{};
    while (true) {
        const ssize_t n = read(g_signal_fd, &si, sizeof(si));
        if (n == static_cast<ssize_t>(sizeof(si))) {
            logLine("收到信号 " + std::to_string(si.ssi_signo) +
                    " —— 停止接收新连接，等待在途请求完成");
            g_stopping.store(true);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0) perror("read signalfd");
        break;
    }

    // 先从 epoll 摘，再 close —— 和连接销账同一个道理（号码会被复用）。
    // 如果之前因为 EMFILE 暂停接收，server_fd 可能已经不在 epoll 里：
    // DEL 返回 ENOENT 是预期，不用管。
    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, server_fd, nullptr);
    close(server_fd);
    server_fd = -1;   // 让 main 知道已经关了，别再关一次
}

// ===========================================================================
// 停机第二步：关掉所有"空闲"连接（busy == false）
//
// busy == true 的 fd 正躺在工人手里，主线程碰它是 use-after-close；
// 那些会由工人在发完响应后自己收掉（见 handleInPool 的 g_stopping 分支）。
// ===========================================================================
static void closeIdleConnections() {
    size_t closed = 0;
    size_t busy = 0;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        for (auto it = g_clients.begin(); it != g_clients.end();) {
            if (it->second.busy) {
                ++busy;
                ++it;
            } else {
                close(it->first);
                it = g_clients.erase(it);
                ++closed;
            }
        }
    }
    logLine("优雅停机：关掉空闲连接 " + std::to_string(closed) +
            " 条，在途任务 " + std::to_string(busy) + " 个");
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    size_t workers = DEFAULT_WORKERS;
    uint16_t port = PORT;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--workers" && i + 1 < argc) {
            unsigned long long v = 0;
            if (!parseUnsigned(argv[++i], v) || v < 1 || v > 1024) {
                std::cout << "--workers 必须是 1..1024 的整数\n";
                return 2;
            }
            workers = static_cast<size_t>(v);
        } else if (a == "--port" && i + 1 < argc) {
            unsigned long long v = 0;
            if (!parseUnsigned(argv[++i], v) || v < 1 || v > 65535) {
                std::cout << "--port 必须是 1..65535 的整数\n";
                return 2;
            }
            port = static_cast<uint16_t>(v);
        } else if (a == "--quiet") {
            g_log_requests = false;
        } else {
            std::cout << "用法: " << argv[0] << " [--workers N] [--port N] [--quiet]\n";
            return (a == "--help" || a == "-h") ? 0 : 2;
        }
    }

    // SIGPIPE：对端已关时 send 会触发它，默认动作是杀进程。
    // 这里忽略 + send 用 MSG_NOSIGNAL 双保险（有些环境会清掉 SIG_IGN）。
    signal(SIGPIPE, SIG_IGN);

    // ⚠ 必须在建线程之前屏蔽信号（线程继承掩码）；
    //   否则 SIGTERM 可能被某个工人线程领走，主线程等不到。
    g_signal_fd = setupSignalFd();
    if (g_signal_fd < 0) return 1;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        perror("setsockopt"); return 1;
    }

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, SOMAXCONN) < 0) {   // backlog 拉满（和 epoll.cpp 同一个教训）
        perror("listen"); return 1;
    }

    // listen fd 设非阻塞：accept 要循环取到 EAGAIN 为止（一次事件可能有多条新连接）
    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1 || fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl"); return 1;
    }

    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) { perror("epoll_create1"); return 1; }

    // ---- 统一事件源：把 listen fd 和 signal fd 都注册进同一个 epoll ----
    struct epoll_event sev{};
    sev.events = EPOLLIN;
    sev.data.fd = server_fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, server_fd, &sev) < 0) {
        perror("epoll_ctl: server_fd"); return 1;
    }

    struct epoll_event sig_ev{};
    sig_ev.events = EPOLLIN;
    sig_ev.data.fd = g_signal_fd;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_signal_fd, &sig_ev) < 0) {
        perror("epoll_ctl: signal_fd"); return 1;
    }

    ThreadPool pool(workers);
    g_pool = &pool;

    logLine("thread_uev ready: workers=" + std::to_string(workers) + ", port=" +
            std::to_string(port) + "（单 Reactor + 线程池 + 统一事件源）");

    struct epoll_event events[MAX_EVENTS];
    bool accept_paused = false;   // EMFILE 时暂时不再 accept
    size_t paused_at = 0;
    bool exit_loop = false;

    while (!exit_loop) {
        // EMFILE 撞墙后的恢复判断（和 src/epoll.cpp 同一套）：
        // 连接数掉回去了，才把 listen fd 重新注册回去。
        if (accept_paused) {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            if (g_clients.size() < paused_at) {
                if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, server_fd, &sev) == 0) {
                    accept_paused = false;
                    logLine("fd 腾出来了 —— 恢复接收新连接");
                }
            }
        }

        const int nfds = epoll_wait(g_epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;   // 理论上被屏蔽后不会再出现，防御
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            const int fd = events[i].data.fd;

            if (fd == g_signal_fd) {
                // ---------- 统一事件源：信号到了 ----------
                handleSignalEvent(server_fd);
                exit_loop = true;
                break;
            }

            if (fd == server_fd) {
                // ---------- 总机：新连接 ----------
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    const int conn_fd =
                        accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EMFILE) {
                            logLine("fd 用光了 —— 暂停接收，等连接释放");
                            epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, server_fd, &sev);
                            accept_paused = true;
                            {
                                std::lock_guard<std::mutex> lock(g_clients_mutex);
                                paused_at = g_clients.size();
                            }
                            break;
                        }
                        perror("accept");
                        break;
                    }

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    if (g_log_requests) {
                        logLine("新连接 fd=" + std::to_string(conn_fd) + " 来自 " + ip +
                                ":" + std::to_string(ntohs(client_addr.sin_port)));
                    }

                    // 连接也设非阻塞：read 循环靠 EAGAIN 退出
                    const int cflags = fcntl(conn_fd, F_GETFL, 0);
                    if (cflags == -1 ||
                        fcntl(conn_fd, F_SETFL, cflags | O_NONBLOCK) == -1) {
                        perror("fcntl conn");
                        close(conn_fd);
                        continue;
                    }

                    struct epoll_event cev{};
                    cev.events = EPOLLIN;
                    cev.data.fd = conn_fd;
                    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, conn_fd, &cev) < 0) {
                        perror("epoll_ctl: conn_fd");
                        close(conn_fd);   // 还没进账本，直接关
                        continue;
                    }

                    ClientContext ctx;
                    ctx.fd = conn_fd;
                    std::lock_guard<std::mutex> lock(g_clients_mutex);
                    if (!g_clients.emplace(conn_fd, ctx).second) {
                        logLine("账本里已经有 fd=" + std::to_string(conn_fd) +
                                " —— 上一个连接漏了 erase");
                    }
                }
                continue;
            }

            // ---------- 分机：有数据 / 对端关了 ----------

            // 这条连接正在工人手里 → 主线程不许碰（也别 read）
            if (ctxBusy(fd)) continue;

            ClientContext* ctx = findCtx(fd);
            if (ctx == nullptr) continue;   // 账本里没这条，不该发生

            char buf[1024];
            bool gone = false;
            while (true) {
                const ssize_t n = read(fd, buf, sizeof(buf));

                if (n > 0) {
                    ctx->inbuf.append(buf, static_cast<size_t>(n));

                    // 一个连接可能一次到好几个请求 → 循环解析，
                    // 但每交出去一个就得停：这条连接已经归工人了，别再动它。
                    while (true) {
                        const ParsedRequest pr = parseRequest(*ctx);
                        if (pr.result == ParseResult::NeedMore) break;
                        if (pr.result == ParseResult::Rejected) {
                            logLine("请求非法 —— 丢弃 fd=" + std::to_string(fd));
                            dropClient(fd);
                            gone = true;
                            break;
                        }
                        handOffToPool(*ctx, pr.total);
                        gone = true;
                        break;
                    }
                    if (gone) break;
                } else if (n == 0) {
                    dropClient(fd);   // 对端关了
                    gone = true;
                    break;
                } else {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    perror("read");
                    dropClient(fd);
                    gone = true;
                    break;
                }
            }
        }
    }

    // =======================================================================
    // 优雅停机
    // =======================================================================
    g_stopping.store(true);

    // 信号路径里已经把 listen fd 关了（server_fd 置 -1）；
    // 如果是 epoll_wait 出错退出来的，这里补一刀。
    if (server_fd >= 0) close(server_fd);   // 即使不是信号退出（epoll_wait 出错）也置上

    // ① 关空闲连接（busy 的留给工人）
    closeIdleConnections();

    // ② 排空线程池：队列里的活会继续执行，工人把响应发完才退休
    pool.drain();

    // ③ 收掉最后两本账
    close(g_signal_fd);
    close(g_epoll_fd);

    logLine("已优雅退出");
    return 0;
}
