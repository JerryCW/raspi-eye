# Bugfix Design: spec-14-webrtc-sdp-fix

## 概述

修复 `webrtc_media.cpp` 中 `on_viewer_offer()` 的 5 个问题，使 WebRTC PeerConnection 能正确建立。
所有修改仅在 `#ifdef HAVE_KVS_WEBRTC_SDK` 分支内，stub 分支不动。

## 修改范围

| 文件 | 修改内容 |
|------|---------|
| `device/src/webrtc_signaling.h` | 新增 `get_ice_config_count()` 和 `get_ice_config_info()` 接口 |
| `device/src/webrtc_signaling.cpp` | 实现上述两个接口（SDK 分支调 SDK API，stub 返回 0/false） |
| `device/src/webrtc_media.cpp` | 重写 `on_viewer_offer()` 和 `on_viewer_ice_candidate()` 的真实实现 |

## 1. WebRtcSignaling 新增 ICE 配置接口

media 模块需要从 signaling client 获取 TURN 服务器信息来配置 `RtcConfiguration`。
当前 signaling handle 封装在 pImpl 内部，不暴露。新增两个 public 方法：

```cpp
// webrtc_signaling.h — 新增接口
class WebRtcSignaling {
public:
    // ... 现有接口 ...

    // 获取 ICE 配置数量（TURN 服务器数量）
    // stub 返回 0
    uint32_t get_ice_config_count() const;

    // 获取第 i 个 ICE 配置的 URI/credential/username
    // 通过输出参数返回，成功返回 true
    // stub 返回 false
    struct IceServerInfo {
        std::string uri;
        std::string username;
        std::string credential;
    };
    bool get_ice_config(uint32_t index,
                        std::vector<IceServerInfo>& servers) const;
};
```

### 实现（SDK 分支）

```cpp
uint32_t WebRtcSignaling::get_ice_config_count() const {
    UINT32 count = 0;
    signalingClientGetIceConfigInfoCount(impl_->signaling_handle, &count);
    return count;
}

bool WebRtcSignaling::get_ice_config(uint32_t index,
                                     std::vector<IceServerInfo>& servers) const {
    PIceConfigInfo info = nullptr;
    STATUS ret = signalingClientGetIceConfigInfo(
        impl_->signaling_handle, index, &info);
    if (STATUS_FAILED(ret) || info == nullptr) return false;
    servers.clear();
    for (UINT32 j = 0; j < info->uriCount; j++) {
        servers.push_back({
            std::string(info->uris[j]),
            std::string(info->userName),
            std::string(info->password)
        });
    }
    return true;
}
```

### 实现（stub 分支）

```cpp
uint32_t WebRtcSignaling::get_ice_config_count() const { return 0; }
bool WebRtcSignaling::get_ice_config(uint32_t, std::vector<IceServerInfo>&) const { return false; }
```

## 2. on_viewer_offer 重写（5 个修复点）

按照参考代码（raspi-ipc-sample/webrtc_agent.cpp `handle_sdp_offer`）的已验证流程重写。

### 修复后的完整步骤

```
步骤 1: peer_id 长度检查 + 旧 peer 清理 + 最大连接数检查（不变）
步骤 2: 配置 RtcConfiguration（含 ICE 服务器）          ← 修复点 2.2
步骤 3: createPeerConnection
步骤 4: addSupportedCodec (H.264 + OPUS)               ← 修复点 2.1
步骤 5: addTransceiver (video SENDRECV + audio SENDRECV) ← 修复点 2.5
步骤 6: 注册回调 (ICE candidate + connection state)
步骤 7: deserializeSessionDescriptionInit + setRemoteDescription
步骤 8: setLocalDescription（空 answer_sdp）             ← 修复点 2.4
步骤 9: createAnswer                                     ← 修复点 2.4
步骤 10: serializeSessionDescriptionInit + send_answer
步骤 11: 存入 peers map
```

### 关键参考代码

#### 步骤 2: ICE 服务器配置

```cpp
RtcConfiguration rtc_config;
MEMSET(&rtc_config, 0x00, SIZEOF(RtcConfiguration));
rtc_config.iceTransportPolicy = ICE_TRANSPORT_POLICY_ALL;

// STUN 服务器（slot 0）
SNPRINTF(rtc_config.iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN,
         "stun:stun.kinesisvideo.%s.amazonaws.com:443",
         region.c_str());

// TURN 服务器（slot 1+）— 从 signaling client 获取
uint32_t ice_count = signaling.get_ice_config_count();
uint32_t uri_idx = 0;
uint32_t max_turn = (ice_count > 0) ? 1 : 0;  // 只用 1 个 TURN 服务器
for (uint32_t i = 0; i < max_turn; i++) {
    std::vector<WebRtcSignaling::IceServerInfo> servers;
    if (signaling.get_ice_config(i, servers)) {
        for (const auto& s : servers) {
            if (uri_idx + 1 < MAX_ICE_SERVERS_COUNT) {
                STRNCPY(rtc_config.iceServers[uri_idx + 1].urls,
                         s.uri.c_str(), MAX_ICE_CONFIG_URI_LEN);
                STRNCPY(rtc_config.iceServers[uri_idx + 1].credential,
                         s.credential.c_str(), MAX_ICE_CONFIG_CREDENTIAL_LEN);
                STRNCPY(rtc_config.iceServers[uri_idx + 1].username,
                         s.username.c_str(), MAX_ICE_CONFIG_USER_NAME_LEN);
                uri_idx++;
            }
        }
    }
}
```

#### 步骤 4: addSupportedCodec

```cpp
ret = addSupportedCodec(peer_connection,
    RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE);
if (STATUS_FAILED(ret)) { /* fatal: cleanup + return false */ }

addSupportedCodec(peer_connection, RTC_CODEC_OPUS);  // non-fatal
```

#### 步骤 5: transceiver direction 改为 SENDRECV

```cpp
// 视频
RtcRtpTransceiverInit video_init;
MEMSET(&video_init, 0, SIZEOF(RtcRtpTransceiverInit));
video_init.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
ret = addTransceiver(peer_connection, &video_track, &video_init, &video_transceiver);

// 音频
RtcRtpTransceiverInit audio_init;
MEMSET(&audio_init, 0, SIZEOF(RtcRtpTransceiverInit));
audio_init.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
ret = addTransceiver(peer_connection, &audio_track, &audio_init, &audio_transceiver);
```

#### 步骤 8-9: SDP answer 流程（SDK 顺序）

```cpp
// setRemoteDescription 已在步骤 7 完成

// 步骤 8: setLocalDescription（空 answer_sdp）
RtcSessionDescriptionInit answer_sdp;
MEMSET(&answer_sdp, 0x00, SIZEOF(RtcSessionDescriptionInit));
ret = setLocalDescription(peer_connection, &answer_sdp);
if (STATUS_FAILED(ret)) { /* cleanup + return false */ }

// 步骤 9: createAnswer
ret = createAnswer(peer_connection, &answer_sdp);
if (STATUS_FAILED(ret)) { /* cleanup + return false */ }

// 步骤 10: serialize + send
UINT32 serialized_len = MAX_SIGNALING_MESSAGE_LEN;
CHAR serialized_answer[MAX_SIGNALING_MESSAGE_LEN];
ret = serializeSessionDescriptionInit(&answer_sdp, serialized_answer, &serialized_len);
```

## 3. on_viewer_ice_candidate 修复

使用 `deserializeRtcIceCandidateInit()` 替代直接 STRNCPY：

```cpp
bool WebRtcMediaManager::on_viewer_ice_candidate(
    const std::string& peer_id, const std::string& candidate,
    std::string* error_msg) {
    std::lock_guard<std::mutex> lock(impl_->peers_mutex);
    auto it = impl_->peers.find(peer_id);
    if (it == impl_->peers.end()) {
        // ... unknown peer warning ...
        return false;
    }

    RtcIceCandidateInit candidate_init;
    MEMSET(&candidate_init, 0, SIZEOF(RtcIceCandidateInit));
    STATUS ret = deserializeRtcIceCandidateInit(
        const_cast<PCHAR>(candidate.c_str()),
        static_cast<UINT32>(candidate.size()),
        &candidate_init);
    if (STATUS_FAILED(ret)) {
        // ... deserialize failed warning ...
        return false;
    }

    ret = addIceCandidate(it->second.peer_connection, candidate_init.candidate);
    // ... error handling ...
}
```

## 4. Impl 需要持有 region 信息

`on_viewer_offer` 需要 region 来构造 STUN URL。当前 Impl 只持有 `WebRtcSignaling&`。
新增 `std::string region` 成员，在 `create()` 工厂方法中传入。

### WebRtcMediaManager::create 签名变更

```cpp
// webrtc_media.h
static std::unique_ptr<WebRtcMediaManager> create(
    WebRtcSignaling& signaling,
    const std::string& aws_region,    // 新增
    std::string* error_msg = nullptr);
```

调用方（app_context.cpp）需要同步传入 `webrtc_config.aws_region`。

## SHALL NOT

- SHALL NOT 修改 stub 分支的逻辑（`#ifndef HAVE_KVS_WEBRTC_SDK`）
- SHALL NOT 修改现有测试文件（stub 测试不受影响）
- SHALL NOT 在不确定 SDK API 签名时凭猜测编写代码 — 参考代码已验证

## 明确不包含

- signaling 重连逻辑优化（后续 spec）
- credential 刷新集成（后续 spec）
- 性能优化（broadcast_frame 异步化等，后续 spec）
