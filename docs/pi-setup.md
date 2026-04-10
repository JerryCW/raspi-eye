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

## 2.5 Build ONNX Runtime with XNNPACK EP (Optional)

The prebuilt ONNX Runtime package does NOT include XNNPACK EP support. To enable
XNNPACK acceleration on Pi 5 (Cortex-A76 ARM NEON), you must build ONNX Runtime
from source with `--use_xnnpack`. This replaces the prebuilt library from step 2.

### Prerequisites

```bash
sudo apt install -y \
    cmake \
    python3-dev \
    python3-numpy \
    python3-pip
```

CMake >= 3.26 is required. Check your version:

```bash
cmake --version
```

If the system cmake is too old, install a newer version:

```bash
pip3 install cmake --break-system-packages
# Or download from https://cmake.org/download/
```

### Increase swap (recommended for 4GB Pi 5)

The build is memory-intensive. Increase swap to avoid OOM:

```bash
sudo dphys-swapfile swapoff
sudo sed -i 's/CONF_SWAPSIZE=.*/CONF_SWAPSIZE=2048/' /etc/dphys-swapfile
sudo dphys-swapfile setup
sudo dphys-swapfile swapon
free -h  # Verify swap is ~2GB
```

### Clone and build

```bash
cd /tmp
git clone --recursive --branch v1.24.4 --depth 1 \
    https://github.com/microsoft/onnxruntime.git
cd onnxruntime

./build.sh \
    --config Release \
    --build_shared_lib \
    --use_xnnpack \
    --parallel \
    --skip_tests \
    --cmake_extra_defines CMAKE_INSTALL_PREFIX=/usr/local
```

> **Build time:** Expect 1-3 hours on Pi 5 depending on swap and thermal
> throttling. If you hit OOM, try `--parallel 1` instead of `--parallel`.

### Install

```bash
cd build/Linux/Release
sudo make install
sudo ldconfig
```

This installs headers to `/usr/local/include/` and libraries to
`/usr/local/lib/`, replacing the prebuilt version from step 2.

### Verify XNNPACK EP availability

Create a quick test program:

```bash
cat > /tmp/test_xnnpack.cpp << 'EOF'
#include <onnxruntime_c_api.h>
#include <cstdio>
int main() {
    const OrtApi* ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    OrtSessionOptions* opts = nullptr;
    ort->CreateSessionOptions(&opts);
    OrtStatus* status = OrtSessionOptionsAppendExecutionProvider(
        opts, "XNNPACK", nullptr, nullptr, 0);
    if (status) {
        printf("XNNPACK EP NOT available: %s\n", ort->GetErrorMessage(status));
        ort->ReleaseStatus(status);
    } else {
        printf("XNNPACK EP available!\n");
    }
    ort->ReleaseSessionOptions(opts);
    return 0;
}
EOF
g++ -o /tmp/test_xnnpack /tmp/test_xnnpack.cpp -lonnxruntime -I/usr/local/include
/tmp/test_xnnpack
```

Expected output: `XNNPACK EP available!`

### Cleanup

```bash
rm -rf /tmp/onnxruntime  # Free ~10GB disk space
```

### Restore swap (optional)

```bash
sudo dphys-swapfile swapoff
sudo sed -i 's/CONF_SWAPSIZE=.*/CONF_SWAPSIZE=100/' /etc/dphys-swapfile
sudo dphys-swapfile setup
sudo dphys-swapfile swapon
```

## 3. Build KVS Producer SDK (for kvssink GStreamer plugin)

The KVS Producer SDK provides `libgstkvssink.so`, a GStreamer sink element that
uploads H.264 video to Amazon Kinesis Video Streams. Our code does NOT link
against this SDK at compile time — it loads the plugin at runtime via
`GST_PLUGIN_PATH`. Without it, the code gracefully falls back to fakesink.

Reference: [AWS docs — Download and build the KVS C++ producer SDK](https://docs.aws.amazon.com/kinesisvideostreams/latest/dg/producersdk-cpp-rpi-download.html)

### Install additional dependencies

```bash
sudo apt-get install -y \
    automake \
    gstreamer1.0-plugins-base-apps \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-tools \
    libcurl4-openssl-dev \
    liblog4cplus-dev \
    libssl-dev
```

### Clone and build

```bash
cd ~
git clone https://github.com/awslabs/amazon-kinesis-video-streams-producer-sdk-cpp.git \
    --single-branch -b master kvs-producer-sdk-cpp
mkdir -p kvs-producer-sdk-cpp/build
cd kvs-producer-sdk-cpp/build
cmake .. -DBUILD_GSTREAMER_PLUGIN=ON -DBUILD_DEPENDENCIES=OFF -DALIGNED_MEMORY_MODEL=ON
make -j$(nproc)
```

Build time: ~10-20 minutes on Pi 5.

### Verify kvssink plugin

```bash
export GST_PLUGIN_PATH=~/kvs-producer-sdk-cpp/build
gst-inspect-1.0 kvssink
```

You should see kvssink's property list (stream-name, aws-region,
iot-certificate, etc.). Press `q` to exit.

### Copy plugin to project plugins directory

The project uses a unified `device/plugins/` directory for all runtime-loaded
GStreamer plugins (kvssink, webrtc, etc.). `pi-build.sh` automatically sets
`GST_PLUGIN_PATH` to this directory.

```bash
cd ~/raspi-eye   # or your PI_REPO_DIR
mkdir -p device/plugins
cp ~/kvs-producer-sdk-cpp/build/libgstkvssink.so device/plugins/
```

Verify:

```bash
export GST_PLUGIN_PATH=$(pwd)/device/plugins
gst-inspect-1.0 kvssink
```

> Future plugins (e.g. WebRTC) should also be copied to `device/plugins/`.
> The directory is in `.gitignore` — plugins are platform-specific binaries
> and must be built on the target device.

### Persist GST_PLUGIN_PATH (optional)

If you run raspi-eye outside of `pi-build.sh`, set the path in your shell:

```bash
echo 'export GST_PLUGIN_PATH=~/raspi-eye/device/plugins' >> ~/.bashrc
source ~/.bashrc
```

> **Note:** `pi-build.sh` sets `GST_PLUGIN_PATH` automatically. You only need
> the `.bashrc` entry for manual runs or systemd service configuration.

### Quick end-to-end test (optional)

```bash
gst-launch-1.0 -v videotestsrc num-buffers=150 ! videoconvert ! \
    x264enc tune=zerolatency speed-preset=ultrafast ! h264parse ! \
    kvssink stream-name="TestStream" aws-region="ap-southeast-1" \
    iot-certificate="iot-certificate,endpoint=YOUR_ENDPOINT,cert-path=device/certs/device-cert.pem,key-path=device/certs/device-private.key,ca-path=device/certs/root-ca.pem,role-aliases=YOUR_ROLE_ALIAS"
```

Replace the placeholder values with your actual IoT credentials. Check the AWS
KVS console to verify the stream receives video.

> **Note:** If you skip this step, raspi-eye still compiles and all tests pass.
> At runtime, `create_kvs_sink()` will log a warning and fall back to fakesink.

## 4. Clone Repository

```bash
git clone https://github.com/JerryCW/raspi-eye.git ~/raspi-eye
cd ~/raspi-eye
```

Replace `https://github.com/JerryCW/raspi-eye.git` with your fork URL if applicable.

## 5. Download YOLO Models

```bash
bash scripts/download-model.sh
```

This downloads `yolo11s.onnx` (small) and `yolo11n.onnx` (nano) to
`device/models/`. If the `yolo` CLI (ultralytics) is installed, it exports from
PyTorch; otherwise it downloads pre-exported ONNX files from GitHub.

## 6. First Build Verification

Run the full configure + build + test cycle to confirm the environment is set up
correctly:

```bash
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && \
    cmake --build device/build && \
    ctest --test-dir device/build --output-on-failure
```

All tests should pass. The first build takes longer because FetchContent
downloads dependencies; subsequent builds are incremental.

## 7. SSH Key Setup

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

## 8. Using pi-build.sh

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
