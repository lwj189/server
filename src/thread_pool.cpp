// thread_pool.cpp -- 线程池独立演示：不接 socket，10 个任务丢给 4 个工人。
//   g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion thread_pool.cpp -o thread_pool -pthread
// 单独跑是因为在服务器里调试线程池很难受：任务和连接、fd、epoll 搅在一起，
// 出了问题分不清是池子的锅还是服务器的锅。先用这个确认池子本身是好的。
//
// ThreadPool 类在 thread_pool.hpp 里，thread.cpp include 的是同一个类。

#include "thread_pool.hpp"

#include <chrono>
#include <iostream>

int main() {
    ThreadPool pool(4);

    for (int i = 1; i <= 10; ++i) {
        // 按值捕获 i。按引用的话，任务真正跑到时 i 可能已经是 11 了。
        pool.submit([i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::cout << "  任务 " << i << " 由线程 " << std::this_thread::get_id()
                      << " 处理" << std::endl;
        });
    }

    std::cout << "10 个任务都提交完了，主线程先歇着（不等结果）" << std::endl;
    // main 返回后 pool 析构，在那里等所有任务跑完
    return 0;
}
