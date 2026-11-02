// ===========================================================================
// thread_pool.hpp：线程池（只有类，没有 main）
//
// 为什么单独做成头文件：ThreadPool 原来写在 thread_pool.cpp 里（那个文件还有个 demo
// main()）。服务器要用这个类，总不能再抄一份 —— 抽成头文件之后，thread_pool.cpp 跑 demo、
// thread.cpp 接真的服务器，同一个类只有一份。
// 为什么不用"一条连接开一个线程"：1000 条连接 = 1000 个线程，光线程栈就 8MB × 1000 = 8GB，
// 线程创建 / 销毁本身也要钱。
// ⚠ 用了 std::thread，编译必须加 -pthread：不加能编译通过，但链接会报 std::thread 相关的一堆 undefined
// ===========================================================================

#pragma once   // 同一个 .cpp 里 include 两次也只展开一次（头文件守卫的另一写法）

#include <condition_variable>  // std::condition_variable
#include <cstddef>             // size_t
#include <functional>          // std::function
#include <iostream>            // std::cout / std::endl
#include <mutex>               // std::mutex / std::lock_guard / std::unique_lock
#include <queue>               // std::queue
#include <thread>              // std::thread
#include <utility>             // std::move
#include <vector>              // std::vector

// ===========================================================================
// 三个成员各管一件事，缺一不可：tasks_ 待办清单、mtx_ 保护它的锁、cv_ 让工人睡着等活
// （没有 cv_，工人只能空转轮询队列，白烧一个核）。
// ===========================================================================
class ThreadPool {
public:
    // 构造函数：开 n 个线程，每个线程立刻进入"等活干"的循环。
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            // [this] 必须捕获：线程函数是自由函数，不捕获就找不到 mtx_ / tasks_
            workers_.emplace_back([this] { workerLoop(); });
        }
        std::cout << "线程池启动，工人数 = " << n << std::endl;
    }

    // 析构函数：把工人全部遣散，并等他们把手上的活干完。
    // ⚠ 这一步不能省：进程退出时如果有线程还在跑，会直接崩（std::terminate）
    ~ThreadPool() {
        {
            // 这一小段结束就解锁：下面 notify_all 唤醒的线程醒来也要拿这把锁，这里还锁着
            // 它们会全卡住 —— 顺序必须是 置 stop_ → 解锁 → notify → join。
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();   // 通知所有睡着的工人："下班了，起来看看"

        // join = 等这个线程跑完。必须等，不能直接走
        for (std::thread& t : workers_) {
            t.join();
        }
        std::cout << "线程池关闭" << std::endl;
    }

    ThreadPool(const ThreadPool&) = delete;             // 线程池不可拷贝：
    ThreadPool& operator=(const ThreadPool&) = delete;  // 拷贝一份 = 两套工人抢一个队列

    // 提交一个任务。调用方（主线程）把活丢进来就走，不等它做完。
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mtx_);   // 改清单前先上锁
            tasks_.push(std::move(task));             // move：把任务搬进去，不拷贝
        }
        // 解锁之后才 notify（同上：别让被唤醒的线程卡在锁上）
        cv_.notify_one();   // 只叫一个工人（活只有一件，叫多了也是白醒）
    }

    size_t workerCount() const { return workers_.size(); }

private:
    // 一个工人的一生：循环"等活 → 领活 → 干活"，直到收到下班通知
    void workerLoop() {
        while (true) {
            std::function<void()> task;

            {
                // wait 要中途解锁，所以必须用 unique_lock（不能用 lock_guard）；
                // 两个参数 = 锁 + "什么时候该醒"：醒来重新上锁并再查一次判断，为 false 接着睡
                std::unique_lock<std::mutex> lock(mtx_);

                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                // 两种该走人的情况：收到下班通知，而且清单已经空了
                // 注意是 && ：只要队列里还有活就必须干完再走，不能 stop_ 一到就扔掉排队的任务
                if (stop_ && tasks_.empty()) {
                    return;   // 工人退休
                }

                // 从清单头上领一件活（front 看一眼，pop 拿走）
                task = std::move(tasks_.front());
                tasks_.pop();
            }   // ← 锁在这里释放：干活时【不该占着锁】，否则其他工人领不到活，池子退化成单线程

            task();   // 干活（在锁外面干）
        }
    }

    std::vector<std::thread> workers_;          // 工人名单
    std::queue<std::function<void()>> tasks_;   // 待办清单：什么活都能装
    std::mutex mtx_;                            // 保护 tasks_ 和 stop_
    std::condition_variable cv_;                // 工人靠它睡着等活（来活 / 下班都从这里叫醒）
    bool stop_ = false;                         // 下班标志
};
