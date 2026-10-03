#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
配平测试 —— 句柄（fd）和账本（ClientContext）两本账

用法（先在另一个终端启动服务器）：
    终端1: g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion epoll.cpp -o epoll && ./epoll
    终端2: python3 tests/balance_test.py
           python3 tests/balance_test.py --pid 12345              # 显式指定服务器 pid
           python3 tests/balance_test.py --log /tmp/server.log    # 用例 [4] 要读服务器日志

测什么：
    [1] fd 配平      一批 HTTP 连接开完关完 → 服务器 fd 数回落到基线
    [2] 恶劣客户端    连上就关 / 半包后断开 / RST 强断 → fd 数照样回落
    [3] 大批量        200 条连接进出 → fd 数照样回落
    [4] 哨兵没响      全程服务器日志里没有"账本里……"的报警 → 账本也配平

为什么单独一个文件、不并进 p2_test.py：
    p2_test.py 测的是【协议正确性】—— 回答对不对；
    本文件测的是【资源配平】—— 东西有没有还回去。
    两者【正交】，一个全绿不代表另一个绿：
      · 漏了 close/erase 的服务器，23 项 HTTP 测试可以全过（实测过）
      · 一个满屏 400 的服务器，fd 也可能是平的

为什么还要专门测 [4]：
    账本条目是【第二本账】，fd 测试看不见它。漏 erase 时编译零警告、
    HTTP 测试全绿 —— 唯一会喊的就是 epoll.cpp 里那两个哨兵。
    本用例把"哨兵有没有响"变成自动判据。

历史：本文件的前身是 cp6b_test.py（CP6b 的 echo 验收：echo / 分多次到达 /
      并发不串台 / fd 配平）。CP6c-2 把 echo 契约换成了真正的 HTTP，
      前三个用例随之退役 —— 但"fd 配平"与回显还是回 HTTP 无关，
      它跨了两次重构依然成立，所以留下来并扩成了本文件。

退出码：0 = 全部通过，1 = 有失败，2 = 连不上服务器
"""

import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time

HOST = os.environ.get('HOST', '127.0.0.1')
PORT = int(os.environ.get('PORT', '8888'))

REQUEST = b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n'
READ_TIMEOUT = 3.0


# ----------------------------------------------------------------------
# 工具函数
# ----------------------------------------------------------------------
def connect():
    return socket.create_connection((HOST, PORT), timeout=5)


def http_round_trip(timeout=READ_TIMEOUT):
    """发一个完整请求 → 读回响应 → 关闭。返回收到的字节（判对错是 p2_test 的事）。

    两个超时是不一样的：【等第一个分片】用 timeout（服务器可能在忙），
    【收尾】只用 0.5 秒 —— 服务器正常时响应发完就 close，客户端立刻读到 EOF；
    万一它不 close（比如被改坏了），也不能在这里干等 timeout × 几百条连接。
    （实测踩过：一个不 close 的坏版本让 [1]+[3] 的 220 条连接 × 3 秒 = 11 分钟。）
    """
    s = connect()
    data = b''
    try:
        s.sendall(REQUEST)

        s.settimeout(timeout)
        try:
            first = s.recv(4096)
        except (socket.timeout, ConnectionError):
            return b''
        if not first:
            return b''
        data = first

        s.settimeout(0.5)
        while True:
            try:
                chunk = s.recv(4096)
            except (socket.timeout, ConnectionError):
                break
            if not chunk:       # EOF：服务器按 Connection: close 关掉了
                break
            data += chunk
    finally:
        s.close()
    return data


def client_closes_immediately():
    """连上不发任何数据就关（端口扫描 / 健康检查 / 浏览器预连接）"""
    connect().close()


def client_half_request():
    """发半个请求就正常关闭 —— 服务器应当读到 EOF（read 返回 0）"""
    s = connect()
    s.sendall(b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n')
    s.close()


def client_rst():
    """发一半就用 SO_LINGER=0 强发 RST —— 服务器应当读到 ECONNRESET"""
    s = connect()
    s.sendall(b'GET / HTTP/1.1\r\n')
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    s.close()   # 内核直接发 RST，不走四次挥手


def fd_count(pid):
    """服务器进程当前持有的 fd 数量（fd 是会被复用的号码，所以只看总数）"""
    try:
        return len(os.listdir('/proc/%d/fd' % pid))
    except OSError:
        return None


def settled_fd_count(pid, timeout=6.0, quiet=0.25):
    """等 fd 数【稳定下来】再读，而不是读一瞬间的快照。

    为什么不能直接读：客户端 close() 之后，服务器要等 epoll 报事件、read 返回 0
    才真正 close(fd) —— 这是异步的，中间有个"已经关了但还没回收"的窗口。
    实测（用未加本函数的版本连跑 30 次）：基线偶尔读到 6/7/8（把上一批还没回收的
    连接算了进去），而跑完 20 条连接后稳定落在 5，于是报出"多了 -2 个"这种【负数】——
    服务器是对的，错的是判据在读一个会变的量。
    这是"假红"，正好是 CP6b 那个"假绿"的镜像：判据本身也要被验证。
    """
    last = fd_count(pid)
    if last is None:
        return None
    stable_since = time.time()
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(quiet)
        now = fd_count(pid)
        if now is None:
            return None
        if now != last:
            last = now                  # 还在变（还有连接没回收完）→ 重新计时
            stable_since = time.time()
        elif time.time() - stable_since >= quiet:
            return now                  # 连续 quiet 秒没变 → 认定稳定
    return last


def find_server_pid(port):
    """用 ss 反查监听该端口的进程 pid（查不到就跳过 fd 用例）"""
    try:
        out = subprocess.run(['ss', '-tlnpH', 'sport = :%d' % port],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r'pid=(\d+)', out)
    return int(m.group(1)) if m else None


def verify_pid_owns_port(pid, port):
    """确认 `--pid` 给的那个进程，就是监听该端口的那个。

    ⚠ 为什么非查不可（实测踩过）：8888 上残留着一个旧服务器时，测试照样能连上、
    照样全绿 —— 但它量的是【旧进程】的 fd，新起的那个可能根本没绑上端口。
    一次"4/4 通过"的假绿就是这么来的。判据必须确认自己在量谁。
    """
    listener = find_server_pid(port)
    if listener is None:
        return False, 'ss 查不到监听 %d 的进程' % port
    if listener != pid:
        return False, ('监听 %d 的是 pid=%d，不是 --pid 给的 pid=%d —— '
                       '端口上残留着别的服务器' % (port, listener, pid))
    return True, ''


def server_alive():
    """服务器还活着吗？（TCP 层面能连上就算活着）"""
    try:
        connect().close()
        return True, ''
    except OSError as e:
        return False, '连不上服务器：%s' % e


class Runner:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, name, ok, detail=''):
        if ok:
            self.passed += 1
            print('  [PASS] %s' % name)
        else:
            self.failed += 1
            print('  [FAIL] %s   %s' % (name, detail))

    def info(self, name, detail):
        print('  [观察] %s   %s' % (name, detail))


# ----------------------------------------------------------------------
# 测试用例
# ----------------------------------------------------------------------
def balance_case(r, pid, title, traffic, rounds_desc=''):
    """公共骨架：量基线 → 跑一段流量 → 再等稳定 → 比对。"""
    print('\n%s' % title)
    if pid is None:
        r.info('跳过', '没拿到服务器 pid（ss 查不到）—— 可用 --pid 手动指定')
        return
    base = settled_fd_count(pid)
    if base is None:
        r.info('跳过', '读不到 /proc/%d/fd（权限不足或进程已退出）' % pid)
        return

    traffic()

    now = settled_fd_count(pid)
    if now is None:
        r.info('跳过', '进程已退出，读不到 fd')
        return

    r.check('fd 数回落到基线%s' % rounds_desc, now == base,
            '基线 %d → 现在 %d（多了 %d 个 = 有连接没被 close）'
            % (base, now, now - base))


def t1_http_connections(r, pid, rounds=20):
    """【1】正常 HTTP 流量：开一批、关一批，fd 必须回落"""
    balance_case(r, pid,
                 '[1] fd 配平：%d 条 HTTP 连接开完关完' % rounds,
                 lambda: [http_round_trip() for _ in range(rounds)])


def t2_hostile_clients(r, pid, rounds=5):
    """【2】最恶劣的三类客户端：连上就关 / 半包后断开 / RST 强断"""
    def traffic():
        for _ in range(rounds):
            client_closes_immediately()
            client_half_request()
            client_rst()
    balance_case(r, pid,
                 '[2] 恶劣客户端配平：连上就关 + 半包断开 + RST 强断（各 %d 次）' % rounds,
                 traffic)


def t3_bulk(r, pid, rounds=200):
    """【3】大批量：压一压 fd 复用路径 —— 200 条连接进出，fd 数不许漂"""
    balance_case(r, pid,
                 '[3] 大批量配平：%d 条连接进出' % rounds,
                 lambda: [http_round_trip() for _ in range(rounds)])


def t4_sentinels(r, logpath):
    """【4】账本配平：全程服务器日志里不该出现哨兵报警"""
    print('\n[4] 哨兵没响：服务器日志里没有"账本里……"的报警（第二本账）')
    if not logpath:
        r.info('跳过', '没给服务器日志路径 —— 用 --log 指定（CI 里会传 server.log）')
        return
    try:
        with open(logpath, 'r', errors='replace') as f:
            text = f.read()
    except OSError as e:
        r.info('跳过', '读不到日志 %s：%s' % (logpath, e))
        return

    hits = [ln for ln in text.splitlines() if '账本里' in ln]
    r.check('账本配平（哨兵一次都没响）', not hits,
            '哨兵报了 %d 次，前两条：%s' % (len(hits), hits[:2]))


# ----------------------------------------------------------------------
def main():
    global HOST, PORT

    ap = argparse.ArgumentParser(description='epoll 服务器的配平测试（fd + 账本）')
    ap.add_argument('--host', default=HOST)
    ap.add_argument('--port', type=int, default=PORT)
    ap.add_argument('--pid', type=int, default=None, help='服务器 pid（不填就自动用 ss 查）')
    ap.add_argument('--log', default=None, help='服务器 stdout/stderr 落地的文件（用例 [4] 用）')
    args = ap.parse_args()
    HOST, PORT = args.host, args.port

    print('=' * 64)
    print(' 配平测试（fd + 账本）  ->  %s:%d' % (HOST, PORT))
    print('=' * 64)

    ok, detail = server_alive()
    if not ok:
        print('\n[错误] 连不上 %s:%d —— 请先在另一个终端启动服务器：' % (HOST, PORT))
        print('       g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion epoll.cpp -o epoll && ./epoll')
        print('       详细原因: %s' % detail)
        return 2

    pid = args.pid if args.pid is not None else find_server_pid(PORT)
    if pid is not None:
        print(' 服务器 pid = %d（fd 用例会读 /proc/%d/fd）' % (pid, pid))
    if args.log:
        print(' 服务器日志 = %s（用例 [4] 会检查里面有没有哨兵报警）' % args.log)

    # ⚠ 先确认"我量的就是它" —— 否则整份 fd 结论都是假的
    if args.pid is not None:
        ok, why = verify_pid_owns_port(args.pid, PORT)
        if not ok:
            print('\n[错误] --pid 指定的进程不是 %d 端口的监听者：%s' % (PORT, why))
            print('       先清掉残留：ss -tlnp | grep 8888   →   kill <pid>')
            print('       （不然 fd 用例量的是别人的 fd，结果没有意义）')
            return 2

    r = Runner()
    t1_http_connections(r, pid)
    t2_hostile_clients(r, pid)
    t3_bulk(r, pid)
    t4_sentinels(r, args.log)

    print('\n' + '=' * 64)
    print(' 结果：通过 %d 项，失败 %d 项' % (r.passed, r.failed))
    print('=' * 64)
    return 0 if r.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
