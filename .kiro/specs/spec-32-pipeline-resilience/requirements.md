# Requirements Document

> Spec 32: 管道韧性加固（KVS 瞬时故障不再拖垮整管道）

## Introduction

本 spec 修复设备端"KVS 云存与 WebRTC 实时观看同时停止数小时、需人工重启"的稳定性问题。
通过 Pi 5 生产日志锁定根因后，做一组**韧性加固**：让 KVS 瞬时故障在分支内自愈、不再拖垮共享管道，
并修复恢复机制本身的三个反效果缺陷（teardown 阻塞、摄像头占用 race、检测与恢复脱节）以及
"FATAL 后进程假活、systemd 不重启"的保活失效。目标：将"冻几小时+人工重启"变为"分支自愈或秒级自动重启"。

### 前置条件
- spec-5（pipeline-health）已通过验证 ✅
- spec-20（systemd-watchdog）已通过验证 ✅
- spec-8（kvs-producer）已通过验证 ✅

## Glossary

- **共享管道（shared pipeline）**：单条 GStreamer tee 管道，KVS / WebRTC / AI 三分支共用上游 camera + encoder。
- **错误域（ErrorScope）**：bus 错误的归属分类——`KVS_BRANCH`（kvs-sink/kvs-parser/q-kvs）、
  `WEBRTC_BRANCH`（webrtc-sink/q-web）、`TRUNK`（src/encoder/tee 等上游）。
- **分支级错误**：仅影响单条分支、可由该分支或其 SDK 自愈的错误，不应触发整管道恢复。
- **整管道恢复**：`PipelineHealthMonitor::attempt_recovery()` 触发的 state reset / full rebuild，会中断所有分支。
- **去抖（debounce）**：连续多次观测到异常才判定，过滤瞬态（如新建管道初始化期的短暂 PAUSED）。
- **看门狗门控**：`sd_notifier` 心跳按真实健康状态决定是否发送 `WATCHDOG=1`。
- **HealthState**：`PipelineHealthMonitor` 状态机枚举 HEALTHY/DEGRADED/ERROR/RECOVERING/FATAL。

## 背景与根因（来自 Pi 5 生产日志 2026-06-07 ~ 06-09）

设备端 KVS 云存与 WebRTC 实时观看会同时停止数小时，需手动重启。完整因果链已通过 `journalctl` 日志锁定：

1. **触发源**：一次瞬时网络抖动导致 KVS Producer SDK 拉取 IoT 凭证 SSL 超时
   （`blockingCurlCall ... SSL connection timeout` → `getStreamingTokenResultEvent status 0x16000001`）。
   KVS SDK 自带 `continuousRetryStreamErrorReportHandler`，30 秒后凭证刷新成功、本可自愈。
2. **放大**：kvssink 配了 `restart-on-error=FALSE`，瞬时错误被抛到 GStreamer bus
   （`Bus ERROR from kvs-sink: general stream error`）→ `PipelineHealthMonitor::attempt_recovery()`
   把**整条共享管道**（KVS + WebRTC + AI 同一 tee 管道）拖入恢复。
3. **恢复反而制造新故障**（恢复的伤害 > 它要修的问题）：
   - `set_state(NULL)` 触发 kvssink `stopStreamSync` 排空 180s 缓冲 → 超时阻塞 **~2 分钟**，
     期间 GLib 主循环冻死，WebRTC 信令一起停。
   - 快速重建时旧管道的 `v4l2src` 尚未释放 `/dev/IMX678` 的 fd → 新管道
     `Could not open device '/dev/IMX678'` → 重建返回 nullptr。
   - 新建管道里 kvssink 仍在连接，管道短暂处于 PAUSED → heartbeat 用 `timeout=0` 查询即误判 ERROR。
4. **检测与动作脱节**：`heartbeat_timer_cb` 检测到非 PLAYING 只 `transition_to(ERROR)`，**从不调用
   `attempt_recovery()`**。管道冻在 PAUSED，直到数小时后偶然来一条 bus ERROR 才触发恢复
   （日志实测 20:36 → 23:19 冻结 2 小时 43 分钟）。
5. **FATAL 死胡同**：3 次恢复失败进入 FATAL（终态），`app_context` 的 health 回调只打日志、进程不退出；
   `sd_notifier` 看门狗线程**无条件**每 15s 发 `WATCHDOG=1`，systemd 收到心跳 → 永不触发
   `Restart=on-failure`。结果功能全死但进程"健康"，只能人工重启。

底层频率因素（来自 backlog 弱网项）：Pi 到 KVS ap-southeast-1 实际上传仅 ~1.7 Mbps，
而默认编码 2.5 Mbps，KVS 长期积压、putMedia 反复断重连，放大了 kvssink 报错频率。

## 准入三问

- **单一验证条件**：本 spec 是一组韧性加固，无法用单条断言验证。改为"分场景验证"——每条需求
  配一个可复现/可单测的验证（见各需求的验证标准 + 末尾"验证"汇总）。已与用户确认采用多场景验证。
- **合理粒度**：改动量处于一个 PR 的上限（约 8 个源文件 + 1 个配置 + 1 个诊断脚本 + 测试），Task 7 个。
  方案 X（有界异步 teardown）较初版多了 `pipeline_manager.{h,cpp}` 的 `release()`。不改已验证模块
  的核心数据通路（tee 结构、编码链、WebRTC peer 生命周期不动），只改"健康监控的判定/恢复策略"
  与"故障注入点的容错"。
- **明确边界**：见末尾"明确不包含"。本 spec 不做分支级独立 bin 架构重构，不做自适应码率接线，
  不换 region。

## Constraints（约束，可量化）

- 目标平台：Raspberry Pi 5（Debian Bookworm aarch64，GCC），生产 Release；macOS 开发用 stub/videotestsrc。
- 语言/风格：C++17，遵循 `.kiro/steering/cpp-standards.md`（RAII、pImpl、`.h/.cpp` 分离、ASCII 日志）。
- 测试：GTest + CTest 统一运行（`ctest --test-dir device/build --output-on-failure`），可单测的逻辑必须有 PBT/example 测试。
- 不引入新的第三方依赖。
- 性能预算：恢复流程对 GLib 主循环的单次阻塞 ≤ 5s（当前实测 ~130s）。健康判定额外 CPU 开销可忽略（仅状态机 + 计数）。
- 兼容性：健康监控对外接口（`HealthState` 枚举、`set_health_callback`、`set_rebuild_callback`）保持向后兼容，不破坏现有单测语义。

## Requirements

### 需求 1：KVS 分支错误自愈，不再触发整管道恢复

WHEN kvssink 因瞬时网络/凭证错误向 GStreamer bus 抛出 ERROR
THE system SHALL 将该错误识别为"分支级（KVS）错误"并记录上报，**不**调用整管道的 `attempt_recovery()`
AND kvssink 通过 `restart-on-error=TRUE` 由 KVS Producer SDK 自行重连恢复
AND WebRTC 分支与摄像头采集不受影响，继续正常工作。

验收标准：
1. `kvs_sink_factory.cpp` 创建 kvssink 时设置 `restart-on-error=TRUE`。
2. `PipelineHealthMonitor::bus_watch_cb` 对 `GST_MESSAGE_ERROR` 按消息源（`GST_OBJECT_NAME(msg->src)`）
   分类：源为 KVS 分支元素（`kvs-sink`/`kvs-parser`/`q-kvs`/`avc-caps`）时记为 `KVS_BRANCH` 错误，仅 warn 日志 + 计数，
   不触发 `attempt_recovery()`；源为 trunk 元素（src/convert/flip/capsfilter/raw-tee/encoder/parser/bs-caps/encoded-tee）时记为 `TRUNK` 错误，触发整管道恢复。
3. **WEBRTC_BRANCH 错误**（`webrtc-sink`/`q-web`）同样记为分支级、仅日志 + 计数、**不**触发整管道恢复（对称隔离，避免 WebRTC 分支问题拖垮 KVS）；appsink 异常本就不应导致整管道重启。
4. **关键不变量（实现阶段 Task 7 在 Pi 上确认——宿主机无 kvssink 无法验证）**：`restart-on-error=TRUE` 时 kvssink 在内部重连期间必须停留在 PLAYING 状态、不把管道聚合状态拉到 PAUSED；否则需求 3 的 heartbeat 会绕过本需求重新触发整管道恢复。Task 7 用 Pi 实测 `GST_DEBUG=GST_STATES:4` 确认；若不成立则改用兜底（把 kvssink 放入独立 `GstBin` 吸收其状态变化）。
5. 提供一个纯函数 `classify_bus_error(const std::string& src_name) -> ErrorScope`（KVS_BRANCH /
   WEBRTC_BRANCH / TRUNK），可被单测覆盖；未知/空名按 `TRUNK` 处理（保守：宁可恢复也不漏）。
6. 单测：给定各元素名，`classify_bus_error` 返回正确的 scope（example + PBT：任意 `kvs-*`/`webrtc-*`/`q-web`/其它前缀）。

### 需求 2：恢复/重建时摄像头打开带重试，消除设备占用 race

WHEN 整管道恢复需要重建、且 `v4l2src` 打开 `/dev/IMX678` 因旧 fd 未释放而失败（device busy / Could not open device）
THE system SHALL 以固定间隔重试打开摄像头（默认 500ms × 最多 6 次，总计 ≤ 3s）
AND 在重试耗尽后才判定重建失败
AND 重试次数与间隔可配置。

验收标准：
1. 摄像头打开重试逻辑作为独立可测函数实现（接受一个"尝试打开"的 callable + 重试参数，返回成功/失败），
   不直接依赖 GStreamer，便于单测。重试粒度是"整次重建尝试（build + set PLAYING）"，不是裸 fd open。
2. 重建路径（`app_context` rebuild callback）在 source 启动失败时走该重试逻辑。
3. **时序约束（与需求 5 协调）**：rebuild 构造新 v4l2src 前，必须先发起旧管道释放——固定顺序为
   `health_monitor->detach()` → 把旧管道交给**有界异步 teardown**（worker 线程 set NULL+unref，主循环封顶 ≤5s）→ 再带重试构建新管道。
   异步 teardown **不要求**在构建新管道前 unref 完成；摄像头重试（本需求）负责覆盖"旧 fd 尚未释放"的窗口，
   必要时把重试上限提到覆盖最坏 fd 释放延迟。design 阶段（决策 B/C）明确该顺序，避免"超时后立刻重建却撞上旧 fd"。
4. 单测：模拟前 N 次失败后成功 → 函数返回成功且调用次数正确；全部失败 → 返回失败且不超过上限。
5. 日志记录每次重试（warn）与最终结果。

### 需求 3：heartbeat 检测到异常主动触发恢复，并对瞬态 PAUSED 去抖

WHEN heartbeat 连续检测到管道处于非 PLAYING 状态达到去抖阈值（默认连续 3 次，间隔 2s）
THE system SHALL 主动调用 `attempt_recovery()`（而非仅标记 ERROR 后无限等待）
AND WHEN `gst_element_get_state` 返回 `GST_STATE_CHANGE_ASYNC`（状态切换中）THE system SHALL 不计入异常计数（视为瞬态）
AND 单次瞬态 PAUSED（如新建管道初始化期）不应触发恢复。

验收标准：
1. `heartbeat_timer_cb` 检查 `gst_element_get_state` 的**返回值**：`ASYNC` 时跳过本轮判定。
2. 引入连续非 PLAYING 计数器，达到阈值（默认 3）才 `transition_to(ERROR)` 并触发 `attempt_recovery()`；
   一旦观测到 PLAYING 立即清零。
3. 去抖阈值与计数逻辑作为纯函数 `should_trigger_recovery(consecutive_non_playing, threshold) -> bool` 可单测。
4. 消除"检测到 ERROR 后无限等待直到 bus ERROR"的路径——ERROR 状态必然在有限时间内进入 RECOVERING。
5. 单测覆盖：连续 N 次非 PLAYING 触发；中间穿插 PLAYING 清零；ASYNC 不计数。

### 需求 4：FATAL 时进程退出交由 systemd 重启，看门狗心跳与健康挂钩

WHEN `PipelineHealthMonitor` 进入 FATAL 状态
THE system SHALL 触发**优雅退出**（停止看门狗心跳 → 退出 GLib 主循环 → 走正常 ShutdownHandler 清理流程 → 进程以非零码退出），交由 systemd `Restart=on-failure` 拉起干净进程
AND WHEN 健康状态为 FATAL（或管道非运行）THE 看门狗心跳 SHALL 停止发送 `WATCHDOG=1`，使 systemd `WatchdogSec` 能感知并重启
AND 正常运行（HEALTHY/DEGRADED/RECOVERING）时看门狗心跳照常发送。

验收标准：
1. health 回调在 `new_state == FATAL` 时触发**优雅退出**：通过既有的主循环 shutdown 机制（与 SIGTERM 同路径，设置 `g_shutdown_requested` 或等效）让 `main` 退出主循环 → `AppContext::stop()` → `log_init::shutdown()`，最终 `return EXIT_FAILURE`。不使用硬 `_exit`（保留 ShutdownHandler 已有的 30s 总超时兜底，避免清理卡死）。
2. 退出时 KVS/WebRTC/各模块按现有 ShutdownHandler 逆序清理，尽量干净释放（kvssink 排空、peer 关闭等），由 ShutdownHandler 的 per-step 5s + 总 30s 超时保护防止卡死。
3. `sd_notifier` 看门狗心跳改为"按健康状态门控"：提供一个查询当前是否健康的回调/标志，非健康（FATAL）时跳过 `notify_watchdog()`；优雅退出阶段 `notify_stopping()` 照常发送。
4. 退出路径需保证 `StartLimitBurst=5/60s` 不会因频繁误退出而耗尽（配合需求 3 去抖，避免误判导致的退出循环）。
5. 单测：看门狗门控逻辑（健康→发送、FATAL→不发送）可单测；FATAL→优雅退出钩子被调用（用回调注入验证，不真的退出进程）。

### 需求 5：恢复 teardown 有界，不阻塞主循环 2 分钟

WHEN 恢复执行 `try_state_reset()` 的 `set_state(NULL)`
THE system SHALL 限制其等待时间（默认 ≤ 5s），超时即放弃 state reset 直接走 full rebuild，不无限等待 kvssink 缓冲排空
AND full rebuild 释放旧管道时同样不应让主循环阻塞超过预算。

验收标准：
1. **（主要杠杆）** 把会阻塞的 `gst_element_set_state(pipeline, GST_STATE_NULL)` **移出 GLib 主循环**：在 worker 线程执行，主循环最多等 `state_reset_timeout_ms`（≤5s）。根因更正——日志实测 ~130s 阻塞在**同步的 `set_state(NULL)`**（kvssink `change_state` 内 `stopStreamSync` 网络超时），**不在** `get_state` 等待；因此不能靠"封顶 get_state"解决，必须异步化 set_state(NULL)（见 design 决策 B / Component 5）。
2. **（次要、收益不确定）** 评估降低 kvssink `buffer-duration`（当前 180s）：理论上更小的缓冲能缩短正常排空时间，但对网络阻塞型超时帮助有限，且会降低弱网下的抗抖动缓冲能力（tradeoff）。design 阶段评估后给一个折中默认（建议 ≤ 40s）并在 `KvsSinkConfig` 暴露；若评估认为收益不足可不改，仅靠异步 teardown（验收标准 1）。
3. 与需求 2 的时序协调：teardown 主循环封顶超时后进入 rebuild，必须配合需求 2 验收标准 3（摄像头重试兜底）避免撞上未释放的旧 fd。
4. 日志记录 state reset / teardown 实际耗时，便于回归对比（目标：单次主循环阻塞从 ~130s 降到 ≤ 5s）。

### 需求 6：降低默认编码码率到上传带宽以下，减少 KVS 积压触发频率

WHEN 设备使用默认配置运行
THE system SHALL 使用一个不超过实测上传带宽（~1.7 Mbps）的默认编码码率（建议 default 1200 kbps，max ≤ 1500 kbps）
AND kvssink `avg-bandwidth-bps` 与编码码率匹配
AND 该值可通过 config.toml 覆盖。

验收标准：
1. `config.toml.example` 与 `StreamingConfig` 默认值调整为 `bitrate_default_kbps≈1200`、`bitrate_max_kbps≤1500`（具体值在 design 阶段定）；并确保 `bitrate_default_kbps` 落在 `[bitrate_min_kbps, bitrate_max_kbps]` 区间内（config 已有该校验），必要时同步调整 `min`。
2. 不改变自适应码率的逻辑结构（本 spec 只调默认值，不接线 streamLatencyPressure——那是独立 backlog 项）。
3. 验证：config 解析单测确认新默认值且通过区间校验；Pi 上运行确认 KVS latency pressure / putMedia 断连频率显著下降（人工观察日志）。

### 需求 7：采集 Pi CPU 占用基线，排查 PAUSED 是否由 CPU 饱和诱发

WHEN 设备在 Pi 5 上正常运行（FULL 模式、含 rotation=180 软件 videoflip + jpegdec + x264enc + YOLO）
THE system SHALL 提供一种低开销的方式记录整机/进程 CPU 占用基线，用于判断管道 PAUSED 是否由 CPU 饥饿（source/encoder 跟不上）诱发
AND 该诊断不引入常驻高频开销，不改变数据通路。

验收标准：
1. 提供采集步骤/脚本：在 Pi 上运行 ≥10 分钟，记录 `raspi-eye` 进程 CPU%、整机 load average、各线程占用（如 `top -H`、`pidstat`），并记录是否出现 `Heartbeat: PAUSED`。
2. 给出判定标准：若进程 CPU 持续接近 `核数×100%`（Pi 5 四核即接近 400%）或 source 线程频繁满载，则 PAUSED 与 CPU 饱和强相关，需后续单开 spec 优化旋转/编码（如硬件旋转、降分辨率/帧率）；否则排除 CPU 因素，PAUSED 归因于 KVS/摄像头瞬时故障（已由需求 1/2/3 覆盖）。
3. 本需求只做**观测与归因**，不改旋转/编码方案（方案优化留后续 spec）。
4. 结论写入 `docs/development-trace.md`，并据此更新 backlog（确认或排除 rotation CPU 风险）。

## SHALL NOT（禁止项，分三层）

按 `.kiro/steering/shall-not.md` 检查后提炼，本 spec 相关：

**Requirements 层**
- SHALL NOT 把 KVS 分支的瞬时错误升级为影响 WebRTC/摄像头的全局动作（本次故障的核心教训：故障域必须隔离）。

**Design 层**
- SHALL NOT 在 GLib 主循环线程中执行可能长时间阻塞的同步操作（`set_state(NULL)` 排空 kvssink 缓冲实测阻塞 130s）。恢复 teardown 必须有超时上界。
- SHALL NOT 让"检测"与"恢复动作"脱节——任何能进入 ERROR 的检测路径都必须在有限时间内驱动恢复或退出，不允许"标记后无限等待"。
- SHALL NOT 让进程级看门狗（`sd_notifier`）无条件发送心跳；心跳必须反映真实健康状态，否则 systemd 保活失效（本次故障：FATAL 后进程"假活"数小时）。
- SHALL NOT 在看门狗线程与主循环之间用非线程安全方式共享健康状态；健康标志必须用 `std::atomic` 或加锁（看门狗线程读、主循环写）。

**Tasks 层**
- SHALL NOT 在恢复重建路径中假设摄像头设备 fd 立即可用（USB IMX678 释放有延迟，必须重试打开）。
- SHALL NOT 直接运行测试可执行文件，必须 `ctest --test-dir device/build --output-on-failure`。
- SHALL NOT 在 `g_print`/日志中使用非 ASCII；不输出凭证/证书内容。

## 参考代码（关键改动点定位 — **以 design.md 为准，下面仅为定位提示，部分已被 design 方案 X 细化/取代**）

```cpp
// kvs_sink_factory.cpp — 自愈
g_object_set(G_OBJECT(sink), "restart-on-error", TRUE, ...);

// pipeline_health.cpp — bus 错误按源分类（需求 1）
GstBusSyncReply/bus_watch_cb: 
  auto scope = classify_bus_error(GST_OBJECT_NAME(msg->src));
  if (scope == ErrorScope::TRUNK) attempt_recovery();
  else { log+count; /* KVS/WEBRTC 分支：不 attempt_recovery */ }

// pipeline_health.cpp — heartbeat 去抖 + 主动恢复（需求 3）
GstState st; auto ret = gst_element_get_state(pipeline_, &st, nullptr, 0);
if (ret == GST_STATE_CHANGE_ASYNC) return G_SOURCE_CONTINUE;   // 瞬态不计
if (st != GST_STATE_PLAYING) { if (++consecutive >= 3) attempt_recovery(); }  // attempt_recovery 内部做 transition
else consecutive = 0;

// app_context.cpp — FATAL → 优雅退出（需求 4，详见 design Component 4）
// health_cb 仅设 flag，不在回调里 quit；看门狗心跳由 sd_notifier 按 is_healthy() 门控
health_cb = [this](old,new){ if (new == HealthState::FATAL && shutdown_requester_) shutdown_requester_(); };
```

## 验证（分场景）

| 需求 | 验证方式 | 预期 |
|------|---------|------|
| 1 | 单测 `classify_bus_error`；Pi 上故障注入（断网几十秒）观察日志 | KVS 自愈、无整管道恢复、WebRTC 不中断 |
| 2 | 单测重试函数（前 N 次失败后成功 / 全失败）；Pi 上恢复时观察无 `Could not open device` 致命 | 重建不再因摄像头占用失败 |
| 3 | 单测 `should_trigger_recovery` + ASYNC 不计数；Pi 上观察 ERROR→RECOVERING 间隔有限 | 无"冻几小时"窗口 |
| 4 | 单测看门狗门控 + FATAL 退出钩子；Pi 上制造 FATAL 观察 `systemctl` 自动重启 | 进程退出并被 systemd 拉起 |
| 5 | 日志记录 state reset 耗时 | 单次主循环阻塞 ≤ 5s |
| 6 | config 解析单测；Pi 上观察 latency pressure 频率 | 默认码率 ≤ 上传带宽，积压显著减少 |
| 7 | Pi 上采集 CPU 基线 ≥10min + 是否出现 PAUSED | 确认或排除 CPU 饱和诱发 PAUSED，结论入 trace |

**整体回归命令**：`ctest --test-dir device/build --output-on-failure` 全绿；
Pi 5 上 `scripts/pi-build.sh` 编译通过；生产运行 ≥ 24h 无"KVS+WebRTC 同时长时间停止"。

## 明确不包含（Out of Scope，留待后续 spec）

- **分支级独立 bin 架构重构**：把 KVS / WebRTC / AI 拆成可独立启停恢复的子 bin（更彻底的故障隔离）。本 spec 只做"错误域分类 + 不升级"，不重构 tee 结构。（对应 review 根因 5 的彻底版）
- **streamLatencyPressure / writeFrame 健康信号接线到 BitrateAdapter / StreamModeController**（backlog 既有项）：当前自适应码率与流模式切换因信号未接线而处于"死代码"状态。本 spec 不接线——改为用需求 6（静态降低默认码率）作为务实替代止血。（对应 review 问题 6）
- **KVS 弱网根治**：换 region（东京/香港）、动态码率匹配上传带宽的完整方案。本 spec 只调默认码率止血。
- **WebRTC 信令/媒体的重连策略变更**（已有 spec-13.6/13.7 覆盖）。
- **视频旋转（rotation=180）的 CPU 方案优化**：当前用软件 `videoflip`，叠加 jpegdec(MJPG) + x264enc + YOLO 后是潜在 CPU 压力源。本 spec 用需求 7 **采集 CPU 基线确认/排除**这一诱因，但**不做**旋转方案优化（硬件旋转、降分辨率等）——若需求 7 确认 CPU 饱和，再单开 spec。（对应 review 问题 8）
- **将整个 recovery 逻辑改为独立线程驱动**：恢复的编排（state reset / rebuild / 状态机）仍由 GLib 主循环驱动；本 spec 只把其中**会阻塞的 `set_state(NULL)` teardown** 放到 worker 线程（方案 X，决策 B），不重写恢复编排为独立线程模型。（对应 review 问题 7 的彻底版）
