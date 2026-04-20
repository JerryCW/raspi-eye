# Signaling WebSocket 断连自动重连 — Bugfix Design

## 概述

KVS WebRTC SDK 的 `reconnect=TRUE` 机制仅将 signaling 状态恢复到 READY（完成 Describe/Create/GetIceConfig），但不会自动调用 `signalingClientConnectSync` 重新建立 WebSocket 连接。导致 signaling 停留在 READY 状态，viewer 无法连接。

修复策略：在 `WebRtcSignaling::Impl` 内部新增重连监控线程，通过 `atomic<bool> needs_reconnect_` 从 SDK 回调通知重连线程，重连线程在独立线程中调用 `signalingClientConnectSync`（避免在 SDK 回调线程中调用导致死锁）。使用指数退避（1s→30s cap）+ 连接稳定 30 秒重置退避 + 无限重连（7×24 设备）。同时增强日志可观测性。

修改范围：
- `device/src/webrtc_signaling.h` — Impl 新增成员声明（pImpl 隐藏）
- `device/src/webrtc_signaling.cpp` — 重连线程实现 + 日志增强（Linux 实现 + macOS stub）
- `device/tests/webrtc_test.cpp` — 新增重连相关测试

不涉及 `app_context.cpp` 修改（重连逻辑完全封装在 WebRtcSignaling 内部）。

## 术语表

- **Bug_Condition (C)**: signaling WebSocket 断连后，SDK 恢复到 READY 状态但无代码调用 `signalingClientConnectSync` 重建 WebSocket，导致 signaling 停留在 READY 无法接收 viewer offer
- **Property (P)**: 断连后重连监控线程检测到 `needs_reconnect_` 标志，在退避等待后调用 `signalingClientConnectSync` 重建 WebSocket，恢复 CONNECTED 状态
- **Preservation**: 首次连接流程、显式 disconnect、send_answer/send_ice_candidate、ICE config 查询、macOS stub 行为保持不变
- **signalingClientConnectSync**: KVS WebRTC SDK API，建立 WebSocket 连接，阻塞直到连接完成或失败。不可在 SDK 回调线程中调用（会死锁）
- **needs_reconnect_**: `std::atomic<bool>` 标志，由 `on_signaling_state_changed(DISCONNECTED)` 回调设置，由重连监控线程消费
- **shutdown_requested_**: `std::atomic<bool>` 标志，区分显式关闭（disconnect/析构）和意外断连，防止关闭时触发自动重连
- **reconnect_thread_**: 独立线程，循环等待 `needs_reconnect_` 信号，执行退避等待后调用 `signalingClientConnectSync`

## Bug 详情

### Bug Condition

signaling WebSocket 断连后，SDK 内置 `reconnect=TRUE` 将状态恢复到 READY，但系统不调用 `signalingClientConnectSync` 重建 WebSocket。signaling 停留在 READY 状态，无法接收 viewer 的 SDP offer。

**形式化规约：**
```
FUNCTION isBugCondition(input)
  INPUT: input of type SignalingStateEvent
  OUTPUT: boolean

  RETURN input.previous_state == CONNECTED
         AND input.current_state == DISCONNECTED
         AND sdk_auto_recovery_reaches(READY)
         AND NOT signalingClientConnectSync_called_after_recovery()
END FUNCTION
```

### 示例

- **示例 1（典型断连）**: signaling 在 11:02:23 断连 → SDK 自动恢复到 READY(6) → 之后再无 CONNECTING/CONNECTED 状态转换 → 所有 viewer 连接超时（Pi 5 生产日志证据）
- **示例 2（网络抖动）**: WiFi 短暂断开 5 秒 → signaling DISCONNECTED → SDK 恢复到 READY → 无重连 → viewer 永久失联
- **示例 3（长时间运行）**: 设备运行 48 小时后 signaling 断连 → 无重连机制 → 需要手动重启服务
- **示例 4（正常情况，不触发 bug）**: signaling 首次连接成功且未断连 → 正常接收 viewer offer → 无需重连

## Expected Behavior

### Preservation Requirements

**不变行为：**
- `WebRtcSignaling::connect()` 首次连接流程（create_and_connect）保持不变
- `WebRtcSignaling::disconnect()` 显式关闭流程保持不变，设置 `shutdown_requested_` 后不触发自动重连
- `send_answer()` / `send_ice_candidate()` 在已连接时正常发送，未连接时返回 false
- `set_offer_callback()` / `set_ice_candidate_callback()` 回调注册保持不变
- `get_ice_config_count()` / `get_ice_config()` ICE 配置查询保持不变
- macOS stub 编译和测试通过，stub 需模拟重连行为
- 所有日志使用 "webrtc" logger

**范围：**
所有不涉及 signaling 断连恢复的行为应完全不受此修复影响。包括：
- 首次连接成功后的正常 signaling 消息收发
- 显式调用 disconnect() 的正常关闭流程
- peer connection 管理（由 WebRtcMediaManager 负责，不受影响）
- ICE 配置查询

## 假设根因分析

基于 Pi 5 生产日志和代码审查，根因是：

1. **缺少重连触发机制**: `on_signaling_state_changed(DISCONNECTED)` 回调仅设置 `connected = false`，不触发任何重连动作。SDK 的 `reconnect=TRUE` 只恢复到 READY，不调用 `signalingClientConnectSync`

2. **死代码**: `WebRtcSignaling::reconnect()` 方法存在但从未被调用。且该方法实现为 `release_signaling_client()` + `create_and_connect()`（完全重建），而实际只需在 SDK 恢复到 READY 后调用 `signalingClientConnectSync` 即可

3. **SDK 回调线程限制**: 不能在 `on_signaling_state_changed` 回调中直接调用 `signalingClientConnectSync`，因为回调运行在 SDK 内部线程上，同步调用会死锁。必须在独立线程中执行

4. **无健康监控**: `log_health_status()` 存在但从未被周期性调用，无法在运行时观测 signaling 状态异常

## Correctness Properties

Property 1: Bug Condition — 断连后自动重连恢复 WebSocket

_For any_ signaling 状态事件序列中出现 DISCONNECTED 且 `shutdown_requested_` 为 false 的情况，修复后的实现 SHALL 设置 `needs_reconnect_` 标志，重连监控线程 SHALL 在指数退避等待后调用 `signalingClientConnectSync` 尝试重建 WebSocket 连接，直到连接成功或 `shutdown_requested_` 被设置。

**Validates: Requirements 2.1, 2.2, 2.3, 2.5, 2.6**

Property 2: Preservation — 非断连场景行为不变

_For any_ 不涉及 signaling 断连的操作（首次连接、显式 disconnect、send_answer、send_ice_candidate、ICE config 查询），修复后的实现 SHALL 产生与原实现相同的结果，保持所有现有功能正常工作。

**Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7**

## Fix Implementation

### 线程模型

```
┌─────────────────────────────────────────────────────────────┐
│                    WebRtcSignaling::Impl                     │
│                                                              │
│  ┌──────────────┐    needs_reconnect_    ┌────────────────┐ │
│  │ SDK 回调线程  │ ──── atomic<bool> ───→ │ reconnect_thread│ │
│  │ (state_changed│                        │ (独立线程)      │ │
│  │  DISCONNECTED)│    shutdown_requested_ │                 │ │
│  └──────────────┘ ←── atomic<bool> ────  │ 指数退避等待    │ │
│                                           │ signalingClient │ │
│  ┌──────────────┐                        │ ConnectSync()   │ │
│  │ disconnect() │ ── set shutdown ──→    │                 │ │
│  │ ~Impl()      │ ── cv.notify_all() ──→ │ 检测 shutdown   │ │
│  └──────────────┘                        │ 安全退出        │ │
│                                           └────────────────┘ │
│                                                              │
│  connected flag 由 SDK 回调设置，重连线程不直接修改           │
└─────────────────────────────────────────────────────────────┘
```

### 指数退避策略

| 重连次数 | 退避时间 | 累计等待 |
|---------|---------|---------|
| 1 | 1s | 1s |
| 2 | 2s | 3s |
| 3 | 4s | 7s |
| 4 | 8s | 15s |
| 5 | 16s | 31s |
| 6+ | 30s (cap) | 61s+ |

- 连接稳定超过 30 秒（`kStableConnectionSec`）才重置退避计数器
- 连接不足 30 秒又断开 → 退避计数器不重置，防止快速重连风暴
- 无最大重连次数限制（7×24 设备）

### 数据结构变更

**Impl 新增成员（Linux 实现）：**

```cpp
struct WebRtcSignaling::Impl {
    // ... 现有字段 ...

    // 重连监控
    std::atomic<bool> needs_reconnect_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::thread reconnect_thread_;
    std::mutex reconnect_mutex_;
    std::condition_variable reconnect_cv_;

    // 统计信息
    std::chrono::steady_clock::time_point connected_at_;
    uint32_t total_disconnects_ = 0;
    uint32_t total_reconnects_ = 0;
    uint32_t reconnect_attempt_ = 0;  // 当前退避计数器

    // 常量
    static constexpr int kInitialBackoffSec = 1;
    static constexpr int kMaxBackoffSec = 30;
    static constexpr int kStableConnectionSec = 30;
    static constexpr int kHealthCheckIntervalSec = 60;

    void reconnect_loop();  // 重连监控线程入口
};
```

**Impl 新增成员（macOS stub）：**

```cpp
struct WebRtcSignaling::Impl {
    // ... 现有字段 ...

    // stub 重连模拟
    std::atomic<bool> needs_reconnect_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::thread reconnect_thread_;
    std::mutex reconnect_mutex_;
    std::condition_variable reconnect_cv_;

    uint32_t total_disconnects_ = 0;
    uint32_t total_reconnects_ = 0;

    void reconnect_loop();  // stub 版本
};
```

### 具体变更

**文件**: `device/src/webrtc_signaling.cpp`

**1. on_signaling_state_changed 回调增强（Linux 实现）**:
- CONNECTED 状态：记录 `connected_at_` 时间戳（退避重置判断由 reconnect_loop 健康检查执行，不在回调中判断）
- DISCONNECTED 状态：设置 `needs_reconnect_ = true`，递增 `total_disconnects_`，通知 `reconnect_cv_`，记录断连持续时长（`now - connected_at_`）和累计断连次数
- READY 状态：日志区分首次连接（`total_disconnects_ == 0`）与断连恢复（`total_disconnects_ > 0`）

**2. reconnect_loop() 新增（Linux 实现）**:
- 循环使用 `condition_variable::wait_for(kHealthCheckIntervalSec)` 等待（兼顾重连信号和健康检查）
- 每次唤醒（无论是 `needs_reconnect_` 信号、健康检查超时、还是 shutdown 通知）：
  - 检查 `shutdown_requested_`，为 true 则安全退出
  - 执行健康检查日志（每 60 秒一次）：正常连接 debug 级别，异常状态 warn 级别
  - 检查退避重置条件：如果当前 `connected == true` 且 `connected_at_` 距今超过 `kStableConnectionSec`（30 秒），重置 `reconnect_attempt_ = 0`
  - 检测到 `needs_reconnect_` 后：消费标志（设为 false），计算退避时间（`min(kInitialBackoffSec << reconnect_attempt_, kMaxBackoffSec)`），等待退避时间（可被 shutdown 唤醒），调用 `signalingClientConnectSync`
- `signalingClientConnectSync` 成功（返回 STATUS_SUCCESS）：递增 `total_reconnects_`，记录恢复日志（包含重连次数和恢复耗时）。注意：`connected = true` 由 SDK 回调设置，重连线程不直接修改
- `signalingClientConnectSync` 失败（返回非 SUCCESS 状态码）：记录错误日志（包含 status hex 和已尝试次数），递增 `reconnect_attempt_`，重新设置 `needs_reconnect_ = true`，继续循环。不区分失败原因，统一走退避重试

**3. reconnect_loop() 新增（macOS stub）**:
- 模拟重连行为：检测到 `needs_reconnect_` 后，短暂等待，设置 `connected = true`
- 保持与 Linux 实现相同的线程生命周期管理

**4. connect() 方法增强**:
- 首次连接成功后启动 `reconnect_thread_`
- 记录 `connected_at_` 时间戳

**5. disconnect() 方法增强**:
- 设置 `shutdown_requested_ = true`
- 通知 `reconnect_cv_`
- join `reconnect_thread_`（如果 joinable）
- 然后执行原有的 `release_signaling_client()`

**6. ~Impl() 增强**:
- 确保 `shutdown_requested_ = true` + join `reconnect_thread_`
- 然后执行原有的资源释放

**7. log_health_status() 增强**:
- 正常连接中：debug 级别，包含连接持续时长、累计断连/重连次数
- 异常状态（DISCONNECTED、重连中、READY 但未 CONNECTED）：warn 级别

**8. send_answer() / send_ice_candidate() 日志增强**:
- 未连接时打印当前 signaling 状态码（而非仅 "not connected"）

**9. 现有 reconnect() 公共方法**:
- 可以保留但标记为内部使用，或删除（重连逻辑已由 reconnect_thread_ 自动处理）
- 建议：保留方法签名但内部实现改为设置 `needs_reconnect_` 标志（触发自动重连流程），而非直接 release + create_and_connect

**文件**: `device/src/webrtc_signaling.h`

- 无公共接口变更（所有新增成员在 Impl 内部，pImpl 隐藏）
- 可选：保留或移除 `reconnect()` 公共方法

**文件**: `device/tests/webrtc_test.cpp`

- 新增 stub 重连行为测试

### 关键常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `kInitialBackoffSec` | 1 | 初始退避时间 |
| `kMaxBackoffSec` | 30 | 最大退避时间上限 |
| `kStableConnectionSec` | 30 | 连接稳定判定阈值（超过此时长才重置退避） |
| `kHealthCheckIntervalSec` | 60 | 健康检查日志间隔 |

### SHALL NOT（Design 层）

- SHALL NOT 在 SDK 回调线程中调用 `signalingClientConnectSync`（来源：KVS WebRTC SDK 线程模型限制）
  - 原因：SDK 回调运行在内部线程上，同步调用会死锁
  - 建议：通过 atomic flag + condition_variable 通知独立重连线程

- SHALL NOT 在 pImpl 模式中将需要访问 private Impl 的回调函数或辅助类型定义在 Impl 外部（来源：shall-not.md）
  - 原因：GCC 严格检查 private 访问，macOS Clang 走 stub 路径不报错，Pi 5 上才暴露

- SHALL NOT 在日志中打印密钥、证书内容、token 等敏感信息（来源：shall-not.md）

- SHALL NOT 在非 pipeline 模块中使用 `spdlog::get("pipeline")` logger（来源：shall-not.md）
  - 建议：统一使用 "webrtc" logger

- SHALL NOT 对含 `std::atomic` 成员的结构体使用 `unordered_map::emplace` 或 `insert`（来源：shall-not.md）
  - 建议：改用 `try_emplace` + 就地构造后逐字段赋值

## Testing Strategy

### 验证方法

测试策略分两阶段：先在未修复代码上验证 bug 存在（探索性测试），再验证修复后行为正确且不引入回归。

### Exploratory Bug Condition Checking

**目标**: 在修复前验证断连后无重连机制，确认根因分析正确。

**测试计划**: 在 stub 实现上模拟断连场景，验证断连后 `is_connected()` 保持 false 且无自动恢复。在未修复代码上运行以观察失败。

**测试用例**:
1. **断连无恢复测试**: stub 连接成功 → 模拟断连（disconnect） → 等待 2 秒 → 验证 `is_connected()` 仍为 false（未修复代码会通过此测试，因为确实无重连机制）
2. **reconnect() 死代码验证**: 检查现有代码中无任何路径调用 `reconnect()`（代码审查级别，非运行时测试）
3. **log_health_status 未调用验证**: 检查现有代码中无周期性调用 `log_health_status()`（代码审查级别）

**预期反例**:
- 断连后系统不尝试重连，signaling 永久停留在未连接状态
- 根因确认：`on_signaling_state_changed(DISCONNECTED)` 仅设置 `connected = false`，无后续动作

### Fix Checking

**目标**: 验证修复后，所有断连场景都能触发自动重连。

**伪代码：**
```
FOR ALL input WHERE isBugCondition(input) DO
  result := reconnect_loop_fixed(input)
  ASSERT needs_reconnect_ 被设置
  ASSERT reconnect_thread_ 在退避后调用 signalingClientConnectSync
  ASSERT 最终 connected == true（stub 中模拟成功）
END FOR
```

### Preservation Checking

**目标**: 验证修复后，所有不涉及断连的操作产生与原实现相同的结果。

**伪代码：**
```
FOR ALL input WHERE NOT isBugCondition(input) DO
  ASSERT original_behavior(input) == fixed_behavior(input)
END FOR
```

**测试方法**: Property-based testing 推荐用于 preservation checking，因为：
- 自动生成大量随机操作序列覆盖 signaling 的各种状态组合
- 捕获手动测试可能遗漏的边界情况
- 强保证所有非断连输入的行为不变

**测试计划**: 在未修复代码上观察 stub 实现的 connect/disconnect/send 行为，然后编写 PBT 验证修复后行为一致。

**测试用例**:
1. **Connect/Disconnect Preservation**: 验证随机 connect/disconnect 序列后 `is_connected()` 状态正确
2. **Send Preservation**: 验证已连接时 send_answer/send_ice_candidate 返回 true，未连接时返回 false
3. **Shutdown Safety**: 验证 disconnect() 后重连线程安全停止，不产生悬挂线程

### Unit Tests

- 测试 stub 断连后自动重连恢复（`needs_reconnect_` → reconnect_thread_ → connected）
- 测试 shutdown_requested_ 阻止自动重连
- 测试指数退避计算逻辑（1s→2s→4s→8s→16s→30s cap）
- 测试连接稳定 30 秒后退避重置
- 测试 disconnect() 安全停止重连线程
- 测试 log_health_status() 输出正确信息

### Property-Based Tests

- 随机 connect/disconnect/simulate_reconnect 操作序列验证 `is_connected()` 状态一致性
- 随机退避参数验证退避时间始终在 [1s, 30s] 范围内
- 随机 send 操作验证连接状态与 send 结果的一致性

### Integration Tests

- Pi 5 端到端：signaling 连接 → 手动断网 → 观察自动重连日志 → 恢复后 viewer 可连接
- Pi 5 端到端：长时间运行（>1 小时）验证健康检查日志输出
- Pi 5 端到端：disconnect() 后验证无重连尝试
