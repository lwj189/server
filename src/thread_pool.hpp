// ===========================================================================
// src/thread_pool.hpp：线程池（只有类，没有 main）
//
// 为什么要单独做成头文件？
//   原来 ThreadPool 类写在 src/thread_pool.cpp 里，那个文件还有个 demo main()。
//   服务器要用这个类，总不能再抄一份（改一处忘一处，两份代码就会漂移）。
//   抽成头文件之后：
//     · src/thread_pool.cpp  —— include 它，跑那个 demo（学习用）
//     · src/thread.cpp  —— include 它，接真的服务器
//   同一个类，只有一份。
//
// 线程池是什么？（一句话）
//   提前养几个线程，让它们排队领活干。连接来了 → 打包成"任务"丢进队列
//   → 某个空闲线程领走 → 干完再领下一个。
//
//   为什么不用"一条连接开一个线程"：1000 条连接 = 1000 个线程，
//   光线程栈就 8MB × 1000 = 8GB，线程创建/销毁本身也要钱。
//
// ⚠ 用了 std::thread，编译必须加 -pthread：
//     不加能编译通过，但链接会报 std::thread 相关的一堆 undefined
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
// 线程池
//
// 三个成员各管一件事，缺一不可：
//   tasks_  —— 待办事项清单（"还有哪些活没人干"）
//   mtx_    —— 清单的锁（多个线程同时读写清单会打架）
//   cv_     —— 门铃（"来活了！" / "下班了！"）
// ===========================================================================
class ThreadPool {
public:
    // 构造函数：开 n 个线程，每个线程立刻进入"等活干"的循环。
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            // emplace_back 直接在 vector 里构造一个 thread 对象（省一次拷贝）
            // [this] 是 lambda 的捕获：把这个 ThreadPool 对象的指针带进线程函数
            //        —— 线程函数是个自由函数，不捕获就找不到 mtx_ / tasks_
            workers_.emplace_back([this] { workerLoop(); });
        }
        std::cout << "线程池启动，工人数 = " << n << std::endl;
    }

    // 析构函数：把工人全部遣散，并等他们把手上的活干完。
    // ⚠ 这一步不能省：进程退出时如果有线程还在跑，会直接崩（std::terminate）
    ~ThreadPool() {
        {
            // 为什么要加花括号：让 lock 在这一小段结束后【立刻解锁】。
            // 下面 notify_all 要唤醒的线程醒来后也要拿这把锁，
            // 如果这里还锁着，它们会全部卡住 —— 那就是死锁。
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
        // 解锁之后才按门铃（理由同上：别让被唤醒的线程卡在锁上）
        cv_.notify_one();   // 只叫一个工人（活只有一件，叫多了也是白醒）
    }

    size_t workerCount() const { return workers_.size(); }

private:
    // 一个工人的一生：循环"等活 → 领活 → 干活"，直到收到下班通知
    void workerLoop() {
        while (true) {
            std::function<void()> task;

            {
                // unique_lock 和 lock_guard 的区别：unique_lock 可以【中途解锁】，
                // condition_variable::wait 需要这个能力，所以这里必须用它
                std::unique_lock<std::mutex> lock(mtx_);

                // wait 的两个参数：锁 + 一个"什么时候该醒"的判断
                //   醒来时它会：先解锁 → 睡 → 被叫醒 → 重新上锁 → 检查判断
                //   · 判断为 true  → 继续往下走
                //   · 判断为 false → 接着睡（这叫"虚假唤醒"，操作系统可能无故叫醒你）
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                // 两种该走人的情况：收到下班通知，而且清单已经空了
                if (stop_ && tasks_.empty()) {
                    return;   // 工人退休
                }

                // 从清单头上领一件活（front 看一眼，pop 拿走）
                task = std::move(tasks_.front());
                tasks_.pop();
            }   // ← 到这里锁自动释放。干活的时候【不该占着锁】，
                //   否则其他工人没法领活，池子退化成单线程

            task();   // 干活（在锁外面干）
        }
    }

    std::vector<std::thread> workers_;          // 工人名单
    std::queue<std::function<void()>> tasks_;   // 待办清单：什么活都能装
    std::mutex mtx_;                            // 保护 tasks_ 和 stop_
    std::condition_variable cv_;                // "来活了" / "下班了" 的门铃
    bool stop_ = false;                         // 下班标志
};
