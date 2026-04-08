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

## 2. Clone Repository

```bash
git clone <repo-url> ~/raspi-eye
cd ~/raspi-eye
```

Replace `<repo-url>` with the actual repository URL.

## 3. First Build Verification

Run the full configure + build + test cycle to confirm the environment is set up
correctly:

```bash
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && \
    cmake --build device/build && \
    ctest --test-dir device/build --output-on-failure
```

All tests should pass. The first build takes longer because FetchContent
downloads dependencies; subsequent builds are incremental.

## 4. SSH Key Setup

Passwordless SSH login is required by `scripts/pi-build.sh` and
`scripts/build-all.sh`. Run the following commands **on your macOS development
machine**.

### Copy your public key to the Pi

```bash
ssh-copy-id pi@raspberrypi.local
```

If your Pi uses a different hostname or user, adjust accordingly:

```bash
ssh-copy-id <user>@<hostname>
```

### Verify passwordless login

```bash
ssh pi@raspberrypi.local "echo OK"
```

You should see `OK` printed without being prompted for a password.
