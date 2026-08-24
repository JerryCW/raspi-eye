# Implementation Plan

> Spec 33: WebRTC 长期运行韧性修复

## Overview

按“失活取证 → Pi SDK 语义决策门 → signaling owner/runtime → message dispatcher（Stage 1 部署门）→ peer/bridge/Reaper → 媒体与资源上限（Stage 2 部署门）→ 双平台与分层长稳”拆为 7 个 Task（0～6）。

分阶段部署原则（design 决策 C）：开发不等待观察窗口，但部署节奏必须分 Stage——Stage 1（Task 2～3，signaling 层）先部署并记录观察窗口，Stage 2（Task 4～5，peer/media 层）部署前先归档 Stage 1 窗口结论，用于失活根因的归因二分。

生产与测试改动为 6 个既有文件（Task 0 零生产代码，取证工具 `scripts/pi-diagnose.sh` 与 SDK 探针 `scripts/webrtc-sdk-probe.sh` 已在仓库中）：

```text
device/src/webrtc_signaling.h
device/src/webrtc_signaling.cpp
device/src/webrtc_media.h
device/src/webrtc_media.cpp
device/tests/webrtc_test.cpp
device/tests/webrtc_media_test.cpp
```

这是超过 2～5 文件建议值的显式例外：两个既有测试目标都含必须修复的脆弱测试；不修改 CMake、不新增测试 target，减少构建面变化。

统一宿主机验证：

```bash
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug
cmake --build device/build
ctest --test-dir device/build --output-on-failure
```

Pi 仅构建用 `./scripts/pi-build.sh`；安装 binary 并重启 systemd 用 `./scripts/pi-deploy.sh`。不得直接运行单个测试可执行文件。

## Tasks

- [x] 0. 失活取证与假设分类（零生产代码，与 Task 1 并行）
  - 用 `scripts/pi-diagnose.sh`（macOS 远程模式）分析最近 72h journald 日志，统计三类失活签名：`Cannot send answer: signaling not connected` 反复出现且 offer 仍在到达（假设 A：僵尸 signaling，connected 标志永久 false 且无 recreate）；webrtc logger 某时刻后完全静默、连 DISCONNECTED 都没有（假设 B：offer 处理占住 SDK 消息线程 + peers_mutex 锁序反转死锁）；`Reconnect attempt` 秒级突发（假设 C：长断网后退避位移溢出）
  - 若设备当前正处于失活状态：先运行 `scripts/pi-diagnose.sh` 采集 gdb 全线程栈再重启服务；重点确认是否有线程阻塞在 `on_viewer_offer`、`signalingClientSendMessageSync` 或 `peers_mutex` 上
  - 将假设排序结论（A/B/C/不确定）与证据摘要写入 `docs/development-trace.md`，作为 Stage 1/Stage 2 部署观察的归因基线
  - 若日志已轮转无法取证，保持当前版本继续暴露，下次失活即取证；本任务不阻塞 Task 1～2 开发
  - 验证：`docs/development-trace.md` 中有带日期的取证记录与假设排序；`git status` 无生产代码 diff
  - SHALL NOT 在取证结论产出前将 root cause 写死为单一缺陷；SHALL NOT 为取证修改生产代码
  - _Requirements: 7.5_

- [x] 1. 完成 Pi 当前 SDK 语义决策门，BLOCKED 时停止后续实现
  - 在 Pi 当前安装的 KVS WebRTC C SDK 头文件/源码中记录 SDK version/commit、证据路径和确认日期（`scripts/webrtc-sdk-probe.sh` 辅助采集）；不得引用 spec-13.6/13.7 的历史假设代替源码
  - 分阶段确认 signaling create/fetch/connect/send/query/free 的有限上界、内部 timeout、允许状态、callback 顺序、与 free 并发规则；为 send/query 记录用于完成 deadline admission 的 `max_duration`（QUERY 2s / SEND 10s；send 上界超过 10s 则 SEND 路径标 BLOCKED 并回 Spec，见决策 D）
  - 确认 `deinitKvsWebRtc()` 是否停止并 join 全部 SDK 内部线程（bridge 池析构的前置；BLOCKED 只影响 Impl 析构收尾，用进程退出兜底并记录，不阻塞运行时池化实现）；free 后 per-handle callback quiescence 仅作补充证据采集；确认 Peer create/description 是否同步 callback
  - 确认 `writeFrame` 对 frame data 的所有权、阻塞上界、与 close/free 并发规则；确认 close/free 阻塞上界与幂等；从当前头文件确认 SRTP-not-ready 符号
  - 确认 `initKvsWebRtc/deinitKvsWebRtc` 的进程级语义，以及 signaling 与 peer 共享 runtime 的合法释放顺序
  - 在 `design.md` 的 SDK Semantic Gates 表填写 Evidence、Status（CONFIRMED/BLOCKED）、Date 和影响范围；reconnect 决策默认 A2（`reconnect=FALSE` + 应用全权 recreate），仅当源码证明 SDK 内置 reconnect 安全且不与 free 竞态时才可改选 A1
  - 若任一后续**运行时路径**依赖 BLOCKED 项，立即停止对应 Task 并先修订三件套；仅影响析构收尾的 deinit BLOCKED 记录兜底方案后可继续；不得编写 production adapter、不得用一次成功实测或固定 sleep 宣称契约成立
  - 验证：表格无 PENDING；每个 CONFIRMED 有源码位置，最小实测只作补充；BLOCKED 项具有明确停止或兜底结论
  - SHALL NOT 在不确定 SDK API 时凭猜测编码；SHALL NOT 为绕过阻塞创建 detached/per-call worker
  - _Requirements: 1.3, 2.1, 4.1, 4.2, 4.4, 5.3, 5.4, 5.6, 7.5, 8.1, 8.2, 8.3, 8.4_

- [x] 2. 建立 SDK seam、共享 runtime 与 SignalingOwner 状态机
  - 在 `webrtc_signaling.h` 补齐 C++17 include，定义 Design Component 1 的 `RuntimeOptions`、`RuntimeClock`、`IceServerRecord`、`SignalingSdkOps` 和精确 `create_for_test(...)`；internal 接口不得暴露 SDK handle、凭证或绝对路径
  - 在 `webrtc_signaling.cpp` 实现 production/fake adapter 的 staged create/fetch/connect/send/query/release；真实 handle、credential 和 SignalingCallbackBridge 仅由 adapter/owner 持有
  - 实现共享 `KvsRuntimeToken` refcount；client recreate 不重复 init/deinit，最后一个 signaling/peer handle 释放后才 deinit
  - 用单 SignalingOwner、容量 256 普通队列、带外 SHUTDOWN 和按 generation 合并的 ControlMailbox 替换 `reconnect_thread_`、`needs_reconnect_` 与跨线程普通字段
  - Command 必含 id/submit_generation/completion_deadline（QUERY 2s / SEND 10s，`RuntimeOptions::send_completion_deadline`）/atomic state/reply；owner 调 SDK 前重检 admission、generation、state 和 `now()+max_duration<=completion_deadline`；完成 exactly once；SEND 类 caller 2s 超时不取消命令，命令在完成 deadline 内仍执行并兑现 promise（迟到无害，决策 D）
  - 实现 `STOPPED→CREATING→FETCHING→CONNECTING→CONNECTED→RECOVERING→STOPPING`；`connect/reconnect==true` 只表示 staged API 链成功，CONNECTED callback 才发布 `is_connected=true`
  - 实现 30s CONNECTED deadline、`observed_at<=deadline` 胜负、1/2/4/8/16/30s 饱和退避、稳定 30s 重置，以及 Task 1 选定的 A1/A2；禁止同 handle 第二次 ConnectSync
  - shutdown 顺序固定为关闭普通 admission → 等 Task 3 关闭/join dispatcher → 带外 SHUTDOWN → owner release/退出 → join owner → runtime token；不得在 join 后要求 owner release
  - 在 `webrtc_test.cpp` 增加 staged failure（含首次 connect 全阶段失败后延迟恢复的启动场景）、10,000 次恢复、deadline 边界、普通洪水不饿死控制面、QUERY 取消/零副作用、SEND caller 超时后迟到 exactly-once 完成、单 owner 和 release 后无调用测试
  - 验证：CTest + ASan；普通队列≤256；10,000 序列≤15s；所有 handle 操作线程 id 唯一
  - SHALL NOT 使用 combined `create_fetch_connect()`；SHALL NOT 用 `1 << attempt`；SHALL NOT 让 QUERY 类 caller timeout 后的 QUEUED command 执行或任何 command 越过完成 deadline 启动 SDK 调用
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 2.1, 2.2, 2.3, 2.4, 2.5, 6.1, 7.1, 7.2, 8.1, 8.2, 8.3, 8.4_
- [x] 3. 实现安全 SignalingCallbackBridge 与有界 MessageDispatcher
  - 在 `webrtc_signaling.cpp` 让 signaling bridge 为 Impl 内固定 slot（注册前就绪、Impl 生命周期内不释放，决策 B）；callback 入口 `fetch_add(in_flight)` → 校验 open/generation → 失配递减返回并计 stale；client recreate 仅执行 open=false → release → generation 递增 → 复开，in-flight 归零前不进入下一 generation
  - 所有 C ABI state/message callback 使用 `try/catch (...)`；state callback 只向 ControlMailbox 发布 generation/state/observed_at，不执行外部 API 或等待业务锁
  - message callback 在分配前校验 peer_id≤256B、payload≤16KiB，只复制并 try-enqueue；容量 512，OFFER 满队列时淘汰最旧 ICE，全 OFFER 时拒绝新 OFFER，新 ICE 满时丢弃
  - 单 MessageDispatcher FIFO 调用 immutable handler snapshot；同 peer OFFER 不并行；旧 generation message 只计 stale
  - shutdown 关闭 message admission、丢弃未开始任务并 join dispatcher，再允许 Task 2 owner release；析构后不得调用用户 handler
  - 在 `webrtc_test.cpp` 用事件门禁阻塞 handler，结构性断言 SDK callback 不调用 handler/Peer SDK、不等待业务锁；覆盖 513+ overflow、异常路径、generation、handler 替换与 shutdown
  - 日志按类首次立即输出，此后 60s 最多一条并附 suppressed，10min 安静后重置；不得输出 SDP 全文/credential/token
  - **Stage 1 部署门**：Task 2～3 完成且宿主机 CTest 全绿后，用 `./scripts/pi-build.sh` 构建、`./scripts/pi-deploy.sh` 部署 signaling 层修复到 Pi，在 `docs/development-trace.md` 记录部署时间与观察窗口起点（目标 ≥ 2× 历史失活间隔）；窗口内失活复现即用 `scripts/pi-diagnose.sh` 取证并对照 Task 0 基线更新假设排序；观察窗口不阻塞 Task 4～5 开发
  - 验证：CTest；message queue≤512；handler thread id 与 SDK callback thread id 不同；wall-clock 只作统计
  - SHALL NOT 在 SDK callback 执行业务处理；SHALL NOT 让 C++ 异常越过 C ABI
  - _Requirements: 1.4, 1.5, 2.3, 2.5, 3.1, 3.2, 3.3, 3.4, 4.1, 4.2, 6.1, 6.3, 7.1, 7.3_

- [x] 4. 以 PeerSession、CallbackBridge、HandlePermit 和 Reaper 重构 peer 生命周期
  - 在 `webrtc_media.h` 定义 Design Component 1 的 `PeerHandle`、`PeerCallbacks`、`PeerSdkOps` 与精确 `create_for_test(...)`；补齐必要 include
  - 在 `webrtc_media.cpp` 将 peer map value 改为 `std::shared_ptr<PeerSession>`；session 保存 generation、HandlePermit、state、slot 指针（不拥有）、opaque handle、I/O gate、keyframe flag 和统计
  - 实现容量 16 的 HandlePermit 池与一一绑定的 16 个 PeerCallbackBridge 固定 slot（Impl 生命周期内不释放，决策 B）；permit 从 create 前持有到 free 完成且 slot in-flight==0；active≤10，active+creating+retired live handle≤16；无 permit 时拒绝新建/替换且保留现有 peer
  - 实现两阶段 offer：取得 permit/构建 session+bridge → map 锁内 generation 占位/旧 session 提交 Reaper → 锁外 create/register/negotiate/send → 按 generation 发布或回滚；同步早到 callback 直接更新 session
  - 所有 create/add/set/answer/addIce/send/close/free 均不得持 peer map mutex；测试锁探针必须覆盖成功和每个失败回滚分支
  - 用容量 16 的唯一 Reaper 替换 cleanup thread；创建失败、替换、remove、timeout、状态失败、shutdown 全部 exactly-once 提交，不得由 callback/调用线程现场 free
  - retirement 固定为 slot open=false → session I/O gate → close/free → slot in-flight==0 → 销毁 session、slot 标记可复用（slot 内存不释放）→ 归还 permit
  - shutdown 顺序为停止 peer admission → 摘除全部 generation → Reaper drain/join → 销毁 media（bridge slot 池在 `deinitKvsWebRtc()` 返回后才随 Impl 销毁）→ signaling shutdown → global runtime；deinit join 语义 BLOCKED 时按 Task 1 记录的进程退出兜底方案执行
  - 在 `webrtc_media_test.cpp` 覆盖同步 callback、注册前后乱序 callback、任意迟到 callback（slot 复用前后）、可解除阻塞 close/free、重复 offer/remove/shutdown、permit 耗尽、Reaper 背压和 exactly-once
  - 验证：CTest + ASan；slot 地址全程稳定、迟到 callback 零副作用、slot 复用仅在 in-flight==0 后、无 double-free、无锁内 SDK；live handles/Reaper≤16
  - SHALL NOT 仅设 `ctx->impl=nullptr` 后 delete；SHALL NOT 用 sleep 推断 quiescence；SHALL NOT 对含 atomic 对象做拷贝 emplace
  - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 5.1, 6.1, 7.2, 8.2, 8.3, 8.4_

- [x] 5. 完成媒体 I/O、local ICE、僵尸/缓存上限与健康观测
  - `broadcast_frame()` 在 map 锁内仅复制 CONNECTED session shared_ptr，解锁后按 session I/O gate 调 write；Reaper 对同 session 使用同一 gate，不同 session 不共享锁
  - 根据 Task 1 契约编码 frame data 生命周期和 write/free 顺序；从当前头文件使用 SRTP-not-ready 符号，不保留裸 `0x5c000003`；无法确认则停止本任务
  - 区分可配置 keyframe-only 阈值与固定 100 次断开阈值；CONNECTING 30s、FAILED/CLOSED、write failure>100 进入 DISCONNECTING，10s 后 Reaper 回收
  - local ICE callback 只复制≤4KiB并调用内部 fire-and-forget `try_post_ice()`，不得等待公开 signaling reply；旧 generation/过期/满队列按分类计数
  - Pending ICE 实现 per-peer 50、peer 20、global 200、单条 4KiB、TTL 30s；单 peer 满淘汰最旧 candidate，peer/global 满淘汰最旧 pending peer，offer 后锁外 FIFO flush
  - CONNECTED callback 只设置 `keyframe_pending`，由非 SDK callback 媒体路径执行既有 force-keyframe；不在本任务修复 pipeline 借用所有权
  - 健康 snapshot/log 增加三个队列深度、active/retired/live、state age、回收原因、stale/overflow/expired；CONNECTED+0 viewer 保持健康
  - 在 `webrtc_media_test.cpp` 用事件门禁证明慢 write 不阻塞 map/其他 session；覆盖两个 failure 阈值、ICE flood/TTL/LRU、local ICE、keyframe flag 和 ≤16MiB 最坏预算
  - 将 `ReadReadConcurrencyWithSharedLock` 吞吐比值改为锁探针/事件门禁确定性测试
  - **Stage 2 部署门**：部署前先在 `docs/development-trace.md` 归档 Stage 1 观察窗口结论（复现/未复现 + 证据），再用 `./scripts/pi-build.sh` + `./scripts/pi-deploy.sh` 部署 peer/media 层修复；若 Stage 1 窗口内失活复现，对照假设 B/C 更新排序后再部署
  - 验证：CTest；active≤10、live/Reaper≤16、pending peer≤20、每 peer≤50、global≤200、估算/RSS增量≤16MiB
  - SHALL NOT 用 detached frame worker、固定 sleep、吞吐比值或裸状态码证明正确性
  - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 6.1, 6.2, 6.3, 6.4, 7.2, 7.3, 8.1, 8.4_

- [x] 6. 收敛双平台回归并执行分层 Pi 长稳门禁
  - 让 macOS fake 与 Linux production adapter 共用 SignalingOwner、MessageDispatcher、PeerSession、Reaper 和 runtime token；删除旧 `reconnect_loop`/`simulate_disconnect` 平行状态机
  - 在 `webrtc_test.cpp` 用 manual clock/事件门禁替换所有 200/500ms 固定 sleep；保留并更新 connect/disconnect/reconnect/send/ICE query preservation，按新语义区分 API 链成功与 CONNECTED callback
  - 在两个测试文件完成 Properties 1～8：10,000 command/reconnect、message overflow、bridge retirement、permit/ICE 资源模型和虚拟时间等价 72h；稳定网络窗口 recreate rate=0
  - 运行 Debug+ASan 完整构建与 `ctest --test-dir device/build --output-on-failure`；保存完整 stderr/stack trace，ASan 不用于宣称无 data race
  - Pi 用 `./scripts/pi-build.sh` 编译真实 adapter；用 `./scripts/pi-deploy.sh` 部署后执行短断网、长断网、同 peer 重复 offer和至少 100 轮 Viewer connect/disconnect，确认无需重启即可恢复
  - 采样线程数、RSS、state/generation、三个队列、stale/overflow rate、active/retired/live handles；累计计数看增长率，不要求原值不增长
  - 只有设备生命周期 P0 已修复后才执行并判定整机 72h soak；此前可完成虚拟 72h 与断网/churn，但整机项必须标记 BLOCKED。若触发 pipeline/ShutdownHandler P0，停止并归因到生命周期 Spec，不在本任务猜修
  - Pi 验证不得注入 Spec 32 recovery/FATAL；完成后将 SDK 证据、Stage 1/Stage 2 观察窗口结论（对照 Task 0 取证基线的归因结果）、断网/churn、资源曲线和满足前置条件后的 soak 结果写入 `docs/development-trace.md`
  - 验证：宿主机 CTest 全绿；Pi SDK 表无 BLOCKED；断网/churn 恢复；前置满足后整机 72h 无永久断连、泄漏、死锁或崩溃
  - 最后仅执行 `git status --short`，确认不包含证书、密钥、`.env`、样本图片或无关 model 改动；不自动 commit
  - SHALL NOT 把 pipeline P0 造成的失败计入 Spec 33；SHALL NOT 直接运行测试 binary；SHALL NOT 自动 commit
  - _Requirements: 1.1, 1.4, 1.5, 2.1, 2.2, 2.3, 2.4, 2.5, 3.4, 4.2, 4.4, 5.2, 5.3, 5.4, 5.6, 6.1, 6.2, 6.3, 6.4, 7.1, 7.2, 7.3, 7.4, 7.5, 8.1, 8.2, 8.3, 8.4_

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 0, "tasks": ["0", "1"], "note": "Task 0 取证与 Task 1 SDK 语义决策门并行；Task 0 只阻塞归因结论不阻塞开发；Task 1 运行时路径 BLOCKED 即停止依赖 Task" },
    { "wave": 1, "tasks": ["2"], "note": "seam、runtime、signaling owner 与恢复状态机" },
    { "wave": 2, "tasks": ["3"], "note": "安全 bridge slot 与 dispatcher；完成后触发 Stage 1 部署门（部署 + 记录观察窗口，不阻塞后续开发）" },
    { "wave": 3, "tasks": ["4"], "note": "peer 生命周期依赖语义门禁与 dispatcher 串行 offer" },
    { "wave": 4, "tasks": ["5"], "note": "媒体/ICE/健康依赖 PeerSession 与 Reaper；完成后触发 Stage 2 部署门（先归档 Stage 1 窗口结论再部署）" },
    { "wave": 5, "tasks": ["6"], "note": "双平台回归与分层长稳依赖全部实现与两个 Stage 的观察记录" }
  ],
  "dependencies": {
    "0": [],
    "1": [],
    "2": ["1"],
    "3": ["1", "2"],
    "4": ["1", "2", "3"],
    "5": ["1", "2", "4"],
    "6": ["0", "1", "2", "3", "4", "5"]
  }
}
```

## Notes

- Task 0 是归因基线：不阻塞 Task 1～2 开发，但 Stage 1/Stage 2 部署门的观察结论必须对照它归档。
- Task 1 是可失败的决策门，不是“查不到也继续”的准备任务；影响运行时路径的 BLOCKED 项先回到 Spec，仅影响析构收尾的 deinit BLOCKED 记录进程退出兜底后可继续。
- Task 2～5 不得在 Task 1 对应契约 PENDING/BLOCKED 时开始。
- 部署节奏分 Stage（开发不等观察窗口，部署等）：Stage 1 = Task 2～3，Stage 2 = Task 4～5；跳过部署门直接上全量会丢失归因信号。
- Task 6 的虚拟 72h、断网和 churn 可先完成；整机 72h 额外依赖设备生命周期 P0 修复。
- 每个编码 Task 修改上述 6 文件的子集；Task 0/1/6 的证据另写 `design.md` 与 `docs/development-trace.md`。
- 同一问题连续修复 2 次仍失败时停止并回到 Spec，不做第 3 次猜测。
- 不自动 commit，不把工作树中的 model 文件或样本图片混入本 Spec。
