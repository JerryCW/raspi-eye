---
inclusion: always
---

# 技术栈

## 语言与标准

- C++17（主应用）
- Python ≥ 3.11（AI 模型推理脚本 / 训练工具 / 辅助脚本）
- Python 环境管理：venv（项目根目录 `.venv-raspi-eye/`）
- 所有 Python 命令必须先 `source .venv-raspi-eye/bin/activate` 再执行

## 编译器

- GCC aarch64-linux-gnu（Pi 5 交叉编译）
- Apple Clang（macOS 开发）

## 构建系统

- CMake（最低 3.16）
- 默认生成 `compile_commands.json`（`CMAKE_EXPORT_COMPILE_COMMANDS ON`）
- 平台检测：macOS x86_64 / macOS aarch64 / Linux aarch64
- 包管理：FetchContent（GTest、spdlog 等），GStreamer 走系统包（pkg-config）
- 未来计划：vcpkg（vcpkg.json manifest）管理 AWS SDK 等复杂依赖

## 核心依赖

| 库 | 用途 |
|----|------|
| GStreamer | 视频采集、编码、tee 分流 |
| AWS KVS Producer SDK | 视频上传到 KVS（kvssink） |
| AWS KVS WebRTC C SDK | 实时信令和媒体（Linux only，macOS stub） |
| libcurl | HTTPS 请求 |
| OpenSSL | mTLS、证书处理 |
| libcamera | CSI 摄像头（Linux aarch64 only） |
| V4L2 | USB 摄像头（Linux only） |
| systemd | 看门狗、服务集成（Linux only） |
| ONNX Runtime + YOLOv11s | 设备端目标检测 |
| SageMaker Serverless | 云端种类识别（DINOv2 等，支持模型切换） |

## 测试框架

- C++：Google Test + CTest 统一管理
- Python：pytest

## 硬件约束

- Raspberry Pi 5（4GB RAM）
- 摄像头：IMX216（CSI，当前）→ IMX678（USB，计划迁移）

## 常用命令

```bash
# Python 环境初始化（首次）
python3 -m venv .venv-raspi-eye
source .venv-raspi-eye/bin/activate
pip install -r requirements.txt

# Python 测试（必须在 venv 中）
source .venv-raspi-eye/bin/activate && pytest

# 构建（开发 - macOS）
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug
cmake --build device/build

# 运行 C++ 测试
ctest --test-dir device/build --output-on-failure

# 一条命令验证（配置 + 编译 + 测试）
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# 交叉编译（Pi 5，Spec 16 之后可用）
cmake -B device/build-pi -S device -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
cmake --build device/build-pi
```

## 开发环境差异

| 环境 | 视频源 | WebRTC | AI 推理 |
|------|--------|--------|---------|
| macOS（开发） | videotestsrc | stub | 可选（本地 ONNX） |
| Pi 5（生产） | IMX216/IMX678 | 完整实现 | YOLO + 云端识别 |

## 各模块技术栈

| 模块 | 语言 | 构建/包管理 | 测试 |
|------|------|-----------|------|
| device | C++17 | CMake + vcpkg | GTest + CTest |
| viewer | 待定 | 待定 | 待定 |
| model | Python ≥ 3.11 | pip + venv | pytest |
| infra | 待定（CDK/CFN/Terraform） | 待定 | 待定 |

## Git 规范

- 分支策略：`main` 保持可用，每个 Spec 一个 feature 分支
- 分支命名：`spec-{N}-{feature-name}`（与 Spec 目录名一致）
- Commit 时机：Spec 全部 tasks 完成并测试通过后，统一提交
- Commit message：`spec-{N}: 简要描述本 Spec 完成的功能`
- 提交后合回 main
- **自动 Commit 规则：** 当 Spec 的所有任务（包括检查点）执行完毕后，agent 必须主动执行 `git add` + `git commit`，不等用户提醒。提交前用 `git status` 确认无敏感文件。

## Spec 创建规则

- **Backlog 检查：** 创建新 Spec 前，必须读取 `docs/spec-backlog.md`，该文件包含两部分内容：
  1. **路线图规划：** 按阶段排列的完整 Spec 列表及依赖关系，作为参考而非硬性约束，实际 Spec 内容可根据需要调整
  2. **延迟待办项：** 开发过程中推迟到后续 Spec 的事项，检查是否有到期的待办应纳入当前 Spec
- 匹配的延迟待办项纳入后，在 backlog 的延迟待办项列表中标注对应 Spec 编号
- **反向同步：** 如果实际创建的 Spec 与 backlog 中的规划有差异（范围变化、依赖调整、拆分合并），必须反向更新 `docs/spec-backlog.md` 保持一致
