#!/bin/bash
# diagnose-cpu.sh
# Spec 32 需求 7：采集 raspi-eye 进程 CPU 基线 + 检测管道 PAUSED，用于判断
# 管道 PAUSED 是否由 CPU 饱和（source/encoder 跟不上）诱发。
#
# 用法：./scripts/diagnose-cpu.sh [DURATION_SEC]
#   DURATION_SEC：采集时长，默认 600（10 分钟）
#
# 输出文件（/tmp）：
#   raspi-eye-cpu.log    各线程 CPU（pidstat -t）
#   raspi-eye-top.log    进程整体 CPU + 整机负载（top）
#   raspi-eye-paused.log 管道 PAUSED / Health state 日志（journalctl grep）
#
# 判定标准（见脚本末尾输出）：
#   - 进程 CPU 持续接近 核数×100%（Pi 5 四核 ≈ 400%）或 source 线程频繁满载
#     => PAUSED 与 CPU 饱和强相关，需后续单开 spec 优化旋转/编码
#   - 否则排除 CPU 因素，PAUSED 归因于 KVS/摄像头瞬时故障（需求 1/2/3 已覆盖）
set -u

DURATION=${1:-600}
INTERVAL=5
COUNT=$((DURATION / INTERVAL))

CPU_LOG=/tmp/raspi-eye-cpu.log
TOP_LOG=/tmp/raspi-eye-top.log
PAUSED_LOG=/tmp/raspi-eye-paused.log

pid=$(pgrep -x raspi-eye)
if [ -z "$pid" ]; then
    echo "ERROR: raspi-eye process not found (is the service running?)"
    exit 1
fi

echo "Diagnosing raspi-eye (pid=$pid) for ${DURATION}s (interval=${INTERVAL}s, count=${COUNT})"
echo "Logs: $CPU_LOG, $TOP_LOG, $PAUSED_LOG"

# 1) Per-thread CPU via pidstat (requires sysstat package)
if command -v pidstat >/dev/null 2>&1; then
    pidstat -p "$pid" -t "$INTERVAL" "$COUNT" > "$CPU_LOG" 2>&1 &
    PIDSTAT_PID=$!
else
    echo "WARNING: pidstat not found (install sysstat for per-thread CPU)"
    PIDSTAT_PID=""
fi

# 2) Process CPU + load average via top (batch mode)
top -b -d "$INTERVAL" -n "$COUNT" -p "$pid" > "$TOP_LOG" 2>&1 &
TOP_PID=$!

# 3) Pipeline PAUSED / Health state transitions
journalctl -u raspi-eye -f --since now 2>/dev/null \
    | grep --line-buffered -E "Heartbeat: pipeline state|Health state|FATAL|recovery|teardown" \
    > "$PAUSED_LOG" &
JOURNAL_PID=$!

# Wait for sampling commands to finish
[ -n "$PIDSTAT_PID" ] && wait "$PIDSTAT_PID"
wait "$TOP_PID"

# Stop the journal follower
kill "$JOURNAL_PID" 2>/dev/null

echo ""
echo "==================== 判定 ===================="
echo "1) 查看进程整体 CPU（%CPU 列），是否持续接近 400%（四核饱和）："
echo "   grep raspi-eye $TOP_LOG | tail -20"
echo "2) 查看各线程 CPU，source/encoder 线程是否频繁满载："
echo "   tail -40 $CPU_LOG"
echo "3) 采集期间是否出现 PAUSED / 恢复："
echo "   cat $PAUSED_LOG"
echo ""
echo "结论写入 docs/development-trace.md："
echo "  - CPU 持续饱和 + 伴随 PAUSED  => CPU 饱和诱发，单开 spec 优化旋转/编码"
echo "  - CPU 未饱和                  => 排除 CPU 因素，PAUSED 归因瞬时故障（已由需求 1/2/3 覆盖）"
