#!/usr/bin/env bash
# webrtc-sdk-probe.sh
# Spec 33 Task 1: collect KVS WebRTC C SDK semantic evidence on the Pi.
# ASCII-only output (avoid terminal mojibake). Run on the Pi, not on macOS.
#
# Usage:
#   bash scripts/webrtc-sdk-probe.sh [SDK_SRC_DIR] > /tmp/kvs-webrtc-probe.txt 2>&1
#
# SDK source dir resolution order:
#   1) first CLI argument
#   2) $KVS_WEBRTC_SRC environment variable
#   3) common clone locations under $HOME
# Installed headers are searched under /usr/local/include and /opt/kvs-webrtc/include.

set -u

SRC_ARG="${1:-}"
ENV_SRC="${KVS_WEBRTC_SRC:-}"

echo "=================================================================="
echo "Spec 33 Task 1 - KVS WebRTC C SDK semantic probe"
echo "date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "host: $(uname -a)"
echo "=================================================================="

# ---- locate SDK source tree ----
SRC=""
for cand in "$SRC_ARG" "$ENV_SRC" \
    "$HOME/amazon-kinesis-video-streams-webrtc-sdk-c" \
    "$HOME/kvs-webrtc-sdk-c" \
    "$HOME/webrtc-sdk-c" \
    "$HOME/amazon-kinesis-video-streams-webrtc-sdk-c-master"; do
    if [ -n "$cand" ] && [ -d "$cand" ]; then
        SRC="$cand"
        break
    fi
done

if [ -z "$SRC" ]; then
    # last resort: search $HOME for a webrtc sdk checkout (bounded depth)
    SRC="$(find "$HOME" -maxdepth 4 -type d -name 'webrtcclient' 2>/dev/null \
        | head -n1 | sed 's#/src/.*##; s#/samples/.*##')"
fi

echo
echo "---- SDK source tree ----"
if [ -n "$SRC" ] && [ -d "$SRC" ]; then
    echo "SRC=$SRC"
else
    echo "SRC=NOT FOUND (pass the SDK source dir as first argument)"
fi

# ---- locate installed headers ----
INC=""
for cand in /usr/local/include /opt/kvs-webrtc/include; do
    if [ -f "$cand/com/amazonaws/kinesis/video/webrtcclient/Include.h" ]; then
        INC="$cand"
        break
    fi
done
echo
echo "---- installed headers ----"
if [ -n "$INC" ]; then
    echo "INC=$INC"
else
    echo "INC=NOT FOUND under /usr/local/include or /opt/kvs-webrtc/include"
fi

# ---- SDK version / commit ----
echo
echo "---- SDK version / commit ----"
if [ -n "$SRC" ] && [ -d "$SRC/.git" ]; then
    git -C "$SRC" describe --tags --always 2>/dev/null || echo "git describe failed"
    git -C "$SRC" rev-parse HEAD 2>/dev/null || echo "git rev-parse failed"
else
    echo "no .git under SRC; searching CMake version strings"
    grep -rns --include=CMakeLists.txt -E "project\\(|VERSION" "${SRC:-/nonexistent}" 2>/dev/null | head -n 20
fi

# ------------------------------------------------------------------
# helper: dump header declarations matching a pattern
# ------------------------------------------------------------------
hdr() {
    title="$1"; pat="$2"
    echo
    echo "=================================================================="
    echo "[HDR] $title"
    echo "pattern: $pat"
    echo "------------------------------------------------------------------"
    if [ -n "$INC" ]; then
        grep -rns -E "$pat" "$INC/com/amazonaws/kinesis/video/webrtcclient" 2>/dev/null | head -n 40
    fi
    if [ -n "$SRC" ]; then
        grep -rns --include=*.h -E "$pat" "$SRC" 2>/dev/null | head -n 40
    fi
}

# ------------------------------------------------------------------
# helper: dump source definition + following context lines
# ------------------------------------------------------------------
src_def() {
    title="$1"; func="$2"; ctx="${3:-45}"
    echo
    echo "=================================================================="
    echo "[SRC] $title  (function: $func, +$ctx lines)"
    echo "------------------------------------------------------------------"
    if [ -z "$SRC" ]; then
        echo "SRC not found; cannot show implementation"
        return
    fi
    # find definition sites: 'STATUS func(' or 'PUBLIC_API ... func(' at col start-ish
    hits="$(grep -rns --include=*.c -E "[A-Za-z_\\*] +$func\\(" "$SRC/src" 2>/dev/null | head -n 4)"
    if [ -z "$hits" ]; then
        hits="$(grep -rns --include=*.c -E "$func\\(" "$SRC/src" 2>/dev/null | head -n 4)"
    fi
    if [ -z "$hits" ]; then
        echo "no definition found for $func under $SRC/src"
        return
    fi
    echo "$hits" | while IFS=: read -r file line _; do
        [ -f "$file" ] || continue
        echo "--- $file:$line ---"
        sed -n "${line},$((line + ctx))p" "$file"
        echo "..."
    done
}

# ==================================================================
# GATE 1: status code symbols (SRTP-not-ready / RTP invalid NALU)
# ==================================================================
hdr "status code symbols" "STATUS_SRTP|STATUS_RTP_INVALID_NALU|0x5c000003|STATUS_SRTP_NOT_READY"

# ==================================================================
# GATE 2: ChannelInfo retry / reconnect semantics
# ==================================================================
hdr "ChannelInfo retry/reconnect fields" "reconnect|BOOL +retry|PChannelInfo|ChannelInfo"
src_def "signaling reconnect handling" "signalingClientConnectSync" 60

# ==================================================================
# GATE 3: signaling create/fetch/connect/send/query blocking bounds
# ==================================================================
hdr "signaling sync APIs" "createSignalingClientSync|signalingClientFetchSync|signalingClientConnectSync|signalingClientSendMessageSync|signalingClientGetIceConfigInfo"
src_def "createSignalingClientSync" "createSignalingClientSync" 50
src_def "signalingClientFetchSync" "signalingClientFetchSync" 40
src_def "signalingClientSendMessageSync" "signalingClientSendMessageSync" 40
hdr "signaling timeout constants" "SIGNALING_.*TIMEOUT|_CONNECT_TIMEOUT|_API_CALL_TIMEOUT|SERVICE_CALL_.*TIMEOUT"

# ==================================================================
# GATE 4: freeSignalingClient + callback quiescence
# ==================================================================
src_def "freeSignalingClient" "freeSignalingClient" 60
hdr "signaling state/message callback typedefs" "SignalingClientStateChangedFunc|SignalingClientMessageReceivedFunc|stateChangeFn|messageReceivedFn"

# ==================================================================
# GATE 5: initKvsWebRtc / deinitKvsWebRtc global semantics
# ==================================================================
src_def "initKvsWebRtc" "initKvsWebRtc" 40
src_def "deinitKvsWebRtc" "deinitKvsWebRtc" 40

# ==================================================================
# GATE 6: peer create / description sync-callback behaviour
# ==================================================================
hdr "peer connection APIs" "createPeerConnection|setRemoteDescription|setLocalDescription|createAnswer|peerConnectionOnConnectionStateChange|peerConnectionOnIceCandidate"
src_def "createPeerConnection" "createPeerConnection" 40
src_def "setLocalDescription" "setLocalDescription" 50

# ==================================================================
# GATE 7: close/free peer connection blocking + quiescence
# ==================================================================
src_def "closePeerConnection" "closePeerConnection" 50
src_def "freePeerConnection" "freePeerConnection" 80

# ==================================================================
# GATE 8: writeFrame blocking / frameData ownership / concurrency
# ==================================================================
hdr "writeFrame declaration" "writeFrame"
src_def "writeFrame" "writeFrame" 70

echo
echo "=================================================================="
echo "probe complete"
echo "=================================================================="
