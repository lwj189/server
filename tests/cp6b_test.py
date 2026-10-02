#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
epoll 服务器 CP6b 验收测试 —— "把分机（conn_fd）纳入 epoll"

用法（先在另一个终端启动服务器）：
    终端1: g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion epoll.cpp -o epoll && ./epoll
    终端2: python3 tests/cp6b_test.py
           python3 tests/cp6b_test.py --pid 12345   # 手动指定服务器 pid（用例 [4] 需要）
           python3 tests/cp6b_test.py --port 9999

CP6b 的契约（本文件 = 这份契约的机器可读版本）：
    [1] echo      分机上收到的字节，原样发回
    [2] 分多次到达 同一条连接分 3 批发送（有间隔），3 批都要收到  → read 循环到 EAGAIN
    [3] 并发不串台 3 条连接【同时保持】，各自的回显不能串到别人身上
    [4] fd 配平    开一批连接 → 全部关闭 → 服务器持有的 fd 数回落（不泄漏）

为什么要用例 [4]？
    HANDOFF 里写的验收标准是"3 个 nc 都不关，应该打印 fd=5、6、7"。
    但 CP6a 的代码里 close(conn_fd) 是注释掉的 —— fd 全泄漏的实现同样打印 5、6、7。
    也就是说那个判据对"分机到底进没进 epoll"完全不敏感，是【假绿】。
    用例 [1][2][3] 测"事件分流有没有生效"，用例 [4] 测"句柄有没有配平" ——
    两者都绿，才说明分机是真的被"接管"了，而不只是被"漏掉了"。

退出码：0 = 全部通过，1 = 有失败，2 = 连不上服务器
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import threading
import time

HOST = os.environ.get('HOST', '127.0.0.1')
PORT = int(os.environ.get('PORT', '8888'))

ECHO_TIMEOUT = 2.0      # 等服务器回显的上限


# ----------------------------------------------------------------------
# 工具函数
# ----------------------------------------------------------------------
def connect():
    return socket.create_connection((HOST, PORT), timeout=5)


def recv_until(s, n, timeout=ECHO_TIMEOUT):
    """尽力收满 n 字节；超时或对端关闭就返回已收到的部分（不抛异常）。"""
    data = b''
    deadline = time.time() + timeout
    while len(data) < n:
        remain = deadline - time.time()
        if remain <= 0:
            break
        s.settimeout(remain)
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        except ConnectionError:
            break
        if not chunk:          # 对端关闭
            break
        data += chunk
    return data


def echo_once(payload, timeout=ECHO_TIMEOUT):
    """连上 → 发 payload → 收同样多的字节 → 关闭。返回收到的字节。"""
    s = connect()
    try:
        s.sendall(payload)
        return recv_until(s, len(payload), timeout)
    finally:
        s.close()


def fd_count(pid):
    """服务器进程当前持有的 fd 数量（fd 是会复用的号码，所以只看总数）"""
    try:
        return len(os.listdir('/proc/%d/fd' % pid))
    except OSError:
        return None


def find_server_pid(port):
    """用 ss 反查监听该端口的进程 pid（用例 [4] 用；查不到就跳过）"""
    try:
        out = subprocess.run(['ss', '-tlnpH', 'sport = :%d' % port],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r'pid=(\d+)', out)
    return int(m.group(1)) if m else None


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
def t1_echo(r):
    """最小用例：分机上来的数据，服务器感知得到吗？"""
    print('\n[1] echo：一条连接发一句话，应当原样收到')
    try:
        got = echo_once(b'hello')
    except OSError as e:
        r.check('分机 echo 正常', False, 'socket 异常：%s' % e)
        return
    r.check('分机上发的话被原样送回', got == b'hello',
            '期望 b"hello"，实际收到 %r（收到 0 字节 = 分机根本没进 epoll）' % got[:60])


def t2_multi_chunks(r):
    """同一连接 3 批到达：验证"每次事件读一部分，攒起来不丢"（read 循环到 EAGAIN）"""
    print('\n[2] 分多次到达：同一连接分 3 批发送（间隔 0.3s）')
    pieces = [b'AAA', b'BBB', b'CCC']
    s = connect()
    try:
        for i, p in enumerate(pieces):
            s.sendall(p)
            if i != len(pieces) - 1:
                time.sleep(0.3)
        got = recv_until(s, sum(len(p) for p in pieces))
    except OSError as e:
        s.close()
        r.check('分 3 批发送全部被收到', False, 'socket 异常：%s' % e)
        return
    finally:
        s.close()
    r.check('分 3 批发送全部被收到', got == b'AAABBBCCC',
            '期望 b"AAABBBCCC"，实际 %r' % got[:60])


def t3_concurrent_not_crossed(r, n=3):
    """3 条连接同时保持（都不关），各自的回显不能串台 —— 这就是 fd 5/6/7 的"真版本" """
    print('\n[3] 并发不串台：%d 条连接同时保持，各说各话' % n)
    marks = [('conn-%d' % i).encode() for i in range(n)]
    results = [None] * n
    errors = [None] * n

    barrier = threading.Barrier(n, timeout=5)

    def one(i):
        try:
            s = connect()
            try:
                s.sendall(marks[i])
                barrier.wait()                      # 等所有连接都建立、都发完再收
                time.sleep(0.2)
                results[i] = recv_until(s, len(marks[i]))
            finally:
                s.close()
        except Exception as e:                      # noqa: BLE001 —— 测试里要把异常变成报告
            errors[i] = e

    threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    if any(errors):
        r.check('%d 条连接同时保持且各自收到自己的回显' % n, False,
                '线程异常：%s' % [e for e in errors if e][:2])
        return

    crossed = [(i, results[i]) for i in range(n) if results[i] != marks[i]]
    r.check('%d 条连接同时保持且各自收到自己的回显' % n, not crossed,
            '串台/丢包：%s（说明分机之间没有正确区分，或只处理了第一条）'
            % [(i, res[:30] if res else res) for i, res in crossed])


def t4_fd_balance(r, pid, rounds=20):
    """开一批连接 → 全部关掉 → 服务器 fd 数必须回落（close 配平，不泄漏）"""
    print('\n[4] fd 配平：开 %d 条连接并全部关闭，fd 数应回落到基线' % rounds)
    if pid is None:
        r.info('跳过', '没拿到服务器 pid（ss 查不到）—— 可用 --pid 手动指定')
        return

    base = fd_count(pid)
    if base is None:
        r.info('跳过', '读不到 /proc/%d/fd（权限不足或进程已退出）' % pid)
        return
    r.info('基线', '服务器当前持有 %d 个 fd' % base)

    for _ in range(rounds):
        try:
            echo_once(b'x')
        except OSError as e:
            r.check('fd 数回落到基线', False, '中途连不上：%s' % e)
            return

    time.sleep(0.5)                 # 给事件循环一点时间处理对端关闭
    now = fd_count(pid)
    if now is None:
        r.info('跳过', '进程已退出，读不到 fd')
        return
    r.check('fd 数回落到基线（没有连接被漏掉）', now == base,
            '基线 %d → 现在 %d（多了 %d 个 = 有 %d 条连接没被 close）'
            % (base, now, now - base, now - base))


# ----------------------------------------------------------------------
def main():
    global HOST, PORT

    ap = argparse.ArgumentParser(description='epoll 服务器 CP6b 验收测试')
    ap.add_argument('--host', default=HOST)
    ap.add_argument('--port', type=int, default=PORT)
    ap.add_argument('--pid', type=int, default=None, help='服务器 pid（不填就自动用 ss 查）')
    args = ap.parse_args()
    HOST, PORT = args.host, args.port

    print('=' * 64)
    print(' CP6b 验收测试  ->  %s:%d' % (HOST, PORT))
    print('=' * 64)

    ok, detail = server_alive()
    if not ok:
        print('\n[错误] 连不上 %s:%d —— 请先在另一个终端启动服务器：' % (HOST, PORT))
        print('       g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion epoll.cpp -o epoll && ./epoll')
        print('       详细原因: %s' % detail)
        return 2

    pid = args.pid if args.pid is not None else find_server_pid(PORT)
    if pid is not None:
        print(' 服务器 pid = %d（用例 [4] 会读 /proc/%d/fd）' % (pid, pid))

    r = Runner()
    t1_echo(r)
    t2_multi_chunks(r)
    t3_concurrent_not_crossed(r)
    t4_fd_balance(r, pid)

    print('\n' + '=' * 64)
    print(' 结果：通过 %d 项，失败 %d 项' % (r.passed, r.failed))
    print('=' * 64)
    return 0 if r.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
