# p2 —— 从零手写的 Linux TCP / HTTP 服务器

一个学习用的极简 HTTP 服务器：纯 C++17，不依赖任何框架，只用 Linux 系统调用和 C++ 标准库。
目的不是"造轮子"，而是把 `socket → bind → listen → accept → read → send → close` 这条链路**亲手写通、写对、压过**。

> 我是正在学服务器方向的学生。这个仓库记录我手写 `src/p2.cpp` 的全过程：
> 每一步为什么这么写、踩过哪些坑、以及实测数据。

---

## 当前状态

```
CP1 socket/bind/listen ✅ → CP2 accept ✅ → CP3 read ✅ → CP4 send ✅
→ CP5 半包/粘包 ✅ → CP6a~d epoll / EMFILE / keep-alive ✅ → 线程池 ✅
```

`src/` 下是七个可执行目标。跑测试的三个（p2 / epoll / thread）在 CI 里全绿，其余四个只保证严格编译零警告：

| 文件 | 并发模型 | 真实代码行 |
| --- | --- | --- |
| `src/p2.cpp` | 阻塞串行（一次一条连接） | 172 |
| `src/p2_route.cpp` | 上者 + 请求行解析 + 路由（实验分支，不进测试矩阵） | 258 |
| `src/epoll.cpp` | 单线程 Reactor | 194 |
| `src/thread.cpp` | 单 Reactor + 线程池 | 370 |
| `src/thread_uev.cpp` | 上者 + `signalfd` 统一事件源（含优雅停机） | 634 |
| `src/p2_lib.cpp` | cpp-httplib（库，每连接一线程） | 45 |
| `src/thread_pool.cpp` | 线程池独立演示（不接服务器） | 15 |

## 编译与运行

```bash
mkdir -p build        # 产物统一输出到 build/（已 gitignore；清理就是 rm -rf build）

# 五个手写版（严格编译：有警告就说明代码不干净）
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/p2.cpp        -o build/p2
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/p2_route.cpp  -o build/p2_route
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/epoll.cpp     -o build/epoll
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/thread.cpp    -o build/thread -pthread
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/thread_uev.cpp -o build/thread_uev -pthread

# 线程池独立演示（不接服务器，单看池子怎么派活）
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/thread_pool.cpp -o build/thread_pool -pthread

# 库版（-I 指向第三方头文件；第一次编译约 8 秒，模板实例化）
g++ -std=c++17 -O2 src/p2_lib.cpp -o build/p2_lib -pthread -I third_party/cpp-httplib

# 起一个（⚠ 都占 8888，同时只能跑一个）
./build/epoll                                  # 或 ./build/p2
./build/thread --workers 4                     # 线程池版
./build/thread_uev --workers 4                 # 线程池 + signalfd，Ctrl+C 会优雅停机
./build/p2_lib                                 # cpp-httplib 版（支持 --port N）
```

端口：手写版在各自源码顶部的 `constexpr uint16_t PORT`；库版用 `--port N`。

另一个终端里访问：

```bash
curl -v http://127.0.0.1:8888/          # 看完整的请求/响应往返
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8888/   # 只打印状态码 → 200
# 或者直接用浏览器打开 http://127.0.0.1:8888/
```

## 测试

### 冒烟 + 健壮性测试（无需额外依赖）

```bash
# 终端1：启动服务器
build/p2

# 终端2：
python3 tests/p2_test.py                 # 默认 127.0.0.1:8888
python3 tests/p2_test.py --port 9999
```

覆盖 5 个用例：

| # | 用例 | 断言 |
| --- | --- | --- |
| 1 | 正常请求 | 200 + 响应体正确 + `Content-Length` 与实际字节数一致 |
| 2 | **连上就关**（端口扫描 / 健康检查） | 服务器必须活下来并继续服务 |
| 3 | **RST 强断**（发一半请求后 `SO_LINGER=0`） | 服务器必须活下来并继续服务 |
| 4 | 并发 20 | 全部返回 200 |
| 5 | 半包观察（分两段发送） | 只记录现象，不计入失败（CP5 才修） |

用例 2/3 是回归测试：**单个坏客户端不该杀死服务器**（对应"响应只在 `n > 0` 时发送" + 忽略 `SIGPIPE`）。

### 压力测试（JMeter）

```bash
./tests/load_test.sh                                  # 默认阶梯 1 / 10 / 50 / 100 并发
./tests/load_test.sh 200 500                          # 200 并发 × 500 次
LEVELS="1:500 20:300 100:200" ./tests/load_test.sh    # 自定义阶梯
PORT=9999 ./tests/load_test.sh                        # 换端口
```

`load_test.sh` 会严格编译（有警告就停）、启动服务器、逐档跑 JMeter，再调用 `analyze.sh` 出分析表，结果落在 `tests/results/`。

#### 实测数据

被测版本：`src/p2.cpp`（单线程阻塞 + `backlog=3`）
环境：4 核 VM / Ubuntu；`somaxconn=4096`，`ulimit -n=524288`
工具：JMeter 5.6.3（**同机压测，绝对值仅供参考**）
场景：`GET /` + `Connection: close`（每个请求新建连接）

⚠ 下表用的是 **JMeter 自己 summary 里的吞吐**；而下面 epoll 那张表用的是
**`tests/analyze.sh` 算的吞吐** —— 两者**分母不同**（同一个 `.jtl`：4935.8/s vs 4499/s）。
所以**这张表和下面那张不能直接相除**。要对比请看 `notes/stress.md` 第 2 节里
用同一把尺子重算过的版本。

| 并发 | 请求数 | 吞吐 | 错误 |
| --- | --- | --- | --- |
| 1 | 1000 | 1811.6 /s | 0 (0.00%) |
| 10 | 2000 | 2034.6 /s | 0 (0.00%) |
| 50 | 10000 | 4284.5 /s | 0 (0.00%) |
| 100 | 20000 | 4935.8 /s | 2 (0.01%) |
| 200 | 40000 | 7900.5 /s | 38 (0.10%) |

现象：`Recv-Q` 顶到 3~4；2 个 ConnectTimeout；`TIME_WAIT` 累积到 13119。

**结论（这部分比数字重要）**：

1. **瓶颈完全不在请求处理**——p50 = 0.4 ms、p90 = 1 ms，处理一个请求几乎是零成本。
2. **失败率随并发暴涨，拐点在 100 ~ 200 并发之间**（0 → 0 → 0 → 2 → 38）。
3. 失败类型**全是 `ConnectTimeoutException`**：不是"处理不过来"，而是**连都连不上**。
4. 建连 p99 = 1001 ms、max = 2003 ms —— 这是 **TCP SYN 重传退避（1s → 2s）** 的铁证。
   根因：`listen(fd, 3)` 的 **accept 队列被填满**，内核开始丢弃新来的 SYN。

→ 也就是说：单线程阻塞 + `backlog=3` 的服务器，**被压垮的不是 CPU，而是那个只有 3 个位置的等位区**。

#### epoll 版实测数据（2026-10-04）

被测版本：`src/epoll.cpp`（单线程事件驱动 + CP6d-2 的 keep-alive）。

> 📌 **完整数据在 `notes/stress.md` 第 2 节**：逐档对照表 / 收尾观测（连接数 **761**、
> `TIME_WAIT` **0**）/ 六条结论 / 压测工具自身的三个坑。
> **这里只留头条** —— 同一张表不写两处，改一处忘另一处早晚不一致。

```bash
BIN=build/epoll SRC=src/epoll.cpp TAG=e- KEEPALIVE=false ./tests/load_test.sh   # 关 keep-alive
BIN=build/epoll SRC=src/epoll.cpp TAG=k- KEEPALIVE=true  ./tests/load_test.sh   # 开 keep-alive
```

| 同机压测，同一把尺子（`analyze.sh`） | 50 并发 | 100 并发 |
| --- | --- | --- |
| epoll + `Connection: close` | 2770 /s（3 错） | 4058 /s（8 错） |
| epoll + **keep-alive** | **10787 /s**（0 错） | **16556 /s**（0 错） |
| p2（阻塞版，同日重跑） | 3949 /s（0 错） | 4055 /s（9 错） |

**一句话结论**：真正的杠杆是 **keep-alive**（50 并发 **3.9×**、100 并发 **4.1×**，且 0 失败）；
而 **epoll 本身在"每请求一条连接"模式下并不比阻塞版快**（1 并发时反而慢 **37%**）——
它的收益是"**同时握住很多连接**"，不是"单条处理更快"。
剩下的墙仍是 `backlog=3`（失败全在爬坡瞬间）→ 下一步调 `listen(fd, backlog)`。

⚠ 上面那张 p2 的旧表用的是 **JMeter 自己 summary 的吞吐**，和这张表的 `analyze.sh`
吞吐**分母不同**（同一个 `.jtl`：4935.8/s vs 4499/s）—— **两张表不能直接相除**。

## 已知限制（哪些已修、哪些还没修）

| # | 限制 | 后果 | 状态 |
| --- | --- | --- | --- |
| 1 | **一次 `read` ≠ 一个完整请求** | 客户端分片发送时，服务器会基于不完整的请求就响应 | ✅ CP5 已修（`inbuf` + 定界） |
| 2 | **阻塞式单连接** | 一个连上却不发数据的客户端就能卡住整个服务器 | ✅ CP6b 已修（非阻塞 + `epoll`） |
| 3 | `backlog = 3` | 爬坡瞬间（几百条 SYN 同时到）仍会丢连接 | 🔶 keep-alive 之后**只剩这一处**，见上面压测结论 ③ |
| 4 | 不支持 keep-alive | 每个请求都要重新建连，`TIME_WAIT` 堆积 | ✅ CP6d-2 已修（`src/epoll.cpp`；2.7× 吞吐） |
| 5 | `send` 未处理部分写 | 响应很大 + 慢客户端时可能发不完整 | ✅ `thread.cpp` / `thread_uev.cpp` 已加全量校验（`EINTR` 重试；部分写一律按"收掉这条连接"处理）。真正支持要加输出缓冲 + `EPOLLOUT`，暂未做 |
| 6 | 没有优雅退出 | `SIGTERM` 直接死，在途请求丢 | ✅ `src/thread_uev.cpp` 已修（`signalfd` 统一事件源 + `drain()` 线程池 + 关空闲连接） |
| 7 | 没有日志系统 | 只有 `std::cout` + 一把锁 | ⬜ 后续 |
| 8 | **每个请求都写 5 行日志到 stdout** | 压测时这是**同步阻塞 IO**（`std::endl` 会 flush） | ⬜ 未验证的猜想：它可能正在压着吞吐上限。要验证就造一个"关掉日志"的变体对比 |

## 目录结构

```
.
├── src/                        ← 源码（全部 C++17）
│   ├── p2.cpp                  阻塞版服务器（CP1~CP5）：一次只能服务一条连接，任何路径都回 Hello
│   ├── p2_route.cpp            上者 + 请求行解析 + 路由（404/405/400、?query），独立分支不进 CI 测试
│   ├── epoll.cpp               单线程 Reactor 版（CP6a~CP6d）：一个线程 + 一个 epoll
│   ├── thread.cpp              单 Reactor + 线程池版：主线程管 IO，工人算业务
│   ├── thread_uev.cpp          上者 + signalfd 统一事件源 + 优雅停机（单文件自包含）
│   ├── p2_lib.cpp          cpp-httplib 对照版（真实代码 45 行）
│   ├── http.hpp                三个手写服务器【共用】的 HTTP 逻辑
│   │                           （ClientContext / 解析 / 拼响应 / 线程安全日志）
│   ├── thread_pool.hpp         线程池类（只有一份，两个文件 include 它）
│   └── thread_pool.cpp         线程池独立演示（不接服务器，10 个假任务看怎么分）
├── third_party/
│   └── cpp-httplib/
│       └── httplib.h           第三方单头文件（23,368 行，vendored 进仓库）
├── build/                      ← 构建产物（已 gitignore；rm -rf build/ 就是彻底清理）
├── notes/                      ← 笔记（8 份入库，另有 1 份 AI 交接文档不入库）
├── tests/                      ← 测试（4 套 Python + 压测驱动）
├── .github/workflows/ci.yml    CI：严格编译 + 跑 4 套测试
├── .clang-format               代码风格约束（缩进 4 空格、指针贴左…）
├── .gitignore
└── README.md
```

> 📌 **产物全在 `build/`**：源码旁边永远是干净的。清理只需 `rm -rf build`。
> 重编：见下面「编译运行」。

**三个服务器怎么选**：

| 想干什么 | 用哪个 |
| --- | --- |
| 看最朴素的阻塞写法（对照用） | `build/p2` |
| 看事件驱动、跑协议/配平测试 | `build/epoll` |
| 看线程池怎么和 epoll 配合 | `build/thread --workers 4` |
| 看信号怎么进 epoll、怎么优雅停机 | `build/thread_uev --workers 4` |
| 单独把线程池跑起来看 | `build/thread_pool` |

## 学习笔记索引

每份笔记**只有一个职责**，同一件事不会写在两个地方：

- **`notes/HANDOFF.md`** —— 新对话读的第一份：我是谁 / 怎么教我 / 项目现状 / 下一步
- **`notes/pitfalls.md`** —— **全部踩过的坑**（CP1~CP6d）+ 元教训。每条包含现象、根因、修法、验证方式。例如：
  - `bool reuse` 让 `setsockopt` 返回 `EINVAL`（内核要求 `optlen >= sizeof(int)`）
  - `EADDRINUSE` 的两种成因（TIME_WAIT vs 有活进程），以及为什么 `SO_REUSEADDR` 只救得了前者
  - `EMFILE`（本进程 fd 用光）vs `ENFILE`（整机用光）—— 填错的表现是"空转照旧"
- **`notes/learning_progress.md`** —— 进度、方法论、已掌握的概念清单
- **`notes/epoll_notes.md`** —— epoll 技术专题
- **`notes/selfcheck.md`** —— 自检 69 题（先自己答，再往下翻答案）
- **`notes/stress.md`** —— JMeter 压测**数字**：第 1 节 `p2`（阻塞版）、第 2 节 `epoll`（keep-alive），含压测工具自身的两个坑
- **`notes/loadtest_guide.md`** —— 压测**方法**：测什么 / 六种类型 / 七条方法论 / 怎么看 `.jtl` / 同时观察什么

## 说明

- 这是个人学习项目，代码追求"每一行都能解释为什么"，不追求生产可用。
- 参考用的 AI 生成实现（`day2.cpp` ~ `day6.cpp`，逐级递进到 epoll / 线程池 / MySQL 连接池）在本仓库**之外**的目录里，不属于本仓库内容。
