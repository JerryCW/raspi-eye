#!/bin/bash
# diagnose-cpu.sh
# Spec 32 / Requirement 7: collect raspi-eye process CPU baseline + detect pipeline
# PAUSED, to determine whether pipeline PAUSED is induced by CPU saturation
# (source/encoder cannot keep up).
#
# Usage: ./scripts/diagnose-cpu.sh [DURATION_SEC]
#   DURATION_SEC: sampling duration, default 600 (10 minutes). Use a small value
#                 (e.g. 60) first to confirm the script collects data correctly.
#
# Output files (/tmp):
#   raspi-eye-cpu.log    per-thread CPU (pidstat if available, else top -H)
#   raspi-eye-top.log    process overall CPU + system load (top)
#   raspi-eye-paused.log pipeline PAUSED / Health state log lines (journalctl grep)
#
# Decision criteria (see end-of-run output):
#   - process CPU stays near cores*100% (Pi 5 quad-core ~= 400%) or a source/encoder
#     thread is frequently maxed out => PAUSED strongly correlates with CPU saturation,
#     open a follow-up spec to optimize rotation/encoding.
#   - otherwise => CPU ruled out, PAUSED attributed to KVS/camera transient faults
#     (already covered by requirements 1/2/3).
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

# 1) Per-thread CPU: prefer pidstat (sysstat); fall back to top -H if unavailable.
if command -v pidstat >/dev/null 2>&1; then
    echo "Per-thread CPU sampler: pidstat"
    pidstat -p "$pid" -t "$INTERVAL" "$COUNT" > "$CPU_LOG" 2>&1 &
    CPU_PID=$!
else
    echo "Per-thread CPU sampler: top -H (pidstat not found; install sysstat for richer output)"
    # top -H shows per-thread rows; -w 512 prevents COMMAND column truncation.
    top -b -H -w 512 -d "$INTERVAL" -n "$COUNT" -p "$pid" > "$CPU_LOG" 2>&1 &
    CPU_PID=$!
fi

# 2) Process overall CPU + load average via top (batch mode, wide output)
top -b -w 512 -d "$INTERVAL" -n "$COUNT" -p "$pid" > "$TOP_LOG" 2>&1 &
TOP_PID=$!

# 3) Pipeline PAUSED / Health state transitions (best-effort; needs journald access)
journalctl -u raspi-eye -f --since now 2>/dev/null \
    | grep --line-buffered -E "Heartbeat: pipeline state|Health state|FATAL|recovery|teardown" \
    > "$PAUSED_LOG" &
JOURNAL_PID=$!

# Wait for the sampling commands to finish
wait "$CPU_PID"
wait "$TOP_PID"

# Stop the journal follower
kill "$JOURNAL_PID" 2>/dev/null

echo ""
echo "==================== ANALYSIS ===================="
echo "1) Process overall CPU (data rows are anchored by pid=$pid; %CPU column):"
echo "   grep -E '^[[:space:]]*$pid ' $TOP_LOG | tail -20"
echo "2) Per-thread CPU (is any source/encoder thread frequently maxed out?):"
echo "   tail -40 $CPU_LOG"
echo "3) Any PAUSED / recovery during the run:"
echo "   cat $PAUSED_LOG"
echo ""
echo "Write conclusion into docs/development-trace.md:"
echo "  - CPU sustained near cores*100% + PAUSED present => CPU-induced; open spec to optimize rotation/encoding"
echo "  - CPU not saturated                              => CPU ruled out; PAUSED attributed to transient faults (req 1/2/3)"
