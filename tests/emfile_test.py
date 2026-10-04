#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CP6d 验收 [1]：EMFILE 忙等 —— 服务器撞到 fd 上限后，不许空转

用法：
    python3 tests/emfile_test.py                 # 默认用 ./epoll
    python3 tests/emfile_test.py --bin ./epoll --limit 64

为什么这个测试要【自己起服务器】（和其他测试不一样）：
    因为要改服务器的 fd 上限（ulimit -n），而那必须在启动前设定。
    p2_test.py / balance_test.py 都是连一个已经跑着的服务器，
    这里必须自己 fork 一个。

它测什么：
    [1] 撞到 fd 上限后，日志不再暴涨（忙等停了）
    [2] 连接释放之后，服务器能恢复接收新连接

为什么判据是"日志增长速度"：
    忙等的表现就是"疯狂刷同一行日志 + CPU 100%"。
    实测坏版本：3 秒 161 万行。修好后：几行。

退出码：0 = 全过，1 = 有失败，2 = 起不来
"""

import argparse
import os
import signal
import socket
import subprocess
import sys
import time

HOST = '127.0.0.1'
PORT = int(os.environ.get('PORT', '8888'))

FILL_TIMEOUT = 2.0      # 单条建连的超时
HOLD_SECONDS = 2.0      # fd 灌满后，观察多久
GROWTH_LIMIT = 200      # 这 2 秒里允许新增几行日志（坏版本是几十万行）


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


def port_busy():
    try:
        socket.create_connection((HOST, PORT), timeout=0.5).close()
        return True
    except OSError:
        return False


def wait_listen(proc, timeout=8.0):
    """等服务器真的进入 LISTEN（进程没死 + 端口能连）"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:      # 服务器自己挂了
            return False
        try:
            socket.create_connection((HOST, PORT), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def log_lines(path):
    try:
        with open(path, 'rb') as f:
            return f.read().count(b'\n')
    except OSError:
        return 0


def fill_fd_table(n):
    """连 n 条，全部挂着不发数据（每条占服务器一个 fd）。返回列表。"""
    conns = []
    for _ in range(n):
        try:
            s = socket.create_connection((HOST, PORT), timeout=FILL_TIMEOUT)
            conns.append(s)
            time.sleep(0.02)     # 慢一点，别把 accept 队列（backlog=3）挤爆
        except OSError:
            break
    return conns


def http_get_ok(timeout=3.0):
    """还能正常服务吗？（发一个完整 GET，看有没有 200）"""
    try:
        s = socket.create_connection((HOST, PORT), timeout=timeout)
    except OSError as e:
        return False, '连不上：%s' % e
    try:
        s.settimeout(timeout)
        s.sendall(b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n')
        data = b''
        while True:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            except ConnectionError:
                break
            if not chunk:
                break
            data += chunk
        if b'200' in data.split(b'\r\n')[0]:
            return True, ''
        return False, '响应不是 200：%r' % data[:60]
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description='CP6d 验收：EMFILE 忙等')
    ap.add_argument('--bin', default='./epoll', help='服务器可执行文件')
    ap.add_argument('--limit', type=int, default=64, help='服务器的 ulimit -n')
    ap.add_argument('--log', default='/tmp/emfile_test_server.log')
    args = ap.parse_args()

    print('=' * 64)
    print(' CP6d 验收 [1]：EMFILE 忙等  ->  %s:%d' % (HOST, PORT))
    print('=' * 64)

    if not os.path.exists(args.bin):
        print('\n[错误] 找不到 %s —— 先编译：' % args.bin)
        print('       g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion epoll.cpp -o epoll')
        return 2
    if port_busy():
        print('\n[错误] 端口 %d 已经被占了 —— 先停掉那个服务器' % PORT)
        print('       ss -tlnp | grep %d' % PORT)
        return 2

    r = Runner()

    # 起一个 fd 上限很低的服务器
    logf = open(args.log, 'wb')
    proc = subprocess.Popen(
        ['bash', '-c', 'ulimit -n %d; exec %s' % (args.limit, args.bin)],
        stdout=logf, stderr=subprocess.STDOUT)
    try:
        if not wait_listen(proc):
            print('\n[错误] 服务器没起来')
            print(open(args.log).read()[:500])
            return 2
        r.info('服务器', 'pid=%d，ulimit -n=%d' % (proc.pid, args.limit))

        # ---- 把 fd 表灌满 ----
        print('\n[1] 灌满 fd 表，然后看日志还涨不涨')
        conns = fill_fd_table(args.limit + 6)
        r.info('灌连接', '成功挂着 %d 条（服务器 fd 上限 %d）' % (len(conns), args.limit))

        time.sleep(0.5)
        n1 = log_lines(args.log)
        time.sleep(HOLD_SECONDS)
        n2 = log_lines(args.log)
        growth = n2 - n1
        r.info('静置 %.0f 秒' % HOLD_SECONDS,
               '日志从 %d 行 → %d 行，新增 %d 行' % (n1, n2, growth))
        r.check('撞到 fd 上限后不再空转（新增日志 < %d 行）' % GROWTH_LIMIT,
                growth < GROWTH_LIMIT,
                '新增了 %d 行 —— 还在忙等（坏版本是几十万行）' % growth)

        # ---- 释放连接，看能不能恢复 ----
        print('\n[2] 把所有连接关掉，看服务器能不能恢复接收')
        for s in conns:
            try:
                s.close()
            except OSError:
                pass
        time.sleep(1.0)
        ok, detail = http_get_ok()
        r.check('连接释放后服务器恢复服务', ok, detail)

    finally:
        try:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=3)
        except Exception:
            proc.kill()
        logf.close()

    print('\n' + '=' * 64)
    print(' 结果：通过 %d 项，失败 %d 项' % (r.passed, r.failed))
    print('=' * 64)
    return 0 if r.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
