#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
p2 服务器的冒烟测试 + 健壮性测试

用法（先在另一个终端启动服务器）：
    终端1: g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion p2.cpp -o p2 && ./p2
    终端2: python3 tests/p2_test.py                  # 默认连 127.0.0.1:8888
           python3 tests/p2_test.py --port 9999      # 换端口（要和 p2.cpp 里的 PORT 一致）
           PORT=9999 python3 tests/p2_test.py

测什么：
    [1] 正常请求   完整 GET → 200，响应体和 Content-Length 都对
    [2] 连上就关   客户端一个字节都不发就关闭 → 服务器必须活下来
    [3] RST 强断   发一半请求后用 SO_LINGER=0 强制发 RST → 服务器必须活下来
    [4] 并发 20    20 个请求全部返回 200
    [5] 半包观察   分两段发送，只打印现象，不计入失败（等 CP5 修）

    [2][3] 是"单个坏客户端不该杀死服务器"的回归测试
    （对应：响应只在 n > 0 时发送 + 忽略 SIGPIPE）

退出码：0 = 全部通过，1 = 有失败，2 = 连不上服务器
"""

import argparse
import os
import re
import socket
import struct
import sys
import threading
import time

HOST = os.environ.get('HOST', '127.0.0.1')
PORT = int(os.environ.get('PORT', '8888'))

READ_TIMEOUT = 3.0                                          # 单次等待服务器响应的上限
REQUEST = b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n'
EXPECT_BODY = b'<h1>Hello from my own server!</h1>'


# ----------------------------------------------------------------------
# 工具函数
# ----------------------------------------------------------------------
def talk(pieces=None, payload=REQUEST, gap=0.0, timeout=READ_TIMEOUT):
    """发一次请求并读回全部响应。

    pieces 非空时按分片依次发送（中间 sleep gap 秒）—— 用来制造半包。
    对端提前关闭时不抛异常，返回已经收到的字节。
    """
    s = socket.create_connection((HOST, PORT), timeout=5)
    data = b''
    try:
        if pieces is not None:
            for i, piece in enumerate(pieces):
                s.sendall(piece)
                if gap and i != len(pieces) - 1:
                    time.sleep(gap)
        else:
            s.sendall(payload)

        s.settimeout(timeout)
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
    finally:
        s.close()
    return data


def abort_after_partial():
    """发一半请求，然后用 SO_LINGER=0 强发 RST 断开（最恶劣的客户端）"""
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.sendall(b'GET / HTTP/1.1\r\n')                        # 只发一半
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                 struct.pack('ii', 1, 0))                   # 1=开启, 0=立即丢弃缓冲区
    s.close()                                               # → 内核直接发 RST


def parse(resp):
    """把响应拆成 (状态码, headers, body)"""
    head, _, body = resp.partition(b'\r\n\r\n')
    lines = head.split(b'\r\n')
    status = None
    if lines:
        m = re.match(rb'HTTP/1\.1\s+(\d{3})', lines[0])
        if m:
            status = int(m.group(1))
    headers = {}
    for line in lines[1:]:
        key, _, val = line.partition(b':')
        headers[key.strip().lower().decode(errors='replace')] = val.strip().decode(errors='replace')
    return status, headers, body


def normal_get_ok():
    """服务器还能正常服务吗？（返回 (ok, 描述)）"""
    try:
        resp = talk()
    except OSError as e:
        return False, '连不上服务器：%s' % e
    status, headers, body = parse(resp)
    if status != 200:
        return False, '状态码 = %s' % status
    return True, '200 OK'


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
def t1_normal_get(r):
    """基线：完整请求一次发出，应当返回 200 且响应体正确"""
    print('\n[1] 正常请求：完整 GET 一次发出')
    resp = talk()
    status, headers, body = parse(resp)
    r.check('状态行是 200', status == 200, '实际=%s' % status)
    r.check('响应体正确', body == EXPECT_BODY, '实际=%r' % body[:60])
    cl = headers.get('content-length')
    r.check('Content-Length 等于响应体字节数',
            cl is not None and cl.isdigit() and int(cl) == len(body),
            'Content-Length=%s 实际 body=%d 字节' % (cl, len(body)))
    r.check('Content-Type 是 text/html',
            headers.get('content-type') == 'text/html',
            '实际=%s' % headers.get('content-type'))


def t2_client_closes_immediately(r):
    """连上就关，不发任何数据（端口扫描 / 健康检查 / 浏览器预连接）"""
    print('\n[2] 连上就关：客户端不发任何数据直接关闭')
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.close()
    time.sleep(0.3)
    ok, detail = normal_get_ok()
    r.check('服务器活下来且仍能正常服务', ok, detail)


def t3_abortive_close(r):
    """发一半请求后用 RST 强断"""
    print('\n[3] RST 强断：发一半请求后 SO_LINGER=0 断开')
    abort_after_partial()
    time.sleep(0.3)
    ok, detail = normal_get_ok()
    r.check('服务器活下来且仍能正常服务', ok, detail)


def t4_concurrent(r, n=20):
    """并发 n 个请求，全部应返回 200"""
    print('\n[4] 并发测试：%d 个请求同时发送' % n)
    results = [None] * n

    def one(i):
        try:
            status, _, _ = parse(talk(timeout=5.0))
            results[i] = status
        except OSError as e:
            results[i] = 'err:%s' % e

    threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    ok = sum(1 for x in results if x == 200)
    r.check('%d 个并发请求全部返回 200' % n, ok == n,
            '成功 %d/%d，其它=%s' % (ok, n, [x for x in results if x != 200][:3]))


def t5_partial_request(r):
    """半包观察：分两段发送。

    注意：p2 目前只 read 一次，不等一个完整请求。所以这里只打印现象，
    不作为失败 —— 等 CP5 加上"按连接缓冲区"之后，才应该断言必须收全。
    """
    print('\n[5] 半包观察：请求分两段发送（间隔 0.5s）—— 不计入失败')
    resp = talk(pieces=[
        b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n',
        b'Connection: close\r\n\r\n',
    ], gap=0.5)
    status, _, _ = parse(resp)
    r.info('服务器对半包请求的响应', '状态码=%s（当前：能回，但是基于不完整的请求）' % status)
    r.info('待办', 'CP5 要实现"读缓冲区攒够完整请求再处理"')


# ----------------------------------------------------------------------
def main():
    global HOST, PORT

    ap = argparse.ArgumentParser(description='p2 服务器的冒烟 + 健壮性测试')
    ap.add_argument('--host', default=HOST)
    ap.add_argument('--port', type=int, default=PORT)
    args = ap.parse_args()
    HOST, PORT = args.host, args.port

    print('=' * 64)
    print(' p2 服务器测试  ->  %s:%d' % (HOST, PORT))
    print('=' * 64)

    # 先探测服务器是否在跑
    try:
        socket.create_connection((HOST, PORT), timeout=3).close()
    except OSError as e:
        print('\n[错误] 连不上 %s:%d —— 请先在另一个终端启动服务器：' % (HOST, PORT))
        print('       g++ -std=c++17 -Wall -Wextra p2.cpp -o p2 && ./p2')
        print('       详细原因: %s' % e)
        return 2

    r = Runner()
    t1_normal_get(r)
    t2_client_closes_immediately(r)
    t3_abortive_close(r)
    t4_concurrent(r)
    t5_partial_request(r)

    print('\n' + '=' * 64)
    print(' 结果：通过 %d 项，失败 %d 项' % (r.passed, r.failed))
    print('=' * 64)
    return 0 if r.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
