# 需求文档：Spec 2 — Pi 5 原生编译 + 双平台验证

## 简介

本 Spec 为 device 模块搭建 Pi 5 原生编译流程和双平台验证基础设施。方向从交叉编译改为 Pi 5 原生编译：开发者在 macOS 上编写代码，通过 git push → SSH 到 Pi 5 → git pull → cmake + build + ctest 完成部署和验证。

项目当前代码量小（< 1000 行），Pi 5 的 4 核 A76 + 4GB RAM 编译速度完全可接受，原生编译维护成本远低于交叉编译（无需 sysroot、toolchain 文件、交叉编译器）。后续项目规模增大（AWS SDK、ONNX Runtime）导致编译时间超过 5 分钟时，再考虑交叉编译。

从 Spec 3 开始，每个 Spec 都将进行双平台构建验证（macOS 本地 Debug + Pi 5 远程 Release），本 Spec 是这一基础设施的建立点。

## 前置条件

- Spec 0 (gstreamer-capture) 已通过验证 ✅
- Spec 1 (spdlog-logging) 已通过验证 ✅

## 术语表

- **Pi_Build_Script**：构建脚本（`scripts/pi-build.sh`），自动检测平台：macOS 上通过 SSH 连接 Pi 5 执行远程构建，Pi 5（Linux）上直接本地执行 git pull + cmake 配置 + 编译 + ctest
- **Build_All_Script**：双平台验证脚本（`scripts/build-all.sh`），依次执行 macOS 本地 Debug 编译测试和 SSH 到 Pi 5 远程 Release 编译测试
- **Pi_Setup_Doc**：Pi 5 环境配置文档（`docs/pi-setup.md`），记录 Pi 5 上需要安装的 apt 依赖和构建工具
- **FetchContent**：CMake 模块，用于在配置阶段自动下载和集成外部依赖（GTest、spdlog、RapidCheck）
- **pkg-config**：系统库依赖发现工具，Pi 5 上通过 apt 安装的 GStreamer 通过 pkg-config 发现
- **ASan**：AddressSanitizer，macOS Debug 构建开启的内存错误检测工具；Pi 5 Release 构建不开启 ASan（运行时开销过大）
- **SSH**：安全远程连接协议，用于从 macOS 连接 Pi 5 执行构建命令

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 开发平台 | macOS（Apple Clang，Debug 构建，开启 ASan） |
| 部署平台 | Raspberry Pi 5（Linux aarch64，Debian Bookworm，GCC，Release 构建） |
| 硬件限制 | Raspberry Pi 5，4 核 A76，4GB RAM |
| Pi 5 GStreamer | 通过 `apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev` 安装 |
| FetchContent 依赖 | GTest v1.14.0、spdlog v1.15.0、RapidCheck（Pi 5 上原生编译，首次构建需联网下载） |
| Pi 5 构建类型 | Release（不开启 ASan） |
| macOS 构建类型 | Debug（开启 ASan） |
| 脚本语言 | Bash，`set -euo pipefail` |
| 部署方式 | git push → SSH → git pull → 原生编译（非 rsync 二进制传输） |
| 环境变量 — PI_HOST | Pi 5 主机名或 IP（默认 `raspberrypi.local`） |
| 环境变量 — PI_USER | Pi 5 用户名（默认 `pi`） |
| 环境变量 — PI_REPO_DIR | Pi 5 上仓库路径（默认 `~/raspi-eye`） |
| 脚本文件路径 | `scripts/pi-build.sh`、`scripts/build-all.sh`（相对于项目根目录） |
| 文档文件路径 | `docs/pi-setup.md`（相对于项目根目录） |

## 禁止项

### Design 层

- SHALL NOT 引入交叉编译工具链、sysroot、toolchain 文件
  - 原因：当前项目规模小，Pi 5 原生编译完全够用，交叉编译维护成本高
  - 建议：后续编译时间超过 5 分钟时再考虑交叉编译

- SHALL NOT 修改现有 `device/CMakeLists.txt` 中已验证通过的 target 定义和依赖关系的核心逻辑
  - 原因：Spec 0 和 Spec 1 的功能已验证通过，平台适配应通过条件编译最小化修改
  - 建议：仅在必要时添加平台条件编译（如 `gst_macos_main` 已有的 `__APPLE__` 条件）

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret（来源：安全基线）
  - 原因：泄露风险，一旦提交到 git 无法撤回

- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）
  - 原因：日志可能被收集到云端或共享给他人

### Tasks 层

- SHALL NOT 直接运行测试可执行文件（如 `./build/smoke_test`），必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行（来源：spec-0, spec-1 共 3 次违规）
  - 原因：直接运行单个测试可执行文件容易遗漏其他测试、运行方式不统一
  - 建议：macOS 用 `ctest --test-dir device/build`，Pi 5 用 `ctest --test-dir device/build`（Pi 5 上构建目录相同）

- SHALL NOT 在脚本中硬编码 Pi 5 的 IP 地址、用户名或仓库路径
  - 原因：不同用户的 Pi 5 网络配置不同，硬编码导致不可移植
  - 建议：通过环境变量 `PI_HOST`、`PI_USER`、`PI_REPO_DIR` 传入，提供合理默认值

- SHALL NOT 将新建文件直接放到项目根目录（来源：历史经验）
  - 原因：根目录应保持干净
  - 建议：脚本放 `scripts/`，文档放 `docs/`

- SHALL NOT 在子代理（subagent）的最终检查点任务中执行 git commit（来源：历史经验）
  - 原因：commit 后编排层还需更新 tasks.md 状态和写入 trace 记录，导致变更遗漏

- SHALL NOT 将 .pem、.key、.env、证书文件提交到 git（来源：安全基线）
  - 原因：密钥泄露不可逆

- SHALL NOT 在脚本中使用 rsync 传输编译产物到 Pi 5
  - 原因：本 Spec 采用 git pull 原生编译方式，不传输二进制文件
  - 建议：Pi 5 上通过 git pull 获取源码后原生编译

## 需求

### 需求 1：Pi 5 环境配置文档

**用户故事：** 作为开发者，我需要一份 Pi 5 环境配置文档，以便新开发者能够快速在 Pi 5 上搭建构建环境。

#### 验收标准

1. THE Pi_Setup_Doc SHALL 以 `docs/pi-setup.md` 形式记录 Pi 5 上的完整环境配置步骤，使用英文编写
2. THE Pi_Setup_Doc SHALL 列出所有需要通过 apt 安装的依赖包，至少包含：`build-essential`、`cmake`、`pkg-config`、`libgstreamer1.0-dev`、`libgstreamer-plugins-base1.0-dev`、`libglib2.0-dev`、`git`
3. THE Pi_Setup_Doc SHALL 包含一条 apt install 命令安装所有依赖，可直接复制执行
4. THE Pi_Setup_Doc SHALL 包含 git 仓库首次 clone 的命令示例
5. THE Pi_Setup_Doc SHALL 包含首次构建验证命令（cmake 配置 + 编译 + ctest），确认环境配置正确
6. THE Pi_Setup_Doc SHALL 说明 SSH 免密登录配置步骤（`ssh-copy-id`），以便脚本无需手动输入密码

### 需求 2：SSH 远程构建脚本

**用户故事：** 作为开发者，我需要一个构建脚本，以便在 macOS 上通过 SSH 远程触发 Pi 5 构建，或在 Pi 5 上直接本地一键编译测试。

#### 验收标准

1. WHEN 在 macOS 上执行 `scripts/pi-build.sh` 时，THE Pi_Build_Script SHALL 通过 SSH 连接到 Pi 5（使用 `PI_USER@PI_HOST`），在 `PI_REPO_DIR` 目录下依次执行：git pull、cmake 配置（Release 构建）、cmake 编译、ctest 运行；WHEN 在 Pi 5（Linux）上执行时，THE Pi_Build_Script SHALL 直接在本地执行相同的构建流程（无需 SSH）
2. THE Pi_Build_Script SHALL 通过环境变量 `PI_HOST`（默认 `raspberrypi.local`）、`PI_USER`（默认 `pi`）、`PI_REPO_DIR`（默认 `~/raspi-eye`）确定连接目标和仓库路径
3. WHEN Pi 5 上 cmake 配置执行时，THE Pi_Build_Script SHALL 使用 `-DCMAKE_BUILD_TYPE=Release` 构建类型（不开启 ASan）
4. WHEN Pi 5 上 ctest 执行时，THE Pi_Build_Script SHALL 使用 `ctest --test-dir device/build --output-on-failure` 运行所有测试
5. THE Pi_Build_Script SHALL 将 Pi 5 上的构建和测试输出实时回显到本地终端
6. IF SSH 连接失败（Pi 5 不可达），THEN THE Pi_Build_Script SHALL 输出英文错误信息并以非零退出码退出
7. IF git pull、cmake 配置、编译或 ctest 中任一步骤失败，THEN THE Pi_Build_Script SHALL 立即停止后续步骤，输出英文错误信息并以非零退出码退出
8. THE Pi_Build_Script SHALL 在脚本开头使用 `set -euo pipefail` 确保任何命令失败时立即退出
9. THE Pi_Build_Script SHALL 在构建开始和结束时输出英文状态信息（如 `[pi-build] Starting build on PI_HOST...`、`[pi-build] All steps passed.`）

### 需求 3：双平台验证脚本

**用户故事：** 作为开发者，我需要一个一键脚本同时验证 macOS 本地编译和 Pi 5 远程编译都通过，以便从 Spec 3 开始每个 Spec 都能快速进行双平台验证。

#### 验收标准

1. WHEN 执行 `scripts/build-all.sh` 时，THE Build_All_Script SHALL 依次执行以下两步：（a）macOS 本地 Debug 编译 + ctest，（b）调用 `scripts/pi-build.sh` 执行 Pi 5 远程 Release 编译 + ctest
2. WHEN macOS 本地编译和测试全部通过后，THE Build_All_Script SHALL 继续执行 Pi 5 远程编译步骤
3. IF macOS 本地编译或测试失败，THEN THE Build_All_Script SHALL 立即停止并输出英文错误信息，不继续执行 Pi 5 远程编译步骤
4. IF Pi 5 远程编译或测试失败，THEN THE Build_All_Script SHALL 输出英文错误信息并以非零退出码退出
5. WHEN 两个平台都编译和测试成功时，THE Build_All_Script SHALL 输出英文摘要信息，包含两个平台的编译和测试状态
6. IF `PI_HOST` 环境变量未设置且默认主机不可达，THEN THE Build_All_Script SHALL 跳过 Pi 5 远程编译步骤并输出英文警告信息（允许在没有 Pi 5 的环境中仅运行 macOS 编译）
7. THE Build_All_Script SHALL 在脚本开头使用 `set -euo pipefail` 确保任何命令失败时立即退出
8. THE Build_All_Script SHALL 在每个阶段开始时输出英文阶段标识（如 `[build-all] Phase 1: macOS Debug build + test`、`[build-all] Phase 2: Pi 5 Release build + test`）

### 需求 4：CMakeLists.txt 平台适配

**用户故事：** 作为开发者，我需要确认现有 CMakeLists.txt 在 Pi 5（Linux aarch64，GCC）上能够正确编译，处理 macOS/Linux 平台差异。

#### 验收标准

1. WHEN 在 Pi 5（Linux aarch64）上执行 cmake 配置时，THE CMake_Build_System SHALL 通过 pkg-config 成功发现系统安装的 GStreamer
2. WHEN 在 Pi 5 上执行编译时，THE CMake_Build_System SHALL 成功编译所有 target（pipeline_manager 静态库、log_module 静态库、raspi-eye 可执行文件、smoke_test 测试、log_test 测试），无编译错误和链接错误
3. WHEN 在 Pi 5 上执行 ctest 时，THE CMake_Build_System SHALL 成功运行所有测试（smoke_test + log_test），全部通过
4. WHEN 在 Pi 5 上以 Release 模式构建时，THE CMake_Build_System SHALL 不开启 ASan（现有 CMakeLists.txt 中 ASan 仅在 Debug 模式开启，Release 模式自动跳过）
5. IF 现有代码中存在 macOS 特有的条件编译（如 `gst_macos_main`），THEN THE CMake_Build_System SHALL 在 Linux 上正确跳过这些代码段，编译无错误

### 需求 5：macOS 本地编译回归验证

**用户故事：** 作为开发者，我需要确认引入新脚本和可能的 CMakeLists.txt 修改后，macOS 本地编译和所有已有测试仍然正常通过。

#### 验收标准

1. WHEN 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` 时，THE CMake_Build_System SHALL 成功完成配置、编译和测试，所有 Spec 0 和 Spec 1 的测试通过
2. WHEN macOS Debug 构建运行测试时，THE Test_Suite SHALL 确保 ASan 不报告任何内存错误

## 验证命令

```bash
# macOS 本地编译回归验证
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# Pi 5 远程一键构建（SSH）
scripts/pi-build.sh

# 双平台一键验证
scripts/build-all.sh

# Pi 5 上手动验证（SSH 登录后）
cd ~/raspi-eye && git pull
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release
cmake --build device/build
ctest --test-dir device/build --output-on-failure
```

## 明确不包含

- 交叉编译工具链、sysroot、toolchain 文件（本 Spec 采用 Pi 5 原生编译）
- H.264 编码和 tee 分流（Spec 3）
- 摄像头抽象层（Spec 4）
- CI/CD 自动化流水线（后续按需添加）
- Docker 编译容器
- vcpkg 支持（当前无 vcpkg 依赖）
- Pi 5 上的 Debug 构建和 ASan（Pi 5 上仅 Release 构建）
- rsync 二进制传输部署（本 Spec 采用 git pull 源码同步 + 原生编译）
- Pi 5 上的自动化 git clone 脚本（首次 clone 手动执行一次，文档说明步骤）
