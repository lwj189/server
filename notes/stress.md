# 压测数据（JMeter）

> 这是本项目**唯一的性能数据**，别埋着。
> 两批数据：**① `p2.cpp`（阻塞 + backlog=3）** 在第 1 节；
> **② `epoll.cpp`（事件驱动 + keep-alive）** 在第 2 节（2026-10-04 测）。
> 重跑：`tests/load_test.sh`（驱动）→ `tests/analyze.sh`（出统计表）

---

1.源码于p2（做一个简易的socket服务器）

int main() {
    创建套接字();
    允许端口复用(); // 具体函数忘了，留空
    绑定端口();
    监听();

    while(1) {
        等待客户端连接(); // accept
        读取请求数据();   // read
        发送固定网页();   // send
        关闭连接();       // close
    }
}

压测：
1 被测版本：p2.cpp（单线程阻塞 + backlog=3）
2 环境：4核VM，ubuntu，somaxconn=4096，ulimit -n=524288
3 工具：JMeter 5.6.3（同机压测 → 绝对值仅供参考）
4 场景：GET /，Connection: close（每请求新建连接）
5 压力测试结果：
===== 1 并发 × 1000 次 =====
summary =   1000 in 00:00:01 = 1811.6/s Avg:     0 Min:     0 Max:    15 Err:     0 (0.00%)
===== 10 并发 × 200 次 =====
summary =   2000 in 00:00:01 = 2034.6/s Avg:     0 Min:     0 Max:    15 Err:     0 (0.00%)
===== 50 并发 × 200 次 =====
summary =  10000 in 00:00:02 = 4284.5/s Avg:     3 Min:     0 Max:  1064 Err:     0 (0.00%)
===== 100 并发 × 200 次 =====
summary =  20000 in 00:00:04 = 4935.8/s Avg:     7 Min:     0 Max:  2002 Err:     2 (0.01%)
===== 200 并发 × 200 次 =====
summary =  40000 in 00:00:05 = 7900.5/s Avg:    12 Min:     0 Max:  2003 Err:    38 (0.10%)
6 现象：Recv-Q 顶到 3~4；2 个 ConnectTimeout；TIME_WAIT 13119

结论：
1. 处理能力极强：p50=0.4ms、p90=1ms —— 瓶颈完全不在请求处理
2. 失败率随并发暴涨：0 → 0 → 0 → 2 → 38
   → 拐点在 100 ~ 200 并发之间
3. 失败类型全是 ConnectTimeoutException（连不上，不是处理不了）
4. 建连 p99=1001ms、max=2003ms —— 接近整数秒
   → 这是 TCP SYN 重传退避（1s → 2s）的铁证
   → 根因：accept 队列（backlog=3）被填满，内核丢弃新 SYN

---

2.源码于epoll（事件驱动 + keep-alive，CP6d-2 之后的版本）

跑法（load_test.sh 现在能切服务器了）：
BIN=./epoll SRC=epoll.cpp TAG=e- KEEPALIVE=false ./tests/load_test.sh
BIN=./epoll SRC=epoll.cpp TAG=k- KEEPALIVE=true  ./tests/load_test.sh

压力测试结果（50 并发 × 200 次，各跑 3 次）：
===== keepalive=false（Connection: close）=====
3530 / 3771 / 3769 /s     请求 10000 / 连接 10000     TIME_WAIT +166 ~ +700
===== keepalive=true（Connection: keep-alive）=====
10142 / 10616 / 9461 /s   请求 10000 / 连接 50        TIME_WAIT 反而在减少

keepalive=true 的阶梯：
===== 100 并发 × 200 次 =====
11947 /s   Err  0 (0.00%)   Avg  4.6ms   p99 20ms   建连p99 0ms
===== 200 并发 × 200 次 =====
14875 /s   Err  3 (0.01%)   Avg  8.3ms   p99 30ms   建连p99 0ms
===== 400 并发 × 200 次 =====
17305 /s   Err 21 (0.03%)   Avg 14.5ms   p99 52ms   建连p99 0ms
（三个档位总共只开了 700 条连接 —— 每个线程 1 条，完美复用）

对比 p2（同为 100 / 200 并发）：
并发 100：p2 4935.8/s（2 错）  → epoll+keepalive 11947/s（0 错）   2.4×
并发 200：p2 7900.5/s（38 错） → epoll+keepalive 14875/s（3 错）   1.9×，错误少 12×

结论：
1. keep-alive 是最大的那个杠杆：2.7× 吞吐、1/200 的连接数
2. 建连 p99 从 1001ms → 0ms = "连不上"那道墙消失的直接证据
3. 墙的根因一直是 backlog=3，不是 CPU；keep-alive 把连接数压掉之后墙才退远
   （p2 那道墙是"每请求一条连接" × backlog=3 撞出来的）
4. 剩下的墙还在 backlog=3：200/400 并发那几个错全发生在【爬坡瞬间】
   （几百条 SYN 同时到，等位区只有 3 个位置）→ 下一步调 listen(fd, backlog) / somaxconn
5. epoll 的收益不是"处理更快"（p50 一直零点几毫秒），
   而是"一个线程同时握住很多连接"—— 没有这点，keep-alive 无从谈起

压测工具自己踩的两个坑（都已修，别再踩）：
1. analyze.sh：JMeter 的 failureMessage 里【带换行】→ 续行被当成新样本
   → $1+0=0 把 min 拉到 0 → 耗时算成 1.79e9 秒、吞吐列变 0、样本数 10019≠10000
   修法：4 处加 $1 ~ /^[0-9]+$/ 守卫
2. http_load.jmx：use_keepalive 写成 boolProp 时，-Jkeepalive=true 【静默失效】
   → 连跑两轮"关 / 开 keep-alive"，其实两轮都是 Connection: close，
     吞吐却差了 60% —— 差点被当成"keep-alive 有效"
   修法：改成 stringProp（实测：2000 请求 2000 条连接 → 10 条连接）
   验证方法：直接看服务器收到的请求头是 keep-alive 还是 close

未验证的猜想（留着以后验）：epoll.cpp 每个请求写 5 行日志到 stdout，
而 std::endl 会 flush —— 同步阻塞 IO，可能正压着吞吐上限。
要验证就造一个"关掉日志"的变体对比。
