# p2 —— 从零手写的 Linux TCP / HTTP 服务器

一个学习用的极简 HTTP 服务器：纯 C++17，不依赖任何框架，只用 Linux 系统调用和 C++ 标准库。
目的不是"造轮子"，而是把 `socket → bind → listen → accept → read → send → close` 这条链路**亲手写通、写对、压过**。


---

## 当前状态

```
CP1 socket/bind/listen ✅ → CP2 accept ✅ → CP3 read ✅ → CP4 send ✅
→ CP5 半包/粘包 ✅ → CP6a~d epoll / EMFILE / keep-alive ✅ → 线程池 ✅
```

`src/` 下是四个可执行目标。跑测试的三个（p2 / epoll / thread）在 CI 里全绿，thread_uev 只保证严格编译零警告（逻辑同 thread）：

| 文件 | 并发模型 | 真实代码行 |
| --- | --- | --- |
| `src/p2.cpp` | 阻塞串行（一次一条连接） | 172 |
| `src/epoll.cpp` | 单线程 Reactor | 194 |
| `src/thread.cpp` | 单 Reactor + 线程池 | 370 |
| `src/thread_uev.cpp` | 上者 + `signalfd` 统一事件源（含优雅停机） | 634 |

## 编译与运行

```bash
mkdir -p build        # 产物统一输出到 build/（已 gitignore；清理就是 rm -rf build）

# 四个手写版（严格编译：有警告就说明代码不干净）
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/p2.cpp         -o build/p2
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/epoll.cpp      -o build/epoll
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/thread.cpp     -o build/thread -pthread
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion src/thread_uev.cpp -o build/thread_uev -pthread

# 起一个（⚠ 都占 8888，同时只能跑一个）
./build/epoll                                  # 或 ./build/p2
./build/thread --workers 4                     # 线程池版
./build/thread_uev --workers 4                 # 线程池 + signalfd，Ctrl+C 会优雅停机
```

端口：默认 8888，写在各自源码顶部的 `constexpr uint16_t PORT`；`thread` / `thread_uev` 支持 `--port N`。

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

# 边界行为（自己起服务器：分片发送 / 半关闭 / 慢连接隔离 / 流水线部分写）
python3 tests/edge_test.py --bin build/epoll
python3 tests/edge_test.py --bin build/thread
python3 tests/edge_test.py --bin build/thread_uev
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

#### 实测数据（2026-11-04 重跑，当前 HEAD）

同机压测：4 vCPU i7-13700HX / 15 GiB / `127.0.0.1`，JMeter 5.6.3 与服务器抢 CPU。
完整口径、两轮区间、原始 `.jtl` 文件名见 **`notes/stress.md`**。只列关键档（未注明即 0 失败）：

| 场景 | 并发 | 吞吐 req/s | 说明 |
| --- | --- | --- | --- |
| p2（close，backlog 3） | 100 | 3,327 | 26/20,000 失败（ConnectTimeout） |
| epoll（close） | 100 | 8,234 | 0 失败；p2 → epoll 2.5× |
| epoll（keep-alive） | 100 | 12,399~12,995 | 比 close +51%~58% |
| epoll（keep-alive） | 400 | 16,149~16,992 | 80,000 请求 / 750 条连接 |
| thread 4 工人（keep-alive） | 400 | 20,351~20,964 | 同档比 epoll 高约 20% |
| thread `/work` 1 工人 | 4×500 | 83~84 | ~1.0 核 |
| thread `/work` 4 工人 | 4×500 | 197~215 | ~2.8 核；约 2.5× |

⚠ 同机压测时 JMeter 与服务器抢 4 个 vCPU，400 并发可能是客户端先饱和；这些数字用于**同环境对比**，
不要和旧数据或其他机器横比。旧记录 79→262（3.3×）在本机未复现，原因见 `notes/stress.md` 4.2 节。


## 已知限制（哪些已修、哪些还没修）

| # | 限制 | 后果 | 状态 |
| --- | --- | --- | --- |
| 1 | **一次 `read` ≠ 一个完整请求** | 客户端分片发送时，服务器会基于不完整的请求就响应 | ✅ CP5 已修（`inbuf` + 定界） |
| 2 | **阻塞式单连接** | 一个连上却不发数据的客户端就能卡住整个服务器 | ✅ CP6b 已修（非阻塞 + `epoll`） |
| 3 | `backlog = 3` | 爬坡瞬间（几百条 SYN 同时到）仍会丢连接 | 🔶 已确认：p2 100/200 并发 26/93 个 ConnectTimeout；epoll/thread（4096）到 400 并发 0 失败 |
| 4 | 不支持 keep-alive | 每个请求都要重新建连，`TIME_WAIT` 堆积 | ✅ CP6d-2 已修（`src/epoll.cpp`） |
| 5 | `send` 未处理部分写 | 响应很大 + 慢客户端时可能发不完整 | ✅ `thread.cpp` / `thread_uev.cpp` 已加全量校验（`EINTR` 重试；部分写一律按"收掉这条连接"处理）。真正支持要加输出缓冲 + `EPOLLOUT`，暂未做 |
| 6 | 没有优雅退出 | `SIGTERM` 直接死，在途请求丢 | ✅ `src/thread_uev.cpp` 已修（`signalfd` 统一事件源 + `drain()` 线程池 + 关空闲连接） |
| 7 | 没有日志系统 | 只有 `std::cout` + 一把锁 | ⬜ 后续 |
| 8 | **每个请求都写 5 行日志到 stdout** | 压测时这是**同步阻塞 IO**（`std::endl` 会 flush） | ⬜ 已验证不是瓶颈：thread 开日志 vs `--quiet` 差异 <5%（197~215 vs 201~211 req/s） |

## 目录结构

```
.
├── src/                        ← 源码（全部 C++17）
│   ├── p2.cpp                  阻塞版服务器（CP1~CP5）：一次只能服务一条连接，任何路径都回 Hello
│   ├── epoll.cpp               单线程 Reactor 版（CP6a~CP6d）：一个线程 + 一个 epoll
│   ├── thread.cpp              单 Reactor + 线程池版：主线程管 IO，工人算业务
│   ├── thread_uev.cpp          上者 + signalfd 统一事件源 + 优雅停机（单文件自包含）
│   ├── http.hpp                三个手写服务器【共用】的 HTTP 逻辑
│   │                           （ClientContext / 解析 / 拼响应 / 线程安全日志）
│   └── thread_pool.hpp         线程池类（只有一份，thread.cpp include 它）
├── build/                      ← 构建产物（已 gitignore；rm -rf build/ 就是彻底清理）
├── notes/                      ← 笔记（7 份入库）
├── tests/                      ← 测试（5 套 Python，其中 4 套进 CI；另有压测驱动）
├── .github/workflows/ci.yml    CI：严格编译 + 跑 4 套测试
├── .clang-format               代码风格约束（缩进 4 空格、指针贴左…）
├── .gitignore
└── README.md
```

> 📌 **产物全在 `build/`**：源码旁边永远是干净的。清理只需 `rm -rf build`。
> 重编：见下面「编译运行」。

**四个服务器怎么选**：

| 想干什么 | 用哪个 |
| --- | --- |
| 看最朴素的阻塞写法（对照用） | `build/p2` |
| 看事件驱动、跑协议/配平测试 | `build/epoll` |
| 看线程池怎么和 epoll 配合 | `build/thread --workers 4` |
| 看信号怎么进 epoll、怎么优雅停机 | `build/thread_uev --workers 4` |

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
- **`notes/loadtest_guide.md`** —— 压测**方法**：测什么 / 六种类型 / 七条方法论 / 怎么看 `.jtl` / 同时观察什么
- **`notes/stress.md`** —— 2026-11-04 重跑的**数据**：口径 / p2 / epoll / thread / 结论（和上一份的方法配套）

## 说明

- 这是个人学习项目，代码追求"每一行都能解释为什么"，不追求生产可用。
