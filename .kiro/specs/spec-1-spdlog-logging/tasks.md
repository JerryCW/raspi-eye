# Implementation Plan: spdlog 结构化日志

## 概述

将 spdlog 日志基础设施集成到 device 模块，按以下顺序递增实现：CMake 集成 → JsonFormatter → log_init 模块 → 测试 → 现有代码迁移 → 最终验证。每一步都在前一步基础上构建，确保无孤立代码。

## Tasks

- [x] 1. CMake 集成：添加 spdlog + RapidCheck FetchContent 和 log_module target
  - [x] 1.1 在 `device/CMakeLists.txt` 中添加 spdlog v1.15.0 FetchContent 声明（在现有 googletest FetchContent 之后）
    - 添加 `FetchContent_Declare(spdlog ...)` 和 `FetchContent_MakeAvailable(spdlog)`
    - _Requirements: 1.1, 1.2_
  - [x] 1.2 在 `device/CMakeLists.txt` 中添加 RapidCheck FetchContent 声明
    - 添加 `FetchContent_Declare(rapidcheck ...)` 和 `set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)` 和 `FetchContent_MakeAvailable(rapidcheck)`
    - _Requirements: 7.1_
  - [x] 1.3 创建 `log_module` 静态库 target（暂用空的 .cpp 占位）和 `log_test` 测试 target
    - 创建 `device/src/json_formatter.h`、`device/src/json_formatter.cpp`、`device/src/log_init.h`、`device/src/log_init.cpp` 的最小骨架文件
    - 添加 `add_library(log_module STATIC src/json_formatter.cpp src/log_init.cpp)`
    - `target_link_libraries(log_module PUBLIC spdlog::spdlog)`
    - 修改 `pipeline_manager` 链接 `log_module`：`target_link_libraries(pipeline_manager PUBLIC ${GST_LIBRARIES} log_module)`
    - 创建 `device/tests/log_test.cpp` 最小骨架（一个空 TEST）
    - 添加 `add_executable(log_test tests/log_test.cpp)` + `target_link_libraries(log_test PRIVATE log_module GTest::gtest_main rapidcheck rapidcheck_gtest)` + `add_test(NAME log_test COMMAND log_test)`
    - _Requirements: 1.2, 1.3_

- [x] 2. Checkpoint — 验证 CMake 配置和编译
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确保 spdlog + RapidCheck 自动下载、编译无错误、所有测试通过（含 Spec 0 smoke_test）
  - 如有问题请询问用户

- [x] 3. 实现 JsonFormatter
  - [x] 3.1 实现 `device/src/json_formatter.h` 和 `device/src/json_formatter.cpp`
    - `JsonFormatter` 继承 `spdlog::formatter`，实现 `format()` 和 `clone()`
    - `format()` 输出单行 JSON：`{"ts":"ISO8601","level":"...","logger":"...","msg":"..."}\n`
    - 时间戳使用 `msg.time` 格式化为 ISO 8601（UTC，毫秒精度）
    - 对 `msg.payload` 中的 `"`、`\`、控制字符进行 JSON 转义
    - 使用 `fmt::format_to(std::back_inserter(dest), ...)` 写入 `spdlog::memory_buf_t`
    - SHALL NOT 引入额外 JSON 解析库
    - _Requirements: 3.2, 3.3, 3.5_
  - [x] 3.2 编写 Property Test：JSON 格式有效性
    - **Property 1: JSON 格式有效性**
    - 在 `device/tests/log_test.cpp` 中使用 `RC_GTEST_PROP` 编写
    - 对任意 `std::string` 消息，验证 JsonFormatter 输出为合法单行 JSON，包含 `ts`、`level`、`logger`、`msg` 四个字段，且 `msg` 反转义后与原始消息一致
    - 通过 `ostream_sink_mt` 捕获输出，手动解析 JSON 结构验证
    - 最少 100 次迭代
    - **Validates: Requirements 3.1, 3.2, 3.3**

- [x] 4. 实现 log_init 模块
  - [x] 4.1 实现 `device/src/log_init.h` 和 `device/src/log_init.cpp`
    - `log_init::init(bool json = false)`：创建共享 `stderr_color_sink_mt`，根据 json 参数设置 JsonFormatter 或 pattern_formatter（`[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v`），注册 `"main"` 和 `"pipeline"` Logger，默认级别 `info`，幂等
    - `log_init::create_logger(const std::string& name)`：创建新命名 Logger，共享 sink 和格式，注册到 spdlog registry
    - `log_init::shutdown()`：调用 `spdlog::shutdown()` + `g_sink.reset()`，幂等
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 3.1, 4.1, 6.1, 6.2, 6.3_
  - [x] 4.2 编写 Example-Based 单元测试：初始化和关闭
    - 在 `device/tests/log_test.cpp` 中编写：
    - `InitCreatesLoggers`：init() 后 `spdlog::get("main")` 和 `spdlog::get("pipeline")` 非空
    - `DefaultPatternFormat`：init(false) 后日志输出包含时间戳、模块名、级别和消息
    - `ShutdownIdempotent`：shutdown() 调用两次无崩溃
    - `ShutdownCleanup`：init() + 使用 + shutdown()，ASan 无报告
    - _Requirements: 2.3, 2.4, 3.1, 6.1, 6.2, 6.3, 7.2, 7.4_
  - [x] 4.3 编写 Property Test：日志级别过滤正确性
    - **Property 2: 日志级别过滤正确性**
    - 在 `device/tests/log_test.cpp` 中使用 `RC_GTEST_PROP` 编写
    - 对任意 Logger 级别 L 和消息级别 M 的组合，验证消息出现在输出中当且仅当 M >= L
    - 验证修改一个 Logger 的级别不影响其他 Logger
    - 通过 `ostream_sink_mt` 捕获输出验证
    - 最少 100 次迭代
    - **Validates: Requirements 4.2, 4.3**
  - [x] 4.4 编写 Property Test：Logger 工厂函数正确性
    - **Property 3: Logger 工厂函数正确性**
    - 在 `device/tests/log_test.cpp` 中使用 `RC_GTEST_PROP` 编写
    - 对任意合法 Logger 名称（非空 ASCII），调用 `create_logger(name)` 后 `spdlog::get(name)` 返回非空实例，默认级别为 `info`
    - 最少 100 次迭代
    - **Validates: Requirements 2.4, 2.6, 4.1**

- [x] 5. Checkpoint — 验证日志核心模块
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确保所有测试通过（含 Spec 0 smoke_test）、ASan 无报告
  - 如有问题请询问用户

- [x] 6. 迁移现有代码：main.cpp 和 PipelineManager
  - [x] 6.1 修改 `device/src/main.cpp`：替换所有 `g_printerr` 为 spdlog 调用
    - 在 `run_pipeline()` 开头解析 `--log-json` 参数，调用 `log_init::init(use_json)`
    - `bus_callback` 中：ERROR → `spdlog::get("main")->error(...)` + `debug(...)`，EOS → `info(...)`
    - 管道创建失败 → `error(...)`，启动失败 → `error(...)`
    - 退出前调用 `log_init::shutdown()`
    - 替换完成后不再包含任何 `g_print` 或 `g_printerr` 调用
    - SHALL NOT 在日志消息中使用非 ASCII 字符
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.6_
  - [x] 6.2 修改 `device/src/pipeline_manager.cpp`：添加 spdlog 诊断日志
    - `#include <spdlog/spdlog.h>`
    - create 成功 → `spdlog::get("pipeline")->info("Pipeline created: {}", pipeline_desc)`
    - start 成功 → `info("Pipeline started")`，失败 → `error(...)`
    - stop → `info("Pipeline stopped")`
    - SHALL NOT 在日志消息中使用非 ASCII 字符
    - _Requirements: 5.7_

- [x] 7. 最终 Checkpoint — 全量验证
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确保所有测试通过（含 Spec 0 smoke_test 回归验证）、ASan 无报告
  - 验证 Spec 0 冒烟测试（PipelineManager 创建、启动、停止）仍然通过，日志替换未破坏已有功能
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题请询问用户
  - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7_

## Notes

- 标记 `*` 的子任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求编号，确保可追溯
- Property test 使用 RapidCheck + GTest 集成（`RC_GTEST_PROP` 宏）
- 所有测试通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- Debug 构建下 ASan 自动生效，测试运行时自动检测内存问题
