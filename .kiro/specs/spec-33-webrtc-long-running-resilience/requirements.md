# Requirements Document

> Spec 33: WebRTC 长期运行韧性修复

## Introduction

本 Spec 修复设备运行 1～2 天后 WebRTC 进程仍存活、但新 Viewer 永久无法连接的问题。现有实现同时存在 signaling handle 多线程访问、SDK 自动重连与应用重连叠加、连接确认无 deadline、SDK callback 同步执行重活、raw `customData` 释放竞态、锁内调用外部 SDK、僵尸 peer 与 pending ICE 无界滞留等缺陷。

本 Spec 将 signaling、消息分发和 peer 释放收敛为三个串行执行域；callback context 使用固定容量池（type-stable memory，slot 地址在 Impl 生命周期内永不释放）加 generation 隔离旧对象回调，因此运行时内存安全不依赖 SDK 的 free-后-callback-quiescence 证明。任何依赖 KVS WebRTC C SDK 阻塞语义或数据所有权的设计，必须先通过 Pi 当前 SDK 语义门禁；证据不足时停止受影响任务，不得以 detached worker、固定 sleep 或无界内存规避。

修复动工前必须先完成 Task 0 取证：用现有日志与失活现场证据（`scripts/pi-diagnose.sh`）对三个失活假设（僵尸 signaling / 消息泵死锁 / 退避风暴）分类，取证结论决定分阶段部署的归因基线，但不阻塞 Task 1～2 的开发。

### 前置条件

- spec-13.6（WebRTC peer 生命周期死锁修复）已通过验证 ✅
- spec-13.7（Signaling WebSocket 自动重连）已通过验证 ✅，本 Spec 将替换其竞态实现
- spec-32 Task 1～6 已完成宿主机验证；Task 7 尚未执行
- 设备生命周期安全 P0（pipeline 双 teardown、ShutdownHandler detached worker、borrower 解绑顺序）尚未修复：Task 1 SDK 门禁和 WebRTC 隔离测试可先执行，但整机 72 小时验收必须等待该 P0 修复

## Glossary

- **SignalingOwner**：唯一拥有并操作 signaling client、credential provider 与其 callback bridge 的 worker。
- **ControlMailbox**：不受普通 send/query 洪水影响的控制通道；SHUTDOWN 为不可拒绝的带外标志，state 事件按 generation 有界合并，deadline 由 owner 时钟直接计算。
- **Command**：带 id、提交 generation、完成 deadline 和 exactly-once 完成状态的普通请求。分两类：**QUERY 类**（query_ice 等 request/reply，caller 等待与完成 deadline 同为 2s）与 **SEND 类**（send_answer/send_ice，迟到无害：caller 最多等待 2s，命令在 10s 完成 deadline 内仍可执行并 exactly-once 完成）。
- **Generation**：每次创建 signaling client 或 PeerSession 时递增的编号；旧 generation 事件不得污染当前对象。
- **MessageDispatcher**：从 SDK message callback 接收有界副本，并在非 SDK callback 线程执行 offer/ICE 业务处理的单 worker。
- **CallbackBridge**：传给 SDK raw `customData` 的稳定地址；从固定容量池分配（signaling 1 个 + peer 16 个 slot，peer slot 与 HandlePermit 一一对应），slot 地址在 Impl 生命周期内永不释放，靠 open 标志、generation 与 in-flight 计数隔离迟到 callback。
- **PeerSession**：以 `std::shared_ptr` 管理的 Viewer 会话，封装 handle、generation、状态、callback gate、I/O gate 和资源 permit。
- **Reaper**：唯一执行 `closePeerConnection` / `freePeerConnection` 的 worker。
- **HandlePermit**：限制 active、creating、retired PeerConnection 总数的令牌，从创建前持有到 free 完成。
- **Slot Quiescence**：bridge slot 的 admission 已关闭（open=false）且 in-flight callback 计数归零；slot 复用给新 generation 的前置条件，由自有原子计数证明，不依赖 SDK 契约。SDK 级 quiescence（`deinitKvsWebRtc()` 停止全部内部线程）仅是析构 bridge 池的前置条件。

## 准入三问

- **单一验证条件**：宿主机确定性 CTest 全绿，Pi SDK 门禁全部有证据，Pi 断网与 100 轮 Viewer churn 后无需重启即可恢复；生命周期 P0 修复后完成整机 72 小时 soak。
- **合理粒度**：7 个 Task（Task 0 为零生产代码的取证任务）；4 个生产文件和 2 个既有测试文件，共 6 个文件。超过 2～5 文件建议值的原因是 signaling/media 已分属两个既有测试目标，若强行合并会遗留固定 sleep 测试或修改 CMake，风险更高。实现按两个部署阶段交付（Stage 1：Task 2～3 signaling 层；Stage 2：Task 4～5 peer/media 层），每阶段独立部署到 Pi 并记录观察窗口结果，用于失活根因的归因二分。
- **明确边界**：不修改 pipeline recovery、ShutdownHandler、systemd、KVS Producer、AI/S3 或全应用健康模型，详见 Out of Scope。

## Constraints

- 目标平台：Raspberry Pi 5（Debian Bookworm aarch64，GCC）真实 KVS WebRTC C SDK；macOS 使用同一 runtime 加可注入 fake ops。
- 语言与依赖：C++17；不新增第三方依赖；RAII；日志使用 `webrtc` logger 且仅输出 ASCII。
- worker 预算：常驻新增 worker 不超过 3 个（SignalingOwner、MessageDispatcher、Reaper）；不得创建 per-command、per-peer 或 detached worker。
- 普通 command queue 容量 256；SDK message queue 容量 512；peer_id ≤256 bytes；command/message payload ≤16KiB；单条 ICE ≤4KiB。
- peer 资源：active PeerSession ≤10；HandlePermit 总数 16，因此 active + creating + retired SDK handle ≤16；Reaper 队列 ≤16。
- Pending ICE：每 peer ≤50、pending peer ≤20、全局 ≤200、TTL 30s。
- WebRTC 新增队列、payload 副本和 session 元数据总预算 ≤16MiB；所有上限按最坏 payload 计算验证。
- 普通命令按类使用两级 deadline：QUERY 类 caller 等待与完成 deadline 同为 2s；SEND 类 caller 最多等待 2s，命令完成 deadline 为提交后 10s（可配置）——SDP answer/ICE 迟到数秒对 viewer 仍有效，caller 超时不取消 SEND，超过完成 deadline 的 SEND 必须取消。owner 只有在 `now() + Task 1 已证明的该 SDK 调用最坏上界 <= 完成 deadline` 时才可启动调用；若已证明上界超过 10s，则 SEND 路径在 Task 1 标记 BLOCKED 并修订本 Spec。`connect()`、`reconnect()`、`disconnect()` 与析构使用各阶段已确认上界，不承诺 2s，且必须安全 join。
- 恢复参数：CONNECTED deadline 30s；重连退避 1/2/4/8/16/30s 后恒 30s；稳定连接 30s 后重置；CONNECTING peer 30s；DISCONNECTING grace 10s；SDK 内置重连裕量窗口 20s（DISCONNECTED 后等待 SDK 单次重连）；signaling 活性超时 3h（超过该时长无任何 state/message callback 即判定半开连接并 recreate，可配置）。
- 兼容性：现有公共方法签名保持不变；最大 10 Viewer、H.264 Annex B、ICE 先到缓存、同 peer 替换和连接后请求关键帧行为保持。
- 测试统一执行 `ctest --test-dir device/build --output-on-failure`，不得直接运行测试可执行文件。
## Requirements

### 需求 1：Signaling 单 owner、命令取消与安全 shutdown

1.1 WHEN `connect()`、`reconnect()`、`disconnect()`、send、ICE query 和 SDK state callback 并发发生 THEN THE system SHALL 让 SignalingOwner 串行执行所有 signaling handle/credential 操作，其他线程不得读取或操作 handle。

1.2 WHEN 普通 command 被提交 THEN THE system SHALL 携带唯一 id、提交 generation、类型对应的完成 deadline（QUERY 2s / SEND 10s，可配置）与原子完成状态；owner 在调用 SDK 前重检 admission、generation、完成 deadline 和取消状态，并保证 reply exactly once。

1.3 WHEN 普通 send/query 队列已满、完成 deadline 前剩余时间不足以覆盖已确认的 SDK 最坏上界，或 shutdown 取消尚未执行的 command THEN THE system SHALL 返回失败且不得调用 SDK；QUERY 类 caller 超时即取消命令，SEND 类 caller 超时后命令仍可在完成 deadline 内执行并 exactly-once 完成（迟到无害），超过完成 deadline 的命令不得启动 SDK 调用。

1.4 WHEN 显式 `disconnect()` 或析构开始 THEN THE system SHALL 依次关闭普通命令与 message admission、停止并 join MessageDispatcher、设置不可拒绝 SHUTDOWN、由 SignalingOwner 取消普通命令并 release、退出 owner 后再 join；不得在 join owner 后要求 owner release，不得遗留越过 `Impl` 生命周期的 worker。

1.5 WHEN SDK state callback 被调用 THEN THE system SHALL 只向 ControlMailbox 发布 generation、状态和 `observed_at`，不得执行 connect/send/free、媒体/GStreamer 操作或等待业务锁；所有 C ABI callback SHALL 捕获全部 C++ 异常并转为计数/错误码。

**验收与验证**：fake SDK 并发提交 10,000 次 command/state/shutdown，断言所有 handle 操作来自同一 owner、队列深度 ≤256、超过完成 deadline 的命令零副作用、reply exactly once（含 SEND 类 caller 超时后仍在完成 deadline 内 exactly-once 完成的场景）、release 后无调用、worker 全部 join；CTest + ASan 通过。

### 需求 2：有 deadline 的完整 client recreate

2.1 WHEN signaling 进入 DISCONNECTED、READY 长期无进展或底层连接失活 THEN THE system SHALL 使用 Task 1 已证明的唯一策略进入 RECOVERING；应用不得对同一失活 handle 并发或重复调用 `signalingClientConnectSync()`。

2.2 WHEN create/fetch/connect 任一阶段失败（含进程启动后的首次 `connect()`，如设备断电恢复时网络尚未就绪）或 connect API 成功后 30s 内未观察到同 generation 的 CONNECTED THEN THE system SHALL release 当前 generation 并按饱和退避创建下一 generation，直到成功或 shutdown，不得放弃重试；`connect()==true` 仅表示本次 create/fetch/connect API 链被接受并成功返回，不表示 `is_connected()==true`。

2.3 WHEN CONNECTED callback 与 deadline 同时竞争 THEN THE system SHALL 以 callback 的 `observed_at` 判定：同 generation 且 `observed_at <= deadline` 的 CONNECTED 获胜，否则 deadline 获胜；普通 command 洪水不得延迟该判定。

2.4 WHEN 连续失败任意次数 THEN THE system SHALL 产生 1/2/4/8/16/30s 后恒 30s 的无溢出退避；仅连接稳定 30s 后重置 attempt。

2.5 WHEN 旧 generation 的 state/message callback 到达 THEN THE system SHALL 丢弃并计数，不得改变当前状态、完成当前 command 或调用当前业务 handler；signaling recreate 不得重建仍 CONNECTED 的 PeerSession。

**验收与验证**：fake 分阶段注入 create/fetch/connect 失败（含首次 connect 全阶段失败后延迟恢复的启动场景）、无 CONNECTED、deadline 边界、10,000 次失败和旧 callback 乱序；断言最终进展、退避有界、控制事件不被 256+ 普通 command 饿死。

### 需求 3：SDK message callback 轻量化与有界分发

3.1 WHEN SDK 收到 OFFER 或 ICE_CANDIDATE THEN THE system SHALL 在 callback 内完成空指针/类型/peer_id/payload 长度校验、有界副本和 try-enqueue 后返回；不得调用 offer/ICE handler 或 Peer SDK。

3.2 WHEN message 队列达到 512 THEN THE system SHALL 让新 OFFER 淘汰最旧 ICE；全为 OFFER 时拒绝最新 OFFER，新 ICE 无空间时丢弃，并分别计数与节流日志，不得扩容。

3.3 WHEN 同 peer 的 OFFER 先后到达 THEN THE system SHALL 按入队顺序串行处理；旧 signaling generation message 与旧 peer generation 结果不得覆盖当前 PeerSession。

3.4 WHEN shutdown 开始 THEN THE system SHALL 关闭 message admission、丢弃尚未开始的业务任务并 join MessageDispatcher，随后才允许 signaling owner release；析构后不得调用用户 handler。

**验收与验证**：用事件门禁阻塞 handler，断言 SDK callback 不执行 handler、不等待业务锁且可完成入队；覆盖 513+ 消息、异常分配、重复 offer 与 shutdown。wall-clock 仅记录宽松统计，不作为硬 10ms 正确性断言。

### 需求 4：CallbackBridge、PeerSession 与唯一 Reaper

4.1 WHEN 创建 signaling client 或 PeerConnection 并注册 raw `customData` THEN THE system SHALL 从固定容量 CallbackBridge 池分配 slot（signaling 1 个、peer 16 个且与 HandlePermit 一一对应），注册 callback 前 slot 已就绪；slot 地址在 Impl 生命周期内永不释放，因此"允许任意迟到 callback、内存有界、无 UAF"三者由池的 type-stable 性质同时满足，不依赖 SDK 的 free-后-quiescence 契约。

4.2 WHEN 迟到 callback 到达已退役的 bridge slot THEN THE system SHALL 先原子递增 slot in-flight 计数，再校验 open 标志与 generation，不匹配时递减计数并直接返回、只累计 stale 计数；slot 只有在 open=false、close/free 已完成且 in-flight==0 时才可复用给新 generation。

4.3 WHEN peer 被创建、替换、回滚、移除、超时或 shutdown THEN THE system SHALL 使用 HandlePermit 保证 live SDK handles ≤16，并由唯一 Reaper close/free；Reaper 队列无 permit 时 SHALL 拒绝新建/替换，不得改由 callback 或调用线程 free。

4.4 WHEN callback 与 retirement 并发 THEN THE system SHALL 先关闭 bridge slot admission（open=false），再 close/free，等待 slot in-flight 归零后销毁 session 并将 slot 标记可复用（slot 内存不释放）；每个 generation close/free 各至多一次。WHEN shutdown 销毁 bridge 池本身 THEN THE system SHALL 在全部 handle 释放且 `deinitKvsWebRtc()` 返回之后进行。

4.5 WHEN create/addTransceiver/setDescription/createAnswer/addIceCandidate/send/close/free 等外部 API 被调用 THEN THE system SHALL 不持有 peer map mutex；同步 callback SHALL 能更新尚未发布到 map 的 PeerSession。

**验收与验证**：fake 同步 callback、延迟 callback、事件可解除的阻塞 close/free、重复 offer 与 shutdown；断言 slot 地址全程稳定、迟到 callback 在 in-flight>0 或 generation 失配时零副作用、slot 复用仅发生在 in-flight==0 之后、无锁内 SDK、无 double-free、active+creating+retired≤16。不可解除的永久阻塞只用于验证 Task 1 应停止，不作为“仍能 bounded shutdown”的矛盾断言。

### 需求 5：僵尸回收、媒体隔离与本地 ICE 非阻塞

5.1 WHEN PeerSession CONNECTING 超过 30s、进入 FAILED/CLOSED 或 writeFrame 连续失败超过 100 次 THEN THE system SHALL 进入 DISCONNECTING，并在 10s grace 后提交 Reaper；达到可配置 keyframe-only 阈值只切换发送模式，不等同于断开阈值。

5.2 WHEN `broadcast_frame()` 遍历 peer THEN THE system SHALL 只在 map 锁内复制 `shared_ptr` 快照，解锁后调用 writeFrame；同 session 的 write/free 通过 I/O gate 串行，不同 session 不共享 I/O 锁。

5.3 WHEN Task 1 无法证明 writeFrame 阻塞上界、frame data 生命周期及与 close/free 的规则 THEN THE system SHALL 停止媒体 I/O 改造；不得用 detached frame worker、无界复制或固定 sleep 猜测。

5.4 WHEN Peer SDK 产生 local ICE callback THEN THE system SHALL 只做有界复制并 fire-and-forget 提交 signaling command，不等待 2s request/reply；旧 generation、过期或满队列 candidate 可计数丢弃。

5.5 WHEN 未知 peer 持续发送 ICE THEN THE system SHALL 执行每 peer 50、pending peer 20、全局 200、单条 4KiB 和 TTL 30s 上限；达到 peer/global 上限时淘汰最旧 pending peer，达到单 peer 上限时淘汰该 peer 最旧 candidate。

5.6 WHEN peer CONNECTED callback 请求关键帧 THEN THE system SHALL 只设置 session 的 pending keyframe 标志，由非 SDK callback 路径执行既有请求；pipeline 借用指针的所有权修复仍属于设备生命周期安全 Spec。

**验收与验证**：fake 注入 write 延迟、两个失败阈值、local ICE flood、CONNECTING 超时和 keyframe 标志；断言 map 操作不被 session I/O 阻塞、资源最终回收且上限成立。

### 需求 6：健康、日志与资源预算

6.1 WHEN 输出 signaling 健康快照 THEN THE system SHALL 包含 state、generation、attempt、connected age、普通/消息/Reaper 队列深度、active/retired/live handles、stale/overflow/expired 计数，且不得输出凭证、完整 SDP、ICE credential 或 token。

6.2 WHEN signaling CONNECTED 且无 Viewer THEN THE system SHALL 视为可接受连接的健康状态；`peer_count()==0` 不得触发恢复。

6.3 WHEN 同类异常重复发生 THEN THE system SHALL 首次立即记录，此后每类每 60s 最多一条 warn/error并附 suppressed 数；连续 10min 无该异常后重置节流窗口。

6.4 WHEN 验证内存预算 THEN THE system SHALL 按 256×16KiB command、512×16KiB message、200×4KiB pending ICE 加固定元数据计算并实测 RSS 增量，新增资源总量 ≤16MiB；累计计数允许增长，验收使用单位时间增长率而非要求原值归零。

**验收与验证**：确定性捕获日志、队列高水位和内存公式；稳定窗口内队列回落、live handles 有界、无敏感字段。

### 需求 7：回归与分层长稳验收

7.1 WHEN macOS 无真实 SDK 构建 THEN THE system SHALL 与 Linux 共用 SignalingOwner、MessageDispatcher、PeerSession、Reaper 和状态机，仅替换 ops；测试使用 manual clock/事件门禁，不用固定 sleep 或吞吐比值断言。

7.2 WHEN 正常首次连接、显式断开、reconnect、send、ICE query、最多 10 Viewer、同 peer 替换、offer 前 ICE、Annex B 广播发生 THEN THE system SHALL 保持既有公共签名和已明确的新返回语义。

7.3 WHEN 完整 CTest 运行 THEN THE system SHALL 修改 `webrtc_test.cpp` 中 200/500ms 固定 sleep 测试，并替换 `webrtc_media_test.cpp` 的 `ReadReadConcurrencyWithSharedLock` 吞吐断言；ASan 用于内存错误，确定性互斥/事件测试用于并发正确性，不宣称 ASan 能证明无 data race。

7.4 WHEN 宿主机执行虚拟时间长稳模型 THEN THE system SHALL 覆盖等价 72h 的断连、deadline、Viewer churn 和乱序 callback，且无需 wall-clock 等待；稳定网络窗口内 recreate rate 为 0，故障窗口后恢复至 CONNECTED。

7.5 WHEN 设备生命周期 P0 已修复且 Pi 执行短/长断网、至少 100 轮 Viewer churn 与整机 72h soak THEN THE system SHALL 无需重启即可恢复接受 offer 和发送视频；线程数、RSS、队列深度及 live handles 无持续增长。若 P0 未修复或触发，则整机结果标记为“阻塞/无结论”，不得把 pipeline 故障归因给 Spec 33。

**验收与验证**：完整 CTest 全绿；Pi 使用 `scripts/pi-build.sh` 构建、`scripts/pi-deploy.sh` 部署，分层归档 SDK 门禁、断网/churn 和满足前置条件后的 72h 数据。

### 需求 8：Pi 当前 SDK 语义门禁与共享 runtime

8.1 WHEN 实现依赖 SDK 同步调用 THEN THE system SHALL 将 create、fetch、connect、send、query、free signaling、Peer create/description、write、close、free 分阶段确认其阻塞上界、取消/并发规则和回调顺序，不得使用合并的 `create_fetch_connect()` 掩盖阻塞阶段。

8.2 WHEN 实现 CallbackBridge 池的析构路径 THEN THE system SHALL 确认 `deinitKvsWebRtc()` 是否停止并 join 全部 SDK 内部线程（timer queue、ICE agent 等），记录 Evidence、Status（CONFIRMED/BLOCKED）、SDK version/commit 和 Date；该项 BLOCKED 只阻塞 Impl 析构收尾的设计（可用进程退出兜底），不阻塞运行时池化实现。free 后 per-handle callback quiescence 不再是任何运行时路径的前置条件，仅作补充证据记录。

8.3 WHEN 初始化/释放 `initKvsWebRtc/deinitKvsWebRtc` THEN THE system SHALL 使用共享 runtime token/refcount，确保 PeerSession 与 signaling client 全部释放后才 deinit；shutdown 顺序固定为 media/Reaper → signaling/owner → global runtime。

8.4 WHEN SDK 源码没有可证明的有限上界或所有权契约（影响运行时路径）THEN THE system SHALL 停止并更新 Requirements/Design/Tasks，不能以一次最小实测成功替代契约证明；deinit 线程 join 语义除外，按 8.2 用进程退出兜底。

**验收与验证**：Pi 语义表每项具有源码位置或明确的最小实测补充（可用 `scripts/webrtc-sdk-probe.sh` 采集）；影响运行时路径的项无 BLOCKED 后才可开始对应编码 Task，仅影响析构收尾的 deinit 项 BLOCKED 时可用进程退出兜底并记录。

## SHALL NOT（Requirements 层）

- SHALL NOT 将“当前无 Viewer”当作故障或升级为 pipeline/systemd 恢复。
- SHALL NOT 因 signaling recreate 重建仍 CONNECTED 的 PeerConnection。
- SHALL NOT 静默丢弃 OFFER；拒绝必须计数并按节流策略记录。
- SHALL NOT 将 caller timeout 当作 SDK call 已取消或已停止副作用。
- SHALL NOT 在设备生命周期 P0 未修复时宣称整机 72h 验收通过。

## 验证汇总

| 需求 | 宿主机确定性验证 | Pi 5 验证 |
|------|------------------|-----------|
| 1–2 | 10,000 command/恢复序列、deadline、exactly-once | SDK 分阶段调用与断网恢复 |
| 3 | message overflow、异常边界、handler 线程隔离 | churn 下持续接收 offer |
| 4 | slot 复用门禁、permit、Reaper exactly-once | deinit/阻塞契约 |
| 5 | I/O gate、local ICE、僵尸/ICE 上限 | 视频发送与 handle 回收 |
| 6 | 日志节流、内存公式与高水位 | RSS/线程/队列采样 |
| 7 | CTest + 虚拟 72h | 100 轮 churn；前置满足后整机 72h |
| 8 | fake adapter 契约 | 当前 SDK Evidence/Status/Date |

## 明确不包含（Out of Scope）

- Spec 32 已发现的 pipeline 双 teardown worker、borrower 解绑、health timer、ShutdownHandler detached worker 和 AppContext 生命周期问题；由独立设备生命周期安全 Spec 修复。
- appsink 保存裸 `WebRtcMediaManager*`、pipeline rebuild 的 `set_pipeline()` 借用所有权；本 Spec 只保持 keyframe 请求行为并避免在 SDK callback 直接操作 GStreamer。
- WebRTC 失败接入 `AppContext::is_healthy()`、sd_notifier 或 systemd。
- KVS Producer、PipelineHealth、StreamModeController、BitrateAdapter、NetworkMonitor、AI、S3、配置和 Viewer 前端修改。
- codec、分辨率、码率、TURN 策略、H.264 stream-format、多摄像头、音频扩展及超过 10 Viewer。
