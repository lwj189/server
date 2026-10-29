// ===========================================================================
// src/p2_lib.cpp：对照组 —— 用 cpp-httplib 实现同一个服务器
//
// 目的：和手写的三个版本（p2 / epoll / thread）跑同一套测试和压测，
// 回答"手写 vs 用库，差在哪"。完整实现只有 9 行。
//
// 对照分析（库拿走了哪几层、上限判定时机的差别、SO_REUSEADDR 的坑）
// 见 notes/handwritten_vs_library.md，代码里不展开。
//
// 编译（单头文件、零依赖；第一次慢是因为 httplib.h 有 2.3 万行）：
//   g++ -std=c++17 -O2 src/p2_lib.cpp -o build/p2_lib -pthread
// 运行：build/p2_lib [--port 9999]
// ===========================================================================

// ===========================================================================
// 关于上限：手写版和库【判定的时机不一样】
// ===========================================================================
//
//   库默认 PAYLOAD_MAX_LENGTH = 100 MB（手写版 1MB）。下面用宏 + 运行时
//   set_payload_max_length 都对齐到了 1MB —— 但**对齐数值之后，p2_test 的
//   [7]、[10] 两项仍然是红的**，因为差别不在数值，在时机：
//
//     手写版：解析头部时看到声明的长度超限 → 立刻拒绝，不等 body
//     库    ：发完头就开始等 body，真收满了才判 413
//
//   所以「声明 64MB 但一个字节都不发」时，手写版立刻止血，库会一直等到读超时。
//   ★ 用库省的是代码，不是决策：每个默认值背后都是一个契约，
//     得知道它是什么、和你想要的一不一样、以及它什么时候判。
// ===========================================================================
#define CPPHTTPLIB_PAYLOAD_MAX_LENGTH (1024 * 1024)   // 1MB，对齐手写版

#include "httplib.h"   // 单头文件：把它拖到项目目录就能用，零依赖

#include <cstdlib>    // std::atoi
#include <iostream>   // std::cout / std::endl
#include <string>     // std::string

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

    // ★ 必须显式设 SO_REUSEADDR —— 库的默认值在这里有个坑：
    //   httplib 的 default_socket_options() 里，只要平台定义了 SO_REUSEPORT
    //   就只设 REUSEPORT、不设 REUSEADDR（Linux 正是这种）。
    //   而 REUSEPORT 只管"多个进程同时监听同一端口"，不解决
    //   "端口上还有 TIME_WAIT 残留时 bind 失败" —— 不补这一句，跑过一轮测试就起不来。
    svr.set_socket_options([](int sock) {
        int yes = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                     static_cast<const void*>(&yes), sizeof(yes));
    });

    // 运行时设上限 —— 比宏更可靠（宏不一定生效）
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

    // ---- 库替你定下的默认值 ----
    //   · 请求头上限   CPPHTTPLIB_HEADER_MAX_LENGTH = 8192（和手写版一致）
    //   · body 上限    CPPHTTPLIB_PAYLOAD_MAX_LENGTH = 100MB（手写版是 1MB）
    //   · 超限返回     431（头过大）/ 413（body 过大）
    //   · 连接超时     5 秒        · 线程池   默认 = CPU 核数
    //   这些都是【编译期宏】，要改必须在 #include 之前 #define；
    //   但就算数值对齐了，判定时机仍然不同 —— 分析见 notes/handwritten_vs_library.md。

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
