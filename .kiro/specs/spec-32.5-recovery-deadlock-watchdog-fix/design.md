# Spec 32.5: 恢复流程死锁 + 看门狗失明修复 — Bugfix Design

## Overview

2026-09-03 定案的生产事故暴露两个精确缺陷：(1) 看门狗健康门控是"状态检查"而非"活性检查"，主线程死锁时状态机停在 RECOVERING（非 FATAL），独立存活的看门狗线程持续喂狗 2 天 8 小时，systemd `WatchdogSec=30` 完全失明；(2) 恢复路径上存在绕过 spec-32 有界异步 teardown 的同步 KVS 拆除调用点，主线程直接进入 `KinesisVideoStream::free()`，在 KVS Producer C SDK 内部形成 join-while-holding-lock 死锁环。

修复策略分两条线，均为对既有机制的最小增量：

- **缺陷 1（看门狗失明）**：喂狗与 GLib 主循环活性绑定。主循环挂 2s 低频 timer 刷新原子 liveness 时间戳；恢复关键路径（`attempt_recovery` 独占主循环期间）在阶段边界插入检查点主动刷新；看门狗线程喂狗前做 `fresh(liveness) AND healthy` 双重门控。真死锁时刷新必然停止，10s 内判定 stale，下一个 15s 喂狗时机必然跳过，systemd 在 `WatchdogSec=30` 窗口内 SIGKILL + `Restart=on-failure` 拉起干净进程。
- **缺陷 2（同步 KVS 拆除泄漏路径）**：先取证后修复。设计给出取证方法论（审阅清单 S1–S6 + gdb 堆栈比对），封堵机制统一为"可能进入 KVS free 链的调用只在 worker 线程"：恢复路径上所有可能触发 kvssink change_state（含 →NULL 拆除与 →PLAYING 上行再初始化——后者经 `kinesis_video_producer_init` 同步析构旧 producer，S6 取证定案）/ dispose / KVS stream free 的调用（含引用计数归零的最后一次 unref）全部经由 bounded helper（既有 set_null_bounded / teardown_pipeline_bounded + 新增对称的 set_playing_bounded）移入 worker 线程，主循环只做有界等待（≤5s）。同时增强 teardown 全程日志（worker 启动/完成/超时/耗时/分支），使同类事故可从日志直接定位。

死锁环本身位于 KVS Producer C SDK 内部（SDK bug），本设计不试图在应用层修 SDK 锁序，只保证"主线程不触碰"，并以 liveness 门控作为对任意未知主线程死锁的兜底（缺陷 1 修复对缺陷 2 残余风险形成双保险）。

## Glossary

- **Bug_Condition (C)**：触发缺陷的条件——(C1) 主循环停摆但健康状态非 FATAL，看门狗照常喂狗；(C2) 恢复路径上某操作在主循环线程同步进入 KVS 拆除链。
- **Property (P)**：期望行为——(P1) 主循环停摆在有限时间窗内被 systemd 感知并重启；(P2) 恢复路径上 KVS 拆除链只在 worker 线程执行。
- **Preservation**：正常运行喂狗节律、FATAL 优雅退出路径、SIGTERM 关闭流程、spec-32 恢复编排结构与状态机转移表、macOS stub 行为，均不得改变。
- **liveness 时间戳**：`std::atomic<int64_t>`（steady_clock 毫秒），由主循环 timer 与恢复检查点刷新，看门狗线程只读。选 int64 毫秒而非 `atomic<time_point>`，保证 aarch64/x86_64 上 lock-free（`is_always_lock_free`）。
- **stale 阈值（T_stale）**：liveness 时间戳陈旧超过该值即判定主循环停摆，默认 10000ms。
- **恢复检查点（checkpoint）**：`attempt_recovery` 执行期间（主循环被该回调独占，2s timer 不运行）在阶段边界主动刷新 liveness 的调用点。
- **引用计数归零点**：`gst_object_unref` 使引用计数归零的那次调用——**dispose 在该调用所在线程执行**。worker 完成 set_state(NULL) ≠ 主线程 unref 安全。
- **bounded helper**：spec-32 交付的 `set_null_bounded`（不转移所有权）/ `teardown_pipeline_bounded`（转移所有权，worker 内 NULL + unref），worker 执行阻塞操作、调用者最多等 budget_ms、超时 detach 后台完成。本 spec 按同模式新增 `set_playing_bounded`（不转移所有权，worker 内 set_state(PLAYING)，S6 封堵）。
- **`watchdog_gate_open()`**：`sd_notifier.cpp` 中看门狗喂狗前的门控判定，现只查 health_check（`state() != FATAL`），本 spec 扩展为 health AND liveness-fresh。
- **`try_state_reset` / `try_full_rebuild` / `attempt_recovery`**：`pipeline_health.cpp` 中 spec-32 交付的两级恢复引擎。
- **rebuild 回调**：`app_context.cpp` 中注册给 health monitor 的整管道重建逻辑（detach → 有界 teardown → `open_with_retry` 带重试 build+start）。

## Bug Details

### Bug Condition

缺陷 1：主线程（GLib 主循环线程）在恢复流程中死锁后，主循环停止运转，但 `SdNotifier` 看门狗线程独立存活，其门控 `watchdog_gate_open()` 只查询 `AppContext::is_healthy()`（即 `state() != HealthState::FATAL`）。死锁时状态机永远停留在 RECOVERING（属于"健康"），门控恒开，WATCHDOG=1 每 15s 照发，systemd 无从感知（bugfix.md 1.1、1.2）；SIGTERM 亦无法退出——signal handler 只设 atomic flag，消费该 flag 的 `check_shutdown` timer 跑在已死的主循环上（1.3）。

缺陷 2：恢复流程中存在未被 bounded helper 覆盖的同步路径，主线程直接进入 kvssink 的 dispose / KVS stream free 链（gdb 定案：主线程经 `__pthread_once`/`std::call_once` 进入 `KinesisVideoStream::free()` → `pthread_join`，持有 KVS Producer client 锁去 join 一个正等待同一把锁做 continuousRetry 重连的 streaming 线程 q-kvs:src，双向死锁；1.4、1.5）。

**Formal Specification:**

```
FUNCTION isBugCondition(input)
  INPUT: input = { main_loop_alive: bool,           -- GLib 主循环是否在运转
                   health_state: HealthState,       -- 状态机当前状态
                   op: RecoveryPathOperation }      -- 恢复路径上正在执行的操作
  OUTPUT: boolean

  -- C1（缺陷 1）：主循环停摆 + 状态非 FATAL + 喂狗继续
  C1 := (NOT input.main_loop_alive)
        AND (input.health_state != FATAL)
        AND watchdog_feed_continues()

  -- C2（缺陷 2）：恢复路径操作在主循环线程同步触碰 KVS 拆除链
  -- set_state_PLAYING_reinit：上行 NULL_TO_READY 触发 kvssink 再初始化、同步析构旧 producer（S6 取证定案）
  C2 := input.op IN { set_state_NULL, set_state_PLAYING_reinit, last_unref_of_pipeline, message_last_unref }
        AND thread_of(input.op) = MAIN_LOOP_THREAD
        AND may_enter_kvs_teardown_chain(input.op)   -- kvssink change_state/dispose/stream free

  RETURN C1 OR C2
END FUNCTION
```

### Examples

- **本事故（C2 → C1 链式）**：09-01 10:39 v4l2-source "Internal data stream error" → TRUNK 恢复 → "Attempting state reset recovery" 后主线程沿某条同步路径进入 `KinesisVideoStream::free()` → SDK 内部死锁 → 主循环冻结（C2 成立）；随后状态停在 RECOVERING，看门狗喂狗 2 天 8 小时（C1 成立），SIGTERM 无效，只能 SIGKILL。
- **假活观测面（1.6）**：主循环死锁后 viewer 发起 WebRTC 连接，信令/ICE 全部成功（信令线程独立存活）但永远无视频帧。
- **竞态分叉（1.7）**：同类触发（v4l2 故障 + KVS 连接 churn）在 08-30 前多次走 FATAL 优雅退出（restart counter=18），本次却死锁——终态取决于恢复路径与 KVS 重连线程的时序竞态。
- **边界反例（非 bug）**：状态机进入 FATAL 时门控关闭、喂狗停止、优雅退出——spec-32 既有路径，工作正常，必须保留。

## Expected Behavior

### Preservation Requirements

**Unchanged Behaviors:**

- 健康（非 FATAL）且主循环正常运转时，看门狗每 15s 发送 WATCHDOG=1，正常运行期间 systemd 不触发重启（3.1）
- FATAL → 优雅退出路径完整保留：shutdown_requester 设 flag → `check_shutdown` timer 退主循环 → `notify_stopping` → `stop_watchdog_thread` → `AppContext::stop()` 逆序清理 → `EXIT_FAILURE` → systemd `Restart=on-failure` 拉起（3.2）
- 正常 SIGTERM/SIGINT 关闭流程不变：notify_stopping、stop_watchdog_thread、ShutdownHandler 逆序清理（per-step 5s + 总 30s 超时保护），不因 liveness 门控产生误杀（3.3）
- spec-32 恢复编排结构不变：state reset → full rebuild → 摄像头带重试打开 → 失败计数/指数退避 → FATAL；状态机转移表不变（3.4)
- ErrorScope 分支错误语义不变：KVS_BRANCH / WEBRTC_BRANCH 只记日志 + 计数，不触发整管道恢复（3.5）
- 恢复成功回 HEALTHY、重置失败计数与退避；`set_health_callback` / `set_rebuild_callback` / `SdNotifier::set_health_check` 对外接口向后兼容，现有单测语义不变（3.6）
- macOS（无 HAVE_SYSTEMD / 无 kvssink）条件编译 stub 路径不变，`ctest --test-dir device/build --output-on-failure` 现有测试全绿（3.7）

**Scope:**

所有不涉及"主循环停摆"与"恢复路径 KVS 拆除"的输入完全不受影响，包括：正常喂狗节律、正常启停、分支级错误处理、恢复成功路径的状态机行为、macOS 全部 stub 行为。

## Hypothesized Root Cause

**缺陷 1 根因（确定，代码可直接印证）：**

`sd_notifier.cpp` 的 `watchdog_gate_open()` 仅执行 `s_health_check()`，而 main.cpp 注册的检查是 `ctx.is_healthy()` = `state() != HealthState::FATAL`。该门控反映的是状态机**标记**，不反映主循环**真实活性**。看门狗线程（`std::thread` + `condition_variable::wait_for`）与主循环完全解耦，主循环死掉它照常运转。这是 spec-32 需求 4 的设计盲区：当时假设"不健康"一定会表现为 FATAL 标记，未覆盖"状态机本身冻结在中间状态"的故障模式。

**缺陷 2 根因（2026-09-04 Task 1 取证定案：S6，推翻原 S1 最强假设——gate 触发后按此更新）：**

gdb 堆栈显示主线程**直接**处于 `KinesisVideoStream::free()`（经 `__pthread_once`/`std::call_once` 进入，bt 深度 12 截断），不在任何 bounded helper 的 cv 等待栈里，且 bounded helper 的完成/超时日志一条未出。**取证定案（三条证据链见 development-trace 2026-09-04 条目）：** 主线程进入 KVS free 的路径是 **S6——`try_state_reset` 中上行 `gst_element_set_state(pipeline_, GST_STATE_PLAYING)`**：GStreamer 分发 NULL_TO_READY → `gst_kvs_sink_change_state` → `kinesis_video_producer_init()` 末行 `data->kinesis_video_producer = KinesisVideoProducer::createSync(...)` 的 unique_ptr 赋值**同步析构旧 producer** → `~KinesisVideoProducer` → `freeStreams` → `KinesisVideoStream::free()`（call_once）→ C 层 `freeStreamMapping` 持 client 锁 `pthread_join` 一个正等同一把锁的 continuousRetry 线程（LWP 198754，线程名 "q-kvs:src" 系 Linux 线程名继承）。原 S6 排除理由"set PLAYING 上行转换一般不触发 free"被 kvssink 源码直接证伪。journal 排除法同时证明 S1 尾部裸 set_state(NULL) 本次**未被执行到**（其前置 warn "Pipeline did not reach PLAYING after reset" 未出现），S1 降级为同类危险面保守封堵项。恢复路径上主线程同步调用点定案如下：

| # | 位置 | 调用 | 执行线程 | 取证结论（2026-09-04 定案） |
|---|------|------|---------|---------|
| S1 | `pipeline_health.cpp` `try_state_reset` 尾部 | PLAYING 拉起失败分支的 `gst_element_set_state(pipeline_, GST_STATE_NULL)` | **主线程** | **排除（本次事故）、保留为同类危险面保守封堵**：journal 证明其前置 warn 未打、本次未执行到；macOS 动态旁证确认该裸 NULL 确在主调线程执行（若走到即同类风险），Task 5 照旧封堵 |
| S2 | `app_context.cpp` rebuild 回调 | `PipelineManager::create` 失败分支的 `gst_object_unref(p)` | **主线程** | **保守封堵维持**：未启动管道 NULL 态 kvssink dispose 行为未验证（kvssink finalize 会 reset producer，可能进 free 链），按 SHALL NOT 不猜测，统一走 worker |
| S3 | `app_context.cpp` rebuild 回调 | `new_pm->start()` 失败 → `new_pm` 析构 → `PipelineManager::stop()`（同步 set NULL + unref） | **主线程** | **保守封堵维持**：set_state(PLAYING) 分发后 kvssink 可能已完成 NULL_TO_READY 建流（producer/stream 已活），主线程同步拆除风险与 S6 同级 |
| S4 | `pipeline_health.cpp` `set_null_bounded` 超时分支 | detached worker 仍持裸指针执行 `get_state(∞)`；随后 rebuild 的 teardown worker 对同一管道 set NULL + unref | worker×2 | **加固维持**：dispose 落在 worker（非主线程），但两 worker 竞态存在 UAF 风险，需 worker 持有自己的引用 |
| S5 | `detach()` 中 `g_source_remove(bus_watch_id_)` | bus source finalize 时 unref 队列中未处理的 GstMessage（消息持有 src element 即 kvssink 的引用） | **主线程** | **加固维持**：teardown worker 在 unref 管道前排空 bus，防消息引用使归零点漂移到不确定线程 |
| S6 | `try_state_reset` 的 `set_state(PLAYING)`（上行再初始化） | **主线程** | **定案为本次死锁调用点（原排除理由被 kvssink 源码证伪）**：NULL_TO_READY → `kinesis_video_producer_init` 末行 unique_ptr 赋值同步析构旧 producer → freeStreams → `KinesisVideoStream::free()`（call_once，对应 gdb #10/#11）→ freeStreamMapping 持 client 锁 join continuousRetry 线程 → 死锁。`get_state(..., 5s)` 有界等待不 free，维持排除 |

**取证定案对封堵设计的直接影响（已消解，见 development-trace 2026-09-04 条目）：** 原 Fix Implementation 只封堵拆除方向（S1/S2/S3），未覆盖 S6 上行方向——主线程的 `set_state(PLAYING)` 同样同步进入 KVS free 链，必须同样移入 worker 有界执行，否则本次死锁原样复发。本文档已按此修订：Fix Implementation 新增 `set_playing_bounded`（S6 封堵，条目 9，选定"PLAYING 拉起入 worker、get_state 留主线程"方案——get_state 只等待不 free，取证维持排除），决策点 1 检查点表补 CP2.5/CP3.5 维持 T_stale=10s 约束链闭合。

**取证方法论（tasks 阶段执行，结论产出后才动代码，2.7）：**

1. 静态审阅：逐条过上表 S1–S6 与 `attempt_recovery` 全部分支，标记每一处 `gst_element_set_state` / `gst_object_unref` / 析构链的执行线程与"是否可能进入 kvssink change_state/dispose"。
2. 堆栈比对：与 `/tmp/hang_stacks_20260903.txt` 存档（`std::call_once` → `KinesisVideoStream::free()` → `pthread_join`）及 journal 时间线（"Attempting state reset recovery" 后无任何 bounded 日志）交叉验证，判定最可能调用点（原最强假设 S1，**取证已定案为 S6**，见上表）。
3. 结论记录到 `docs/development-trace.md`（含排除项的排除理由），随后按封堵设计动代码。取证若推翻 S1 假设，回到本表重新定位（re-hypothesize），不凭猜测扩大修改面。

## 设计决策

### 决策点 1（消解 bugfix.md 2.4）：liveness stale 阈值 vs 恢复关键路径最坏合法阻塞

矛盾：`attempt_recovery` 在**一次主循环回调内**串行执行 set_null_bounded（≤5s）→ set_playing_bounded（≤5s，S6 封堵后）→ get_state（≤5s）→ 失败分支 set_null_bounded（≤5s，S1 封堵后）→ teardown_pipeline_bounded（≤5s）→ open_with_retry（默认 500ms × 6 次，每次含 build+start，单次最坏约 3s）——最坏合法阻塞可达 **46s+**，期间主循环 2s liveness timer 完全不运行。

- **方案 A（抬高阈值覆盖最坏值）**：T_stale ≥ 50s → 死锁检测窗口 = 50s（stale 判定）+ 30s（WatchdogSec）≈ 80s+；且阈值与恢复路径总时长脆弱耦合——未来任何恢复路径改动（加一次重试、加一个阶段）都会 silently 突破阈值造成误杀，或迫使阈值继续膨胀。**否决**。
- **方案 B（恢复检查点主动刷新，选定）**：恢复流程在阶段边界插入检查点刷新 liveness（"有进展即存活"）。阈值只与**单次有界等待预算 5s**（spec-32 明确设计常量 `state_reset_timeout_ms`）耦合，可保持 10s；真死锁时主线程卡死在某个同步调用**内部**，检查点必然停止刷新，检测窗口短且确定。

**检查点位置与相邻间隔论证（任意相邻检查点间隔 < T_stale=10s）：**

| 检查点 | 位置 | 距上一检查点的最大合法间隔 |
|--------|------|--------------------------|
| CP0 | 主循环 2s timer（正常运行期唯一刷新源） | 2s + 调度抖动 |
| CP1 | `attempt_recovery` 入口（RECOVERING 转移后） | ≤2s（自最后一次 CP0 tick） |
| CP2 | `try_state_reset`：`set_null_bounded` 返回后 | ≤5s（有界等待预算）+ ε |
| CP2.5 | `try_state_reset`：`set_playing_bounded` 返回后（S6 封堵新增） | ≤5s（有界等待预算）+ ε |
| CP3 | `try_state_reset`：PLAYING `get_state` 返回后 | ≤5s（`state_reset_timeout_ms`）+ ε |
| CP3.5 | `try_state_reset`：失败分支 `set_null_bounded` 返回后（本修订自检补齐） | ≤5s（有界等待预算）+ ε |
| CP4 | rebuild 回调：`teardown_pipeline_bounded` 返回后 | ≤5s（budget）+ ε |
| CP5 | rebuild 回调：`open_with_retry` 每次 try_open 返回后 | ≤ 单次 build+start（<3s）+ 500ms 重试间隔 |
| CP6 | `attempt_recovery` 出口（成功/失败/调度退避后，控制权还给主循环 → CP0 接管） | ≤ε |

最大相邻间隔 = 5s + ε（ε 为毫秒级调度开销），T_stale = 10s 给出 2 倍安全余量。**不变式：恢复路径上每个有界等待原语（set_null_bounded / set_playing_bounded / get_state / teardown_pipeline_bounded）返回点即检查点**——任何两段 5s 有界等待串行而中间无检查点即破 T_stale=10s（CP2.5/CP3.5 即为此补齐：无 CP2.5 则 CP2→CP3 = set_playing(5s)+get_state(5s) = 10s+ε 越界；无 CP3.5 则失败路径 CP3→CP4 = 失败分支 set_null(5s)+teardown(5s) = 10s+ε 越界），未来新增有界阶段必须同点位补检查点并重推本表。恢复失败进入退避等待时控制权返回主循环，CP0 timer 恢复运行，间隔回到 2s。

**完整量化推导（喂狗间隔 15s / WatchdogSec=30 / StartLimitBurst=5/60s / RestartSec=5 / TimeoutStopSec=35，均取自 `scripts/raspi-eye.service` 实际值）：**

- **T_stale 下界**：> 相邻检查点最大间隔（5s+ε），取 2 倍余量 → **10s**。
- **T_stale 上界约束**：T_stale(10s) < 喂狗间隔(15s)。这保证死锁发生后的**第一个**喂狗时机必然判定 stale 并跳过：死锁于 t0，最后刷新 t_r ∈ [t0−2s, t0]，下次喂狗尝试 ≥ t0+ε 且相邻喂狗间隔 15s，则该时机的陈旧度 ≥ 15 − 2 = 13s > 10s，必跳过，无需等第二个周期。
- **检测窗口**：最后一次成功喂狗 ≤ t0 → systemd 在 ≤ t0+30s 触发 watchdog kill（SIGABRT/SIGKILL）→ RestartSec=5 后拉起。总窗口 ≤ **35s**（对比事故的 2 天 8 小时）。
- **三阶段不误杀论证**：
  1. *正常运行*：主循环回调均为毫秒级（有界等待只出现在恢复期），陈旧度上界 ≈ 2s（CP0 间隔）+ 回调耗时 << 10s。恒喂狗。
  2. *合法恢复中*：检查点保证陈旧度 ≤ 5s+ε < 10s。即使某次因极端调度抖动瞬时越过 10s 跳喂一次，systemd 只在**连续 30s 无喂狗**（即连续两个喂狗周期都跳过）才动手——下个周期（15s 后）恢复中的检查点已再次刷新，正常喂狗。双重护栏。StartLimitBurst=5/60s 不会被误杀耗尽：误杀需要"60s 内 5 次连续误杀重启"，而单次误杀概率已被上述双护栏压到设计裕度之外；恢复期最坏合法路径 46s+ 也全程被检查点覆盖。
  3. *优雅退出*：`g_main_loop_run` 返回后立即 `notify_stopping()` → `stop_watchdog_thread()`（毫秒级完成）。主循环退出瞬间 liveness 陈旧度 ≤2s，即使此窗口内恰有一次喂狗 tick 也判定 fresh 照常喂；线程停止后无喂狗 = spec-20 既有行为。STOPPING=1 已发，ShutdownHandler 总超时 30s < TimeoutStopSec=35，退出期不受 watchdog 误杀。

### 决策点 2：SdNotifier 接口形态（扩展 vs 新增）

保留 `set_health_check` 原语义不变（3.6 向后兼容），**新增**独立的 liveness 原语，二者在 `watchdog_gate_open()` 内做 AND 组合（2.3：FATAL 检查与 liveness 检查两者都通过才喂狗）：

- `refresh_liveness()`：写原子时间戳，主循环 timer 与恢复检查点调用。
- `should_feed_watchdog(now_ms, last_liveness_ms, stale_threshold_ms, healthy)`：纯静态函数，全部判定逻辑集中于此，时钟由参数注入（2.5，GTest + RapidCheck 可测）。
- 未刷新哨兵（last_liveness_ms == 0）时 liveness 门控视为开（只由 health 决定）——保证现有 `sd_notifier_test` 与 macOS 行为零变化；生产上 main.cpp 在 `start_watchdog_thread()` 之前先做一次显式 `refresh_liveness()` 并注册 2s timer，哨兵态不会出现在生产运行期。

### 决策点 3：pipeline_health 与 SdNotifier 的耦合方式

`pipeline_health.cpp` 内的检查点（CP1、CP2、CP2.5、CP3、CP3.5、CP6）**不直接**依赖 SdNotifier，通过新增 `PipelineHealthMonitor::set_liveness_callback(std::function<void()>)` 注入；app 层（`app_context.cpp`）接线到 `SdNotifier::refresh_liveness`，rebuild 回调内的 CP4/CP5 由 app_context 直接调用。好处：pipeline_health 保持可独立单测（注入计数回调断言检查点触发次数），且 macOS 上无需条件编译。

### 决策点 4：S1 封堵方式（删除 vs 替换）

`try_state_reset` 失败分支的同步 `set_state(NULL)` **替换为 `set_null_bounded(pipeline_, config_.state_reset_timeout_ms)`**（结果忽略，函数照常 return false）。不选"直接删除"：替换保持"管道朝 NULL 推进"的原语义（后续 full_rebuild 的 teardown 接管所有权时管道已在趋向 NULL），且 worker 执行天然满足"KVS 拆除只在 worker 线程"。若 rebuild 回调未注册（纯单测场景），退避重试再次进入 try_state_reset 时开头的 set_null_bounded 也能兜住。

## Correctness Properties

Property 1: Bug Condition — 主循环停摆时喂狗必停、systemd 有限窗口兜底

_For any_ 主循环停摆场景（liveness 时间戳停止刷新，无论死锁位置、无论状态机是否标记 FATAL），修复后的看门狗门控 SHALL 在陈旧度超过 T_stale(10s) 后对所有后续喂狗时机返回"跳过"，使 systemd 自最后一次成功喂狗起 ≤ WatchdogSec(30s) 内触发重启——检测总窗口 ≤ 35s（含 RestartSec）。

**Validates: Requirements 2.1, 2.2, 2.6**

Property 2: Bug Condition — stale 判定纯函数性质

_For any_ 单调推进的时钟序列与刷新序列，`should_feed_watchdog` SHALL 满足：(a) 阈值边界精确——`now − last ≤ T_stale` 且 healthy 时喂、`now − last > T_stale` 时不喂；(b) 无刷新时 fresh→stale 单向不可逆（时间继续推进不会从 stale 变回 fresh）；(c) 任意一次 refresh 之后立即判定必为 fresh；(d) 判定不依赖真实时钟与 systemd（时间全部参数注入）。

**Validates: Requirements 2.1, 2.2, 2.4, 2.5**

Property 3: Bug Condition — 门控组合四象限

_For any_ (healthy, fresh) ∈ {true,false}²，喂狗判定 SHALL 恰为 healthy AND fresh：两者皆真才喂；FATAL（healthy=false）时无论 fresh 与否不喂（保留 spec-32 门控语义）；stale（fresh=false）时无论 healthy 与否不喂。

**Validates: Requirements 2.2, 2.3**

Property 4: Bug Condition — 恢复路径无主线程 KVS 拆除（结构性）

_For any_ 恢复路径执行（state reset 成功/失败、full rebuild 成功/失败、build 失败、start 失败），所有可能进入 kvssink change_state（含 →NULL 拆除与 →PLAYING 上行再初始化——后者同步析构旧 producer，S6 取证定案）/ dispose / KVS stream free 的调用（含引用计数归零的最后 unref）SHALL 只在 worker 线程发生，主循环线程只做有界等待（≤5s）。验证方式双轨：(a) 取证审阅清单 S1–S6 逐项封堵并记录结论（代码审阅性质，2.7）；(b) 注入 teardown_fn/set_null_fn/set_playing_fn 捕获执行线程 id 的自动化测试，断言 ≠ 调用者线程 id（覆盖 S1/S2/S3/S6 改造点）。

**Validates: Requirements 2.7, 2.8**

Property 5: Bug Condition — teardown 全程可观测

_For any_ 有界 teardown / set-null 操作，系统 SHALL 输出成对日志：worker 启动（含操作标签）、完成（含实际耗时）或超时（含预算与 detach 说明），detach 后台完成时 worker 侧 SHALL 补一条完成日志——同类事故再发生时能从日志直接定位恢复流程走到哪一步。

**Validates: Requirements 2.9**

Property 6: Bug Condition — 确定性终态收敛

_For any_ 恢复不可完成的场景，系统 SHALL 在有限时间内收敛到三种终态之一：恢复成功回 HEALTHY；失败计数达 max_retries 走 FATAL 优雅退出交 systemd 重启；主循环停摆被 liveness 门控兜底重启（Property 1）。不存在"永久 RECOVERING 假活"的第四种终态。

**Validates: Requirements 2.10**

Property 7: Preservation — 正常运行喂狗节律不变

_For any_ 健康（非 FATAL）且主循环正常运转（liveness 持续刷新）的运行期，修复后的看门狗 SHALL 与原实现产生相同的喂狗行为（每 15s 发送 WATCHDOG=1），systemd 不触发重启。

**Validates: Requirements 3.1**

Property 8: Preservation — FATAL 优雅退出与正常关闭流程不变

_For any_ FATAL 转移或正常 SIGTERM/SIGINT 关闭，修复后的系统 SHALL 产生与原实现相同的行为序列：FATAL → shutdown_requester → 主循环退出 → notify_stopping → stop_watchdog_thread → ShutdownHandler 逆序清理（per-step 5s + 总 30s）→ 相应退出码；关闭期间 liveness 门控不引入误杀（stop_watchdog_thread 先于 liveness 变 stale）。

**Validates: Requirements 3.2, 3.3**

Property 9: Preservation — 恢复编排、分支错误语义与对外接口不变

_For any_ TRUNK 错误 / heartbeat 去抖触发，恢复编排 SHALL 保持 spec-32 结构（state reset → full rebuild → 带重试打开 → 退避 → FATAL）与状态机转移表；_for any_ KVS_BRANCH / WEBRTC_BRANCH 错误，SHALL 仅记日志 + 计数不触发整管道恢复；恢复成功 SHALL 回 HEALTHY 并重置计数与退避；`set_health_callback` / `set_rebuild_callback` / `set_health_check` 接口语义向后兼容，现有单测不改断言即通过。

**Validates: Requirements 3.4, 3.5, 3.6**

Property 10: Preservation — macOS stub 路径不变

_For any_ macOS 构建（无 HAVE_SYSTEMD / 无 kvssink），sd_notify 相关调用 SHALL 保持 no-op stub，新增 liveness 逻辑纯逻辑层（不依赖 systemd）在两平台行为一致，`ctest --test-dir device/build --output-on-failure` 全绿。

**Validates: Requirements 3.7**

## Fix Implementation

### Changes Required

按 2026-09-04 取证定案编排（S6 为定案调用点；S1/S2/S3 为同类危险面保守封堵）：

**文件 1–2：`device/src/sd_notifier.h` / `sd_notifier.cpp`（缺陷 1 核心）**

1. 新增静态原子 `s_liveness_ms`（`std::atomic<int64_t>`，初始 0 = 未刷新哨兵）与 `s_stale_threshold_ms`（`std::atomic<int64_t>`，默认 10000）。
2. 新增 `static void refresh_liveness()`：写入 steady_clock 当前毫秒。
3. 新增纯函数 `static bool should_feed_watchdog(int64_t now_ms, int64_t last_liveness_ms, int64_t stale_threshold_ms, bool healthy)`：
   ```cpp
   // 纯函数：喂狗判定。healthy 与 liveness-fresh 取 AND；
   // last_liveness_ms == 0 为未初始化哨兵，liveness 门控视为开（向后兼容）。
   static bool should_feed_watchdog(int64_t now_ms, int64_t last_liveness_ms,
                                    int64_t stale_threshold_ms, bool healthy) {
       if (!healthy) return false;
       if (last_liveness_ms == 0) return true;
       return (now_ms - last_liveness_ms) <= stale_threshold_ms;
   }
   ```
4. `watchdog_gate_open()` 改为：读 health_check（既有逻辑不动）+ 读 `s_liveness_ms` + 取 now → 调 `should_feed_watchdog`。跳过喂狗时的 warn 日志区分原因（health gate / liveness stale，ASCII）。
5. 新增 `static void set_liveness_stale_threshold_ms(int64_t ms)`（测试用，生产用默认值）。

**文件 3：`device/src/main.cpp`（缺陷 1 接线）**

6. Phase 5.5 中、`start_watchdog_thread()` 之前：显式 `SdNotifier::refresh_liveness()`（消除哨兵态）+ `g_timeout_add(2000, liveness_tick, nullptr)` 注册主循环 liveness timer（CP0，回调仅调 `refresh_liveness()` 并 `G_SOURCE_CONTINUE`，开销一次原子写）。

**文件 4–5：`device/src/pipeline_health.h` / `pipeline_health.cpp`（缺陷 1 检查点 + 缺陷 2 封堵）**

7. 新增 `void set_liveness_callback(std::function<void()>)`；在 CP1（attempt_recovery 入口）、CP2（set_null_bounded 返回后）、CP2.5（set_playing_bounded 返回后）、CP3（PLAYING get_state 返回后）、CP3.5（失败分支 set_null_bounded 返回后）、CP6（attempt_recovery 出口）调用（若注册）——不变式：try_state_reset 内每个有界等待原语返回后必有检查点（间隔论证见决策点 1）。
8. **S1 封堵（保守）**：`try_state_reset` 失败分支的同步 `gst_element_set_state(pipeline_, GST_STATE_NULL)` 替换为 `set_null_bounded(pipeline_, config_.state_reset_timeout_ms)`（结果忽略）。取证已排除其为本次事故调用点（journal 证明该分支未执行到），保留为同类危险面封堵。
9. **S6 封堵（取证定案调用点）**：新增 `bool set_playing_bounded(GstElement* pipeline, int budget_ms, std::function<void(GstElement*)> set_playing_fn = nullptr)`，与 `set_null_bounded` 同模式（复用 `run_bounded` 基础设施：不转移所有权、fn 可注入供测试捕获执行线程、默认实现为 worker 内 `gst_element_set_state(pipeline, GST_STATE_PLAYING)`）。`try_state_reset` 的 PLAYING 拉起改为 `set_playing_bounded(pipeline_, config_.state_reset_timeout_ms)`：预算内完成 → 主线程继续 `get_state` 确认 PLAYING（get_state 只等待不 free，取证维持排除）；超时 → 视为 reset 失败走既有失败分支（其 NULL 推进已由 S1 封堵改为 set_null_bounded）。`set_state(PLAYING)` 返回 FAILURE 的判定由后续 get_state 承接（失败的状态转换 get_state 返回 GST_STATE_CHANGE_FAILURE，既有 ok 判定不变）；超时后 detached PLAYING worker 与失败分支 NULL worker 的竞态由 S4 的 ref/unref 加固兜底（`gst_element_set_state` 本身 MT-safe，状态转换内部串行化）。
10. **S4 加固**：`run_bounded` 的 worker 进入 work_fn 前 `gst_object_ref(pipeline)`、work_fn 返回后 unref（防 detached worker UAF；该额外引用的归零点也在 worker 线程）。set_playing_bounded 复用 run_bounded，自动获得同一加固。
11. **S5 加固**：`default_teardown` 扩展为 worker 内先排空 bus（`gst_element_get_bus` → `gst_bus_set_flushing(bus, TRUE)` → unref bus），再 set NULL + get_state + unref pipeline——把队列消息持有的元素引用释放搬到 worker 线程，保证管道 unref 时不残留消息引用导致归零点漂移。
12. **日志增强（2.9）**：`run_bounded` 增加 `op_tag` 参数；worker 侧打 "worker [tag] start" / "worker [tag] done, elapsed=Xms"（detach 后后台完成同样打）；caller 侧保留 completed/timeout 分支日志并补充实际等待耗时。`try_state_reset` / `attempt_recovery` 阶段边界补 info 日志（与检查点同点位）。set_playing_bounded 经由同一 op_tag 机制输出 "worker [set-playing] ..." 成对日志。

**文件 6：`device/src/app_context.cpp`（缺陷 2 封堵 + 检查点接线）**

13. **S2 封堵**：build 后 `PipelineManager::create` 失败分支的 `gst_object_unref(p)` → `teardown_pipeline_bounded(p, /*budget*/5000)`（未启动管道 NULL 转换为 no-op，开销可忽略，统一走 worker 保证归零点在 worker 线程）。
14. **S3 封堵**：`new_pm->start()` 失败分支改为 `GstElement* hp = new_pm->release(); new_pm.reset(); teardown_pipeline_bounded(hp, 5000);`，消除半启动管道在主线程的同步析构。
15. **CP4/CP5**：rebuild 回调内 `teardown_pipeline_bounded` 返回后、`open_with_retry` 每次 try_open lambda 尾部，直接调 `SdNotifier::refresh_liveness()`。
16. 接线：`health_monitor->set_liveness_callback([]{ SdNotifier::refresh_liveness(); });`

**文件预算说明**：生产文件 6 个（sd_notifier.{h,cpp}、main.cpp、pipeline_health.{h,cpp}、app_context.cpp），超出常规 2–5 预算 1 个，为本 bugfix 的显式例外（两个缺陷分属看门狗层与恢复层，物理上无法再收敛；先例：spec-33 六文件显式例外）。测试文件复用既有 `device/tests/sd_notifier_test.cpp` 与 `device/tests/health_test.cpp` 扩展，不新建测试文件。

### Constraints 落地

- **C++17 / cpp-standards**：全部改动 .h/.cpp 分离；无新 new/malloc（atomic + 既有 helper）；日志 ASCII；Debug 构建 ASan 照常覆盖新路径（worker ref/unref 配对由 ASan 验证）。
- **零新依赖**：不引入任何新库；HAVE_SYSTEMD 条件编译模式不变（liveness 判定为纯逻辑层，两平台共用，sd_notify 调用仍在 `#ifdef HAVE_SYSTEMD` 内）。
- **性能**：liveness 刷新为单次原子写（CP0 每 2s 一次、检查点仅恢复期触发），不触碰数据通路；bounded helper 加 ref/unref 为恢复路径低频操作，无高性能路径影响。
- **不改 systemd 单元**：`raspi-eye.service`（WatchdogSec=30、StartLimitBurst=5/60s、TimeoutStopSec=35）保持不变，量化推导以其现值为前提。

### SHALL NOT（Design 层）

- SHALL NOT 让看门狗心跳与主循环活性解耦（本事故根因；喂狗判定必须包含 liveness 新鲜度）
- SHALL NOT 在 GLib 主循环线程执行任何可能进入 kvssink stopStreamSync / dispose / KVS stream free 的同步调用——含 set_state(NULL)、set_state(PLAYING)（上行 NULL_TO_READY 再初始化会同步析构旧 producer 进入 free 链，S6 取证定案）与 unref/dispose 链；**引用计数归零点所在线程即 dispose 执行线程**，worker 完成 set NULL 不等于主线程 unref 安全
- SHALL NOT 在看门狗线程与主循环间用非线程安全方式共享 liveness 状态（必须 `std::atomic<int64_t>`，且确认目标平台 lock-free）
- SHALL NOT 让 liveness 阈值与喂狗间隔 / WatchdogSec 组合产生误杀（约束链：相邻检查点最大间隔 5s+ε < T_stale=10s < 喂狗间隔 15s < WatchdogSec=30s；量化推导见"设计决策"节，改任何一个参数必须重推）
- SHALL NOT 在不确定外部 SDK 行为时凭猜测——KVS SDK 死锁环是 SDK 内部 bug，应用层只保证"不在主线程触碰"，不得试图在应用层"修 SDK 锁序"；S2 的 NULL 态 kvssink dispose 行为未验证，一律保守走 worker
- SHALL NOT 在取证结论（2.7）记录到 development-trace 之前修改缺陷 2 相关代码

## Testing Strategy

### Validation Approach

两阶段：先在未修复代码上确认缺陷（探索性检查 + 取证），再验证修复正确性（Fix Checking）与既有行为不变（Preservation Checking）。缺陷 2 的 KVS 死锁无法在 macOS 复现（无 kvssink），结构性封堵以"取证审阅 + 注入线程断言测试"双轨验证；systemd 端到端行为标注"需实测确认"，落到 Pi 5 集成验证任务。

### Exploratory Bug Condition Checking

**Goal**：在修复前于未修复代码上呈现反例，确认或推翻根因分析；推翻则回到 Hypothesized Root Cause 重新假设。

**Test Plan**：

1. **看门狗失明复现（缺陷 1，将在未修复代码上失败）**：`set_health_check` 注册返回 true（模拟 RECOVERING 非 FATAL），不做任何 liveness 刷新模拟主循环死锁，断言"门控应关闭"——未修复代码上 `watchdog_gate_open()` 恒 true，测试失败即证明喂狗不会停。
2. **恢复路径主线程同步拆除取证（缺陷 2）**：静态审阅 S1–S6 清单 + gdb 堆栈/journal 时间线比对（见取证方法论），结论记录 development-trace。不强行动态复现 SDK 死锁。
3. **S1 动态旁证（macOS 可跑）**：用 fakesink 管道驱动 `try_state_reset` 走"set_null_bounded 成功 + PLAYING 拉起失败"分支，在未修复代码上确认尾部 NULL 调用发生在主调线程（可用 GST_DEBUG 或临时线程 id 记录观察）。

**Expected Counterexamples**：

- 缺陷 1：liveness 从不刷新时门控仍开（喂狗继续）。
- 缺陷 2：try_state_reset 失败分支存在主线程裸同步 set_state(NULL)；rebuild 失败分支存在主线程 unref/析构。
- 可能推翻点：若取证显示主线程进入 KVS free 的路径不在 S1–S3 中（如经由 S5 消息引用），则按 S5 论证补充封堵并更新本设计。（2026-09-04 取证结果：确已推翻——定案 S6 上行再初始化路径，Fix Implementation 已按此更新，新增 set_playing_bounded 封堵。）

### Fix Checking

**Goal**：验证 bug condition 成立的所有输入上，修复后行为满足 Property 1–6。

**Pseudocode:**

```
FOR ALL input WHERE isBugCondition(input) DO
  -- C1: 主循环停摆
  IF NOT input.main_loop_alive THEN
    ASSERT should_feed_watchdog(now, last_refresh, 10000, healthy) = false
           WHENEVER now - last_refresh > 10000
    ASSERT systemd_restart_within(35s)          -- Pi 5 实测确认
  -- C2: 恢复路径 KVS 拆除
  IF input.op IN recovery_teardown_ops THEN
    ASSERT thread_of(input.op) != MAIN_LOOP_THREAD
END FOR
```

### Preservation Checking

**Goal**：验证 bug condition 不成立的所有输入上，修复后行为与原实现一致。

**Pseudocode:**

```
FOR ALL input WHERE NOT isBugCondition(input) DO
  ASSERT watchdog_behavior_fixed(input) = watchdog_behavior_original(input)
  ASSERT recovery_orchestration_fixed(input) = recovery_orchestration_original(input)
END FOR
```

**Testing Approach**：Preservation 采用 property-based testing（RapidCheck）为主：自动生成大量 (now, last, threshold, healthy) 组合与恢复场景序列，覆盖手写用例遗漏的边界，为"非 bug 输入行为不变"提供强保证。

**Test Plan**：先在未修复代码上观察正常喂狗节律、FATAL 门控、恢复编排行为（既有测试即为行为快照），修复后既有测试不改断言全绿。

**Test Cases**：

1. **正常喂狗保持**：healthy=true 且持续刷新 liveness → 门控恒开（对应既有 `sd_notifier_test` 行为不变）。
2. **FATAL 门控保持**：health_check 返回 false → 无论 liveness 新鲜与否门控关闭。
3. **恢复编排保持**：`health_test` 既有状态机/恢复用例（state reset、full rebuild、退避、FATAL）不改断言全绿；检查点回调未注册时恢复行为与现状完全一致。
4. **关闭流程保持**：stop_watchdog_thread 幂等、快速唤醒语义不变。

### Unit Tests

- `should_feed_watchdog` 边界用例：恰好等于阈值（fresh）、超出 1ms（stale）、哨兵 0（视为开）、healthy=false 短路。
- 门控四象限枚举（Property 3）。
- 检查点触发：注入计数回调，驱动 try_state_reset 成功/失败路径，断言各路径对应检查点触发（成功路径：CP1/CP2/CP2.5/CP3/CP6 各一次；PLAYING 拉起失败路径：额外 CP3.5）。
- S1/S2/S3/S6 线程断言：注入 teardown_fn/set_null_fn/set_playing_fn（set_playing_bounded 与 set_null_bounded 同模式接受可注入 fn）捕获 `std::this_thread::get_id()`，断言 ≠ 测试主线程 id；S6 通过驱动 try_state_reset 的 PLAYING 拉起路径覆盖。
- 日志/分支可观测:注入慢 fn（> budget）驱动超时分支，断言返回 false 且 worker 后台完成标志最终置位（配合超时分级：多轮恢复类 ≤15s）。

### Property-Based Tests

- **PBT-1（Validates Property 2）**：随机生成单调时钟序列 + 刷新事件序列，验证 fresh→stale 单向、刷新后必 fresh、阈值边界精确。
- **PBT-2（Validates Property 3/7）**：随机 (healthy, now−last, threshold) 向量，验证 feed ⇔ healthy ∧ (now−last ≤ threshold ∨ last==0)。
- **PBT-3（Validates Property 9）**：复用/扩展既有 health_test PBT（恢复序列生成），断言检查点注入不改变状态机轨迹。

### Integration Tests

- **macOS 全量**：`ctest --test-dir device/build --output-on-failure` 全绿（含 ASan），验证 stub 路径与既有行为（Property 10）。
- **Pi 5 端到端（需实测确认，落最终验证任务）**：
  1. 部署后 `kill -STOP <pid>`（或 gdb attach 挂起主线程）模拟主循环停摆 → 观察 journal：liveness stale 跳喂日志 → systemd watchdog kill → 自动重启，全程 ≤ ~35s（Property 1/6）。
  2. 正常运行 ≥ 1 个喂狗周期无跳喂日志；正常 `systemctl stop` 走优雅关闭无误杀（Property 7/8）。
  3. 拔插 USB 摄像头触发 TRUNK 恢复，确认恢复期无 stale 跳喂、teardown 全程日志成对出现（Property 5 + 三阶段不误杀论证实测）。
