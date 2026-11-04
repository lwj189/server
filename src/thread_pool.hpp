// thread_pool.hpp -- 固定大小的线程池。
// thread_pool.cpp 用它跑 demo，thread.cpp 用它接真服务器，同一个类只有一份。
// 用 std::thread，编译必须加 -pthread（不加能过编译，链接时报 undefined）。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            // 必须捕获 this，线程函数里要用 mtx_ / tasks_
            workers_.emplace_back([this] { workerLoop(); });
        }
        std::cout << "线程池启动，工人数 = " << n << std::endl;
    }

    // 停机 + join 不能省：进程退出时还有线程在跑，会直接 std::terminate。
    ~ThreadPool() {
        {
            // 花括号让 lock 在这里结束。下面 notify 唤醒的线程醒来也要拿这把锁，
            // 还锁着就全部卡死。
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();

        for (std::thread& t : workers_) {
            t.join();
        }
        std::cout << "线程池关闭" << std::endl;
    }

    // 拷贝一份就是两套工人抢同一个队列
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 丢进来就走，不等做完
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.push(std::move(task));
        }
        // 解锁之后再按门铃，只叫一个：活只有一件，多叫的白醒一次
        cv_.notify_one();
    }

    size_t workerCount() const { return workers_.size(); }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;

            {
                // 必须 unique_lock：wait 要在等待期间把锁放掉，lock_guard 做不到
                std::unique_lock<std::mutex> lock(mtx_);

                // 谓词不能省：可能被虚假唤醒，醒来后要重新确认有没有活或者要不要下班
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                if (stop_ && tasks_.empty()) {
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }   // 锁在这里释放。干活不能在锁里，否则其他工人领不到活，池子退化成单线程

            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;              // 保护 tasks_ 和 stop_
    std::condition_variable cv_;
    bool stop_ = false;
};
