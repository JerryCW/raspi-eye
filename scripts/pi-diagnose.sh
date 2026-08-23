#!/usr/bin/env bash
# pi-diagnose.sh — Spec 33 Task 0: one-shot forensic capture for WebRTC stall
#
# Captures the live state of the raspi-eye process WITHOUT restarting it:
#   - process/thread inventory (/proc, ps)
#   - full thread backtraces via gdb (pauses process ~1-2s, read-only)
#   - TCP/TLS connections owned by the process (signaling WebSocket state)
#   - journald log-signature analysis for the three stall hypotheses:
#       A) zombie signaling  — "Cannot send answer: signaling not connected"
#       B) silent msg pump   — webrtc logger goes completely quiet
#       C) backoff storm     — rapid "Reconnect attempt" bursts
#
# Run it the moment WebRTC is observed dead (viewers cannot connect) and
# BEFORE restarting the service. All captures are read-only / best-effort.
#
# Usage:
#   macOS (remote):  ./scripts/pi-diagnose.sh          # SSH to Pi, run, pull results
#   Pi 5  (local):   ./scripts/pi-diagnose.sh          # capture locally
#
# Env: PI_HOST (default raspberrypi.local), PI_USER (default pi)
set -u

PI_HOST="${PI_HOST:-raspberrypi.local}"
PI_USER="${PI_USER:-pi}"
SERVICE="raspi-eye.service"
PROC_NAME="raspi-eye"
REMOTE_OUT="/tmp/raspi-eye-diagnose"

OS="$(uname -s)"

# ──────────────────────────────────────────────────────────────────────
# Remote mode: ship this script to the Pi via stdin, then pull results.
# ──────────────────────────────────────────────────────────────────────
if [ "${OS}" != "Linux" ]; then
    echo "[pi-diagnose] Remote mode: capturing on ${PI_USER}@${PI_HOST}"
    ssh -o ConnectTimeout=5 -o BatchMode=yes "${PI_USER}@${PI_HOST}" true || {
        echo "[pi-diagnose] ERROR: cannot connect to ${PI_USER}@${PI_HOST}" >&2
        exit 1
    }
    ssh "${PI_USER}@${PI_HOST}" 'bash -s' < "$0" || {
        echo "[pi-diagnose] ERROR: remote capture failed" >&2
        exit 1
    }
    TS="$(date '+%Y%m%d-%H%M%S')"
    LOCAL_OUT="/tmp/raspi-eye-diagnose-${TS}"
    mkdir -p "${LOCAL_OUT}"
    scp -q -r "${PI_USER}@${PI_HOST}:${REMOTE_OUT}/" "${LOCAL_OUT}/" || {
        echo "[pi-diagnose] ERROR: failed to pull results" >&2
        exit 1
    }
    echo "[pi-diagnose] Results pulled to: ${LOCAL_OUT}"
    echo "[pi-diagnose] Read verdict:      ${LOCAL_OUT}/raspi-eye-diagnose/90-verdict.txt"
    exit 0
fi

# ──────────────────────────────────────────────────────────────────────
# Local mode (running on the Pi): actual capture.
# ──────────────────────────────────────────────────────────────────────
rm -rf "${REMOTE_OUT}"
mkdir -p "${REMOTE_OUT}"
cd "${REMOTE_OUT}"

log() { echo "[pi-diagnose] $*"; }

# journalctl may need elevated access; prefer passwordless sudo, else direct.
JCTL="journalctl"
if sudo -n true 2>/dev/null; then JCTL="sudo -n journalctl"; fi

log "capture started: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
date -u '+%Y-%m-%dT%H:%M:%SZ' > 00-capture-time.txt
uname -a >> 00-capture-time.txt

# ---- 1. locate process ----
PID="$(systemctl show -p MainPID --value "${SERVICE}" 2>/dev/null || true)"
if [ -z "${PID}" ] || [ "${PID}" = "0" ]; then
    PID="$(pgrep -x "${PROC_NAME}" | head -1 || true)"
fi
systemctl status "${SERVICE}" --no-pager > 01-service-status.txt 2>&1 || true

if [ -z "${PID}" ] || [ "${PID}" = "0" ]; then
    log "WARNING: ${PROC_NAME} process not found — process-level capture skipped"
    echo "process not running at capture time" > 02-process.txt
else
    log "target pid: ${PID}"
    # ---- 2. process / thread inventory ----
    {
        echo "== /proc/${PID}/status =="
        cat "/proc/${PID}/status"
        echo
        echo "== open fd count =="
        ls "/proc/${PID}/fd" 2>/dev/null | wc -l
        echo
        echo "== memory (smaps_rollup) =="
        cat "/proc/${PID}/smaps_rollup" 2>/dev/null || true
    } > 02-process.txt 2>&1
    ps -L -p "${PID}" -o tid,comm,pcpu,state,wchan:32 > 03-threads.txt 2>&1 || true

    # ---- 3. full thread backtraces (pauses process ~1-2s) ----
    if command -v gdb >/dev/null 2>&1; then
        log "attaching gdb for thread backtraces (process paused briefly)..."
        sudo -n gdb -p "${PID}" -batch \
            -ex 'set pagination off' \
            -ex 'thread apply all bt' > 04-gdb-backtraces.txt 2>&1 \
        || gdb -p "${PID}" -batch \
            -ex 'set pagination off' \
            -ex 'thread apply all bt' > 04-gdb-backtraces.txt 2>&1 \
        || echo "gdb attach failed (need sudo or CAP_SYS_PTRACE)" > 04-gdb-backtraces.txt
    else
        echo "gdb not installed — run: sudo apt install gdb" > 04-gdb-backtraces.txt
        log "WARNING: gdb not installed, backtraces skipped"
    fi

    # ---- 4. network state (signaling WebSocket is a TLS conn to port 443) ----
    ss -tnpi 2>/dev/null | grep -E "pid=${PID}|State|:443" > 05-sockets.txt 2>&1 || true
fi

# ---- 5. log signature analysis (journald) ----
${JCTL} -u "${SERVICE}" --since "-72h" --no-pager > 10-journal-72h.txt 2>&1 || true
grep -i "webrtc" 10-journal-72h.txt | tail -500 > 11-webrtc-tail.txt 2>&1 || true

sig_count() { grep -c "$1" 10-journal-72h.txt 2>/dev/null || echo 0; }
last_line_time() { grep "$1" 10-journal-72h.txt 2>/dev/null | tail -1 | awk '{print $1, $2, $3}'; }

CANNOT_SEND="$(sig_count 'Cannot send answer: signaling not connected')"
NOT_CONNECTED="$(sig_count 'Signaling health: NOT connected')"
RECONNECT_ATT="$(sig_count 'Reconnect attempt')"
OFFERS="$(sig_count 'Received SDP offer')"
LAST_WEBRTC="$(grep -i 'webrtc' 10-journal-72h.txt | tail -1 | awk '{print $1, $2, $3}')"
LAST_OFFER="$(last_line_time 'Received SDP offer')"
LAST_STATE="$(grep 'Signaling state:' 10-journal-72h.txt | tail -5)"

{
    echo "=================================================================="
    echo "raspi-eye WebRTC stall forensic verdict  ($(date -u '+%Y-%m-%dT%H:%M:%SZ'))"
    echo "window: last 72h of journald"
    echo "=================================================================="
    echo "signature counts:"
    echo "  'Cannot send answer: signaling not connected' : ${CANNOT_SEND}"
    echo "  'Signaling health: NOT connected'             : ${NOT_CONNECTED}"
    echo "  'Reconnect attempt'                           : ${RECONNECT_ATT}"
    echo "  'Received SDP offer'                          : ${OFFERS}"
    echo "last webrtc log line at : ${LAST_WEBRTC:-none}"
    echo "last SDP offer received : ${LAST_OFFER:-none}"
    echo "last signaling states   :"
    echo "${LAST_STATE:-  none}"
    echo
    echo "interpretation guide:"
    echo "  A) zombie signaling  — 'Cannot send answer' / 'NOT connected' repeat"
    echo "     while offers still arrive => connected flag stuck false, no recreate."
    echo "  B) silent msg pump   — webrtc lines stop entirely (no offers, no states)"
    echo "     while process alive => message thread blocked/deadlocked; check"
    echo "     04-gdb-backtraces.txt for threads stuck in pthread_mutex/cond wait"
    echo "     inside on_viewer_offer / signalingClientSendMessageSync."
    echo "  C) backoff storm     — 'Reconnect attempt' bursts seconds apart"
    echo "     => backoff shift overflow after a long outage."
    echo "  If offers arrive and answers are sent but viewers still fail,"
    echo "  capture is inconclusive at signaling layer => peer/ICE layer."
    echo "=================================================================="
} > 90-verdict.txt

cat 90-verdict.txt
log "capture complete: ${REMOTE_OUT}"
