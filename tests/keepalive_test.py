#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CP6d 验收 [2]：keep-alive —— 一条连接服务多个请求

用法：
    python3 tests/keepalive_test.py                  # 自己起 ./epoll
    python3 tests/keepalive_test.py --bin ./epoll

契约：
    [1] 分两次发两个请求        → 收到两个响应，服务器不关连接
    [2] 一次 send 发两个请求（粘包）→ 收到两个响应
    [3] 请求里写了 Connection: close → 回完就关（回归：p2_test.py 靠这条活着）

⚠ 本测试为什么要【自己起服务器】：
    因为要读服务器的日志，才能证明"它到底处理了哪个请求"。

⚠ 为什么两个请求要用 /first 和 /second 两个【不同】的路径：
    这个测试的第一版用的是两个一模一样的请求 —— 那有个洞：
    服务器就算把【第一个请求回了两遍】、压根没看第二个，测试也照样全绿（假绿）。
    只有在日志里看到 /first 和 /second 【各出现一次】，才证明两个请求真的都被处理了。
    （教训：判据要能区分"做对了"和"碰巧看起来对了"。）

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

TIMEOUT = 2.0   # 等响应的上限


def req(path):
    """不带 Connection 头的请求 —— HTTP/1.1 默认就是复用连接"""
    return ('GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n' % path).encode()


CLOSE_REQ = b'GET /closeme HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n'


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
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            socket.create_connection((HOST, PORT), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def log_text(path):
    try:
        with open(path, 'r', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def connect():
    return socket.create_connection((HOST, PORT), timeout=5)


def recv_more(s, data, want_total, timeout=TIMEOUT):
    """在已有 data 的基础上继续收，直到看见 want_total 个状态行 / 超时 / 对端关闭。

    返回 (累计收到的字节, 是否看到对端关闭)。
    """
    closed = False
    deadline = time.time() + timeout
    while data.count(b'HTTP/1.1') < want_total:
        remain = deadline - time.time()
        if remain <= 0:
            break
        s.settimeout(remain)
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        except ConnectionError:
            closed = True
            break
        if not chunk:          # 对端关闭
            closed = True
            break
        data += chunk
    return data, closed


# ----------------------------------------------------------------------
# 测试用例
# ----------------------------------------------------------------------
def t1_two_requests_separate(r, logpath):
    """同一条连接：先发 /first，收到响应后再发 /second"""
    print('\n[1] 同一条连接，分两次发两个请求（/first 然后 /second）')
    s = connect()
    err = ''
    data, closed = b'', False
    try:
        s.sendall(req('/first'))
        data, closed = recv_more(s, data, 1)
        if closed:
            err = '服务器在第一个请求之后就关了连接（第二个请求发不出去了）'
        else:
            s.sendall(req('/second'))
            data, closed = recv_more(s, data, 2)
    except OSError as e:
        err = 'socket 异常：%s' % e
    finally:
        s.close()

    n = data.count(b'HTTP/1.1')
    r.check('两个请求都收到了响应', n == 2, err or '只收到 %d 个响应（期望 2）' % n)

    log = log_text(logpath)
    r.check('服务器真的处理了 /first', 'GET /first' in log, '日志里没有 /first')
    r.check('服务器真的处理了 /second', 'GET /second' in log,
            '日志里没有 /second —— 它可能把第一个请求又回了一遍')


def t2_pipelined_one_send(r, logpath):
    """两个请求粘在一起，一次 send 发过去（同一个 TCP 段）"""
    print('\n[2] 一次 send 两个请求（粘包：inbuf 里同时躺着两个）')
    s = connect()
    err = ''
    data, closed = b'', False
    try:
        s.sendall(req('/first') + req('/second'))
        data, closed = recv_more(s, data, 2)
    except OSError as e:
        err = 'socket 异常：%s' % e
    finally:
        s.close()

    n = data.count(b'HTTP/1.1')
    r.check('粘在一起的两个请求都被处理了', n == 2,
            err or '只收到 %d 个响应（期望 2）—— 处理过的字节没从 inbuf 里删掉，'
                   '第二个请求永远轮不到' % n)

    log = log_text(logpath)
    r.check('日志里两个请求都出现过', 'GET /first' in log and 'GET /second' in log,
            '日志里只有其中一个 —— 说明 inbuf 里的第二个请求没被处理')


def t3_connection_close_still_closes(r, logpath):
    """回归：客户端明说 Connection: close，服务器就得关"""
    print('\n[3] 请求里写了 Connection: close → 回完就关（回归）')
    s = connect()
    err = ''
    data, closed = b'', False
    try:
        s.sendall(CLOSE_REQ)
        data, closed = recv_more(s, data, 1)
        if not closed:
            s.settimeout(1.0)
            try:
                closed = (s.recv(4096) == b'')
            except socket.timeout:
                closed = False
            except ConnectionError:
                closed = True
    except OSError as e:
        err = 'socket 异常：%s' % e
    finally:
        s.close()

    n = data.count(b'HTTP/1.1')
    r.check('收到了 1 个响应', n == 1, err or '收到 %d 个响应' % n)
    r.check('服务器关闭了连接', closed,
            '服务器没关 —— 但客户端明确要求了 Connection: close')


# ----------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description='CP6d 验收：keep-alive')
    ap.add_argument('--bin', default='./epoll', help='服务器可执行文件')
    ap.add_argument('--log', default='/tmp/keepalive_test_server.log')
    args = ap.parse_args()

    print('=' * 64)
    print(' CP6d 验收 [2]：keep-alive  ->  %s:%d' % (HOST, PORT))
    print('=' * 64)

    if not os.path.exists(args.bin):
        print('\n[错误] 找不到 %s —— 先编译：' % args.bin)
        print('       g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion '
              'epoll.cpp -o epoll')
        return 2
    if port_busy():
        print('\n[错误] 端口 %d 已经被占了 —— 先停掉那个服务器' % PORT)
        print('       ss -tlnp | grep %d' % PORT)
        return 2

    logf = open(args.log, 'wb')
    proc = subprocess.Popen([args.bin], stdout=logf, stderr=subprocess.STDOUT)
    try:
        if not wait_listen(proc):
            print('\n[错误] 服务器没起来')
            print(log_text(args.log)[:500])
            return 2
        r = Runner()
        r.info('服务器', 'pid=%d，日志 %s' % (proc.pid, args.log))

        t1_two_requests_separate(r, args.log)
        t2_pipelined_one_send(r, args.log)
        t3_connection_close_still_closes(r, args.log)
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
