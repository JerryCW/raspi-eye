# Bugfix Requirements Document

## Introduction

Pi 5 上 WebRTC 连接失败，`setRemoteDescription()` 返回错误码 `0x40100001`。AWS Console Viewer 发送 SDP offer 后，设备端 `webrtc_media.cpp` 中的 `on_viewer_offer()` 无法处理 offer，导致 PeerConnection 无法建立，viewer 看不到视频流。

通过对比已验证能工作的参考实现（raspi-ipc-sample/webrtc_agent.cpp），定位到 5 个关键差异，其中缺少 `addSupportedCodec` 调用是最可能的根因。

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN viewer 发送 SDP offer 且设备端未调用 `addSupportedCodec` 注册支持的编解码器 THEN `setRemoteDescription()` 返回错误码 `0x40100001`，PeerConnection 建立失败

1.2 WHEN 设备端 `RtcConfiguration` 全零初始化且未配置任何 ICE 服务器（STUN/TURN） THEN ICE 候选收集无法穿越 NAT，即使 SDP 协商成功也无法建立媒体通道

1.3 WHEN 设备端收到 viewer 的 ICE candidate JSON 后直接 `STRNCPY` 到 `candidate` 字段而非使用 `deserializeRtcIceCandidateInit()` THEN SDK 无法正确解析 ICE candidate 的 JSON 格式，导致 `addIceCandidate` 失败或行为异常

1.4 WHEN 设备端 SDP answer 流程使用 `setRemoteDescription → createAnswer → setLocalDescription` 顺序而非 SDK 期望的 `setRemoteDescription → setLocalDescription → createAnswer` 顺序 THEN SDK 内部状态机可能不一致，导致 answer 生成或发送异常

1.5 WHEN 设备端视频 transceiver 未设置 direction（NULL init）且音频 transceiver 使用 `RECVONLY` THEN SDP 协商时 m= 行的 direction 属性与 viewer 期望不匹配，可能导致媒体流方向协商失败

1.6 WHEN 设备端 `send_answer` 和 `send_ice_candidate` 中 `payloadLen` 使用 `std::string::size()` THEN payload 可能包含 null terminator，导致 viewer 端解析 SDP/ICE 异常，ICE 协商失败 `0x5a00000d`

1.7 WHEN viewer 的 ICE candidate 在 SDP offer 之前到达（Trickle ICE 时序），设备端 PeerConnection 尚未创建 THEN `on_viewer_ice_candidate` 丢弃这些 candidate（"ICE candidate for unknown peer"），丢失可能包含 host candidate 的关键连接路径

1.8 WHEN 设备端 `h264parse` 输出 AVC 格式（length-prefixed NALU）给 WebRTC SDK 的 `writeFrame` THEN SDK 报 `STATUS_RTP_INVALID_NALU (0x5c000003)`，因为 SDK 期望 Annex B 格式（byte-stream，`00 00 00 01` start codes）

### Expected Behavior (Correct)

2.1 WHEN viewer 发送 SDP offer THEN 系统 SHALL 在 `addTransceiver` 之前调用 `addSupportedCodec` 注册 H.264（`RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE`）和 OPUS（`RTC_CODEC_OPUS`）编解码器，使 `setRemoteDescription` 能正确匹配 offer 中的 m= 行并返回 `STATUS_SUCCESS`

2.2 WHEN 创建 PeerConnection THEN 系统 SHALL 从 signaling client 获取 ICE 服务器配置（STUN + TURN），填充到 `RtcConfiguration.iceServers` 中，使 ICE 候选收集能穿越 NAT

2.3 WHEN 收到 viewer 的 ICE candidate JSON THEN 系统 SHALL 使用 `deserializeRtcIceCandidateInit()` SDK 函数反序列化 ICE candidate，而非直接 `STRNCPY`

2.4 WHEN 处理 SDP offer 并生成 answer THEN 系统 SHALL 按照 KVS WebRTC C SDK 期望的顺序执行：`setRemoteDescription → setLocalDescription（空 answer）→ createAnswer → serializeSessionDescriptionInit → send`

2.5 WHEN 添加视频和音频 transceiver THEN 系统 SHALL 将视频和音频的 direction 都设置为 `RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV`，与 viewer 端的期望一致

2.6 WHEN 通过 signaling 发送 SDP answer 或 ICE candidate THEN 系统 SHALL 使用 `(UINT32) STRLEN(msg.payload)` 设置 `payloadLen`，并设置 `msg.correlationId[0] = '\0'`，确保 payload 不含 null terminator

2.7 WHEN viewer 的 ICE candidate 在 SDP offer 之前到达 THEN 系统 SHALL 将这些 candidate 缓存到 `pending_candidates` 队列（最多 50 条），在 PeerConnection 创建完成后立即 flush 并调用 `addIceCandidate`

2.8 WHEN 向 WebRTC SDK 的 `writeFrame` 发送 H.264 帧数据 THEN 系统 SHALL 确保帧数据为 Annex B 格式（byte-stream，`00 00 00 01` start codes），通过在 `h264parse` 后加 capsfilter `stream-format=byte-stream,alignment=au` 实现；kvssink 分支单独加 `h264parse → stream-format=avc` 做格式转换

### Unchanged Behavior (Regression Prevention)

3.1 WHEN macOS 开发环境编译（无 `HAVE_KVS_WEBRTC_SDK` 宏） THEN 系统 SHALL CONTINUE TO 使用 stub 实现，所有 11 个现有测试通过

3.2 WHEN 多个 viewer 同时连接（≤ 10 个） THEN 系统 SHALL CONTINUE TO 正确管理 peer 生命周期，包括创建、替换、移除和最大连接数限制

3.3 WHEN peer 连接状态变为 FAILED 或 CLOSED THEN 系统 SHALL CONTINUE TO 自动清理 PeerConnection 资源和 callback context，防止内存泄漏

3.4 WHEN 调用 `broadcast_frame` 发送 H.264 帧 THEN 系统 SHALL CONTINUE TO 通过 `writeFrame` 向所有已连接 peer 的 video transceiver 发送帧数据，超过失败阈值时自动移除 peer

3.5 WHEN signaling client 发送 SDP answer 和 ICE candidate THEN 系统 SHALL CONTINUE TO 通过 `WebRtcSignaling::send_answer` 和 `send_ice_candidate` 正确序列化并发送消息
