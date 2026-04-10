#!/usr/bin/env bash
set -euo pipefail

# INT8 dynamic quantization script for YOLO ONNX models.
# Requires: project Python venv with onnxruntime and onnx packages.

VENV_DIR=".venv-raspi-eye"
if [ ! -f "${VENV_DIR}/bin/activate" ]; then
    echo "ERROR: Python venv not found at ${VENV_DIR}"
    echo "Run: python3 -m venv ${VENV_DIR} && source ${VENV_DIR}/bin/activate && pip install onnxruntime onnx"
    exit 1
fi
source "${VENV_DIR}/bin/activate"

MODEL_DIR="device/models"

quantize_model() {
    local input="$1"
    local output="$2"

    if [ ! -f "${input}" ]; then
        echo "SKIP: Input model not found: ${input}"
        return 0
    fi
    if [ -f "${output}" ]; then
        echo "SKIP: Output already exists: ${output}"
        return 0
    fi

    echo "Quantizing: ${input} -> ${output}"
    python3 -c "
from onnxruntime.quantization import quantize_dynamic, QuantType
quantize_dynamic('${input}', '${output}', weight_type=QuantType.QInt8)
print('Done: ${output}')
"
}

quantize_model "${MODEL_DIR}/yolo11s.onnx" "${MODEL_DIR}/yolo11s-int8.onnx"
quantize_model "${MODEL_DIR}/yolo11n.onnx" "${MODEL_DIR}/yolo11n-int8.onnx"

echo "Quantization complete."
