# Pi 5 Build Environment Setup

This guide walks you through setting up a Raspberry Pi 5 as a native build
target for the raspi-eye project.

## Prerequisites

- Raspberry Pi 5 (4 GB+ RAM) running Debian Bookworm (64-bit)
- Network connectivity (for apt install and git clone)
- SSH access from your development machine (macOS)

## 1. Install Build Dependencies

Install all required packages in one command:

```bash
sudo apt update && sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libglib2.0-dev \
    git
```

> **Note:** CMake's `FetchContent` will automatically download GTest, spdlog,
> and RapidCheck on the first build. Make sure the Pi has internet access.

## 2. Install ONNX Runtime (for YOLO detection)

Download and install the ONNX Runtime aarch64 prebuilt library:

```bash
ORT_VERSION="1.24.4"
cd /tmp
curl -L -o "onnxruntime-linux-aarch64-${ORT_VERSION}.tgz" \
    "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-aarch64-${ORT_VERSION}.tgz"
tar xzf "onnxruntime-linux-aarch64-${ORT_VERSION}.tgz"
sudo cp -r "onnxruntime-linux-aarch64-${ORT_VERSION}/include/"* /usr/local/include/
sudo cp -r "onnxruntime-linux-aarch64-${ORT_VERSION}/lib/"* /usr/local/lib/
sudo ldconfig
```

Verify installation:

```bash
ls /usr/local/include/onnxruntime_c_api.h
ls /usr/local/lib/libonnxruntime.so
```

> **Note:** Without ONNX Runtime, CMake will automatically disable the YOLO
> module (`ENABLE_YOLO=OFF`). Other modules are not affected.

## 3. Clone Repository

```bash
git clone https://github.com/JerryCW/raspi-eye.git ~/raspi-eye
cd ~/raspi-eye
```

Replace `https://github.com/JerryCW/raspi-eye.git` with your fork URL if applicable.

## 4. Download YOLO Models

```bash
bash scripts/download-model.sh
```

This downloads `yolo11s.onnx` (small) and `yolo11n.onnx` (nano) to
`device/models/`. If the `yolo` CLI (ultralytics) is installed, it exports from
PyTorch; otherwise it downloads pre-exported ONNX files from GitHub.

## 5. First Build Verification

Run the full configure + build + test cycle to confirm the environment is set up
correctly:

```bash
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && \
    cmake --build device/build && \
    ctest --test-dir device/build --output-on-failure
```

All tests should pass. The first build takes longer because FetchContent
downloads dependencies; subsequent builds are incremental.

## 6. SSH Key Setup

Passwordless SSH login is required by `scripts/pi-build.sh` (remote mode) and
`scripts/build-all.sh`. Run the following commands **on your macOS development
machine**.

### Generate SSH key (if you don't have one)

```bash
ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519
```

### Add Pi's host key and copy your public key

```bash
ssh-keyscan <PI_IP> >> ~/.ssh/known_hosts
ssh-copy-id pi@<PI_IP>
```

You will be prompted for the Pi's password once.

### Verify passwordless login

```bash
ssh pi@<PI_IP> "echo OK"
```

You should see `OK` printed without being prompted for a password.

## 7. Using pi-build.sh

`scripts/pi-build.sh` auto-detects the platform:
- On Pi 5 (Linux): runs build locally
- On macOS: builds remotely on Pi 5 via SSH

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `PI_HOST` | `raspberrypi.local` | Pi 5 hostname or IP |
| `PI_USER` | `pi` | Pi 5 username |
| `PI_REPO_DIR` | `~/raspi-eye` | Repo path on Pi 5 |

### Example (remote from macOS)

```bash
PI_HOST=192.168.x.x PI_REPO_DIR='~/Workspace/raspi-eye' ./scripts/pi-build.sh
```

> **Important:** Use single quotes for `PI_REPO_DIR` to prevent `~` from
> expanding on macOS. It must expand on the Pi side.
