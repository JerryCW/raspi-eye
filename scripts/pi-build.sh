#!/usr/bin/env bash
# pi-build.sh — Build and test on Pi 5 (remote via SSH from macOS, or locally on Pi 5)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PI_HOST="${PI_HOST:-raspberrypi.local}"
PI_USER="${PI_USER:-pi}"
PI_REPO_DIR="${PI_REPO_DIR:-~/raspi-eye}"

OS="$(uname -s)"

PLUGINS_DIR="device/plugins"

if [ "${OS}" = "Linux" ]; then
    # ── Local mode: running on Pi 5 directly ──
    echo "[pi-build] Local mode: building on Pi 5"
    cd "${PROJECT_ROOT}"

    echo "[pi-build] git pull..."
    git pull

    # Set GST_PLUGIN_PATH if plugins directory exists and contains .so files
    if [ -d "${PLUGINS_DIR}" ] && ls "${PLUGINS_DIR}"/*.so &>/dev/null; then
        export GST_PLUGIN_PATH="${PROJECT_ROOT}/${PLUGINS_DIR}"
        echo "[pi-build] GST_PLUGIN_PATH=${GST_PLUGIN_PATH}"
    else
        echo "[pi-build] No plugins in ${PLUGINS_DIR}/, kvssink/webrtc will fall back to fakesink"
    fi

    echo "[pi-build] cmake configure (Release)..."
    cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release

    echo "[pi-build] cmake build..."
    cmake --build device/build

    echo "[pi-build] ctest..."
    ctest --test-dir device/build --output-on-failure

    # TODO: deploy step, e.g. sudo systemctl restart raspi-eye

    echo "[pi-build] All steps passed. (local)"
else
    # ── Remote mode: SSH from macOS to Pi 5 ──
    echo "[pi-build] Remote mode: building on ${PI_HOST} via SSH"

    ssh -o ConnectTimeout=5 -o BatchMode=yes "${PI_USER}@${PI_HOST}" true || {
        echo "[pi-build] ERROR: Cannot connect to ${PI_USER}@${PI_HOST}" >&2
        exit 1
    }

    ssh "${PI_USER}@${PI_HOST}" bash -s -- "${PI_REPO_DIR}" <<'REMOTE'
        set -euo pipefail
        REPO_DIR="$1"
        cd "${REPO_DIR}"

        echo "[pi-build] git pull..."
        git pull

        # Set GST_PLUGIN_PATH if plugins directory exists
        PLUGINS_DIR="device/plugins"
        if [ -d "${PLUGINS_DIR}" ] && ls "${PLUGINS_DIR}"/*.so &>/dev/null; then
            export GST_PLUGIN_PATH="$(pwd)/${PLUGINS_DIR}"
            echo "[pi-build] GST_PLUGIN_PATH=${GST_PLUGIN_PATH}"
        else
            echo "[pi-build] No plugins in ${PLUGINS_DIR}/, kvssink/webrtc will fall back to fakesink"
        fi

        echo "[pi-build] cmake configure (Release)..."
        cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release

        echo "[pi-build] cmake build..."
        cmake --build device/build

        echo "[pi-build] ctest..."
        ctest --test-dir device/build --output-on-failure
REMOTE

    echo "[pi-build] All steps passed. (remote)"
fi
