# 学习进度（持续更新）

> 用法：每完成一项就把 `[ ]` 改成 `[x]`。
>
> **这份文件只负责三件事**：我在哪（进度）、我会了什么（方法论 + 概念清单）、接下来做什么（路线）。
>
> 另外两份笔记各管一摊，避免重复维护：
> - **撞过的坑** → `pitfalls.md`
> - **day2 逐行讲解 + 手册怎么查** → `../../serverai/day2_walkthrough.md`（AI 参考实现一起放在仓库外的 `serverai/`）

## 当前位置

```
day2.cpp 逐行读懂 ✅ → CP1 ✅ → CP2 ✅ → CP3 ✅ → CP4 ✅ → CP5（加固）🚧 → day3 半包 ⬜ → day4 epoll ⬜
```

练习文件：`p2.cpp`（自己手写，不抄 day2.cpp）
手写代码已经跑通：`curl -v http://127.0.0.1:8888/` 返回完整的 200 响应。

---

## 一、已完成

### ✅ day2.cpp 逐行读懂

能解释每一段在干什么、为什么这么写、写错会怎样：

- `socket` / `setsockopt` / `bind` / `listen` / `accept` / `read` / `send` / `close` 的返回值语义
- HTTP 响应的构造（状态行 + 头 + 空行 + body，`Content-Length` 是字节数）
- 两个 fd 的角色：`server_fd`（总机）vs `new_socket`（分机）
- 阻塞式单连接的代价

### ✅ CP1：socket + setsockopt + bind + listen

**验收证据**：

```
$ ss -tlnp | grep 8888
LISTEN 0  3  0.0.0.0:8888  users:(("p2",pid=...,fd=3))
       ↑  ↑      ↑              ↑
   Recv-Q=0 │   INADDR_ANY     fd 是进程内最小可用整数
        Send-Q = backlog = 3
```

- [x] `socket(AF_INET, SOCK_STREAM, 0)`
- [x] `setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))`
- [x] 填 `sockaddr_in`：`sin_family` / `sin_addr.s_addr = INADDR_ANY` / `sin_port = htons(PORT)`
- [x] `bind(server_fd, (struct sockaddr*)&address, sizeof(address))`
- [x] `listen(server_fd, 3)`
- [x] 启动日志 + `pause()` 停住
- [x] 编译零错误零警告
- [x] 亲手撞过 `EADDRINUSE` 的两种成因（见 `pitfalls.md` D1）

### ✅ CP2：accept

**验收证据**：

```
新连接 fd=4 来自 127.0.0.1:39658
```

- [x] `struct sockaddr_in client_addr{};`（新变量，不覆盖 bind 用的 `address`）
- [x] `socklen_t client_len = sizeof(client_addr);`（**放在 while 里面**）
- [x] `accept(server_fd, (struct sockaddr*)&client_addr, &client_len)`
- [x] 失败时用 `continue;`（EINTR/ECONNABORTED 是可恢复错误）
- [x] `inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip))`
- [x] `close(conn_fd);`（关分机，不关总机）
- [x] 删掉 `pause();`

**彩蛋已完成**：连两次，打印出来的 fd **都是 4** → 亲眼看到"fd 会被复用"。

### ✅ CP3：read

**验收证据**（三个实验，全部亲测）：

| 实验 | 结果 |
| --- | --- |
| `curl` 发完整请求 | 完整打印出请求（含结尾空行）✅ |
| 客户端**分两段**发送（间隔 0.5s） | **只打印出前半段** —— 亲眼看到**半包** ⚠️ |
| 客户端**连上但不发数据** | 另一个 curl **1047ms 超时** —— 整个服务器被卡住 ⚠️ |

- [x] `ssize_t n = read(conn_fd, buffer, sizeof(buffer));`
- [x] 三态处理：`n > 0` 数据 / `n == 0` 对端关闭 / `n < 0` 出错
- [x] `std::string req(buffer, n);`（用 `n` 当长度，不依赖 `\0`）

### ✅ CP4：send

**验收证据**：

```
$ curl -v http://127.0.0.1:8888/
< HTTP/1.1 200 OK
< Content-Type: text/html
< Content-Length: 34
< Connection: close
< 
<h1>Hello from my own server!</h1>

$ curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8888/
200
```

（`Content-Length: 34` = `<h1>`(4) + `Hello from my own server!`(25) + `</h1>`(5)，用 `body.size()` 算的，一个字节不差）

- [x] 按 RFC 9112 §2.1 构造消息：状态行 + 头 + **空行** + body
- [x] `Content-Length: " + std::to_string(body.size())`
- [x] `send(conn_fd, response.c_str(), response.size(), 0)`（第一个参数是**分机** `conn_fd`）
- [x] `if (sent < 0) { perror("send"); }`

### 🚧 CP5：加固（进行中）

- [ ] 把响应整段挪进 `if (n > 0)`（没收到请求就不该回响应）
- [ ] 加 `#include <csignal>` + `signal(SIGPIPE, SIG_IGN);`
- [ ] 用"临时连发两次 send"的实验验证 SIGPIPE（测完删掉实验代码）
- [ ] 编译零错误零警告
- [ ] 跑两个异常客户端：连上就关 / RST 强断 → 服务器仍然活着

---

## 二、已掌握的方法论（这些比语法重要）

### 1. 查手册的四步法

```
SYNOPSIS      ① 唯一要"抄"的：include + 函数签名
DESCRIPTION   ② 参数能填什么、这函数干嘛
RETURN VALUE  ③ 决定你的 if 怎么写
ERRORS        ④ 决定失败后重试/退出/忽略
```

### 2. 参数名当索引

不确定"这段是不是我要的"时：抄下 SYNOPSIS 的参数名 → 在页内 `/那个名字`。
（例：`man 2 bind | grep -n -A3 addrlen` 找到的段落，就是第三个参数的答案。）

### 3. 四条查询路径

| 要找什么 | 去哪 |
| --- | --- |
| 系统调用 | `man 2 名字` |
| C 库函数（含 `htons`） | `man 3 名字`（`htons` → `man 3 byteorder`） |
| 结构体/类型 | `man 3type 名字`（`sockaddr_in`） |
| 协议/常量/特殊值 | `man 7 协议`（`INADDR_ANY` → `man 7 ip`） |
| 不知道页名 | `man -k 关键词`（看节号 + 描述筛选） |
| 都不确定 | `grep -rn '名字' /usr/include/`（终极兜底） |

### 4. 三档过滤法（看不懂的内容怎么办）

- **A 必读**：会改变我这一行代码的句子
- **B 记名**：跳转引用（`protocols(5)`）—— 跳过，需要时再点
- **C 忽略**：与我场景无关的选项、罕见情况、历史

一句话判据：**这句话会改变我这一行代码吗？不会就跳过。**

### 5. 三句话自检（每一步都问）

1. 这个调用的**返回值语义**是什么？（成功/失败各是什么）
2. 我怎么**在运行期观察到**它生效了？（工具 + 看哪一列）
3. 如果它失败，我会看到**什么现象**？

### 6. 编译器当索引

报 `'sockaddr_in' was not declared` → 回 man 的 SYNOPSIS 抄 include，不背 include 表。
报 `'response' was not declared` → 检查**作用域**。

### 7. 改源码 → 保存 → 重新编译

```bash
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion p2.cpp -o p2 && ./p2
```

`&&` = 编译成功才运行，永远不会忘编译。
怀疑"改了没生效"时先比时间戳：`ls -l --time-style=+%H:%M:%S p2.cpp p2`

### 8. 手册给你零件，组装靠模型

`conn_fd` 还是 `server_fd`、`htons` 还是 `ntohs` —— 手册**不会直接告诉你**，靠这些心智模型：

- **总机 / 分机**：`server_fd` 一直监听，`conn_fd` 是一条连接
- **host ↔ network**：填进去用 `hton*`，读出来用 `ntoh*`
- **字节流**：TCP 没有消息边界，一次 read ≠ 一个请求

---

## 三、已掌握的概念清单

**socket 层**

- `socket(domain, type, protocol)`：`AF_INET` / `SOCK_STREAM` / `0`（单协议时填 0）
- 选型逻辑：把手册关键词翻中文 → 对照需求（可靠/有序/双向/面向连接 → `SOCK_STREAM`）
- `setsockopt` 五参数通式：`(fd, level, optname, &value, sizeof(value))`
- `level` = 选项所在的层（`SOL_SOCKET` / `IPPROTO_TCP` / `IPPROTO_IP`）
- `SO_REUSEADDR` 必须写在 `bind` 之前；它只允许覆盖 **TIME_WAIT**，不允许两个活着的监听进程
- `bind` 的"通用指针 + 长度"设计（`sockaddr` vs `sockaddr_in`，为什么要强转）
- `listen` 的 backlog：**accept 队列**长度上限，`min(backlog, somaxconn)`；另有一个 SYN 队列
- 队列满了的表现：ECONNREFUSED **或** 请求被忽略（客户端重传）→ 表现为"变慢"而不是失败
- `ss -tlnp` 的 `Recv-Q` = 当前排队数；`Send-Q` = backlog

**accept 层**

- `accept` 的第一个参数是**监听套接字**，返回**新的 fd**（`man 2 accept`：*"the listening socket, sockfd … returns a new file descriptor"*）
- 原来的 `server_fd` **不受影响**，继续监听
- `addrlen` 是**值-结果参数**：传进去是缓冲区大小，返回时被内核改写成实际长度 → **每轮都要重置**
- accept 的可恢复错误：`EINTR`（信号打断）、`ECONNABORTED`（对端握手后跑了）→ `continue` 而不是 `return 1`

**字节序 / 地址**

- `htons` = host → network（填进去）；`ntohs` = network → host（读出来）
- 16 位用 `s`（端口），32 位用 `l`（IPv4 地址）
- `INADDR_ANY` = 0.0.0.0 = 所有网卡（已经是网络字节序，不用转）
- 打印端口忘了 `ntohs` → 会打印出 13986 这种数字

**错误处理**

- `errno` 机制：失败返回 -1 并设置 `errno`；ERRORS 是"故障字典"
- 三类错误：可重试（`EINTR`）/ 可恢复（`EMFILE`）/ 编程错误（`EINVAL`）
- `perror` 打印 `strerror(errno)`

**进程与 fd**

- fd 是进程内的**小整数**（0/1/2 被 stdio 占了，所以第一个是 3）
- **close 后 fd 号立刻会被复用**（实测：连两次都是 fd=4）→ day5 use-after-close 竞态根源
- `server_fd`（总机，LISTEN，全程不关）vs `conn_fd`（分机，一条连接，用完就关）

**作用域**

- 一对 `{}` 就是一个作用域，里面声明的变量出了 `}` 就销毁
- 越往里声明活得越短：`server_fd`(main) > `conn_fd`(while) > `response`(if)
- 所以"处理一个请求"的所有东西（`req`/`body`/`response`/`sent`）必须待在同一个 `{}` 里

**信号**

- 信号 = 内核发给进程的异步通知，每个信号有**默认处置**（终止/忽略/暂停）
- `SIGPIPE(13)`：往"没有读者"的管道/连接写数据时触发，**默认终止进程**
- 服务器必须 `signal(SIGPIPE, SIG_IGN)` → 让 `send` 返回 -1 + `EPIPE`，由代码处理
- **第一次 send 通常成功**（数据进了发送缓冲区），第二次写才 `EPIPE` → 所以单次 send 的代码很难撞上
- **信号处置会被子进程继承**（父进程设了 `SIG_IGN`，子进程也是 `SIG_IGN`）
- 排查技巧：进程莫名消失时 `echo $?` = **128 + 信号号**（141 = 128+13 = SIGPIPE）

**HTTP**

- 响应格式：状态行 + 头 + **空行** + body；`\r\n` 结尾（RFC 9112 §2.1）
- `Content-Length` 必须是**字节数**且与实际 body 一致
- `Connection: close` 是显式声明关闭；HTTP/1.1 **默认 keep-alive**
- "HTTP 无状态" ≠ "短连接"（无状态指不记得上一个请求，所以要 Cookie）

**内核行为**

- TIME_WAIT：主动关闭方等 2MSL（Linux 约 60 秒），期间占用本地端口
- TCP 是**字节流**，没有消息边界 → 一次 `read` ≠ 一个请求（半包/粘包）
- 阻塞式 `read` + 单线程 → **一个不发数据的客户端就能卡死整个服务器**

---

## 四、亲手撞过的坑 → 见 `pitfalls.md`

已整理成 22 条，分五类，每条都有"现象（报错原文）→ 根因 → 解决 → 验证"：

| 分类 | 编号 | 主题 |
| --- | --- | --- |
| A. 手册与资料查询 | A1~A6 | 把 man 当代码抄 / `man -k` 筛选 / 找不到 RETURN VALUE / 看不懂 ERRORS / `htons` 在哪页 / 参数名当索引 |
| B. 语法与 API | B1~B3 | `bool reuse` 导致 EINVAL / `pause("system")` / 块作用域 |
| C. 编译与工作流 | C1~C5 | 没重新编译 / 没保存 / 文件路径 / 输出缓冲 / `using namespace std` |
| D. 运行期与系统行为 | D1~D7 | EADDRINUSE 两种成因 / Ctrl+Z / "没输出"其实在运行 / **半包** / **阻塞 read** / **SIGPIPE** / fd 复用 |
| E. 逻辑与设计 | E1 | 没请求也回响应 |

`pitfalls.md` 里还有：**症状速查表**（看到报错直接查编号）、**常用命令小抄**、**SIGPIPE 专题**。

---

## 五、接下来做什么

- [ ] **CP5 收尾**：响应挪进 `if (n > 0)`、加 SIGPIPE 忽略、验证异常客户端杀不掉服务器
- [ ] **回到 `day3.cpp`**：学正规的半包解法 —— 按连接读缓冲区 + `Content-Length` + **长度上限防 DoS**
- [ ] `day4.cpp`：非阻塞 IO + `epoll`（解决"一个慢客户端卡死全服"）
- [ ] `day5.cpp`：`epoll` + 线程池（单 Reactor）；注意 fd 所有权交接的竞态
- [ ] `day6.cpp`：MySQL 连接池 + 预处理语句

### 自检问题（能答上来才算真会）

**CP2（accept）**
1. 为什么 `accept` 第一个参数是 `server_fd`？
2. `conn_fd` 和 `server_fd` 有什么区别？谁先关、谁后关？
3. `client_len` 为什么每次都要重置？
4. `accept` 失败为什么用 `continue` 而不是 `return 1`？
5. 打印端口为什么要 `ntohs`？

**CP3（read）**

6. `read` 的三个返回值分别代表什么？
7. 为什么说"一次 `read` ≠ 一个请求"？man 里哪一句写过这件事？
8. 一个连上但不发数据的客户端，为什么能卡死整个服务器？

**CP4（send）**

9. `Content-Length` 填的是什么？填错了会怎样？
10. `send` 第一个参数传谁？为什么不是 `server_fd`？
11. `send` 的返回值为什么可能**小于**你要求的长度？
12. SIGPIPE 是什么？为什么服务器要忽略它？不忽略会怎样？
