#!/usr/bin/env bash
set -euo pipefail

MODEL_DIR="device/models"
mkdir -p "${MODEL_DIR}"

download_model() {
    local name="$1"
    local file="${MODEL_DIR}/${name}.onnx"

    if [ -f "${file}" ]; then
        echo "[download-model] Already exists: ${file}"
        return 0
    fi

    # Method 1: Export via ultralytics CLI (preferred)
    if command -v yolo &>/dev/null; then
        echo "[download-model] Exporting ${name} via ultralytics CLI..."
        yolo export model=${name}.pt format=onnx imgsz=640
        mv ${name}.onnx "${file}"
        echo "[download-model] Exported: ${file}"
        return 0
    fi

    # Method 2: Download from GitHub Releases (fallback)
    echo "[download-model] Downloading ${name}.onnx..."
    curl -L -o "${file}" \
        "https://github.com/ultralytics/assets/releases/download/v8.3.0/${name}.onnx"

    if [ ! -s "${file}" ]; then
        echo "[download-model] ERROR: Download failed or file is empty"
        rm -f "${file}"
        return 1
    fi

    echo "[download-model] Downloaded: ${file}"
}

download_model "yolo11s"
download_model "yolo11n"

echo "[download-model] Done."
