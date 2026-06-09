# Implementation Plan

> Spec 32: 管道韧性加固 — 任务清单

## Overview

按"纯函数（宿主可测）→ 核心机制 → 集成 → Pi 实测"顺序拆为 7 个可独立验证的 task。
统一验证：`ctest --test-dir device/build --output-on-failure`；Pi 5 用 `scripts/pi-build.sh`。
Task 1/2/3 相互独立可并行；Task 4 依赖 1+3；Task 5 依赖 3+4；Task 6 依赖 5；Task 7 依赖全部且必须在 Pi 5 上跑。

## Tasks

- [x] 1. 纯函数 + 单测（无 GStreamer 依赖，宿主机可测）
  - 在 `pipeline_health.h` 定义 `enum class ErrorScope { KVS_BRANCH, WEBRTC_BRANCH, TRUNK }`，声明并在 `.cpp` 实现 `classify_bus_error(const std::string&)`：KVS 分支名（`q-kvs`/`kvs-parser`/`avc-caps`/`kvs-sink`）→KVS_BRANCH，WebRTC（`q-web`/`webrtc-sink`）→WEBRTC_BRANCH，其余（含 `v4l2-source`/空串/未知）→TRUNK
  - 实现 `should_trigger_recovery(int consecutive_non_playing, int threshold)`（`c>=t && t>0`）
  - 在 `camera_source.h` 定义 `struct OpenRetryConfig{int max_attempts=6; int interval_ms=500;}`，实现 `open_with_retry(try_open, cfg, sleep_fn={}, out_attempts=nullptr)`（可注入假 sleep）
  - 调整 `StreamingConfig` 默认值：`bitrate_min_kbps=800`、`bitrate_max_kbps=1500`、`bitrate_default_kbps=1200`；确认 `validate_streaming_config` 通过
  - 新增/扩展单测覆盖 Property 1-5（classify 全覆盖+TRUNK 保守、should_trigger 阈值、open_with_retry 次数有界、validate 区间），example + RapidCheck PBT
  - _Requirements: 1.5, 1.6, 2.1, 2.4, 3.3, 3.5, 6.1, 6.3_

- [x] 2. kvssink 自愈 + buffer-duration 下调
  - `kvs_sink_factory.cpp`：将 `restart-on-error` 由 `FALSE` 改为 `TRUE`
  - `StreamingConfig.buffer_duration_sec` 与 `KvsSinkConfig.buffer_duration_sec` 默认 180 → 40；`to_kvssink_config` 跟随
  - `config.toml.example` 同步注释与默认值（码率 + buffer_duration），保留可覆盖说明
  - 验证：config 解析单测确认新默认值；`to_kvssink_config` 单测 `avg-bandwidth-bps=default*1000`
  - _Requirements: 1.1, 5.2, 6.1, 6.2_

- [x] 3. PipelineManager::release() + 健康解绑 detach() + 有界异步 teardown helper（核心机制 + 单测）
  - `pipeline_manager.{h,cpp}`：新增 `GstElement* release()`——放弃所有权返回裸指针、内部置 nullptr，不 stop/unref；析构对空指针安全
  - `pipeline_health.{h,cpp}`：新增 `void detach()`——`remove_probe()` + 移除 bus watch + `pipeline_=nullptr`，幂等（决策 C）
  - 实现有界异步 teardown 的**两个 helper**（均 worker 线程同步 set_state(NULL)、主循环用 condition_variable 等 `state_reset_timeout_ms`(5s)、超时 detach 后台完成；参考 `shutdown_handler.cpp` 的 thread+cv+detach 模式）：
    - `teardown_pipeline_bounded(GstElement* /*转移所有权*/, int)`——worker 完成 NULL 后 `unref`（供 full_rebuild 销毁旧管道）
    - `set_null_bounded(GstElement* /*不转移所有权*/, int)`——worker 仅 set NULL 不 unref（供 try_state_reset 复用尝试）
  - `HealthConfig` 新增 `heartbeat_fail_threshold=3`、`state_reset_timeout_ms=5000`
  - 单测：`release()` 后壳为空且原指针有效；teardown helper 注入快/慢 set_null fn——快→预算内返回 true，慢→超时返回 false 且主线程耗时≈预算（不被慢操作阻塞）
  - _Requirements: 2.3, 5.1_

- [x] 4. pipeline_health 集成：错误按域分流 + heartbeat 去抖主动恢复 + try_state_reset 重写
  - `bus_watch_cb` 的 `GST_MESSAGE_ERROR`：调用 `classify_bus_error`，TRUNK→`attempt_recovery()`；KVS_BRANCH/WEBRTC_BRANCH→仅 warn 日志 + `branch_error_count_++`，不恢复
  - `heartbeat_timer_cb`：检查 `gst_element_get_state` 返回值，`GST_STATE_CHANGE_ASYNC`→跳过；PLAYING→`consecutive_non_playing_=0`；非 PLAYING→`++`；达 `heartbeat_fail_threshold` 即清零并 `attempt_recovery()`（消除"标记 ERROR 后无限等待"）
  - `try_state_reset` 重写：用 task 3 的有界 teardown——worker 做 `set_state(NULL)` 主循环等 ≤5s；预算内完成→`set_state(PLAYING)` 复用；超时→半 NULL 化管道交后台 teardown、返回 false→走 full_rebuild
  - 记录 state reset / teardown 实际耗时 info 日志（覆盖需求 5.4）
  - 单测：去抖序列（连续 N 次触发、中途 PLAYING 清零、ASYNC 不计）；bus 分流（注入不同 src 名，用可注入 recovery hook 计数验证是否触发）
  - _Requirements: 1.2, 1.3, 1.5, 3.1, 3.2, 3.4, 3.5, 5.1, 5.3, 5.4_

- [x] 5. app_context rebuild 改造 + FATAL 优雅退出回调
  - rebuild 回调按决策 B/C 顺序：`ai_handler_->stop()` → `health_monitor->detach()` → `pipeline_manager->release()` 取旧裸指针 + `reset()` → 有界异步 teardown 旧管道 → `open_with_retry` 构建+start 新管道（暂存 `pending_pm_` 成功转正）→ 更新 stream_controller/bitrate_adapter/media_manager 引用 → 返回新 pipeline
  - `app_context.h`：新增 `set_shutdown_requester(std::function<void()>)` 与 `bool is_healthy()`（转发 `health_monitor->state()!=FATAL`，空时 true）
  - health 回调：`new_state==FATAL` 时调 `shutdown_requester()`（仅设 flag，不在回调里 quit）
  - 日志记录每次重试（warn）与最终结果（覆盖需求 2.5）
  - 验证：编译通过；宿主机 stub/videotestsrc 跑现有 app/集成测试不回归
  - _Requirements: 2.2, 2.3, 2.5, 4.1, 4.2_

- [x] 6. sd_notifier 健康门控 + main 退出码
  - `sd_notifier.{h,cpp}`：新增 `set_health_check(std::function<bool()>)`（`s_health_mtx` 保护）；心跳发送前查询，非健康（含未注册默认 true）则跳过 `notify_watchdog()`
  - `main.cpp`：新增 `std::atomic<bool> g_fatal`；注册 `set_shutdown_requester([]{ g_fatal=true; g_shutdown_requested=true; })` 与 `SdNotifier::set_health_check([&]{ return ctx.is_healthy(); })`；主循环退出后 `return g_fatal ? EXIT_FAILURE : 0`
  - 单测：看门狗门控（注入 health_check + 计数 notify_watchdog：healthy→发送、FATAL→不发送）；FATAL→shutdown_requester 被调用（注入回调，不真退出）
  - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5_

- [ ] 7. CPU 诊断脚本 + Pi 5 集成验证 + trace 归档
  - 新增 `scripts/diagnose-cpu.sh`：采集 `raspi-eye` 进程/各线程 CPU、load average，并 grep `Heartbeat: PAUSED`，运行 ≥10min（需求 7）
  - Pi 5 实测**决策 A 关键不变量**：`GST_DEBUG=GST_STATES:4` 下注入 KVS 网络中断（断网几十秒），确认 `restart-on-error=TRUE` 重连期间管道聚合状态保持 PLAYING、无整管道恢复、WebRTC 不中断；若证伪则启用决策 A 兜底（KVS 分支封 GstBin 状态隔离）并记录
  - Pi 5 验证：制造 TRUNK 故障触发 recovery，确认单次主循环阻塞 ≤5s、摄像头无 `Could not open device` 致命、ERROR→RECOVERING 间隔有限；制造 FATAL 确认 `systemctl` 自动重启；CPU 基线判定 rotation 是否诱发 PAUSED
  - 结论写 `docs/development-trace.md`（决策 A 不变量结论、CPU 归因、回归数据）；据 CPU 结论更新 `docs/spec-backlog.md`；反哺 `.kiro/steering/shall-not.md`（如"set_state(NULL) 同步阻塞不可用 get_state 超时封顶"）
  - _Requirements: 1.1, 1.4, 2.1, 3.4, 4.1, 5.1, 7.1, 7.2, 7.3, 7.4_

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": ["1", "2", "3"], "note": "纯函数/kvssink/核心机制，相互独立可并行" },
    { "wave": 2, "tasks": ["4"], "note": "health 集成，依赖 1(classify/should_trigger)+3(teardown/detach)" },
    { "wave": 3, "tasks": ["5"], "note": "app_context rebuild 改造，依赖 3+4" },
    { "wave": 4, "tasks": ["6"], "note": "sd_notifier/main，依赖 5 的 is_healthy/shutdown_requester" },
    { "wave": 5, "tasks": ["7"], "note": "Pi 实测+归档，依赖全部" }
  ],
  "dependencies": {
    "1": [],
    "2": [],
    "3": [],
    "4": ["1", "3"],
    "5": ["3", "4"],
    "6": ["5"],
    "7": ["1", "2", "3", "4", "5", "6"]
  }
}
```

## Notes

- Task 1/2/3 无相互依赖，可并行先做。
- Task 4 依赖 Task 1（classify/should_trigger）+ Task 3（teardown/detach）。
- Task 5 依赖 Task 3、4；Task 6 依赖 Task 5（is_healthy/shutdown_requester 接口）。
- Task 7 依赖全部，且必须在 Pi 5 上跑——其中"决策 A 关键不变量实测"是本 spec 唯一能确认主方案成立的环节，若证伪需走兜底（GstBin 隔离）。
- 不自动 commit：每个 task 完成 `git status` 确认无敏感文件，由用户测试后提交。
- 性能门禁：恢复路径单次主循环阻塞 ≤5s（回归对比，从 ~130s 降下）。
