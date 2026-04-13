# Bugfix Requirements Document

## Introduction

WebRTC viewer 在服务重启后首次连接成功，但后续 viewer 连接静默失败。第二个 viewer 的 SDP offer 被 signaling 回调接收，但无 SDP answer 返回。根因是 `webrtc_media.cpp` 中的死锁：`on_connection_state_change(CLOSED)` 回调在 SDK 内部线程上持有 `peers_mutex` 后调用阻塞式 `freePeerConnection()`，而新 viewer 的 `on_signaling_message_received` 在另一个 SDK 线程上等待 `peers_mutex`，形成死锁。`broadcast_frame()` 在 GStreamer 线程上也存在相同的死锁风险。

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN viewer 断开连接触发 `on_connection_state_change(CLOSED/FAILED)` 回调，该回调在 SDK 内部线程上获取 `peers_mutex` 后调用 `remove_peer_locked()` → `freePeerConnection()`（阻塞等待 SDK 内部线程完成清理） THEN 系统死锁，因为 `freePeerConnection` 等待的 SDK 内部线程可能需要 signaling 线程完成工作，而 signaling 线程被新 viewer 的 offer 处理阻塞在 `peers_mutex` 上

1.2 WHEN `broadcast_frame()` 在 GStreamer 流线程上持有 `peers_mutex` 并对写入失败超阈值的 peer 调用 `remove_peer_locked()` → `freePeerConnection()` THEN 系统死锁风险相同，`freePeerConnection` 阻塞等待 SDK 内部线程，而这些线程可能需要获取被 GStreamer 线程间接阻塞的资源

1.3 WHEN 第一个 viewer 断开后新 viewer 发送 SDP offer THEN `on_viewer_offer()` 尝试获取 `peers_mutex` 被无限期阻塞，SDP answer 永远不会发送，viewer 连接静默失败

1.4 WHEN `Impl` 析构函数持有 `peers_mutex` 并对所有 peer 调用 `freePeerConnection()` THEN 如果此时有 SDK 回调线程等待 `peers_mutex`，同样存在死锁风险

1.5 WHEN `broadcast_frame()` 使用独占锁 `std::mutex` 遍历 peers 发送帧数据 THEN 帧发送期间所有 offer 处理和 peer 管理操作被阻塞，高帧率场景下加剧锁竞争

### Expected Behavior (Correct)

2.1 WHEN `on_connection_state_change(CLOSED/FAILED)` 回调触发 THEN 系统 SHALL 仅将 peer 状态标记为 DISCONNECTING 并记录时间戳，不调用 `freePeerConnection()`，不阻塞 SDK 回调线程

2.2 WHEN peer 处于 DISCONNECTING 状态超过清理超时时间（10 秒） THEN 系统 SHALL 由独立的清理线程调用 `closePeerConnection()` 后再调用 `freePeerConnection()` 释放资源，确保不在任何 SDK 回调线程或 GStreamer 线程上执行阻塞式释放

2.3 WHEN `broadcast_frame()` 检测到 peer 写入失败超阈值 THEN 系统 SHALL 仅将该 peer 标记为 DISCONNECTING，不在 GStreamer 流线程上调用 `freePeerConnection()`

2.4 WHEN 新 viewer 发送 SDP offer 且存在 DISCONNECTING 状态的 peer THEN 系统 SHALL 在创建新 PeerConnection 之前强制清理所有过期的 DISCONNECTING peer（`closePeerConnection` + `freePeerConnection`，在锁外执行），确保 SDK 资源及时回收

2.4.1 WHEN `on_viewer_offer` 收到已存在 peer_id 的新 offer 需要替换旧 PeerConnection THEN 系统 SHALL 将旧 peer 从 map 中移除并收集到待释放列表，在释放 `peers_mutex` 后再调用 `closePeerConnection` + `freePeerConnection`

2.5 WHEN `broadcast_frame()` 遍历 peers 发送帧数据 THEN 系统 SHALL 使用 `std::shared_lock`（读锁），仅在 peer 增删时使用 `std::unique_lock`（写锁），减少帧发送对 offer 处理的阻塞

2.6 WHEN 系统关闭（`Impl` 析构） THEN 系统 SHALL 先对所有 peer 调用 `closePeerConnection()`，等待短暂间隔（1 秒）后再调用 `freePeerConnection()`，且不持有 `peers_mutex` 执行阻塞式释放

2.7 WHEN 系统执行 `freePeerConnection()` THEN 系统 SHALL NOT 在持有 `peers_mutex` 的情况下调用该函数

2.8 WHEN SDK 回调线程触发 `on_connection_state_change` THEN 系统 SHALL NOT 在该回调线程上直接调用 `freePeerConnection()`

### Unchanged Behavior (Regression Prevention)

3.1 WHEN macOS 开发环境编译（无 `HAVE_KVS_WEBRTC_SDK` 宏） THEN 系统 SHALL CONTINUE TO 使用 stub 实现，所有现有测试通过

3.2 WHEN 多个 viewer 同时连接（≤ 10 个） THEN 系统 SHALL CONTINUE TO 正确管理 peer 生命周期，包括创建、替换、移除和最大连接数限制

3.3 WHEN peer 连接状态变为 FAILED 或 CLOSED THEN 系统 SHALL CONTINUE TO 最终清理 PeerConnection 资源和 callback context，防止内存泄漏

3.4 WHEN 调用 `broadcast_frame` 发送 H.264 帧 THEN 系统 SHALL CONTINUE TO 向所有已连接 peer 的 video transceiver 发送帧数据，超过失败阈值时标记 peer 待清理

3.5 WHEN `on_viewer_offer` 收到已存在 peer_id 的新 offer THEN 系统 SHALL CONTINUE TO 替换旧 PeerConnection 并创建新连接

3.6 WHEN `on_viewer_ice_candidate` 在 offer 处理前收到 ICE candidate THEN 系统 SHALL CONTINUE TO 缓存 candidate 并在 PeerConnection 创建后 flush

3.7 WHEN `peer_count()` 被调用 THEN 系统 SHALL CONTINUE TO 返回当前活跃 peer 数量（仅计 CONNECTED 状态的 peer，不含 CONNECTING 和 DISCONNECTING 状态）
