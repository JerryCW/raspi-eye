# 实施计划：Spec 15 — 自适应码率控制 + 流模式切换

## 概述

基于 requirements.md 和 design.md，实现两个新模块：**StreamModeController**（流模式状态机 + 分支数据流控制）和 **BitrateAdapter**（自适应码率控制器）。核心决策逻辑提取为纯函数（`compute_target_mode`、`compute_queue_params`、`compute_next_bitrate`）以支持 PBT。按依赖顺序：stream_mode_controller.h/cpp → bitrate_adapter.h/cpp → CMakeLists.txt 更新 → 编译检查点 → stream_mode_test.cpp → bitrate_test.cpp → app_context.cpp 集成 → 最终检查点。实现语言为 C++17。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符（GLib 输出函数在部分终端不正确处理 UTF-8）
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 GStreamer C API 外）
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、kvs_test、webrtc_test、webrtc_media_test、yolo_test）
- SHALL NOT 在子代理的最终检查点任务中执行 git commit
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在 StreamModeController 或 BitrateAdapter 中持有 GstElement* 的额外引用（每次操作通过 `gst_bin_get_by_name` 获取，用完 `gst_object_unref`）
- SHALL NOT 在 GStreamer 回调中持有 mutex 时调用用户回调（先复制回调和参数，释放 mutex 后再调用）
- SHALL NOT 在高频路径（buffer probe、每帧回调）中调用 `gst_bin_get_by_name`（仅在模式切换时调用）

## 任务

- [x] 1. StreamModeController 模块实现
  - [x] 1.1 创建 `device/src/stream_mode_controller.h`
    - 声明 `StreamMode` 枚举（FULL / KVS_ONLY / WEBRTC_ONLY / DEGRADED）
    - 声明 `BranchStatus` 枚举（HEALTHY / UNHEALTHY）
    - 声明 `ModeChangeCallback` 类型别名
    - 声明 `QueueParams` 和 `BranchQueueParams` 结构体（纯数据，用于 PBT）
    - 声明纯函数 `compute_target_mode(BranchStatus kvs, BranchStatus webrtc) -> StreamMode`
    - 声明纯函数 `compute_queue_params(StreamMode mode) -> BranchQueueParams`
    - 声明 `stream_mode_name(StreamMode) -> const char*` 辅助函数
    - 声明 `StreamModeController` 类（pImpl 模式）：`explicit StreamModeController(GstElement* pipeline)`、`report_kvs_status(BranchStatus)`、`report_webrtc_status(BranchStatus)`、`set_mode_change_callback(ModeChangeCallback)`、`current_mode() const`、`start()`、`stop()`、`set_pipeline(GstElement*)`
    - 禁用拷贝构造和拷贝赋值（`= delete`）
    - _需求：1.1, 1.2, 1.8, 1.9, 1.10, 2.5, 2.6, 7.4_

  - [x] 1.2 创建 `device/src/stream_mode_controller.cpp`
    - `Impl` 结构体：`current_mode_`（初始 FULL）、`kvs_confirmed_`/`webrtc_confirmed_`（初始 HEALTHY）、`pending_kvs_`/`pending_webrtc_` + 对应时间戳、`pipeline_`（不持有所有权）、`mutex_`、`mode_change_cb_`、`debounce_timer_id_`
    - 实现 `compute_target_mode` 纯函数：4 种 (BranchStatus, BranchStatus) 组合 → 唯一 StreamMode
    - 实现 `compute_queue_params` 纯函数：4 种 StreamMode → (QueueParams, QueueParams) 映射表
    - 实现 `stream_mode_name`：返回英文模式名（"FULL"/"KVS_ONLY"/"WEBRTC_ONLY"/"DEGRADED"）
    - `report_kvs_status` / `report_webrtc_status`：记录 pending 状态和时间戳，启动 3 秒防抖定时器（`g_timeout_add`）
    - 防抖定时器回调 `evaluate()`：检查 pending 状态是否持续 ≥ 3 秒，是则确认状态变化 → 调用 `compute_target_mode` → 如果目标模式不同则执行切换
    - `apply_mode(StreamMode new_mode)`：通过 `gst_bin_get_by_name` 获取 q-kvs 和 q-web → `g_object_set` 设置 queue 属性 → `gst_object_unref` → 记录 info 日志 → 在 mutex 外调用回调
    - `start()` / `stop()`：管理定时器生命周期
    - `set_pipeline()`：更新 pipeline 指针，重新应用当前模式的 queue 参数
    - 所有 `gst_bin_get_by_name` 返回 nullptr 时记录 warn 日志并跳过，不崩溃
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 1.10, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 7.1, 7.3, 7.4_

- [x] 2. BitrateAdapter 模块实现
  - [x] 2.1 创建 `device/src/bitrate_adapter.h`
    - 声明 `BitrateConfig` 结构体（min_kbps=1000, max_kbps=4000, step_kbps=500, default_kbps=2500, eval_interval_sec=5, rampup_interval_sec=30）
    - 声明纯函数 `compute_next_bitrate(int current_kbps, BranchStatus kvs_status, bool rampup_eligible, const BitrateConfig& config) -> int`
    - 声明 `BitrateAdapter` 类（pImpl 模式）：`explicit BitrateAdapter(GstElement* pipeline, const BitrateConfig& config = BitrateConfig{})`、`on_mode_changed(StreamMode old_mode, StreamMode new_mode)`、`report_kvs_health(BranchStatus)`、`current_bitrate_kbps() const`、`start()`、`stop()`、`set_pipeline(GstElement*)`
    - 禁用拷贝构造和拷贝赋值（`= delete`）
    - _需求：3.1, 3.6, 3.7, 3.8, 6.1, 6.2, 6.3, 7.5_

  - [x] 2.2 创建 `device/src/bitrate_adapter.cpp`
    - `Impl` 结构体：`current_bitrate_kbps_`（初始 default_kbps）、`current_mode_`（初始 FULL）、`kvs_status_`（初始 HEALTHY）、`last_kvs_healthy_time_`、`pipeline_`、`config_`、`eval_timer_id_`、`mutex_`
    - 实现 `compute_next_bitrate` 纯函数：
      - DEGRADED 模式 → 返回 min_kbps
      - KVS UNHEALTHY → max(current - step, min)
      - KVS HEALTHY 且 rampup_eligible → min(current + step, max)
      - 否则不变
    - `on_mode_changed`：更新 current_mode_，DEGRADED 时立即设码率为 min_kbps
    - `report_kvs_health`：更新 kvs_status_，HEALTHY 时记录时间戳，UNHEALTHY 时触发降档
    - 评估定时器（`g_timeout_add`，每 eval_interval_sec 秒）：检查 rampup 条件（KVS HEALTHY 持续 ≥ rampup_interval_sec），调用 `compute_next_bitrate`，如果码率变化则 apply
    - `apply_bitrate(int new_kbps)`：`gst_bin_get_by_name(pipeline, "encoder")` → `g_object_set(encoder, "bitrate", new_kbps, nullptr)` → `gst_object_unref`；尝试 `gst_bin_get_by_name(pipeline, "kvs-sink")` → 检查 `avg-bandwidth-bps` 属性是否存在 → 存在则 `g_object_set`，不存在则 info 日志 → `gst_object_unref`
    - `start()` / `stop()`：管理评估定时器
    - `set_pipeline()`：更新 pipeline 指针，重新设置当前码率
    - 所有 element 获取失败时 warn 日志并跳过
    - _需求：3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8, 6.1, 6.2, 6.3, 7.2, 7.5_

- [x] 3. CMakeLists.txt 更新与编译检查点
  - [x] 3.1 修改 `device/CMakeLists.txt`
    - 添加 `stream_mode_module` 静态库（`src/stream_mode_controller.cpp`），链接 `${GST_LIBRARIES}` + `spdlog::spdlog`，include `${GST_INCLUDE_DIRS}`
    - 添加 `bitrate_module` 静态库（`src/bitrate_adapter.cpp`），链接 `stream_mode_module`（复用 StreamMode/BranchStatus 枚举）+ `${GST_LIBRARIES}` + `spdlog::spdlog`，include `${GST_INCLUDE_DIRS}`
    - `pipeline_manager` 链接 `stream_mode_module` 和 `bitrate_module`
    - 添加 `stream_mode_test` 测试目标（`tests/stream_mode_test.cpp`），链接 `stream_mode_module`、`pipeline_manager`、`GTest::gtest`、`rapidcheck`、`rapidcheck_gtest`
    - 添加 `bitrate_test` 测试目标（`tests/bitrate_test.cpp`），链接 `bitrate_module`、`stream_mode_module`、`pipeline_manager`、`GTest::gtest`、`rapidcheck`、`rapidcheck_gtest`
    - `add_test(NAME stream_mode_test COMMAND stream_mode_test)`
    - `add_test(NAME bitrate_test COMMAND bitrate_test)`
    - 不修改现有库和测试目标的定义
    - _需求：全部（构建基础设施）_

  - [x] 3.2 编译检查点
    - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug`
    - 执行 `cmake --build device/build`
    - 确认编译无错误
    - 执行 `ctest --test-dir device/build --output-on-failure` 确认现有测试全部通过
    - 确认 ASan 无内存错误报告
    - 如有问题，询问用户

- [x] 4. StreamModeController 测试
  - [x] 4.1 创建 `device/tests/stream_mode_test.cpp`
    - 自定义 `main()`：调用 `gst_init(&argc, &argv)` 后 `RUN_ALL_TESTS()`
    - 包含 `stream_mode_controller.h`、`gtest/gtest.h`、`rapidcheck.h`、`rapidcheck/gtest.h`、`gst/gst.h`
    - Example-based 测试：
      1. `ComputeTargetMode_AllCombinations`：验证 4 种 (BranchStatus, BranchStatus) 组合返回正确 StreamMode — _需求：1.3, 1.4, 1.5, 1.6_
      2. `ComputeQueueParams_AllModes`：验证 4 种 StreamMode 返回正确 queue 参数 — _需求：2.1, 2.2, 2.3, 2.4_
      3. `InitialMode_IsFull`：StreamModeController 初始化后 `current_mode() == FULL` — _需求：1.2_
      4. `ModeChangedCallback_Invoked`：模式切换时回调被调用，参数包含旧模式、新模式、原因字符串 — _需求：1.8_
      5. `NullPipeline_NoQueueCrash`：pipeline 为 nullptr 时 report_kvs_status 不崩溃 — _需求：错误处理_
    - _需求：1.2, 1.3, 1.4, 1.5, 1.6, 1.8, 2.1, 2.2, 2.3, 2.4_

  - [x] 4.2 PBT — Property 1: 模式决策正确性
    - **Property 1: 模式决策正确性**
    - **验证：需求 1.3, 1.4, 1.5, 1.6**
    - 生成随机 `(BranchStatus, BranchStatus)` 组合，验证 `compute_target_mode` 返回值与预定义映射一致
    - RapidCheck 配置：≥ 100 次迭代
    - 标签：`Feature: spec-15-adaptive-streaming, Property 1: Mode decision correctness`

  - [x] 4.3 PBT — Property 3: Queue 参数映射正确性
    - **Property 3: Queue 参数映射正确性**
    - **验证：需求 2.1, 2.2, 2.3, 2.4**
    - 生成随机 `StreamMode`，验证 `compute_queue_params` 返回值与预定义映射表一致
    - RapidCheck 配置：≥ 100 次迭代
    - 标签：`Feature: spec-15-adaptive-streaming, Property 3: Queue params mapping correctness`

- [x] 5. BitrateAdapter 测试
  - [x] 5.1 创建 `device/tests/bitrate_test.cpp`
    - 自定义 `main()`：调用 `gst_init(&argc, &argv)` 后 `RUN_ALL_TESTS()`
    - 包含 `bitrate_adapter.h`、`stream_mode_controller.h`（复用枚举）、`gtest/gtest.h`、`rapidcheck.h`、`rapidcheck/gtest.h`、`gst/gst.h`
    - Example-based 测试：
      1. `ComputeNextBitrate_UnhealthyDecreases`：UNHEALTHY 时码率降一档 — _需求：3.2_
      2. `ComputeNextBitrate_HealthyRampup`：HEALTHY + rampup_eligible 时码率升一档 — _需求：3.3_
      3. `ComputeNextBitrate_DegradedForcesMin`：DEGRADED 模式强制最低码率（通过 on_mode_changed 触发后验证 current_bitrate_kbps） — _需求：3.4_
      4. `ComputeNextBitrate_ClampToRange`：码率不超出 [min, max] 范围 — _需求：3.1_
      5. `InitialBitrate_IsDefault`：BitrateAdapter 初始化后 `current_bitrate_kbps() == default_kbps` — _需求：7.5_
    - _需求：3.1, 3.2, 3.3, 3.4, 7.5_

  - [x] 5.2 PBT — Property 4: 码率范围不变量
    - **Property 4: 码率范围不变量与调整方向**
    - **验证：需求 3.1, 3.2, 3.3, 3.4**
    - 生成随机健康事件序列（HEALTHY/UNHEALTHY + rampup_eligible），从 default_kbps 开始连续调用 `compute_next_bitrate`
    - 每次验证：min ≤ bitrate ≤ max 且 (bitrate - min) % step == 0
    - UNHEALTHY 后 bitrate ≤ 调整前；HEALTHY + rampup 后 bitrate ≥ 调整前
    - RapidCheck 配置：≥ 100 次迭代
    - 标签：`Feature: spec-15-adaptive-streaming, Property 4: Bitrate range invariant`

- [x] 6. AppContext 集成
  - [x] 6.1 修改 `device/src/app_context.cpp`
    - 新增 `#include "stream_mode_controller.h"` 和 `#include "bitrate_adapter.h"`
    - `Impl` 新增成员：`std::unique_ptr<StreamModeController> stream_controller` 和 `std::unique_ptr<BitrateAdapter> bitrate_adapter`
    - 在 `start()` 中，health_monitor 创建之后：
      1. 创建 `StreamModeController`，传入 `pipeline_manager->pipeline()`
      2. 创建 `BitrateAdapter`，传入 `pipeline_manager->pipeline()`
      3. 注册 StreamModeController 的模式切换回调 → 调用 `bitrate_adapter->on_mode_changed`
      4. 启动 `stream_controller->start()` 和 `bitrate_adapter->start()`
    - 在 rebuild 回调中：调用 `stream_controller->set_pipeline(new_pipeline)` 和 `bitrate_adapter->set_pipeline(new_pipeline)`
    - 在 shutdown_handler 中注册 stop 步骤：`stream_controller->stop()` 和 `bitrate_adapter->stop()`
    - _需求：1.10, 3.5, 3.6, 3.7, 7.1, 7.2_

- [x] 7. 最终检查点 — 全量验证
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug`
  - 执行 `cmake --build device/build`
  - 执行 `ctest --test-dir device/build --output-on-failure` 确认所有测试通过（现有全部测试 + 新增 stream_mode_test + bitrate_test）
  - 确认 ASan 无内存错误报告
  - 确认现有测试行为不变
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户

## 备注

- 新建文件：`device/src/stream_mode_controller.h`、`device/src/stream_mode_controller.cpp`、`device/src/bitrate_adapter.h`、`device/src/bitrate_adapter.cpp`、`device/tests/stream_mode_test.cpp`、`device/tests/bitrate_test.cpp`
- 修改文件：`device/CMakeLists.txt`、`device/src/app_context.cpp`
- 不修改文件：所有现有测试文件、pipeline_health.h/cpp、webrtc_media.h/cpp、pipeline_builder.h/cpp
- 纯函数 `compute_target_mode`、`compute_queue_params`、`compute_next_bitrate` 从 Impl 中提取，既作为内部实现使用，也暴露给测试做 PBT
- macOS 上 pipeline 使用 videotestsrc + fakesink，queue element 可能不存在（`gst_bin_get_by_name` 返回 nullptr），测试中需处理此情况
- stream_mode_test 和 bitrate_test 使用自定义 main()（gst_init），不使用 GTest::gtest_main
- PBT 使用 RapidCheck，每个 Property 最少 100 次迭代
- 标记 `*` 的子任务为可选（PBT 属性测试），可跳过以加速 MVP
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
- KVS 健康检测（需求 4）和 WebRTC 健康检测（需求 5）的完整集成依赖运行时 bus message 和 peer_count 轮询，本 Spec 在 StreamModeController 中实现接口和内部逻辑，通过 `report_kvs_status` / `report_webrtc_status` 外部调用触发，实际的 bus message 监听和 peer_count 轮询在 app_context 集成时接入
