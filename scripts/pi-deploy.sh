#!/usr/bin/env bash
# pi-deploy.sh — Build, install, and deploy raspi-eye on Pi 5
# Remote via SSH from macOS, or locally on Pi 5
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# --- Environment variables (same defaults as pi-build.sh) ---
PI_HOST="${PI_HOST:-raspberrypi.local}"
PI_USER="${PI_USER:-pi}"
PI_REPO_DIR="${PI_REPO_DIR:-~/raspi-eye}"

# --- Command-line flags ---
SKIP_BUILD=false
SKIP_TEST=false
NO_PULL=false

# --- System path constants ---
INSTALL_BIN="/usr/local/bin/raspi-eye"
CONFIG_DIR="/etc/raspi-eye"
CERTS_DIR="/etc/raspi-eye/certs"
PLUGINS_DIR="/usr/local/lib/raspi-eye/plugins"
SERVICE_NAME="raspi-eye.service"

# --- Deploy summary tracking ---
SUMMARY_BINARY=""
SUMMARY_CONFIG=""
SUMMARY_CERTS=""
SUMMARY_PLUGINS=""
SUMMARY_SERVICE=""

# --- Logging ---
log() {
    echo "[pi-deploy] $*"
}

# --- Argument parsing ---
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --skip-build)
                SKIP_BUILD=true
                shift
                ;;
            --skip-test)
                SKIP_TEST=true
                shift
                ;;
            --no-pull)
                NO_PULL=true
                shift
                ;;
            *)
                log "ERROR: Unknown option: $1"
                log "Usage: pi-deploy.sh [--skip-build] [--skip-test] [--no-pull]"
                exit 1
                ;;
        esac
    done
}

# =============================================
# Build phase functions
# =============================================

do_pull() {
    if [[ "${SKIP_BUILD}" == "true" ]] || [[ "${NO_PULL}" == "true" ]]; then
        log "Skipping git pull"
        return
    fi
    log "git pull..."
    git pull
}

do_build() {
    if [[ "${SKIP_BUILD}" == "true" ]]; then
        if [[ ! -f "device/build/raspi-eye" ]]; then
            log "ERROR: --skip-build specified but device/build/raspi-eye not found"
            exit 1
        fi
        log "Skipping build (--skip-build), using existing binary"
        return
    fi
    log "cmake configure (Release)..."
    cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release
    log "cmake build..."
    cmake --build device/build
}

do_test() {
    if [[ "${SKIP_BUILD}" == "true" ]] || [[ "${SKIP_TEST}" == "true" ]]; then
        log "Skipping tests"
        return
    fi
    log "ctest..."
    if ! ctest --test-dir device/build --output-on-failure; then
        log "ERROR: Tests failed, aborting deploy"
        exit 1
    fi
}

# =============================================
# Deploy phase functions
# =============================================

install_binary() {
    log "Installing binary to ${INSTALL_BIN}..."
    sudo cp "device/build/raspi-eye" "${INSTALL_BIN}"
    sudo chmod 755 "${INSTALL_BIN}"
    SUMMARY_BINARY="installed"
    log "Binary installed: ${INSTALL_BIN}"
}

deploy_config() {
    log "Deploying config..."
    sudo mkdir -p "${CONFIG_DIR}"
    sudo chown pi:pi "${CONFIG_DIR}"

    if [[ -f "${CONFIG_DIR}/config.toml" ]]; then
        log "Config already exists at ${CONFIG_DIR}/config.toml, keeping existing (user modifications preserved)"
        SUMMARY_CONFIG="kept"
        return
    fi

    local src=""
    if [[ -f "device/config/config.toml" ]]; then
        src="device/config/config.toml"
    elif [[ -f "device/config/config.toml.example" ]]; then
        src="device/config/config.toml.example"
    else
        log "WARNING: No config source found (device/config/config.toml or config.toml.example), skipping config deploy"
        SUMMARY_CONFIG="skipped (no config source)"
        return
    fi

    sudo cp "${src}" "${CONFIG_DIR}/config.toml"
    # Replace dev cert paths with system paths (first deploy only)
    sudo sed -i 's|device/certs/|/etc/raspi-eye/certs/|g' "${CONFIG_DIR}/config.toml"
    sudo chmod 640 "${CONFIG_DIR}/config.toml"
    sudo chown pi:pi "${CONFIG_DIR}/config.toml"
    SUMMARY_CONFIG="new (from ${src})"
    log "Config deployed: ${CONFIG_DIR}/config.toml (from ${src})"
}

deploy_certs() {
    log "Deploying certificates..."
    if [[ ! -d "device/certs" ]]; then
        log "WARNING: device/certs/ directory not found, skipping cert deploy"
        SUMMARY_CERTS="skipped (no certs directory)"
        return
    fi

    local has_certs=false
    for f in device/certs/*.pem device/certs/*.key; do
        if [[ -f "${f}" ]]; then
            has_certs=true
            break
        fi
    done

    if [[ "${has_certs}" == "false" ]]; then
        log "WARNING: No .pem or .key files in device/certs/, skipping cert deploy"
        SUMMARY_CERTS="skipped (no cert files)"
        return
    fi

    sudo mkdir -p "${CERTS_DIR}"
    sudo chown pi:pi "${CERTS_DIR}"

    for f in device/certs/*.pem; do
        if [[ -f "${f}" ]]; then
            sudo cp "${f}" "${CERTS_DIR}/"
            sudo chmod 644 "${CERTS_DIR}/$(basename "${f}")"
            sudo chown pi:pi "${CERTS_DIR}/$(basename "${f}")"
        fi
    done

    for f in device/certs/*.key; do
        if [[ -f "${f}" ]]; then
            sudo cp "${f}" "${CERTS_DIR}/"
            sudo chmod 600 "${CERTS_DIR}/$(basename "${f}")"
            sudo chown pi:pi "${CERTS_DIR}/$(basename "${f}")"
        fi
    done

    SUMMARY_CERTS="deployed"
    log "Certificates deployed to ${CERTS_DIR}/"
}

install_plugins() {
    log "Installing plugins..."
    local has_so=false
    for f in device/plugins/*.so; do
        if [[ -f "${f}" ]]; then
            has_so=true
            break
        fi
    done

    if [[ "${has_so}" == "false" ]]; then
        log "WARNING: No .so files in device/plugins/, skipping plugin install"
        SUMMARY_PLUGINS="skipped (no .so files)"
        return
    fi

    sudo mkdir -p "${PLUGINS_DIR}"

    for f in device/plugins/*.so; do
        if [[ -f "${f}" ]]; then
            sudo cp "${f}" "${PLUGINS_DIR}/"
            sudo chmod 755 "${PLUGINS_DIR}/$(basename "${f}")"
        fi
    done

    SUMMARY_PLUGINS="installed"
    log "Plugins installed to ${PLUGINS_DIR}/"
}

restart_service() {
    log "Restarting ${SERVICE_NAME}..."

    if ! systemctl list-unit-files "${SERVICE_NAME}" &>/dev/null; then
        log "WARNING: ${SERVICE_NAME} not found. Create it first (see spec-20). Skipping service restart."
        SUMMARY_SERVICE="skipped (service not found)"
        return
    fi

    sudo systemctl daemon-reload
    sudo systemctl restart "${SERVICE_NAME}"
    sleep 3

    if systemctl is-active --quiet "${SERVICE_NAME}"; then
        log "Service ${SERVICE_NAME} is active"
        SUMMARY_SERVICE="restarted"
    else
        log "ERROR: Service ${SERVICE_NAME} failed to start"
        journalctl -u "${SERVICE_NAME}" -n 20 --no-pager
        exit 1
    fi
}

print_summary() {
    log "========================================="
    log "         Deploy Summary"
    log "========================================="
    log "Binary:   ${SUMMARY_BINARY:-not run}"
    log "Config:   ${SUMMARY_CONFIG:-not run}"
    log "Certs:    ${SUMMARY_CERTS:-not run}"
    log "Plugins:  ${SUMMARY_PLUGINS:-not run}"
    log "Service:  ${SUMMARY_SERVICE:-not run}"
    log "========================================="
    log "Deploy complete!"
}

# --- Main flow (called in local mode) ---
main() {
    parse_args "$@"
    cd "${PROJECT_ROOT}"

    do_pull
    do_build
    do_test
    install_binary
    deploy_config
    deploy_certs
    install_plugins
    restart_service
    print_summary
}

# --- Entry point ---
OS="$(uname -s)"

if [[ "${OS}" == "Darwin" ]]; then
    # ── Remote mode: SSH from macOS to Pi 5 ──
    log "Remote mode: deploying on ${PI_HOST} via SSH"

    # Parse args locally to build remote flags
    REMOTE_ARGS=""
    for arg in "$@"; do
        case "${arg}" in
            --skip-build|--skip-test|--no-pull)
                REMOTE_ARGS="${REMOTE_ARGS} ${arg}"
                ;;
        esac
    done

    ssh -o ConnectTimeout=5 -o BatchMode=yes "${PI_USER}@${PI_HOST}" true || {
        log "ERROR: Cannot connect to ${PI_USER}@${PI_HOST}"
        exit 1
    }

    # shellcheck disable=SC2029
    ssh "${PI_USER}@${PI_HOST}" "cd ${PI_REPO_DIR} && bash scripts/pi-deploy.sh${REMOTE_ARGS}"

    log "Remote deploy complete."
elif [[ "${OS}" == "Linux" ]]; then
    log "Local mode: deploying on Pi 5"
    main "$@"
fi
