// p2_lib.cpp -- 对照组：同一个服务器用 cpp-httplib 实现，用于对比手写和用库。
//   g++ -std=c++17 -O2 p2_lib.cpp -o p2_lib -pthread
// 对照结论（库拿走了哪几层）见 notes/handwritten_vs_library.md。

// 上限不只是数值，还有判定时机：手写版在解析头部时看到声明的长度超限就拒绝，
// 库要真把 body 收满才判 413。对齐到 1MB 之后 p2_test 的 [7]、[10] 仍然是红的，
// 原因就在这里 —— "声明 64MB 但一个字节不发"，手写版立刻止血，库要等到读超时。
#define CPPHTTPLIB_PAYLOAD_MAX_LENGTH (1024 * 1024)   // 1MB，对齐手写版

#include "httplib.h"   // 单头文件，零依赖

#include <cstdlib>
#include <iostream>
#include <string>

// 和另外三个版本返回完全一样的内容，否则 23 项测试没有可比性，
// 也分不清是"库不行"还是"我写得不一样"。
static constexpr const char* kBody = "<h1>Hello from my own server!</h1>";

int main(int argc, char** argv) {
    int port = 8888;

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

    // 必须显式设 SO_REUSEADDR：httplib 在定义了 SO_REUSEPORT 的平台上（Linux 就是）
    // 只设 REUSEPORT，不解决 TIME_WAIT 残留导致的 bind 失败，跑过一轮测试就起不来。
    svr.set_socket_options([](int sock) {
        int yes = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                     static_cast<const void*>(&yes), sizeof(yes));
    });

    // 运行时设更可靠，宏不一定生效
    svr.set_payload_max_length(1024 * 1024);
    svr.set_read_timeout(5, 0);

    // 库默认不打日志。keepalive_test 靠读日志确认两个请求都被处理了，不补这个会红。
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        std::cout << req.method << " " << req.path << " -> " << res.status << std::endl;
    });

    // 用正则匹配任何路径：另外三个版本不看路径，一律返回同样的 body。
    svr.Get(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kBody, "text/html");
    });

    // 每个方法都要注册：库是按方法路由的，没注册的直接 404，
    // 而手写版根本不看方法 —— 少一个 p2_test 就红几项。
    auto sameBody = [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kBody, "text/html");
    };
    svr.Post(R"(/.*)", sameBody);
    svr.Put(R"(/.*)", sameBody);
    svr.Patch(R"(/.*)", sameBody);
    svr.Delete(R"(/.*)", sameBody);
    svr.Options(R"(/.*)", sameBody);
    // 没有 Head：cpp-httplib 用 Get 处理 HEAD。

    // 库的其余默认值：请求头 8192、body 100MB、超限 431 / 413、连接超时 5s、
    // 线程池 = CPU 核数。都是编译期宏，改必须在 #include 之前。

    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "监听 " << port << " 失败（端口被占？）" << std::endl;
        return 1;
    }
    return 0;
}
