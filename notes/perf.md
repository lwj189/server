# 压测数据（JMeter）

> 这是本项目**唯一的性能数据**，别埋着。
> ⚠ 被测版本是 **`p2.cpp`（单线程阻塞 + backlog=3）** —— epoll 版还没测过。
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
