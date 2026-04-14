# 实施计划：Spec 26 — 网络自适应码率控制

## 概述

在 Spec 15（adaptive-streaming）已建立的 BitrateAdapter + StreamModeController 架构基础上，补齐网络感知信号通路。按依赖顺序实现：StreamingConfig 扩展（纯数据 + 纯函数）→ NetworkMonitor 纯函数 + 模块 → BandwidthProbe 纯函数 + 模块 → KvsSinkFactory 扩展 → WebRtcMediaManager 扩展 → app_context 集成。纯函数优先实现，便于 PBT 测试覆盖 8 个正确性属性。实现语言为 C++17。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 使用 `spdlog::get("pipeline")` logger，NetworkMonitor 使用 `spdlog::get("network")` logger
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在 `on_latency_pressure()` 中执行阻塞操作（该函数从 KVS SDK 内部线程调用）
- SHALL NOT 在 force-keyunit 中修改编码器的 key-int-max 设置（仅插入额外 IDR）
- SHALL NOT 硬编码带宽估算参数（所有参数从 StreamingConfig 获取）
- SHALL NOT 在子代理的最终检查点任务中执行 git commit
- SHALL NOT 在测试中用假凭证数据调用可能创建真实 kvssink 的函数
- SHALL NOT 对含 `std::atomic` 成员的结构体使用 `unordered_map::emplace` 或 `insert`（改用 `try_emplace`）

## 任务

- [x] 1. StreamingConfig 扩展与配置解析
  - [x] 1.1 扩展 `device/src/config_manager.h` 中的 `StreamingConfig` 结构体
    - 新增 6 个字段：`buffer_duration_sec`(180)、`latency_pressure_threshold`(5)、`latency_pressure_cooldown_sec`(30)、`bandwidth_probe_enabled`(true)、`bandwidth_probe_duration_sec`(10)、`writeframe_fail_threshold`(10)
    - 新增 `KvsSinkConfig` 结构体（`avg_bandwidth_bps`, `buffer_duration_sec`）
    - 新增 `NetworkConfig` 结构体声明（或在 network_monitor.h 中声明，config_manager.h 中声明转换函数）
    - 声明纯函数 `to_network_config(const StreamingConfig&)`、`to_probe_config(const StreamingConfig&)`、`to_kvssink_config(const StreamingConfig&, const BitrateConfig&)`
    - _需求：5.1, 5.2_

  - [x] 1.2 扩展 `device/src/config_manager.cpp` 中的 `parse_streaming_config()`
    - 解析 6 个新字段，缺失时使用默认值
    - 无效值（负数、零）返回 false 并填充 error_msg
    - `bandwidth_probe_enabled` 使用 `parse_bool_field()` 解析
    - 实现 `to_network_config()`、`to_probe_config()`、`to_kvssink_config()` 纯函数
    - _需求：5.1, 5.2, 5.3_

  - [x] 1.3 扩展 `device/tests/config_test.cpp` 中的配置测试
    - Example-based：空 map 默认值验证（6 个新字段）、完整 kv map 解析验证、无效值拒绝验证
    - _需求：5.1, 5.2, 5.3_

  - [x] 1.4 PBT — 属性 4：配置解析 round-trip
    - **属性 4：配置解析 round-trip**
    - **验证：需求 5.1, 5.2**
    - 生成随机有效 StreamingConfig（含 6 个新字段），序列化为 kv map 后再解析，验证等价
    - 标签：`Feature: network-adaptive-bitrate, Property 4: Config parse round-trip`

  - [x] 1.5 PBT — 属性 5：无效配置拒绝
    - **属性 5：无效配置拒绝**
    - **验证：需求 5.3**
    - 生成包含无效值（负数、零）的 kv map，验证 `parse_streaming_config()` 返回 false 且 error_msg 非空
    - 标签：`Feature: network-adaptive-bitrate, Property 5: Invalid config rejected`

  - [x] 1.6 PBT — 属性 8：kvssink 属性初始化
    - **属性 8：kvssink 属性初始化**
    - **验证：需求 2.1, 2.2**
    - 生成随机有效 StreamingConfig 和 BitrateConfig，验证 `to_kvssink_config()` 满足：`avg_bandwidth_bps == default_kbps × 1000`，`buffer_duration_sec == StreamingConfig.buffer_duration_sec`
    - 标签：`Feature: network-adaptive-bitrate, Property 8: KvsSink config initialization`

- [x] 2. NetworkMonitor 纯函数与模块实现
  - [x] 2.1 创建 `device/src/network_monitor.h`
    - 声明 `NetworkConfig` 结构体（`latency_pressure_threshold`, `latency_pressure_cooldown_sec`, `writeframe_fail_threshold`, `writeframe_recovery_count`）
    - 声明纯函数 `compute_kvs_network_status(int pressure_count, int threshold, bool cooldown_expired) -> BranchStatus`
    - 声明纯函数 `compute_webrtc_network_status(int consecutive_failures, int consecutive_successes, int fail_threshold, int recovery_count) -> BranchStatus`
    - 声明纯函数 `compute_keyframe_only_transition(bool current_keyframe_only, int consecutive_failures, int consecutive_successes, int fail_threshold, int recovery_in_keyframe_mode) -> bool`（用于属性 6 PBT）
    - 声明 `NetworkMonitor` 类（pImpl 模式）：`on_latency_pressure(uint64_t buffer_duration_ms)`、`on_writeframe_result(bool success)`、`set_bitrate_adapter(BitrateAdapter*)`、`set_stream_mode_controller(StreamModeController*)`、`start()`、`stop()`、`kvs_network_status() const`、`webrtc_network_status() const`
    - _需求：1.1, 1.2, 1.3, 1.4, 3.1, 3.2, 6.4, 7.1, 7.2_

  - [x] 2.2 创建 `device/src/network_monitor.cpp`
    - `Impl` 结构体：`std::deque<time_point> pressure_timestamps`（10 秒滑动窗口）、`last_pressure_time`、`consecutive_failures`、`consecutive_successes`、`kvs_status`、`webrtc_status`、`BitrateAdapter*`、`StreamModeController*`、`NetworkConfig`、`std::mutex`
    - `on_latency_pressure()`：mutex lock → 记录时间戳 → 清理过期条目 → 调用 `compute_kvs_network_status()` → 状态变化时调用 `report_kvs_health()` → warn 日志（使用 "network" logger）
    - `on_writeframe_result()`：更新连续计数 → 调用 `compute_webrtc_network_status()` → 状态变化时调用 `report_webrtc_status()`
    - 内部定时器检查 cooldown 过期，恢复 HEALTHY
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5, 3.1, 3.2, 3.3, 6.1, 6.3, 6.4_

  - [x] 2.3 创建 `device/tests/network_monitor_test.cpp`
    - 自定义 `main()`：`gst_init` + `RUN_ALL_TESTS()`
    - Example-based：`compute_kvs_network_status` 基本场景、`compute_webrtc_network_status` 基本场景、`compute_keyframe_only_transition` 基本场景、NetworkMonitor 集成测试（with nullptr adapter/controller，验证不崩溃）
    - _需求：1.1, 1.2, 1.3, 3.1, 3.2, 7.1, 7.2_

  - [x] 2.4 PBT — 属性 1：Latency pressure 状态机不变量
    - **属性 1：Latency pressure 状态机不变量**
    - **验证：需求 1.1, 1.2, 1.3**
    - 生成随机 `(pressure_count, threshold, cooldown_expired)` 组合，验证 `compute_kvs_network_status()` 满足：count ≥ threshold → UNHEALTHY；count < threshold 且 cooldown 过期 → HEALTHY；count < threshold 且 cooldown 未过期 → UNHEALTHY
    - 标签：`Feature: network-adaptive-bitrate, Property 1: Latency pressure state machine invariant`

  - [x] 2.5 PBT — 属性 2：writeFrame 健康状态转换
    - **属性 2：writeFrame 健康状态转换**
    - **验证：需求 3.1, 3.2**
    - 生成随机 `(consecutive_failures, consecutive_successes, fail_threshold, recovery_count)` 组合，验证 `compute_webrtc_network_status()` 满足：failures ≥ threshold → UNHEALTHY；successes ≥ recovery → HEALTHY
    - 标签：`Feature: network-adaptive-bitrate, Property 2: WriteFrame health state transition`

  - [x] 2.6 PBT — 属性 6：仅关键帧模式状态转换
    - **属性 6：仅关键帧模式状态转换**
    - **验证：需求 7.1, 7.2**
    - 生成随机 per-peer 帧序列（is_keyframe + writeFrame 成功/失败），验证 `compute_keyframe_only_transition()` 满足：连续失败 ≥ threshold → 进入仅关键帧模式；仅关键帧模式下连续成功 ≥ 10 → 恢复正常
    - 标签：`Feature: network-adaptive-bitrate, Property 6: Keyframe-only mode state transition`

  - [x] 2.7 PBT — 属性 7：多 peer 仅关键帧模式独立性
    - **属性 7：多 peer 仅关键帧模式独立性**
    - **验证：需求 7.4**
    - 生成 2-5 个 peer 的独立帧序列，验证每个 peer 的仅关键帧模式状态独立维护：一个 peer 进入仅关键帧模式不影响其他 peer
    - 标签：`Feature: network-adaptive-bitrate, Property 7: Multi-peer keyframe-only independence`

- [x] 3. BandwidthProbe 纯函数与模块实现 + CMake 更新 + 编译检查点
  - [x] 3.1 创建 `device/src/bandwidth_probe.h` 和 `device/src/bandwidth_probe.cpp`
    - 声明并实现纯函数 `compute_initial_bitrate(int estimated_bandwidth_kbps, const BitrateConfig& config) -> int`：取估算带宽 80% → 向下取整到 step 档位 → clamp 到 [min, max]
    - 声明 `BandwidthProbe` 类（pImpl 模式）：`ProbeConfig`、`start_probe(GstElement*, BitrateAdapter*, const BitrateConfig&)`、`estimated_bandwidth_kbps() const`、`probe_completed() const`
    - 探测机制：pipeline 启动后统计 kvssink 字节计数，计算上传速率
    - 探测失败（fakesink、pipeline 崩溃、禁用）时使用 default_kbps
    - info 日志输出估算带宽和选定初始码率
    - _需求：4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 6.2_

  - [x] 3.2 修改 `device/CMakeLists.txt`
    - 添加 `network_module` 静态库（`src/network_monitor.cpp`），链接 `stream_mode_module`、`bitrate_module`、`spdlog::spdlog`
    - 添加 `bandwidth_probe_module` 静态库（`src/bandwidth_probe.cpp`），链接 `bitrate_module`、`${GST_LIBRARIES}`、`spdlog::spdlog`
    - 链接 `network_module` 和 `bandwidth_probe_module` 到 `pipeline_manager`
    - 添加 `network_monitor_test` 测试目标，链接 `network_module`、`GTest::gtest`、`rapidcheck`、`rapidcheck_gtest`
    - 更新 `bitrate_test` 链接 `bandwidth_probe_module`（用于 `compute_initial_bitrate` PBT）
    - 更新 `config_test` 链接（确保能访问新增转换函数）
    - `add_test(NAME network_monitor_test COMMAND network_monitor_test)`
    - _需求：全部（构建基础设施）_

  - [x] 3.3 编译检查点
    - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
    - 执行 `ctest --test-dir device/build --output-on-failure` 确认所有测试通过
    - 确认 ASan 无内存错误报告
    - 如有问题，询问用户

  - [x] 3.4 PBT — 属性 3：带宽探测码率计算
    - **属性 3：带宽探测码率计算**
    - **验证：需求 4.2, 4.3, 4.4**
    - 在 `device/tests/bitrate_test.cpp` 中新增
    - 生成随机估算带宽（0~100000 kbps）和有效 BitrateConfig，验证 `compute_initial_bitrate()` 满足：结果 ∈ [min, max]；结果 ≤ estimated × 0.8（除非 estimated × 0.8 < min）；(结果 - min) % step == 0
    - 标签：`Feature: network-adaptive-bitrate, Property 3: Bandwidth probe bitrate calculation`

- [x] 4. KvsSinkFactory 扩展 + WebRtcMediaManager 扩展
  - [x] 4.1 扩展 `device/src/kvs_sink_factory.h` 和 `device/src/kvs_sink_factory.cpp`
    - 新增 `KvsSinkConfig` 结构体（如果在 config_manager.h 中声明则此处 include）
    - `create_kvs_sink()` 签名新增可选参数 `const KvsSinkConfig* sink_config = nullptr`
    - Linux kvssink 创建后：检查属性存在性 → `g_object_set` 设置 `avg-bandwidth-bps` 和 `buffer-duration`
    - info 日志输出实际设置值
    - _需求：2.1, 2.2, 2.4_

  - [x] 4.2 扩展 `device/src/webrtc_media.h` 和 `device/src/webrtc_media.cpp`
    - 新增 `set_pipeline(GstElement*)` 方法（持有 pipeline 引用，用于 force-keyunit）
    - 新增 `set_writeframe_fail_threshold(int)` 方法（配置仅关键帧模式阈值）
    - `PeerInfo` 新增 `keyframe_only_mode`、`keyframe_mode_success_count` 字段
    - `broadcast_frame()` 中实现 per-peer 仅关键帧模式逻辑：连续失败 ≥ threshold → 进入仅关键帧模式（跳过非关键帧）；仅关键帧模式下连续成功 ≥ 10 → 恢复正常
    - `on_connection_state_change` 回调中 CONNECTED 时发送 `gst_video_event_new_upstream_force_key_unit`（需要 pipeline 引用）
    - pipeline 引用不可用时跳过 force-keyunit 并输出 warn 日志
    - 模式切换时输出 info 日志，帧跳过时不逐帧输出
    - _需求：7.1, 7.2, 7.3, 7.4, 9.1, 9.2, 9.3, 9.4, 9.5_

  - [x] 4.3 扩展现有测试验证
    - `device/tests/kvs_test.cpp`：新增 `KvsSinkConfig` 属性设置测试（fakesink 场景验证不崩溃）
    - `device/tests/webrtc_media_test.cpp`：新增 `set_pipeline` 和 force-keyunit 相关测试（stub 场景）
    - _需求：2.1, 2.2, 9.1, 9.4_

- [x] 5. app_context 集成与最终检查点
  - [x] 5.1 修改 `device/src/app_context.cpp`
    - 新增 `#include "network_monitor.h"` 和 `#include "bandwidth_probe.h"`
    - `Impl` 新增成员：`std::unique_ptr<NetworkMonitor> network_monitor`、`std::unique_ptr<BandwidthProbe> bandwidth_probe`
    - 在 `start()` 中：
      1. 从 StreamingConfig 转换 NetworkConfig、ProbeConfig、KvsSinkConfig
      2. 创建 NetworkMonitor，连接 BitrateAdapter 和 StreamModeController
      3. 创建 BandwidthProbe，启动探测
      4. 将 KvsSinkConfig 传递给 KvsSinkFactory（在 pipeline 构建时）
      5. 调用 `webrtc_media->set_pipeline(pipeline)` 和 `webrtc_media->set_writeframe_fail_threshold(threshold)`
      6. 启动 `network_monitor->start()`
    - 在 rebuild 回调中：更新 pipeline 引用
    - 在 shutdown 中：`network_monitor->stop()`
    - 更新 `config.toml.example` 添加 6 个新字段示例
    - _需求：1.1, 2.1, 3.1, 4.1, 5.1, 7.1, 9.1_

  - [x] 5.2 最终检查点 — 全量验证
    - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
    - 执行 `ctest --test-dir device/build --output-on-failure` 确认所有测试通过
    - 确认 ASan 无内存错误报告
    - 确认现有测试行为不变
    - SHALL NOT 在此检查点执行 git commit
    - 如有问题，询问用户

## 备注

- 新建文件：`device/src/network_monitor.h`、`device/src/network_monitor.cpp`、`device/src/bandwidth_probe.h`、`device/src/bandwidth_probe.cpp`、`device/tests/network_monitor_test.cpp`
- 修改文件：`device/src/config_manager.h`、`device/src/config_manager.cpp`、`device/src/kvs_sink_factory.h`、`device/src/kvs_sink_factory.cpp`、`device/src/webrtc_media.h`、`device/src/webrtc_media.cpp`、`device/src/app_context.cpp`、`device/CMakeLists.txt`、`device/tests/config_test.cpp`、`device/tests/bitrate_test.cpp`、`device/tests/kvs_test.cpp`、`device/tests/webrtc_media_test.cpp`、`device/config/config.toml.example`
- 纯函数提取：`compute_kvs_network_status()`、`compute_webrtc_network_status()`、`compute_initial_bitrate()`、`compute_keyframe_only_transition()`、`to_kvssink_config()` — 全部无副作用，便于 PBT
- PBT 覆盖全部 8 个正确性属性（属性 1-8），分布在 `network_monitor_test.cpp`（属性 1, 2, 6, 7）、`bitrate_test.cpp`（属性 3）、`config_test.cpp`（属性 4, 5, 8）
- 标记 `*` 的子任务为可选（PBT 属性测试），可跳过以加速 MVP
- NetworkMonitor 使用 `spdlog::get("network")` logger，不使用 `spdlog::get("pipeline")`
- macOS 上 kvssink 不可用时，NetworkMonitor 跳过 latency pressure 监听，BandwidthProbe 探测失败使用 default_kbps
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
