# 交接文档 —— 给"新对话"看的第一份文件

> 这份文件是给**另一个对话**（新的 AI 会话）读的，不是给未来的自己看的。
> 目的：让新对话快速恢复"教我的上下文"，包括**我的水平、我的偏好、项目现状、以及哪些坑已经踩过**。
>
> 用法：新对话开始时，第一句话就说 —— *"请先读 notes/HANDOFF.md，然后跑一遍 tests/p2_test.py 确认现状。我们继续做 CP6c。"*

---

## 0. 请先做这几件事（别急着回答我）

```bash
cd ~/桌面/server
cat notes/HANDOFF.md                      # ① 读这份文件
git log --oneline -5                      # ② 看时间线，确认我在哪一步

g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion p2.cpp -o p2   # ③ 编译 p2（阻塞版）
./p2 &                                    #    后台跑服务器
python3 tests/p2_test.py                  #    跑测试（应为 23/23 通过）
kill %1                                   #    收工

g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion epoll.cpp -o epoll   # ④ epoll 版也要过
./epoll &                                 #    ⚠ p2 和 epoll 都占 8888，不能同时开
python3 tests/cp6b_test.py                #    跑测试（应为 4/4 通过）
kill %1
# 如果测试不通过，说明代码状态和我说的不一致 —— 先问清楚，别急着往下教
```

第 ③ 步很重要：**测试是唯一不依赖自然语言的记忆**。跑通就知道"什么是对的"。

---

## 1. 我是谁

- **大二学生**，自学 Linux 服务器编程，从零手写（**不用任何框架**，只用系统调用 + C++ 标准库）
- 已经能自己做到：
  - 查 man 手册（知道 2/3/3type/7 分节，知道查 SYNOPSIS/RETURN VALUE/ERRORS）
  - 写测试、用 `strace` / `ss` / `ps` / `curl -v` 做运行期验证
  - 从"AI 给的代码"改到"自己手写并解释每一行"
- 还在学：**epoll 的事件驱动思维**（CP6a ✅ 事件循环 / CP6b ✅ 分机纳入 epoll，下一步 CP6c）
- 英文能读手册，但**长段落需要拆解**（帮我断句、翻译生词、给类比）

---

## 2. 我的工作方式（★ 最容易丢的一块，请照这个来）

| 要求 | 具体表现 |
| --- | --- |
| **不要直接给答案** | 告诉我"该看哪一页 man / 该想什么问题"。骨架可以给，关键的空让我自己填 |
| **用实测数据说话** | 结论要有 `strace` / `ss` / 测试输出 / 编译错误来支撑，别只是断言 |
| **TDD：先红后绿** | 先写一个会失败的测试，再改代码让它通过（我很吃这一套） |
| **一次只推进一小步** | 每步都有明确验收标准（一条命令 + 一个预期输出） |
| **中文回答**，代码注释也用中文 | 注释要写"为什么"，不只是"是什么" |
| **鼓励实测验证** | 让我自己跑命令看现象，而不是只读结论 |
| 发现我"猜"而不是"查"时 | 指出来（这是我一直想改的习惯） |

**反面例子**（别这样做）：直接甩一大段完整代码让我复制。

---

## 3. 项目地图

```
~/桌面/server/
├── p2.cpp              ← 我手写的服务器（阻塞版），CP1~CP5，23 项测试全过
├── epoll.cpp           ← day4 练习（CP6a ✅ 事件循环 / CP6b ✅ 分机纳入 epoll）
├── README.md
├── .clang-format       ← 代码风格（4 空格缩进 / Attach 括号 / 指针贴左 / 不折行）
├── .github/workflows/ci.yml ← CI：严格编译 p2+epoll，再【串行】跑两套测试（23 项 + 4 项）
├── notes/
│   ├── HANDOFF.md          ← 本文件（给新对话的第一份）
│   ├── learning_progress.md ← 主线进度：各 CP 小节 + 方法论 + 概念清单
│   ├── epoll_notes.md      ← epoll 专题：API + 概念 + 坑 + 速查表
│   ├── pitfalls.md         ← 更细的踩坑记录（CP1~CP4）
│   ├── BRANCH_HANDOFF.md   ← 某个【分支对话】的交接（CP6b/CP6c-1 那一段）
│   └── date.txt            ← 老存档：CP1 伪代码 + JMeter 压测原始数据
└── tests/
    ├── p2_test.py       ← 【正确性】p2 的集成测试 t1~t13（半包/并发/RST/超长头/body 分片…）
    ├── cp6b_test.py     ← 【正确性】epoll 的 CP6b 验收（echo/分批/并发不串台/fd 配平）
    ├── load_test.sh     ← 【压测】驱动：编译 → 起服务器 → 逐档 JMeter → 调 analyze.sh
    ├── http_load.jmx    ← 【压测】JMeter 参数模板（是配置文件，不是可执行脚本）
    ├── analyze.sh       ← 【压测】读 .jtl 出统计表（含失败率拐点对比）
    └── results/         ← 【压测】产物（.jtl / 服务器日志 / JMeter 日志）

~/桌面/serverai/         ← AI 生成的参考实现 day2~day6（只对照，不照抄）
```

⚠ **`p2` 和 `epoll` 都监听 8888**，同时只能开一个。两个测试脚本也都默认连 8888 ——
跑哪个测试取决于你起的是哪个服务器（起 `./epoll` 却跑 `p2_test.py` 会一片红）。

**git**：分支 `main` + `cp5-halfpacket`，已推到 origin。
**写这份时的最新提交**：CP6b（分机纳入 epoll + fd 配平）—— 哈希跑 `git log --oneline -1` 看。

⚠ **命令行推不了**：本机没有 git 凭证（无 `credential.helper`、无 `~/.git-credentials`、
无 token、`gh` 未登录），`git push` 会报
`could not read Username for 'https://github.com'`。
→ 用 **VS Code 的"同步更改"按钮**推（凭证存在 VS Code 的 GitHub 会话里）。
→ 验证：`git fetch origin && git log --oneline origin/main -1`

---

## 4. 已掌握的概念（不用再从头讲）

**socket 层**
- `socket / setsockopt / bind / listen / accept / read / send / close` 各自的生命周期和返回值语义
- 两个 fd 的角色：`server_fd`（总机）vs `conn_fd`（分机）
- `SO_REUSEADDR`（只允许覆盖 TIME_WAIT，不允许两个活着的监听进程）
- `listen` 的 backlog = accept 队列上限（`ss` 的 `Send-Q` 就是它）
- `accept` 的 `addrlen` 是**值-结果参数**（每轮都要重置）

**协议层**
- HTTP 消息分帧：头部用 `\r\n\r\n` 定界，body 用 `Content-Length` 定长（RFC 9112 §2.1 / §6.2 / §6.3）
- 头字段名**大小写不敏感**（RFC 9110 §5.1）；冒号后 OWS 可为 0 个（RFC 9112 §5）
- 状态码：200 / 400 / 413 / 431

**系统层**
- 字节序：`htons`（填进去）/ `ntohs`（读出来）
- `errno` 机制 + 错误三分类（可重试 `EINTR` / 可恢复 `EAGAIN`·`EMFILE` / 编程错误 `EINVAL`）
- TCP 是**字节流**：一次 `read` ≠ 一个请求（半包/粘包）
- TIME_WAIT、fd 会被复用、`SIGPIPE` 为什么必须忽略
- **epoll 三件套**（`epoll_create1` / `epoll_ctl` / `epoll_wait`）、非阻塞 `fcntl` 读-改-写、`EAGAIN` 四态、LT vs ET

（细节都在 `notes/epoll_notes.md` 和 `notes/learning_progress.md` 里）

---

## 5. 踩过的坑（别再让我踩一遍）

| 现象 | 原因 |
| --- | --- |
| `redefinition of 'struct sockaddr_in'` | 把 man 里的结构体定义当代码抄了（那是说明书） |
| `setsockopt` 返回 -1 / `EINVAL` | 用了 `bool` 而不是 `int`（内核要求 `optlen >= sizeof(int)`） |
| 改了源码行为没变 | **没重新编译**（现在用 `g++ ... && ./p2` 一条命令） |
| 编辑了但没生效 | **没按 Ctrl+S 保存** |
| `cp1.cpp: 没有那个文件或目录` | 工作目录 / 文件名不对（`pwd`、`ls`、Tab 补全自检） |
| `bind: Address already in use` | ① TIME_WAIT（加 SO_REUSEADDR）② **上一个进程还活着**（`ss -tlnp` 找 pid 再 kill） |
| Ctrl+Z 之后端口还占着 | Ctrl+Z 是**挂起**（进程还在）；Ctrl+C 才是退出 |
| `./p2 > log.txt` 后日志是空的 | stdout 重定向到文件是**全缓冲** → 用 `std::endl`（会 flush） |
| `pause("system")` 编译错误 | `pause(void)` 不收参数；记混的是 Windows 的 `system("pause")` |
| `int fcntl(...)` 报错 | 那是**声明变量**的语法，不是调用函数 |
| 少一个 `}` → 整段成了死代码 | C++ 里花括号位置决定控制流，写完要检查缩进层级 |
| `send` 报 `Connection reset by peer` | 对端已经走了；忽略 SIGPIPE 后返回 -1，正常处理即可 |
| 小写 `content-length` 没被识别 | 头字段名大小写不敏感（已修，回归用例 t11/t12） |
| `Content-Length: 10abc` 被当成 10 | `stoul` 遇非数字就停（已修，回归用例 t13） |
| **测试通过了，但功能其实是坏的** | **假绿**：判据对"要测的那件事"不敏感。例："3 个 nc 都不关、该打印 fd 5/6/7" 在 `close(conn_fd)` 被注释掉、fd 全泄漏时**同样成立**。→ 判据也要反过来问"**反例能不能通过**" |
| 明明 kill 了还报 `Address already in use` | `p2` 和 `epoll` **都监听 8888**，同时只能开一个（`ss -tlnp \| grep 8888` 找 pid） |
| 以为 `-Wconversion` 会抓符号转换 | **C++ 下不含**符号检查（C 下含）。实测同一段代码：`gcc -Wconversion` 报，`g++ -Wconversion` 不报 → 要显式加 `-Wsign-conversion` |
| `F_SETFL` 直接传 `O_NONBLOCK` | `F_SETFL` 是**整体替换**不是追加 → 会抹掉原有标志位（实测 `O_APPEND` 被抹掉）→ 必须 `F_GETFL` → `\|` → `F_SETFL` 三步读-改-写 |
| 以为 `conn_fd` 会自动继承 `server_fd` 的非阻塞 | `accept` 出来的新 socket **不继承** file status flags（`man 2 accept` 明说，且注明与 BSD 不同）→ 每条连接都得自己设一遍 |
| 撞 fd 上限后 CPU 打满、日志暴涨 | `accept` 返回 `EMFILE` 后 `break`，但 LT 模式下监听 fd 仍然"可读" → **忙等死循环**。实测把 ulimit 压到 64：59 条连接就撞墙，几秒钟写了 **21797 行** `accept: Too many open files`。（CP6d 处理） |

---

## 6. 心智模型（用同一套语言跟我交流）

- **总机 / 分机**：`server_fd` 是公司总机（固定号码，永久存在），`conn_fd` 是转接出去的分机（一通电话一个，挂断后号码会被复用）
- **餐厅等位区**：accept 队列；backlog 是等位区容量；队列满了不报错，只会"变慢"
- **水管 / 快递**：TCP 是**水管里的水**（字节流，没有边界），不是一箱一箱的快递
- **状态机切片**：epoll 改造的本质 = 把"一个连续执行的函数"改造成"每次事件恢复一点进度"，所以状态必须从栈上搬进 `ClientContext`
- **fd 是会被复用的号码**：`close(5)` 之后下一个连接很可能又拿到 5（实测三次连接全是 fd=5）
- **分帧（framing）**：所有基于流的协议都要自己定消息边界，这是 HTTP/1.1 用 `\r\n\r\n` + `Content-Length` 的原因

---

## 7. 当前进度 + 下一步

```
✅ CP1  socket + setsockopt + bind + listen
✅ CP2  accept（addrlen 值-结果 / 总机分机）
✅ CP3  read（三态）
✅ CP4  send（RFC 9112 消息结构 / Content-Length 是字节数）
✅ CP5  半包与粘包（按连接读缓冲区 + Content-Length 收 body + DoS 上限）
✅ 修复 头字段名大小写 / 冒号后 OWS / stoul 非法值（t11~t13 回归用例）
✅ CP6a epoll 事件循环 + 非阻塞 server_fd + accept 循环到 EAGAIN
✅ CP6b 分机纳入 epoll（conn_fd 非阻塞 + EPOLL_CTL_ADD + 分流 + read 到 EAGAIN + close 配平）
▶  下一步 CP6c：引入 ClientContext，把 CP5 的状态机搬进事件循环
```

**CP6b 做了什么**：accept 之后不再立刻 close，而是

1. `conn_fd` 设非阻塞（`F_GETFL` → `| O_NONBLOCK` → `F_SETFL` 三步读-改-写）
2. `epoll_ctl(EPOLL_CTL_ADD)` 登记，`data.fd = conn_fd`
3. 事件循环里用 `events[i].data.fd` **分流**：总机 → `accept` / 分机 → `read`；
   分机循环 `read` 到 `EAGAIN`，`read()==0`（对端正常关闭）或出错时 `close` 配平

**CP6b 的验收**：`python3 tests/cp6b_test.py` → **4/4 全绿**

```
[1] echo        分机收到的话原样送回
[2] 分多次到达   分 3 批发送都要收到（read 循环到 EAGAIN 的证据）
[3] 并发不串台   3 条连接同时保持，各收各的（= "fd 5/6/7" 的真版本）
[4] fd 配平      开 20 条连接全关掉，fd 数回落到基线
```

实测：连跑 3 次，基线**始终是 5**；累计 **78 条连接**进出，fd 数从头到尾没动过。

> ⚠ **这条判据是从一个"假绿"改过来的，别改回去。**
>
> 原来的写法是：*"同时开 3 个 nc 都不关，服务器打印的 fd 应该是 5、6、7"*。
> 但它对"分机到底进没进 epoll"**完全不敏感** —— CP6a 那份代码里 `close(conn_fd)`
> 是注释掉的（fd 全泄漏），跑同样的场景**照样打印 5、6、7**。
>
> | | 泄漏版（CP6a） | 配平版（CP6b） |
> | --- | --- | --- |
> | 3 个 nc 同时开 | 打印 5、6、7 ✅ | 打印 5、6、7 ✅ |
> | 跑 20 条连接后 | fd 11 → 31 | fd 5 → 5 |
> | 跑 78 条连接后 | fd 只涨不落 | fd **还是 5** |
>
> **教训：验收标准本身也要被验证 —— 要问"反例能不能通过"。**

**之后的路线**：CP6c 引入 `ClientContext` 把 CP5 的状态机搬进来
→ CP6d 加固（`EMFILE` 忙等 / 部分写）→ 然后 day5 线程池

> 📌 CP6c **不换 `data.ptr`**（原先这么写过，重新评估后否掉了）：
> `data.fd` 保持不变，另加一个 `std::unordered_map<int, ClientContext>` 账本。
> 理由：分流逻辑一行都不用改；`data.ptr` 还得引入"哨兵"来区分总机（指针没法跟
> `server_fd` 这个 int 比），而且查不到时 map 能 `continue` 兜住、悬垂指针直接炸。
> 账本无论如何都要有（所有权 + 谁还活着），那 `ptr` 就只剩"省一次哈希查找"这点收益。
> 详见 `learning_progress.md` 的 CP6c 小节。

> 📌 曾把"同批陈旧事件"列为 CP6d 的一项，**实测证明在本设计里不存在**
> （单线程 LT + 不 `dup()`：30000 条连接 / 65240 个事件，陈旧 0 个）。已划掉。

---

## 8. 待确认 / 未闭环的三件事

1. **`socklen_t` 是 typedef，不是结构体**（`typedef __socklen_t socklen_t;`，手册在 `man 3type sockaddr`）——我以前叫错了，注意别再顺着我错。
2. **"总机 / 分机"这一轮专门讲透了**（`ss` + `/proc/<pid>/fd` 实测 + "门铃 / 电话线"的比喻），CP6c 还要接着用。
   ⚠ **真正的卡点在这**：同一个 `EPOLLIN`，在监听 socket 上意思是"有新连接"，在连接 socket 上是"有数据（或对端关了）"——**位是同一个位，含义由 socket 种类决定**。
3. **`conn_fd` 泄漏是分阶段收敛的，第一阶段已闭环**：
   - ✅ **手工配平**（`epoll.cpp`）：每条失败路径都 `close`，实测 78 条连接 fd 数不动
   - ▶ 下一阶段：引入 `ClientContext` 后变成 **`close` / `erase` 配对**（map 里也得摘干净）
   - ▶ 最终阶段：RAII
   - 检测手段已从"手工数 `/proc/<pid>/fd | wc -l`"升级成 **`tests/cp6b_test.py` 的 `[4]`**（自动比对基线，泄漏的实现过不了）

---

## 9. 教学偏好：我吃哪一套

- ✅ **先给"为什么"，再给"怎么做"**（比如先讲"阻塞的代价"，再讲 epoll）
- ✅ **对比表格**（旧写法 vs 新写法、有两列差异一目了然）
- ✅ **真实命令输出**（不是"应该会打印 xxx"，而是把实测结果贴出来）
- ✅ **类比**（总机/分机、等位区、水管）
- ✅ **指出我哪里在"猜"**
- ❌ 大段完整代码直接给
- ❌ 一次给三个新概念（我消化不了）
