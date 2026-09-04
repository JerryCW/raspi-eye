# Bugfix Requirements Document

> Spec 32.5: 恢复流程死锁 + 看门狗失明修复（2026-09-03 生产事故）

## Introduction

2026-09-03 取证定案的生产事故：Pi 5 上 raspi-eye 服务在管道恢复流程中主线程（GLib 主循环线程）死锁，进程"假活"冻结 2 天 8 小时。触发源是 09-01 10:39 v4l2-source 的瞬时 USB 摄像头故障（"Internal data stream error"），恢复流程走到 "Attempting state reset recovery" 后再无任何日志。gdb 定案死锁环：主线程在 KVS stream free 路径（`KinesisVideoStream::free()` → … → `pthread_join`）**持有** KVS Producer client 锁去 join 一个正在等待**同一把锁**做 continuousRetry 重连的 GStreamer streaming 线程（q-kvs:src），经典 join-while-holding-lock，死锁环位于 KVS Producer C SDK 内部（堆栈存档 `/tmp/hang_stacks_20260903.txt`，trace 归档见 `docs/development-trace.md` 2026-09-03 条目）。

事故暴露两个精确缺陷，为本 spec 的修复对象：

- **缺陷 1（看门狗失明）**：`SdNotifier` 健康门控是"状态检查"（`is_healthy()` 仅判定 `state() != FATAL`）而非"活性检查"。死锁时状态机永远停在 RECOVERING（属于"健康"），看门狗线程独立存活、持续喂狗 2 天，systemd `WatchdogSec=30` 完全失明；SIGTERM 也无法退出（主循环已死，`check_shutdown` timer 不再运行），只能 SIGKILL。
- **缺陷 2（同步 KVS 拆除泄漏路径）**：spec-32 已交付 `set_null_bounded` / `teardown_pipeline_bounded`（worker 线程执行 set_state(NULL)、主循环封顶 5s），但 gdb 显示主线程**直接**处于 `KinesisVideoStream::free()`（经 `__pthread_once`/`std::call_once` 进入），不在任何 bounded helper 的 cv 等待栈里，且 bounded helper 的完成/超时日志一条都没出——存在一条未被 bounded helper 覆盖的同步路径让主线程直接触碰了 KVS stream free。修复前必须先完成代码路径取证（不允许凭猜测修复）。

佐证：同类触发（v4l2 Internal data stream error）在 08-30 前的多次事件走的是 FATAL 优雅退出路径（restart counter=18），本次却挂死——同一触发两种终态，说明 recovery 路径存在竞态分叉。

**已有机制边界（本 spec 不重复设计）**：spec-32 已交付 ErrorScope 分流、restart-on-error=TRUE、heartbeat 去抖主动恢复、set_null_bounded/teardown_pipeline_bounded、`PipelineManager::release()`/`detach()`、FATAL 优雅退出、`sd_notifier` FATAL 门控、码率 1200kbps；spec-20 已交付 systemd 集成（WatchdogSec=30、Restart=on-failure、15s 间隔喂狗线程）。本 spec 只做两件事：(a) 喂狗与主循环活性绑定（保留 FATAL 门控语义）；(b) 取证并封堵绕过有界异步 teardown 的同步 KVS 拆除路径。

## Bug Analysis

### Current Behavior (Defect)

**缺陷 1：看门狗健康门控对"主线程死锁"零覆盖**

1.1 WHEN 主线程（GLib 主循环线程）在恢复流程中死锁、主循环停止运转 THEN `SdNotifier` 看门狗线程（独立线程）照常每 15s 通过健康门控并发送 WATCHDOG=1，systemd 感知不到任何故障（本事故持续 2 天 8 小时）

1.2 WHEN 主线程死锁时 `PipelineHealthMonitor` 状态机停留在 RECOVERING（非 FATAL）THEN `AppContext::is_healthy()` 返回 true（仅判定 `state() != HealthState::FATAL`），看门狗门控永远开启——门控只反映状态机标记，不反映主循环真实活性

1.3 WHEN 主循环死锁后 systemd 发送 SIGTERM THEN 进程无法优雅退出（signal handler 只设 atomic flag，消费该 flag 的 `check_shutdown` timer 跑在已死的主循环上），systemd 最终只能 SIGKILL

**缺陷 2：恢复路径存在绕过有界异步 teardown 的同步 KVS 拆除**

1.4 WHEN 整管道恢复流程沿某条当前未定位的代码路径执行（可疑面：set_null_bounded 超时后 full_rebuild 分支中的同步 unref/dispose 链、构建失败时的 `gst_object_unref(p)`、半启动 PipelineManager 析构的 stop+unref、bus watch 中对 kvssink 的引用释放）THEN 主线程直接（同步）进入 kvssink 的 dispose/KVS stream free 链（`KinesisVideoStream::free()`），未经过 worker 线程 + 有界等待

1.5 WHEN 主线程在 KVS stream free 中持有 KVS Producer client 锁进入 `pthread_join`，而被 join 的 streaming 线程（q-kvs:src）恰在 continuousRetry 重连回调中等待同一把 client 锁 THEN 双向死锁形成，主循环永久冻结，"Attempting state reset recovery" 之后 set_null_bounded / teardown_pipeline_bounded 的完成或超时日志一条都不出现

1.6 WHEN 主循环死锁后 viewer 发起 WebRTC 连接 THEN 信令/ICE 全部成功（信令线程独立存活）但永远无视频帧，形成"看起来在线、实际全死"的最坏观测面

1.7 WHEN 同类触发（v4l2 Internal data stream error + 当天 KVS 连接 churn）发生 THEN 恢复流程的终态不确定：有时走 FATAL 优雅退出被 systemd 正常拉起（08-30 前 restart counter=18），有时死锁挂死——存在竞态分叉

### Expected Behavior (Correct)

**缺陷 1 修复：喂狗与 GLib 主循环活性绑定**

2.1 WHEN GLib 主循环正常运转 THEN 系统 SHALL 通过挂在主循环上的低频周期 timer 刷新一个原子 liveness 时间戳（liveness heartbeat），刷新动作本身开销可忽略（仅写一个 atomic）

2.2 WHEN 看门狗线程准备发送 WATCHDOG=1 THEN 系统 SHALL 先检查 liveness 时间戳新鲜度：时间戳陈旧（stale）超过阈值即跳过喂狗，使 systemd `WatchdogSec=30` 在有限时间内感知并 SIGKILL + `Restart=on-failure` 拉起干净进程

2.3 WHEN 健康状态为 FATAL THEN 看门狗 SHALL 按既有门控跳过喂狗（保留 spec-32 需求 4 的 FATAL 门控语义，liveness 检查与 FATAL 检查为"与"关系：两者都通过才喂狗）

2.4 WHEN 设定 liveness stale 阈值 THEN 阈值 SHALL 大于主循环的最大合法停顿，并给出与喂狗间隔 15s、`WatchdogSec=30`、`StartLimitBurst` 约束的量化关系，保证正常运行、合法恢复中、优雅退出三个阶段都不产生误杀导致的重启循环。注意最大合法停顿的约束对象不是单次 5s 封顶等待，而是**整个恢复关键路径**：`attempt_recovery` 在一次主循环回调内串行执行 set_null_bounded（≤5s）→ teardown_pipeline_bounded（≤5s）→ 摄像头重试（≤3s）× 多次 build+start，最坏合法阻塞可能远超 5s——design SHALL 在"阈值覆盖恢复关键路径最坏值"与"恢复流程内在检查点主动刷新 liveness 时间戳（恢复有进展即视为存活）"两种方案间做决策消解（决策点，倾向后者：阈值可保持小、检测窗口短，且死锁时检查点必然停止刷新）

2.5 WHEN 实现 liveness 新鲜度判定与门控组合逻辑 THEN 系统 SHALL 将其实现为纯函数/可注入时钟的形式（不直接依赖 systemd 与真实时钟），可被 GTest 单测与 PBT 覆盖

2.6 WHEN 主线程在任意位置死锁或永久阻塞导致主循环停摆（不限于本次 KVS 死锁）THEN 系统 SHALL 在"stale 阈值 + WatchdogSec"的有限时间窗内被 systemd 检测并重启——对该故障模式的覆盖不依赖状态机是否标记 FATAL

**缺陷 2 修复：取证定位并封堵同步 KVS 拆除路径**

2.7 WHEN 修复缺陷 2 之前 THEN 系统开发流程 SHALL 先完成代码路径取证：逐条审阅 `pipeline_health.cpp`（try_state_reset / attempt_recovery / try_full_rebuild）与 `app_context.cpp` rebuild 回调中的每一处 `gst_element_set_state` / `gst_object_unref` / 对象析构链，比对 gdb 堆栈存档，定位主线程直接进入 KVS stream free 的精确调用点，取证结论记录后再动代码（SHALL NOT 凭猜测修复）

2.8 WHEN 恢复流程中任何可能进入 kvssink change_state / dispose / KVS stream free 的调用（set_state(NULL)、unref、析构）需要执行 THEN 系统 SHALL 保证其只在 worker 线程发生，主循环线程只做有界等待（≤5s，超时后所有权移交后台线程完成），恢复路径上不残留任何同步触碰 KVS 拆除链的调用点

2.9 WHEN 有界 teardown 的 worker 启动、完成或超时 THEN 系统 SHALL 输出可辨识的日志（含实际耗时与所走分支），使同类事故再次发生时能从日志直接定位恢复流程走到了哪一步（本事故日志断在 "Attempting state reset recovery" 后无从判断）

2.10 WHEN 同类触发（v4l2 故障 + KVS 连接 churn）再次发生且恢复不可完成 THEN 系统 SHALL 收敛到确定性终态：要么恢复成功回 HEALTHY，要么在有限时间内走 FATAL 优雅退出交 systemd 重启，要么主循环停摆被 liveness 门控兜底重启——不存在"永久 RECOVERING 假活"的第四种终态

### Unchanged Behavior (Regression Prevention)

3.1 WHEN 健康状态非 FATAL 且主循环正常运转 THEN 看门狗 SHALL CONTINUE TO 每 15s 发送 WATCHDOG=1，正常运行期间 systemd 不触发重启

3.2 WHEN `PipelineHealthMonitor` 进入 FATAL THEN 系统 SHALL CONTINUE TO 走既有优雅退出路径（shutdown_requester 设 flag → 主循环退出 → `AppContext::stop()` 逆序清理 → `EXIT_FAILURE`），交 systemd `Restart=on-failure` 拉起

3.3 WHEN 正常 SIGTERM/SIGINT 关闭 THEN 系统 SHALL CONTINUE TO 执行 notify_stopping、stop_watchdog_thread 与 ShutdownHandler 逆序清理（per-step 5s + 总 30s 超时保护），正常关闭期间不因 liveness 门控产生误杀

3.4 WHEN TRUNK 级 bus 错误或 heartbeat 去抖达到阈值 THEN 系统 SHALL CONTINUE TO 走 spec-32 的恢复编排（state reset → full rebuild → 摄像头带重试打开 → 失败计数/指数退避 → FATAL），本 spec 不改变恢复编排结构与状态机转移表

3.5 WHEN KVS / WebRTC 分支级错误（ErrorScope 为 KVS_BRANCH / WEBRTC_BRANCH）发生 THEN 系统 SHALL CONTINUE TO 仅记日志 + 计数，不触发整管道恢复（spec-32 需求 1 语义不变）

3.6 WHEN 恢复成功 THEN 状态机 SHALL CONTINUE TO 回到 HEALTHY 并重置失败计数与退避时间，`set_health_callback` / `set_rebuild_callback` 对外接口与现有单测语义保持向后兼容

3.7 WHEN 在 macOS（无 HAVE_SYSTEMD / 无 kvssink）上编译运行 THEN 系统 SHALL CONTINUE TO 走既有条件编译 stub 路径，`ctest --test-dir device/build --output-on-failure` 现有测试全绿
