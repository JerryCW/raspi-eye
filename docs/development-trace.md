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
