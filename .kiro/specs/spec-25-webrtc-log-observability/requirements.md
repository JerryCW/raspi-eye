# 需求文档：Spec 25 — WebRTC 日志可观测性优化

## 简介

从 Pi 5 生产日志审查中发现 WebRTC 模块（`webrtc_signaling.cpp`、`webrtc_media.cpp`）的日志可观测性不足，存在五个问题：

1. **ICE candidate 日志过于啰嗦**：收到/发送每个 ICE candidate 都打 info 级别，一次连接产生 20+ 条重复日志，淹没关键信息。
2. **缺少关键状态转换日志**：没有 ICE connection state 变化（checking → connected → completed）、DTLS 握手完成等里程碑日志。
3. **SDP offer/answer 缺少摘要信息**：只打了 peer_id，没有 codec、方向（sendrecv/sendonly）等关键 SDP 属性。
4. **连接期间完全静默**：peer connected 到 DISCONNECTING 之间没有任何日志，缺少 media flow 状态（首帧发送、writeFrame 统计等）。
5. **cleanup 日志信息不足**：cleanup thread 释放 peer 时只打了 peer_id，缺少 peer 存活时长、断开原因。

本 Spec 仅优化日志输出，不修改 WebRTC 功能逻辑。所有日志使用 `spdlog::get("webrtc")` logger，日志消息使用英文。

## 前置条件

- Spec 12 (webrtc-signaling) 已通过验证 ✅
- Spec 13 (webrtc-media) 已通过验证 ✅
- Spec 23 (log-management) 已通过验证 ✅

## 术语表

- **WebRTC_Logger**: 名为 `"webrtc"` 的 spdlog Component_Logger 实例，由 log_init 创建，WebRTC 模块的所有日志通过此 logger 输出
- **ICE_Candidate**: Interactive Connectivity Establishment 候选地址，WebRTC 连接建立过程中交换的网络地址信息
- **ICE_Connection_State**: ICE 连接状态机，包含 new → checking → connected → completed → failed → closed 等状态
- **SDP**: Session Description Protocol，描述媒体会话参数（codec、方向、分辨率等）的协议
- **SDP_Summary**: 从 SDP 文本中提取的关键属性摘要，包含 codec 列表和媒体方向
- **DTLS_Handshake**: Datagram Transport Layer Security 握手，WebRTC 媒体加密通道建立过程
- **Media_Flow_State**: 媒体数据流状态，包括首帧发送、writeFrame 成功/失败统计等
- **PeerState**: peer 连接状态枚举（CONNECTING → CONNECTED → DISCONNECTING），定义在 `webrtc_media.cpp`
- **Cleanup_Thread**: `WebRtcMediaManager::Impl` 中的后台线程，定期扫描并释放 DISCONNECTING 超过 grace period 的 peer
- **ICE_Candidate_Counter**: 每个 peer 的 ICE candidate 收发计数器，用于连接建立后输出汇总日志

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| 目标平台 | macOS（开发）/ Linux aarch64（Pi 5 生产） |
| 硬件限制 | Raspberry Pi 5，4GB RAM |
| 涉及文件 | `device/src/webrtc_signaling.cpp`、`device/src/webrtc_media.cpp` |
| Logger | `spdlog::get("webrtc")`，不创建新 logger |
| 日志输出语言 | 英文（禁止非 ASCII 字符） |
| 功能逻辑 | 不修改 WebRTC 连接、信令、媒体流的功能逻辑 |
| 第三方 SDK | 不修改 KVS WebRTC C SDK 内部日志 |
| 条件编译 | stub 和 real 实现均需更新日志（`#ifdef HAVE_KVS_WEBRTC_SDK`） |
| 高性能路径 | `broadcast_frame` 中的日志不得在每帧调用时输出（仅首帧、统计间隔、异常时输出） |
| Debug 构建 | 开启 ASan |

## 禁止项

### Requirements 层

- SHALL NOT 修改 WebRTC 连接建立、信令交换、媒体流传输的功能逻辑
- SHALL NOT 在 `broadcast_frame` 的每帧调用中输出 info 级别日志（高频路径，会严重影响性能）
- SHALL NOT 修改 KVS WebRTC C SDK 内部的日志输出

### Design 层

- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
  - 原因：日志可能被收集到云端或共享给他人
  - 建议：日志中只输出资源标识（如 peer_id、channel_name），不输出凭证内容
- SHALL NOT 在非 pipeline 模块中使用 `spdlog::get("pipeline")` logger
  - 原因：所有模块共用 pipeline logger 导致日志无法按模块过滤
  - 建议：WebRTC 模块统一使用 `spdlog::get("webrtc")` logger

### Tasks 层

- SHALL NOT 在日志消息中使用非 ASCII 字符
- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在 SDP 摘要日志中输出完整 SDP 文本（SDP 可能包含 IP 地址等信息，且文本过长）

## 需求

### 需求 1：ICE Candidate 日志降级与汇总

**用户故事：** 作为运维人员，我希望 ICE candidate 收发日志不再淹没关键信息，同时在连接建立后能看到一条汇总，以便快速了解 ICE 协商结果。

#### 验收标准

1. WHEN 收到来自 viewer 的 ICE candidate 时，THE WebRTC_Logger SHALL 使用 debug 级别记录单条 candidate 信息（包含 peer_id），替代当前的 info 级别
2. WHEN 发送本地 ICE candidate 到 viewer 时，THE WebRTC_Logger SHALL 使用 debug 级别记录单条 candidate 信息（包含 peer_id），替代当前的 info 级别
3. WHEN 缓存早到的 ICE candidate 时（peer 尚未创建），THE WebRTC_Logger SHALL 使用 debug 级别记录缓存操作（包含 peer_id 和当前缓存数量），替代当前的 info 级别
4. WHEN peer 连接状态变为 CONNECTED 时，THE WebRTC_Logger SHALL 使用 info 级别输出一条 ICE candidate 汇总日志，包含：peer_id、发送的 candidate 数量、接收的 candidate 数量
5. THE WebRtcMediaManager::Impl SHALL 为每个 peer 维护 ICE candidate 收发计数器（sent_candidates、received_candidates），在 PeerInfo 结构体中新增这两个字段

### 需求 2：ICE/DTLS 状态转换里程碑日志

**用户故事：** 作为开发者，我希望在日志中看到 ICE 连接状态的每次变化和 DTLS 握手完成事件，以便快速定位连接建立过程中的问题。

#### 验收标准

1. WHEN PeerConnection 的 ICE connection state 发生变化时，THE WebRTC_Logger SHALL 使用 info 级别记录状态转换，格式包含：peer_id、旧状态、新状态
2. WHEN PeerConnection 状态变为 CONNECTED 时，THE WebRTC_Logger SHALL 使用 info 级别记录连接建立耗时（从 on_viewer_offer 创建 PeerConnection 到 CONNECTED 的时间差）
3. WHEN PeerConnection 状态变为 FAILED 时，THE WebRTC_Logger SHALL 使用 warn 级别记录连接失败事件，包含：peer_id、断开原因（connection_failed）
4. WHEN PeerConnection 状态变为 CLOSED 时，THE WebRTC_Logger SHALL 使用 info 级别记录正常断开事件，包含：peer_id、断开原因（connection_closed）
5. THE WebRtcMediaManager::Impl 的 PeerInfo SHALL 新增 `created_at` 字段（`std::chrono::steady_clock::time_point`），记录 PeerConnection 创建时间

### 需求 3：SDP Offer/Answer 摘要日志

**用户故事：** 作为开发者，我希望在收到 SDP offer 和发送 SDP answer 时能看到关键 SDP 属性摘要，以便快速判断 codec 协商是否正确。

#### 验收标准

1. WHEN 收到 viewer 的 SDP offer 时，THE WebRTC_Logger SHALL 使用 debug 级别输出 SDP 摘要，包含：peer_id、包含的 codec 列表（从 SDP 的 `a=rtpmap:` 行提取）
2. WHEN 发送 SDP answer 到 viewer 时，THE WebRTC_Logger SHALL 使用 debug 级别输出 SDP 摘要，包含：peer_id、选定的 codec 列表
3. WHEN SDP 文本解析失败（无法提取 codec 信息）时，THE WebRTC_Logger SHALL 回退到仅输出 peer_id 和 SDP 长度，不崩溃
4. THE SDP 摘要提取 SHALL 实现为一个独立的辅助函数（`extract_sdp_summary`），接受 SDP 文本字符串，返回摘要字符串
5. THE `extract_sdp_summary` 函数 SHALL 从 SDP 文本的 `a=rtpmap:` 行中提取 codec 名称（如 H264、opus），返回逗号分隔的 codec 列表字符串

### 需求 4：连接期间 Media Flow 状态日志

**用户故事：** 作为运维人员，我希望在 peer 连接期间能看到 media flow 状态日志，以便确认视频数据是否正常发送。

#### 验收标准

1. WHEN 某个 peer 的首次 writeFrame 成功时，THE WebRTC_Logger SHALL 使用 info 级别记录首帧发送事件，包含：peer_id、帧大小（bytes）、是否为关键帧
2. WHEN 某个 peer 的 writeFrame 连续失败次数达到阈值的 50%（kMaxWriteFailures / 2）时，THE WebRTC_Logger SHALL 使用 warn 级别记录写入失败警告，包含：peer_id、当前连续失败次数、最大允许次数
3. THE WebRtcMediaManager::Impl 的 PeerInfo SHALL 新增 `first_frame_sent` 布尔字段（默认 false），用于跟踪首帧发送状态
4. WHEN writeFrame 因 SRTP 未就绪（status 0x5c000003）而跳过时，THE WebRTC_Logger SHALL 使用 debug 级别记录跳过事件（当前为静默跳过），包含 peer_id

### 需求 5：Cleanup 日志增强

**用户故事：** 作为开发者，我希望 cleanup thread 释放 peer 时能看到更丰富的诊断信息，以便分析 peer 断开的模式和原因。

#### 验收标准

1. WHEN cleanup thread 释放过期的 DISCONNECTING peer 时，THE WebRTC_Logger SHALL 使用 info 级别记录释放事件，包含：peer_id、peer 存活时长（从 created_at 到释放时刻的秒数）、断开原因
2. WHEN on_viewer_offer 入口处清理 DISCONNECTING peer 时，THE WebRTC_Logger SHALL 使用 info 级别记录清理事件，包含：peer_id、peer 存活时长
3. WHEN WebRtcMediaManager 析构时释放所有 peer 时，THE WebRTC_Logger SHALL 使用 info 级别记录每个 peer 的释放事件，包含：peer_id、peer 存活时长、最终状态（CONNECTING/CONNECTED/DISCONNECTING）
4. THE WebRtcMediaManager::Impl 的 PeerInfo SHALL 新增 `disconnect_reason` 字符串字段（默认为空），在状态变为 DISCONNECTING 时记录原因（如 "connection_failed"、"connection_closed"、"max_write_failures"、"manual_remove"）

### 需求 6：Stub 实现日志同步更新

**用户故事：** 作为开发者，我希望 macOS stub 实现的日志与 real 实现保持一致的格式和级别，以便在开发环境中验证日志输出。

#### 验收标准

1. WHEN stub 实现中 peer 状态变化时，THE WebRTC_Logger SHALL 使用与 real 实现相同的日志格式和级别
2. WHEN stub 实现中 cleanup thread 释放 peer 时，THE WebRTC_Logger SHALL 输出与 real 实现相同格式的诊断信息（peer_id、存活时长）
3. THE stub 实现 SHALL 维护与 real 实现相同的 PeerInfo 扩展字段（created_at、first_frame_sent、disconnect_reason、ICE candidate 计数器），确保日志输出一致

### 需求 7：冒烟测试

**用户故事：** 作为开发者，我需要自动化测试验证日志优化未破坏现有功能，且新增的计数器和字段不影响 peer 管理逻辑。

#### 验收标准

1. THE Test_Suite SHALL 验证：现有 webrtc_test 和 webrtc_media_test 全部通过，日志改动未破坏已有功能
2. THE Test_Suite SHALL 验证：PeerInfo 新增字段（created_at、first_frame_sent、disconnect_reason、ICE candidate 计数器）不影响 peer_count 不变量
3. THE Test_Suite SHALL 验证：extract_sdp_summary 函数对有效 SDP 文本返回 codec 列表，对空字符串或无效文本返回空字符串
4. WHEN 执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 运行所有测试并报告结果，ASan 无报告

## 验证命令

```bash
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```

预期结果：编译无错误、所有测试通过（含现有 webrtc_test + webrtc_media_test）、ASan 无报告。

## 明确不包含

- 修改 WebRTC 连接建立、信令交换、媒体流传输的功能逻辑
- 修改 KVS WebRTC C SDK 内部日志
- 日志采样或限流机制（当前 WebRTC 日志量优化后已可控）
- WebRTC 连接质量指标采集（RTT、丢包率等 — 属于后续 Spec 范围）
- 日志文件轮转或远程日志收集
- 新增 logger（复用现有 `"webrtc"` logger）
