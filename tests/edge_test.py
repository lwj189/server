#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
边界行为验收：分片发送 / 半关闭 / 慢连接隔离 / 流水线部分写

用法：
    python3 tests/edge_test.py --bin build/epoll
    python3 tests/edge_test.py --bin build/thread --server-arg=--workers --server-arg=4

四个探针分别回答：
    [1] 请求分两次到达，服务器会不会把半包当完整请求？
    [2] 客户端发完请求就 shutdown(SHUT_WR)，服务器还能不能把响应发完再正常收尾？
    [3] 一条连接只发半个请求头挂着，另一条正常连接会不会被拖住？
    [4] 客户端灌大量流水线请求但不读，发送缓冲区塞满后，
        服务器是收掉这条连接，还是留下一条永远发不完的连接？

目标二进制：epoll / thread。
p2 是单线程阻塞基线，[3] 和 [4] 预期会失败（慢连接会把整个进程卡住），
那是对照结论，不是回归；不要拿 p2 跑这套测试进 CI。

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
TIMEOUT = 2.0


def request():
    return b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n'


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


def connect():
    return socket.create_connection((HOST, PORT), timeout=5)


def read_until_close(s, timeout=TIMEOUT):
    """读到 EOF / 超时 / 出错。返回 (data, closed, error)。"""
    data = b''
    deadline = time.time() + timeout
    while True:
        remain = deadline - time.time()
        if remain <= 0:
            return data, False, 'timeout'
        s.settimeout(remain)
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            return data, False, 'timeout'
        except ConnectionError as e:
            return data, True, type(e).__name__
        if not chunk:
            return data, True, 'eof'
        data += chunk


def t1_split_request(r):
    print('\n[1] 请求分两次发送（前半段 20 字节，等 0.2 秒再发后半段）')
    s = None
    try:
        s = connect()
        payload = request()
        s.sendall(payload[:20])
        time.sleep(0.2)
        s.sendall(payload[20:])
        s.settimeout(TIMEOUT)
        data = s.recv(4096)
        r.check('分片请求仍返回 200', data.startswith(b'HTTP/1.1 200'),
                '收到：%r' % data[:40])
    except OSError as e:
        r.check('分片请求仍返回 200', False, repr(e))
    finally:
        if s is not None:
            s.close()


def t2_half_close(r):
    print('\n[2] 发完请求后 shutdown(SHUT_WR)，再读响应')
    s = None
    try:
        s = connect()
        s.sendall(request())
        s.shutdown(socket.SHUT_WR)
        data, closed, how = read_until_close(s, TIMEOUT)
        r.check('关闭写端后仍收到 200', data.startswith(b'HTTP/1.1 200'),
                '收到：%r' % data[:40])
        r.check('响应后连接正常关闭', closed, '结束方式：%s' % how)
    except OSError as e:
        r.check('关闭写端后仍收到 200', False, repr(e))
        r.check('响应后连接正常关闭', False, repr(e))
    finally:
        if s is not None:
            s.close()


def t3_slowloris_isolation(r):
    print('\n[3] 一条连接只发半个请求头挂着，另一条连接请求 /')
    slow = None
    fast = None
    try:
        slow = connect()
        # 故意不补最后的空行：服务器必须停在 NeedMore，不能回响应
        slow.sendall(b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n')
        time.sleep(0.1)

        fast = connect()
        t0 = time.monotonic()
        fast.sendall(request())
        fast.settimeout(TIMEOUT)
        data = fast.recv(4096)
        dt = time.monotonic() - t0
        r.check('慢连接不阻塞另一条连接（首字节 < 1s）',
                data.startswith(b'HTTP/1.1 200') and dt < 1.0,
                '首字节 %.1f ms，收到 %r' % (dt * 1000, data[:40]))
    except OSError as e:
        r.check('慢连接不阻塞另一条连接（首字节 < 1s）', False, repr(e))
    finally:
        if fast is not None:
            fast.close()
        if slow is not None:
            slow.close()


def t4_pipelined_partial_write(r, n):
    print('\n[4] 灌 %d 个流水线请求、不读响应，塞满发送缓冲区' % n)
    s = None
    fast = None
    try:
        s = connect()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.settimeout(10)
        t0 = time.monotonic()
        s.sendall(request() * n)
        send_dt = time.monotonic() - t0
        time.sleep(1.0)

        # 旧连接还没处理完时，新连接必须仍然可用：证明不是整个服务器卡死
        fast = connect()
        fast.sendall(request())
        fast.settimeout(TIMEOUT)
        first = fast.recv(4096)
        new_ok = first.startswith(b'HTTP/1.1 200')

        # 旧连接应该被服务器收掉（EOF / RST），而不是留下半条响应一直挂着
        s.settimeout(3.0)
        got = 0
        state = 'open'
        try:
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    state = 'eof'
                    break
                got += len(chunk)
        except socket.timeout:
            state = 'timeout(open)'
        except ConnectionError:
            state = 'reset'

        old_ok = state in ('eof', 'reset')
        r.check('流水线灌入后新连接仍可用', new_ok,
                '首包 %r' % first[:40])
        r.check('旧连接被收掉，不留半条响应', old_ok,
                'send %.2fs，读到 %d 字节，状态 %s' % (send_dt, got, state))
    except OSError as e:
        r.check('流水线灌入后新连接仍可用', False, repr(e))
        r.check('旧连接被收掉，不留半条响应', False, repr(e))
    finally:
        if fast is not None:
            fast.close()
        if s is not None:
            s.close()


def main():
    ap = argparse.ArgumentParser(description='边界行为验收')
    ap.add_argument('--bin', default='build/epoll', help='服务器可执行文件')
    ap.add_argument('--log', default='/tmp/edge_test_server.log')
    ap.add_argument('--requests', type=int, default=30000,
                    help='[4] 流水线请求数（默认 30000）')
    ap.add_argument('--server-arg', action='append', default=[],
                    help='传给服务器的参数，可重复，如 --server-arg=--workers --server-arg=4')
    args = ap.parse_args()

    print('=' * 64)
    print(' 边界行为验收 -> %s:%d  (%s)' % (HOST, PORT, args.bin))
    print('=' * 64)

    if not os.path.exists(args.bin):
        print('\n[错误] 找不到 %s —— 先编译' % args.bin)
        return 2
    if port_busy():
        print('\n[错误] 端口 %d 已经被占了 —— 先停掉那个服务器' % PORT)
        print('       ss -tlnp | grep %d' % PORT)
        return 2

    logf = open(args.log, 'wb')
    proc = subprocess.Popen([args.bin] + args.server_arg,
                            stdout=logf, stderr=subprocess.STDOUT)
    try:
        if not wait_listen(proc):
            print('\n[错误] 服务器没起来')
            print(open(args.log, 'rb').read()[:500].decode('utf-8', 'replace'))
            return 2
        r = Runner()
        r.info('服务器', 'pid=%d，日志 %s' % (proc.pid, args.log))

        t1_split_request(r)
        t2_half_close(r)
        t3_slowloris_isolation(r)
        t4_pipelined_partial_write(r, args.requests)
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
