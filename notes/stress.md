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
  ⚠ JMeter 自己只有 1GB 堆（`jmeter.log` 里的 Max memory=1073741824），
    4 核机器上它和被测服务器抢 CPU —— 并发拉很高时**JMeter 可能先成为瓶颈**。
  ⚠ 手动跑 JMeter 一定要带 `-j <文件>`，否则它会把 `jmeter.log` 拉在当前目录
    （load_test.sh 已经带了，日志进 tests/results/）。
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

2.源码于epoll（事件驱动 + CP6d-1 EMFILE 暂停 + CP6d-2 keep-alive）

跑法（load_test.sh 能切服务器；三个配置各跑一遍，前缀不同不会互相覆盖）：
BIN=./epoll SRC=epoll.cpp TAG=e- KEEPALIVE=false ./tests/load_test.sh   # e- = 关 keep-alive
BIN=./epoll SRC=epoll.cpp TAG=k- KEEPALIVE=true  ./tests/load_test.sh   # k- = 开 keep-alive
BIN=./p2    SRC=p2.cpp    TAG=p-                 ./tests/load_test.sh   # p- = p2 同日重跑

⚠ 所有数字的原始 .jtl 都在 tests/results/ 里，看前缀就知道是哪个配置。
   下面的数字【全部用 analyze.sh 同一把尺子】算出来（原因见文末"两把尺子"）。

压力测试结果（同机压测 → 绝对值仅供参考；每格 = 吞吐/s）：

并发×次数     e-（close）      k-（keep-alive）     p-（p2 阻塞版）
1×1000        1280             2915                 2020
10×200        2200             2278                 2235
50×200        2770（3 错）      10787（0 错）        3949（0 错）
100×200       4058（8 错）      16556（0 错）        4055（9 错）
200×200       —                16045（1 错）        —
400×200       —                22014（7 错）        —

k- 那一整轮的收尾观测：accept 连接数 = 761（= 1+10+50+100+200+400，每线程 1 条，
完美复用），TIME_WAIT 残留 = 0。

结论：
1. keep-alive 是最大的那个杠杆：50 并发 2770 → 10787（3.9×）、
   100 并发 4058 → 16556（4.1×），而且 50/100 两档【0 失败】
2. ★ epoll 本身（close 模式）并不比阻塞版 p2 快 —— 1 并发时反而慢 37%
   （1280 vs 2020）。差的正是 epoll 每条连接多出来的开销：
   fcntl×2 + epoll_ctl + 一次额外的 epoll_wait 往返。
   → epoll 的收益是"**同时握住很多连接**"，不是"单条处理更快"。
     没有 keep-alive 的话，epoll 在这个场景里是**负收益**。
3. 建连 p99：close 模式 1006ms（= SYN 重传退避）→ keep-alive 0ms。
   这是"连不上"那道墙消失的直接证据。
4. TIME_WAIT 13131 → 0；连接数 33000 → 761。
5. 墙的根因一直是 backlog=3，不是 CPU。keep-alive 把连接数压掉之后墙才退远
   （p2 那道墙是"每请求一条连接" × backlog=3 撞出来的）。
6. 剩下的墙还在 backlog=3：200/400 并发那几个错全发生在【爬坡瞬间】
   （几百条 SYN 同时到，等位区只有 3 个位置）
   → 下一步：调 listen(fd, backlog) 与 net.core.somaxconn

压测工具自己踩的三个坑（都已修，别再踩）：
1. analyze.sh：JMeter 的 failureMessage 里【带换行】→ 续行被当成新样本
   → $1+0=0 把 min 拉到 0 → 耗时算成 1.79e9 秒、吞吐列变 0、样本数 10019≠10000
   修法：4 处加 $1 ~ /^[0-9]+$/ 守卫
2. http_load.jmx：use_keepalive 写成 boolProp 时，-Jkeepalive=true 【静默失效】
   → 连跑两轮"关 / 开 keep-alive"，其实两轮都是 Connection: close，
     吞吐却差了 60% —— 差点被当成"keep-alive 有效"
   修法：改成 stringProp（实测：2000 请求 2000 条连接 → 10 条连接）
   验证方法：直接看服务器收到的请求头是 keep-alive 还是 close
   ⚠ 这次事故还留下 4 个**名不副实**的 ek-*.jtl（标着 keepalive=true，实际是
     close）—— 已删除。留着的话以后会拿它"证明"错误结论。
3. ★ 两把尺子：**JMeter 自己的 summary 吞吐 ≠ analyze.sh 算的吞吐**（分母不同）。
   实测同一个 L2_100x200.jtl：JMeter 报 4935.8/s，analyze.sh 算 4499/s。
   两者都不是"错"的，但**混着比就没意义**。
   → 本项目所有对比一律用 analyze.sh 的数字（README 里 p2 那张旧表用的是
     JMeter summary，所以它和 epoll 的表**不能直接相除**）。

未验证的猜想（留着以后验）：epoll.cpp 每个请求写 5 行日志到 stdout，
而 std::endl 会 flush —— 同步阻塞 IO，可能正压着吞吐上限
（尤其 srv.log 重定向到文件时，实测跑到 400 并发单文件能到 20MB+）。
要验证就造一个"关掉日志"的变体对比。
