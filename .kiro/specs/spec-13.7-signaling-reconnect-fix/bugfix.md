# Bugfix Requirements Document

## Introduction

WebRTC signaling 在长时间运行后断连，KVS WebRTC SDK 的内置 `reconnect=TRUE` 机制仅将 signaling 状态恢复到 READY（完成 Describe/Create/GetIceConfig），但不会自动调用 `signalingClientConnectSync` 重新建立 WebSocket 连接。导致 signaling 停留在 READY 状态，viewer 无法连接。

Pi 5 生产日志证据：signaling 在 11:02:23 断连后，SDK 自动恢复到 READY(6) 状态，但之后再无 CONNECTING/CONNECTED 状态转换，所有 viewer 连接超时。

此问题与 spec-13.6（peer lifecycle 死锁）无关。spec-13.6 修的是 `freePeerConnection` 在 SDK 回调线程上阻塞导致的死锁，本 spec 修的是 signaling WebSocket 断连后缺少重建机制。

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN signaling WebSocket 断连触发 `on_signaling_state_changed(DISCONNECTED)` 回调 THEN 系统仅设置 `connected = false`，不触发任何重连动作

1.2 WHEN SDK 内置 `reconnect=TRUE` 将 signaling 状态从 DISCONNECTED 恢复到 READY（经过 DESCRIBE → CREATE → GET_ICE_CONFIG → READY） THEN 系统不调用 `signalingClientConnectSync` 建立 WebSocket，signaling 停留在 READY 状态，无法接收 viewer 的 SDP offer

1.3 WHEN signaling 处于 READY 但未 CONNECTED 状态时 viewer 尝试连接 THEN viewer 连接超时失败，因为 WebSocket 未建立，signaling 无法中转 SDP offer/answer 和 ICE candidate

1.4 WHEN `WebRtcSignaling::reconnect()` 方法存在于代码中 THEN 该方法从未被任何代码路径调用，形同死代码

1.5 WHEN `WebRtcSignaling::log_health_status()` 方法存在于代码中 THEN 该方法从未被周期性调用，无法在运行时观测 signaling 健康状态

1.6 WHEN signaling 断连后 `send_answer()` 或 `send_ice_candidate()` 失败 THEN 日志仅打印 "not connected"，不包含当前 signaling 状态码、断连持续时长等诊断信息

1.7 WHEN signaling 经历断连-恢复周期 THEN 无日志区分首次连接与重连恢复，无累计断连/重连计数，无连接持续时长记录

### Expected Behavior (Correct)

2.1 WHEN `on_signaling_state_changed(DISCONNECTED)` 回调触发 THEN 系统 SHALL 设置 `connected = false` 并设置 atomic flag `needs_reconnect_` 通知重连监控线程，同时记录断连时间戳

2.2 WHEN 重连监控线程检测到 `needs_reconnect_` 标志 THEN 系统 SHALL 在退避等待后直接调用 `signalingClientConnectSync` 重新建立 WebSocket 连接（该 API 内部会等待 SDK 状态就绪），不需要额外轮询 SDK 状态是否已到 READY

2.3 WHEN 重连尝试失败 THEN 系统 SHALL 使用指数退避策略重试（初始 1s，倍增至 2s/4s/8s/16s，上限 30s），并记录每次重连尝试的次数和退避时间

2.4 WHEN 重连成功（signaling 状态变为 CONNECTED）且本次连接持续超过 30 秒（连接稳定） THEN 系统 SHALL 重置退避时间为初始值 1s，记录重连成功日志（包含总重连次数和恢复耗时）；若连接持续不足 30 秒又断开，退避计数器不重置，防止快速重连风暴

2.5 WHEN 系统处于 7×24 无人值守运行模式 THEN 系统 SHALL 无限重连（不设最大重连次数限制），退避到 30s 上限后保持 30s 间隔持续重试；累计重连次数仅用于日志告警，不作为停止重连的条件

2.6 WHEN 重连监控线程运行时 THEN 系统 SHALL NOT 在 SDK 回调线程中直接调用 `signalingClientConnectSync`（会导致死锁），必须在独立线程中执行

2.7 WHEN `WebRtcSignaling` 对象析构或 `disconnect()` 被显式调用（正常关闭） THEN 系统 SHALL 设置 `shutdown_requested_` 标志，安全停止重连监控线程（通过 RAII 管理），不产生悬挂线程或资源泄漏，且不触发自动重连

2.8 WHEN signaling 断连触发 DISCONNECTED 回调 THEN 系统 SHALL 在日志中打印断连原因（如果 SDK 提供）、本次连接持续时长、累计断连次数

2.9 WHEN signaling 状态变为 READY THEN 系统 SHALL 在日志中区分首次连接（initial connect）与断连恢复（reconnect recovery）

2.10 WHEN 重连尝试执行时 THEN 系统 SHALL 在日志中打印当前重连次数、退避等待时间

2.11 WHEN 重连成功或失败时 THEN 系统 SHALL 在日志中打印详细状态（成功：恢复耗时、总重连次数；失败：错误信息、已尝试次数）

2.12 WHEN signaling 处于运行状态 THEN 系统 SHALL 每 60 秒打印一次健康检查日志：正常连接中使用 debug 级别（避免日志刷屏），异常状态（DISCONNECTED、重连中、READY 但未 CONNECTED）使用 warn 级别；日志内容包含：当前状态、连接持续时长、累计断连次数、累计重连次数

2.13 WHEN `send_answer()` 或 `send_ice_candidate()` 因未连接而失败 THEN 系统 SHALL 在日志中打印当前 signaling 状态码（而非仅 "not connected"）

2.14 WHEN signaling 断连后重连成功 THEN 系统 SHALL NOT 重建已有的 peer connection（WebRTC 数据通道是 P2P 的，不经过 signaling），已连接的 viewer 继续正常工作，仅新 viewer 的连接能力恢复

2.15 WHEN 重连线程调用 `signalingClientConnectSync` 成功 THEN 系统 SHALL 等待 SDK 回调 `on_signaling_state_changed(CONNECTED)` 设置 `connected = true`，重连线程不直接修改 `connected` 标志

### Unchanged Behavior (Regression Prevention)

3.1 WHEN macOS 开发环境编译（无 `HAVE_KVS_WEBRTC_SDK` 宏） THEN 系统 SHALL CONTINUE TO 使用 stub 实现，stub 需模拟重连行为（模拟断连/恢复状态转换），所有现有测试通过

3.2 WHEN signaling 首次连接成功且未发生断连 THEN 系统 SHALL CONTINUE TO 正常接收 viewer 的 SDP offer 和 ICE candidate，正常发送 SDP answer

3.3 WHEN `WebRtcSignaling::connect()` 被调用 THEN 系统 SHALL CONTINUE TO 执行完整的 create_and_connect 流程（创建 signaling client → fetch → connect）

3.4 WHEN `WebRtcSignaling::disconnect()` 被显式调用（正常关闭） THEN 系统 SHALL CONTINUE TO 释放 signaling client 资源，设置 `shutdown_requested_` 标志，重连监控线程检测到该标志后停止重连

3.5 WHEN `WebRtcSignaling::set_offer_callback()` 和 `set_ice_candidate_callback()` 注册回调 THEN 系统 SHALL CONTINUE TO 在收到 signaling message 时正确分发到注册的回调

3.6 WHEN `get_ice_config_count()` 和 `get_ice_config()` 被调用 THEN 系统 SHALL CONTINUE TO 返回正确的 ICE server 配置信息

3.7 WHEN 所有日志输出 THEN 系统 SHALL CONTINUE TO 使用 "webrtc" logger，不引入新的 logger 名称
