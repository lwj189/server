#!/usr/bin/env bash
#
# p2 服务器压测驱动（JMeter）
#
# 用法：
#   ./tests/load_test.sh                         # 默认阶梯：1 / 10 / 50 / 100 并发
#   ./tests/load_test.sh 200 500                 # 只跑一档：200 并发 × 500 次
#   LEVELS="1:500 20:300 100:200" ./tests/load_test.sh   # 自定义阶梯
#   PORT=9999 ./tests/load_test.sh               # 换端口（要和 p2.cpp 里的 PORT 一致）
#
# 它做五件事：
#   1. 严格编译 p2.cpp（有警告就停）
#   2. 启动服务器，等它真的进入 LISTEN（并打印 Recv-Q / Send-Q）
#   3. 逐档跑 JMeter（tests/http_load.jmx），结果存进 tests/results/
#   4. 调用 tests/analyze.sh 出完整分析表   ← 统计逻辑只有那一份，不在这里重复
#   5. 看 TIME_WAIT 残留和 accept 总数
#
# Ctrl+C 安全：脚本退出时自动杀掉服务器（trap EXIT）
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # 先记住脚本所在目录（tests/）
cd "$SCRIPT_DIR/.."                            # 再切到 server/，无论从哪里调用

# ---------------- 配置 ----------------
JMETER="${JMETER:-$HOME/apache-jmeter-5.6.3/bin/jmeter}"
PLAN="tests/http_load.jmx"                     # 通用模板
ANALYZE="$SCRIPT_DIR/analyze.sh"               # 通用分析器
OUTDIR="tests/results"
PORT="${PORT:-8888}"                           # 要和服务器源码里的 constexpr PORT 一致
RAMPUP=1                                       # 每档的爬坡时间（秒）

# 压谁？（默认 p2 阻塞版）
#   压 epoll 版：  BIN=./epoll SRC=epoll.cpp TAG=e- ./tests/load_test.sh
SRC="${SRC:-p2.cpp}"
BIN="${BIN:-./p2}"
KEEPALIVE="${KEEPALIVE:-false}"                # 透传给 .jmx 的 -Jkeepalive
TAG="${TAG:-}"                                 # 结果文件名前缀，避免不同服务器/配置互相覆盖

# 阶梯格式：并发:循环次数
if [ $# -eq 2 ]; then
    LEVELS="$1:$2"                             # 命令行给两个参数 → 只跑一档
else
    LEVELS="${LEVELS:-1:1000 10:200 50:200 100:200}"
fi

# ---------------- 0. 环境检查 ----------------
[ -x "$JMETER" ]  || { echo "✗ 找不到 JMeter：$JMETER"; exit 1; }
[ -f "$PLAN" ]    || { echo "✗ 找不到测试计划：$PLAN"; exit 1; }
[ -x "$ANALYZE" ] || { echo "✗ 找不到分析脚本：$ANALYZE"; exit 1; }
mkdir -p "$OUTDIR"

if ss -tln 2>/dev/null | grep -q ":$PORT "; then
    echo "✗ 端口 $PORT 已被占用："
    ss -tlnp 2>/dev/null | grep ":$PORT "
    echo "  先停掉它再跑（kill <pid> 或 pkill -x p2）"
    exit 1
fi

# ---------------- 1. 编译 ----------------
echo "==> 编译 $SRC ..."
if ! g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion "$SRC" -o "$BIN" 2> "$OUTDIR/build.log"; then
    echo "✗ 编译失败："
    cat "$OUTDIR/build.log"
    exit 1
fi
[ -s "$OUTDIR/build.log" ] && { echo "✗ 有警告，先修干净："; cat "$OUTDIR/build.log"; exit 1; }
echo "    ✓ 编译通过（零错误零警告）"

# ---------------- 2. 启动服务器 ----------------
echo "==> 启动服务器 $BIN（端口 $PORT，keepalive=$KEEPALIVE）..."
"$BIN" > "$OUTDIR/srv.log" 2>&1 &
SRV_PID=$!

cleanup() {
    kill "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
    echo
    echo "==> 服务器已停止 (pid=$SRV_PID)"
}
trap cleanup EXIT INT TERM

for _ in $(seq 1 30); do
    ss -tln 2>/dev/null | grep -q ":$PORT " && break
    sleep 0.1
done
LINE=$(ss -tln 2>/dev/null | grep ":$PORT ")
[ -n "$LINE" ] || { echo "✗ 服务器没起来，看 $OUTDIR/srv.log"; exit 1; }
echo "    ✓ $LINE"
echo "      Recv-Q=$(echo "$LINE" | awk '{print $2}')（当前排队数）  Send-Q=$(echo "$LINE" | awk '{print $3}')（= backlog）"

# ---------------- 3. 逐档压测 ----------------
echo
echo "==> 开始压测（阶梯：$LEVELS）"
GENERATED=""                                   # 本轮生成的结果文件（下面累加）

for lv in $LEVELS; do
    THREADS="${lv%%:*}"
    LOOPS="${lv##*:}"
    LABEL="${TAG}t${THREADS}x${LOOPS}"
    JTL="$OUTDIR/$LABEL.jtl"
    rm -f "$JTL"
    GENERATED="$GENERATED $JTL"                # 先删后记，避免读到旧结果

    echo -n "    ${THREADS} 并发 × ${LOOPS} 次 ... "
    # -j：把 JMeter 自己的日志收进 results/，否则会落在当前目录
    "$JMETER" -n -j "$OUTDIR/$LABEL.jmeter.log" -t "$PLAN" -l "$JTL" \
              -Jhost=127.0.0.1 -Jport="$PORT" \
              -Jthreads="$THREADS" -Jloops="$LOOPS" -Jrampup="$RAMPUP" \
              -Jkeepalive="$KEEPALIVE" \
              > /dev/null 2>&1
    echo "完成"
done

# ---------------- 4. 分析：复用 analyze.sh（统计逻辑只有一份）----------------
echo
echo "==> 结果分析"
# shellcheck disable=SC2086
"$ANALYZE" $GENERATED

# ---------------- 5. 收尾观察 ----------------
echo
echo "==> TIME_WAIT 残留（服务器先 close，所以它自己攒 60 秒）: $(ss -tan state time-wait 2>/dev/null | grep -c ":$PORT ")"
echo "==> 服务器 accept 的连接数: $(grep -c '新连接' "$OUTDIR/srv.log")"
echo "    结果文件：$OUTDIR/"
echo "    HTML 报告：$JMETER -g $OUTDIR/${LABEL}.jtl -o $OUTDIR/report"
