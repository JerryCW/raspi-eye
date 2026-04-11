# 需求文档：Spec 13 — WebRTC 媒体流传输

## 简介

本 Spec 在 Spec 12（信令层）基础上实现 WebRTC 媒体流传输，核心工作：
1. 创建 WebRtcMediaManager 模块，管理 PeerConnection 生命周期（每个 Viewer 一个 PeerConnection）
2. 替换 pipeline_builder 中 webrtc 分支的 fakesink，通过 appsink 获取 H.264 帧
3. 将 H.264 帧通过 KVS WebRTC C SDK 的 `writeFrame()` 写入各 PeerConnection

数据流路径：
```
encoded-tee → queue(leaky) → appsink("webrtc-sink")
                                  ↓ (GstBuffer → H.264 frame)
                            WebRtcMediaManager
                                  ↓ (writeFrame per PeerConnection)
                            Viewer 1 ... Viewer N (最多 10)
```

Spec 12 提供信令层（SDP/ICE 交换），本 Spec 在收到 Viewer 的 SDP Offer 后创建 PeerConnection、生成 SDP Answer、交换 ICE Candidate，并持续向已连接的 PeerConnection 写入媒体帧。

平台策略（与 Spec 12 一致）：
- **Pi 5（Linux aarch64）**：使用 KVS WebRTC C SDK 创建真实 PeerConnection，通过 `writeFrame()` 发送 H.264 帧
- **macOS（开发环境）**：stub 实现，appsink 正常接收帧但不创建真实 PeerConnection，验证管道集成和帧分发逻辑

## 前置条件

- Spec 12（webrtc-signaling）✅ — 信令通道连接、SDP/ICE 交换、WebRtcSignaling 类
- Spec 3（h264-tee-pipeline）✅ — 双 tee 管道，webrtc 分支输出 H.264 编码流
- Spec 5（pipeline-health）✅ — 管道健康监控

## 术语表

- **WebRtcMediaManager**：本 Spec 新增的媒体管理模块，管理多个 PeerConnection 的生命周期和帧分发
- **PeerConnection**：WebRTC 点对点连接，每个 Viewer 一个，由 KVS WebRTC C SDK 的 `createPeerConnection()` 创建
- **Transceiver**：媒体轨道收发器，通过 `addTransceiver()` 添加到 PeerConnection，本 Spec 只添加 video transceiver（H.264）
- **appsink**：GStreamer 元素，允许应用程序从管道中拉取 buffer，替换 webrtc 分支的 fakesink
- **writeFrame()**：KVS WebRTC C SDK 函数，向 PeerConnection 的 video transceiver 写入一帧媒体数据
- **Frame**：KVS WebRTC C SDK 的帧结构体，包含帧数据指针、大小、时间戳、帧类型（关键帧/非关键帧）等
- **RtcMediaStreamTrack**：KVS WebRTC C SDK 的媒体轨道结构体，描述编解码器类型（H.264）和轨道 ID
- **ICE Server Config**：STUN/TURN 服务器配置，从 KVS 信令服务获取，用于 NAT 穿透
- **WebRtcSignaling**：Spec 12 交付的信令客户端，本 Spec 通过其回调接收 SDP Offer 和 ICE Candidate
- **HAVE_KVS_WEBRTC_SDK**：条件编译宏，定义时编译 KVS WebRTC C SDK 真实实现，未定义时编译 stub

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| 最大并发 PeerConnection | 10（KVS WebRTC 服务限制，每个 Viewer 一个） |
| 帧分发延迟 | appsink 收到 buffer 到 writeFrame 调用 ≤ 5ms（不含网络传输） |
| 单个测试耗时 | 管道启停 ≤ 5 秒，PBT ≤ 15 秒 |
| 代码组织 | .h + .cpp 分离模式 |
| 资源管理 | RAII，PeerConnection 在析构或 Viewer 断开时释放 |
| 日志语言 | 英文，不使用非 ASCII 字符 |
| 条件编译宏 | `HAVE_KVS_WEBRTC_SDK`（与 Spec 12 一致） |
| 新增代码量 | 200-500 行 |
| 涉及文件 | 3-5 个（webrtc_media.h/cpp 新增，pipeline_builder.h/cpp 修改，CMakeLists.txt 修改） |
| 向后兼容 | 不修改现有测试文件；pipeline_builder 在不传入 WebRtcMediaManager 时保持 fakesink 行为 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现自适应码率控制（属于 Spec 14: adaptive-streaming）
- SHALL NOT 在本 Spec 中实现 DataChannel 功能
- SHALL NOT 在本 Spec 中实现 Viewer 端（属于 Spec 20: viewer-mvp）
- SHALL NOT 在本 Spec 中自建 TURN 服务器（使用 KVS 提供的 STUN/TURN 服务）
- SHALL NOT 在本 Spec 中实现音频轨道（仅 video H.264）

### Design 层

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret（来源：安全基线）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）
- SHALL NOT 在 macOS 上尝试链接 KVS WebRTC C SDK（macOS 使用 stub 实现）
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、kvs_test、webrtc_test、yolo_test）
- SHALL NOT 在不确定 KVS WebRTC C SDK API 用法时凭猜测编写代码（来源：shall-not.md）
- SHALL NOT 在 appsink 的 new-sample 回调中执行同步磁盘 I/O 或阻塞网络请求（高性能路径）
- SHALL NOT 在帧分发路径中分配堆内存（writeFrame 使用 GstBuffer 的 map 数据指针，零拷贝）

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 KVS WebRTC C SDK C API 外）
- SHALL NOT 在测试中用假凭证数据调用可能创建真实 PeerConnection 的函数（来源：spec-8 经验）

## 需求

### 需求 1：WebRtcMediaManager 模块创建

**用户故事：** 作为设备端程序，我需要一个媒体管理模块来管理多个 PeerConnection 的生命周期，以便同时向多个 Viewer 发送 H.264 视频流。

#### 验收标准

1. THE WebRtcMediaManager 模块 SHALL 定义 `WebRtcMediaManager` 类，提供以下接口：`on_viewer_offer(peer_id, sdp_offer)` 处理 Viewer 的 SDP Offer、`on_viewer_ice_candidate(peer_id, candidate)` 处理 Viewer 的 ICE Candidate、`remove_peer(peer_id)` 移除指定 Viewer 的 PeerConnection、`broadcast_frame(data, size, timestamp, is_keyframe)` 向所有已连接的 PeerConnection 广播一帧 H.264 数据、`peer_count()` 返回当前活跃 PeerConnection 数量
2. THE WebRtcMediaManager 类 SHALL 通过工厂方法 `create()` 构造，接受 `WebRtcSignaling&` 引用（用于发送 SDP Answer 和 ICE Candidate）和信令通道的 ICE 服务器配置
3. THE WebRtcMediaManager 类 SHALL 遵循 RAII 语义，析构时释放所有 PeerConnection 资源
4. THE WebRtcMediaManager 类 SHALL 禁用拷贝构造和拷贝赋值（`= delete`）
5. THE WebRtcMediaManager 类 SHALL 通过条件编译（`#ifdef HAVE_KVS_WEBRTC_SDK`）隔离平台特定代码，macOS 使用 stub 实现

### 需求 2：PeerConnection 生命周期管理

**用户故事：** 作为设备端程序，我需要为每个 Viewer 创建独立的 PeerConnection，以便隔离各 Viewer 的连接状态和媒体传输。

#### 验收标准

1. WHEN 收到 Viewer 的 SDP Offer 时，THE WebRtcMediaManager SHALL 创建一个新的 PeerConnection（通过 SDK 的 `createPeerConnection()`），并将其与 peer_id 关联存储
2. WHEN 创建 PeerConnection 时，THE WebRtcMediaManager SHALL 添加一个 video transceiver（通过 SDK 的 `addTransceiver()`），编解码器类型为 H.264
3. WHEN 创建 PeerConnection 时，THE WebRtcMediaManager SHALL 设置远端 SDP（`setRemoteDescription`，Viewer 的 Offer），然后创建本地 SDP Answer（`createAnswer`），设置本地 SDP（`setLocalDescription`），并通过 WebRtcSignaling 发送 Answer 给 Viewer
4. WHEN 创建 PeerConnection 时，THE WebRtcMediaManager SHALL 注册 ICE Candidate 生成回调（`onIceCandidate`），将本地生成的 ICE Candidate 通过 WebRtcSignaling 发送给对应 Viewer
5. WHEN 收到 Viewer 的 ICE Candidate 时，THE WebRtcMediaManager SHALL 调用对应 PeerConnection 的 `addIceCandidate()` 添加远端 ICE Candidate
6. IF 当前 PeerConnection 数量已达到 10 个上限，THEN THE WebRtcMediaManager SHALL 拒绝新的 Viewer 连接，通过 spdlog 记录 warning 并丢弃该 SDP Offer
7. IF 同一 peer_id 的 PeerConnection 已存在，THEN THE WebRtcMediaManager SHALL 先释放旧的 PeerConnection 再创建新的（处理 Viewer 重连场景）
8. THE WebRtcMediaManager SHALL 通过 spdlog 记录 PeerConnection 的创建和释放事件，包含 peer_id 和当前活跃连接数
9. IF peer_id 长度超过 256 字节，THEN THE WebRtcMediaManager SHALL 拒绝该连接，通过 spdlog 记录 warning 并返回 false（防止恶意超长 peer_id 消耗内存）

### 需求 3：H.264 帧分发

**用户故事：** 作为设备端程序，我需要将 encoded-tee 输出的 H.264 帧广播到所有已连接的 PeerConnection，以便 Viewer 能实时观看视频。

#### 验收标准

1. WHEN 调用 `broadcast_frame()` 时，THE WebRtcMediaManager SHALL 遍历所有活跃的 PeerConnection，对每个调用 SDK 的 `writeFrame()` 写入帧数据
2. THE `broadcast_frame()` 方法 SHALL 接受以下参数：帧数据指针（`const uint8_t*`）、帧大小（`size_t`）、时间戳（`uint64_t`，单位 100ns，与 KVS SDK 的 `HUNDREDS_OF_NANOS` 一致）、是否为关键帧（`bool`）
3. IF 某个 PeerConnection 的 `writeFrame()` 失败，THEN THE WebRtcMediaManager SHALL 通过 spdlog 记录 warning（包含 peer_id 和错误码），继续向其他 PeerConnection 写入，不中断广播
4. THE WebRtcMediaManager SHALL NOT 在 `broadcast_frame()` 中拷贝帧数据，直接使用调用方传入的数据指针构造 SDK Frame 结构体
5. THE WebRtcMediaManager SHALL 根据 `is_keyframe` 参数设置 Frame 的 flags 字段（`FRAME_FLAG_KEY_FRAME` 或 `FRAME_FLAG_NONE`）
6. IF 某个 PeerConnection 的 `writeFrame()` 连续失败超过 100 次，THEN THE WebRtcMediaManager SHALL 自动调用 `remove_peer()` 清理该 PeerConnection（自愈：防止僵尸连接持续产生 warning 日志和浪费 CPU）

### 需求 4：管道集成（替换 webrtc 分支 fakesink）

**用户故事：** 作为设备端程序，我需要将 pipeline_builder 中 webrtc 分支的 fakesink 替换为 appsink，以便从管道中获取 H.264 帧并分发给 WebRTC Viewer。

#### 验收标准

1. WHEN WebRtcMediaManager 指针传入 `build_tee_pipeline()` 时，THE PipelineBuilder SHALL 将 webrtc 分支的 fakesink 替换为 appsink（元素名称保持 "webrtc-sink"）
2. WHEN WebRtcMediaManager 指针为 nullptr 时，THE PipelineBuilder SHALL 保持 fakesink 行为（向后兼容）
3. THE appsink SHALL 配置以下属性：`emit-signals=true`（启用 new-sample 信号）、`drop=true`（队列满时丢弃旧帧）、`max-buffers=1`（最多缓存 1 帧，配合上游 leaky queue 保持低延迟）、`sync=false`（不与时钟同步，实时流无需同步）
4. THE appsink 的 `new-sample` 信号回调 SHALL 从 GstBuffer 中提取 H.264 帧数据（通过 `gst_buffer_map`），调用 WebRtcMediaManager 的 `broadcast_frame()`，然后 `gst_buffer_unmap` 释放映射
5. THE appsink 回调 SHALL 从 GstBuffer 的 flags 中检测关键帧（`GST_BUFFER_FLAG_DELTA_UNIT` 未设置表示关键帧）
6. THE appsink 回调 SHALL 将 GstBuffer 的 PTS（`GST_BUFFER_PTS`）转换为 KVS SDK 的时间戳单位（100ns），传入 `broadcast_frame()`

### 需求 5：PeerConnection 清理

**用户故事：** 作为设备端程序，我需要在 Viewer 断开或连接异常时清理对应的 PeerConnection 资源，以便释放系统资源并允许新 Viewer 连接。

#### 验收标准

1. WHEN 调用 `remove_peer(peer_id)` 时，THE WebRtcMediaManager SHALL 释放对应的 PeerConnection 资源（通过 SDK 的 `freePeerConnection()`）并从内部映射中移除
2. WHEN PeerConnection 的连接状态变为 FAILED 或 CLOSED 时，THE WebRtcMediaManager SHALL 通过 spdlog 记录事件并自动调用 `remove_peer()` 清理资源
3. WHEN WebRtcMediaManager 析构时，THE WebRtcMediaManager SHALL 释放所有剩余的 PeerConnection 资源
4. IF `remove_peer()` 传入不存在的 peer_id，THEN THE WebRtcMediaManager SHALL 忽略该调用（不报错，幂等操作）
5. THE WebRtcMediaManager SHALL 通过 spdlog 记录 PeerConnection 清理事件，包含 peer_id 和清理原因（手动移除 / 连接失败 / 析构清理）

### 需求 6：macOS 开发环境测试

**用户故事：** 作为开发者，我需要在 macOS 上验证 WebRTC 媒体集成的代码逻辑（appsink 集成、帧分发、PeerConnection 管理），无需真实 KVS 环境。

#### 验收标准

1. THE Test_Suite SHALL 验证：WebRtcMediaManager stub 创建成功，peer_count() 初始为 0
2. THE Test_Suite SHALL 验证：stub 的 on_viewer_offer() 增加 peer_count()，remove_peer() 减少 peer_count()
3. THE Test_Suite SHALL 验证：stub 的 broadcast_frame() 在无 PeerConnection 时正常返回（不崩溃）
4. THE Test_Suite SHALL 验证：stub 的 on_viewer_offer() 在达到 10 个 PeerConnection 上限后拒绝新连接
5. THE Test_Suite SHALL 验证：同一 peer_id 重复调用 on_viewer_offer() 不增加 peer_count()（旧连接被替换）
6. THE Test_Suite SHALL 验证：remove_peer() 传入不存在的 peer_id 不崩溃（幂等）
7. THE Test_Suite SHALL 验证：appsink 替换 fakesink 后管道能正常启动并接收 buffer（管道冒烟测试）
8. WHEN 单个测试执行时，THE Test_Suite SHALL 在 5 秒内完成（管道启停测试），PBT 在 15 秒内完成
9. THE Test_Suite SHALL 在 macOS Debug（ASan）构建下通过，无内存错误报告
10. THE Test_Suite SHALL 使用 RapidCheck 对 PeerConnection 管理进行 property-based testing：随机生成 peer_id 序列（add/remove 操作），验证 peer_count() 始终等于当前活跃 peer 集合的大小，且 ≤ 10

### 需求 7：双平台构建验证

**用户故事：** 作为开发者，我需要确保 WebRTC 媒体集成在 macOS 和 Pi 5 上均能成功编译和测试。

#### 验收标准

1. WHEN 在 macOS 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误
2. WHEN 在 Pi 5 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误
3. WHEN 在 macOS 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试（包括现有测试和新增的 webrtc_media_test），ASan 不报告任何内存错误
4. WHEN 在 Pi 5 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试

## 参考代码

### KVS WebRTC C SDK PeerConnection 创建（来源：SDK samples/kvsWebRTCClientMaster.c）

```c
// RTC 配置（包含 ICE 服务器）
RtcConfiguration rtcConfiguration;
MEMSET(&rtcConfiguration, 0, SIZEOF(RtcConfiguration));
// ICE 服务器从信令服务获取，通过 signalingClientGetIceConfigInfoCount/signalingClientGetIceConfigInfo

// 创建 PeerConnection
PRtcPeerConnection pPeerConnection = NULL;
STATUS retStatus = createPeerConnection(&rtcConfiguration, &pPeerConnection);

// 添加 video transceiver
RtcMediaStreamTrack videoTrack;
MEMSET(&videoTrack, 0, SIZEOF(RtcMediaStreamTrack));
videoTrack.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
videoTrack.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
STRCPY(videoTrack.streamId, "myKvsVideoStream");
STRCPY(videoTrack.trackId, "myVideoTrack");

PRtcRtpTransceiver pVideoTransceiver = NULL;
retStatus = addTransceiver(pPeerConnection, &videoTrack, NULL, &pVideoTransceiver);

// 设置远端 SDP（Viewer 的 Offer）
RtcSessionDescriptionInit offerSdp;
MEMSET(&offerSdp, 0, SIZEOF(RtcSessionDescriptionInit));
offerSdp.type = SDP_TYPE_OFFER;
STRCPY(offerSdp.sdp, viewerSdpOffer);
retStatus = setRemoteDescription(pPeerConnection, &offerSdp);

// 创建 Answer
RtcSessionDescriptionInit answerSdp;
MEMSET(&answerSdp, 0, SIZEOF(RtcSessionDescriptionInit));
retStatus = createAnswer(pPeerConnection, &answerSdp);

// 设置本地 SDP
retStatus = setLocalDescription(pPeerConnection, &answerSdp);

// 发送 Answer 通过信令通道（调用 WebRtcSignaling::send_answer）
```

### writeFrame 帧写入（来源：SDK samples）

```c
Frame frame;
MEMSET(&frame, 0, SIZEOF(Frame));
frame.version = FRAME_CURRENT_VERSION;
frame.trackId = DEFAULT_VIDEO_TRACK_ID;  // video transceiver 的 track ID
frame.duration = 0;
frame.decodingTs = timestamp;  // 单位：HUNDREDS_OF_NANOS (100ns)
frame.presentationTs = timestamp;
frame.frameData = pFrameData;  // H.264 帧数据指针
frame.size = frameSize;
frame.flags = isKeyFrame ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;

STATUS retStatus = writeFrame(pVideoTransceiver, &frame);
```

### appsink 集成模式（GStreamer C API）

```c
// 创建 appsink 替代 fakesink
GstElement* appsink = gst_element_factory_make("appsink", "webrtc-sink");
g_object_set(G_OBJECT(appsink),
    "emit-signals", TRUE,
    "drop", TRUE,
    "max-buffers", 1,
    "sync", FALSE,
    nullptr);

// 连接 new-sample 信号
g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), user_data);

// new-sample 回调
static GstFlowReturn on_new_sample(GstElement* sink, gpointer user_data) {
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        uint64_t pts = GST_BUFFER_PTS(buffer);
        bool is_keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        // 转换 PTS: GStreamer ns → KVS 100ns
        uint64_t timestamp_100ns = pts / 100;

        // 调用 broadcast_frame
        manager->broadcast_frame(map.data, map.size, timestamp_100ns, is_keyframe);

        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
```

### ICE Candidate 回调注册（来源：SDK samples）

```c
// 注册本地 ICE Candidate 生成回调
retStatus = peerConnectionOnIceCandidate(pPeerConnection, (UINT64) customData, onIceCandidateHandler);

// 回调函数
VOID onIceCandidateHandler(UINT64 customData, PCHAR candidateJson) {
    if (candidateJson == NULL) {
        // ICE gathering 完成
        return;
    }
    // 通过信令通道发送给 Viewer
    // signaling->send_ice_candidate(peer_id, candidateJson);
}
```

### PeerConnection 连接状态回调（来源：SDK samples）

```c
// 注册连接状态变化回调
retStatus = peerConnectionOnConnectionStateChange(pPeerConnection, (UINT64) customData, onConnectionStateChange);

// 回调函数
VOID onConnectionStateChange(UINT64 customData, RTC_PEER_CONNECTION_STATE newState) {
    if (newState == RTC_PEER_CONNECTION_STATE_FAILED || newState == RTC_PEER_CONNECTION_STATE_CLOSED) {
        // 清理 PeerConnection
    }
}
```

## 验证命令

```bash
# macOS Debug 构建 + 测试（使用 stub，不需要 KVS WebRTC SDK）
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug
cmake --build device/build
ctest --test-dir device/build --output-on-failure

# Pi 5 Release 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release
cmake --build device/build
ctest --test-dir device/build --output-on-failure

# Pi 5 端到端验证（需要 KVS WebRTC C SDK + 真实 AWS 环境）
# 前置：
#   1. Spec 12 已通过端到端验证（信令通道连接正常）
#   2. 运行程序，在 AWS Console 的 KVS WebRTC 页面使用 Test Page 作为 Viewer 连接
#   3. 验证 Viewer 能看到视频流
```

## 明确不包含

- 自适应码率控制（Spec 14: adaptive-streaming）
- DataChannel 功能
- 音频轨道
- Viewer 端实现（Spec 20: viewer-mvp）
- TURN 服务器自建（使用 KVS 提供的 STUN/TURN 服务）
- 信令层修改（Spec 12 已完成，本 Spec 只消费其接口）
- 零拷贝缓冲区优化（Spec 15: zero-copy-buffers）
- PeerConnection 连接质量监控和统计（可在 Spec 14 中实现）
- WebRtcMediaManager 与 PipelineHealthMonitor 集成（Spec 14: adaptive-streaming 中实现流模式切换时需要 WebRTC 分支状态反馈）
- broadcast_frame 异步帧分发（当前同步调用 writeFrame，如 Pi 5 端到端验证发现阻塞问题，在 Spec 14 中改为异步队列）
