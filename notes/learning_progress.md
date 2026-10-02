# 学习进度（持续更新）

> 用法：每完成一项就把 `[ ]` 改成 `[x]`。
>
> **这份文件只负责三件事**：我在哪（进度）、我会了什么（方法论 + 概念清单）、接下来做什么（路线）。
>
> 另外几份笔记各管一摊，避免重复维护：
> - **撞过的坑** → `pitfalls.md`
> - **epoll 的 API + 概念 + 速查表** → `epoll_notes.md`
> - **给"新对话"的交接文档**（工作方式 / 项目地图 / 心智模型）→ `HANDOFF.md`
> - **day2 逐行讲解 + 手册怎么查** → `../../serverai/day2_walkthrough.md`（AI 参考实现一起放在仓库外的 `serverai/`）

## 当前位置

```
day2.cpp 逐行读懂 ✅ → CP1 ✅ → CP2 ✅ → CP3 ✅ → CP4 ✅ → CP5a ✅ → CP5b ✅
        → 修 3 个解析问题 ✅ → day4：CP6a ✅ → CP6b ⬜ ← 下一步
```

练习文件：`p2.cpp`（自己手写，**阻塞版**，CP1~CP5）
　　　　　`epoll.cpp`（**day4 练习**，CP6a 完成，还没接管客户端）
自动化验收：`tests/p2_test.py` —— **13 个用例 / 23 项检查，当前全绿**

一句话现状：**`p2.cpp` 是一个能扛住各种异常客户端的 HTTP 服务器（阻塞式，一次一条连接）；`epoll.cpp` 刚跑通事件循环（空闲时睡在 `ep_poll` 里、0% CPU），但客户端 fd 还没纳进来。**

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

### ✅ CP5a：半包处理（按连接读缓冲区）+ 请求头大小上限

**目标**：不论客户端怎么切分发送，都要收到**完整的请求头**再响应。

**验收证据**（`tests/p2_test.py` 的 [5] [6] [7]）：

| 场景 | 结果 |
| --- | --- |
| 分两段发送（间隔 0.5s） | 200 ✅ |
| **逐字节发送**（每字节间隔 5ms） | 200 ✅ |
| 发 16KB 垃圾、永远不发 `\r\n\r\n` | 服务器**主动断开** ✅ |

- [x] 删掉"单次 read + `if (n > 0)`"那套旧结构，改成循环读
- [x] `std::string read_buffer;` 声明在 `while (true)` **里面**（每个连接一份）
- [x] `read_buffer.append(chunk, n);` 把每次读到的追加进去
- [x] 判据用 `read_buffer.find("\r\n\r\n")`（来自 RFC 9112 §2.1 的 ABNF）
- [x] `constexpr size_t MAX_HEADER_SIZE = 8 * 1024;` 防 DoS
- [x] 超限检查放在"找判据**之后**"（收全了就不算超限）

**内存实测**（DoS 保护真的生效）：

```
发 10MB 垃圾前 RSS =  3948 KB
发 10MB 垃圾后 RSS =  4040 KB   ← 基本没变；修复前是 4136 → 14508 KB
```

### ✅ CP5b：按 Content-Length 收全 body + body 大小上限

**核心认知：头部完整 ≠ 请求完整。**

```
一个 HTTP 消息 = 头部（分隔符定界 \r\n\r\n） + body（长度前缀定界 Content-Length）
```

**验收证据**（`tests/p2_test.py` 的 [8] [9] [10]）：

| 场景 | 结果 |
| --- | --- |
| POST + 完整 body 一次送达 | 200 ✅ |
| **body 分两段：先发 5/10 字节** | 服务器**不提前响应** ✅；补齐后 200 ✅ |
| `Content-Length: 67108864`（64MB）但不发 body | 被拒/断开 ✅ |
| `Content-Length: abc`（非法值） | 服务器**活着** ✅（try/catch 生效）|
| `curl -X POST -d 'name=Bob&age=25'` | 日志里能看到**完整 body** ✅ |

- [x] `header_end = read_buffer.find("\r\n\r\n")`
- [x] 解析 `Content-Length`：`cl_pos < header_end` 保证它是在**头部里**找到的
- [x] `total = header_end + 4 + content_len`
- [x] **`while (read_buffer.size() < total)` 继续读**（这是 CP5b 的关键）
- [x] `std::stoul` 用 `try / catch` 包住（否则非法值会让整个进程 terminate）
- [x] body 大小上限检查
- [x] 用 `read_buffer.size() >= total` 判断（**不能**假设"再 read 一次 body 就齐了"）

### ✅ 修 3 个解析问题（大小写 / OWS / 非法值）

**性质**：这三个都是"**能编译、能跑、还返回 200**，但解析结果是错的"类型 —— 不写测试根本发现不了。

| 问题 | 旧实现 | 新实现 | 依据 |
| --- | --- | --- | --- |
| 头字段名**大小写敏感** | `find("Content-Length: ")` | `toLowerAscii(...) == "content-length"` | RFC 9110 §5.1 *"Field names are case-insensitive"* |
| 冒号后**必须有空格** | 写死一个空格 | 跳过 OWS（0 个或多个空格/制表符） | RFC 9112 §5 `field-line = field-name ":" OWS field-value OWS` |
| 值可以是 `10abc` | `stoul` 遇非数字就停 → 返回 10 | `isAllDigits` 先卡纯数字，`stoull` 只负责溢出保护 | RFC 9112 §6.3 要求无效的 Content-Length 必须拒绝 |

**新增的三个纯函数**：`toLowerAscii` / `findHeader` / `isAllDigits`（源码注释里逐条写了语法点：`static` 的内部链接、`const &` 只读参数、输出参数惯用法、范围 for 的 `&`、`static_cast<unsigned char>` 为什么必要）

**验收（TDD 红→绿）**：

```
修复前（只把 Content-Length 那一段还原）：t11/t12/t13 全红，5 项失败
   服务器提前响应了 b'HTTP/1.1 200 OK...'   ← 说明它没认出 Content-Length
   服务器回了 None —— 说明 "10abc" 被当成了合法值

修复后：通过 23 项，失败 0 项
```

**测试设计的教训（值得记住）**：解析失败的**症状不是报错，而是"提前响应"**。
所以 t11/t12 必须"**先只发一半 body、停 1 秒、看服务器有没有抢跑**" ——
只看"最终返回 200"是抓不住 bug 的，因为**正确和错误两种行为都会返回 200**。

### ✅ CP6a：epoll 事件循环（`epoll.cpp`）

**目标**：把"阻塞单连接"改成"**1 个线程照看 N 个连接**"。先只搭事件循环，**不接 HTTP 解析**。

**为什么先不接解析**：epoll 回答"哪个 fd 有事件"（IO 层），HTTP 解析回答"这堆字节里哪一段是完整请求"（应用层）—— **两者正交**。一次只引入一个新变量，否则出 bug 时分不清是哪一层错了。

**验收证据（实测）**：

```
① 编译：g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion → 退出码 0

② 空闲时：  PID  STAT  %CPU  WCHAN      CMD
             11   S     0.0   ep_poll    ./e
                            ↑ 线程睡在内核的 ep_poll 里，0% CPU

③ 同时来 3 个连接 → 一次事件全部接到（accept 循环到 EAGAIN 生效）
   新连接 fd=5 / fd=5 / fd=5    ← 都是 5，因为立刻 close 了（fd 复用）
```

- [x] `server_fd` 设非阻塞：`fcntl(F_GETFL, 0)` 读 → `F_SETFL | O_NONBLOCK` 写（**读-改-写三步**）
- [x] `epoll_create1(0)` 创建 epoll 实例（**它本身也是一个 fd**）
- [x] `epoll_ctl(ADD, server_fd, &ev)`：`ev.events = EPOLLIN`、`ev.data.fd = server_fd`
- [x] `epoll_wait` 三态：`>0` 就绪个数 / `0` 超时 / `-1` 出错（`EINTR` → `continue` 重试）
- [x] `if (events[i].data.fd == server_fd)` 区分"总机事件"
- [x] `accept` **循环到 `EAGAIN/EWOULDBLOCK`**（一次事件可能对应队列里多个新连接）
- [x] 踩过"少一个 `}` → 整段事件循环变成死代码"（已修，注释里留了警告）

**还没做的**：client fd 还没纳入 epoll → CP6b。

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
报 `expected '}' at end of input` → 括号层级错乱（改代码时最常见）。

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

### 9. 设计题的三问模板（CP5 学到的）

前面几步都是"查询题"（手册里有答案）；CP5 开始是"**设计题**"（手册只给零件）。三问：

1. **问题是什么？**（现象 → 约束）　例：一次 read 不够 → 要循环 + 要缓冲
2. **规范里"完整/正确"的定义是什么？**（RFC / 协议）　例：RFC 9112 §2.1 → `\r\n\r\n`
3. **我手上有什么零件？**（API 能力）　例：`read` + `std::string::append/find`

三样拼起来就是骨架。**骨架不是"标准答案"，是"一个合理选择"**——要能说出"我为什么这么设计"。

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
- `perror` 打印 `strerror(errno)`；**只能紧跟在失败的系统调用之后**，隔几行再 perror 打的是**过期的 errno**（会打印 `read: Success` 这种垃圾）
- `std::stoul` 会抛异常（`"abc"` / 空串 / 超大数 / 负数回绕）→ 不 catch 就是 `std::terminate`

**进程与 fd**

- fd 是进程内的**小整数**（0/1/2 被 stdio 占了，所以第一个是 3）
- **close 后 fd 号立刻会被复用**（实测：连两次都是 fd=4）→ day5 use-after-close 竞态根源
- `server_fd`（总机，LISTEN，全程不关）vs `conn_fd`（分机，一条连接，用完就关）
- ⚠️ 在 `while (true)` 里用 `continue` 时，**必须先 `close(conn_fd)`**，否则跳过底部的 close → fd 泄漏

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
- 一个完整请求 = **头部（分隔符定界）** + **body（长度前缀定界）** ← CP5b

**消息定界（CP5 的核心）**

- TCP 是字节流，**没有消息边界**，定界方式只有三种：定长 / **分隔符** / **长度前缀**
- HTTP 很特别：头部用**分隔符**（`\r\n\r\n`），body 用**长度前缀**（`Content-Length`）
- **半包**：一次 read **不够**一个消息 → 循环**读**、攒够
- **粘包**：一次 read **超过**一个消息 → 循环**解析**、`erase` 消费掉已处理的字节
- 两者共用同一套基础设施：**每连接缓冲区 + 解析循环**
- 缓冲区属于**连接的状态**，必须和连接同生共死（声明在 `while (true)` 里）
- **当前实现用"一个连接只处理一个请求"规避了粘包**（响应完就 `close`，多余字节丢弃）——代价是没有 keep-alive

**内核行为**

- TIME_WAIT：主动关闭方等 2MSL（Linux 约 60 秒），期间占用本地端口
- 阻塞式 `read` + 单线程 → **一个不发数据的客户端就能卡死整个服务器**

**epoll / 事件驱动（CP6a）**

- **思想转变**：从"我主动去读、没数据就等着" → "**先把连接登记给内核、内核告诉我谁就绪**"（Reactor）
- `fcntl(fd, F_GETFL, 0)` 里那个 `0` 是**占位符**（`man 2const F_GETFL` 原文：*"arg is ignored"*）
- **必须读-改-写三步**：`F_SETFL` 是**整体替换** status flags，直接写 `O_NONBLOCK` 会把别的标志抹掉
- `O_NONBLOCK` 的效果：`read` 没数据时**立刻返回 -1 + `EAGAIN`**（而不是挂起线程）
- **`EAGAIN` 不是错误**，是"这一轮读完了"；`EAGAIN == EWOULDBLOCK`（Linux 同值）
- 非阻塞 `read` 的**四态**：`>0` 数据 / `0` EOF / `-1`+`EAGAIN` 读完了 / `-1`+其他 真错误
- `epoll_event.data` 是**你写给内核的标记，事件回来时原样带回** → 用它分辨事件属于哪个 fd
- `epoll_wait` 的 `timeout`：`-1` 永久等（0% CPU）/ `0` 立即返回 / `>0` 等多少毫秒
- **LT vs ET**：LT（默认）只要没读完就**还会**通知你；ET（要加 `EPOLLET`）只在状态变化时通知一次，**必须循环读到 `EAGAIN`**
- `sizeof(struct epoll_event) == 12`（因为 `__EPOLL_PACKED`；不加会因对齐补到 16）
- **一次事件可能对应多个就绪 fd，也可能对应一个 fd 的多批数据** → `accept` 和 `read` 都要**循环到 `EAGAIN`**
- `EPOLLRDHUP`：对端 half-close 的通知，判断断开比 `EPOLLIN` 更早更准

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

**CP5 / CP6 阶段新撞的、还没归档的坑**（下次更新 `pitfalls.md` 时补进去）：

| # | 现象 | 根因 |
| --- | --- | --- |
| E2 | `'response' was not declared in this scope` | 把声明挪进 `if` 块，但 `send` 还留在块外 → **作用域** |
| E3 | `expected '}' at end of input` | 改动时括号层级错乱（`if (header_done)` 的 `}` 用掉了 main 的） |
| D8 | 请求头收全了就以为"请求收全了" | **头部完整 ≠ 请求完整**，还要按 `Content-Length` 收 body |
| D9 | 发 10MB 垃圾 → 服务器 RSS 从 4MB 涨到 14MB | 没有 `MAX_HEADER_SIZE`，`read_buffer` 无限增长 |
| D10 | `Content-Length: abc` 让整个进程消失 | `std::stoul` 抛异常没人 catch → `std::terminate` |
| D11 | 两个请求一次发，只回了一个响应 | **粘包**：`\r\n\r\n` 只匹配到第一个请求，多余的被 `close` 丢弃 |
| E4 | `else { perror("read"); }` 打印 `read: Success` | 隔了逻辑分支再 `perror` → 打印**过期的 errno** |
| E5 | body 上限复用了 `MAX_HEADER_SIZE` | 头/体是两个约束，共用一个常量会误杀合法请求（8KB body 太小）|
| E6 | 小写 `content-length` 不被识别 | `find("Content-Length: ")` **大小写敏感**；RFC 9110 §5.1 规定头字段名不敏感 |
| E7 | `Content-Length:5`（冒号后无空格）不被识别 | 冒号后的 OWS 可为 0 个（RFC 9112 §5），写死一个空格就会漏 |
| E8 | `Content-Length: 10abc` 被当成 10 接受 | `stoul` 遇非数字就停 → 要先用 `isAllDigits` 卡纯数字 |
| B4 | `int fcntl(fd, F_SETFL, ...)` 编译报错 | 那是**声明变量**的语法，不是调用函数 |
| B5 | 事件循环整段不执行（死代码）| `if (epoll_ctl(...) < 0) { ... }` **少一个 `}`**，把后面全关进了错误分支 |

---

## 五、接下来做什么

- [x] **CP5a**：半包（按连接读缓冲区）+ 请求头大小上限
- [x] **CP5b**：按 `Content-Length` 收全 body + body 大小上限
- [x] **小修**：body 上限拆成独立的 `MAX_BODY_SIZE`（1MB，与 nginx `client_max_body_size` 默认值一致）
- [x] **修 3 个解析问题**：头字段名大小写（RFC 9110 §5.1）/ 冒号后 OWS（RFC 9112 §5）/ 非法值 `10abc`（回归用例 t11~t13）
- [x] **CP6a**：`epoll` 事件循环 + 非阻塞 `server_fd` + `accept` 循环到 `EAGAIN`（`epoll.cpp`，提交 `e468ac9`）
- [ ] **CP6b**：把 **client fd 纳入 epoll** ← **下一步**
      · 做法：`conn_fd` 设非阻塞 → `EPOLL_CTL_ADD` → 事件循环里用 `events[i].data.fd` 区分总机/分机
      · 验收：同时开 3 个 `nc` **都不关**，打印的 fd 应该是 **5、6、7**（而不是全是 5）
- [ ] **CP6c**：引入 `ClientContext` + `map<fd, ClientContext>`，把 CP5 的状态机搬进事件驱动
      · 核心问题：`read_buffer` 该放哪？——"下一次事件"是**另一次函数调用**，局部变量活不到那时
      · 验收：`tests/p2_test.py` 的 13 个用例**全部还要通过**（最好的回归测试）
- [ ] **CP6d**：加固 —— 头/体上限、`EPOLL_CTL_DEL` + `clients.erase` 配对、`EPOLLRDHUP`、错误隔离
- [ ] `day5.cpp`：`epoll` + 线程池（单 Reactor）；注意 fd 所有权交接的竞态
- [ ] `day6.cpp`：MySQL 连接池 + 预处理语句
- [ ] **粘包 / keep-alive**：等 CP6c 之后再做（配套：`read_buffer.erase` 消费已处理字节、按请求决定是否关闭、idle 超时）

> 为什么 keep-alive 排在 epoll 后面：阻塞模型下一个 keep-alive 连接会**永久占住**唯一的服务线程，反而放大弱点。

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

**CP5a（半包）**

13. `read_buffer` 为什么必须声明在 `while (true)` **里面**？放外面会出什么事？
14. 判据为什么是 `\r\n\r\n` 而不是 `\n\n`？这个判据是从哪份规范的哪一节来的？
15. 超限检查为什么必须放在"找判据**之后**"？
16. 客户端一直发数据、永远不发判据，会发生什么？你怎么防？

**CP5b（body）**

17. 为什么"头部收全了"不等于"请求收全了"？
18. 一个完整请求的总字节数怎么算？（`header_end` / `+4` / `content_len` 各是什么）
19. 为什么用 `read_buffer.size() >= total` 判断，而不是"再 read 一次 body 就到齐了"？
20. `std::stoul` 遇到 `Content-Length: abc` 会怎样？不加 try/catch 的后果是什么？
21. 在 `while (true)` 里用 `continue` 之前，必须先做什么？为什么？

**HTTP / 定界**

22. 半包和粘包是同一个问题的两面吗？它们各自需要什么手段？
23. 你现在是用什么方式"规避"粘包的？代价是什么？
24. 要真正支持 keep-alive，需要改哪三处？（提示：循环解析、消费缓冲区、按请求决定关闭）

**解析的严谨性（CP5 的三个修复）**

25. HTTP 头字段名区分大小写吗？你在哪份规范的哪一节看到的？
26. `Content-Length:5`（冒号后没有空格）合法吗？为什么？语法是哪一节给的？
27. `std::stoul("10abc")` 返回什么？为什么必须在它之前先做 `isAllDigits`？
28. 为什么"解析失败的测试"**不能只看"最终有没有返回 200"**？正确的判据是什么？

**CP6a（epoll 事件循环）**

29. 为什么要把 `server_fd` 设成非阻塞？不设会怎样？
30. `fcntl(fd, F_GETFL, 0)` 里那个 `0` 是什么？为什么不能省？
31. 为什么 `F_SETFL` 必须"读-改-写"三步，不能直接 `fcntl(fd, F_SETFL, O_NONBLOCK)`？
32. `epoll_event.data.fd` 是谁填的？事件回来时靠什么分辨"这个事件属于哪个 fd"？
33. `epoll_wait` 的三个返回值分别是什么？遇到 `EINTR` 该怎么处理？
34. 为什么 `accept` 要循环到 `EAGAIN`？一次事件不是只对应一个连接吗？
35. LT 和 ET 有什么区别？现在用的是哪个？为什么先用它？
36. 空闲时服务器在干什么？用哪条命令能看出"睡在 `ep_poll` 里、0% CPU"？
