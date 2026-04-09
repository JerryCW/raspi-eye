# 实施计划：Spec 5 — 管道健康监控 + 自动恢复

## 概述

为 GStreamer 管道引入独立的 `PipelineHealthMonitor` 类，实现三层故障检测（Buffer Probe Watchdog + Bus ERROR 监听 + 心跳轮询）和两级自动恢复（状态重置 → 完全重建）。按依赖顺序：pipeline_health.h/cpp 新建 → CMakeLists.txt 更新 → health_test.cpp 测试 → main.cpp 集成 → 双平台验证。实现语言为 C++17，包含 3 个 PBT 属性 + 9 个 example-based 测试。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 GStreamer C API 外）
- SHALL NOT 在高性能路径（Buffer Probe 回调）中执行同步磁盘 I/O
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 修改现有测试文件（smoke_test.cpp、log_test.cpp、tee_test.cpp、camera_test.cpp）
- SHALL NOT 在不确定外部 SDK/库 API 用法时凭猜测编写代码

## 任务

- [x] 1. PipelineHealthMonitor 头文件与实现
  - [x] 1.1 创建 `device/src/pipeline_health.h`
    - 定义 `HealthState` 枚举：`HEALTHY`、`DEGRADED`、`ERROR`、`RECOVERING`、`FATAL`
    - 声明 `health_state_name(HealthState)` 返回人类可读状态名
    - 定义 `HealthConfig` POD 结构体：`watchdog_timeout_ms`（默认 5000）、`heartbeat_interval_ms`（默认 2000）、`initial_backoff_ms`（默认 1000）、`max_retries`（默认 3）
    - 定义 `HealthStats` 结构体：`total_recoveries`、`last_recovery_time`、`healthy_since`
    - 定义 `HealthCallback` 和 `RebuildCallback` 类型别名
    - 声明 `PipelineHealthMonitor` 类，包含：
      - 构造函数 `(GstElement* pipeline, const HealthConfig& config = {})`，不接管 pipeline 所有权
      - `start(const std::string& source_element_name = "src")`：安装 probe + bus watch + 定时器
      - `stop()`：移除 probe + 定时器 + bus watch
      - `state() const`：线程安全返回当前状态
      - `stats() const`：线程安全返回统计信息
      - `set_health_callback(HealthCallback)`
      - `set_rebuild_callback(RebuildCallback)`
      - `set_pipeline(GstElement*, const std::string&)`：rebuild 后更新管道指针
    - 拷贝构造和拷贝赋值 `= delete`
    - 私有成员：`mutex_`、`state_`、`stats_`、`config_`、`pipeline_`、`probe_pad_`、`probe_id_`、`bus_watch_id_`、`watchdog_timer_id_`、`heartbeat_timer_id_`、`health_cb_`、`rebuild_cb_`、`recovery_in_progress_`、`consecutive_failures_`、`current_backoff_ms_`、`last_buffer_time_`
    - _需求：1.1, 1.2, 1.8, 2.1, 2.5, 5.3, 6.5, 7.1, 7.3, 8.1, 8.4_
  - [x] 1.2 创建 `device/src/pipeline_health.cpp`
    - 实现 `health_state_name()`：switch 返回 `"HEALTHY"` / `"DEGRADED"` / `"ERROR"` / `"RECOVERING"` / `"FATAL"`
    - 实现构造函数：初始化 `config_`、`pipeline_`、`state_ = HEALTHY`、`current_backoff_ms_ = config.initial_backoff_ms`、`last_buffer_time_ = now`、`stats_.healthy_since = now`
    - 实现 `start()`：
      - 调用 `install_probe(source_element_name)` 安装 buffer probe
      - 获取 bus 并注册 `bus_watch_cb`（`gst_bus_add_watch`），获取后立即 `gst_object_unref(bus)`
      - 注册 watchdog 定时器（`g_timeout_add(config_.watchdog_timeout_ms, watchdog_timer_cb, this)`）
      - 注册 heartbeat 定时器（`g_timeout_add(config_.heartbeat_interval_ms, heartbeat_timer_cb, this)`）
    - 实现 `stop()`：
      - `g_source_remove` 移除 watchdog 和 heartbeat 定时器（ID 非零时）
      - `g_source_remove` 移除 bus watch（ID 非零时）
      - 调用 `remove_probe()` 移除 buffer probe
      - 所有 ID 置零
    - 实现析构函数：调用 `stop()`
    - 实现 `state()`：`std::lock_guard` 保护，返回 `state_`
    - 实现 `stats()`：`std::lock_guard` 保护，返回 `stats_` 拷贝
    - 实现 `transition_to()`：验证合法转换（参照设计文档状态转换表），非法转换 spdlog warn 并返回 false
    - 实现 `buffer_probe_cb()`：仅 `std::lock_guard` 更新 `last_buffer_time_ = now`，返回 `GST_PAD_PROBE_OK`
    - 实现 `bus_watch_cb()`：
      - `GST_MESSAGE_ERROR`：spdlog error 记录来源元素和错误描述，`g_error_free` + `g_free`，调用 `attempt_recovery()`
      - `GST_MESSAGE_WARNING`：spdlog warn 记录，不触发状态转换，`g_error_free` + `g_free`
      - `GST_MESSAGE_EOS`：spdlog info 记录
    - 实现 `watchdog_timer_cb()`：
      - 计算 `now - last_buffer_time_` 是否超过 `watchdog_timeout_ms`
      - 超时且 `state_ == HEALTHY` 时转换到 `DEGRADED`，spdlog warn 记录超时时长
      - 回调在 mutex 外调用
      - 返回 `G_SOURCE_CONTINUE`
    - 实现 `heartbeat_timer_cb()`：
      - `gst_element_get_state(pipeline_, &state, nullptr, 0)` 非阻塞查询
      - 状态非 `GST_STATE_PLAYING` 且当前为 `HEALTHY` 或 `DEGRADED` 时转换到 `ERROR`
      - 回调在 mutex 外调用
      - 返回 `G_SOURCE_CONTINUE`
    - 实现 `attempt_recovery()`：
      - 检查 `recovery_in_progress_` 防重入，是则跳过
      - 转换到 `RECOVERING`
      - 先 `try_state_reset()`，失败则 `try_full_rebuild()`
      - 成功：重置 `consecutive_failures_` 和 `current_backoff_ms_`，递增 `stats_.total_recoveries`，更新时间戳，转换到 `HEALTHY`
      - 失败：递增 `consecutive_failures_`，检查是否达 `max_retries`
        - 达上限：转换到 `FATAL`，spdlog error
        - 未达上限：翻倍 `current_backoff_ms_`，转换回 `ERROR`，通过 `g_timeout_add` 调度延迟重试
      - 所有回调在 mutex 外调用
    - 实现 `try_state_reset()`：`set_state(NULL)` → `get_state` 等待 → `set_state(PLAYING)`，失败返回 false
    - 实现 `try_full_rebuild()`：调用 `rebuild_cb_`，返回 nullptr 则失败；成功则 `set_pipeline()` 更新指针
    - 实现 `install_probe()`：`gst_bin_get_by_name` 获取源元素 → `gst_element_get_static_pad("src")` → `gst_pad_add_probe` → `gst_object_unref` pad 和元素
    - 实现 `remove_probe()`：`gst_pad_remove_probe` + `gst_object_unref(probe_pad_)` → 置零
    - 实现 `set_pipeline()`：`remove_probe()` → 更新 `pipeline_` → `install_probe()` → 重新注册 bus watch
    - _需求：1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 2.1, 2.2, 2.3, 2.4, 2.5, 3.1, 3.2, 3.3, 3.4, 4.1, 4.2, 4.3, 5.1, 5.2, 5.3, 5.4, 5.5, 6.1, 6.2, 6.3, 6.4, 6.5, 7.1, 7.2, 7.3, 8.1, 8.2, 8.3, 8.4_

- [x] 2. CMakeLists.txt 更新与编译验证
  - [x] 2.1 修改 `device/CMakeLists.txt`
    - 在 `pipeline_manager` 静态库源文件列表中添加 `src/pipeline_health.cpp`
    - 新增 `health_test` 测试目标：
      - `add_executable(health_test tests/health_test.cpp)`
      - `target_link_libraries(health_test PRIVATE pipeline_manager GTest::gtest rapidcheck rapidcheck_gtest)`
      - `add_test(NAME health_test COMMAND health_test)`
    - 注意：`health_test` 链接 `GTest::gtest`（非 `gtest_main`），因为需要自定义 `main()` 调用 `gst_init` + GMainLoop
    - 不修改现有 `smoke_test`、`log_test`、`tee_test`、`camera_test` 的定义
    - _需求：10.3, 10.4_
  - [x] 2.2 macOS Debug 编译验证
    - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
    - 确认编译无错误
    - _需求：10.3_

- [x] 3. 检查点 — 编译通过，现有测试回归
  - 执行 `ctest --test-dir device/build --output-on-failure`
  - 确认现有测试（8 smoke + 7 log + 8 tee + 10 camera）全部通过
  - health_test 此时可能因测试用例未完成而失败，属预期
  - 如有问题，询问用户

- [x] 4. health_test.cpp — Example-Based 测试
  - [x] 4.1 创建 `device/tests/health_test.cpp` — 基础结构与 example-based 测试
    - 自定义 `main()`：`gst_init` → `RUN_ALL_TESTS()`（GMainLoop 在各测试用例内部按需创建和运行）
    - 辅助函数 `test_config()`：返回毫秒级 `HealthConfig`（`watchdog_timeout_ms=100`、`heartbeat_interval_ms=50`、`initial_backoff_ms=10`、`max_retries=3`）
    - 辅助函数 `inject_bus_error(GstElement*)`：向 bus post `GST_MESSAGE_ERROR`
    - 辅助函数 `create_test_pipeline()`：创建 `videotestsrc ! fakesink` 简单管道并设为 PLAYING
    - 辅助函数 `run_until(GMainLoop*, condition, timeout_ms)`：运行 GMainLoop 直到条件满足或超时
    - 9 个 example-based 测试用例：
      - `InitialStateIsHealthy`：构造后 `state() == HEALTHY` — _需求：1.2_
      - `BusErrorTriggersRecovery`：post ERROR → 状态经过 ERROR → RECOVERING → HEALTHY — _需求：3.2, 5.1, 5.2, 9.1, 9.2_
      - `ConsecutiveFailuresReachFatal`：rebuild 回调返回 nullptr，连续失败 3 次 → FATAL — _需求：1.7, 9.3_
      - `HealthCallbackInvoked`：注册回调，触发状态转换，验证回调被调用且 (old, new) 正确 — _需求：8.2, 9.4_
      - `NoCallbackNoCrash`：不注册回调，触发状态转换，不崩溃 — _需求：8.3_
      - `CallbackOutsideMutex`：回调中调用 `state()` 和 `stats()` 不死锁 — _需求：8.4_
      - `StatsIncrementOnRecovery`：恢复成功后 `stats().total_recoveries` 递增 — _需求：7.2, 9.5_
      - `WarningNoStateChange`：post WARNING → 状态保持 HEALTHY — _需求：3.4_
      - `FullRebuildAfterStateResetFails`：状态重置失败时尝试完全重建 — _需求：5.3, 5.4_
    - 所有测试仅使用 `fakesink`，每个测试 ≤ 5 秒
    - _需求：9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 9.7, 9.8, 9.9_

  - [x] 4.2 添加 PBT — Property 1: 指数退避序列与 FATAL 终止
    - **Property 1: 指数退避序列与 FATAL 终止**
    - 随机生成 `initial_backoff_ms`（1~100）和 `max_retries`（1~10）
    - rebuild 回调始终返回 nullptr（强制失败）
    - 连续注入 N 次 ERROR，验证退避间隔序列为 `[B, B*2, B*4, ..., B*2^(N-2)]`
    - 第 N 次失败后 `state() == FATAL`，不再有恢复尝试
    - **验证：需求 1.7, 6.1, 6.2**

  - [x] 4.3 添加 PBT — Property 2: 恢复计数器准确性
    - **Property 2: 恢复计数器准确性**
    - 随机生成 K（1~20）次成功恢复
    - rebuild 回调返回有效管道
    - 验证 `stats().total_recoveries == K`
    - 验证 `stats().last_recovery_time` 不早于第 K 次恢复时间
    - **验证：需求 7.2**

  - [x] 4.4 添加 PBT — Property 3: 状态转换回调正确性
    - **Property 3: 状态转换回调正确性**
    - 随机故障注入序列（ERROR / 恢复成功 / 恢复失败）
    - 注册回调记录所有 `(old_state, new_state)` 对
    - 验证每次转换回调恰好调用一次，且 `(old, new)` 与实际转换匹配
    - **验证：需求 8.2**

- [x] 5. 检查点 — health_test 全量通过
  - 执行 `cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过（约 45 个：8 smoke + 7 log + 8 tee + 10 camera + ~12 health）
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户

- [x] 6. main.cpp 集成 PipelineHealthMonitor
  - [x] 6.1 修改 `device/src/main.cpp`
    - 添加 `#include "pipeline_health.h"`
    - 移除现有的 `bus_callback` 函数（monitor 接管 bus watch）
    - 移除 `gst_bus_add_watch(bus, bus_callback, nullptr)` 及相关 bus 获取/释放代码
    - 在 `pm->start()` 成功后、`g_main_loop_run()` 之前：
      - 创建 `PipelineHealthMonitor health_monitor(pm->pipeline())`
      - 注册 rebuild 回调：lambda 中调用 `PipelineBuilder::build_tee_pipeline` + `PipelineManager::create` + `start`，成功时 `pm = std::move(new_pm)` 并返回 `pm->pipeline()`
      - 注册 health 回调：spdlog 记录状态变化，`FATAL` 时 `g_main_loop_quit(loop)`
      - 调用 `health_monitor.start("src")`
    - 在 `g_main_loop_run()` 之后、`pm->stop()` 之前：
      - 调用 `health_monitor.stop()`
    - 其余逻辑（参数解析、管道构建、SIGINT handler、`gst_macos_main` 包装）不变
    - _需求：3.1, 3.2, 5.1, 5.3, 8.1, 8.2_
  - [x] 6.2 编译验证
    - 执行 `cmake --build device/build`
    - 确认编译无错误
    - 执行 `ctest --test-dir device/build --output-on-failure` 确认所有测试通过
    - _需求：10.1, 10.3_

- [x] 7. 最终检查点 — 全量验证
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认约 45 个测试全部通过（8 smoke + 7 log + 8 tee + 10 camera + ~12 health）
  - 确认 ASan 无内存错误报告
  - 确认现有测试（smoke_test、log_test、tee_test、camera_test）行为不变
  - Pi 5 Release 验证（`scripts/pi-build.sh` 或手动 SSH）需要 Pi 5 可达，不可达则标注跳过
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：10.1, 10.2, 10.3, 10.4_

## 备注

- 新建文件：`device/src/pipeline_health.h`、`device/src/pipeline_health.cpp`、`device/tests/health_test.cpp`
- 修改文件：`device/CMakeLists.txt`、`device/src/main.cpp`
- 不修改文件：`device/src/pipeline_manager.h`、`device/src/pipeline_manager.cpp`、`device/src/pipeline_builder.h`、`device/src/pipeline_builder.cpp`、`device/src/camera_source.h`、`device/src/camera_source.cpp`、`device/tests/smoke_test.cpp`、`device/tests/log_test.cpp`、`device/tests/tee_test.cpp`、`device/tests/camera_test.cpp`
- `health_test.cpp` 需要自定义 `main()`（`gst_init` + GMainLoop），链接 `GTest::gtest` + `rapidcheck` + `rapidcheck_gtest`
- PBT 使用毫秒级配置参数（`initial_backoff_ms = 1~100`），确保 100+ 迭代在 5 秒内完成
- monitor 接管 bus watch 后，main.cpp 中原有的 `bus_callback` 不再需要
- 回调在 mutex 外调用，避免回调中操作 monitor 导致死锁
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
