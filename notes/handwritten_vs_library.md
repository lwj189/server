# 手写 vs 用库：cpp-httplib 对照

> 对应代码：`src/p2_lib.cpp`（完整实现 9 行）。
> 从 `src/p2_lib.cpp` 的文件头注释搬过来的 —— 代码里只留一句指路，细节放这里。

---

## 为什么有这一版

项目里有三个手写的服务器：

| 文件 | 行数 | 并发模型 |
| --- | --- | --- |
| `src/p2.cpp` | 335 | 阻塞串行 |
| `src/epoll.cpp` | 367 | 单线程 Reactor |
| `src/thread.cpp` | 492 | 单 Reactor + 线程池 |

`src/p2_lib.cpp` 用现成的 cpp-httplib 做同一个功能，**完整代码 9 行**。

放在一起跑同一套测试、同一个压测，才能回答一个真问题：
**手写 vs 用库，到底差在哪？**

- 代码量差 40 倍（一目了然）
- 协议正确性差多少？→ 跑 `tests/p2_test.py` 看
- 吞吐 / 延迟差多少？→ 跑 `tests/load_test.sh` 看
- 但**控制力**呢？→ 见下面「库拿走了什么」

---

## 库拿走了什么

（这正是前三版手写过的部分）

| 这一层 | 手写版 | cpp-httplib |
| --- | --- | --- |
| socket / bind / listen | `src/p2.cpp` 步骤 1~5 | 库内部 |
| accept 循环 | `src/p2.cpp` 步骤 6 | 库内部（线程池） |
| read 到完整请求 | 每连接缓冲区 + 循环 | 库内部 |
| `\r\n\r\n` 定界 | `findHeader` | 库内部 |
| Content-Length + 上限 | `MAX_HEADER/BODY_SIZE` | 库内部（有默认值） |
| 拼 HTTP 响应 | `buildResponse` | `res.set_content` |
| epoll 事件循环 | `src/epoll.cpp` 全部 | **无**（每连接一线程） |
| keep-alive | `want_close` + erase | 库内部 |
| EMFILE / 配平 / 哨兵 | 踩过的那些坑 | 库内部 |

### ⚠ 关键区别：cpp-httplib 不是 epoll

它用的是**每连接一个线程 + 阻塞 IO**（内部有个线程池，默认线程数 = CPU 核数）。

所以它**没有** `src/epoll.cpp` 里的那些东西：

- 没有 epoll 三件套
- 没有非阻塞 fd / EAGAIN
- 没有 `ClientContext`
- 没有 `EPOLL_CTL_DEL` / `ADD` 的交接

连接数一多，它就卡在"线程数"上 —— **这正是 epoll 存在的理由**。

---

## 上限：判定的**时机**比数值更关键

库的默认值：

| 宏 | 默认值 | 手写版 |
| --- | --- | --- |
| `CPPHTTPLIB_HEADER_MAX_LENGTH` | 8192 | 8KB（一致） |
| `CPPHTTPLIB_PAYLOAD_MAX_LENGTH` | **100 MB** | **1 MB**（宽了 100 倍） |

`src/p2_lib.cpp` 里用宏 + 运行时 `set_payload_max_length` 都对齐到了 1MB。
但**对齐数值之后，`p2_test` 的 [7]、[10] 两项仍然是红的**，因为差别不在数值，在时机：

| 场景 | 手写版 | cpp-httplib |
| --- | --- | --- |
| 声明 64MB，一个字节 body 都不发 | 解析头部时就看到 64MB > 1MB → **立刻拒绝** | 发完头就等着收 body，**不拒绝**（等读超时才断） |
| 声明 2MB，真发 2MB | 同上，立刻拒绝 | 真收满 2MB → **413** ✓ |
| 声明 1MB，发 1MB | 200 | 200 ✓（合法的照常过） |

> 实测（同一台机器）：声明 64MB + 只发 1MB → 库 **TIMEOUT**（还在等 body）。

**安全上的意义**：手写版是"声明大了就不收"，库是"收着收着发现大了才拒绝"。
前者更早止血，后者要等到数据真的来。

> ★ 用库省的是**代码**，不是**决策**。每个默认值背后都是一个契约 ——
> 你得知道它是什么、和你想要的一不一样、以及**它什么时候判**。

---

## SO_REUSEADDR 的坑

httplib 的 `default_socket_options()` 原文：

```cpp
#ifdef SO_REUSEPORT
    set_socket_opt(sock, SOL_SOCKET, SO_REUSEPORT, 1);
#else
    set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
```

Linux 上 `SO_REUSEPORT` 是**有定义**的 → 所以它只设了 REUSEPORT，**根本没设 REUSEADDR**。

两者不是一回事：

| 选项 | 作用 |
| --- | --- |
| `SO_REUSEADDR` | 允许 bind 一个**还有 TIME_WAIT 残留**的端口 ← 我们要的 |
| `SO_REUSEPORT` | 允许多个进程**同时监听**同一端口（多 Reactor 用） |

后果：端口上还有 TIME_WAIT 残留时 bind 失败，服务器直接起不来。
手写版（`p2` / `epoll` / `thread`）都显式设了 REUSEADDR，所以没这个问题。

---

## 当前测试结果

| 测试 | 结果 |
| --- | --- |
| `p2_test.py` | **21/23** ← t7（超长头）、t10（超大 body）红，**预期内**，原因见上 |
| `balance_test.py` | 4/4 |
| `keepalive_test.py` | 7/7 |
| `emfile_test.py` | 2/2 |

> [7] / [10] 红**不是 bug**，是"用库"这个对照实验的结论本身。
