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
