# epoll 学习笔记（CP6a ~ CP6d）

> 来源：合并自两个分支会话的问答 —— epoll1（钻 API 细节）+ epoll2（问概念工程）。
> 这是一份 **查阅型笔记**：以后遇到同样的问题直接翻这里，不用再问一遍。
> 关于"我当时学到哪了"请看 `learning_progress.md`；关于"怎么跟新对话交接"请看 `HANDOFF.md`。

---

## 1. 为什么需要 epoll（附实测证据）

**阻塞式的代价**：等待会霸占整个线程。

CP3 时的实测（阻塞版 `p2.cpp`）：

```
客户端1 连上但不发数据
  → 服务器阻塞在 read(conn_fd) 上
  → 此时客户端2 请求：1047ms 后超时（curl 退出码 28）
```

**epoll 版的实测**（`epoll.cpp`，CP6a）：

```
$ ps -o pid,stat,pcpu,wchan:18,cmd -p <pid>
  PID  STAT  %CPU  WCHAN      CMD
   11   S     0.0   ep_poll    ./e
                  ↑ 空闲时线程睡在内核的 ep_poll 里，0% CPU
```

**一句话**：阻塞版是"挨个敲门，没人就站在门口等"；epoll 是"装个门铃，谁来了铃响，我才过去"。

---

## 2. 思想转变：从"阻塞等待"到"就绪通知"

| | 阻塞式 | epoll |
| --- | --- | --- |
| 心智模型 | 我主动去读；没有数据就**等着** | 先把所有连接**登记**给内核；内核告诉我**谁就绪了** |
| `read` 没数据时 | **挂起线程** | 立刻返回 -1 + `EAGAIN` |
| 谁来等 | 你的线程 | **内核** |
| 正式名字 | — | **Reactor（反应器）** |

> 注意：epoll 解决的是 **IO 等待**，不是**业务处理慢**。
> 如果回调里做阻塞操作（sleep、慢查询），整个事件循环照样卡住 —— 那是 day5 线程池要解决的。

---

## 3. 准备工作：把 fd 设为非阻塞

```cpp
// 读-改-写 三步，缺一不可
int flags = fcntl(fd, F_GETFL, 0);                  // ① 读出当前的 status flags
if (flags == -1) { perror("F_GETFL"); return 1; }
if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) { // ② 加上 O_NONBLOCK 再写回
    perror("F_SETFL"); return 1;
}
```

**三个必须理解的点：**

| 疑问 | 答案 | 依据 |
| --- | --- | --- |
| `F_GETFL` 后面那个 `0` 是什么？ | **占位符，内核忽略它** | `man 2const F_GETFL` 原文：*"F_GETFL — Return (as the function result) the file access mode and the file status flags; **arg is ignored**."* |
| 为什么必须"读-改-写"三步？ | `F_SETFL` 是**整体替换** status flags。直接写 `O_NONBLOCK` 会把别的标志（如 `O_APPEND`）**抹掉** | `man 2 fcntl` 的 F_SETFL 一节 |
| `fcntl` 的原型为什么有 `...`？ | 因为不同 `op` 需要不同的第三个参数，有的不需要 | `man 2 fcntl`：`int fcntl(int fd, int op, ...);` |

**查哪几页**：
```bash
man 2 fcntl              # 总入口，SYNOPSIS + 所有 op 的清单
man 2const F_GETFL       # 单个 op 的专页（新版 man-pages 的 2const 节）
man 2const F_SETFL
man 2 open               # O_NONBLOCK 的语义（它本来是 open 的标志）
```

**`O_NONBLOCK` 的效果**：`read` 没数据时**立刻返回 -1 + `errno = EAGAIN`**，而不是挂起线程。

---

## 4. epoll 三件套

| 调用 | 作用 | 查哪页 |
| --- | --- | --- |
| `epoll_create1(0)` | 创建 epoll 实例（**返回的也是一个 fd**） | `man 2 epoll_create` |
| `epoll_ctl(epfd, op, fd, &ev)` | 登记 / 修改 / 删除要监视的 fd | `man 2 epoll_ctl` |
| `epoll_wait(epfd, events, maxevents, timeout)` | **阻塞等事件**，返回就绪的 fd 数组 | `man 2 epoll_wait` |
| （概览） | LT vs ET、和 select/poll 的对比 | **`man 7 epoll`** ← 最该读的一页 |

### 4.1 `epoll_create1(0)`

参数 `0` 表示不加标志；可以传 `EPOLL_CLOEXEC`。见 `man 2 epoll_create`。

### 4.2 `epoll_ctl` —— 你写给内核的"标记"

```c
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *_Nullable event);
```

| `op` | 含义 |
| --- | --- |
| `EPOLL_CTL_ADD` | 把 `fd` 加入监视 |
| `EPOLL_CTL_MOD` | 修改已有 `fd` 关心的 `events` |
| `EPOLL_CTL_DEL` | **移除**（`event` 参数可以传 `nullptr`） |

**结构体（实测 `sizeof(struct epoll_event) == 12`）**：

```c
struct epoll_event {
    uint32_t     events;   // 位掩码：关心哪些事件
    epoll_data_t data;     // 联合体：.fd / .u32 / .u64 / .ptr 任选一个
} __EPOLL_PACKED;          // ← 不加 packed 会因对齐补齐到 16 字节
```

**事件掩码的值（实测）**：

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `EPOLLIN` | `0x1` | 可读（监听 socket 上 = 有新连接排队） |
| `EPOLLRDHUP` | `0x2000` | 对端关闭了写方向（half-close），**判断断开比 EPOLLIN 更早更准** |
| `EPOLLET` | `0x80000000` | 边缘触发 |
| `EPOLLOUT` / `EPOLLERR` / `EPOLLHUP` | — | 可写 / 出错 / 挂断（`ERR` 和 `HUP` 总是会被监视，不用显式登记） |

> 🔑 **`data` 的精髓**：它是**你写给内核的标记**，`epoll_wait` 事件回来时**原样带回**。
> 所以：登记时 `ev.data.fd = server_fd` → 事件回来时 `events[i].data.fd == server_fd` 就能分辨
> "这是监听套接字的事件"还是"某个客户端的事件"。
> （之所以能用 `.fd` 是因为 `epoll_data_t` 是个 union：`.fd` / `.u32` / `.u64` / `.ptr` 任选。）

### 4.3 `epoll_wait` —— 返回值三态

```
> 0  ：就绪的 fd 个数（events[0..nfds-1] 有效）
= 0  ：超时（不是错误）
-1  ：出错，errno 被设置（EINTR = 被信号打断，应当 continue 重试）
```

`man 2 epoll_wait` RETURN VALUE 原文：
> *"On success, epoll_wait() returns the number of file descriptors ready for the requested I/O operation, or zero if no file descriptor became ready during the requested timeout milliseconds. On failure, epoll_wait() returns -1 and errno is set."*

`timeout` 的三种用法：`-1` = 永久等（0% CPU）｜`0` = 立即返回｜`>0` = 等这么多毫秒。
`maxevents` 必须大于 0。

---

## 5. 事件循环模板（可直接抄）

```cpp
constexpr int MAX_EVENTS = 16;
struct epoll_event events[MAX_EVENTS];

while (true) {
    int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    if (nfds < 0) {
        if (errno == EINTR) continue;   // 被信号打断：重试，不是错误
        perror("epoll_wait");
        break;
    }

    for (int i = 0; i < nfds; i++) {
        int fd = events[i].data.fd;     // 登记时你填进去的标记

        if (fd == server_fd) {
            // ---- 新连接：必须循环 accept 到 EAGAIN ----
            // 一次事件可能对应队列里【多个】刚到达的连接
        } else {
            // ---- 已有连接：循环 read 到 EAGAIN ----
            // 一次事件可能有多批数据（ET 下不读完会丢事件）
        }
    }
}
```

---

## 6. `EAGAIN` 与错误分类

### 非阻塞 `read` 的**四态**（比阻塞版多一种）

| 返回 | 含义 | 该做什么 |
| --- | --- | --- |
| `> 0` | 读到的字节数 | 追加到缓冲区，**继续读** |
| `0` | EOF：对端正常关闭 | 关闭连接 |
| `-1` + `errno == EAGAIN` | **暂时没数据了（正常！）** | `break` 退出读循环 |
| `-1` + 其他 errno | 真错误 | 打日志 + 关闭连接 |

**`EAGAIN` 不是错误** —— 它是"这一轮读完了"的信号。

### 从哪找这些错误码？

**去那个调用的 `ERRORS` 一节。** 例如 `man 2 read` 的 ERRORS 里有：

> *EAGAIN — The file descriptor fd refers to a file other than a socket and has been marked nonblocking (O_NONBLOCK), and the read would block.*
> *EAGAIN or EWOULDBLOCK — The file descriptor fd refers to a socket and has been marked nonblocking (O_NONBLOCK), and the read would block.*

`EAGAIN` 和 `EWOULDBLOCK` 在 Linux 上是**同一个值**，所以 `errno == EAGAIN || errno == EWOULDBLOCK` 这种写法是**可移植写法**（POSIX 允许两者不同）。

### 错误三分类（和前面几轮一致）

| 类别 | 例子 | 处理 |
| --- | --- | --- |
| 可重试 | `EINTR` | 重试（`continue`） |
| 可恢复 / 正常 | `EAGAIN`、`EMFILE` | `break` 或特殊处理 |
| 编程错误 | `EINVAL`、`EFAULT` | 打日志，别硬撑 |

---

## 7. 水平触发（LT）vs 边缘触发（ET）

| | LT（水平触发，**默认**） | ET（边缘触发，要显式加 `EPOLLET`） |
| --- | --- | --- |
| 通知条件 | **只要还有数据没读完**，下次 `epoll_wait` 还会通知 | 只在**状态变化**时通知一次 |
| 读的姿势 | 可以一次只读一点 | **必须循环读到 `EAGAIN`**，否则数据会永远读不到 |
| 编程难度 | 低 | 高（容易漏读） |
| 效率 | 略低（可能重复通知） | 略高（通知次数少） |

**结论：先用 LT**（什么都不用做，默认就是 LT）。等你把 LT 版跑通、理解了事件循环，再试 ET。

---

## 8. 必踩的 6 个坑

| # | 坑 | 后果 | 对策 |
| --- | --- | --- | --- |
| 1 | 把 `read_buffer` 留在**局部变量**里 | 下一次事件读不到上次的数据 → 永远收不到完整请求 | 放进 `ClientContext`，用 `map<fd, ClientContext>` 保存 |
| 2 | `accept` 只调**一次**（没循环到 EAGAIN） | 并发连接时漏掉一部分 | 循环 accept 到 `EAGAIN` |
| 3 | `read` 只调**一次**（没循环到 EAGAIN） | 数据残留、半包处理错乱、ET 下丢事件 | 循环 read 到 `EAGAIN` |
| 4 | `close(fd)` 后忘了 `clients.erase(fd)` | **新连接拿到同一个 fd 号 → 读到上一条连接的残留状态** | close 和 erase 永远配对 |
| 5 | 在事件回调里做**阻塞操作** | 整个事件循环卡住，所有连接一起卡 | 慢业务交给线程池（day5） |
| 6 | `close` 前不 `EPOLL_CTL_DEL` | fd 被复用后 epoll 里还挂着旧登记 → 诡异 bug | close 前先 DEL |

> 坑 4 的实测证据：CP6a 里连续 3 次连接，服务器打印的 fd **全是 5** ——
> 因为立刻 `close(conn_fd)`，号码被立刻复用。**fd 号 ≠ 连接的所有权。**

---

## 9. 路线图：CP6a → CP6d

| 步骤 | 内容 | 状态 |
| --- | --- | --- |
| **CP6a** | 事件循环 + 非阻塞 `server_fd` + `accept` 循环到 `EAGAIN` | ✅ 完成（`e468ac9`） |
| **CP6b** | 把 **client fd 纳入 epoll**（设非阻塞 + `EPOLL_CTL_ADD`），事件循环里区分 `server_fd` / client fd | ✅ 完成（`a9e7494`） |
| **CP6c-1** | 引入 `ClientContext` + `unordered_map<int, ClientContext>` 账本（**行为不变**） | ✅ 完成（`5392991`） |
| **CP6c-2** | 把 CP5 的**状态机搬进来**，回真正的 HTTP 响应（`inbuf` + `tryHandleRequest`） | ✅ 完成（验收：`p2_test.py` 23/23 对着 `./epoll`） |
| **CP6d-1** | 加固①：`EMFILE` 忙等 —— 撞 fd 上限后**暂停接收**而不是空转 | ✅ 完成（验收：`emfile_test.py` 2/2） |
| **CP6d-2** | 加固②：**keep-alive** —— 一条连接服务多个请求 | ✅ 完成（验收：`keepalive_test.py` 7/7） |
| **CP6d-3** | 加固③：**部分写**（`send` 返回 < n → 输出缓冲 + `EPOLLOUT`） | ⬜ 下一步（现在撞不到，见下） |

### CP6d-1：`EMFILE` —— "接不了新连接"时该怎么办

**这不是"处理一个错误码"，是"处理一个状态"。**

坏版本实测（`ulimit -n 64`，fd 灌满后静置）：

| | 数字 |
| --- | --- |
| `EMFILE` 触发 | **2,052,391 次** |
| CPU | **67~75%，状态 `R`**（一直在跑，不是在睡） |
| 3 秒日志 | **+1,626,534 行**（≈43 MB，同一个文件 append） |
| **已经在线的用户** | **`TimeoutError: timed out`** ← 连它们也服务不了 |

**死循环怎么来的**：`accept` 返回 `EMFILE` → `break` → 回 `epoll_wait` →
**但 `server_fd` 还在 epoll 里、accept 队列里还排着人 → 它依然"可读"** → 立刻又返回 →
又 `accept` → 又 `EMFILE`…… 中间**没有任何等待**，因为条件永远成立。

**解法（暂停 / 恢复）**：

```cpp
if (errno == EMFILE) {                    // per-process（ENFILE 才是整机）
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, &ev);   // 把话筒摘了
    accept_paused = true;
    paused_at     = clients.size();       // 记下暂停那一刻的账本大小
    break;
}
// 每轮 epoll_wait 【之前】问一句：账本比暂停时小了吗？（= 腾出 fd 了吗）
if (accept_paused && clients.size() < paused_at) {
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);   // 把话筒放回去
    accept_paused = false;
}
```

三个**不能动**的点：

| 点 | 错了会怎样 |
| `errno == EMFILE` | 写成 `ENFILE`（整机上限）→ 永远进不来这个分支，**空转照旧**（实测日志 109 万行） |
| 恢复判断放在 `epoll_wait` **之前** | 放在"刚暂停那一轮"的末尾 → 同一轮里 `暂停 → 恢复 → 再暂停`，**又变回死循环** |
| `paused_at = clients.size()` | 填 `0` → `size_t` 无符号，`size() < 0` 永远不成立 → **摘了话筒再也放不回去**（实测 `[2] FAIL 连不上`，而 `[1]` 照样绿 —— 所以要两个用例） |

**这个坑和 `EAGAIN` 是同一个形状**：非阻塞 fd 上"现在做不了"时，
**正确反应都是"去等一个事件"，而不是"原地重试"**。

### CP6d-2：keep-alive —— 一条连接服务多个请求

| | 现在（`Connection: close`） | keep-alive |
| --- | --- | --- |
| 一个连接服务几个请求 | **1 个** | 多个 |
| 200 个请求留下的 `TIME_WAIT` | **402 个**（每个 2 个，各占 60 秒） | 2 个 |
| 握手次数 | 每个请求 1 次 | 总共 1 次 |

**改动就三处**（都在 `tryHandleRequest` 和它的调用处）：

1. `ClientContext` 加 `bool want_close = false;` —— 第三个状态："回完这个请求要不要挂断"
2. 读完 `Connection` 头决定它（值可能是 `close` / `keep-alive` / `Close`… → 先 `toLowerAscii` 再比）
3. **处理完一个请求就 `ctx.inbuf.erase(0, total)`** —— 把用掉的字节消费掉

**为什么第 3 步不能省**：不删的话，下次事件进来 `find("\r\n\r\n")` **又找到同一个请求** →
把它再回一遍，排在后面的请求永远轮不到。

**调用处要循环**（这是"粘包"的另一半）：

```cpp
while (tryHandleRequest(ctx)) {       // true = 处理掉了一个；false = 还不够
    if (ctx.want_close) { close(fd); clients.erase(fd); gone = true; break; }
    // 不关 → 回 while 顶部：inbuf 里可能还躺着下一个请求
}
```

> ⚠ **坑**：`bool want_close = true;` 是**新建一个局部变量**（行首有类型 = 声明），
> 改不到 `ctx` 里的成员；`ctx.want_close = true;` 才是赋值。
> 编译器**只给 warning**（`unused variable`），因为"声明一个没用过的变量"是合法语句。
> **对警告不能当噪音。**

> ⚠ **坑**：读 `Connection` 时**别整段照抄**读 `Content-Length` 的代码。
> 两者只有前半截像（都是 `findHeader` 取值），后半截完全不同 ——
> 那个要 `isAllDigits` / `stoull` / `MAX_BODY_SIZE`（**把数字串转成整数**），
> 这个只要**比字符串**。抄多了的后果：日志刷 109 万行 `Connection 非法（不是纯数字）`。
> **"照抄形状"可以，但抄进来的每一行都得知道在干嘛。**

**`Connection` 头的规范依据**：RFC 9112 讲 Tear-down 的那一节（`Connection: close` 表示回完就关）。
HTTP/1.1 **默认是复用**，所以"没写这个头"= 继续保持。

### CP6d-3：部分写为什么现在不动

| | 字节数 |
| --- | --- |
| 一次响应 | **117** |
| 内核发送缓冲区 | **16384** |

117 ≪ 16384 → **撞不到**。按 TDD 规矩"**写不出会红的测试，就别改代码**"，
等响应变大 / 长连接压测时再回来（那时要加输出缓冲 + `EPOLLOUT`）。

**部分写本身是真的**（实测：想 `send` 1048576 字节 → 第一次只出去 **32741**，第二次 **47616**，第三次 `EAGAIN`）。

**CP6b 的验收**：当时的 `cp6b_test.py` → **4/4 全绿**
（[1] echo / [2] 分多次到达 / [3] 并发不串台 / [4] **fd 配平**）

> 📌 这个文件后来改名 `tests/balance_test.py`：CP6c-2 把 echo 契约换成了真正的 HTTP，
> [1][2][3] 随之退役 —— 但 **[4] fd 配平与协议无关**，跨了两次重构依然成立，所以留下并扩成了
> "fd + 账本"两本账的配平测试（新增 [4] 哨兵没响）。详见 `learning_progress.md` 的「两个轴」。

> ⚠ **这条判据是从一个"假绿"改过来的，别改回去。**
>
> 原来的写法是"同时开 3 个 `nc` 都不关，服务器打印的 fd 应该是 5、6、7"。
> 实测发现：`close(conn_fd)` 被注释掉、fd 全泄漏时，**照样打印 5、6、7**。
>
> | | 泄漏版 | 配平版 |
> | --- | --- | --- |
> | 3 个 nc 同时开 | 打印 5、6、7 ✅ | 打印 5、6、7 ✅ |
> | 跑 20 条连接后 | fd 11 → 31 | fd 5 → 5 |
>
> **教训：验收标准本身也要被验证 —— 要问"反例能不能通过"。**

**CP6c 的核心问题**：`read_buffer` 现在该放哪？
答案是 `struct ClientContext { int fd; std::string inbuf; };`
放在 `unordered_map<int, ClientContext>` 里 —— 因为"下一次事件"是**另一次函数调用**，
局部变量活不到那时。**CP6c-1 已经把"账本 + 空壳对象"这套机制验证过了，
CP6c-2 把 `inbuf` 真正用起来了。**
**这就是"状态机切片"**：把一个连续执行的函数，改造成"每次事件恢复一点进度"。

**CP6c-2 的切片点**（和 `p2.cpp` 对照着看最清楚）：

| | `p2.cpp`（阻塞版） | `epoll.cpp`（事件驱动） |
| --- | --- | --- |
| 收到一部分 | `while (!header_done) { read... }` **堵着等** | `ctx.inbuf.append(...)` 攒起来 |
| 还不够一个请求 | 循环继续 | **`return false`**，函数退出，进度留在 `inbuf` |
| 下次怎么接着算 | 不用管（没退出过） | 下一个 `EPOLLIN` 事件**重新进函数**，重头算一遍 `total` |
| 收齐了 | 就地处理 + 响应 | 就地处理 + 响应 + `return true`（调用方收连接） |

**CP6c-1 额外踩到的**：从这里开始有**两本账**（fd 号 + 账本条目）。
漏 `erase` 时编译零警告、**`p2_test.py` 23 项照样全绿、fd 也是平的** ——
所以加了个哨兵，而且**哨兵必须放在 `emplace` 那边**（放 `erase` 那边永远喊不出来）。
详见 `notes/learning_progress.md` 的「两个轴」与自检问题 49/50、以及本文件 §11.3。

---

## 10. 速查表：哪个 API 查哪一页 man

| 要查什么 | 命令 |
| --- | --- |
| fcntl 的 op 清单 | `man 2 fcntl` |
| F_GETFL / F_SETFL 的细节 | `man 2const F_GETFL` / `man 2const F_SETFL` |
| O_NONBLOCK 的语义 | `man 2 open` |
| 创建 epoll | `man 2 epoll_create` |
| 登记 / 删除 | `man 2 epoll_ctl` |
| 等事件 | `man 2 epoll_wait` |
| **LT vs ET、和 select/poll 的对比** | **`man 7 epoll`** |
| EAGAIN / EINTR 从哪来 | 各调用的 **ERRORS** 一节（如 `man 2 read`） |
| epoll_event 结构体 | **`man 3type epoll_event`**（含 `epoll_data_t` union 的完整定义）<br>或头文件 `/usr/include/x86_64-linux-gnu/sys/epoll.h`（grep 最快） |
| socklen_t 是什么 | **`man 3type sockaddr`**（和 sockaddr 共用一页） |

---

## 11. 三个还没完全闭环的点

### 11.1 `socklen_t` **不是结构体**，是 typedef

之前问的是"socklen_t 这个结构体从哪来"。实测：

```
/usr/include/x86_64-linux-gnu/bits/socket.h:33
    typedef __socklen_t socklen_t;          ← 整数类型，不是 struct
```

它是一个**整数类型**，表示"地址结构体有多少字节"。这也是为什么：
- `accept` 要传 `&client_len`（要写回）
- `addrlen` 是**值-结果参数**（传进去是缓冲区大小，返回时被改写成实际大小）

**手册在 `man 3type sockaddr`**（sockaddr / sockaddr_storage / socklen_t 共用一页），
没有单独的 `man 3type socklen_t`。

### 11.2 "总机 / 分机"的隐喻（CP6b / CP6c 全靠它）

```
客户打 010-1234（公司总机）
  → 总机接线员接通
  → 转接到 801 分机
  → 你和具体某个人通话
  → 挂断，801 释放，可以再分配给下一通电话
```

- **总机 = `server_fd`**：一个固定号码，永久存在，只负责"接待"
- **分机 = `conn_fd`**：每次通话一个新的，通话结束就释放，**号码会被复用**
- 对应到代码：`server_fd` 全程不关；`conn_fd` 每条连接一个，用完 `close`
- 对应到 epoll：两个 fd **都要登记**（总机关心"新连接"，分机关心"可读"）
- 对应到 CP6c：`conn_fd` 还要作为 `clients` 这个 map 的 **key**

### 11.3 `conn_fd` 泄漏是**分阶段收敛**的，不是"学一个知识点就解决"

| 阶段 | 手段 | 状态 |
| --- | --- | --- |
| ① **手工配平** | 保证每条路径都 `close`（例如"先 `close` 再 `continue`"） | ✅ CP6b 完成 |
| ② **`close` / `erase` 配对** | `close(fd)` 和 `clients.erase(fd)` 永远一起出现 | ✅ CP6c-1 完成<br>✅ CP6c-2 补齐（`tryHandleRequest` 成功路径也要配对） |
| ③ **RAII** | 把 fd 包进一个类，析构函数自动 `close` | ⬜ 以后（C++ 进阶） |

**检测手段也在升级：**

| 阶段 | 怎么发现泄漏 |
| --- | --- |
| 最早 | 手工 `ls /proc/<pid>/fd \| wc -l`，反复请求 1000 次看它涨不涨 |
| CP6b 起 | **`tests/balance_test.py` 的 `[1][2][3]`** —— 自动比对 fd 基线，泄漏的实现过不了 |
| CP6c 起 | ⚠ 多了**第二本账**（账本条目），而 fd 测试**看不见**它 → 靠**哨兵**<br>（`balance_test.py` 的 `[4]` 把"哨兵有没有响"也变成了自动判据） |

**CP6c 的哨兵（第二本账唯一的报警器）：**

```cpp
// 主哨兵：放在 emplace 那边，不是 erase 那边！
if (!clients.emplace(conn_fd, ctx).second) {
    std::cerr << "账本里已经有 fd=" << conn_fd << " —— 上一个连接漏了 erase" << std::endl;
}
```

**为什么必须在 `emplace`**：漏了 `erase` 之后，旧条目还躺在账本里；
下一个连接复用同一个 fd 时，`erase` 照样能删掉它（返回 1）—— 所以 **`erase` 那边永远喊不出来**。

**实测**：故意删掉所有 `erase` → `p2_test.py` **23 项照样全绿、fd 也是平的**（编译也零警告），
但哨兵报了 **232 次**。**测试看不见的 bug，只有哨兵能抓。**
