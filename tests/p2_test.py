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
    [1] 正常请求     完整 GET → 200，响应体 / Content-Length / Content-Type 都对
    [2] 连上就关     客户端一个字节都不发就关闭 → 服务器必须活下来
    [3] RST 强断     发一半请求后用 SO_LINGER=0 强制发 RST → 服务器必须活下来
    [4] 并发 20      20 个请求全部返回 200
    [5] 半包         请求分两段发送（间隔 0.5s）→ 必须收全再响应（CP5a）
    [6] 极端半包     请求逐字节发送 → 必须收全再响应（CP5a）
    [7] 超长请求头   发 16KB 不含 \\r\\n\\r\\n 的数据 → 必须拒绝或断开（CP5a 的大小上限）
    [8] POST body    Content-Length 声明的 body 完整送达 → 200（CP5b）
    [9] body 未齐    只发一半 body 时 → 服务器绝不能提前响应（CP5b 核心）
    [10] 超大 body   Content-Length 声明 64MB → 必须拒绝或断开，不能无限等（CP5b）

    [2][3] 对应"响应只在 n > 0 时发送 + 忽略 SIGPIPE"的回归测试
    [5][6] 对应"按连接读缓冲区，攒够完整请求再处理"
    [7]    对应 MAX_HEADER_SIZE（没有上限的实现会一直等下去 = DoS）
    [8][9][10] 对应"头部完整 ≠ 请求完整"：还要按 Content-Length 把 body 收全

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
    """半包：请求分两段发送（中间隔 0.5s），服务器必须收全再响应。

    CP5a 之前这里只能"观察"；现在是硬断言 —— 收不全就不该回 200。
    """
    print('\n[5] 半包：请求分两段发送（间隔 0.5s）')
    resp = talk(pieces=[
        b'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n',
        b'Connection: close\r\n\r\n',
    ], gap=0.5)
    status, _, body = parse(resp)
    r.check('返回 200（说明收全了完整请求才响应）', status == 200,
            '实际=%s' % status)
    r.check('响应体完整', body == EXPECT_BODY, '实际=%r' % body[:60])


def t6_byte_by_byte(r):
    """极端半包：请求的每个字节单独发送（最恶劣的拆包方式）"""
    print('\n[6] 极端半包：请求逐字节发送（每字节间隔 5ms，共 %d 字节）' % len(REQUEST))
    pieces = [bytes([b]) for b in REQUEST]
    resp = talk(pieces=pieces, gap=0.005, timeout=5.0)
    status, _, body = parse(resp)
    r.check('逐字节发送仍返回 200', status == 200, '实际=%s' % status)
    r.check('响应体完整', body == EXPECT_BODY, '实际=%r' % body[:60])


def t7_oversized_header(r, nbytes=16 * 1024, wait=2.0):
    """安全上限：发一批永远不含 CRLF CRLF 的数据。

    正确行为：服务器主动断开，或返回 431（Request Header Fields Too Large）/ 413。
    错误行为：一直 read 等下去 —— 那意味着 read_buffer 会随客户端发送量无限增长（DoS）。

    "连接被断开" 和 "服务器还在傻等" 必须区分开：
      recv 返回 b'' 或抛 ConnectionError → 断开（正确）
      recv 超时（socket.timeout）        → 还在等（没有上限，错误）
    """
    print('\n[7] 超长请求头：发送 %d KB 不含 \\r\\n\\r\\n 的数据' % (nbytes // 1024))
    s = socket.create_connection((HOST, PORT), timeout=5)
    closed = False
    data = b''
    try:
        s.sendall(b'X' * nbytes)
        s.settimeout(wait)
        while True:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break                       # 服务器还在等 → 没有上限
            except ConnectionError:
                closed = True               # 被 RST / 连接异常
                break
            if not chunk:
                closed = True               # 对端正常关闭
                break
            data += chunk
    finally:
        s.close()

    status, _, _ = parse(data)
    if closed:
        r.info('服务器的处理方式', '直接断开了连接（可以接受）')
        r.check('超长请求头被拒绝或断开', True)
    elif status in (431, 413):
        r.info('服务器的处理方式', '返回了 %s（最规范的做法）' % status)
        r.check('超长请求头被拒绝或断开', True)
    else:
        r.check('超长请求头被拒绝或断开', False,
                '%ds 内既没回错误也没断开 —— 说明没有请求头大小上限，'
                'read_buffer 会无限增长（DoS）' % wait)

    time.sleep(0.3)
    ok, detail = normal_get_ok()
    r.check('之后服务器仍能正常服务', ok, detail)


def t8_post_with_body(r):
    """CP5b：带 body 的 POST —— 头部 + 完整 body 一起送达，应当返回 200"""
    print('\n[8] POST 请求：Content-Length 声明的 body 完整送达')
    body = b'name=Bob&age=25'
    req = (b'POST /submit HTTP/1.1\r\nHost: 127.0.0.1\r\n'
           b'Content-Length: %d\r\nConnection: close\r\n\r\n' % len(body)) + body
    resp = talk(payload=req)
    status, _, _ = parse(resp)
    r.check('返回 200（带 body 的请求被正确处理）', status == 200, '实际=%s' % status)


def t9_body_split_no_early_response(r):
    """CP5b 核心：Content-Length 声明 10 字节，先只发 5 字节。

    服务器不能因为"头部已经以 CRLF CRLF 结束"就提前响应 —— 必须等 body 收齐。
    """
    print('\n[9] body 未到齐：先发 5 字节 body，1 秒后补剩下的 5 字节')
    s = socket.create_connection((HOST, PORT), timeout=5)
    try:
        s.sendall(b'POST /submit HTTP/1.1\r\nHost: 127.0.0.1\r\n'
                  b'Content-Length: 10\r\nConnection: close\r\n\r\n'
                  b'hello')                             # body 只发了 5 / 10
        s.settimeout(1.0)
        early = b''
        try:
            early = s.recv(4096)
        except socket.timeout:
            pass                                        # 超时 = 服务器在等 body（正确）
        except ConnectionError:
            pass
        r.check('body 没到齐时服务器不响应', early == b'',
                '服务器提前响应了：%r' % early[:40])

        s.sendall(b'world')                             # 补齐 body
        s.settimeout(3.0)
        data = b''
        while True:
            try:
                chunk = s.recv(4096)
            except (socket.timeout, ConnectionError):
                break
            if not chunk:
                break
            data += chunk
        status, _, _ = parse(data)
        r.check('补齐 body 后返回 200', status == 200, '实际=%s' % status)
    finally:
        s.close()


def t10_oversized_body(r, declared=64 * 1024 * 1024, wait=2.0):
    """CP5b：声明超大 body 却一个字节都不发 —— 必须拒绝或断开，不能无限等。"""
    print('\n[10] 超大 body：Content-Length: %d（%d MB），但一个字节都不发'
          % (declared, declared // 1024 // 1024))
    s = socket.create_connection((HOST, PORT), timeout=5)
    closed = False
    data = b''
    try:
        s.sendall(b'POST /submit HTTP/1.1\r\nHost: 127.0.0.1\r\n'
                  b'Content-Length: %d\r\nConnection: close\r\n\r\n' % declared)
        s.settimeout(wait)
        while True:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break                                   # 服务器还在等 → 没有 body 上限
            except ConnectionError:
                closed = True
                break
            if not chunk:
                closed = True
                break
            data += chunk
    finally:
        s.close()

    status, _, _ = parse(data)
    if status is not None and 200 <= status < 300:
        r.check('超大 body 被拒绝或断开', False,
                '服务器对"声明 %dMB body 却一个字节没发"的请求直接回了 %s '
                '—— 既没等 body，也没有大小上限' % (declared // 1024 // 1024, status))
    elif closed:
        r.info('服务器的处理方式', '直接断开了连接（可以接受）')
        r.check('超大 body 被拒绝或断开', True)
    elif status in (413, 431, 400):
        r.info('服务器的处理方式', '返回了 %s' % status)
        r.check('超大 body 被拒绝或断开', True)
    else:
        r.check('超大 body 被拒绝或断开', False,
                '%ds 内既没回错误也没断开 —— 没有 body 大小上限' % wait)

    time.sleep(0.3)
    ok, detail = normal_get_ok()
    r.check('之后服务器仍能正常服务', ok, detail)


def _check_body_waited(r, header_line, label):
    """发一个"声明了 body"的请求，但**先只发一部分 body**。

    为什么必须这样测？因为解析失败的**症状不是报错，而是提前响应**：
      - 正确：识别出 Content-Length → 等 body 收齐 → 补齐后返回 200
      - 错误：没识别出 Content-Length（当成 0）→ 头一发完就回 200，body 被丢弃
    只看"最终返回 200"是抓不住 bug 的 —— 两种行为都会返回 200。
    """
    body = b'hello'
    s = socket.create_connection((HOST, PORT), timeout=5)
    try:
        s.sendall(b'POST /submit HTTP/1.1\r\nHost: 127.0.0.1\r\n' +
                  header_line + b'\r\nConnection: close\r\n\r\n' + body[:2])
        s.settimeout(1.0)
        early = b''
        try:
            early = s.recv(4096)
        except socket.timeout:
            pass                       # 超时 = 服务器在等 body（正确）
        except ConnectionError:
            pass
        r.check('%s：body 没到齐时服务器不提前响应' % label, early == b'',
                '服务器提前响应了：%r  ← 说明它没认出 Content-Length' % early[:40])

        s.sendall(body[2:])            # 补齐 body
        s.settimeout(3.0)
        data = b''
        while True:
            try:
                chunk = s.recv(4096)
            except (socket.timeout, ConnectionError):
                break
            if not chunk:
                break
            data += chunk
        status, _, _ = parse(data)
        r.check('%s：补齐 body 后返回 200' % label, status == 200, '实际=%s' % status)
    finally:
        s.close()


def t11_lowercase_content_length(r):
    """头字段名【大小写不敏感】（RFC 9110 §5.1: Field names are case-insensitive）。

    修复前：read_buffer.find("Content-Length: ") 精确匹配 → 小写形式找不到
            → content_len 当成 0 → 头读完就响应，body 被丢弃。
    """
    print('\n[11] 小写头字段名：content-length（协议规定大小写不敏感）')
    _check_body_waited(r, b'content-length: 5', '小写 content-length')


def t12_content_length_no_space(r):
    """冒号后的空白是【可选】的（RFC 9112 §5: field-name ":" OWS field-value）。

    修复前：find("Content-Length: ") 要求冒号后恰好一个空格 → "Content-Length:5" 找不到。
    """
    print('\n[12] 无空格头字段：Content-Length:5（冒号后直接跟值）')
    _check_body_waited(r, b'Content-Length:5', '无空格写法')


def t13_content_length_junk(r):
    """Content-Length 的值必须是纯数字 —— 非法值必须被拒绝。

    修复前：std::stoul("10abc") 会返回 10（解析到非数字就停），非法值被当成合法值。
    RFC 9112 §6.3 要求：无效的 Content-Length 必须以 400 拒绝（或直接断开）。
    """
    print('\n[13] 非法值：Content-Length: 10abc（不能只取前面的数字）')
    s = socket.create_connection((HOST, PORT), timeout=5)
    closed = False
    data = b''
    try:
        s.sendall(b'POST /submit HTTP/1.1\r\nHost: 127.0.0.1\r\n'
                  b'Content-Length: 10abc\r\nConnection: close\r\n\r\n5hello')
        s.settimeout(2.0)
        while True:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            except ConnectionError:
                closed = True
                break
            if not chunk:
                closed = True
                break
            data += chunk
    finally:
        s.close()

    status, _, _ = parse(data)
    if closed:
        r.info('服务器的处理方式', '断开了连接（正确）')
        r.check('非法 Content-Length 被拒绝', True)
    elif status is not None and 400 <= status < 500:
        r.info('服务器的处理方式', '返回了 %s' % status)
        r.check('非法 Content-Length 被拒绝', True)
    else:
        r.check('非法 Content-Length 被拒绝', False,
                '服务器回了 %s —— 说明 "10abc" 被当成了合法值' % status)


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
    t6_byte_by_byte(r)
    t7_oversized_header(r)
    t8_post_with_body(r)
    t9_body_split_no_early_response(r)
    t10_oversized_body(r)
    t11_lowercase_content_length(r)
    t12_content_length_no_space(r)
    t13_content_length_junk(r)

    print('\n' + '=' * 64)
    print(' 结果：通过 %d 项，失败 %d 项' % (r.passed, r.failed))
    print('=' * 64)
    return 0 if r.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
