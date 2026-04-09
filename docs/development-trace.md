# Development Trace

基于 Meta-Harness 方法论的开发 trace 知识库。
用于禁止项反哺、失败归因追踪、Spec 迭代优化。

## 已提炼的 SHALL NOT（禁止项池）

_从反复出现的失败模式中提炼，直接复制到下一轮 Spec。_

- SHALL NOT 直接运行测试可执行文件（如 `./build/log_test`），必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行（来源：spec-0, spec-1 共 3 次违规）

---

## Trace 条目

### 2026-04-08 — Spec: spec-0-gstreamer-capture / 任务: 1.1 创建 device/CMakeLists.txt

**完成概要：** 创建 CMakeLists.txt，配置 C++17、ASan、pkg-config GStreamer、FetchContent GTest v1.14.0、三个 target（pipeline_manager 静态库、raspi-eye 可执行文件、smoke_test 测试）。

**测试状态：** 未运行（轻量模式，纯配置文件创建）— 无新增测试

**Trace 记录：**

| # | 症状 | 归因类别 | 完整 Trace | 解决方案 | 建议行动 |
|---|------|---------|-----------|---------|----------|
| 1 | macOS 上链接 GStreamer 失败，`${GST_LIBRARIES}` 只含库名不含路径 | Spec 缺少信息 | ld: library 'gstreamer-1.0' not found | 添加 `target_link_directories(pipeline_manager PUBLIC ${GST_LIBRARY_DIRS})` | 加参考代码到 design.md 的 CMakeLists.txt 示例中 |

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。（单次问题，观察后续是否反复出现）

**涉及文件：** device/CMakeLists.txt

---

### 2026-04-08 — Spec: spec-0-gstreamer-capture / 任务: 1.2 创建目录结构和占位文件

**完成概要：** 创建 4 个占位文件（pipeline_manager.h、pipeline_manager.cpp、main.cpp、smoke_test.cpp），确保 CMake 配置和编译通过。

**测试状态：** 未运行（轻量模式，纯占位文件创建）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/pipeline_manager.h, device/src/pipeline_manager.cpp, device/src/main.cpp, device/tests/smoke_test.cpp

---

### 2026-04-08 — Spec: spec-0-gstreamer-capture / 任务: 2.1 + 2.2 实现 PipelineManager

**完成概要：** 实现 pipeline_manager.h 接口声明和 pipeline_manager.cpp 完整实现（create/start/stop/current_state/析构/移动语义），编译零错误零警告。

**测试状态：** 未运行（测试覆盖将在任务 5 冒烟测试中统一实现）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。首次子代理调用因网络 TLS 断连失败，重试后成功。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/pipeline_manager.h, device/src/pipeline_manager.cpp

---

### 2026-04-08 — Spec: spec-0-gstreamer-capture / 任务: 3. 检查点 — 确认 PipelineManager 编译通过

**完成概要：** 清理旧 build 目录后重新 cmake 配置 + 编译，GStreamer 1.28.1 和 GTest v1.14.0 正确发现，三个 target 全部编译零错误零警告。

**测试状态：** 未运行（检查点任务，仅验证编译）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯编译验证）

---

### 2026-04-08 — Spec: spec-0-gstreamer-capture / 任务: 4. 实现 main.cpp 应用入口

**完成概要：** 实现 main.cpp 完整应用入口（run_pipeline + bus_callback + sigint_handler + macOS/Linux 平台隔离），额外在 pipeline_manager.h 添加 `pipeline()` accessor 供获取 bus 使用，编译零错误零警告。

**测试状态：** 未运行（main.cpp 为应用入口，测试覆盖由 Task 5 冒烟测试统一处理）— 无新增测试

**Trace 记录：**

| # | 症状 | 归因类别 | 完整 Trace | 解决方案 | 建议行动 |
|---|------|---------|-----------|---------|----------|
| 1 | Design 文档未声明 pipeline() accessor，子代理自行添加 | Spec 缺少信息 | main.cpp 需要 `gst_element_get_bus(pm->pipeline())` 获取 bus，但 pipeline_manager.h 原始设计无此方法 | 子代理添加 `GstElement* pipeline() const` 内联方法 | 后续 Spec 中如果外部需要访问内部 GstElement*，在 design.md 中明确声明 accessor |

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。（单次问题，观察后续是否反复出现）

**涉及文件：** device/src/main.cpp, device/src/pipeline_manager.h

---

### 2026-04-08 — Spec: spec-0-gstreamer-capture / 任务: 5.1 实现冒烟测试

**完成概要：** 实现 8 个 GTest 冒烟测试（CreateValidPipeline、CreateInvalidPipeline、CreateUnknownElement、StartPipeline、StopPipeline、StopIdempotent、RAIICleanup、NoCopy），全部使用 fakesink，编译通过，ctest 全部通过（83ms），ASan 无报告。

**测试状态：** 全部通过（8/8）— 新增 8 个测试

**Trace 记录：**

| # | 症状 | 归因类别 | 完整 Trace | 解决方案 | 建议行动 |
|---|------|---------|-----------|---------|----------|
| 1 | 首次子代理调用 TLS 断连 | 网络问题 | `Client network socket disconnected before secure TLS connection was established` | 重试成功 | 无需行动（网络瞬断） |

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/tests/smoke_test.cpp

---

### 2026-04-08 — Spec: spec-0-gstreamer-capture / 任务: 6. 最终检查点 — 全量验证

**完成概要：** 从干净 build 目录执行完整验证（cmake 配置 + 编译 + ctest），CMake 配置成功、编译零错误零警告、8 个冒烟测试全部通过（1.55s）、ASan 无报告。

**测试状态：** 全部通过（8/8，1.55s）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-08 — Spec: spec-1-spdlog-logging / 任务: 1. CMake 集成（1.1 + 1.2 + 1.3）

**完成概要：** 在 CMakeLists.txt 中添加 spdlog v1.15.0 和 RapidCheck FetchContent，创建 log_module 静态库 target 和 log_test 测试 target，4 个骨架文件就位，pipeline_manager 已链接 log_module。

**测试状态：** 全部通过（2/2：smoke_test + log_test）— 新增 1 个占位测试（log_test Placeholder）

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/CMakeLists.txt, device/src/json_formatter.h, device/src/json_formatter.cpp, device/src/log_init.h, device/src/log_init.cpp, device/tests/log_test.cpp

---

### 2026-04-08 — Spec: spec-1-spdlog-logging / 任务: 2. Checkpoint — 验证 CMake 配置和编译

**完成概要：** cmake configure + build + ctest 全部通过，spdlog + RapidCheck 自动下载，编译零错误，2/2 测试通过（smoke_test + log_test），ASan 无报告。

**测试状态：** 全部通过（2/2）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-08 — Spec: spec-1-spdlog-logging / 任务: 3. 实现 JsonFormatter（3.1 + 3.2）

**完成概要：** 实现 JsonFormatter（json_formatter.h + json_formatter.cpp），输出单行 JSON 格式日志，手动 JSON 转义。编写 Property Test JsonFormatValidity.MsgRoundTrips（100+ 迭代），验证任意字符串消息的 JSON 格式有效性和 msg 字段 round-trip 正确性。

**测试状态：** 全部通过（2/2：smoke_test + log_test[1 property test]）— 新增 1 个 property test

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/json_formatter.h, device/src/json_formatter.cpp, device/tests/log_test.cpp

---

### 2026-04-08 — Spec: spec-1-spdlog-logging / 任务: 4. 实现 log_init 模块（4.1 + 4.2 + 4.3 + 4.4）

**完成概要：** 实现 log_init 模块（init/create_logger/shutdown），编写 4 个 example-based 单元测试和 2 个 property tests（日志级别过滤 + Logger 工厂函数），全部通过。

**测试状态：** 全部通过（7/7：smoke_test 8 tests + log_test 7 tests）— 新增 4 个单元测试 + 2 个 property tests

**Trace 记录：**

| # | 症状 | 归因类别 | 完整 Trace | 解决方案 | 建议行动 |
|---|------|---------|-----------|---------|----------|
| 1 | spdlog v1.15.0 的 stderr_color_sink.h 路径变更 | Spec 缺少信息 | `#include <spdlog/sinks/stderr_color_sink.h>` 编译失败，需要 `<spdlog/sinks/stdout_color_sinks.h>` | 修改 include 路径 | Design 文档中 spdlog include 示例应使用 v1.15.0 实际路径 |
| 2 | spdlog v1.15.0 需要显式 include pattern_formatter | Spec 缺少信息 | `spdlog::pattern_formatter` 未声明，需要 `#include <spdlog/pattern_formatter.h>` | 添加显式 include | 同上 |

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。（spdlog header 路径问题为单次问题，已在实现中修复）

**涉及文件：** device/src/log_init.h, device/src/log_init.cpp, device/tests/log_test.cpp

---

### 2026-04-08 — Spec: spec-1-spdlog-logging / 任务: 5. Checkpoint — 验证日志核心模块

**完成概要：** 全量验证通过，7/7 测试通过（smoke_test + log_test），ASan 无报告。

**测试状态：** 全部通过（2 test executables, 15 total tests）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-08 — Spec: spec-1-spdlog-logging / 任务: 6. 迁移现有代码（6.1 + 6.2）

**完成概要：** main.cpp 中所有 g_printerr 替换为 spdlog 调用（含 --log-json 参数解析），pipeline_manager.cpp 添加 create/start/stop 诊断日志，Spec 0 smoke_test 回归通过。

**测试状态：** 全部通过（smoke_test 8 tests + log_test 7 tests）— 无新增测试（由 smoke_test 回归覆盖）

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/main.cpp, device/src/pipeline_manager.cpp

---

### 2026-04-08 — Spec: spec-1-spdlog-logging / 任务: 7. 最终 Checkpoint — 全量验证

**完成概要：** 干净构建全量验证通过，smoke_test 8/8 + log_test 7/7，编译零错误，ASan 无报告，总耗时 2.91s。

**测试状态：** 全部通过（15/15 total tests across 2 executables）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-08 — Spec: spec-2-cross-compile / 任务: 1.1 创建 docs/pi-setup.md

**完成概要：** 创建 Pi 5 环境配置文档（英文），包含 Prerequisites、Install Build Dependencies、Clone Repository、First Build Verification、SSH Key Setup 五个章节。

**测试状态：** 未运行（轻量模式，纯文档变更）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** docs/pi-setup.md

---

### 2026-04-08 — Spec: spec-2-cross-compile / 任务: 2.1 创建 scripts/pi-build.sh

**完成概要：** 创建 SSH 远程构建脚本，包含环境变量默认值、SSH 连接检测、heredoc 远程执行（git pull + cmake Release + build + ctest），`bash -n` 语法检查通过，`chmod +x` 已设置。

**测试状态：** 未运行（轻量模式，纯 Bash 脚本创建）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** scripts/pi-build.sh

---

### 2026-04-08 — Spec: spec-2-cross-compile / 任务: 3.1 创建 scripts/build-all.sh

**完成概要：** 创建双平台验证脚本，包含 SCRIPT_DIR/PROJECT_ROOT 推导、Pi 5 可达性缓存、Phase 1 macOS Debug + Phase 2 Pi 5 Release、摘要输出，`bash -n` 语法检查通过，`chmod +x` 已设置。

**测试状态：** 未运行（轻量模式，纯 Bash 脚本创建）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** scripts/build-all.sh

---

### 2026-04-08 — Spec: spec-2-cross-compile / 任务: 4.1 脚本语法检查 + 4.2 macOS 本地编译回归验证

**完成概要：** `bash -n` 两个脚本语法检查通过；macOS Debug cmake configure + build + ctest 全部通过（smoke_test 0.45s + log_test 0.09s），ASan 无报告。

**测试状态：** 全部通过（2/2：smoke_test + log_test）— 无新增测试

**Trace 记录：**

| # | 症状 | 归因类别 | 完整 Trace | 解决方案 | 建议行动 |
|---|------|---------|-----------|---------|----------|
| 1 | cmake configure 首次因 GitHub SSL 瞬时错误失败（FetchContent 下载） | 网络问题 | SSL connect error，重试成功 | 重试 | 无需行动（网络瞬断） |

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-08 — Spec: spec-2-cross-compile / 任务: 4.3 验证 CMakeLists.txt 平台适配（代码审查）

**完成概要：** 审查 CMakeLists.txt 和源码的 Pi 5 兼容性，5 个维度全部通过（pkg-config、FetchContent、ASan、条件编译、GCC 兼容），确认无需修改 CMakeLists.txt。

**测试状态：** 未运行（轻量模式，纯代码审查）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯审查）

---

### 2026-04-08 — Spec: spec-2-cross-compile / 任务: 5. 最终检查点 — 全量验证

**完成概要：** 全量验证 6/6 项通过：bash -n 两脚本语法正确、cmake configure + build + ctest 全部通过（smoke_test + log_test）、ASan 无报告、docs/pi-setup.md 存在且完整。Pi 5 远程验证标注 SKIPPED（不可达）。

**测试状态：** 全部通过（2/2：smoke_test + log_test）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-08 — Spec: spec-2-cross-compile / Pi 5 实机验证

**完成概要：** Pi 5 实机验证 pi-build.sh 双模式脚本，过程中发现并解决多个环境问题，最终远程和本地模式均跑通。

**测试状态：** Pi 5 远程构建通过 — 无新增测试

**Trace 记录：**

| # | 症状 | 归因类别 | 完整 Trace | 解决方案 | 建议行动 |
|---|------|---------|-----------|---------|----------|
| 1 | Pi 5 上 RapidCheck FetchContent 拉 submodule 时 TLS 握手失败 | Spec 缺少信息 | `gnutls_handshake() failed: The TLS connection was non-properly terminated.` clone ext/catch 和 ext/googletest 失败 | CMakeLists.txt 中 RapidCheck 的 FetchContent_Declare 加 `GIT_SUBMODULES ""` | Design 层禁止项：FetchContent 引入第三方库时必须加 `GIT_SUBMODULES ""` 防止不必要的 submodule 下载 |
| 2 | macOS 上 `PI_REPO_DIR=~/Workspace/raspi-eye` 被本地展开为 `/Users/wangcjer/...` 传到 Pi 5 上找不到 | Spec 缺少信息 | `bash: line 3: cd: /Users/wangcjer/Workspace/raspi-eye: No such file or directory` | 使用单引号 `PI_REPO_DIR='~/Workspace/raspi-eye'` 防止本地展开 | 文档中说明 PI_REPO_DIR 必须用单引号设置 |
| 3 | macOS 和 Pi 5 上都没有 SSH key，`ssh-copy-id` 报 `No identities found` | Spec 缺少信息 | `/usr/bin/ssh-copy-id: ERROR: No identities found` | 先 `ssh-keygen -t ed25519` 再 `ssh-copy-id` | pi-setup.md 中补充 macOS 端也需要先生成 SSH key |
| 4 | Pi 5 仓库路径为 `~/Workspace/raspi-eye` 而非默认的 `~/raspi-eye` | 用户环境差异 | `cd: /home/pi/raspi-eye: No such file or directory` | 通过 `PI_REPO_DIR` 环境变量覆盖 | 文档中说明默认值和覆盖方式 |
| 5 | pi-build.sh 原设计为纯 SSH 远程脚本，Pi 5 上无法直接使用 | Spec 不够精确 | 用户需要在 Pi 5 上一键编译，但脚本只支持远程模式 | 改为双模式：`uname -s` 检测平台，Linux 本地 / macOS SSH 远程 | 后续脚本设计时考虑双平台使用场景 |

**提炼的禁止项（SHALL NOT）：**

- **Design 层：** SHALL NOT 在 FetchContent_Declare 中省略 `GIT_SUBMODULES ""`（除非确实需要 submodule），Pi 5 网络不稳定时 submodule clone 容易 TLS 失败

本次无新增禁止项。（FetchContent submodule 问题为首次出现，观察后续是否反复）

**涉及文件：** scripts/pi-build.sh, device/CMakeLists.txt, docs/pi-setup.md

---

### 2026-04-08 — Spec: spec-3-h264-tee-pipeline / 任务: 1. PipelineManager 新工厂方法（1.1 + 1.2）

**完成概要：** 提取 `ensure_gst_init()` 私有静态方法，添加 `create(GstElement*, std::string*)` 工厂方法重载，编译通过，15 个现有测试回归通过。

**测试状态：** 全部通过（15/15：smoke_test 8 + log_test 7）— 无新增测试（测试覆盖将在任务 4 tee_test 中实现）

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/pipeline_manager.h, device/src/pipeline_manager.cpp

---

### 2026-04-08 — Spec: spec-3-h264-tee-pipeline / 任务: 2. PipelineBuilder 实现（2.1 + 2.2 + 2.3）

**完成概要：** 创建 pipeline_builder.h（namespace + 函数声明 + 拓扑注释）和 pipeline_builder.cpp（EncoderCandidate 候选列表 + create_encoder + link_tee_to_element + build_tee_pipeline 完整实现），getDiagnostics 无语法问题。

**测试状态：** 未运行（源文件创建，CMakeLists.txt 更新在任务 3，测试在任务 4）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/pipeline_builder.h, device/src/pipeline_builder.cpp

---

### 2026-04-08 — Spec: spec-3-h264-tee-pipeline / 任务: 3. CMakeLists.txt 更新与编译验证（3.1 + 3.2）

**完成概要：** pipeline_manager 静态库添加 pipeline_builder.cpp，新增 tee_test 测试目标（占位），macOS Debug 编译通过，16 个测试全部通过（8 smoke + 7 log + 1 tee placeholder）。

**测试状态：** 全部通过（16/16）— 新增 1 个占位测试（tee_test Placeholder）

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/CMakeLists.txt, device/tests/tee_test.cpp（占位）

---

### 2026-04-08 — Spec: spec-3-h264-tee-pipeline / 任务: 4. tee_test 冒烟测试（4.1 + 4.2）

**完成概要：** 实现 8 个 GTest 冒烟测试（AdoptValidPipeline、AdoptNullPipeline、AdoptedPipelineStart、TeePipelinePlaying、TeePipelineNamedSinks、TeePipelineStop、TeePipelineRAII、EncoderDetection），自定义 main() 先调用 gst_init，CMakeLists.txt 改为链接 GTest::gtest。ctest 23 个测试全部通过，ASan 无报告。

**测试状态：** 全部通过（23/23：8 smoke + 7 log + 8 tee）— 新增 8 个测试

**Trace 记录：**

| # | 症状 | 归因类别 | 完整 Trace | 解决方案 | 建议行动 |
|---|------|---------|-----------|---------|----------|
| 1 | tee_test 需要自定义 main() 先调用 gst_init，因为测试直接调用 gst_pipeline_new | Spec 缺少信息 | 使用 GTest::gtest_main 时 gst_init 未被调用，gst_pipeline_new 返回 nullptr | 改为自定义 main() + GTest::gtest，先 gst_init 再 RUN_ALL_TESTS | 后续 Spec 中如果测试直接调用 GStreamer C API，在 tasks.md 中明确说明需要自定义 main() |

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。（gst_init 问题为首次出现，观察后续是否反复）

**涉及文件：** device/tests/tee_test.cpp, device/CMakeLists.txt

---

### 2026-04-08 — Spec: spec-3-h264-tee-pipeline / 任务: 5. 检查点 — 确认现有测试回归通过

**完成概要：** 确认 23 个测试全部通过（8 smoke + 7 log + 8 tee），行为不变，ASan 无报告。

**测试状态：** 全部通过（23/23）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-09 — Spec: spec-3-h264-tee-pipeline / 任务: 6. main.cpp 管道升级（6.1 + 6.2）

**完成概要：** main.cpp 中 `gst_parse_launch` 单路管道替换为 `PipelineBuilder::build_tee_pipeline()` + `PipelineManager::create(GstElement*)`，编译零错误，23 个测试全部通过，ASan 无报告。

**测试状态：** 全部通过（23/23：8 smoke + 7 log + 8 tee）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/main.cpp

---

### 2026-04-09 — Spec: spec-3-h264-tee-pipeline / 任务: 7. 最终检查点 — 全量验证

**完成概要：** 干净 build 全量验证通过，CMake 配置成功、编译无错误、23 个测试全部通过（8 smoke + 7 log + 8 tee）、ASan 无报告。Pi 5 Release 验证标注 SKIPPED（不可达）。

**测试状态：** 全部通过（23/23）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-09 — Spec: spec-4-camera-abstraction / 任务: 1.1 + 1.2 CameraSource 模块创建

**完成概要：** 创建 camera_source.h（CameraType 枚举 + CameraConfig POD + 4 个函数声明）和 camera_source.cpp（default_camera_type + camera_type_name + parse_camera_type + create_source 完整实现），完全按照 design.md 接口定义。

**测试状态：** 未运行（轻量模式，测试覆盖将在任务 4 camera_test 中实现）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/camera_source.h, device/src/camera_source.cpp

---

### 2026-04-09 — Spec: spec-4-camera-abstraction / 任务: 2.1 + 2.2 PipelineBuilder 签名扩展

**完成概要：** pipeline_builder.h 添加 `#include "camera_source.h"` 并扩展 build_tee_pipeline 签名（CameraConfig 默认参数），pipeline_builder.cpp 将硬编码 videotestsrc 替换为 CameraSource::create_source 调用，管道拓扑不变。

**测试状态：** 未运行（轻量模式，签名扩展向后兼容，由 tee_test 回归 + 任务 4 camera_test 覆盖）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/pipeline_builder.h, device/src/pipeline_builder.cpp

---

### 2026-04-09 — Spec: spec-4-camera-abstraction / 任务: 3.1 + 3.2 CMakeLists.txt 更新与编译验证

**完成概要：** pipeline_manager 库添加 camera_source.cpp，新增 camera_test 测试目标（GTest::gtest），创建占位 camera_test.cpp，macOS Debug 编译零错误。

**测试状态：** 未运行（轻量模式，检查点任务）— 新增 1 个占位测试（camera_test Placeholder）

**Trace 记录：**

| # | 症状 | 归因类别 | 完整 Trace | 解决方案 | 建议行动 |
|---|------|---------|-----------|---------|----------|
| 1 | GitHub 网络不可达，FetchContent update step 失败 | 网络问题 | `fatal: unable to access 'https://github.com/emil-e/rapidcheck.git/': Failed to connect to github.com port 443 after 75003 ms` | 使用 `-DFETCHCONTENT_UPDATES_DISCONNECTED=ON` 跳过更新 | 无需行动（网络瞬断，依赖已缓存） |

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/CMakeLists.txt, device/tests/camera_test.cpp（占位）

---

### 2026-04-09 — Spec: spec-4-camera-abstraction / 任务: 4.1 + 4.2 camera_test 测试

**完成概要：** 创建 camera_test.cpp 包含 11 个测试用例（DefaultCameraType、CameraTypeName、ParseCameraTypeValid/CaseInsensitive/Invalid、CreateSourceTest、CreateSourceUnavailable[macOS]、TeePipelineDefaultConfig/ExplicitTest/SourceElement），全量测试 4/4 套件通过（smoke 8 + log 7 + tee 8 + camera ~10），ASan 无报告，总耗时 0.65s。

**测试状态：** 全部通过（4/4 套件）— 新增 ~10 个测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/tests/camera_test.cpp

---

### 2026-04-09 — Spec: spec-4-camera-abstraction / 任务: 5. 检查点 — 确认现有测试回归通过

**完成概要：** 确认 4/4 套件全部通过（smoke_test 0.30s + log_test 0.07s + tee_test 0.15s + camera_test 0.13s），ASan 无报告，总耗时 0.65s。

**测试状态：** 全部通过（4/4 套件）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-09 — Spec: spec-4-camera-abstraction / 任务: 6.1 + 6.2 main.cpp 命令行集成与编译验证

**完成概要：** main.cpp 添加 --camera/--device 命令行解析（三阶段模式：解析→初始化日志→验证），cam_config 传入 build_tee_pipeline，编译零错误，4/4 套件全部通过（0.65s），ASan 无报告。

**测试状态：** 全部通过（4/4 套件）— 无新增测试（main.cpp 命令行解析由 camera_test 中 parse_camera_type 测试间接覆盖）

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** device/src/main.cpp

---

### 2026-04-09 — Spec: spec-4-camera-abstraction / 任务: 7. 最终检查点 — 全量验证

**完成概要：** 干净 build 全量验证通过，CMake 配置成功（GStreamer 1.28.1）、编译零错误、4/4 套件全部通过（smoke_test 1.53s + log_test 1.13s + tee_test 1.12s + camera_test 0.96s = 4.76s）、ASan 无报告。Pi 5 Release 验证标注 SKIPPED（不可达）。

**测试状态：** 全部通过（4/4 套件）— 无新增测试

**Trace 记录：**

无异常，任务顺利完成。

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。

**涉及文件：** 无文件变更（纯验证）

---

### 2026-04-09 — Spec: spec-4-camera-abstraction / Pi 5 Release 验证

**完成概要：** Pi 5 Release 构建通过，但 tee_test.TeePipelinePlaying 和 camera_test.TeePipelineDefaultConfig 失败。根因：`current_state()` 中 `gst_element_get_state` 超时 1 秒不够，Pi 5 上 x264enc 软编码器初始化需要 ~1.8 秒。修复：超时从 1s 改为 3s，Pi 5 全部通过。

**测试状态：** 修复后全部通过（4/4 套件：smoke 8 + log 7 + tee 8 + camera 9）— 无新增测试

**Trace 记录：**

| # | 症状 | 归因类别 | 完整 Trace | 解决方案 | 建议行动 |
|---|------|---------|-----------|---------|----------|
| 1 | Pi 5 上 TeePipelinePlaying 和 TeePipelineDefaultConfig 失败，current_state() 返回 3（PAUSED）而非 4（PLAYING） | Spec 缺少信息 | `Expected equality: pm->current_state() Which is: 3, GST_STATE_PLAYING Which is: 4`，耗时 1815ms / 1615ms | `gst_element_get_state` 超时从 1s 改为 3s（pipeline_manager.cpp） | 后续 Spec 中涉及 `gst_element_get_state` 超时时，考虑 Pi 5 硬件编码器初始化延迟，默认使用 3s 超时 |

**提炼的禁止项（SHALL NOT）：**

本次无新增禁止项。（首次出现 Pi 5 状态转换超时问题，观察后续是否反复）

**涉及文件：** device/src/pipeline_manager.cpp

---
