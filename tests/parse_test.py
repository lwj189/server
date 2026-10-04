#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
C++ 靶场 [1] 靶纸：请求行解析

用法（一条命令，红还是绿）：
    python3 tests/parse_test.py

它自己编译 request_line.cpp，然后把每个用例喂进去、对答案。
不需要起服务器、不占 8888 —— 十个用例加起来 0.05 秒。

契约（= 靶子该打中的地方，和 request_line.cpp 里的注释一一对应）：
    1. 恰好 3 段；连续多个空白算【一个】分隔符（RFC 9112 §3 允许宽容解析）
    2. 首尾空白忽略
    3. 三块都必须非空
    4. target 原样保留（`?query` 算它的一部分）
    5. 不是恰好 3 段 → 输出 ERR

⚠ 白名单说明：空白 = 空格 或 制表符。
   HTTP 的 field-line 用的 OWS 就是这两个（RFC 9112 §5），不包括换行。

退出码：0 = 全中，1 = 有脱靶，2 = 编译不过
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, 'request_line.cpp')
BIN = os.path.join(ROOT, 'request_line')

# (输入,             期望 method, 期望 target,  期望 version, 这个用例在打什么)
CASES = [
    ('GET / HTTP/1.1',        'GET',  '/',       'HTTP/1.1', '最基本的一行'),
    ('GET /a?b=1 HTTP/1.1',   'GET',  '/a?b=1',  'HTTP/1.1', '查询串算 target 的一部分'),
    ('POST /submit HTTP/1.1', 'POST', '/submit', 'HTTP/1.1', '方法原样保留，不做转换'),
    ('GET  /  HTTP/1.1',      'GET',  '/',       'HTTP/1.1', '连续多个空格 = 一个分隔符'),
    ('   GET / HTTP/1.1   ',  'GET',  '/',       'HTTP/1.1', '首尾空白要忽略'),
    ('GET\t/\tHTTP/1.1',      'GET',  '/',       'HTTP/1.1', '制表符也算空白（别只判空格）'),
    ('GET /',                 None,   None,      None,       '只有 2 段 → 不合法'),
    ('GET / HTTP/1.1 extra',  None,   None,      None,       '4 段 → 不合法'),
    ('',                      None,   None,      None,       '空行 → 不合法'),
    ('      ',                None,   None,      None,       '整行全空白 → 不合法'),
]


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


def build():
    """编译靶子。返回 (成功?, 编译器输出)"""
    cmd = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Wformat=2', '-Wconversion',
           SRC, '-o', BIN]
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode == 0, (p.stdout + p.stderr).strip()


def shoot(line):
    """打一发：把这一行喂给靶子，返回它打印的那一行"""
    p = subprocess.run([BIN], input=line.encode(), capture_output=True, timeout=5)
    return p.stdout.decode(errors='replace').strip()


def expect_str(method, target, version):
    if method is None:
        return 'ERR'
    return 'OK|%s|%s|%s' % (method, target, version)


def main():
    print('=' * 64)
    print(' C++ 靶场 [1]：请求行解析')
    print('=' * 64)

    ok, out = build()
    if not ok:
        print('\n[错误] 编译不过：\n')
        print(out)
        return 2
    if out:
        print('  [观察] 编译器有话说（这些不是废话，逐条看）：\n%s\n' % out)
    else:
        print('  [观察] 编译通过，零警告\n')

    r = Runner()
    for i, (line, m, t, v, why) in enumerate(CASES, 1):
        got = shoot(line)
        want = expect_str(m, t, v)
        show = line if len(line) <= 28 else line[:28] + '…'
        r.check('[%2d] %-30s %s' % (i, repr(show), why), got == want,
                '输入 %r → 期望 %s，实际 %s' % (line, want, got or '(无输出)'))

    print('\n' + '=' * 64)
    print(' 结果：命中 %d 项，脱靶 %d 项' % (r.passed, r.failed))
    print('=' * 64)
    if r.failed:
        print('\n手动打一发看看（改完先自己试，再跑本脚本）：')
        print("    printf 'GET /a?b=1 HTTP/1.1' | ./request_line")
    return 0 if r.failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
