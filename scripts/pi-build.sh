#!/usr/bin/env bash
# pi-build.sh — Build and test on Pi 5 via SSH
set -euo pipefail

# 环境变量（带默认值）
PI_HOST="${PI_HOST:-raspberrypi.local}"
PI_USER="${PI_USER:-pi}"
PI_REPO_DIR="${PI_REPO_DIR:-~/raspi-eye}"

echo "[pi-build] Starting build on ${PI_HOST}..."

# SSH 连接检测
ssh -o ConnectTimeout=5 -o BatchMode=yes "${PI_USER}@${PI_HOST}" true || {
    echo "[pi-build] ERROR: Cannot connect to ${PI_USER}@${PI_HOST}" >&2
    exit 1
}

# 远程执行：git pull + cmake + build + ctest
ssh "${PI_USER}@${PI_HOST}" bash -s -- "${PI_REPO_DIR}" <<'REMOTE'
    set -euo pipefail
    REPO_DIR="$1"
    cd "${REPO_DIR}"

    echo "[pi-build] git pull..."
    git pull

    echo "[pi-build] cmake configure (Release)..."
    cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release

    echo "[pi-build] cmake build..."
    cmake --build device/build

    echo "[pi-build] ctest..."
    ctest --test-dir device/build --output-on-failure
REMOTE

echo "[pi-build] All steps passed."
