# Design Document

> Spec 32: 管道韧性加固 — 设计

## Overview

本设计在不重构 tee 管道结构、不改 WebRTC peer 生命周期的前提下，修改"健康监控的判定/恢复策略"
与少量"故障注入点的容错"，实现：KVS 瞬时故障分支内自愈、恢复不再拖垮 WebRTC、恢复有界且不假死、
FATAL 自动重启。改动集中在 8 个源文件 + 1 个配置 + 1 个诊断脚本。

设计遵循一条主线：**把"错误/异常"按故障域分类，只有共享 trunk 的问题才动整管道；动整管道时保证
有界、有序（先释放再重建）、且失败能升级到进程重启。**

## Architecture

### 错误与恢复决策流（核心）

```
GStreamer bus ERROR ──► classify_bus_error(src_name)
                          ├─ KVS_BRANCH   ─► 仅日志+计数，不恢复（kvssink restart-on-error 自愈）
                          ├─ WEBRTC_BRANCH─► 仅日志+计数，不恢复
                          └─ TRUNK        ─► attempt_recovery()（整管道）

heartbeat(2s) ──► get_state(pipeline)
                  ├─ ASYNC          ─► 跳过本轮（瞬态）
                  ├─ PLAYING        ─► consecutive=0
                  └─ 非 PLAYING     ─► ++consecutive; 若 ≥3 ─► attempt_recovery()（整管道）

attempt_recovery() ─► try_state_reset(worker set NULL, 主循环等 ≤5s)
                       └─ 失败/超时 ─► try_full_rebuild() [detach 健康监控 → release()+有界异步 teardown 旧管道 → 带重试构建新管道]
                                   ├─ 成功 ─► HEALTHY
                                   └─ 连续失败 ≥3 ─► FATAL ─► 优雅退出(EXIT_FAILURE) ─► systemd 重启

watchdog 心跳线程(15s) ─► is_healthy()? ── 是 ─► sd_notify(WATCHDOG=1)
                                          └─ 否(FATAL/非PLAYING) ─► 跳过 ─► systemd WatchdogSec 兜底重启
```

### 模块改动总览

| 文件 | 改动 | 对应需求 |
|------|------|---------|
| `pipeline_health.{h,cpp}` | `ErrorScope` 枚举 + `classify_bus_error` 纯函数；bus_watch 按域分流；heartbeat 去抖+主动恢复；`detach()`；有界异步 teardown（set NULL 移出主循环）；try_state_reset 重写；FATAL 触发退出回调 | 1,3,4,5 |
| `pipeline_manager.{h,cpp}` | `release()`：放弃所有权返回裸指针（供 teardown 转移） | 2,5 |
| `kvs_sink_factory.cpp` | `restart-on-error=TRUE` | 1 |
| `camera_source.{h,cpp}` | `open_with_retry` 纯重试 helper + `OpenRetryConfig` | 2 |
| `app_context.cpp` | rebuild 回调"detach→有界异步 teardown→带重试构建新"；注册 FATAL→shutdown 回调；`is_healthy()` | 2,4 |
| `sd_notifier.{h,cpp}` | 心跳按 `is_healthy()` 门控 | 4 |
| `main.cpp` | FATAL 退出路径（g_fatal flag + 退出码）；注册健康查询给 sd_notifier | 4 |
| `config_manager.{h,cpp}` + `config.toml.example` | 默认码率下调 + buffer_duration 默认下调 | 5,6 |
| `scripts/diagnose-cpu.sh`（新增） | CPU 基线采集脚本 | 7 |

## Components and Interfaces

### 1. ErrorScope 分类（需求 1）

`pipeline_health.h` 新增：

```cpp
enum class ErrorScope { KVS_BRANCH, WEBRTC_BRANCH, TRUNK };

// 纯函数，按 GStreamer 元素名归类故障域。未知/空名按 TRUNK（保守：宁可恢复不漏）。
ErrorScope classify_bus_error(const std::string& src_name);
```

`pipeline_health.cpp` 实现（基于 pipeline_builder 实际元素命名）：

```cpp
ErrorScope classify_bus_error(const std::string& n) {
    // KVS 分支：q-kvs / kvs-parser / avc-caps / kvs-sink
    if (n == "q-kvs" || n == "kvs-parser" || n == "avc-caps" || n == "kvs-sink")
        return ErrorScope::KVS_BRANCH;
    // WebRTC 分支：q-web / webrtc-sink
    if (n == "q-web" || n == "webrtc-sink")
        return ErrorScope::WEBRTC_BRANCH;
    // 其它（src/v4l2-source/convert/videoflip/capsfilter/raw-tee/q-enc/encoder/parser/bs-caps/encoded-tee/q-ai/ai-sink）
    // 注：摄像头错误源元素名是内部的 "v4l2-source"（source 为名为 "src" 的 Source Bin）；未知名同样落 TRUNK。
    return ErrorScope::TRUNK;
}
```

`bus_watch_cb` 的 `GST_MESSAGE_ERROR` 分支改为：

```cpp
auto scope = classify_bus_error(GST_OBJECT_NAME(msg->src));
if (scope == ErrorScope::TRUNK) {
    self->attempt_recovery();
} else {
    // 分支级：仅日志 + 计数，交给 SDK/分支自愈
    self->branch_error_count_++;   // 新增成员，供观测/单测
    logger->warn("Branch error ({}) from {}: {} — not triggering pipeline recovery",
                 scope == ErrorScope::KVS_BRANCH ? "KVS" : "WEBRTC",
                 GST_OBJECT_NAME(msg->src), err->message);
}
```

> 注：`branch_error_count_` 仅用于可观测与单测，不参与状态机。

### 关键设计决策 A — 解决"需求1↔需求3 互相抵消"（最重要）

**风险**：若 kvssink 在 `restart-on-error=TRUE` 内部重连时把管道聚合状态拉到 PAUSED，
heartbeat（需求3）仍会触发整管道恢复，使需求1 失效。

**主方案（先验证）**：`restart-on-error=TRUE` 时 KVS Producer SDK 在 worker 内部重试，
kvssink 元素本身停留在 PLAYING、不发起 element 级状态回退，因此不影响 `gst_element_get_state(pipeline)`。
heartbeat 的 3 次去抖（≈6s）也为短暂波动留出缓冲。**design 阶段必须在 Pi 上用 `GST_DEBUG=GST_STATES:4`
注入一次 KVS 网络中断，确认管道聚合状态保持 PLAYING。**

**兜底方案（若主方案被证伪）**：把 KVS 分支 `q-kvs → kvs-parser → avc-caps → kvs-sink` 封装进一个
`GstBin`，对该 bin 调用 `gst_bin_set_message_forwarding` 控制并让 bin 吸收子元素的状态变化，
使其不冒泡到顶层 pipeline 的聚合状态。此为更大改动，仅在主方案验证失败时启用（记入 tasks 的条件分支）。

验证写入 `docs/development-trace.md`，作为本 spec 的关键风险结论。

### 2. 摄像头打开重试 + 重建时序（需求 2）

`camera_source.h` 新增（纯逻辑，不依赖 GStreamer，便于单测）：

```cpp
struct OpenRetryConfig {
    int max_attempts = 6;       // 最多尝试次数
    int interval_ms  = 500;     // 间隔
};

// 反复调用 try_open（返回 true 即成功），直到成功或耗尽。
// sleep_fn 可注入（测试用假 sleep），默认真实 sleep。
// 返回成功与否 + 实际尝试次数（out_attempts，可选）。
bool open_with_retry(const std::function<bool()>& try_open,
                     const OpenRetryConfig& cfg,
                     const std::function<void(int)>& sleep_ms = {},
                     int* out_attempts = nullptr);
```

### 关键设计决策 B — 解决"需求2↔需求5 摄像头 fd 释放时序" + 主循环不被同步 teardown 阻塞（重要）

**现状 bug（两层）**：
1. `app_context` 的 rebuild 回调先 `build_tee_pipeline()`（新 v4l2src 打开 `/dev/IMX678`），再 `move-assign`
   销毁旧管道——新 source 打开设备时旧 source 还占着 fd → `Could not open device`。
2. 销毁旧管道走 `PipelineManager::stop()` → `gst_element_set_state(pipeline, NULL)`，这是**同步阻塞**调用：
   GStreamer 在调用线程逐个跑 element `change_state`，kvssink PAUSED→READY 触发 `stopStreamSync` 可阻塞
   ~130s（日志实测）。而 NULL 先处理 sink 再到 source，卡在 kvssink 期间 v4l2src 的 fd 根本还没释放。
   **不能用后续 `get_state` 超时去"封顶"一个已经在前面同步卡住的 `set_state`。**

**修复 — 有界异步 teardown（方案 X）**：把旧管道的"置 NULL + unref"转移到独立 worker 线程，主循环只等
预算 `state_reset_timeout_ms`（默认 5s）；超时即 detach（worker 后台继续完成并最终 unref），主线程
继续构建新管道。新增 `PipelineManager::release()` 转移裸指针所有权给 teardown 任务。

```cpp
// pipeline_manager.h 新增：释放所有权（不 stop、不 unref），返回裸指针，内部置空
GstElement* release();

// pipeline_health 内新增两个有界 helper（均在 worker 线程做同步 set_state(NULL)，主循环有界等待 budget_ms）：
//
// (a) 转移所有权版（用于 full_rebuild 销毁旧管道）：worker 完成 NULL 后 unref；
//     主线程超时即 detach，worker 后台完成并 unref。返回 true=预算内完成。
bool teardown_pipeline_bounded(GstElement* pipeline /*ownership 转移*/, int budget_ms);
//
// (b) 不转移所有权版（用于 try_state_reset 复用同一管道）：worker 只 set_state(NULL) 不 unref；
//     返回 true=预算内到达 NULL（主线程随后 set PLAYING 复用）；false=超时（此时调用方改用 (a) 接管+rebuild）。
bool set_null_bounded(GstElement* pipeline /*不转移所有权*/, int budget_ms);
```

rebuild 回调（与决策 C 的解绑顺序合并）：

```cpp
health_monitor->set_rebuild_callback([this]() -> GstElement* {
    if (ai_handler_) ai_handler_->stop();
    // (1) 决策 C：先让 health monitor 与旧管道解绑（移除 probe + bus watch），避免悬空引用
    impl_->health_monitor->detach();
    // (2) 有界异步销毁旧管道：转移所有权给 worker，主循环最多等 5s；超时则后台完成
    GstElement* old = impl_->pipeline_manager->release();   // 放弃所有权，不 stop
    impl_->pipeline_manager.reset();                        // PipelineManager 壳已空，安全析构
    teardown_pipeline_bounded(old, /*budget*/5000);         // worker: set NULL + unref
    // (3) 带重试构建新管道（覆盖 fd 释放窗口；若旧 teardown 仍卡住、fd 未放，重试耗尽→返回 nullptr→恢复失败→FATAL→systemd 重启）
    GstElement* p = nullptr; std::string err;
    OpenRetryConfig rc;                                     // 500ms × 6
    bool ok = open_with_retry([&]() -> bool {
        p = PipelineBuilder::build_tee_pipeline(&err, cam_config_, ...);
        if (!p) return false;
        auto pm = PipelineManager::create(p, &err);
        if (!pm || !pm->start(&err)) { if (p && !pm) gst_object_unref(p); p = nullptr; return false; }
        impl_->pending_pm_ = std::move(pm);                 // 暂存，成功后转正
        return true;
    }, rc);
    if (!ok) return nullptr;
    impl_->pipeline_manager = std::move(impl_->pending_pm_);
    // 更新各模块 pipeline 引用（stream_controller / bitrate_adapter / media_manager）...
    return impl_->pipeline_manager->pipeline();             // health_monitor 在 try_full_rebuild 返回后 set_pipeline 重新 attach
});
```

> 设计取舍与已知瞬态：
> - teardown 超时 detach 后，旧 kvssink 可能仍在后台连着同名 KVS stream，新 kvssink 同时启动 →
>   KVS 可能短暂拒绝新 producer；但 `restart-on-error=TRUE` + 决策 A 使该错误被归为 KVS_BRANCH、
>   忽略并由 SDK 重试，待旧的释放后自愈。可接受的自愈型瞬态。
> - 若旧 teardown 真卡死且 v4l2 fd 未释放，新建重试（3s）会失败 → 恢复失败累计 → FATAL → 优雅退出 →
>   systemd 重启进程（彻底释放 fd 与 KVS 连接）。这是有界的终态兜底。
> - `v4l2src` 实际打开设备在 READY/PAUSED 切换（`PipelineManager::start()` set PLAYING），故"尝试"粒度
>   是 build + start；上面已把 start 纳入重试闭包。
> - `try_state_reset` 路径见需求 5 实现：其 `set_state(NULL)` 同样用有界 teardown 思路处理。

### 关键设计决策 C — 健康监控与旧管道解绑顺序（修悬空引用）

**问题**：rebuild 提前销毁旧管道时，`PipelineHealthMonitor` 仍持有旧 `pipeline_` 指针、`probe_pad_`（带 ref）、
`bus_watch_id_`，要到 `set_pipeline()` 才解绑——中间这段指向已释放/孤立对象。

**修复**：`PipelineHealthMonitor` 新增 `detach()`，在销毁旧管道**之前**调用，先 `remove_probe()` + 移除 bus watch
并把 `pipeline_` 置空；rebuild 成功后 `try_full_rebuild` 照常 `set_pipeline(new)` 重新 attach。固定顺序：
**detach → 有界 teardown 旧 → build new（带重试）→ set_pipeline(new) re-attach**。

```cpp
// pipeline_health.h 新增
void detach();   // remove probe + bus watch，pipeline_ 置 nullptr（幂等）
```

> `detach()` 与 `set_pipeline()` 均在 GLib 主循环线程调用（attempt_recovery 调用栈内），与 timer/bus 回调
> 同线程串行，无并发。probe_pad_ 自带 ref，detach 时 remove+unref 安全。

### 3. heartbeat 去抖 + 主动恢复（需求 3）

`HealthConfig` 新增字段：

```cpp
struct HealthConfig {
    int watchdog_timeout_ms   = 5000;
    int heartbeat_interval_ms = 2000;
    int initial_backoff_ms    = 1000;
    int max_retries           = 3;
    int heartbeat_fail_threshold = 3;   // 新增：连续非 PLAYING 触发恢复的阈值
    int state_reset_timeout_ms   = 5000; // 新增：try_state_reset 等待 NULL/PLAYING 的上界
};
```

纯函数（便于单测）：

```cpp
// 连续非 PLAYING 计数达阈值即应触发恢复
bool should_trigger_recovery(int consecutive_non_playing, int threshold);
```

`heartbeat_timer_cb` 改为：

```cpp
GstState st = GST_STATE_NULL;
GstStateChangeReturn ret = gst_element_get_state(self->pipeline_, &st, nullptr, 0);
if (ret == GST_STATE_CHANGE_ASYNC) return G_SOURCE_CONTINUE;   // 瞬态切换中，不计
{
    std::lock_guard lock(self->mutex_);
    if (st == GST_STATE_PLAYING) {
        self->consecutive_non_playing_ = 0;
    } else {
        self->consecutive_non_playing_++;
    }
}
if (should_trigger_recovery(self->consecutive_non_playing_, self->config_.heartbeat_fail_threshold)) {
    self->consecutive_non_playing_ = 0;
    self->attempt_recovery();   // 主动驱动恢复，消除"标记 ERROR 后无限等待"
}
```

> 与现有 watchdog（无 buffer → DEGRADED）并存：watchdog 仍负责"有 PLAYING 但无数据"的场景；
> heartbeat 负责"状态非 PLAYING"。两者都通过 `attempt_recovery()` 的 `recovery_in_progress_`
> 重入保护串行化。`attempt_recovery` 内已处理 HEALTHY/DEGRADED→ERROR→RECOVERING 转换。

### 4. FATAL 优雅退出 + 看门狗健康门控（需求 4）

**sd_notifier 健康门控**：

```cpp
// sd_notifier.h 新增
static void set_health_check(std::function<bool()> is_healthy);  // 线程安全注册
```

```cpp
// sd_notifier.cpp：心跳线程内
if (!s_stop.load()) {
    bool healthy = true;
    { std::lock_guard lk(s_health_mtx); if (s_health_check) healthy = s_health_check(); }
    if (healthy) notify_watchdog();    // 非健康则跳过 → systemd WatchdogSec 兜底
}
```

`is_healthy()` 实现：查询 `PipelineHealthMonitor::state() != FATAL`（FATAL 时返回 false）。
线程安全由 `PipelineHealthMonitor::state()` 已有的 mutex 保证；sd_notifier 侧回调指针用 `s_health_mtx` 保护。

**FATAL → 优雅退出**：

```cpp
// app_context.cpp：health 回调
health_monitor->set_health_callback([this](HealthState o, HealthState n) {
    logger->info("Health state: {} -> {}", health_state_name(o), health_state_name(n));
    if (n == HealthState::FATAL && impl_->shutdown_requester) {
        logger->error("Pipeline FATAL — requesting graceful shutdown for systemd restart");
        impl_->shutdown_requester();   // 触发 main 的退出
    }
});
```

```cpp
// app_context.h：新增注册接口
void set_shutdown_requester(std::function<void()> fn);  // main 注入
```

`main.cpp`：

```cpp
static std::atomic<bool> g_fatal{false};
// ...注册：
ctx.set_shutdown_requester([]{ g_fatal.store(true); g_shutdown_requested.store(true); });
SdNotifier::set_health_check([&ctx]{ return ctx.is_healthy(); });  // ctx 暴露健康查询
// ...主循环退出后：
return g_fatal.load() ? EXIT_FAILURE : 0;   // 非零触发 systemd Restart=on-failure
```

> 退出走既有路径：`g_main_loop_quit` → `SdNotifier::notify_stopping()` → `ctx.stop()`（ShutdownHandler
> 逆序清理，per-step 5s / 总 30s 超时兜底）→ `return EXIT_FAILURE`。FATAL 回调在 GLib 主循环线程
> （attempt_recovery 调用栈）内执行，只设置 atomic flag，由 200ms 的 `check_shutdown` timer 退出主循环，
> 不在回调里直接 quit（避免在 recovery 调用栈中析构自身）。
> `AppContext::is_healthy()` 转发 `health_monitor->state() != FATAL`（health_monitor 为空时返回 true）。

### 5. 恢复 teardown 有界（需求 5）

**根因更正**：日志里的 ~130s 阻塞在 `gst_element_set_state(pipeline, GST_STATE_NULL)`（同步调用，
kvssink `change_state` 内 `stopStreamSync` 阻塞），**不在** `gst_element_get_state` 的等待。因此修复不是
"封顶 get_state 的等待"，而是**把会阻塞的 `set_state(NULL)` 移出 GLib 主循环**（决策 B 的有界异步 teardown）。

`try_state_reset` 改为基于同一 helper 的有界异步实现：

```cpp
bool PipelineHealthMonitor::try_state_reset() {
    // (b) 不转移所有权的有界 NULL：worker set_state(NULL)，主循环等 ≤ state_reset_timeout_ms(5s)
    if (!set_null_bounded(pipeline_, config_.state_reset_timeout_ms)) {
        // 超时：NULL 仍在进行（kvssink 卡住）。放弃复用：用 release() 取得所有权并交给 (a) 接管 unref，
        // 返回 false → attempt_recovery 走 full_rebuild（决策 B：detach + teardown_pipeline_bounded + 新建）。
        // 注：state_reset 与 full_rebuild 同处一次 attempt_recovery 调用，pipeline_ 由 detach/rebuild 流程接管，
        //     此处仅需保证不复用半 NULL 化的管道。
        logger->warn("state reset NULL not done within {}ms, hand off to full_rebuild",
                     config_.state_reset_timeout_ms);
        return false;
    }
    // NULL 在预算内完成 → set PLAYING 复用同一管道
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) return false;
    GstState actual = GST_STATE_VOID_PENDING;
    auto r = gst_element_get_state(pipeline_, &actual, nullptr,
                                   (gint64)config_.state_reset_timeout_ms * GST_MSECOND);
    return (r != GST_STATE_CHANGE_FAILURE && actual == GST_STATE_PLAYING);
}
```

> 注：state_reset 超时时，该"半 NULL 化"的旧管道在 full_rebuild 阶段经 `release()` 取得裸指针后交
> `teardown_pipeline_bounded`（版本 a）接管完成 NULL+unref；full_rebuild **新建**全新 pipeline（不复用）。
> `state_reset_timeout_ms` 同时作为 state-reset 与 full-rebuild teardown 的主循环等待预算。
> 两个 helper 的区别：`set_null_bounded`（b，不 unref，供复用尝试）vs `teardown_pipeline_bounded`（a，转移所有权+unref，供销毁）。

记录 state reset / teardown 实际耗时（steady_clock）写 info 日志，回归对比目标：单次主循环阻塞 ≤ 5s
（从 ~130s 降下来）。

**buffer-duration（次要、收益不确定）**：`KvsSinkConfig.buffer_duration_sec` 与 `StreamingConfig.buffer_duration_sec`
默认从 180 调到 **40**。这是次要杠杆——130s 主因是 `stopStreamSync` 网络超时而非排空慢，有界异步 teardown
（上面）才是关键。tradeoff：更小缓冲降低弱网抗抖动能力，40s 为折中；design 评估后若认为收益不足可保留 180
仅靠异步 teardown。

### 6. 默认码率下调（需求 6）

`StreamingConfig` 默认值：

```cpp
int bitrate_min_kbps     = 800;    // 1000 → 800（给 default 留下调空间）
int bitrate_max_kbps     = 1500;   // 4000 → 1500
int bitrate_default_kbps = 1200;   // 2500 → 1200（≤ 实测上传 1.7Mbps）
```

满足 `validate_streaming_config` 的 `min ≤ default ≤ max`（800 ≤ 1200 ≤ 1500）。
`config.toml.example` 同步注释与默认值。`avg-bandwidth-bps` 经 `to_kvssink_config` 自动跟随 default*1000。

### 7. CPU 基线诊断（需求 7）

新增 `scripts/diagnose-cpu.sh`（不改设备端代码）：

```bash
#!/bin/bash
# 采集 raspi-eye CPU 基线 + 检测 PAUSED，运行 >=10min
DURATION=${1:-600}
pid=$(pgrep -x raspi-eye)
pidstat -p "$pid" -t 5 "$((DURATION/5))" > /tmp/raspi-eye-cpu.log &   # 各线程 CPU
top -b -d 5 -n "$((DURATION/5))" -p "$pid" >> /tmp/raspi-eye-top.log &
journalctl -u raspi-eye -f --since now | grep -E "Heartbeat: PAUSED|Health state" > /tmp/raspi-eye-paused.log &
wait
# 判定：进程 CPU 持续接近 400%（四核）或 source 线程满载 → CPU 饱和与 PAUSED 强相关
```

判定标准与结论写 `docs/development-trace.md` + 更新 backlog（确认/排除 rotation CPU 风险）。

## Data Models

| 新增/修改 | 位置 | 说明 |
|-----------|------|------|
| `enum class ErrorScope` | pipeline_health.h | KVS_BRANCH/WEBRTC_BRANCH/TRUNK |
| `HealthConfig.heartbeat_fail_threshold=3` | pipeline_health.h | 去抖阈值 |
| `HealthConfig.state_reset_timeout_ms=5000` | pipeline_health.h | NULL/teardown 主循环等待预算 |
| `void detach()` | pipeline_health.h | 与旧管道解绑（决策 C） |
| `teardown_pipeline_bounded(GstElement*, int)` | pipeline_health（私有/helper） | (a) 转移所有权：worker set NULL+unref，主循环有界等待，超时 detach（方案 X） |
| `set_null_bounded(GstElement*, int)` | pipeline_health（私有/helper） | (b) 不转移所有权：worker 仅 set NULL（供 try_state_reset 复用），有界等待 |
| `int consecutive_non_playing_` | pipeline_health.cpp 成员 | heartbeat 计数器（主循环单线程访问） |
| `uint64_t branch_error_count_` | pipeline_health.cpp 成员 | 分支错误计数（观测） |
| `GstElement* PipelineManager::release()` | pipeline_manager.h | 放弃所有权返回裸指针、内部置空（不 stop/unref） |
| `struct OpenRetryConfig` | camera_source.h | max_attempts/interval_ms |
| `std::unique_ptr<PipelineManager> pending_pm_` | app_context.cpp Impl | rebuild 暂存新管道 |
| `StreamingConfig` 默认值变更 | config_manager.h | 码率 + buffer_duration |
| `std::function<bool()> s_health_check` | sd_notifier.cpp 静态 | 健康门控（s_health_mtx 保护） |
| `std::function<void()> shutdown_requester` | app_context.cpp Impl | FATAL→退出 |
| `std::atomic<bool> g_fatal` | main.cpp | 退出码区分 |

## Error Handling

- `classify_bus_error` 未知/空名 → TRUNK（保守恢复，不漏真故障）。
- 摄像头重试耗尽 → rebuild 返回 nullptr → 计入 recovery 失败 → 达 max_retries 进 FATAL → 优雅退出重启。
- state reset 超时 → 视为失败 → full rebuild（先销毁旧）。
- sd_notifier `set_health_check` 未注册（如测试环境）→ 默认 healthy=true（保持现有行为）。
- FATAL 优雅退出若 ShutdownHandler 卡死 → 既有 30s 总超时 `_exit(EXIT_FAILURE)` 兜底（已存在）。

## Testing Strategy

宿主机（macOS，无 kvssink/真实摄像头）单测，统一 `ctest --test-dir device/build --output-on-failure`：

| 测试 | 覆盖 | 类型 |
|------|------|------|
| `classify_bus_error` 各元素名 → 正确 scope；未知→TRUNK | 需求 1 | example + PBT |
| `should_trigger_recovery` 阈值边界；中途 PLAYING 清零 | 需求 3 | example + PBT |
| `open_with_retry` 前 N 次失败后成功 / 全失败 / 调用次数 / 注入假 sleep | 需求 2 | example + PBT |
| sd_notifier 健康门控：healthy→发送、FATAL→不发送（注入 health_check + 计数 notify） | 需求 4 | example |
| FATAL→shutdown_requester 被调用（注入回调，不真退出） | 需求 4 | example |
| `to_kvssink_config` / `validate_streaming_config` 新默认值通过区间校验 | 需求 5,6 | example |
| `PipelineManager::release()` 释放后壳为空、不 stop/unref，原指针仍有效 | 决策 B | example |
| `teardown_pipeline_bounded`：预算内完成返回 true；模拟慢 NULL（注入假 set_state）超时返回 false 且主线程不阻塞超预算 | 需求 5/决策 B | example（注入假 teardown fn） |

Pi 5 集成验证（人工，写 trace）：KVS 断网注入观察无整管道恢复 + 管道保持 PLAYING（决策 A）；
制造 FATAL 观察 systemctl 自动重启；CPU 基线（需求 7）。

## Correctness Properties

供 RapidCheck PBT 验证的不变量（针对纯函数，宿主机可测）：

### Property 1: classify_bus_error 全覆盖且确定
对任意字符串输入，`classify_bus_error` 必返回三个 scope 之一且同输入恒同输出；KVS 分支元素名集合恒返回 `KVS_BRANCH`，WebRTC 分支元素名集合恒返回 `WEBRTC_BRANCH`。
**Validates: Requirements 1.5, 1.6**

### Property 2: TRUNK 是保守默认
任意不在已知 KVS/WebRTC 元素名白名单内的输入（含空串/随机串）→ `TRUNK`（保证未知故障不会被误判为可忽略的分支错误）。
**Validates: Requirements 1.5**

### Property 3: should_trigger_recovery 单调阈值
`should_trigger_recovery(c, t)` 当且仅当 `c >= t && t > 0` 为真；对任意 `c < t` 恒为假（去抖不会提前触发）。
**Validates: Requirements 3.3, 3.5**

### Property 4: open_with_retry 次数有界且正确
对任意 `max_attempts ≥ 1`，调用 `try_open` 的次数 ∈ [1, max_attempts]；首次返回 true 的尝试即停止；全失败时恰好调用 `max_attempts` 次并返回 false；前 k 次失败、第 k+1 次成功（k < max_attempts）则返回 true 且恰好调用 k+1 次。
**Validates: Requirements 2.4**

### Property 5: validate_streaming_config 区间一致性
新默认值（min=800, default=1200, max=1500）满足 `min ≤ default ≤ max`；对任意满足该序关系的三元组校验通过，违反则失败。
**Validates: Requirements 6.1, 6.3**

## 文件改动清单（约束：≤ 一个 PR）

```
device/src/pipeline_health.h        (+ ErrorScope, classify_bus_error, HealthConfig 字段, detach(), teardown helper, 成员)
device/src/pipeline_health.cpp      (classify 实现, bus_watch 分流, heartbeat 去抖+恢复, detach, 有界异步 teardown, try_state_reset 重写)
device/src/pipeline_manager.h       (+ release())
device/src/pipeline_manager.cpp     (release() 实现)
device/src/kvs_sink_factory.cpp     (restart-on-error=TRUE)
device/src/camera_source.h          (+ OpenRetryConfig, open_with_retry 声明)
device/src/camera_source.cpp        (open_with_retry 实现)
device/src/app_context.h            (+ set_shutdown_requester, is_healthy)
device/src/app_context.cpp          (rebuild: detach+有界teardown+重试新建+pending_pm_, FATAL 回调, 健康查询)
device/src/sd_notifier.h            (+ set_health_check)
device/src/sd_notifier.cpp          (心跳门控)
device/src/main.cpp                 (g_fatal, 退出码, 注册回调)
device/src/config_manager.h         (StreamingConfig 默认值)
device/config/config.toml.example   (码率 + buffer_duration 注释/默认)
device/tests/...                    (新增/扩展单测)
scripts/diagnose-cpu.sh             (新增, 需求 7)
```

## 关键设计决策汇总

1. **决策 A（需求1↔3）**：主用 `restart-on-error=TRUE` + 错误域不升级 + heartbeat 去抖；Pi 实测确认
   kvssink 重连不拖累管道状态，否则启用 GstBin 状态隔离兜底。
2. **决策 B（需求2↔5，方案 X）**：`set_state(NULL)` 是同步阻塞（kvssink 卡 ~130s），必须**移出主循环**——
   旧管道置 NULL+unref 转移到 worker 线程，主循环有界等待 ≤5s 超时即 detach；新建带摄像头重试覆盖 fd 释放
   窗口；真卡死→恢复失败→FATAL→systemd 重启兜底。新增 `PipelineManager::release()`。
3. **决策 C（修悬空引用）**：销毁旧管道前先 `health_monitor->detach()`，固定顺序 detach→teardown→build→re-attach。
4. **优雅退出而非硬 _exit**：FATAL 设 atomic flag → 既有 200ms timer 退出主循环 → ShutdownHandler
   逆序清理（带 30s 兜底）→ 非零退出码触发 systemd 重启。
5. **诊断与修复分离**：需求 7（CPU 基线）只观测归因，不在本 spec 改旋转/编码方案。
