#!/usr/bin/env python3
"""空闲 keep-alive 连接的服务器内存成本（每连接 ≈? KB，用户态 + 内核态）。

用法:
    python3 tests/idle_conn_mem.py --bin build/thread --port 8890 --conns 5000 \
        --server-args "--workers 4 --quiet" --rounds 2
    python3 tests/idle_conn_mem.py --bin build/epoll --port 8888 --conns 5000 \
        --server-args "" --rounds 1

做什么:
    1. 起服务器，等端口可连，读 VmRSS 基线 + /proc/net/sockstat 的 TCP mem 基线
    2. 逐条建立连接：发一个 keep-alive 请求、读完整个响应，然后**不关**
       —— 这就是"空闲 keep-alive 连接"：握手完成、请求已答、服务器在等下一个请求
    3. 每 --batch 条打印一次 VmRSS
    4. 全部建完等 2s：每连接用户态 = (VmRSS 增量) / 连接数；每连接内核态 = (sockstat mem 增量) / 连接数
    5. 关掉所有客户端连接，等 2s 再读 VmRSS —— 内存有没有还回去
    6. --rounds 2 时再开第二批：第二批还涨同样的量 = 泄漏；几乎不涨 = 分配器复用
    7. 杀掉服务器

口径（写简历时照着说）:
    VmRSS 只算**服务器用户态**：fd 表、每连接的上下文、用户态缓冲区。
    /proc/net/sockstat 的 "TCP ... mem" 是内核给 TCP socket 记的账（页，1 页=4KB），
    不含客户端侧的 socket。两项都只算"增量"，不算进程本身的固定开销。
    同机回环，客户端与服务器抢同一台机器，数字只用于"每连接增量"这种相对量。
"""

import argparse
import os
import socket
import subprocess
import sys
import time


def read_rss_kb(pid):
    with open(f"/proc/{pid}/status", encoding="utf-8") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return None


def read_tcp_mem_kb():
    """/proc/net/sockstat: TCP: inuse N orphan N tw N alloc N mem N（mem 单位是页）"""
    with open("/proc/net/sockstat", encoding="utf-8") as f:
        for line in f:
            if line.startswith("TCP:"):
                parts = line.split()
                mem_pages = int(parts[parts.index("mem") + 1])
                alloc = int(parts[parts.index("alloc") + 1])
                return mem_pages * 4, alloc
    return 0, 0


def read_meminfo():
    """Slab / SUnreclaim / MemAvailable（KB）。内核给每个 socket 的结构体记在 slab 里。"""
    out = {}
    with open("/proc/meminfo", encoding="utf-8") as f:
        for line in f:
            k, _, v = line.partition(":")
            out[k] = int(v.split()[0])
    return out["Slab"], out["SUnreclaim"], out["MemAvailable"]


def wait_port(host, port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


REQUEST = b"GET / HTTP/1.1\r\nHost: mem\r\nConnection: keep-alive\r\n\r\n"


def read_one_response(sock):
    """读一条完整响应（头 + Content-Length 声明的 body）。"""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("连接在响应头之前就关了")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    length = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1].strip())
            break
    while len(rest) < length:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("body 没读完连接就关了")
        rest += chunk
    return len(head) + 4 + len(rest)


def open_batch(host, port, n, batch, pid, base_rss):
    conns = []
    t0 = time.time()
    for i in range(n):
        s = socket.create_connection((host, port), timeout=2.0)
        s.sendall(REQUEST)
        read_one_response(s)
        conns.append(s)
        if (i + 1) % batch == 0:
            rss = read_rss_kb(pid)
            print(f"  {i + 1:5d} 条  VmRSS = {rss:6d} KB   每连接 ≈ {(rss - base_rss) / (i + 1):.2f} KB")
    return conns, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/thread")
    ap.add_argument("--port", type=int, default=8890)
    ap.add_argument("--conns", type=int, default=5000)
    ap.add_argument("--batch", type=int, default=1000)
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--server-args", default="--workers 4 --quiet")
    args = ap.parse_args()

    if not os.access(args.bin, os.X_OK):
        sys.exit(f"找不到可执行文件: {args.bin}")

    cmd = [args.bin, "--port", str(args.port)] + args.server_args.split()
    print("启动:", " ".join(cmd))
    srv = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        if not wait_port(args.host, args.port):
            sys.exit("服务器 5s 内没起来")
        time.sleep(0.3)
        base_rss = read_rss_kb(srv.pid)
        base_mem, base_alloc = read_tcp_mem_kb()
        base_slab, base_sun, base_avail = read_meminfo()
        print(f"基线: VmRSS = {base_rss} KB，sockstat TCP mem = {base_mem} KB，alloc = {base_alloc}")

        for rnd in range(1, args.rounds + 1):
            print(f"--- 第 {rnd} 批 ---")
            conns, dt = open_batch(args.host, args.port, args.conns, args.batch, srv.pid, base_rss)
            n = len(conns)
            time.sleep(2.0)
            final_rss = read_rss_kb(srv.pid)
            mem_kb, alloc = read_tcp_mem_kb()
            print(f"{n} 条建完，耗时 {dt:.2f}s；VmRSS = {final_rss} KB"
                  f"（增量 {final_rss - base_rss} KB，每连接 ≈ {(final_rss - base_rss) / n:.2f} KB 用户态）")
            slab, sun, avail = read_meminfo()
            print(f"内核粗测: Slab 增量 {slab - base_slab} KB、SUnreclaim 增量 {sun - base_sun} KB、"
                  f"MemAvailable 减少 {base_avail - avail} KB（{n} 条 → "
                  f"约 {(base_avail - avail) / n:.2f} KB/连接，含噪声）")
            print(f"sockstat: TCP mem = {mem_kb} KB（只算排队数据，不代表 socket 结构体）；alloc = {alloc}")
            ss = subprocess.run(["bash", "-c",
                                 f"ss -tnH state established '( sport = :{args.port} )' | wc -l"],
                                capture_output=True, text=True).stdout.strip()
            print(f"ss 实测服务器侧 established = {ss}")

            for s in conns:
                s.close()
            conns.clear()
            time.sleep(2.0)
            after = read_rss_kb(srv.pid)
            print(f"全部关闭 2s 后 VmRSS = {after} KB（比峰值少 {final_rss - after} KB）")
            if rnd == 1:
                base_rss, base_mem = final_rss, read_tcp_mem_kb()[0]  # 第二批只算“比上一批多多少”
                base_slab, base_sun, base_avail = read_meminfo()
                print("（第二批基线更新为第一批的峰值 —— 第二批还涨 = 泄漏；不涨 = 分配器复用）")
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=3)
        except subprocess.TimeoutExpired:
            srv.kill()


if __name__ == "__main__":
    main()
