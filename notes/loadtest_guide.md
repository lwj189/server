# 压测方法（Load Testing）—— 查阅型

> **这份文件只讲"方法"**：测什么 / 怎么设计 / 怎么看结果 / 同时观察什么。
> **具体数字在 `notes/stress.md`**（`p2` 阻塞版 + `epoll` keep-alive 两批数据）——
> 同一件事不写两处：这里只留方法论，数字一律去 `stress.md` 查。
>
> 工具链三件套：`tests/load_test.sh`（驱动）→ `tests/analyze.sh`（出统计表）
> → `tests/http_load.jmx`（JMeter 计划模板）。
>
> 📌 原来这份是桌面上的 `网络,压测.test`（散在那儿没人翻）。2026-10-04 移进仓库，
> 同时修掉了几处过时内容（JMeter 搬家、epoll 已测完、失效的文件引用）。
> 开头原本还有一段"该查哪一节 man"—— 那段已由 `learning_progress.md` 方法论 #3
> 「四条查询路径」覆盖（更全），所以这里不再重复。

---

## 一、压测到底测什么

一句话：**不是测"我的服务器有多强"，而是找出"它在什么条件下会坏、坏在哪里、坏成什么样"。**

它回答五个不同的问题：

| # | 问题 | 怎么测 |
| --- | --- | --- |
| 1 | **能扛多少？**（容量） | 并发阶梯 1 → 10 → 50 → 100 → 200 |
| 2 | **快不快、稳不稳？**（延迟） | 看分位数 `p50/p90/p99/p99.9/max` |
| 3 | **会不会出错、会不会崩？**（稳定性） | 错误率 + 错误**类型** + `pgrep` 看进程还活着吗 |
| 4 | **慢在哪里？**（瓶颈定位） | `Recv-Q` 顶到多少 + 错误类型指向谁 |
| 5 | **有没有资源泄漏？** | `ss -tan state time-wait \| grep -c 端口` |

⚠ **别只看平均**：`Avg=10ms` 看着很好，而 `max` 已经 2 秒 —— 用户感知到的是那个 2 秒。

★ 关键认识：压测**不是功能测试**（那不是它管的），
它管的是"**在多大压力下、哪个环节先撑不住**"。

---

## 二、性能曲线的四个区（找"拐点"是压测的核心目的）

```
吞吐
 ↑
 │                              ┌──────── ④ 崩溃区
 │                    ③ 平台期 ─┘          延迟爆炸、大量超时/拒绝
 │                  ┌──────────
 │                  │ ← 拐点(knee)：某个资源饱和了，压测最想找到它
 │           ┌──────┘
 │      ┌────┘  ① 线性区：加并发 = 加吞吐，延迟平稳
 │  ┌───┘
 └──────────────────────────────────────→ 并发数
```

| 区 | 特征 |
| --- | --- |
| ① 线性区 | 资源充足，吞吐随并发线性上升 |
| ② 拐点 | 吞吐增速变缓、延迟开始抬头 ← **瓶颈在这里** |
| ③ 平台期 | 吞吐到顶，再加压只涨延迟不涨吞吐 |
| ④ 崩溃区 | 大量超时/拒绝、延迟爆炸 |

**本项目实测到的拐点**：在 `p2`（阻塞 + `backlog=3`）上，拐点表现为
`max` 从 19ms 飙到 2003ms、错误类型全是 `ConnectTimeoutException`
→ 罪魁祸首是 `backlog=3`。完整数据见 `stress.md`。

---

## 三、压测的六种类型（别混为一谈）

| 类型 | 目的 | 怎么做 |
| --- | --- | --- |
| 基准 | 记录"改之前"的数字 | 单并发 × 固定次数 |
| 负载 | 预期负载下是否达标 | 正常并发 × 长时间 |
| 压力 | 找崩溃点 | 并发一路加到崩 |
| 尖峰 | 突发流量能不能扛 | 瞬间拉起大量并发 |
| 稳定性 | 长时间跑会不会泄漏 | 中低并发 × 几小时 |
| 容量规划 | 要几台机器 | 找到拐点后算 |

---

## 四、七条方法论（比工具重要）

1. **一次只改一个变量** —— 并发、请求大小、连接复用，别同时变
2. **压测机和被测机别抢资源** —— 本项目是**同机**（4 核）→ 数字只能**横向对比**，不能当绝对值
   （⚠ JMeter 自己只有 **1GB 堆**，并发拉很高时它可能先成为瓶颈）
3. **先热身** —— 第一次请求 30ms、后面 1ms，那是 JVM/JIT 初始化，不是服务器慢
4. **看分位数，不看平均** —— p99 / max 才是用户能感知的
5. **关注错误的"类型"** —— "2 个错误"没用；"2 个 `ConnectTimeoutException`"直接指向 `backlog`
6. **记录环境** —— CPU 核数、`ulimit -n`、`somaxconn`、内核版本，否则数字无法复现
7. **有对比才有意义** —— 单独一个 4376/s 什么都说明不了

---

## 五、怎么做：三条路

### 【路 1】一键脚本（最快）

```bash
cd ~/桌面/server

./tests/load_test.sh                                # 默认阶梯 1/10/50/100（跑 p2）
./tests/load_test.sh 200 500                        # 只跑一档：200并发×500次
LEVELS="1:500 20:300 100:200" ./tests/load_test.sh  # 自定义阶梯
```

脚本自动做：编译 → 启服务 → 等 LISTEN → 逐档压测 → 调 `analyze.sh` 出分析表
→ 看 `TIME_WAIT` / accept 数 → 杀服务（`trap EXIT`，Ctrl+C 也安全）

**压哪台服务器**（2026-10-04 起支持切）：

```bash
BIN=./epoll SRC=epoll.cpp TAG=e-  KEEPALIVE=false ./tests/load_test.sh   # epoll，关 keep-alive
BIN=./epoll SRC=epoll.cpp TAG=k-  KEEPALIVE=true  ./tests/load_test.sh   # epoll，开 keep-alive
BIN=./p2    SRC=p2.cpp    TAG=p-                  ./tests/load_test.sh   # p2 基线
```

| 环境变量 | 含义 | 默认 |
| --- | --- | --- |
| `SRC` / `BIN` | 编译哪个源码 / 跑哪个二进制 | `p2.cpp` / `./p2` |
| `KEEPALIVE` | 透传给 `.jmx` 的 `-Jkeepalive` | `false` |
| `TAG` | 结果文件名前缀（避免不同配置互相覆盖） | 空 |

★ 它自己**不做统计**，而是**调用 `analyze.sh`** —— 统计逻辑只有一份，
改 `analyze.sh` 这里跟着一起变。（以前两边各有一份，早晚不一致）

★ `PORT=` 只是脚本用的；**服务器实际监听哪个端口由源码里的 `constexpr PORT` 决定**，
两者必须一致。

### 【路 2】手动版（要学会这个）

```bash
# 终端1（服务器）：
cd ~/桌面/server
./epoll                     # 或 ./p2
# 看到 listening on port 8888 ... 就起来了，它会占住这个终端

# 终端2（压测）：
cd ~/apache-jmeter-5.6.3/bin
./jmeter -n \
  -t ~/桌面/server/tests/http_load.jmx \
  -l ~/桌面/server/tests/results/my.jtl \
  -j ~/桌面/server/tests/results/my.jmeter.log \
  -Jthreads=100 -Jloops=200 -Jrampup=1
```

⚠ 手动跑两个必记：
- **`-l` 是追加模式**，重复跑同一档要先 `rm` 掉旧文件（`load_test.sh` 没这问题，它跑之前会 `rm`）
- **一定要带 `-j`**，否则 `jmeter.log` 会拉在你当前所在目录（曾在桌面留过一个）

```bash
# 终端3（观察，可选但很有价值）：
while true; do ss -lnt | awk '/:8888/{print "Recv-Q="$2" Send-Q="$3}'; sleep 0.2; done
```

收尾：回终端1 按 **Ctrl+C**（不是 Ctrl+Z！）

### 【路 3】GUI 模式（只用来看和改计划，不要用它压测）

```bash
cd ~/apache-jmeter-5.6.3/bin
./jmeter
# File → Open → ~/桌面/server/tests/http_load.jmx
# 左侧树：Test Plan → Load → HTTP GET /
# 右键 Load → Add → Listener → Aggregate Report（加图表）
```

⚠ 官方警告：GUI 本身吃 CPU/内存，跑正式压测结果会脏。**GUI 只用来搭和改，跑压测回命令行。**

---

## 六、JMeter 命令逐参数拆解

```
./jmeter -n -t 计划.jmx -l 结果.jtl -j 自己的日志.log -Jthreads=100 -Jloops=200 -Jrampup=1
```

| 参数 | 含义 |
| --- | --- |
| `-n` | non-GUI 模式（真正压测必须加，不加就开图形界面） |
| `-t <文件>` | test plan，测试计划（就是 `.jmx`） |
| `-l <文件>` | log，结果文件（`.jtl`，本质是 CSV）**追加模式** |
| `-j <文件>` | JMeter 自己的日志（默认 `jmeter.log`，会落在当前目录） |
| `-Jthreads=N` | 覆盖计划里的 `${__P(threads,10)}` ← 并发数 |
| `-Jloops=N` | 覆盖 `${__P(loops,100)}` ← 每线程循环几次 |
| `-Jrampup=N` | 覆盖 `${__P(rampup,1)}` ← N 秒内逐渐启动完线程 |
| `-e -o <目录>` | 跑完生成 HTML 报告（推荐加） |
| `-g <jtl> -o <目录>` | 从已有 jtl 生成报告（不重跑压测） |

★ **核心机制**：`.jmx` 里写的是 `${__P(key,默认值)}` 这种"属性占位符"，
所以同一个计划文件靠 `-J` 就能跑出任意并发，不用改文件。

★ **唯一的标准模板**：`server/tests/http_load.jmx`（通用 HTTP 压测模板）
支持 10 个 `-J` 参数（都有默认值，不传也能跑）：

```
-Jhost=127.0.0.1        -Jport=8888          -Jpath=/
-Jmethod=GET            -Jthreads=10         -Jloops=100
-Jrampup=1              -Jkeepalive=false
-Jconnect_timeout=2000  -Jresponse_timeout=5000
```

带 HTTP 200 响应断言（响应码不是 200 就算失败）。
压任何 HTTP 服务都是同一条命令，例如：

```bash
./jmeter -n -t http_load.jmx -l out.jtl -Jport=8080 -Jpath=/api -Jthreads=100
```

⚠ **`-Jkeepalive` 有个坑（2026-10-04 踩到并修掉）**：
`use_keepalive` 在 `.jmx` 里**必须写成 `stringProp`，不能写 `boolProp`** ——
JMeter 不对 `boolProp` 里的 `${__P(...)}` 求值，`-Jkeepalive=true` 会**静默失效**
（照旧发 `Connection: close`）。验证方法：看服务器收到的请求头是 `keep-alive` 还是 `close`。

---

## 七、结果怎么看（四种方式）

### 【方式 A】命令行 summary（最快）

```
summary =  20000 in 00:00:05 = 4376.4/s Avg: 10 Min: 0 Max: 2003 Err: 2 (0.01%)
           └样本数┘ └─总耗时─┘ └吞吐/s┘  └平均┘ └最小┘└最大┘ └失败数+比例┘
```

### 【方式 B】`.jtl` 原始数据（能自己算分位数）

```bash
# 失败数量
awk -F, 'NR>1 && $8=="false"' 结果.jtl | wc -l

# 失败原因
awk -F, 'NR>1 && $8=="false" {print $4, $9}' 结果.jtl

# p99 延迟
awk -F, 'NR>1{print $2}' 结果.jtl | sort -n | awk '{a[NR]=$1} END{print a[int(NR*0.99)]}'
```

⚠ **上面这三条在失败样本上会算错**：JMeter 的 `failureMessage`（第 9 列）**可能带换行**，
续行的第 1 列不是数字，会被当成新样本。加个守卫：

```bash
awk -F, 'NR>1 && $1 ~ /^[0-9]+$/ {print $2}' 结果.jtl | sort -n | awk '{a[NR]=$1} END{print a[int(NR*0.99)]}'
```

**列的含义**（记住前 9 列 + 第 17 列就够）：

| 列 | 名字 | 说明 |
| --- | --- | --- |
| 1 | `timeStamp` | 请求发出时间戳(ms) —— **判断"是不是真样本行"就看它是不是纯数字** |
| 2 | `elapsed` | ★ 总耗时(ms) |
| 3 | `label` | 采样器名字 |
| 4 | `responseCode` | HTTP 状态码 / 非 HTTP 异常名 |
| 8 | `success` | ★ `true`/`false` |
| 9 | `failureMessage` | ★ 失败原因（⚠ 可能多行） |
| 11 | `bytes` | 响应字节数 |
| 15 | `Latency` | 首字节延迟 |
| 17 | `Connect` | ★ 建连耗时（每请求一条连接时这列才有意义） |

### 【方式 C】HTML 报告（最直观）

```bash
./jmeter -e -o ~/桌面/server/tests/results/report \
         -n -t 计划.jmx -l 结果.jtl -Jthreads=100 -Jloops=200

# 或从已有 jtl 补生成
./jmeter -g 结果.jtl -o ~/桌面/server/tests/results/report
```

浏览器打开 `tests/results/report/index.html`（有吞吐曲线、延迟分位图、错误明细表）。

### 【方式 D】一键分析（推荐，通用）

```bash
cd ~/桌面/server
./tests/analyze.sh tests/results/*.jtl
```

输出一张表：`文件 样本 失败 失败率 耗时s 吞吐/s 平均ms p50 p90 p99 最大ms 建连p99`
外加：失败原因分类 + 失败率随档位变化（找拐点，柱状图）。

★ 这个脚本对**任何** JMeter 的 `.jtl` 都有效（列格式由 JMeter 固定），
换服务器不用改它 —— 这是整条工具链里**最通用**的部分。

★ 它先检查文件存在/非空 —— 避免"第一个命令失败、管道后面照跑"产出全 0 的假结果。

★ **"耗时"列异常大 / 吞吐异常小** = 两种可能：
① 这个 `.jtl` 被多次运行**追加**过；② **失败样本的多行 `failureMessage` 被当成了样本**
（2026-10-04 修掉，见方式 B 的守卫）。看到 `耗时 = 1.79e9 秒` 就是第二种。

★ 脚本里踩过的另一个坑：**中文表头对不齐** ——
`printf` 的 `%-24s` 是按"字节"补齐的，而中文在终端占 2 列：
"文件" 是 6 字节却只显示 4 列 → 表头整体错位。
正解：自己算显示宽度（字符数 + CJK 字符数）再补空格；
对 `printf '%*s'`（右对齐）还要把 N 写成 `字节数 + (目标宽度 - 显示宽度)`。
自检：用 python 的 `unicodedata.east_asian_width` 算每行的真实显示宽度，
表头 / 分隔线 / 数据行必须相等。

---

## 八、压测时同时观察什么（比数字更有价值）

**看 accept 队列**（`Recv-Q` 就是当前排队数，`Send-Q` 就是 `backlog`）：

```bash
while true; do ss -lnt | awk '/:8888/{print $2}'; sleep 0.1; done | sort -n | uniq -c
```

`p2` 的实测结果：

```
Recv-Q=0 → 61 次
Recv-Q=1 → 5 次
Recv-Q=2 → 1 次
Recv-Q=3 → 4 次   ← backlog=3，满了
Recv-Q=4 → 1 次   ← Linux 允许 backlog+1
```

**看 `TIME_WAIT`**（服务器先 close，所以它自己攒 60 秒）：

```bash
ss -tan state time-wait | grep -c ':8888'
```

**看进程还活着吗 / 卡在哪**：

```bash
pgrep -a epoll
ps -o pid,stat,wchan:20,cmd -p $(pgrep epoll)
```

**看服务器日志有没有异常**：

```bash
grep -E 'reset|Broken|perror' tests/results/srv.log
```

★ **延迟尖峰的读法**：`max = 1271ms / 2003ms` 这种"接近整数秒"的数字，
不是服务器慢，是 **accept 队列满 → 内核丢 SYN → 客户端按 TCP 退避重传（1s → 2s → 4s）**。
`建连 p99` 那一列是判断这件事的关键（`stress.md` 里 epoll 把它压到了 0ms）。

---

## 九、本项目基线

**数字全部在 `notes/stress.md`**，这里只留"怎么读"：

| 版本 | 关键现象 | 结论 |
| --- | --- | --- |
| `p2`（阻塞 + `backlog=3`） | `Recv-Q` 顶到 3~4；错误全是 `ConnectTimeoutException`；`建连 p99` 1001ms | 瓶颈**不在处理能力**（p50=0ms），在 **accept 队列** + 单线程串行 |
| `epoll`（事件驱动 + keep-alive） | `建连 p99` **0ms**；同样并发下吞吐 2.4×、错误少 12× | keep-alive 把"每请求一条连接"消掉 → 墙退远；**剩下的墙仍是 `backlog=3`**（只在爬坡瞬间咬人） |

**下一步杠杆**：`listen(fd, backlog)` 与 `net.core.somaxconn`
（`backlog=3` 是被这次压测**证实**的下一个瓶颈，不是猜的）。

---

## 十、压测记录模板（每次压测都记这七项）

```
1 被测版本：
2 环境：CPU 核数、ulimit -n、somaxconn、内核版本
3 工具：JMeter 版本（同机压测 → 绝对值仅供参考）
4 场景：方法 / 路径 / Connection: close 还是 keep-alive
5 结果：表格（并发 / 吞吐 / p50 / p90 / p99 / max / Err%）
6 现象：Recv-Q、TIME_WAIT、错误类型、进程状态
7 结论：瓶颈在哪；下一步验证什么
```

---

## 十一、文件清单

```
~/apache-jmeter-5.6.3/bin/jmeter            JMeter 可执行文件（2026-10 从桌面搬到 home）
~/桌面/server/tests/http_load.jmx           通用压测模板（10 个 -J 参数 + HTTP 200 断言）
~/桌面/server/tests/load_test.sh            一键压测驱动（项目专用；可切服务器）
~/桌面/server/tests/analyze.sh              通用结果分析器（对任何 .jtl 都有效）
~/桌面/server/tests/results/                结果输出（.jtl + srv.log + .jmeter.log，已 gitignore）
~/桌面/server/notes/stress.md               压测数字
~/桌面/server/notes/loadtest_guide.md       本文件（方法）

被测服务器：~/桌面/server/p2.cpp（阻塞版）、~/桌面/server/epoll.cpp（事件驱动版）
```

（已删除：`~/桌面/loadtest-8888.jmx`、`tests/p2_load.jmx`、`tests/test_client.py`
—— 前两个合并成了 `tests/http_load.jmx`；`test_client.py` 的职责由
`tests/p2_test.py` 等四套脚本接管。）
