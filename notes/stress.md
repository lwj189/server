# 压测报告：p2 / epoll / thread（2026-11-04）

> 对象：`src/p2.cpp`（阻塞）、`src/epoll.cpp`（单 Reactor）、`src/thread.cpp`（单 Reactor + 线程池）。
> 所有数字来自当前 HEAD 在本机的重跑；原始证据是 `tests/results/*.jtl`（该目录被 .gitignore 忽略，只在本机）。
> 统计只用 `tests/analyze.sh` 这一把尺子，不混用 JMeter 自己的 summary。
> 旧的 2026-10-04 数据已删除（旧代码、线程池没有原始 .jtl）；本报告会标注哪些旧说法**没有复现**。

## 结论速览

- **keep-alive 是最大收益**：epoll 100 并发 8,234 → 12,399~12,995 req/s（**+51%~58%**），80,000 请求只开 **750~761** 条连接。
- **backlog=3 是 p2 的硬伤**：100/200 并发分别 **26 / 93** 个 `ConnectTimeoutException`；epoll/thread（4096）到 400 并发 **0 失败**。
- **400 并发**：epoll 16,149~16,992 req/s；thread 20,351~20,964 req/s（高约 20%）。
- **CPU 密集 `/work`**：1 工人 83~84 req/s（~1.0 核）→ 4 工人 197~215（~2.8 核），约 **2.5×**；旧 3.3× 未复现。
- **日志不是瓶颈**：thread 开日志 vs `--quiet` 差异 <5%；但 epoll 没有 `/work` 路由（任何路径都回 Hello）。

## 1. 口径（先看这一节，否则数字没有意义）

| 项 | 值 |
| --- | --- |
| 服务器 | 本机 VM：4 vCPU（i7-13700HX，每核 1 线程）、15 GiB RAM、kernel 7.0.0-42-generic |
| 网络 | 客户端与服务器**同一台 VM**，`127.0.0.1` 回环；不是"客户端分离"的干净测量 |
| 服务器 backlog | p2 = 3；epoll / thread = 4096（`SOMAXCONN`） |
| ulimit -n | 524288（服务器进程） |
| 压测工具 | Apache JMeter 5.6.3，CLI（`-n`），Java 21.0.12.1，默认堆 `-Xms1g -Xmx1g` |
| 测试计划 | `tests/http_load.jmx`；HttpClient4；rampup 1 s；每档固定 loops；keepalive 是 stringProp（已验证生效） |
| 被压路径 | `/`（固定 34 B body，整条响应 117 B）；`/work?n=2000000`（body 78 B，整条响应 161 B） |
| 判定 | JMeter 断言 HTTP code == 200；失败率 0 才算干净 |
| 结果统计 | `tests/analyze.sh`；"耗时"是 .jtl 首尾时间差，"建连p99"是 Connect Time |
| 运行次数 | keepalive 两组各 2 次（报告区间）；p2 / epoll close 各 1 次；/work 重点档 2 次 |

⚠ 三个容易误读的点：
1. **同机压测**：JMeter 和服务器抢同样的 4 个 vCPU。400 并发时客户端可能先饱和，吞吐数字是"本机端到端"的下限，不是服务器单机上限。
2. **p2 高并发档的吞吐不要用**：连接失败（ConnectTimeout）耗时很短，会把"吞吐/s"抬高；看 p2 只看失败率和低并发档。
3. **epoll 没有 /work 路由**：它对任何路径都回同一个 34 B Hello（实测 `/work?n=1000` 也是 200 + Hello）。所以 CPU 密集场景只能拿 thread 的 1 工人 vs 4 工人比，不能拿 epoll 当基线。

## 2. p2（阻塞串行，backlog = 3，Connection: close）

| 并发 × 次数 | 吞吐 req/s | 失败 | p99 ms | 最大 ms | 建连 p99 |
| --- | --- | --- | --- | --- | --- |
| 1 × 1000 | 1,089 | 0 | 5 | 30 | 3 |
| 10 × 200 | 2,307 | 0 | 4 | 25 | 2 |
| 50 × 200 | 2,669 | 1（0.01%） | 7 | 2,002 | 3 |
| 100 × 200 | 3,327 | 26（0.13%） | 1,023 | 2,004 | 1,014 |
| 200 × 200 | 5,990（虚高，见口径） | 93（0.23%） | 1,023 | 2,688 | 1,008 |

- 失败全是 `ConnectTimeoutException`：SYN 在 backlog=3 的队列里被丢，客户端等不到握手。
- 收尾：accept 72,880，TIME_WAIT 14,071（60 s 窗口内的残留）。
- 结果文件：`tests/results/p-t*.jtl`。

## 3. epoll（单 Reactor，backlog = 4096）

### 3.1 Connection: close

| 并发 × 次数 | 吞吐 req/s | 失败 | p99 ms | 最大 ms |
| --- | --- | --- | --- | --- |
| 1 × 1000 | 1,779 | 0 | 1 | 22 |
| 10 × 200 | 2,165 | 0 | 4 | 20 |
| 50 × 200 | 6,592 | 0 | 25 | 57 |
| 100 × 200 | 8,234 | 0 | 35 | 161 |

- 收尾：accept 33,000，TIME_WAIT 14,114。
- 对比 p2 100 并发：3,327（26 失败）→ 8,234（0 失败），吞吐 2.5×，失败消失。
- 结果文件：`tests/results/e-t*.jtl`。

### 3.2 keep-alive（Connection: keep-alive，两轮）

| 并发 × 次数 | 第 1 轮 req/s | 第 2 轮 req/s | p99（两轮） | 失败 |
| --- | --- | --- | --- | --- |
| 50 × 200 | 9,208 | 9,390 | 8 / 10 | 0 |
| 100 × 200 | 12,995 | 12,399 | 14 / 20 | 0 |
| 200 × 200 | 13,527 | 13,671 | 24 / 34 | 0 |
| 400 × 200 | 16,992 | 16,149 | 55 / 46 | 0 |

- 100 并发：close 8,234 → keep-alive 12,399~12,995（**+51%~58%**）。
- 400 并发 80,000 请求只开了 **750~761** 条 TCP 连接（srv.log 的"新连接"计数）；close 模式是 33,000 请求 / 33,000 条连接。
- 收尾 TIME_WAIT 754 / 750（这是上一轮 close 测试 60 s 窗口的残留，不是本轮产生的）。
- 结果文件：`tests/results/k-t*.jtl`、`e2k-t*.jtl`。

## 4. thread（单 Reactor + 4 工人，backlog = 4096）

### 4.1 普通路由 `/`，keep-alive（两轮）

| 并发 × 次数 | 第 1 轮 req/s | 第 2 轮 req/s | p99（两轮） | 失败 |
| --- | --- | --- | --- | --- |
| 50 × 200 | 9,174 | 9,302 | 7 / 8 | 0 |
| 100 × 200 | 11,142 | 13,803 | 21 / 21 | 0 |
| 200 × 200 | 16,736 | 17,786 | 24 / 18 | 0 |
| 400 × 200 | 20,351 | 20,964 | 26 / 43 | 0 |

- 收尾：400 并发 80,000 请求 / 750 条连接，0 失败。
- 与 epoll keep-alive 对比：
  - 100 并发：12.4~13.0k（epoll） vs 11.1~13.8k（thread），基本持平（受噪声影响）；
  - 200 并发：13.6~13.7k vs 16.7~17.8k；
  - 400 并发：16.1~17.0k vs 20.4~21.0k（thread 高 **约 20%**）。
- 为什么线程版反而更快：解析请求、拼响应、写日志这些 CPU 活被分到 4 个工人线程，Reactor 只做 accept/read/write；同机 4 核正好吃满。**不要把它说成"线程池没有代价"**——同机客户端也在抢 CPU，换到客户端分离的环境数字会变。
- 结果文件：`tests/results/t4k-t*.jtl`、`t2k-t*.jtl`。

### 4.2 CPU 密集路由 `/work?n=2000000`，keep-alive

| 配置 | 并发 × 次数 | 吞吐 req/s | 平均 ms | p99 ms | 服务器 CPU |
| --- | --- | --- | --- | --- | --- |
| 1 工人（--quiet） | 2 × 200 | 82 | 21.9 | 27 | ~1.0 核 |
| 1 工人（--quiet） | 4 × 500 | 84 | 46.4 | 51 | ~1.0 核（4×400 档实测 100%） |
| 4 工人（--quiet） | 2 × 200 | 114 | 14.9 | 25 | — |
| 4 工人（--quiet） | 4 × 500 | 211 / 201（两轮） | 17.6 / 18.2 | 32 / 33 | ~2.8 核（4×800 档实测 270~307%） |
| 1 工人（开日志） | 4 × 500 | 83 | 46.8 | 54 | — |
| 4 工人（开日志） | 4 × 500 | 197 | 18.6 | 36 | — |
| 4 工人（开日志） | 8 × 500 | 227 | 33.7 | 54 | — |

- **同机结论**：1 工人 83~84 req/s（1 核）→ 4 工人 197~215 req/s（约 2.8 核），**2.4~2.6×**。
- 旧记录是 79 → 262（3.3×）；这次**没有复现**。最可能的原因：旧数据可能是 JMeter 在另一台机器上压的（服务器能拿满 4 核），而这次客户端和服务器抢 4 个 vCPU，服务器只到 2.8 核。写简历/笔记只能用这次可复现的 2.5×，或者注明"客户端分离时 3.3×"。
- 开日志 vs `--quiet` 在 4 工人 4×500 档：197~215 vs 201~211，差异在噪声内 → **日志不是这个场景的瓶颈**（但 srv.log 会很大，压测脚本默认删）。
- 结果文件：`tests/results/w1-*.jtl`、`w4-*.jtl`、`q1-*.jtl`、`q4-*.jtl`、`q4b-*.jtl`。

## 5. 这一轮确认/推翻的结论

1. **keep-alive 的收益是实的**：epoll 100 并发 +51%~58%，80,000 请求只开 750 条连接；不是"数字好看"，连接数和 TIME_WAIT 都对得上。
2. **backlog=3 是 p2 的硬伤**：100/200 并发分别 26 / 93 个 ConnectTimeout；epoll/thread 到 400 并发 0 失败。
3. **线程池在普通路由上不是"有代价"**：100 并发持平，400 并发高约 20%；原因见 4.1。旧笔记里"+0.7%"的说法不要再当普遍结论。
4. **CPU 密集场景线程池收益可复现，但倍数是 2.5× 不是 3.3×**（同机压测口径下）。
5. **日志假设被推翻**：关日志对吞吐影响 <5%，瓶颈在 CPU/客户端，不在 5 行日志（但日志文件仍要清）。
6. **epoll 不支持 /work**：它对所有路径都回 Hello；CPU 密集对比只能用 thread 的工人数做自变量。

## 6. 这些数字怎么用（简历 / 面试）

**可以讲（本次实测，能拿出 .jtl）**：
- epoll keep-alive 在同机 100 并发下 8,234 → 12,399~12,995 req/s（+51%~58%），80,000 请求只开 750 条连接；close 模式 33,000 请求就是 33,000 条连接。
- backlog=3 时 p2 在 100/200 并发出现 26/93 个 ConnectTimeout；epoll/thread 用 4096 到 400 并发 0 失败。
- `/work?n=2000000` 从 1 工人 83~84 req/s（~1.0 核）到 4 工人 197~215 req/s（~2.8 核），约 2.5×。

**不要讲（本次没复现 / 旧数据已删）**：
- “线程池 79 → 262，3.3×”：同机只测到 2.4~2.6×；3.3× 可能是客户端分离时的数字，要用必须重测。
- “线程池在普通路由只有 +0.7% 代价”：本次 400 并发线程版反而高约 20%，但同机客户端在抢 CPU，不能当普遍结论。
- “峰值 12,121 req/s”：旧数据，已删除；本次同机峰值是 thread 400 并发的 20,351~20,964 req/s（同样受客户端影响）。

**被追问时要能答**（不能白板解释就先别写进简历）：
- keep-alive 为什么快：省掉每次请求的 TCP 三次握手/四次挥手，服务器端 TIME_WAIT 和 fd 创建都少。
- backlog 为什么丢连接：SYN 队列满时内核丢 SYN，客户端重试到超时；错误类型是 `ConnectTimeoutException`，不是 HTTP 500。
- `/work` 为什么线程池有收益：业务是 CPU 密集型，留在 Reactor 线程会把 accept/read 全堵住；分给工人后 4 核并行。
- 为什么 epoll 不能测 `/work`：它没有这个路由，任何路径都回同一个 Hello；CPU 密集对比只能用 thread 的工人数做自变量。

## 7. 复现命令

```bash
# p2（close）
LEVELS="1:1000 10:200 50:200 100:200 200:200" \
  BIN=build/p2 SRC=src/p2.cpp TAG=p- KEEPALIVE=false ./tests/load_test.sh

# epoll（close / keep-alive 两轮）
LEVELS="1:1000 10:200 50:200 100:200" \
  BIN=build/epoll SRC=src/epoll.cpp TAG=e- KEEPALIVE=false ./tests/load_test.sh
LEVELS="50:200 100:200 200:200 400:200" \
  BIN=build/epoll SRC=src/epoll.cpp TAG=k- KEEPALIVE=true ./tests/load_test.sh

# thread（普通路由，4 工人）
SRV_ARGS="--workers 4" BIN=build/thread SRC=src/thread.cpp TAG=t4k- KEEPALIVE=true \
  LEVELS="50:200 100:200 200:200 400:200" ./tests/load_test.sh

# thread（CPU 密集，比较 1 工人 vs 4 工人；--quiet 减少日志干扰）
SRV_ARGS="--workers 1 --quiet" BIN=build/thread SRC=src/thread.cpp TAG=q1- \
  KEEPALIVE=true HTTP_PATH="/work?n=2000000" LEVELS="2:200 4:500" ./tests/load_test.sh
SRV_ARGS="--workers 4 --quiet" BIN=build/thread SRC=src/thread.cpp TAG=q4- \
  KEEPALIVE=true HTTP_PATH="/work?n=2000000" LEVELS="2:200 4:500" ./tests/load_test.sh
```

`tests/load_test.sh` 在 2026-11-04 增加了两个环境变量：`SRV_ARGS`（服务器启动参数，如 `--workers 4`）和 `HTTP_PATH`（被压路径，如 `/work?n=2000000`）；默认值不变（空 / `/`）。

## 8. 边界测试与还没测的

### 8.1 已补测（2026-11-04）

新增 `tests/edge_test.py`，四个探针在 epoll / thread / thread_uev 上各 6 项全过：

| 探针 | 做什么 | 结果 |
| --- | --- | --- |
| 分片发送 | 请求分两次发（前 20B，等 0.2s 再发其余） | 三台都返回 200 |
| 半关闭 | 发完请求后 `shutdown(SHUT_WR)` | 三台都收到 200，随后 EOF |
| 慢连接隔离 | 一条只发半个请求头挂着，另一条请求 `/` | 全部通过；thread_uev 首字节 0.3ms、epoll 0.8ms |
| 流水线部分写 | 一次灌 30,000 个请求、不读响应 | thread/thread_uev 收掉旧连接；epoll 修复后同样收掉；新连接均正常 |

- **epoll 部分写修复**：`src/epoll.cpp` 的发送改为循环 + `EINTR` 重试；`send` 部分写或失败 → `want_close=true`，由调用方统一 `close` + 销账。没有 outbuf/EPOLLOUT，策略与 thread 一致（一次发不完就收连接）。
- 回归：epoll `p2_test` 23/23、`keepalive_test` 7/7、`emfile_test` 2/2。
- 复现：`python3 tests/edge_test.py --bin build/epoll`（thread / thread_uev 把 `--bin` 换掉即可）。

### 8.2 还没测的 / 不能当结论的

- **客户端分离**（Windows 或另一台 VM 跑 JMeter）——唯一能把 3.3× 找回来、并确认 400 并发是否客户端饱和的办法；仍未做。
- **thread_uev 的 close / `/work` 压测**：keepalive 已补（100/200/400 并发 = 12,422 / 16,000 / 19,517 req/s，0 失败）；close 与 `/work` 只跑了同通信路径的 thread，thread_uev 未单独跑。
- **close 模式 200/400**：epoll 跑过，200×200 = 6,792/s、94 失败；400×200 = 4,755/s、1,012 失败（ConnectTimeout 为主）。同机 JMeter 与服务器抢 4 vCPU，不能当服务器容量结论，需客户端分离后重测。
- **p2 的慢连接/部分写**：p2 是阻塞单线程基线，没有输出缓冲；慢客户端预期会卡住整个进程，本轮没有把它做成正式测试（expected fail，不是回归）。
