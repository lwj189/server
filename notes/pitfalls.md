# 踩坑记录（CP1 → CP6d）

> 这份文档记录的是**我在手写 `p2.cpp` 时真实撞过的坑**，不是"常见错误大全"。
> 每条都包含：**现象（报错原文）→ 根因 → 解决 → 怎么验证**。
> 遇到问题先在下面的"症状速查"里找编号，再跳过去看根因。
>
> 维护方式：以后每踩一个新坑，就往对应的分类里加一条，并在速查表里登记。

---

## 症状速查

| 你看到的 | 去哪条 |
| --- | --- |
| `redefinition of 'struct sockaddr_in'` | A1 |
| `'protocol' was not declared in this scope` | A1 |
| `unused variable 'tcp_socket'` | A1 |
| `man -k xxx` 输出几十条，不知道选哪个 | A2 |
| 在 man 页里**找不到** `RETURN VALUE` | A3 |
| 看不懂 `ERRORS` 一节 | A4 |
| 不知道 `htons` / `INADDR_ANY` 在哪一页 | A5 |
| 不确定"找到的这段是不是我要的答案" | A6 |
| `setsockopt` 返回 -1 / `errno = EINVAL` | B1 |
| `too many arguments to function 'int pause()'` | B2 |
| `error: 'response' was not declared in this scope` | B3 |
| 改了源码，运行行为没变 | C1 |
| VSCode 里编辑了，但文件内容没变 | C2 |
| `xxx.cpp: 没有那个文件或目录` | C3 |
| `./p2 > log.txt` 之后日志是空的 | C4 |
| 代码里 `using namespace std;` 和 `std::` 混用 | C5 |
| `bind: Address already in use` | D1 |
| 退出后端口还被占着 | D2 |
| `./p2` 跑起来"什么都没有" | D3 |
| 客户端分两段发，服务器只收到前半段 | D4 |
| 一个不发数据的客户端让整个服务器卡住 | D5 |
| `send: Connection reset by peer` / `Broken pipe` | D6 |
| 每次连接的 fd 都是同一个数字 | D7 |
| 没收到请求，服务器还是"回"了响应 | E1 |

---

## F. CP5 ~ CP6d 的坑（HTTP 解析 / epoll / keep-alive / EMFILE）

> 这一段先只记「**现象 → 根因**」，每条都有实测数据支撑。
> 更细的机制见 `epoll_notes.md`，进度脉络见 `learning_progress.md`。

| 现象 | 根因 |
| --- | --- |
| `int fcntl(...)` 报错 | 那是**声明变量**的语法，不是调用函数 |
| 少一个 `}` → 整段成了死代码 | C++ 里花括号位置决定控制流，写完要检查缩进层级 |
| 小写 `content-length` 没被识别 | 头字段名大小写不敏感（已修，回归用例 t11/t12） |
| `Content-Length: 10abc` 被当成 10 | `stoul` 遇非数字就停（已修，回归用例 t13） |
| **测试通过了，但功能其实是坏的** | **假绿**：判据对"要测的那件事"不敏感。例："3 个 nc 都不关、该打印 fd 5/6/7" 在 `close(conn_fd)` 被注释掉、fd 全泄漏时**同样成立**。→ 判据也要反过来问"**反例能不能通过**" |
| 明明 kill 了还报 `Address already in use` | `p2` 和 `epoll` **都监听 8888**，同时只能开一个（`ss -tlnp \| grep 8888` 找 pid） |
| 以为 `-Wconversion` 会抓符号转换 | **C++ 下不含**符号检查（C 下含）。实测同一段代码：`gcc -Wconversion` 报，`g++ -Wconversion` 不报 → 要显式加 `-Wsign-conversion` |
| `F_SETFL` 直接传 `O_NONBLOCK` | `F_SETFL` 是**整体替换**不是追加 → 会抹掉原有标志位（实测 `O_APPEND` 被抹掉）→ 必须 `F_GETFL` → `\|` → `F_SETFL` 三步读-改-写 |
| 以为 `conn_fd` 会自动继承 `server_fd` 的非阻塞 | `accept` 出来的新 socket **不继承** file status flags（`man 2 accept` 明说，且注明与 BSD 不同）→ 每条连接都得自己设一遍 |
| 撞 fd 上限后 CPU 打满、日志暴涨 | `accept` 返回 `EMFILE` 后 `break`，但 LT 模式下监听 fd 仍然"可读" → **忙等死循环**。实测（CP6d-1 坏版本）静置 2 秒 **+1,098,114 行**、CPU 75%、**连已经连上的用户都超时**。已修：撞墙时 `EPOLL_CTL_DEL` 把 `server_fd` **摘掉**，等腾出 fd 再 `EPOLL_CTL_ADD` 加回去 |
| 恢复判断放错位置 | **必须在 `while` 循环里面** —— 放外面 = 这段永远不执行 → 摘了话筒再也放不回去（实测 `[2]` 报"连不上"，而 `[1]` 照样绿）。放"`epoll_wait` 之前"和放"循环体末尾"实测**等价**（两个都 2/2） |
| **注释里写的原因也可能是猜的** | 上面那条原来写的是"放末尾会 `暂停→恢复→再暂停` 又变死循环" —— **实测推翻**。这已经是本项目第二次了（第一次是"同批陈旧事件"，实测也不存在）→ **注释里的因果也要验** |
| `EMFILE` 写成了 `ENFILE` | 前者是**本进程** fd 用光，后者是**整机**所有进程用光。`ulimit -n` 改的是 per-process → 撞出来的是 `EMFILE`。填错的表现：暂停分支永远进不去，**空转照旧**（实测日志 109 万行） |
| `paused_at` 记成 `0` | 恢复判据是 `clients.size() < paused_at`，而 `size()` 返回 **`size_t`（无符号）** → `0 < 0` 永远不成立 → **话筒摘了就再也放不回去**。实测 `[2]` 报"连不上"，但 `[1]` 照样绿（所以要两个用例） |
| `bool want_close = true;` 改不到成员 | **行首有类型 = 新建一个局部变量**；`ctx.want_close = true;` 才是给成员赋值。编译器**只给 warning**（`unused variable`），因为"声明一个没用过的变量"是合法语句 → **警告不是噪音** |
| 读 `Connection` 时整段照抄 `Content-Length` | 两者只有**前半截**像（都是 `findHeader` 取值）。后半截 `isAllDigits` / `stoull` / `MAX_BODY_SIZE` 是"**把数字串转成整数**"，对 `"close"` 永远为假 → 每个带 `Connection` 头的请求都被判"非法"丢弃。实测日志刷 **109 万行** `Connection 非法（不是纯数字）`。**照抄形状可以，但抄进来的每一行都得知道在干嘛** |
| keep-alive 下 `inbuf` 不消费 | 处理完不 `erase` → 下次事件 `find("\r\n\r\n")` 又找到**同一个请求** → 回两遍，排在后面的请求永远轮不到（`[2]` 抓） |
| keep-alive 的测试用两个**一模一样**的请求 | **假绿**：服务器把第一个请求回了两遍、压根没看第二个，照样全绿 → 改成 `/first` + `/second` 两个不同路径，并**读服务器日志**确认两个都真被处理 |
| 在 `/tmp` 里编译好、下一条命令就没了 | `/tmp` **跨命令会被清掉** → 报 `Connection refused` 之类莫名其妙的错。要跨命令用的东西放项目目录里 |

---

## A. 手册与资料查询类

### A1 把 man 页的内容**当代码抄进 `.cpp`**

**现象**

```
error: redefinition of 'struct sockaddr_in'
  /usr/include/netinet/in.h:249: note: previous definition of 'struct sockaddr_in'
error: redefinition of 'struct in_addr'
  /usr/include/netinet/in.h:31:  note: previous definition of 'struct in_addr'
error: 'protocol' was not declared in this scope
warning: unused variable 'tcp_socket' / 'udp_socket' / 'raw_socket'
```

**根因**

man 页里的东西性质完全不同，我一股脑全抄了：

| man 页里的内容 | 性质 | 该怎么做 |
| --- | --- | --- |
| `#include <sys/socket.h>`（SYNOPSIS 的 include 行） | **原料** | ✅ 抄 |
| `int socket(int domain, int type, int protocol);` | **菜单**：声明，头文件里早有 | ❌ 不要抄 |
| `struct sockaddr_in { ... }`、`typedef ...` | **说明书**：讲字段名和含义 | ❌ 不要抄，用来知道该填哪些字段 |
| `tcp_socket = socket(AF_INET, SOCK_STREAM, 0);` 那几行 | **举例**：三种用法的样例 | ❌ 挑一个用，不是全抄 |

**解决**

- 只抄 **include**；结构体只用来**读字段名**（`sin_family` / `sin_port` / `sin_addr`）
- 函数原型不用写，include 之后直接调用
- 验证：删掉所有抄来的结构体/原型，程序照样编译通过（证明它们本来就在头文件里）

```
$ g++ -std=c++17 -Wall minimal.cpp -o minimal
退出码 = 0        ← 只留 include，一样能编译
```

**教训**：**SYNOPSIS 的 include 是原料，函数原型是菜单，DESCRIPTION 的结构体是说明书——只有原料要拿进后厨。**

---

### A2 `man -k bind` 输出几十条，不知道选哪个

**现象**：`man -k bind` 列出 `bind(2)`、`mbind(2)`、`ldap_bind(3)`、`bindtextdomain(3)`、`netplan-rebind(8)`、`Glib::*(3pm)`…

**根因**：`man -k`（= `apropos`）搜的是**所有手册页的 NAME 行（名字 + 一句话描述）**，而且是**子串匹配**，所以凡含 "bind" 的都被捞出来。

**解决**

```bash
man -k bind | grep '(2)'     # 只留第 2 节（系统调用）
man -k -s 2 bind             # 限定节号
man -k '^bind$'              # 正则精确匹配名字 → 只剩一行
```

判断一个结果要不要，只看两样：**括号里的节号** + **破折号后面的描述**。

**更重要的：知道名字时根本不用 `-k`**

```
知道名字 + 系统调用 → man 2 名字
知道名字 + C 库函数 → man 3 名字
知道名字 + 结构体   → man 3type 名字
知道名字 + 协议常量 → man 7 协议名
不知道名字         → man -k '关键词'   ← 这才是 -k 的用途
```

例：`man -k 'byte order'` → 找到 `byteorder(3)`（`htons` 就在那）。

---

### A3 在 `man 2 socket` 里**找不到 `RETURN VALUE`**

**现象**：翻来翻去只看到大段 DESCRIPTION，"这页好像没有 RETURN VALUE"。

**根因**（三个可能，我全中了）

1. 分页器被设成 `cat`/`more` → 整页一次刷过去，**没有搜索和翻页功能**
2. `less` 的搜索**区分大小写**：搜 `/return value` 匹配不到大写的 `RETURN VALUE`
3. 搜 `ERRORS` 跳过去之后**只往下看** —— 而 `RETURN VALUE` 就在它**上面 4 行**

实测 `man 2 socket` 共 **224 行**：`DESCRIPTION` 从第 14 行开始占了 140 多行，`RETURN VALUE` 在第 **158** 行、`ERRORS` 在 **162** 行 —— 都在页面**最后三分之一**。

**解决**

```bash
echo $PAGER                  # 先确认你的分页器是什么
man -P less 2 socket         # 单次强制用 less（进去后 /RETURN VALUE 搜索）

# 目录法：先看有哪些节、各在第几行
man 2 socket | grep -n '^[A-Z][A-Z ]*$'

# 抽取法：不跟分页器较劲（最推荐）
man 2 socket | sed -n '/^RETURN VALUE/,/^ERRORS/p'
man 2 socket | sed -n '/^ERRORS/,/^VERSIONS/p'
```

**教训**：读 man 是**查字典**不是读小说。先用目录法知道结构，再抽取要的节。

---

### A4 看不懂 `ERRORS` 一节

**现象**：一堆 `EACCES` / `EINVAL` / `EMFILE`，不知道在说什么。

**根因**：没建立 **errno 机制**的心智模型。

**解决：ERRORS 是一张"故障字典"，不是待办清单**

机制：

```
调用 socket(...) 失败 → 返回 -1，同时内核把全局整数 errno 设成某个数字
那个数字的"名字"就是 ERRORS 里列的符号（如 24 = EMFILE）
名字后面那句英文 = "什么情况下会出现这个错误"
```

代码形态：

```cpp
#include <cerrno>
#include <cstring>

if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");                   // 打印 "socket: " + strerror(errno)
    if (errno == EMFILE) { /* 只有能补救的才补救 */ }
    return 1;
}
```

三类处理策略：

| 类别 | 例子 | 该做什么 |
| --- | --- | --- |
| 可重试 | `EINTR` | 重试 |
| 可恢复/可降级 | `EMFILE`、`ENFILE`、`ENOBUFS` | 特殊处理（关闲置 fd、降级、稍后重试） |
| 编程错误（不可恢复） | `EINVAL`、`EAFNOSUPPORT` | 打日志、退出 |
| 其余 | — | `perror` 记一笔就够 |

注意：**同一个 errno 可能出现在多个条目里**（`man 2 bind` 的 `EADDRINUSE` 有两条）；ERRORS 结尾常有 *"Other errors may be generated by the underlying protocol modules."* —— **列表不是全集**。

---

### A5 不知道 `htons` / `INADDR_ANY` 在哪一页（差点只靠自动补全）

**根因**：**VSCode 自动补全只给名字，不给语义**。它能同时弹出 `htons` 和 `ntohs`，选错方向照样编译通过。

**解决：四条查询路径**

| 要找什么 | 去哪 | 例子 |
| --- | --- | --- |
| 系统调用 | `man 2 名字` | `socket` / `bind` / `accept` |
| **C 库函数** | `man 3 名字` | **`htons` → `man 3 byteorder`**（注意是第 3 节，不是第 7 节） |
| 结构体/类型 | `man 3type 名字` | `sockaddr_in` |
| 协议/常量/特殊值 | `man 7 协议` | `INADDR_ANY` → `man 7 ip` |
| 不知道页名 | `man -k 关键词` | `man -k htons` |
| 都不确定 | `grep -rn '名字' /usr/include/`（终极兜底） | `grep -rn 'INADDR_ANY' /usr/include/netinet/in.h` |

方向口诀（`man 3 byteorder`）：

- `htons` / `htonl` = **host → network**（填进去 / 发出去时用）
- `ntohs` / `ntohl` = **network → host**（读出来打印 / 比较时用）
- 16 位用 `s`（端口），32 位用 `l`（IPv4 地址）

**反面教材**：打印 `client_addr.sin_port` 忘记 `ntohs`，会打印出 **13986** 这种数字而不是真实端口。

---

### A6 不确定"找到的这段是不是我要的答案"

**解决：用参数名当索引**

man 的编排规律是：**SYNOPSIS 给参数名字，DESCRIPTION 用这些名字来讲解**。

```
① 抄下 SYNOPSIS 的函数签名
② 挑出你不确定的那个参数名
③ 在页内搜 /那个名字
```

例：`bind(server_fd, (struct sockaddr*)&address, ____)` 的第三个空 → `man 2 bind` 里第三个参数叫 `addrlen` → 搜 `/addrlen` → 找到：

> *"addrlen specifies the size, in bytes, of the address structure pointed to by addr."*

→ 那就是答案（填 `sizeof(address)`）。而且 ERRORS 里有 `EINVAL addrlen is wrong`，证明它必须填对。

---

## B. 语法与 API 类

### B1 `bool reuse` 让 `setsockopt` 返回 -1 / `EINVAL`

**现象**

```
setsockopt 返回值 = -1  (0=成功, -1=失败)
失败原因: Invalid argument          ← errno = EINVAL
```

**根因**：`optval` 是 `void*` + 长度，**内核靠长度判断类型**。`SO_REUSEADDR` 要求 `optlen >= sizeof(int)`，而 `bool` 只有 1 字节。

依据：`man 7 socket` 里 SO_REUSEADDR 条目最后一句 —— *"Argument is an **integer** boolean flag."*（integer 是硬要求）

**解决**：用 `int`

```cpp
int reuse = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
```

**教训**：选项设置"莫名其妙失败"时，**第一个怀疑对象是值类型/长度**。以后设 `SO_LINGER`（要 `struct linger`）、`SO_RCVTIMEO`（要 `struct timeval`）同理——去 man 里看 "Argument is …"。

---

### B2 `pause("system")` 编译报错

**现象**

```
error: too many arguments to function 'int pause()'
```

**根因**：记忆串台了。`system("pause")` 是 **Windows** 的写法（让 cmd 执行内建命令 `pause`）；Linux 上没有这个命令，`sh: 1: pause: not found`。

**解决**

`man 2 pause` 的原型是 `int pause(void);` —— **不收任何参数**：

```cpp
pause();        // 挂起进程，直到收到信号（Ctrl+C 退出）
```

Linux 上让程序停住的几种写法：

```cpp
pause();                  // ✅ 推荐
getchar();                // 等一个字符
sleep(3600);              // 睡一小时
while (true);             // ❌ 空转，跑满一个 CPU 核心
```

---

### B3 块作用域：`response` 在 `if` 里声明、`send` 在外面

**现象**

```
error: 'response' was not declared in this scope
```

**根因**：**C++ 的块作用域**。一对 `{}` 就是一个作用域，在里面声明的变量出了 `}` 就销毁。

> 类比：花括号是房间，变量是房间里的人。出了房间，外面当然找不到。

**解决**：把"处理这一个请求"的**所有东西**放进同一个作用域：

```cpp
if (n > 0) {
    std::string req(buffer, n);
    std::cout << "收到请求:\n" << req << std::endl;

    std::string body = "<h1>Hello from my own server!</h1>";
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;

    ssize_t sent = send(conn_fd, response.c_str(), response.size(), 0);
    if (sent < 0) { perror("send"); }

} else if (n == 0) {
    std::cout << "客户端主动关闭了连接" << std::endl;
} else {
    perror("read");
}

close(conn_fd);      // close 留在外面：所有分支都要关连接
```

**顺带记住作用域的层级**：

| 变量 | 声明位置 | 谁能用 | 活多久 |
| --- | --- | --- | --- |
| `server_fd` | `main` 里 | 整个 main | 整个程序 |
| `conn_fd`、`client_addr`、`ip` | `while` 里 | 这一轮循环 | 一条连接 |
| `req`、`body`、`response`、`sent` | `if (n > 0)` 里 | 只有这个分支 | 一次请求 |

---

## C. 编译与工作流类

### C1 改了源码，运行行为没变（**没重新编译**）

**现象**：明明改了 `p2.cpp`，`./p2` 跑出来还是老样子（甚至"什么都没有"）。

**根因**：C++ 是**编译型**语言。`p2.cpp` 是图纸，`p2` 是上次造好的机器——**改图纸不重新造，机器不会变**。

时间戳实锤：

```
p2       16:07:48   ← 二进制
p2.cpp   16:09:07   ← 源码（比二进制晚 1 分 19 秒）→ 没重新编译
```

**解决**

```bash
# 编译 + 运行串成一条命令：&& = 前面成功才执行后面
g++ -std=c++17 -Wall -Wextra p2.cpp -o p2 && ./p2

# 怀疑"改了没生效"时，第一件事比时间戳
ls -l --time-style=+%H:%M:%S p2.cpp p2
```

**教训**：专业项目用 `make` / `CMake` / `ninja`，它们干的核心事情之一就是**比较源码和产物的时间戳**，决定要不要重新编译。手动敲 `g++` 烦了的时候，就是该学 `Makefile` 的时候。

---

### C2 编辑了但没保存

**现象**：和 C1 很像，但这次是**源码文件本身没变**（`grep` 找不到你刚写的代码）。

**根因**：VSCode 里改了但没 `Ctrl+S`。文件名旁边有个**小圆点**就表示"未保存"。

**解决**

```bash
# 看源码修改时间有没有变
ls -l --time-style=+%H:%M:%S p2.cpp
```

改完立刻 `Ctrl+S`，再编译。

---

### C3 `xxx.cpp: 没有那个文件或目录`

**现象**

```
$ g++ -std=c++17 -Wall -Wextra cp1.cpp -o cp1
cc1plus: fatal error: cp1.cpp: 没有那个文件或目录
```

**根因**（两个都中了）

1. **文件名不对**：文件实际叫 `p2.cpp`，我编译的是 `cp1.cpp`
2. **工作目录不对**：终端在 `~`（家目录 `/home/aaa`），文件在 `~/桌面/server/`

`g++ p2.cpp` 是**在当前工作目录里找文件**，不是全盘搜索。

**解决：三下自检**

| 命令 | 回答什么 |
| --- | --- |
| `pwd` | 我在哪个目录？ |
| `ls` / `ls *.cpp` | 这个目录里有什么？ |
| `find ~ -name '*.cpp'` | 全盘找（最后手段） |

**最快的一招是 Tab 补全**：敲 `g++ p2` 后按 `Tab` —— 补不出东西就说明当前目录没有这个文件。

路径概念：

| 写法 | 含义 |
| --- | --- |
| `p2.cpp` | 相对路径：从当前工作目录找 |
| `./p2.cpp` | 同上，`.` 显式表示当前目录 |
| `../day2.cpp` | `..` = 上一级目录 |
| `~/桌面/server/p2.cpp` | `~` = 家目录，绝对路径，任何目录下都能用 |

---

### C4 `./p2 > log.txt` 之后日志是空的

**现象**：程序在跑，但日志文件里什么都没有；连 `kill` 之后看还是空的。

**根因**：**stdout 的缓冲策略随目标而变**

| stdout 指向 | 缓冲方式 | 表现 |
| --- | --- | --- |
| 终端 | **行缓冲** | 遇到换行就输出，立刻能看到 |
| 文件/管道 | **全缓冲** | 攒够几 KB 才写；进程被 `pause()` 卡住 → 永远不刷新 |

**解决（三选一）**

```cpp
std::cout << "..." << std::endl;   // ✅ endl = 换行 + flush
fflush(stdout);                    // 手动刷新（C 风格）
```
```cpp
fprintf(stderr, "...");            // stderr 本身无缓冲
```
```bash
stdbuf -o0 ./p2 > log.txt          // 命令行强制不缓冲
```

**教训**：以后 day4/day5 把日志重定向到文件排查问题时，"日志是空的"会让你怀疑人生——先想想缓冲。

---

### C5 `using namespace std;` 和 `std::` 混用

**现象**：文件里 `cout`、`std::cout`、`std::string` 混着出现，风格不统一。

**更严重的问题**：命名冲突。实测：

```cpp
#include <algorithm>
using namespace std;

int count = 42;              // 我自己定义的变量

int main() {
    cout << count << endl;   // ← error: reference to 'count' is ambiguous
}
```

`using namespace std;` 会把 `std` 里**几千个名字**倒进全局作用域，而 `std::count`、`std::size`、`std::data`、`std::swap`、`std::distance` 这些都很容易和自己的代码撞车。

**解决**

| 写法 | 什么时候用 |
| --- | --- |
| **`std::` 全限定名**（推荐） | 任何情况。`std::` 是免费的可读性提示："这是标准库" |
| `using std::cout;` 等（using-declaration） | 只想少打字时，只引入用到的几个名字 |
| `using namespace std;` | 只在短小练习里；**头文件里绝对禁止**（会污染所有 include 它的文件） |

服务器项目建议用第一种：后面会定义 `Server`、`Connection`、`handleClient`，还要用 `std::thread`/`std::mutex`（day5 线程池）——冲突风险是真实的。

---

## D. 运行期与系统行为类（最值钱的一类）

### D1 `bind: Address already in use`（**两种成因**）

**成因一：端口处于 TIME_WAIT**

- 谁先 `close()` 谁进 TIME_WAIT，等 2MSL（Linux 约 **60 秒**），期间**占用本地端口**
- 你的服务器先 `close(conn_fd)`，所以 TIME_WAIT 攒在服务器侧
- 但注意：TIME_WAIT 在**连接套接字**上，不是监听套接字上

**成因二：有活着的监听进程**（比如上次的 `p2` 没退出）

**关键区别**：

| 成因 | `SO_REUSEADDR` 有用吗 | 解决 |
| --- | --- | --- |
| 端口 TIME_WAIT | ✅ 有用 | 加 `setsockopt(..., SO_REUSEADDR, ...)`（必须在 bind 前） |
| **有活着的监听进程** | ❌ **没用** | **找到 pid 并杀掉** |

依据 `man 7 socket`：

> *"a socket may bind, **except when there is an active listening socket bound to the address**"*

**解决：排查链条**

```bash
ss -tlnp | grep 8888        # 找到占用者：users:(("p2",pid=7128,fd=3))
kill 7128                   # 先礼（SIGTERM）
kill -9 7128                # 后兵（SIGKILL，程序没有清理机会）
pkill -x p2                 # 按程序名杀
ss -tlnp | grep 8888        # 确认端口干净（应该没有输出）
```

**怎么确认 8888 上那个就是自己的程序**：看 `Send-Q` 列 —— 你设的 backlog 是 3，其他服务一般是 4096/511/151。

---

### D2 退出后端口还被占着（`Ctrl+Z` 陷阱）

| 按键 | 效果 | 端口释放吗 |
| --- | --- | --- |
| `Ctrl+C` | 发 SIGINT，进程**真的结束** | ✅ 释放 |
| **`Ctrl+Z`** | **挂起**（暂停），进程**还活着** | ❌ **仍然占着**（状态 T） |

**解决**：`fg` 把它拉回前台再 `Ctrl+C`；或 `kill %1`。

**自检**：退出后 `pgrep -a p2` 应该没有输出。

---

### D3 `./p2` 跑起来"什么都没有"

**根因**：**这通常不是 bug**。服务器程序会**一直运行、一直占着终端**，所以光标停在那里不动正是它在工作。

| 你看到的 | 含义 |
| --- | --- |
| 光标停住、**没有**回到 `$` 提示符 | ✅ 程序在运行（卡在 `pause()` / `accept()`） |
| 立刻回到 `$` 提示符 | ❌ 程序退出了（那才有问题） |

**解决：用第二个终端验证**（服务器占着第一个终端，这是常态）

```bash
# 终端1
./p2

# 终端2（新开一个）
ss -tlnp | grep 8888        # 看监听状态
curl -v http://127.0.0.1:8888/
```

**判断进程状态的三条命令**

| 命令 | 回答 |
| --- | --- |
| `pgrep -a p2` | 还在跑吗 |
| `ps -o pid,stat,wchan:20,cmd -p $(pgrep p2)` | 卡在哪个系统调用（`WCHAN=do_sys_pause` = 卡在 pause） |
| `Ctrl+C` | 退出 |

---

### D4 半包：客户端分两段发，服务器只收到前半段

**现象**：客户端隔 0.5 秒发两段请求，服务器只打印了：

```
收到请求:
GET / HTTP/1.1
Host: 127.0.0.1            ← "Connection: close" 和那个空行不见了
```

**根因**：**TCP 是字节流，没有消息边界**。一次 `read` 只给你"此刻到了多少"，不保证是一个完整请求。

这不是"意外"，`man 2 read` 的 RETURN VALUE 明确写着：

> *"It is **not an error** if this number is smaller than the number of bytes requested"*

**验证脚本**（能稳定复现）

```python
import socket, time
s = socket.create_connection(('127.0.0.1', 8888))
s.sendall(b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n')
time.sleep(0.5)
s.sendall(b'Connection: close\r\n\r\n')
time.sleep(0.3)
print(s.recv(4096).split(b'\r\n')[0])
```

**解决（day3 的核心）**：按连接维护读缓冲区

1. 每次 `read` 到的都 **追加**到该连接的 `read_buffer`
2. `find("\r\n\r\n")` → 头部齐了吗？没齐 → 继续读
3. 齐了 → 解析头，看 `Content-Length`；body 收够了吗？没够 → 继续读
4. 够了 → 切出一个完整请求交给业务
5. ⚠️ 加**长度上限**（`MAX_HEADER_SIZE`），否则客户端一直不发 `\r\n\r\n` 就能把内存撑爆（DoS）

---

### D5 一个不发数据的客户端把整个服务器卡住

**现象**（实测）

```
客户端1 连上但不发任何数据
→ 服务器阻塞在 read(conn_fd) 上，动弹不得
→ 此时客户端2 用 curl 请求：1047ms 后超时（curl 退出码 28）
→ 直到客户端1 断开（read 返回 0），服务器才脱身
```

**根因**：**阻塞式 `read` + 单线程**。`accept` 取到一条连接后，必须把 `read`/`send`/`close` 全部处理完，才能回去取下一个。一个客户端不发数据，整个服务器就陪着等。

**这是 day4 引入 `epoll` 的直接动机。**

---

### D6 对端提前断开：`Connection reset by peer` / `Broken pipe`（SIGPIPE）

**现象**（实测日志）

```
收到请求:
GET / HTTP/1.1

send: Connection reset by peer      ← errno = ECONNRESET
```

**根因**：对端已经走了，你还在写。分两种情况：

| 对端怎么走的 | write/send 返回 | 会发 SIGPIPE 吗 |
| --- | --- | --- |
| 发 RST（强制断开、或已关闭的 socket 收到数据） | `-1` + `ECONNRESET` | 不会 |
| 正常关闭（FIN）之后你继续写 | `-1` + `EPIPE` | **会 → 默认杀死进程** |

**SIGPIPE 是什么**（详见附录二）：`man 7 signal` 的官方描述是

```
SIGPIPE   P1990   Term   Broken pipe: write to pipe with no readers
                   ↑
              默认处置 = 终止进程
```

**为什么你单次 send 一直没被咬到**：**第一次 send 通常成功**（数据进了内核发送缓冲区就返回，此时对端 RST 还没回来）。要**第二次**写才 EPIPE。实测：

```
demo(SIGPIPE=默认):
第一次 send: 6
   ← 到这里就没了！"第二次 send"那行根本没打印 —— 进程被杀
```

**解决**

```cpp
#include <csignal>              // 顶部

int main() {
    signal(SIGPIPE, SIG_IGN);   // main 第一行
    ...
}
```

或者只对这一次调用生效：

```cpp
send(conn_fd, buf, len, MSG_NOSIGNAL);
```

忽略之后，正常处理返回值即可：

```cpp
ssize_t sent = send(conn_fd, response.c_str(), response.size(), 0);
if (sent < 0) {
    if (errno == EPIPE) { /* 客户端早就走了，正常现象，记一行日志 */ }
    perror("send");
}
```

**额外发现**：**信号处置会被子进程继承**。我沙箱里父进程把 SIGPIPE 设成了 `SIG_IGN`，所以第一次做 demo 时进程**没死**；用 Python 把处置重置成 `SIG_DFL` 后再跑，进程立刻被 `-13` 杀掉。以后写守护进程、用 `nohup`/`systemd` 启动时都要注意这点。

---

### D7 每次连接的 fd 都是同一个数字

**现象**：连着测三次，日志里都是 `新连接 fd=4 来自 ...`。

**根因**：**fd 只是进程内的一个小整数（内核表的下标）**，`close(conn_fd)` 之后号码**立刻可以被复用**，下一次 `accept` 又拿到最小的可用号码。

顺带印证了 `man 2 socket` 那句原文：

> *"The file descriptor returned by a successful call will be the **lowest-numbered** file descriptor not currently open for the process."*

**为什么这个必须记住**：它是 **day5 那个 use-after-close 竞态的根源**——主线程把 fd 投给线程池后没从 epoll 摘除，主线程可能对同一个 fd 再次入队，而工作线程已经 `close(fd)`，此时 fd 号可能已经分配给**另一个客户端**了。

**记住**：`server_fd`（总机，生命周期 = 进程）vs `conn_fd`（分机，生命周期 = 一次会话）。**fd 号 ≠ 连接的所有权。**

---

## E. 逻辑与设计类

### E1 没收到请求，服务器还是"回"了响应

**现象**：`send` 写在 `if (n > 0)` **外面**，所以 `n == 0`（客户端连上就关）或 `n < 0`（读失败）时也会执行 send。

**危害**

| 情况 | 现实场景 | 后果 |
| --- | --- | --- |
| `n == 0` | 端口扫描器、负载均衡健康检查、浏览器预连接、curl 超时 | 对着已关闭的 socket 发数据 → 日志刷满 `ECONNRESET` 噪音 |
| `n < 0` | 读都失败了 | 还要写，纯属多余 |

**根因**：**响应和请求失去了对应关系**。HTTP 的语义是"一个请求 → 一个响应"。

**解决**：把响应整段收进 `if (n > 0)`（见 B3 的代码）。或用**卫语句**让主流程更平：

```cpp
if (n == 0) {
    std::cout << "客户端主动关闭了连接" << std::endl;
    close(conn_fd);
    continue;
}
if (n < 0) {
    perror("read");
    close(conn_fd);
    continue;
}
// 走到这里，n > 0 是确定的 —— 主流程零缩进
```

---

## 元教训（比单个坑更值钱）

1. **编译器/工具就是索引**
   `'AF_INET' was not declared` → 回 man 抄 include；`'response' was not declared` → 检查作用域。报错不是敌人，是导航。

2. **"改代码"不是一步，是一个循环**
   编辑 → **保存** → 编译 → 运行 → 观察。任何一环漏掉，现象都是"改了没生效"。

3. **"能跑" ≠ "正确"**
   半包、阻塞、SIGPIPE 都让程序**看起来在正常工作**，只是在等某个特定条件爆发。

4. **手册给零件，组装靠模型**
   `conn_fd` 还是 `server_fd`、`htons` 还是 `ntohs` —— 手册不会直接告诉你，靠"总机/分机""host↔network"这些心智模型。

5. **亲手撞一次的坑，胜过看十遍文档**
   TIME_WAIT 的两种成因、fd 复用、半包 —— 现在是真的懂了，不是背下来的。

### 两个轴：协议正确性 vs 资源配平

| | 测什么 | 脚本 |
| --- | --- | --- |
| **协议正确性** | 回答**对不对** | `tests/p2_test.py`（23 项） |
| **资源配平** | 东西有没有**还回去**（fd + 账本） | `tests/balance_test.py`（4 项） |

**两者正交 —— 一个全绿不代表另一个绿**（实测，造坏版本验的）：

- 摘掉 EOF 路径的 `close` → **23/23 HTTP 测试全过**，但 fd 一路泄漏
- 摘掉所有 `erase` → **23/23 全过、fd 还是平的**，只有**哨兵**会喊（232 次）

> 所以"测试全绿"这句话必须带上"**哪套测试**"。这也是为什么 CI 里 epoll 要跑两个脚本。

### 判据本身也会骗人：假绿 与 假红

| | 现象 | 例子 |
| --- | --- | --- |
| **假绿** | 判据对"要测的那件事"不敏感，坏实现照样过 | "3 个 nc 看到 fd 5/6/7" —— 泄漏版同样打印 5、6、7 |
| **假绿 · 进阶** | **判据量错了对象** | `balance_test` 对着残留的旧服务器跑，`--pid` 指向新进程却全绿 → 已加"pid 必须是端口监听者"校验 |
| **假红** | 判据读了一个**会变的量** | fd 基线在服务器还没回收完连接时拍了快照 → 报出"多了 -2 个"这种负数 → 改成"等稳定后再读" |
| **假绿 · 又一次** | 判据**分不清两个相同的输入** | CP6d-2 的 keep-alive 测试第一版：用两个**一模一样**的请求，于是"服务器把第一个回了两遍、压根没看第二个"也照样全绿 → 改成 `/first` + `/second` 两个不同路径，并**读服务器日志**确认两个都真被处理了 |

**结论：判据也要被验证 —— 要问"反例能不能通过"（抓假绿）、"好实现会不会被误杀"（抓假红）。**

---

## 附录一：常用命令小抄

```bash
# ---- 查手册 ----
man 2 socket                      # 系统调用
man 3 byteorder                   # C 库函数（htons/ntohs）
man 3type sockaddr_in             # 结构体
man 7 ip                          # 协议/常量
man -P less 2 socket              # 强制用 less 分页器（能搜索）
man -k '^bind$'                   # 精确匹配页名
man 2 socket | grep -n '^[A-Z][A-Z ]*$'               # 目录法：节名 + 行号
man 2 socket | sed -n '/^RETURN VALUE/,/^ERRORS/p'    # 抽取法：只要某一节
man 2 listen | grep -n -A5 'backlog argument'         # 抓关键段落

# ---- 编译（零错误零警告再继续）----
g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion p2.cpp -o p2 && ./p2

# ---- 运行期验证 ----
ss -tlnp | grep 8888              # 监听状态 / Recv-Q / Send-Q / fd
ss -tan  | grep 8888              # 所有连接状态
ps -o pid,stat,wchan:20,cmd -p $(pgrep p2)     # 卡在哪个系统调用
strace -e trace=socket,setsockopt,bind,listen,accept ./p2   # 系统调用序列
nc 127.0.0.1 8888                 # 手动连一下
curl -v http://127.0.0.1:8888/    # 看原始往返（含空行）
curl -sI --http1.1 https://example.com          # 看真实生产服务器的响应头

# ---- 排查端口占用 ----
ss -tlnp | grep 8888              # 找 pid
kill <pid>                        # 先礼（SIGTERM）
kill -9 <pid>                     # 后兵（SIGKILL）
pkill -x p2                       # 按程序名杀

# ---- 排查"改了没生效" ----
ls -l --time-style=+%H:%M:%S p2.cpp p2
```

## 附录二：SIGPIPE 专题

**一句话**：内核在"你往一个没人读的管道/连接写数据"时发的"闭嘴"信号，**默认处置是直接杀掉你的进程**。

**名字来源**：`PIPE` = Unix 的管道 `|`。经典场景 `yes | head -1`：

- `head` 读一行就退出 → 管道读端关闭
- `yes` 还在拼命写 → 内核发 SIGPIPE → `yes` 被终止
- **这正是我们想要的**，否则 `yes` 会永远跑下去

所以 SIGPIPE 的设计目的是"**没人在听你说话了，请你安静地停下来**"，它**假设"一对一"**。

**为什么牵连到 socket**：在内核眼里 socket 和 pipe 是同一种东西（可写的文件描述符）：

```
写 pipe   → 读端已关闭 → EPIPE → SIGPIPE
写 socket → 对端已关闭 → EPIPE → SIGPIPE     ← 同一套逻辑
```

**为什么对服务器是灾难**

| 程序类型 | "读者走了"意味着 | 默认处置合适吗 |
| --- | --- | --- |
| 命令行过滤器（`yes`） | 我该收工了 | ✅ 很合适 |
| **服务器** | 只是**一个**客户端走了，还有几千个 | ❌ **灾难**：一个用户刷新页面就可能弄死整个服务 |

**信号编号**（`/usr/include/.../bits/signum-generic.h`）

| 编号 | 名字 | 默认处置 | 触发场景 |
| --- | --- | --- | --- |
| 2 | `SIGINT` | 终止 | `Ctrl+C` |
| 9 | `SIGKILL` | 终止（**不可捕获、不可忽略**） | `kill -9` |
| 11 | `SIGSEGV` | 终止 + core | 段错误 |
| **13** | **`SIGPIPE`** | **终止** | 往没有读者的管道/连接写 |
| 15 | `SIGTERM` | 终止 | `kill`（默认信号） |
| 20 | `SIGTSTP` | 暂停 | `Ctrl+Z` |

**排查技巧**：进程"莫名其妙消失、日志什么都没写"时，第一个怀疑对象是**被信号杀了**——`echo $?` 得到 `128 + 信号号` 就能确认（如 141 = 128 + 13 = SIGPIPE）。

**三种处理方式**

| 方式 | 代码 | 范围 |
| --- | --- | --- |
| 全局忽略（服务器首选） | `signal(SIGPIPE, SIG_IGN);` | 全局 |
| 更规范 | `sigaction()` 配 `SIG_IGN` | 全局 |
| 单次禁用 | `send(..., MSG_NOSIGNAL)` | 只这一次调用（写库时用） |
