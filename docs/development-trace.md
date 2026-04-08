# Development Trace

基于 Meta-Harness 方法论的开发 trace 知识库。
用于禁止项反哺、失败归因追踪、Spec 迭代优化。

## 已提炼的 SHALL NOT（禁止项池）

_从反复出现的失败模式中提炼，直接复制到下一轮 Spec。_

暂无。

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
