# 实施计划：Spec 2 — Pi 5 原生编译 + 双平台验证

## 概述

为 device 模块搭建 Pi 5 原生编译流程和双平台验证基础设施。按依赖顺序：Pi 5 环境文档 → SSH 远程构建脚本 → 双平台验证脚本 → macOS 回归验证 + 脚本语法检查 → 最终检查点。本 Spec 交付物为 Bash 脚本和文档，不涉及 C++ 代码修改和 PBT。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在脚本中硬编码 Pi 5 的 IP 地址、用户名或仓库路径，通过环境变量传入并提供默认值
- SHALL NOT 将新建文件直接放到项目根目录，脚本放 `scripts/`，文档放 `docs/`
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 在脚本中使用 rsync 传输编译产物到 Pi 5
- SHALL NOT 引入交叉编译工具链、sysroot、toolchain 文件
- SHALL NOT 修改现有 `device/CMakeLists.txt` 中已验证通过的 target 定义和依赖关系的核心逻辑
- SHALL NOT 在脚本输出中使用非 ASCII 字符，所有脚本输出使用英文

## 任务

- [x] 1. 创建 Pi 5 环境配置文档
  - [x] 1.1 创建 `docs/pi-setup.md`
    - 英文编写，包含以下章节：Prerequisites、Install Build Dependencies、Clone Repository、First Build Verification、SSH Key Setup
    - 列出所有 apt 依赖包：`build-essential`、`cmake`、`pkg-config`、`libgstreamer1.0-dev`、`libgstreamer-plugins-base1.0-dev`、`libglib2.0-dev`、`git`
    - 提供一条可直接复制执行的 `sudo apt update && sudo apt install -y ...` 命令
    - 包含 `git clone <repo-url> ~/raspi-eye` 命令示例
    - 包含首次构建验证命令：`cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
    - 包含 SSH 免密登录配置步骤（`ssh-copy-id pi@raspberrypi.local` + 验证命令）
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5, 1.6_

- [x] 2. 创建构建脚本（双模式：本地 + SSH 远程）
  - [x] 2.1 创建 `scripts/pi-build.sh`
    - 脚本开头 `#!/usr/bin/env bash` + `set -euo pipefail`
    - 通过 `uname -s` 检测平台：Linux → 本地模式，Darwin → SSH 远程模式
    - **本地模式（Pi 5）**：`cd` 到项目根目录，依次执行 git pull、cmake Release、build、ctest
    - **远程模式（macOS）**：读取环境变量 `PI_HOST`（默认 `raspberrypi.local`）、`PI_USER`（默认 `pi`）、`PI_REPO_DIR`（默认 `~/raspi-eye`），SSH 连接检测后通过 heredoc 远程执行相同构建流程

- [x] 3. 创建双平台验证脚本
  - [x] 3.1 创建 `scripts/build-all.sh`
    - 脚本开头 `#!/usr/bin/env bash` + `set -euo pipefail`
    - 通过 `${BASH_SOURCE[0]}` 推导 `SCRIPT_DIR` 和 `PROJECT_ROOT`，`cd "${PROJECT_ROOT}"`
    - 读取 `PI_HOST`、`PI_USER` 环境变量（带默认值）
    - 开头检测 Pi 5 可达性（SSH 连接测试），缓存到 `PI_REACHABLE` 变量
    - Phase 1：macOS 本地 Debug 编译 + 测试
      - 输出 `[build-all] Phase 1: macOS Debug build + test`
      - `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug`
      - `cmake --build device/build`
      - `ctest --test-dir device/build --output-on-failure`
      - 通过后输出 `[build-all] Phase 1 passed: macOS Debug ✅`
      - 失败立即退出，不执行 Phase 2
    - Phase 2：Pi 5 远程 Release 编译 + 测试
      - 输出 `[build-all] Phase 2: Pi 5 Release build + test`
      - Pi 5 可达时调用 `"${SCRIPT_DIR}/pi-build.sh"`，通过后输出 `[build-all] Phase 2 passed: Pi 5 Release ✅`
      - Pi 5 不可达时输出 `[build-all] WARNING: Pi 5 (${PI_HOST}) not reachable, skipping Phase 2` 到 stderr，不报错退出
    - 摘要输出：显示两个平台的 PASSED / SKIPPED 状态
    - 设置 `chmod +x`
    - _需求：3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8_

- [x] 4. macOS 回归验证 + 脚本语法检查
  - [x] 4.1 脚本语法检查
    - 执行 `bash -n scripts/pi-build.sh` 确认语法正确
    - 执行 `bash -n scripts/build-all.sh` 确认语法正确
    - _需求：2.8, 3.7_
  - [x] 4.2 macOS 本地编译回归验证
    - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
    - 确认 Spec 0（smoke_test）和 Spec 1（log_test）所有测试通过
    - 确认 ASan 无内存错误报告
    - _需求：5.1, 5.2_
  - [x] 4.3 验证 CMakeLists.txt 平台适配（代码审查）
    - 审查 `device/CMakeLists.txt`：确认 pkg-config GStreamer 发现、FetchContent 依赖、ASan 仅 Debug 开启、条件编译隔离（`#ifdef __APPLE__`）在 Pi 5 上无兼容性问题
    - 确认预期无需修改 CMakeLists.txt（如发现问题则最小化修改）
    - _需求：4.1, 4.2, 4.3, 4.4, 4.5_

- [x] 5. 最终检查点 — 全量验证
  - 确认以下全部通过：
    - `bash -n scripts/pi-build.sh` — 语法正确
    - `bash -n scripts/build-all.sh` — 语法正确
    - `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` — macOS 编译测试通过、ASan 无报告
    - `docs/pi-setup.md` 存在且内容完整
  - Pi 5 远程验证（`scripts/pi-build.sh`）需要 Pi 5 可达，如不可达则标注跳过
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户

## 备注

- 本 Spec 不涉及 PBT（交付物为 Bash 脚本和文档，无可测试的纯函数）
- 脚本语法检查用 `bash -n`，macOS 回归验证用 `cmake + build + ctest`
- Pi 5 远程验证需要 Pi 5 可达（通过 SSH），不可达时相关验证标注跳过
- 所有脚本输出使用英文
- CMakeLists.txt 预期无修改，如实际构建遇问题按最小化修改原则处理
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
