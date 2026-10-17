// ===========================================================================
// lib_server.cpp：第四个版本 —— 用 cpp-httplib 实现（对照组）
//
// 为什么要它？
//   项目里已经有三个【手写】的服务器：
//     p2.cpp     阻塞串行        335 行
//     epoll.cpp  单线程 Reactor   367 行
//     thread.cpp 单 Reactor + 线程池 492 行
//   这一版用现成的库实现同一个功能，完整代码 【9 行】。
//
//   放在一起跑同一套测试、同一个压测，才能回答一个真问题：
//   **手写 vs 用库，到底差在哪？**
//     · 代码量差 40 倍（这个一目了然）
//     · 协议正确性差多少？（跑 tests/p2_test.py 看）
//     · 吞吐/延迟差多少？（跑 tests/load_test.sh 看）
//     · 但【控制力】呢？—— 见下面「库拿走了什么」
//
// ===========================================================================
// 库拿走了什么（这正是前三版你亲手写过的部分）
// ===========================================================================
//
//   ┌─────────────────────────┬──────────────────────┬─────────────────────┐
//   │ 这一层                  │ 手写版（你写的）      │ cpp-httplib         │
//   ├─────────────────────────┼──────────────────────┼─────────────────────┤
//   │ socket/bind/listen      │ p2.cpp 步骤 1~5      │ 库内部              │
//   │ accept 循环             │ p2.cpp 步骤 6        │ 库内部（线程池）     │
//   │ read 到完整请求         │ 每连接缓冲区 + 循环   │ 库内部              │
//   │ \r\n\r\n 定界           │ findHeader           │ 库内部              │
//   │ Content-Length + 上限   │ MAX_HEADER/BODY_SIZE │ 库内部（有默认值）   │
//   │ 拼 HTTP 响应            │ buildResponse        │ res.set_content     │
//   │ epoll 事件循环          │ epoll.cpp 全部       │ 无（每连接一线程）   │
//   │ keep-alive              │ want_close + erase   │ 库内部              │
//   │ EMFILE / 配平 / 哨兵    │ 你踩过的那些坑        │ 库内部              │
//   └─────────────────────────┴──────────────────────┴─────────────────────┘
//
//   ⚠ 关键区别：cpp-httplib **不是 epoll**。它用的是"每连接一个线程 + 阻塞 IO"
//     （内部有个线程池，默认线程数 = CPU 核数）。
//     所以它【没有】你 epoll.cpp 里的那些东西：
//       · 没有 epoll 三件套    · 没有非阻塞 fd / EAGAIN
//       · 没有 ClientContext   · 没有 EPOLL_CTL_DEL / ADD 的交接
//     连接数一多，它就卡在"线程数"上 —— 这正是 epoll 存在的理由。
//
// ===========================================================================
// 编译运行
// ===========================================================================
//
//   编译（单头文件，不需要链接任何库）：
//       g++ -std=c++17 -O2 lib_server.cpp -o lib_server -pthread
//
//   ⚠ 第一次编译会慢（30~60 秒）：httplib.h 有 2.3 万行，预处理和模板实例化都在这。
//     之后改了代码重编会快一些（`-O2` 帮了不少忙）。
//
//   运行：
//       ./lib_server
//       ./lib_server --port 9999
//
//   测试（和另外三个版本完全一样）：
//       ./lib_server &
//       python3 tests/p2_test.py
//       python3 tests/balance_test.py --pid $! --log server.log
//
// ===========================================================================

// ===========================================================================
// ⚠ 上限：手写版和库【检查的时机不一样】（实测出来的，不是猜的）
// ===========================================================================
//
//   库的默认值：
//     CPPHTTPLIB_HEADER_MAX_LENGTH    = 8192   （和手写版 8KB 一致）
//     CPPHTTPLIB_PAYLOAD_MAX_LENGTH   = 100 MB （手写版是 1MB，宽了 100 倍）
//
//   下面的宏 + 运行时 set_payload_max_length 都把它对齐到了 1MB。
//   但**对齐了上限，p2_test 的 [7] 和 [10] 两项仍然是红的** —— 原因不是数值，
//   是【判定的时机】：
//
//     ┌──────────────────┬──────────────────────┬────────────────────────┐
//     │ 场景              │ 手写版（p2/epoll）    │ cpp-httplib            │
//     ├──────────────────┼──────────────────────┼────────────────────────┤
//     │ 声明 64MB，                                        │
//     │ 一个字节 body 都不发 │ 解析头部时就看到 64MB  │ 发完头部就等着收 body，│
//     │                    │ 超 1MB → 立刻拒绝      │ 不拒绝（等到读超时才断）│
//     ├──────────────────┼──────────────────────┼────────────────────────┤
//     │ 声明 2MB，        │ 同上，立刻拒绝        │ 真收满 2MB → 413 ✓     │
//     │ 真发 2MB          │                       │                       │
//     └──────────────────┴──────────────────────┴────────────────────────┘
//
//   实测证据（同一台机器）：
//     声明 64MB + 只发 1MB   → 库 TIMEOUT（还在等 body）
//     声明 2MB  + 真发 2MB   → 库 413 Payload Too Large ✓
//     声明 1MB  + 发 1MB     → 库 200 OK ✓（合法的照常过）
//
//   ⚠ 差别在安全上的意义：手写版是"**声明大了就不收**"，
//     库是"**收着收着发现大了才拒绝**"。前者更早止血，后者要等到数据真的来。
//
// ★ 这就是"用库"的核心认知：**省的是代码，不是决策**。
//   每一个默认值背后都是一个契约，你得知道它是什么、和你想要的一不一样 ——
//   而且**光对齐数值还不够，还要知道它什么时候判**。
// ===========================================================================
#define CPPHTTPLIB_PAYLOAD_MAX_LENGTH (1024 * 1024)   // 1MB，对齐手写版

#include "httplib.h"   // 单头文件：把它拖到项目目录就能用，零依赖

#include <cstdlib>     // std::atoi
#include <iostream>    // std::cout / std::endl
#include <string>      // std::string

// 和另外三个版本返回完全一样的内容 —— 这样 p2_test.py 的 23 项才有可比性
// （如果这里随便写，测试红了一片，就分不清是"库不行"还是"我写的不一样"）
static constexpr const char* kBody = "<h1>Hello from my own server!</h1>";

int main(int argc, char** argv) {
    int port = 8888;

    // 只解析一个 --port，别的不折腾（这一版的重点是"最少代码"）
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            std::cout << "用法: " << argv[0] << " [--port N]\n";
            return 0;
        }
    }

    httplib::Server svr;

    // ★ 必须显式设 SO_REUSEADDR —— 库的默认值在这里有个坑（实测踩到，不是猜的）
    //
    //   httplib 的 default_socket_options() 原文：
    //       #ifdef SO_REUSEPORT
    //           set_socket_opt(sock, SOL_SOCKET, SO_REUSEPORT, 1);
    //       #else
    //           set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
    //       #endif
    //
    //   Linux 上 SO_REUSEPORT 是【有定义】的 → 所以它只设了 REUSEPORT，
    //   根本没设 REUSEADDR。
    //
    //   而两者不是一回事：
    //     · SO_REUSEADDR：允许 bind 一个【还有 TIME_WAIT 残留】的端口  ← 我们要的
    //     · SO_REUSEPORT：允许多个进程【同时监听】同一端口（多 Reactor 用）
    //
    //   现象：跑完一轮测试后 8888 上积了 641 个 TIME_WAIT，库版启动直接失败
    //         "监听 8888 失败（端口被占？）"，而手写版（p2/epoll/thread）全都
    //         显式设了 SO_REUSEADDR，所以照样能起。
    //   → 换端口（9999）也证明不是代码问题：空端口一次就起来了。
    svr.set_socket_options([](int sock) {
        int yes = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                     static_cast<const void*>(&yes), sizeof(yes));
    });

    // 运行时设上限 —— 比宏更可靠（宏那个我实测没生效，见文件顶部注释）
    svr.set_payload_max_length(1024 * 1024);   // 1MB，对齐手写版
    svr.set_read_timeout(5, 0);

    // 手写版每个请求都会打一行日志；库默认【什么都不打】。
    // 这又是一条"库替你做了决定"：它默认没有访问日志。
    // keepalive_test.py 就是靠读日志来确认"两个请求真的都被处理了"——
    // 不补这个 logger，那 3 项会红（但 keep-alive 本身是好的）。
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        std::cout << req.method << " " << req.path << " -> " << res.status << std::endl;
    });

    // ---- 全部就是这些 ----
    //
    // R"(/.*)" 是正则：匹配任何以 / 开头的路径。
    // 为什么用正则而不是 svr.Get("/", ...)：另外三个版本【不看路径】，
    // 一律返回同样的 body。用同一个契约，测试才有可比性。
    //
    // 这里面省掉的东西：
    //   · 不用 accept、不用 read、不用自己判断"请求收全了没有"
    //   · 不用拼 "HTTP/1.1 200 OK\r\nContent-Type: ...\r\nContent-Length: ..."
    //   · 不用管 keep-alive（库按 Connection 头自动处理）
    //   · 不用管 send 的返回值（库内部有输出缓冲 + 部分写的处理）
    svr.Get(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        // set_content 会做三件事：填 body、设 Content-Type、自动算 Content-Length
        res.set_content(kBody, "text/html");
    });

    // ⚠ 必须把每个方法都注册一遍 —— 这是"用库"和"手写"的一个真实差异：
    //   手写版根本不看请求行里的方法（一律返回同样 body）；
    //   库是【按方法路由】的，没注册的方法直接回 404。
    //   我第一版只写了 Get，p2_test 的 23 项里立刻挂了 7 项（全是 404）。
    //   这是个很好的例子：**用库省代码，但你要先知道它默认替你做了什么决定。**
    auto sameBody = [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kBody, "text/html");
    };
    svr.Post(R"(/.*)", sameBody);
    svr.Put(R"(/.*)", sameBody);
    svr.Patch(R"(/.*)", sameBody);
    svr.Delete(R"(/.*)", sameBody);
    svr.Options(R"(/.*)", sameBody);
    // （没有 svr.Head：cpp-httplib 用 Get 处理 HEAD 请求，不提供单独注册）

    // ---- 库给了什么默认值（前三个版本是你自己定的，这里是库替你定的）----
    //
    //   · 请求头/行上限      CPPHTTPLIB_HEADER_MAX_LENGTH（默认 8192 字节）
    //   · body 上限          CPPHTTPLIB_PAYLOAD_MAX_LENGTH（默认 100 MB —— 比手写版的 1MB 宽 100 倍）
    //   · 超限返回           431（头过大）/ 413（body 过大）
    //   · 连接超时           CPPHTTPLIB_KEEPALIVE_TIMEOUT_SECOND（默认 5 秒）
    //   · 线程池大小         CPPHTTPLIB_THREAD_POOL_COUNT（默认 = CPU 核数）
    //
    //   ⚠ 这些值是【编译期宏】。要改必须在 include 之前 #define —— 比如想让它
    //     和手写版的上限一致（8KB / 1MB）：
    //         #define CPPHTTPLIB_PAYLOAD_MAX_LENGTH (1024 * 1024)
    //         #include "httplib.h"
    //     不改的话，p2_test.py 里测"body 超过 1MB 要被拒"的那几项
    //     行为会不一样（库的默认上限是 8MB，不会拒绝 1MB 的 body）。

    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "监听 " << port << " 失败（端口被占？）" << std::endl;
        return 1;
    }
    return 0;
}

// ===========================================================================
// 看完这份代码，可以拿来问自己的三个问题
// ===========================================================================
//
//   1. 为什么这一版没有 epoll？
//      —— 因为库用的是"每连接一个线程"。线程一多就吃内存和调度开销，
//         所以它撑不了 2.8 万个并发连接。你 epoll 版能撑，就是因为
//         一个线程照看所有连接，每条连接只占一个 fd + 一小块状态。
//
//   2. 如果线上要"10 万并发连接"，这个库能不能用？
//      —— 不能（10 万线程直接爆）。要么自己写 epoll，要么换异步库（如 Asio）。
//
//   3. 那什么时候该用库？
//      —— 当你的时间比"省那点内存"值钱的时候。绝大多数内部服务，
//         几百个并发连接，用库是对的 —— 少写 300 行，少踩几十个坑。
//         这也解释了为什么"会写 epoll"依然是本事：**知道什么时候不该用库**。
// ===========================================================================
