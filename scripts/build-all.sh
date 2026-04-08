#!/usr/bin/env bash
# build-all.sh — Build and test on both macOS and Pi 5
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

PI_HOST="${PI_HOST:-raspberrypi.local}"
PI_USER="${PI_USER:-pi}"
PI_REACHABLE=false

# Check Pi 5 reachability once
if ssh -o ConnectTimeout=5 -o BatchMode=yes "${PI_USER}@${PI_HOST}" true 2>/dev/null; then
    PI_REACHABLE=true
fi

# ── Phase 1: macOS Debug build + test ──
echo "[build-all] Phase 1: macOS Debug build + test"
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug
cmake --build device/build
ctest --test-dir device/build --output-on-failure
echo "[build-all] Phase 1 passed: macOS Debug ✅"

# ── Phase 2: Pi 5 Release build + test ──
echo "[build-all] Phase 2: Pi 5 Release build + test"

if [ "${PI_REACHABLE}" = true ]; then
    "${SCRIPT_DIR}/pi-build.sh"
    echo "[build-all] Phase 2 passed: Pi 5 Release ✅"
else
    echo "[build-all] WARNING: Pi 5 (${PI_HOST}) not reachable, skipping Phase 2" >&2
fi

# ── Summary ──
echo "[build-all] =============================="
echo "[build-all] macOS Debug:   PASSED"
if [ "${PI_REACHABLE}" = true ]; then
    echo "[build-all] Pi 5 Release:  PASSED"
else
    echo "[build-all] Pi 5 Release:  SKIPPED (not reachable)"
fi
echo "[build-all] =============================="
