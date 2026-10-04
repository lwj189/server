# p2 —— 从零手写的 Linux TCP / HTTP 服务器

一个学习用的极简 HTTP 服务器：纯 C++17，不依赖任何框架，只用 Linux 系统调用和 C++ 标准库。
目的不是"造轮子"，而是把 `socket → bind → listen → accept → read → send → close` 这条链路**亲手写通、写对、压过**。

> 我是正在学服务器方向的学生。这个仓库记录我手写 `p2.cpp` 的全过程：
> 每一步为什么这么写、踩过哪些坑、以及实测数据。

---

## 当前状态

```
CP1 socket/bind/listen ✅ → CP2 accept ✅ → CP3 read ✅ → CP4 send ✅ → CP5 半包处理 ⬜
```

**已经是一个能用的服务器**：`curl` 和浏览器都能访问，返回一个固定的 HTML 页面。

## 编译与运行

```bash
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion p2.cpp -o p2 && ./p2
# 输出：listening on port 8888 ...
```

监听端口在 `p2.cpp` 顶部的 `constexpr uint16_t PORT` 定义。运行后它一直阻塞等待连接，`Ctrl+C` 退出。

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
./p2

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

被测版本：`p2.cpp`（单线程阻塞 + `backlog=3`）
环境：4 核 VM / Ubuntu；`somaxconn=4096`，`ulimit -n=524288`
工具：JMeter 5.6.3（**同机压测，绝对值仅供参考**）
场景：`GET /` + `Connection: close`（每个请求新建连接）

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

被测版本：`epoll.cpp`（单线程事件驱动 + CP6d-2 的 keep-alive）。
**完整数据（A/B 对照、阶梯、工具坑）见 `notes/stress.md` 第 2 节**，这里只留摘要。

```bash
BIN=./epoll SRC=epoll.cpp TAG=e- KEEPALIVE=false ./tests/load_test.sh   # 关 keep-alive
BIN=./epoll SRC=epoll.cpp TAG=k- KEEPALIVE=true  ./tests/load_test.sh   # 开 keep-alive
```

| 场景（同机压测，绝对值仅供参考） | 吞吐 | 失败 |
| --- | --- | --- |
| epoll，50 并发，`Connection: close` | ~3690 /s | 0 |
| epoll，50 并发，**keep-alive** | **~10073 /s**（2.7×，连接数 10000 → **50**） | 0 |
| epoll，100 并发，keep-alive | 11947 /s | 0 |
| epoll，200 并发，keep-alive | 14875 /s | 3 (0.01%) |
| epoll，400 并发，keep-alive | 17305 /s | 21 (0.03%) |
| p2（对比），100 并发 | 4935.8 /s | 2 (0.01%) |
| p2（对比），200 并发 | 7900.5 /s | 38 (0.10%) |

**结论（这部分比数字重要）**

1. **keep-alive 是最大的那个杠杆**：连接数 10000 → 50，吞吐 2.7×。
   epoll 的收益**不是**"处理请求更快"（p50 一直零点几毫秒），
   而是"**一个线程同时握住很多连接**"—— 没有这一点，keep-alive 根本无从谈起。
2. **`建连 p99` 从 1001 ms 掉到 0 ms** —— p2 那道"连不上"的墙消失了
   （1001 ms = TCP SYN 重传退避，见上一节）。
3. **墙的根因一直是 `backlog=3`，不是 CPU**：p2 是被"每请求一条连接"撞出来的；
   keep-alive 把连接数压掉，墙才退远。剩下的失败（200 / 400 并发那几个）
   **全部发生在爬坡瞬间** → 下一步该调 `listen(fd, backlog)` 与 `net.core.somaxconn`。

> ⚠ **压测工具自己踩过两个坑**（都已在本次修复，详见 `notes/stress.md`）：
> ① `analyze.sh` 会把 JMeter 多行 `failureMessage` 的续行当成样本
> → 吞吐列直接变 **0**；② `http_load.jmx` 的 `use_keepalive` 写成 `boolProp` 时
> `-Jkeepalive=true` **静默失效** → 连跑两轮其实测的是同一个配置。

## 已知限制（都还没修，正是后面的学习内容）

| # | 限制 | 后果 | 状态 |
| --- | --- | --- | --- |
| 1 | **一次 `read` ≠ 一个完整请求** | 客户端分片发送时，服务器会基于不完整的请求就响应 | ✅ CP5 已修（`inbuf` + 定界） |
| 2 | **阻塞式单连接** | 一个连上却不发数据的客户端就能卡住整个服务器 | ✅ CP6b 已修（非阻塞 + `epoll`） |
| 3 | `backlog = 3` | 爬坡瞬间（几百条 SYN 同时到）仍会丢连接 | 🔶 keep-alive 之后**只剩这一处**，见上面压测结论 ③ |
| 4 | 不支持 keep-alive | 每个请求都要重新建连，`TIME_WAIT` 堆积 | ✅ CP6d-2 已修（`epoll.cpp`；2.7× 吞吐） |
| 5 | `send` 未处理部分写 | 响应很大时可能发不完整 | ⬜ CP6d-3 故意留着（当前响应 117 字节，**撞不到**，写不出会红的测试） |
| 6 | 没有日志系统、没有优雅退出 | 只用 `std::cout`；`Ctrl+C` 直接终止 | ⬜ 后续 |
| 7 | **每个请求都写 5 行日志到 stdout** | 压测时这是**同步阻塞 IO**（`std::endl` 会 flush） | ⬜ 未验证的猜想：它可能正在压着吞吐上限。要验证就造一个"关掉日志"的变体对比 |

## 目录结构

```
.
├── p2.cpp                     ← 阻塞版服务器（CP1 ~ CP5）
├── epoll.cpp                  ← 事件驱动服务器（CP6a ~ CP6d）
├── request_line.cpp           ← C++ 靶场：请求行解析
├── README.md
├── .clang-format              ← 代码风格约束（缩进 4 空格、指针贴左…）
├── .gitignore
├── .github/workflows/ci.yml   ← CI：严格编译 + 跑 4 套测试（36 项）
├── notes/
│   ├── HANDOFF.md             给新对话的第一份（我是谁 / 怎么教我 / 现状）
│   ├── learning_progress.md   进度 / 方法论 15 条 / 概念清单
│   ├── epoll_notes.md         epoll 专题（三件套 / EAGAIN 四态 / LT vs ET）
│   ├── pitfalls.md            全部踩坑 CP1~CP6d + 元教训
│   ├── selfcheck.md           自检 69 题（题 + 答案）
│   └── stress.md              JMeter 压测数据（p2 + epoll 两批）
└── tests/
    ├── p2_test.py             23 项【协议正确性】对 p2 和 epoll 都跑
    ├── balance_test.py        4 项【资源配平】fd + 账本
    ├── keepalive_test.py      7 项【keep-alive】自己起服务器
    ├── emfile_test.py         2 项【EMFILE 不空转】自己起服务器
    ├── parse_test.py          10 项【请求行解析靶场】
    ├── load_test.sh           JMeter 压测驱动
    ├── analyze.sh             压测结果分析
    ├── http_load.jmx          JMeter 测试计划
    └── results/               压测产物（已 gitignore）
```

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
- **`notes/stress.md`** —— JMeter 压测数据：第 1 节 `p2`（阻塞版）、第 2 节 `epoll`（keep-alive），含压测工具自身的两个坑

## 说明

- 这是个人学习项目，代码追求"每一行都能解释为什么"，不追求生产可用。
- 参考用的 AI 生成实现（`day2.cpp` ~ `day6.cpp`，逐级递进到 epoll / 线程池 / MySQL 连接池）在本仓库**之外**的目录里，不属于本仓库内容。
