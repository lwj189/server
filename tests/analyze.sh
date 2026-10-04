#!/usr/bin/env bash
#
# 通用 JMeter 结果分析器（.jtl）—— 和"是哪个服务器"完全无关
#
# 用法：
#   ./tests/analyze.sh tests/results/*.jtl
#   ./tests/analyze.sh ~/桌面/server/tests/results/t200x200.jtl
#
# 输出：
#   样本 / 失败 / 失败率 / 耗时 / 吞吐 / 平均 / p50 / p90 / p99 / 最大 / 建连p99
#   + 失败原因分类 + 失败率对比（找拐点）
#
# 为什么单独做成一个脚本：
#   1) 这部分逻辑对任何 JMeter 结果都通用（.jtl 的列格式由 JMeter 固定）
#   2) 先检查文件存在/非空 —— 避免"第一个命令失败、管道后面照跑"产出全 0 的假结果
#
# ⚠ 吞吐是按"首样本开始 → 末样本结束"算的。
#   如果同一个 .jtl 是多次运行**追加**出来的（JMeter 的 -l 是追加模式），
#   中间的空档会被算进去，吞吐会偏小 —— 看"耗时"列就能发现。
#
set -uo pipefail

[ $# -ge 1 ] || { echo "用法: $0 <结果.jtl> [更多.jtl ...]"; exit 1; }

# ---------------- 1. 先验文件（关键！）----------------
valid=()
for f in "$@"; do
    if   [ ! -e "$f" ]; then echo "✗ 文件不存在：$f" >&2
    elif [ ! -f "$f" ]; then echo "✗ 不是普通文件：$f" >&2
    elif [ ! -s "$f" ]; then echo "✗ 文件是空的：$f" >&2
    else valid+=("$f")
    fi
done
[ ${#valid[@]} -gt 0 ] || { echo "没有可分析的文件"; exit 1; }

# ---------------- 2. 汇总表 ----------------
#
# 列宽（必须和下面的数据行格式一致）：文件 样本 失败 失败率 耗时s 吞吐/s 平均ms p50 p90 p99 最大ms 建连p99
COLS=(24 8 7 8 8 9 8 6 6 7 8 9)
LABELS=("文件" "样本" "失败" "失败率" "耗时s" "吞吐/s" "平均ms" "p50" "p90" "p99" "最大ms" "建连p99")

# 显示宽度：CJK 字符在终端占 2 列，而 printf 的 %-Ns 按字节补齐 → 直接用会错位
dw() {
    local c
    c=$(printf '%s' "$1" | grep -oP '[\x{4e00}-\x{9fff}\x{3000}-\x{303f}\x{ff00}-\x{ffef}]' | wc -l)
    echo $(( ${#1} + c ))
}
lhdr() { local s="$1" t="$2" w; w=$(dw "$s"); printf '%s' "$s"; for (( i=w; i<t; i++ )); do printf ' '; done; }
rhdr() {
    # 注意：printf '%*s' 按"字节数"补齐，所以 N = 字节数 + (目标显示宽度 - 实际显示宽度)
    local s="$1" t="$2" w b
    w=$(dw "$s")
    b=$(printf '%s' "$s" | wc -c)
    printf '%*s' $(( b + t - w )) "$s"
}

# 表头：第一列左对齐（和数据行的 %-24s 一致），其余数字列右对齐
HDR="$(lhdr "${LABELS[0]}" "${COLS[0]}")"
for i in $(seq 1 $((${#COLS[@]} - 1))); do
    HDR="$HDR $(rhdr "${LABELS[$i]}" "${COLS[$i]}")"
done
echo "$HDR"

# 分隔线：总宽 = 各列宽之和 + 列间空格
TOTAL=0
for w in "${COLS[@]}"; do TOTAL=$(( TOTAL + w )); done
TOTAL=$(( TOTAL + ${#COLS[@]} - 1 ))
printf '%0.s-' $(seq 1 "$TOTAL"); echo

for f in "${valid[@]}"; do
    # 一趟 awk：样本数 / 失败数 / 平均 / 耗时 / 吞吐
    #
    # ⚠ $1 ~ /^[0-9]+$/ 这个守卫不能删：
    #   JMeter 的 failureMessage 字段可能【带换行】（失败详情里的 "****** received :"），
    #   那些续行会被当成新样本 —— 它们的 $1 不是数字，算进来会同时毁掉三样东西：
    #     · 样本数虚高（实测 10019 ≠ 10000）
    #     · $1 + 0 = 0 把 min 拉到 0 → 耗时算成 1.79e9 秒（56 年）
    #     · 吞吐 = 样本数 / 耗时 → 直接归零（最坑：看起来像"服务器崩了"）
    #   真正的样本行第一列是毫秒时间戳，一定是纯数字。
    read -r n err avg dur tput < <(awk -F, '
        NR > 1 && $1 ~ /^[0-9]+$/ {
            n++; sum += $2;
            if ($8 == "false") err++;
            ts = $1 + 0; te = ts + $2;
            if (min == "" || ts < min) min = ts;
            if (te > max) max = te;
        }
        END {
            d = (max - min) / 1000;
            printf "%d %d %.1f %.2f %.0f", n, err, (n ? sum/n : 0), d, (d > 0 ? n/d : 0);
        }' "$f")

    # 总耗时分位数（第 2 列 elapsed）
    read -r p50 p90 p99 pmax < <(awk -F, 'NR>1 && $1 ~ /^[0-9]+$/ {print $2}' "$f" | sort -n | awk '
        { a[NR] = $1 }
        END {
            if (NR == 0) { print "0 0 0 0"; exit }
            printf "%d %d %d %d", a[int(NR*0.5)], a[int(NR*0.9)], a[int(NR*0.99)], a[NR];
        }')

    # 建连 p99（最后一列 Connect：用来区分"建连慢"还是"处理慢"）
    cp99=$(awk -F, 'NR>1 && $1 ~ /^[0-9]+$/ {print $NF}' "$f" | sort -n | awk '{ a[NR] = $1 } END { print (NR ? a[int(NR*0.99)] : 0) }')

    rate=$(awk -v e="$err" -v n="$n" 'BEGIN { printf "%.2f%%", (n ? e*100/n : 0) }')

    printf "%-24s %8d %7d %8s %8s %9s %8.1f %6d %6d %7d %8d %9d\n" \
           "$(basename "$f")" "$n" "$err" "$rate" "$dur" "$tput" "$avg" "$p50" "$p90" "$p99" "$pmax" "$cp99"
done

# ---------------- 3. 失败原因分类 ----------------
any=0
for f in "${valid[@]}"; do
    msg=$(awk -F, 'NR>1 && $8=="false" { print $4 " | " substr($9,1,70) }' "$f" | sort | uniq -c | sort -rn)
    if [ -n "$msg" ]; then
        if [ $any -eq 0 ]; then echo; echo "失败原因："; any=1; fi
        echo "  $(basename "$f"):"
        echo "$msg" | sed 's/^/      /'
    fi
done
[ $any -eq 0 ] && { echo; echo "失败原因：无失败 ✅"; }

# ---------------- 4. 失败率对比（找拐点）----------------
if [ ${#valid[@]} -ge 2 ]; then
    echo
    echo "失败率随档位变化（柱长按最差的那档等比缩放）："

    rates=(); maxr=0
    for f in "${valid[@]}"; do
        n=$(awk -F, 'NR>1 && $1 ~ /^[0-9]+$/' "$f" | wc -l)
        e=$(awk -F, 'NR>1 && $1 ~ /^[0-9]+$/ && $8=="false"' "$f" | wc -l)
        r=$(awk -v e="$e" -v n="$n" 'BEGIN { printf "%.4f", (n ? e*100/n : 0) }')
        rates+=("$r")
        maxr=$(awk -v a="$maxr" -v b="$r" 'BEGIN { print (b > a ? b : a) }')
    done

    i=0
    for f in "${valid[@]}"; do
        r=${rates[$i]}
        bar=$(awk -v r="$r" -v m="$maxr" 'BEGIN {
            if (m <= 0) { k = 0 }
            else { k = int(r/m*30 + 0.5); if (k == 0 && r > 0) k = 1 }
            s = ""; for (j = 0; j < k; j++) s = s "#"; print s }')
        printf "  %-24s %7.2f%%  %s\n" "$(basename "$f")" "$r" "$bar"
        i=$((i + 1))
    done
fi
