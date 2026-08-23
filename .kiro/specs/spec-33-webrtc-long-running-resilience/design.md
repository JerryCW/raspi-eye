# Design Document

> Spec 33: WebRTC 长期运行韧性修复

## Overview

当前长期失活由两个互相放大的生命周期缺陷造成：

1. callback、公开方法和 reconnect thread 并发访问 signaling handle；`ConnectSync` 返回成功但未收到 CONNECTED 时，系统可永久停在假恢复状态。
2. message callback 同步进入 offer 处理；peer map 锁内调用外部 SDK；raw callback context 在未证明 quiescence 时被删除。

本设计采用三个串行执行域：SignalingOwner、MessageDispatcher、Reaper；callback context 采用固定容量池（type-stable memory），运行时内存安全不依赖 SDK 的 free-后-quiescence 契约（见决策 B）。任何同步 SDK 调用只有在 Task 1 从 Pi 当前 SDK 源码证明有限返回后，才允许在 owner/Reaper 执行。若任一影响运行时路径的关键调用没有有限上界或所有权契约，本 Spec 停止在门禁，不用额外 executor、detach 或 sleep 伪造 bounded shutdown。

## Architecture

```text
SDK state callback ─► SignalingCallbackBridge ─► ControlMailbox
公开 connect/send/query ─► bounded CommandQueue ─► SignalingOwner ─► staged SDK ops
SDK message callback ─► bounded MessageQueue ─► MessageDispatcher ─► offer/ICE
Peer callback ─► PeerCallbackBridge ─► PeerSession atomic event/flag
GStreamer frame ─► peer shared_ptr snapshot ─► session I/O gate ─► writeFrame
Peer retirement ─► HandlePermit + bounded ReaperQueue ─► Reaper close/free/quiesce
```

优先级固定为：`SHUTDOWN > 当前 generation state/deadline > 普通 command`。SHUTDOWN 是带外原子标志，不受队列容量影响；deadline 由 owner 的 monotonic clock 直接计算，不作为普通 command 入队。

## 设计决策

### 决策 A：同步 SDK 调用门禁

Task 1 分阶段确认 create、fetch、connect、send、query、free signaling、Peer create/description、write、close、free。只有源码存在有限 timeout/可等待 completion，且 callback/free 规则明确，状态才标为 `CONFIRMED`。一次实测返回只作补充证据，不能证明上界。

- 全部关键项 `CONFIRMED`：owner/Reaper 可同步调用，最坏停顿写入契约与日志。
- 任一关键项 `BLOCKED`：停止依赖该项的 Task，先修订 Spec；不得增加 detached SDK-call worker。

这消解了“owner 同步 ConnectSync”与“owner 必须处理 CONNECTED/deadline/shutdown”的冲突：同步调用可以暂时占用 owner，但不得无限占用；callback 先写 ControlMailbox，owner 返回后按 `observed_at` 判定。

### 决策 B：raw customData 用固定池（type-stable memory）消除 UAF

`shared_ptr` 和 generation 只能保护 callback 成功解引用 bridge 之后的对象，不能保护已经释放的 bridge 地址。与其依赖 SDK 证明 free 后 callback quiescence（最难证明、最可能 BLOCKED 的契约），本设计直接让 bridge 地址永不失效：

- signaling bridge：每个 `WebRtcSignaling::Impl` 一个固定 slot，Impl 生命周期内不释放；client recreate 只递增 generation。
- peer bridge：固定 16 个 slot 的数组（与 HandlePermit 一一对应），Impl 生命周期内不释放；slot 复用条件为 `open=false && close/free 完成 && in_flight==0`，复用时 generation 递增。

callback 入口协议：`fetch_add(in_flight)` → 校验 open/generation → 失配则 `fetch_sub` 返回并计 stale。地址稳定 + 16 slot 上限 + generation 隔离，同时满足"任意迟到 callback、内存有界、无 UAF"。运行时不需要任何 SDK quiescence 契约；只有 Impl 析构销毁池本身时需要 `deinitKvsWebRtc()` 已停止全部 SDK 内部线程（Task 1 确认；BLOCKED 时以进程退出兜底并记录，不阻塞运行时实现）。

### 决策 C：取证先行 + 分阶段部署归因 + 分层长稳验收

root cause 尚未实证确认，因此实现按两个可独立部署的阶段交付并观察：

- **Task 0（取证）**：用 `scripts/pi-diagnose.sh` 对上次/下次失活采集日志签名与线程栈，将三个假设（A 僵尸 signaling / B 消息泵死锁 / C 退避风暴）排序，结论写入 `docs/development-trace.md`。不阻塞开发，但为归因建立基线。
- **Stage 1（Task 2～3，signaling 层）**：部署后观察 ≥ 2× 历史失活间隔。若失活仍复现，root cause 大概率在 peer/UAF 层——这本身是证据。
- **Stage 2（Task 4～5，peer/media 层）**：Stage 1 观察窗口记录结果后部署。

开发不等待观察窗口（Stage 2 可在窗口期开发），但**部署节奏必须分阶段**，否则丢失归因信号。宿主机用 manual clock 瞬时覆盖虚拟 72h；Pi 先完成 SDK 门禁、断网和 churn。整机真实 72h 必须等待设备生命周期 P0 修复，未满足时结果只能标记为阻塞/无结论。

### 决策 D：命令两级 deadline（QUERY vs SEND）

`signalingClientSendMessageSync` 等待服务端 ACK，最坏上界很可能超过 2s。若沿用"caller deadline 2s 且 owner 只在能于 deadline 前完成时启动"，send 路径会被 admission 永久锁死。因此按副作用性质分两类：

- **QUERY 类**（query_ice）：request/reply，caller 等待与完成 deadline 同为 2s；caller 超时即取消，保持零副作用语义。
- **SEND 类**（send_answer/send_ice）：迟到无害——answer/ICE 晚到数秒 viewer 仍在等且有效。caller 最多等 2s 拿结果；命令完成 deadline 为提交后 10s（`RuntimeOptions::send_completion_deadline`），caller 超时不取消，owner 在完成 deadline 内仍可启动并 exactly-once 完成（promise 照常兑现，无人等待也不泄漏）。超过完成 deadline 的 SEND 取消且不调用 SDK。

admission 统一为 `now() + 已证明 max_duration <= command.completion_deadline`。若 Task 1 证明的 send 上界超过 10s，SEND 路径标 BLOCKED 并修订 Spec，不得以“caller 先返回、SDK 无限期继续”规避。
## Components and Interfaces

### Component 1：C++17 SDK seam、时钟与测试工厂

两个头文件补齐 `<atomic>`、`<chrono>`、`<condition_variable>`、`<functional>`、`<memory>`、`<mutex>`、`<string_view>`、`<vector>` 等实际依赖。internal 类型不引用 `WebRtcSignaling::IceServerInfo` 嵌套类型，避免声明顺序悬空。

```cpp
namespace webrtc::internal {

enum class WaitResult { NOTIFIED, DEADLINE };
enum class SdkCallStatus { OK, RETRYABLE, FATAL };

struct SdkCallResult {
    SdkCallStatus status;
    uint32_t code;
};

struct RuntimeOptions {
    size_t command_capacity = 256;
    size_t message_capacity = 512;
    size_t handle_permits = 16;
    std::chrono::seconds connected_deadline{30};
    std::chrono::seconds stable_connection{30};
    std::chrono::seconds caller_wait_timeout{2};        // caller 最长等待（QUERY 完成 deadline 同值）
    std::chrono::seconds send_completion_deadline{10};  // SEND 类完成 deadline（决策 D）
};

struct IceServerRecord {
    std::string uri;
    std::string username;
    std::string credential;
};

class RuntimeClock {
public:
    virtual ~RuntimeClock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
    virtual WaitResult wait_until(
        std::condition_variable& cv,
        std::unique_lock<std::mutex>& lock,
        std::chrono::steady_clock::time_point deadline,
        const std::function<bool()>& notified) = 0;
};

struct SignalingCallbacks {
    std::function<void(uint64_t generation, int state,
                       std::chrono::steady_clock::time_point observed_at)> on_state;
    std::function<void(uint64_t generation, int type,
                       std::string_view peer, std::string_view payload)> on_message;
    // SDK errorReportFn 桥接（决策 A3）：RECONNECT_FAILED 等错误只发布事件，不在 callback 内处理
    std::function<void(uint64_t generation, uint32_t status_code,
                       std::chrono::steady_clock::time_point observed_at)> on_error;
};

class SignalingSdkOps {
public:
    virtual ~SignalingSdkOps() = default;
    virtual SdkCallResult create(uint64_t, const SignalingCallbacks&) = 0;
    virtual SdkCallResult fetch() = 0;
    virtual SdkCallResult connect() = 0;
    virtual SdkCallResult send_answer(std::string_view, std::string_view) = 0;
    virtual SdkCallResult send_ice(std::string_view, std::string_view) = 0;
    virtual SdkCallResult query_ice(std::vector<IceServerRecord>&) = 0;
    virtual SdkCallResult release() = 0;
};

class PeerHandle { public: virtual ~PeerHandle() = default; };
struct PeerCallbacks {
    std::function<void(uint64_t, int)> on_state;
    std::function<void(uint64_t, std::string_view)> on_local_ice;
};
class PeerSdkOps {
public:
    virtual ~PeerSdkOps() = default;
    virtual SdkCallResult create(uint64_t, const PeerCallbacks&,
                                 std::unique_ptr<PeerHandle>&) = 0;
    virtual SdkCallResult negotiate(PeerHandle&, std::string_view,
                                    std::string& answer) = 0;
    virtual SdkCallResult add_ice(PeerHandle&, std::string_view) = 0;
    virtual SdkCallResult write_frame(PeerHandle&, const uint8_t*, size_t,
                                      uint64_t, bool) = 0;
    virtual SdkCallResult close(PeerHandle&) = 0;
    virtual SdkCallResult release(std::unique_ptr<PeerHandle>) = 0;
};
}
```

测试工厂是现有类的静态方法，生产 factory 签名不变：

```cpp
static std::unique_ptr<WebRtcSignaling> create_for_test(
    const WebRtcConfig&, const AwsConfig&,
    std::shared_ptr<webrtc::internal::SignalingSdkOps>,
    std::shared_ptr<webrtc::internal::RuntimeClock>,
    const webrtc::internal::RuntimeOptions&,
    std::string* error_msg = nullptr);

static std::unique_ptr<WebRtcMediaManager> create_for_test(
    WebRtcSignaling&, const std::string& region,
    std::shared_ptr<webrtc::internal::PeerSdkOps>,
    std::shared_ptr<webrtc::internal::RuntimeClock>,
    const webrtc::internal::RuntimeOptions&,
    std::string* error_msg = nullptr);
```

production adapter 隐藏真实 handle、凭证与 SDK 类型；fake 支持阶段失败、同步/延迟 callback、事件可解除阻塞、调用线程记录和 manual clock 唤醒。

**Validates: Requirements 7.1, 8.1, 8.4**

### Component 2：SignalingOwner、ControlMailbox 与 Command

```cpp
enum class CommandState { QUEUED, RUNNING, COMPLETED, CANCELLED };
enum class CommandType { CONNECT, RECONNECT, SEND_ANSWER, SEND_ICE, QUERY_ICE };
struct Command {
    uint64_t id;
    CommandType type;                    // SEND_* 为迟到无害类，QUERY_* 为 request/reply 类
    uint64_t submit_generation;
    std::chrono::steady_clock::time_point completion_deadline;  // QUERY: +2s / SEND: +10s
    std::string peer_id;
    std::string payload;
    std::atomic<CommandState> state{CommandState::QUEUED};
    std::shared_ptr<std::promise<CommandResult>> reply;
};
```

- 普通队列容量 256，满时立即失败。
- QUERY 类：caller 超时以 CAS 将 `QUEUED` 改为 `CANCELLED`（等待与完成 deadline 同为 2s，保持零副作用）。
- SEND 类：caller 最多等 2s 后自行返回超时，但**不取消命令**；owner 在 10s 完成 deadline 内仍可执行并 exactly-once 兑现 promise（迟到无害，见决策 D）。
- Task 1 为每类 send/query SDK call 给出已证明的 `max_duration`；owner 仅在 `now()+max_duration <= completion_deadline` 时将状态改为 `RUNNING`，因此契约成立时 RUNNING 必在完成 deadline 前 exactly-once 完成。若 send 的 `max_duration` 超过 10s，SEND 路径在 Task 1 标为 BLOCKED，不以无界的“caller 先返回、SDK 后续继续”规避。
- owner 调 SDK 前依次检查 shutdown、completion_deadline、generation、state；任何失败均不调用 SDK。
- ControlMailbox 不保存无界事件：每 generation 仅保留最新 state 加 observed_at；CONNECTED/DISCONNECTED 转换计数独立保存。SHUTDOWN 为带外标志。
- owner 每轮先处理 shutdown，再处理所有当前 generation state/deadline，最后最多处理一个普通 command，防止 send 洪水饿死控制面。

`connect()`/`reconnect()` 的 true 表示 staged create/fetch/connect API 链成功返回；`is_connected()` 只有当前 generation 的 CONNECTED callback 才为 true。send/query 使用已确认 `max_duration` 对各自完成 deadline 做 admission，caller 最多等待 2s（SEND 类超时后命令仍可在 10s 内完成，见决策 D）；connect/reconnect/disconnect/析构使用门禁记录的阶段上界执行安全 join，不伪造 2s 保证。

shutdown 固定顺序：

1. 关闭普通 command admission，取消 QUEUED command。
2. 关闭 message admission，清空未执行 message，join MessageDispatcher。
3. 设置带外 SHUTDOWN 并唤醒 owner。
4. owner 停止重连、release client/credential/bridge，发布 STOPPED 后退出。
5. join owner；最后释放共享 runtime token。

**Validates: Requirements 1.1, 1.2, 1.3, 1.4, 1.5**

### Component 3：恢复状态机、deadline 与 generation

```text
STOPPED → CREATING → FETCHING → CONNECTING → CONNECTED
              │          │           │           │
              └──────────┴───────────┴───────────┘
                              ▼
                          RECOVERING ─退避─► CREATING(g+1)
                              │
                           STOPPING → STOPPED
```

每阶段使用独立 ops，日志可定位阻塞/失败阶段。connect 返回后设置 `connected_deadline = now()+30s`；state callback 先记录 `observed_at`。当 owner 处理事件时，同 generation 且 `observed_at <= deadline` 的 CONNECTED 胜出，否则 deadline 胜出。

Task 1 取证推翻了 A1/A2 的前提（`ChannelInfo.reconnect` 为死配置，SDK 内置 reconnect **无法禁用**），最终策略为 **A3：与 SDK 单次重连共存 + 应用层全兜底**：

- **注册 `errorReportFn`**（SDK 可选回调，现有实现未注册——永久失活路径 1 的直接成因）：收到 `STATUS_SIGNALING_RECONNECT_FAILED` 只向 ControlMailbox 发布 RECONNECT_FAILED 事件，owner 消费后 release + backoff + recreate 下一 generation。
- **DISCONNECTED 裕量窗口**：state callback 报 DISCONNECTED 后，owner 给 SDK 内置 reconnect 20s 窗口（覆盖其 15s 状态机上界）；窗口内同 generation CONNECTED 到达则恢复并重置退避，超时则视同 RECONNECT_FAILED 进入 recreate。
- **活性检测（半开防线，永久失活路径 2 的唯一防御）**：SDK 无 ping/pong，服务端静默断开不产生任何 callback；但 SDK 每约 2h 的 ICE config refresh 必产生 state callback 序列（Task 0 实测 + 源码 refresh 状态机）。owner 维护 `last_signal_at`（任何 state/message callback 均刷新），超过 liveness timeout（默认 3h，可配置，>2h refresh 周期留裕量）无任何信号即判定半开，release + recreate。
- 应用不得对任何 handle 调第二次 ConnectSync；recreate 永远走 free → create → fetch → connect 的新 generation（同时天然刷新凭证与签名 URL）。饱和退避使用常量表 `{1,2,4,8,16,30}`；连接连续稳定 30s 才清零 attempt。signaling generation 与 peer generation 独立，client recreate 不触碰 peer map。

**Validates: Requirements 2.1, 2.2, 2.3, 2.4, 2.5**

### Component 4：Signaling CallbackBridge 与 MessageDispatcher

SignalingCallbackBridge 是 Impl 内的固定 slot（决策 B），保存 generation、`open`、`in_flight` 和对 runtime state 的引用；slot 地址在 Impl 生命周期内永不释放。callback 入口先 `fetch_add(in_flight)` 取得 lease，再校验 open/generation，失配则递减返回；client recreate 只做 `open=false` → release → generation 递增 → `open=true`，slot 在 in-flight 归零前不得进入下一 generation。

所有 C callback 包含 `try/catch (...)`，异常只增加 `callback_exception_count` 并返回 SDK 允许的错误值。message callback 在分配前验证 peer≤256、payload≤16KiB；队列规则：

1. 深度 ≤512。
2. 新 OFFER 满时淘汰最旧 ICE；全 OFFER 则拒绝新 OFFER。
3. 新 ICE 满时拒绝。
4. Dispatcher FIFO 执行业务 handler；同 peer 不并行。
5. generation 不匹配只累计 stale。

callback 的正确性断言是“不调用 handler、不等待业务锁”；wall-clock 仅采集分位数，不设 ASan 环境下的硬 10ms 门槛。

**Validates: Requirements 3.1, 3.2, 3.3, 3.4, 4.1, 4.2**

### Component 5：共享 KVS runtime 生命周期

`KvsRuntimeToken` 在 production adapter 内使用进程级 mutex + refcount：首个 token 调 `initKvsWebRtc()`，最后一个 token 且所有 signaling/peer handle 已释放后调 `deinitKvsWebRtc()`。client recreate 不反复 init/deinit。对象销毁顺序固定为：WebRtcMediaManager/Reaper → WebRtcSignaling/Owner → token/global runtime。

**Validates: Requirements 8.3**

### Component 6：PeerSession、HandlePermit 与两阶段创建

```cpp
struct PeerSession : std::enable_shared_from_this<PeerSession> {
    const std::string peer_id;
    const uint64_t generation;
    HandlePermit permit;                 // free 完成前不归还
    std::atomic<PeerState> state{PeerState::CONNECTING};
    std::atomic<bool> keyframe_pending{false};
    std::mutex io_mutex;
    PeerCallbackBridge* bridge;          // 指向池中固定 slot，不拥有、不释放（决策 B）
    std::unique_ptr<webrtc::internal::PeerHandle> handle;
};
```

permit 池容量 16，覆盖 active、creating、retired 全生命周期，因此 live SDK handles 天然 ≤16。创建前必须取得 permit；无 permit 时拒绝新建/替换，原 peer 不先摘除。

两阶段 offer：

1. Dispatcher 收到 OFFER，取得 permit 并创建尚无 handle 的 session/bridge。
2. map 锁内检查 active≤10；同 peer 旧 session 仅在新 session 已具备 permit 后摘除并提交 Reaper；插入新 generation 占位；解锁。
3. 锁外 create/register/negotiate/send answer。
4. 同步早到 callback 通过 bridge 直接更新 session，不依赖 map 查找。
5. 成功只在 map generation 匹配时发布；失败按 generation 摘除并提交 Reaper。

PeerCallbackBridge slot 与 HandlePermit 一一绑定：取得 permit 即取得对应 slot，归还 permit 前 slot 必须已完成 `open=false → close/free → in_flight==0` 序列（slot 内存不释放，仅标记可复用）。Reaper queue 容量 16；只有 Reaper close/free。若 queue、permit 与 slot 状态不一致，视为 invariant violation，拒绝新操作并记录 error，不得现场 free。

**Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5**

### Component 7：Reaper、僵尸状态与媒体 I/O

Reaper condition_variable 驱动，不忙轮询；下一 deadline 为最早 CONNECTING timeout、DISCONNECTING grace 或 shutdown。状态规则：

- CONNECTING 30s、FAILED/CLOSED、write failure >100 → DISCONNECTING。
- keyframe-only 阈值使用现有可配置值，只影响发送模式。
- DISCONNECTING 10s → map 按 generation 摘除并入 Reaper。
- close/free 完成且 slot in-flight==0 后销毁 session、标记 slot 可复用（slot 内存不释放）并归还 permit。

`broadcast_frame()` 在 map 锁内只复制 CONNECTED session 的 shared_ptr，随后逐 session 获取 I/O gate。Reaper 对同 session 使用同一 gate；Task 1 必须证明 write 有限返回，否则本组件停止。不同 session 无共享 I/O 锁，因此慢 peer 不阻塞 map 和其他 peer。

local ICE callback 只校验/复制 ≤4KiB candidate，并调用内部 `try_post_ice()`；它不等待公开 `send_ice_candidate()` 的 reply。peer CONNECTED callback 只设置 `keyframe_pending=true`，由后续非 callback 媒体路径执行既有 force-keyframe helper。

**Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.6**

### Component 8：Pending ICE、健康与资源预算

Pending ICE 用 FIFO + peer LRU：每 peer 50、peer 20、全局 200、单条 4KiB、TTL 30s。单 peer 满淘汰该 peer 最旧 candidate；peer/global 满淘汰最旧 pending peer；offer 到达后锁外 FIFO flush。

健康 snapshot 为 immutable copy，包含 state/generation/attempt/age、三个队列深度、active/retired/live handles 和分类计数。日志每类异常首次立即输出，此后 60s 最多一条并附 suppressed，10min 安静后重置。

最坏 payload 预算：command `256×16KiB=4MiB`，message `512×16KiB=8MiB`，pending ICE `200×4KiB≈0.8MiB`，剩余约 3.2MiB 用于 deque/string/session/bridge 元数据，总计 ≤16MiB。测试同时检查逻辑容量与 RSS 增量；累计计数按稳定窗口增长率判断。

**Validates: Requirements 5.5, 6.1, 6.2, 6.3, 6.4**

## SDK Semantic Gates

| Unknown | Evidence | Status | Date | Blocking scope |
|---------|----------|--------|------|----------------|
| SDK version/commit 与状态码符号 | 运行库 `strings /usr/local/lib/libkvsWebrtcClient.so`=ef4649473b=**v1.18.0**；源码基准 `~/Workspace/amazon-kinesis-video-streams-webrtc-sdk-c`（/opt 的 v1.12.1 为弃用旧克隆） | CONFIRMED | 2026-08-22 | 全部 adapter |
| retry/reconnect 语义 | `ChannelInfo.reconnect` 为死配置（全库仅 ChannelInfo.c:133 拷贝，无读取，**无法禁用**）；LwsApiCalls.c:340-415 CLOSED/CONN_ERROR 时无条件 detached `reconnectHandler` 单次驱动状态机重连（15s 上界），失败仅调可选 `errorReportFn`（`STATUS_SIGNALING_RECONNECT_FAILED`）后放弃——当前应用未注册该回调=永久失活路径 1；already-connected 时 connect 步 no-op 返回 OK（Signaling.c:1315，不产生双连） | CONFIRMED | 2026-08-22 | 决策 A 修订为 A3（见 Component 3） |
| create/fetch/connect/send/query 的有限上界与 callback 顺序 | create 10s（SIGNALING_CREATE_TIMEOUT）；connect 状态机 15s（SIGNALING_CONNECT_STATE_TIMEOUT，Signaling.c:572-595）；单次 ws connect 5s；HTTP API 2s conn+5s completion（LwsApiCalls.h:14-17）；**send=SIGNALING_SEND_TIMEOUT=5s（Linux 分支 Include.h:721，Windows 才是 15s）**；sendLwsMessage 在该超时内等待发送完成（Signaling.c:452-482） | CONFIRMED | 2026-08-22 | send `max_duration`=5s ≤10s，**SEND 路径不 BLOCKED**，决策 D 成立 |
| freeSignalingClient 阻塞上界 | `terminateOngoingOperations`（Signaling.c:430-450）= terminateLwsListenerLoop + await reconnecter，上界 SIGNALING_CLIENT_SHUTDOWN_TIMEOUT=9s（Signaling.h:24） | CONFIRMED | 2026-08-22 | owner release 阶段（callback quiescence 由 slot 池免除） |
| init/deinit 全局语义；deinit 是否 join 全部内部线程 | `deinitKvsWebRtc`（PeerConnection.c:1894）= srtp_shutdown + destroyThreadPoolContext（threadpool join）；timer queue 为 per-peer 资源由 freePeerConnection 处理 | CONFIRMED | 2026-08-22 | 池析构安全；无需进程退出兜底 |
| Peer create/description 同步 callback | `changePeerConnectionState` 同步调 onConnectionStateChange（可早于应用发布到 map）；`onNewIceLocalCandidate`（PeerConnection.c:549-577）在 SDK **持 peerConnectionObjLock 时同步调应用回调**——回调内阻塞（如 5s send）即占住 SDK 锁，回调内调 Peer SDK API 即死锁 | CONFIRMED | 2026-08-22 | 两阶段创建 + local ICE fire-and-forget 为**必要**而非优化 |
| close/free 阻塞上界与幂等 | closePeerConnection（PeerConnection.c:1774）= dtlsSessionShutdown + iceAgentShutdown，有限阻塞；freePeerConnection 释放 per-peer timer queue 等资源 | CONFIRMED | 2026-08-22 | Reaper（callback quiescence 由 slot 池免除） |
| writeFrame 阻塞、frameData 所有权、与 free 并发 | Rtp.c:262-360：持 pSrtpSessionLock 做分包+SRTP 加密+iceAgentSend（UDP 非阻塞），上界毫秒级；frameData 同步消费，**返回后调用方即可释放**；与 free 通过 srtpSessionLock 串行，但 transceiver 句柄悬空须由应用 I/O gate 防护 | CONFIRMED | 2026-08-22 | I/O gate |
| SRTP-not-ready 状态符号 | `STATUS_SRTP_NOT_READY_YET` Include.h:279 | CONFIRMED | 2026-08-22 | write 分类（替换裸 0x5c000003） |
| **半开连接检测（新增）** | 全 LwsApiCalls.c 无 WebSocket ping/pong/keepalive/lws retry 配置——服务端静默断开（NAT 超时等）不触发 CLOSED callback，SDK 永远自认 connected=永久失活路径 2；上游至 v1.20.0 changelog 无针对性修复 | CONFIRMED | 2026-08-22 | 应用层活性检测为**唯一**防线（Component 3） |

Task 1 结论：**全部 CONFIRMED，无 BLOCKED**（`scripts/webrtc-sdk-probe.sh` 采集 + 人工源码核对）。两条永久失活路径与 Task 0 观测（offer 达 AWS 而设备未收）闭环。SDK 升级至 v1.20.0（含 #2372 stale TURN credentials 修复）记入 backlog，不在本 Spec。

## Data Models

| 模型 | 所有者 | 上限/关键字段 | 并发规则 |
|------|--------|---------------|----------|
| `Command` | SignalingOwner queue | 256；id/generation/deadline/state/reply | caller 只提交/取消，owner exactly-once 完成 |
| `ControlMailbox` | SignalingOwner | SHUTDOWN 带外；state 按 generation 合并 | callback 只发布，owner 优先消费 |
| `SignalingMessageEvent` | MessageDispatcher | 512；peer≤256B、payload≤16KiB | callback try-enqueue，dispatcher FIFO 消费 |
| `SignalingCallbackBridge` | Impl 固定 slot（1 个） | generation/open/in-flight | slot 永不释放；in-flight==0 后才进入下一 generation |
| `PeerSession` | peer map + Reaper shared_ptr | active≤10；持 HandlePermit | map 锁只保护容器，I/O 走 session gate |
| `PeerCallbackBridge` | Impl 固定 slot 池（16 个，与 permit 绑定） | generation/open/in-flight | slot 永不释放；open=false 且 in-flight==0 后复用 |
| `HandlePermitPool` | media Impl | 16 | 覆盖 active/creating/retired handle |
| `PendingIceStore` | media Impl | 20 peer/50 each/200 global/30s TTL | peer LRU + candidate FIFO |
| `HealthSnapshot` | owner/Reaper 发布 | immutable counters/high-water marks | 日志线程只读副本 |

## Correctness Properties

### Property 1: 单 owner 与命令 exactly-once
_For any_ command/state/shutdown 并发序列，handle 操作线程恒为 owner；command reply 至多一次，过期/取消 QUEUED command 不调用 SDK，release 后无操作。
**Validates: Requirements 1.1, 1.2, 1.3, 1.4, 1.5**

### Property 2: 恢复最终进展
_For any_ 10,000 次分阶段失败/成功/丢失 callback 序列，每个 generation 要么按 observed_at 在 deadline 内 CONNECTED，要么 release 并进入下一 generation；退避恒属于 `{1,2,4,8,16,30}`。
**Validates: Requirements 2.1, 2.2, 2.3, 2.4**

### Property 3: Generation 隔离
_For any_ signaling/peer 当前 generation `g` 与事件 `e`，仅 `e==g` 可改变当前状态；旧事件只计数。
**Validates: Requirements 2.5, 3.3, 4.4**

### Property 4: 消息队列有界且 callback 结构轻量
_For any_ OFFER/ICE 序列，message 深度 ≤512，OFFER 按规则优先；SDK callback 不调用 handler/Peer SDK且不等待业务锁。
**Validates: Requirements 3.1, 3.2, 3.4**

### Property 5: Bridge slot 安全与 peer exactly-once retirement
_For any_ 同步/延迟/任意迟到 callback、replace/remove/fail/shutdown 序列，slot 地址全程稳定且迟到 callback 零副作用，slot 复用仅发生在 open=false 且 in-flight==0 之后，每 generation close/release 至多一次，所有 PeerSdkOps 调用均不持 map mutex。
**Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5**

### Property 6: Peer/ICE/内存资源有界
_For any_ peer/ICE/状态序列，active≤10、live handles≤16、Reaper≤16、pending peer≤20、per-peer ICE≤50、global ICE≤200，估算 payload≤16MiB。
**Validates: Requirements 5.1, 5.5, 6.4**

### Property 7: 媒体与 local ICE 隔离
_For any_ frame/free/local ICE 并发序列，map 锁外执行 write，单 session write/free 串行，local ICE callback 不等待 signaling reply，keyframe 请求不在 SDK callback 操作 GStreamer。
**Validates: Requirements 5.2, 5.3, 5.4, 5.6**

### Property 8: Preservation 与分层长稳
_For any_ 合法操作序列，公共签名、10 Viewer、replace、early ICE、Annex B 行为保持；虚拟 72h 后资源有界，真实整机 soak 只在生命周期前置满足后判定。
**Validates: Requirements 7.1, 7.2, 7.3, 7.4, 7.5, 8.3, 8.4**

## Requirements Traceability

| Requirement | Components/Decisions | Properties | Tasks |
|-------------|----------------------|------------|-------|
| 1.1–1.5 | A, D, 1,2,4 | 1 | 1,2,3,6 |
| 2.1–2.5 | A, 2,3 | 2,3 | 1,2,6 |
| 3.1–3.4 | 4 | 3,4 | 2,3,6 |
| 4.1–4.5 | B, 4,6 | 3,5 | 1,4,6 |
| 5.1–5.6 | 6,7,8 | 6,7 | 1,4,5,6 |
| 6.1–6.4 | 8 | 6 | 2,3,4,5,6 |
| 7.1–7.5 | C, 1–8 | 8 | 0–6 |
| 8.1–8.4 | A,B,D,1,5 + Gates | 5,8 | 1,2,4,6 |

## Error Handling

- 队列满/closed、完成 deadline 预算不足、generation 失配：完成失败且不调用 SDK。
- RUNNING command：只可在剩余完成 deadline 覆盖已确认 `max_duration` 时启动（QUERY 2s / SEND 10s）；若 SDK 不能提供该契约，Task 1 阻断该路径。
- create/fetch/connect 失败或 CONNECTED deadline：release generation，饱和退避 recreate。
- callback 输入非法/分配异常：C ABI 内捕获，分类计数，不传播异常。
- HandlePermit 不足/Reaper 背压：拒绝新建或替换，保留现有 peer，不现场 free。
- 影响运行时路径的阻塞或所有权语义 BLOCKED：停止对应 Task，修订 Spec；仅影响析构收尾的 deinit BLOCKED 用进程退出兜底并记录。

## Testing Strategy

- Linux production adapter 位于对应 `.cpp` 的 `#ifdef HAVE_KVS_WEBRTC_SDK`，回调类型放在 private Impl/adapter 内，遵守 GCC private 访问规则。
- macOS 使用同一 runtime 和 fake ops，不保留“立即 connected”的平行状态机。
- `device/tests/webrtc_test.cpp`：signaling factory、command、owner、deadline、reconnect、shutdown、固定 sleep 替换。
- `device/tests/webrtc_media_test.cpp`：dispatcher、PeerSession、bridge、permit、Reaper、I/O/ICE/PBT，替换吞吐比值测试。
- manual clock advance 必须显式唤醒 owner；事件门禁替代固定 sleep。ASan 验证 UAF/越界，确定性锁探针验证并发结构；如环境支持可另跑 TSan，但不作为本 Spec 必需依赖。
- 统一验证：`ctest --test-dir device/build --output-on-failure`。Pi 用 `scripts/pi-build.sh`，部署用 `scripts/pi-deploy.sh`。

## Performance and Resource Budget

- 常驻 worker 恰好 3 个；全部 condition_variable 阻塞，Pi idle CPU 增量目标 <单核 1%。
- 纯状态机测试 ≤1s；10,000 序列/并发测试 ≤15s；不等待真实退避。
- callback 结构不执行 handler/外部 SDK/业务锁；仅记录宽松 latency 分位数。
- 内存按 Component 8 公式 ≤16MiB；live handles≤16。

## SHALL NOT（Design 层）

- SHALL NOT 让 SDK reconnect 与应用 ConnectSync 同时操作同一 handle。
- SHALL NOT 将 combined `create_fetch_connect()` 当作不可分的黑盒，掩盖阻塞阶段。
- SHALL NOT 让 QUERY 类 caller timeout 后的 QUEUED command 继续执行；SHALL NOT 让任何超过完成 deadline 的 command 启动 SDK 调用。
- SHALL NOT 在 SDK callback 中执行业务 handler、GStreamer 或等待 map/session 锁。
- SHALL NOT 在 Impl 存活期间释放任何 bridge slot 内存，或以 shared_ptr/generation 声称能保护已释放的 raw 地址。
- SHALL NOT 用 detached worker、固定 sleep、裸状态码或吞吐比值证明安全。
- SHALL NOT 在 peer map 锁内调用外部 API。
- SHALL NOT 在 peer/signaling handle 存活时 deinit 全局 SDK runtime。
- SHALL NOT 输出凭证、token、完整 SDP 或 ICE credential。

## File Change List

```text
device/src/webrtc_signaling.h
device/src/webrtc_signaling.cpp
device/src/webrtc_media.h
device/src/webrtc_media.cpp
device/tests/webrtc_test.cpp
device/tests/webrtc_media_test.cpp
```

共 6 个既有文件；不改 CMake，不新增依赖，不修改 AppContext/pipeline/systemd。6 文件是有记录的粒度例外，原因见 Requirements 准入三问。
