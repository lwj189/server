#include <iostream>        // std::cout / std::endl
#include <cstdint>         // uint16_t
#include <cstddef>         // size_t
#include <string>          // std::string（CP6c-2：inbuf / 请求文本）
#include <cctype>          // std::tolower
#include <exception>       // std::exception（stoull 溢出时抛的那个）
#include <cerrno>          // errno / EAGAIN / EWOULDBLOCK / EINTR
#include <csignal>         // signal / SIGPIPE / SIG_IGN
#include <sys/socket.h>    // socket / bind / listen / setsockopt / accept / sockaddr
#include <netinet/in.h>    // sockaddr_in / INADDR_ANY / htons
#include <cstdio>          // perror
#include <unistd.h>        // close
#include <arpa/inet.h>     // inet_ntop
#include <fcntl.h>         // fcntl / F_GETFL / F_SETFL / O_NONBLOCK
#include <sys/epoll.h>     // epoll_create1 / epoll_ctl / epoll_wait / struct epoll_event
#include <unordered_map>   // std::unordered_map（CP6c：fd → 连接状态）

constexpr uint16_t PORT = 8888;   // 监听端口

// ★【DoS 保护】请求头 / 请求体大小上限（和 p2.cpp 同一套值）
//   没有上限的实现会被"一直发数据、永远不发 \r\n\r\n"的客户端吊死（内存涨爆）。
//   MAX_HEADER_SIZE = 8 KB 与 nginx 的 large_client_header_buffers 同量级；
//   MAX_BODY_SIZE = 1 MB 与 nginx 的 client_max_body_size 默认值一致。
constexpr size_t MAX_HEADER_SIZE = 8 * 1024;
constexpr size_t MAX_BODY_SIZE = 1024 * 1024;

// ===========================================================================
// CP6c：每条连接的状态
//
// 为什么需要它？
//   CP6a / CP6b 里"一条连接"就只是一个 fd 号 —— 连接相关的所有东西都活在
//   事件处理的【栈帧】里，事件一处理完就烟消云散。
//   一旦要处理半包 / 粘包（CP5 那套），就必须把"这条连接已经收到哪儿了"存下来，
//   而栈帧留不住它 → 状态必须从栈上搬到外面的容器里。
//
//   这就是"状态机切片"：把"一个连续执行的函数"改造成"每次事件恢复一点进度"，
//   所以"进度"得有个地方放。
// ===========================================================================
struct ClientContext {
    int fd = -1;

    // ---- CP6c-2 新增：这条连接自己的读缓冲区 ----
    // 为什么必须是"每条连接一个"：TCP 是字节流，一次 read 拿到的可能只是半个请求。
    // 半个请求得先存着，等剩下的到了再拼起来 —— 而"等"意味着函数要返回，
    // 栈上的局部变量活不到下一次事件，所以只能存在这里。
    // （p2.cpp 里它是 main 循环里的一个局部 std::string，一次只服务一条连接，
    //   所以放栈上够用；epoll 版同时持有很多条，就必须一人一个。）
    std::string inbuf;

    // ---- CP6d 新增：这条连接【回完当前请求之后】要不要关？----
    // 谁说了算：客户端在请求头里说（去看 tests/p2_test.py 第 45 行，它带了什么字段）。
    // 出错的连接也必须关（头过大 / Content-Length 非法 …），也是靠这个成员。
    //     true  = 该关了（客户端要求，或这条连接被判为非法）
    //     false = 留着，继续服务后面的请求（HTTP/1.1 的默认行为）
    //
    bool want_close = false;
};

// ===========================================================================
// CP6c-2：下面三个纯函数是从 p2.cpp 原样搬过来的（CP5 的成果，不用重写）
//
// 它们不依赖 socket、不依赖 epoll —— 输入一段字符串，输出解析结果。
// 正因为是纯函数，"搬家"才这么省事：换个文件照样能用。
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
// CP6c-2 的心脏：从 ctx.inbuf 里取出【一个完整请求】并响应
//
// 对比 p2.cpp 的同款逻辑 —— 这是"状态机切片"最直观的一处：
//   p2.cpp：一个 while 循环堵在那里读到收全为止（连续执行，栈上有 read_buffer）
//   这里  ：每次事件调用一次，不够就【原地返回】，进度留在 ctx.inbuf 里
//
// 返回值：true  = 已经有结果了（响应发了 / 或这条连接该被丢弃）→ 调用方收连接
//         false = 还不够一个完整请求 → 等下次 EPOLLIN 再进来接着算
// ===========================================================================
static bool tryHandleRequest(ClientContext& ctx) {
    // ---- ① 头部定界：找 \r\n\r\n（RFC 9112 §2.1）----
    const size_t header_end = ctx.inbuf.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        // 头部还没收全。但别无限等 —— 对方可能永远不发定界符（DoS）。
        // ⚠ 判断顺序和 p2.cpp 一致：先看"收全没有"，再看"超限"。
        //   反过来会把"头很小但 body 很大"的合法请求误杀。
        if (ctx.inbuf.size() > MAX_HEADER_SIZE) {
            std::cout << "请求头过大 —— 丢弃 fd=" << ctx.fd << std::endl;
            ctx.want_close = true;   // 判死刑的连接一律要关（下面 3 处错误分支同理）
            return true;
        }
        return false;   // 还不够，等下次事件
    }

    // ---- ② 解析 Content-Length（CP5 的三个坑由上面两个纯函数解决）----
    size_t content_len = 0;   // 没有这个头字段就按 0 处理（普通 GET 就是这种）
    std::string cl_value;
    if (findHeader(ctx.inbuf, header_end, "Content-Length", cl_value)) {
        if (!isAllDigits(cl_value)) {
            std::cout << "Content-Length 非法（不是纯数字）—— 丢弃 fd=" << ctx.fd << std::endl;
            ctx.want_close = true;
            return true;
        }
        try {
            const unsigned long long v = std::stoull(cl_value);
            if (v > MAX_BODY_SIZE) {
                std::cout << "body 过大（Content-Length=" << v << "）—— 丢弃 fd=" << ctx.fd
                          << std::endl;
                ctx.want_close = true;
                return true;
            }
            content_len = static_cast<size_t>(v);
        } catch (const std::exception&) {
            std::cout << "Content-Length 溢出 —— 丢弃 fd=" << ctx.fd << std::endl;
            ctx.want_close = true;
            return true;
        }
    }

    // ---- 客户端要不要关连接？（HTTP/1.1 默认复用，客户端说 close 才关）----
    // 位置讲究：必须在【拼响应之前】问出来，因为响应里的 Connection 头要跟着它走。
    //
    // ⚠ 和上面找 Content-Length 只有【前半截】像（都是 findHeader 取值），后半截完全不同：
    //   那个要"把数字串转成整数"（isAllDigits / stoull / MAX_BODY_SIZE），
    //   这个只要"比字符串"。照抄形状可以，抄进来的每一行都得知道在干嘛。
    //
    // 值可能是 "close" / "keep-alive" / "Close"… → 先 toLowerAscii 再比（RFC 9110 §5.1 大小写不敏感）
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
        return false;   // 还不够，等下次事件
    }

    // ---- ⑤ 收齐了：处理 + 响应（这段从 p2.cpp 原样搬过来）----
    const std::string request = ctx.inbuf.substr(0, total);
    std::cout << "收到完整请求:\n"
              << request << std::endl;

    const std::string body = "<h1>Hello from my own server!</h1>";
    // 响应里的 Connection 头跟着 ctx.want_close 走 —— 这一处我写好了，不用你填
    const std::string conn_header = ctx.want_close ? "Connection: close\r\n"
                                                   : "Connection: keep-alive\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/html\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\n" +
                                 conn_header +
                                 "\r\n" +
                                 body;

    if (send(ctx.fd, response.c_str(), response.size(), 0) < 0) {
        perror("send");
    }

    // ---- 把【已经处理掉】的字节从 inbuf 里删掉（消费）----
    // 不删会怎样：下次事件进来 find("\r\n\r\n") 又会找到【同一个请求】→ 又回一遍，
    //             排在后面的请求永远轮不到。（判据：keepalive_test 的 [2]）
    // 删完剩下的才是"还没处理的字节"，下一个请求就排在它前面。
    ctx.inbuf.erase(0, total);

    return true;   // 我处理掉了一个请求（后面还有没有，调用方接着问）
}

int main() {
    // 忽略 SIGPIPE：客户端提前断开时 send 不会杀掉整个进程，而是返回 -1 (EPIPE)
    signal(SIGPIPE, SIG_IGN);

    // ---- 步骤 1：创建 socket ----
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
    address.sin_family = AF_INET;           // IPv4 地址族
    address.sin_addr.s_addr = INADDR_ANY;   // 0.0.0.0：监听本机所有网卡
    address.sin_port = htons(PORT);         // 端口必须转成网络字节序

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

    // ---- 步骤 6：把 server_fd 设为非阻塞（读-改-写）----
    // 之后 accept 在没有新连接时立刻返回 -1 + EAGAIN，而不是挂起线程
    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return 1;
    }
    if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return 1;
    }

    // ---- 步骤 7：创建 epoll 实例（它也返回一个 fd）----
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    // ---- 步骤 8：把 server_fd 登记进 epoll ----
    // ev.data 是"你写给内核的标记"，epoll_wait 事件回来时原样带回，
    // 用来分辨"这个事件属于哪个 fd"（总机还是分机）。
    struct epoll_event ev{};
    ev.events = EPOLLIN;   // 关心"可读"：对监听 socket 就是"有新连接排队"
    ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl: server_fd");
        return 1;
    }   // ← 这个花括号必须在这！少一个就会把下面整段关进 if 的错误分支（死代码）

    std::cout << "epoll ready, listening on port " << PORT << " ..." << std::endl;

    // ---- CP6c：连接账本 ----
    // fd → 该连接的状态。这是"现在到底有哪些连接活着"的唯一真相来源。
    //
    // ⚠ 从这里开始有【两本账】要配平：
    //     · fd 号     —— 靠 close() 销账
    //     · 账本条目  —— 靠 erase() 销账
    //   只 close 不 erase = 内存泄漏；只 erase 不 close = fd 泄漏。两本都得平。
    //   这就是 HANDOFF §8.3 说的"第二阶段：close / erase 配对"。
    std::unordered_map<int, ClientContext> clients;

    // ---- CP6d：EMFILE 暂停状态 ----
    // 为什么要它：accept 撞到 EMFILE（fd 用光）后如果只是 break，
    // LT 模式下监听 fd 仍然"可读" → epoll_wait 立刻又返回 → 死循环烧 CPU。
    // 实测：3 秒刷了 161 万行 "accept: Too many open files"，CPU 100%。
    // 办法：暂停时把 server_fd 从 epoll 里【摘掉】；等有连接关闭、fd 腾出来，再【加回去】。
    bool accept_paused = false;
    size_t paused_at = 0;   // 暂停那一刻账本里有几条（后面拿它判断"有没有腾出 fd"）

    // ---- 步骤 9：事件循环（Reactor 的心脏）----
    constexpr int MAX_EVENTS = 16;
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        // ---- CP6d：先看看能不能恢复接收 ----
        // 判据：暂停时账本有 paused_at 条，现在少了 → 至少腾出了一个 fd。
        //
        // ⚠ 位置：必须在这个 while 循环【里面】。
        //   实测（造变体验的）：放循环外 → 这段永远不执行 → 摘了话筒再也放不回去 → [2] 报"连不上"。
        //   放这里 vs 放循环体【末尾】：实测【等价】，两个都 2/2。
        //   （原来这行注释写的是"放末尾会 暂停→恢复→再暂停 又变死循环" —— 那是个
        //    没验证过的猜测，已被实测推翻。留在这里只是因为语义更清楚：）
        // ★ "能不能恢复"是【睡前】要问的问题 —— 先问，再睡。
        if (accept_paused && clients.size() < paused_at) {
            // 加回去。用的还是当初登记 server_fd 时那个 event（它还在作用域里）
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == 0) {
                accept_paused = false;
                std::cout << "fd 腾出来了 —— 恢复接收新连接" << std::endl;
            }
        }

        // 返回值三态：>0 就绪的 fd 个数 / 0 超时（不是错误）/ -1 出错
        // timeout = -1：没有事件就一直睡（实测 0% CPU，wchan=ep_poll）
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;   // 被信号打断：重试，不是错误
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            // 分流：data.fd 是登记时写进去的标记，现在有两种可能 ——
            //   == server_fd → 总机：有新连接在排队
            //   != server_fd → 分机：这条连接上有数据到达，或者对端关闭了
            if (events[i].data.fd == server_fd) {

                // 一次事件可能对应队列里【多个】刚到达的连接，所以循环接到 EAGAIN
                // （ET 模式下不循环会丢连接；LT 模式下是效率问题）
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 接完了
                        if (errno == EMFILE) {   // 本【进程】的 fd 用光了（ENFILE 才是整机）
                            std::cerr << "fd 用光了 —— 暂停接收，等连接释放" << std::endl;
                            // 把它从 epoll 摘掉，别再收它的事件 —— 摘了才不会空转。
                            // （EPOLL_CTL_DEL 的 event 参数会被内核忽略，传什么都不会读）
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, &ev);
                            accept_paused = true;
                            paused_at = clients.size();   // 记下暂停时的账本大小（恢复判据比它小）
                            break;
                        }
                        perror("accept");
                        break;
                    }

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    std::cout << "新连接 fd=" << conn_fd << " 来自 " << ip << ":"
                              << ntohs(client_addr.sin_port) << std::endl;

                    // ---- CP6b ①：conn_fd 也要设成非阻塞 ----
                    // 和第 53~61 行处理 server_fd 时完全同形（都是"读-改-写"），只是换了 fd。
                    // 为什么必须非阻塞：事件循环是"一个线程轮流照顾所有连接"。
                    // 如果这条分机的 read 阻塞住，整个线程就停在这一条连接上，
                    // 其它分机和总机的事件全都处理不了 —— 那就退化回 CP2 的阻塞版了。
                    int cflags = fcntl(conn_fd, F_GETFL, 0);
                    if (cflags == -1) {
                        perror("fcntl F_GETFL conn");
                        close(conn_fd);   // 失败也要配平，否则就是一条 fd 泄漏
                        continue;
                    }
                    if (fcntl(conn_fd, F_SETFL, cflags | O_NONBLOCK) == -1) {
                        perror("fcntl F_SETFL conn");
                        close(conn_fd);
                        continue;
                    }

                    // ---- CP6b ②：把 conn_fd 登记进 epoll ----
                    // 登记之后，这条分机上的数据到达时，epoll_wait 才会把它报给我们。
                    // data.fd 写 conn_fd：事件回来时，就用这个标记分辨"是哪条分机"。
                    struct epoll_event cev{};
                    cev.events = EPOLLIN;    // 关心"可读"：对分机就是"对端发数据来了"
                    cev.data.fd = conn_fd;   // 标记写 conn_fd：事件回来时靠它认出是哪条分机

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &cev) < 0) {
                        perror("epoll_ctl: conn_fd");
                        close(conn_fd);   // 没登记成功就没人管它了，必须自己关掉
                        continue;
                    }

                    // ---- CP6c：把这条连接记进账本 ----
                    // 放在 epoll_ctl 成功【之后】：前面任何一步失败都已经 close + continue 了，
                    // 走到这儿才动手，就不用再回头擦屁股。
                    ClientContext ctx;
                    ctx.fd = conn_fd;
                    // ---- CP6c：存进账本（这里才是第一道哨兵）----
                    // emplace 返回 pair<iterator, bool>，那个 bool = "到底插进去了没有"。
                    // key 已存在时它【静默不插】—— 而 fd 号会被复用，
                    // 所以 false 就意味着：上一个用这个号码的连接没销账。
                    //
                    // ⚠ 为什么哨兵必须放这儿、不能只放在 erase 那儿：
                    //   漏了 erase 之后，这个 fd 的旧条目【还躺在账本里】，
                    //   下一个连接复用同一个号码时，erase 照样能删掉它（返回 1）——
                    //   所以 erase 那边永远等不到"删不到"的情况，喊不出来。
                    //   只有 emplace 这一侧才知道"这个号码不该有人占着"。
                    if (!clients.emplace(conn_fd, ctx).second) {
                        std::cerr << "账本里已经有 fd=" << conn_fd << " —— 上一个连接漏了 erase"
                                  << std::endl;
                    }
                }
            } else {
                // ---- CP6b ③ / CP6c：分机的事件（data.fd != server_fd）----
                int fd = events[i].data.fd;

                // ---- 先从账本里把这条连接找出来 ----
                // 账本 = fd → 这条连接的状态。找不到说明不该发生（登记和销账没配平），
                // 这时 continue 兜住即可 —— 绝不能拿着一个空的 ctx 硬来。
                auto it = clients.find(fd);
                if (it == clients.end()) {
                    continue;                 // 账本里没这条 —— 不该发生，跳过别硬来
                }

                // ⚠ 用【引用】而不是拷贝：拷贝一份改的是副本，inbuf 攒的东西全丢了。
                //   （副本会静默地什么都不做 —— 和 for (char c : out) 少个 & 是同一类 bug）
                ClientContext& ctx = it->second;

                char buf[1024];
                // 为什么要循环：一次事件只保证"现在有数据可读"，
                // 不保证"读一次就读完了"。和 CP6a 里 accept 循环到 EAGAIN 是同一个形状 ——
                // 非阻塞 fd 的标准用法就是"一直操作，直到 EAGAIN 为止"。
                while (true) {
                    ssize_t n = read(fd, buf, sizeof(buf));

                    if (n > 0) {
                        // ---- CP6c-2：攒进这条连接【自己】的缓冲区（不再回显）----
                        // 是 append 不是 = ：一段一段往上接，这就是半包/粘包的落脚点。
                        ctx.inbuf.append(buf, static_cast<size_t>(n));

                        // ---- 一个连接可能有好几个请求：循环解析 ----
                        // tryHandleRequest 的意思：
                        //     true  = 我处理掉了一个完整请求（已经 erase 掉了）
                        //     false = 还不够一个完整请求
                        // 所以要【循环】问它，直到问不出请求为止 ——
                        // 一次 read 可能把两个请求一起收进来（粘包），
                        // 不循环的话第二个就永远躺在 inbuf 里没人管。
                        bool gone = false;
                        while (tryHandleRequest(ctx)) {
                            // 每处理完一个就问一句：这条连接该收了吗？
                            if (ctx.want_close) {
                                // 两本账一起销：fd 靠 close，账本条目靠 erase
                                close(fd);
                                if (clients.erase(fd) == 0) {
                                    std::cerr << "账本里没有 fd=" << fd << " —— 有地方漏销账"
                                              << std::endl;
                                }
                                gone = true;
                                break;
                            }
                            // 不用关 → 回到 while 顶部，看 inbuf 里还有没有下一个请求
                        }
                        if (gone) break;   // 连接已经收了 → 跳出 read 循环
                    } else if (n == 0) {
                        // read 返回 0 = 对端【正常】关闭（有序关闭，不是 RST）。
                        // 只 close 就够了，不必显式 EPOLL_CTL_DEL：
                        // man 7 epoll 说 fd 关闭时会自动从所有 interest list 摘除；
                        // 显式 DEL 反而有风险 —— fd 号可能已被新连接复用，会误删别人的登记。
                        close(fd);
                        // fd 销了，账本也要销 —— 两本账必须一起平。
                        //
                        // 为什么要写成 if：erase 返回"删掉了几个"（只能是 0 或 1）。
                        // 返回 0 = 账本里【本来就没有】这条 —— 那必然是别的地方漏销账了。
                        //
                        // 这是【故意加的哨兵】：漏 erase 时，fd 数是平的、cp6b_test.py
                        // 4/4 全绿、编译零警告 —— 只有这里会喊出来。
                        if (clients.erase(fd) == 0) {
                            std::cerr << "账本里没有 fd=" << fd << " —— 有地方漏销账" << std::endl;
                        }
                        break;
                    } else {
                        if (errno == EINTR) continue;   // 被信号打断：重试，不是错误
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;   // 数据读干净了 —— 这是【正常】出口，不是错误
                        }
                        perror("read");   // 其它错误（如 ECONNRESET：对端 RST 强断）
                        close(fd);
                        // 这条路径同样两本都要销（哨兵作用和上面那个一样，见上）
                        if (clients.erase(fd) == 0) {
                            std::cerr << "账本里没有 fd=" << fd << " —— 有地方漏销账" << std::endl;
                        }
                        break;
                    }
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
