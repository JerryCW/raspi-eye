# 需求文档：网络自适应码率控制

## 简介

本 Spec 解决 Pi 5 生产环境中 KVS putMedia 连接反复断开的核心问题：编码码率（2500 kbps）持续超过实际上传带宽（~1.7 Mbps），导致 KVS SDK 内部 buffer 积压、streamLatencyPressure 回调触发 10000+ 次/30 分钟，但该信号未接入 BitrateAdapter，系统无法自动降码率。

当前 Spec 15（adaptive-streaming）已建立 BitrateAdapter + StreamModeController 架构，但健康信号仅来自 GStreamer bus message（kvssink stream-status），而 KVS Producer SDK 的 streamLatencyPressure 回调和 WebRTC writeFrame 失败率均未接入。本 Spec 补齐网络感知信号通路，实现真正的网络自适应码率控制。

## 前置条件

- spec-15（adaptive-streaming）✅ — BitrateAdapter + StreamModeController 架构
- spec-25（webrtc-log-observability）✅ — WebRTC 日志可观测性

## 术语表

- **Bitrate_Adapter**：自适应码率控制器（已有，spec-15 实现），根据健康信号动态调整编码参数
- **Stream_Mode_Controller**：流模式状态机（已有，spec-15 实现），根据分支健康信号切换流模式
- **Network_Monitor**：网络状态监控器（新增），聚合 KVS latency pressure、WebRTC writeFrame 失败率等网络信号，向 BitrateAdapter 报告
- **Latency_Pressure**：KVS Producer SDK 的 streamLatencyPressure 回调，当 SDK 内部 buffer 积压超过阈值时触发
- **Bandwidth_Probe**：启动时的轻量级带宽估算，通过测量 KVS putMedia 初始数据传输速率推算可用上传带宽
- **kvssink**：GStreamer KVS sink element，KVS Producer SDK 的 GStreamer 封装

## 硬件与网络约束

- 目标平台：Raspberry Pi 5（4GB RAM，Debian Bookworm aarch64）
- 网络环境：家庭宽带，上传带宽不稳定（实测 KVS ap-southeast-1 仅 211 KB/s ≈ 1.7 Mbps）
- KVS SDK 的 streamLatencyPressure 回调在 SDK 内部线程触发，非 GStreamer 线程
- kvssink 属性 avg-bandwidth-bps 默认 4194304（4 Mbps），buffer-duration 默认 120 秒
- WebRTC TURN relay 在网络拥塞时 16 秒内超时断开

## 明确不包含

- KVS region 迁移（东京/香港）评估（运维决策，非代码变更）
- 动态帧率调整（需要 videorate element，复杂度高，推迟）
- 动态分辨率降级（需要重建 pipeline 或 videoscale element，推迟）
- AI 推理分支的网络自适应控制（AI 分支不涉及网络上传）
- KVS SDK 源码修改（仅通过 SDK 公开回调和 kvssink 属性调优）

## 需求

### 需求 1：KVS Latency Pressure 信号接入

**用户故事：** 作为设备运维人员，我希望 KVS SDK 的 streamLatencyPressure 回调能触发码率降低，以便在网络拥塞时自动减少编码码率，避免 buffer 无限积压导致连接断开。

#### 验收标准

1. WHEN KVS Producer SDK 触发 streamLatencyPressure 回调，THE Network_Monitor SHALL 将该事件转发给 Bitrate_Adapter 作为 UNHEALTHY 信号
2. WHEN streamLatencyPressure 回调在 10 秒内触发超过 5 次，THE Network_Monitor SHALL 向 Bitrate_Adapter 报告连续 UNHEALTHY，触发多次降档直到码率稳定在上传带宽以下
3. WHEN streamLatencyPressure 回调停止触发超过 30 秒，THE Network_Monitor SHALL 允许 Bitrate_Adapter 恢复正常的 rampup 逻辑
4. THE Network_Monitor SHALL 通过线程安全机制（mutex 或 atomic）接收 KVS SDK 内部线程的回调，不阻塞 SDK 线程
5. IF kvssink 不可用（macOS fakesink），THEN THE Network_Monitor SHALL 跳过 latency pressure 监听

### 需求 2：kvssink 属性调优

**用户故事：** 作为设备运维人员，我希望 kvssink 的 avg-bandwidth-bps 和 buffer-duration 属性与实际网络条件匹配，以便 KVS SDK 内部缓冲区管理更合理，减少不必要的连接断开。

#### 验收标准

1. WHEN kvssink 创建时，THE KVS_Sink_Factory SHALL 将 avg-bandwidth-bps 设置为 BitrateConfig 的 default_kbps × 1000（而非 SDK 默认的 4 Mbps）
2. WHEN kvssink 创建时，THE KVS_Sink_Factory SHALL 将 buffer-duration 设置为配置文件中指定的值（默认 180 秒），提供更大的缓冲容忍度
3. WHEN Bitrate_Adapter 调整码率后，THE Bitrate_Adapter SHALL 同步更新 kvssink 的 avg-bandwidth-bps 属性为新码率值（已有逻辑，本需求确认保留）
4. THE KVS_Sink_Factory SHALL 在创建 kvssink 后输出 info 级别日志，包含 avg-bandwidth-bps 和 buffer-duration 的实际设置值

### 需求 3：WebRTC 网络健康信号

**用户故事：** 作为设备运维人员，我希望 WebRTC 路径的网络健康状态能反馈给码率控制器，以便在 TURN relay 拥塞时也能触发码率降低。

#### 验收标准

1. WHEN WebRtcMediaManager 的 broadcast_frame 调用中 writeFrame 连续失败超过 10 次，THE Network_Monitor SHALL 向 Stream_Mode_Controller 报告 WebRTC 分支 UNHEALTHY
2. WHEN writeFrame 失败率从高位恢复（连续 50 次成功），THE Network_Monitor SHALL 向 Stream_Mode_Controller 报告 WebRTC 分支 HEALTHY
3. THE Network_Monitor SHALL 独立于现有的 peer_count 检测逻辑运行：peer_count 检测判断"有无观众"，writeFrame 失败率检测判断"网络是否拥塞"
4. WHEN WebRTC 分支因 writeFrame 失败报告 UNHEALTHY，THE Stream_Mode_Controller SHALL 按现有逻辑切换流模式（与 peer_count=0 触发的 UNHEALTHY 行为一致）

### 需求 4：启动带宽探测

**用户故事：** 作为设备运维人员，我希望系统启动时能估算实际上传带宽，以便设置合理的初始码率，而非始终从 2500 kbps 开始导致立即积压。

#### 验收标准

1. WHEN 系统启动且 KVS 分支启用时，THE Bandwidth_Probe SHALL 在 pipeline 启动后的前 10 秒内测量 KVS 上传吞吐量
2. WHEN 带宽探测完成，THE Bandwidth_Probe SHALL 计算可用上传带宽估算值，并将 Bitrate_Adapter 的初始码率设置为估算带宽的 80%（向下取整到最近的 step 档位）
3. IF 估算带宽低于 BitrateConfig 的 min_kbps，THEN THE Bandwidth_Probe SHALL 将初始码率设置为 min_kbps
4. IF 估算带宽高于 BitrateConfig 的 max_kbps，THEN THE Bandwidth_Probe SHALL 将初始码率设置为 max_kbps
5. IF 带宽探测失败（KVS 未连接、fakesink 等），THEN THE Bandwidth_Probe SHALL 使用 BitrateConfig 的 default_kbps 作为初始码率
6. THE Bandwidth_Probe SHALL 在探测完成后输出 info 级别日志，包含估算带宽值和选定的初始码率

### 需求 5：网络相关配置

**用户故事：** 作为设备运维人员，我希望通过 config.toml 配置网络自适应参数，以便根据不同部署环境调整行为。

#### 验收标准

1. THE ConfigManager SHALL 从 config.toml 的 [streaming] section 解析以下新增字段：
   - `buffer_duration_sec`（整数，默认 180，kvssink buffer-duration 秒数）
   - `latency_pressure_threshold`（整数，默认 5，10 秒内触发次数阈值）
   - `latency_pressure_cooldown_sec`（整数，默认 30，pressure 停止后恢复等待秒数）
   - `bandwidth_probe_enabled`（布尔，默认 true，是否启用启动带宽探测）
   - `bandwidth_probe_duration_sec`（整数，默认 10，探测持续秒数）
   - `writeframe_fail_threshold`（整数，默认 10，WebRTC writeFrame 连续失败阈值）
2. WHEN 配置字段缺失，THE ConfigManager SHALL 使用上述默认值
3. WHEN 配置字段值无效（负数、零等），THE ConfigManager SHALL 返回解析错误并填充 error_msg

### 需求 6：网络自适应日志与可观测性

**用户故事：** 作为设备运维人员，我希望所有网络自适应决策都有结构化日志记录，以便远程诊断网络问题。

#### 验收标准

1. WHEN Network_Monitor 收到 streamLatencyPressure 回调，THE Network_Monitor SHALL 输出 warn 级别日志，包含当前 buffer 积压时长和触发次数
2. WHEN Bandwidth_Probe 完成探测，THE Bandwidth_Probe SHALL 输出 info 级别日志，包含测量的上传速率（KB/s）、估算带宽（kbps）、选定初始码率（kbps）
3. WHEN Network_Monitor 因 writeFrame 失败率触发 WebRTC UNHEALTHY，THE Network_Monitor SHALL 输出 warn 级别日志，包含连续失败次数
4. THE Network_Monitor SHALL 使用 "network" logger 名称，与现有 "bitrate" logger 区分

### 需求 7：WebRTC 弱网帧丢弃策略

**用户故事：** 作为观看者，我希望在网络拥塞时仍能看到低帧率的画面，而不是完全黑屏后断开。

#### 验收标准

1. WHEN WebRtcMediaManager 的 writeFrame 连续失败超过 writeframe_fail_threshold 次，THE broadcast_frame SHALL 切换到"仅关键帧"模式：跳过所有非关键帧，仅尝试发送关键帧
2. WHEN "仅关键帧"模式下 writeFrame 连续成功超过 10 次，THE broadcast_frame SHALL 恢复正常模式（发送所有帧）
3. WHEN 帧被"仅关键帧"模式跳过时，THE WebRTC_Logger SHALL 使用 debug 级别记录跳过事件（不在每帧输出，仅在模式切换时输出 info 日志）
4. THE "仅关键帧"模式 SHALL 为每个 peer 独立维护状态（一个 peer 拥塞不影响其他 peer 的帧发送）

### 需求 8：WebRTC 连接拥塞自动重建

**用户故事：** 作为设备运维人员，我希望 TURN relay 持续超时时系统能自动重建连接，尝试获取新的 TURN 通道，而不是让连接一直挂在那里。

#### 验收标准

1. WHEN 某个 peer 的 writeFrame 连续失败超过 kMaxWriteFailures（现有逻辑，100 次），THE WebRtcMediaManager SHALL 将该 peer 标记为 DISCONNECTING（现有逻辑，本需求确认保留）
2. WHEN peer 被标记为 DISCONNECTING 后，THE cleanup_thread SHALL 在 grace period 后释放该 peer 的资源（现有逻辑）
3. WHEN viewer 重新发起 SDP offer（自动重连），THE WebRtcMediaManager SHALL 正常接受并创建新的 PeerConnection（现有逻辑，本需求确认端到端重连路径可用）
4. THE WebRTC_Logger SHALL 在 peer 因 writeFrame 失败被标记 DISCONNECTING 时输出 warn 级别日志，包含 disconnect_reason="max_write_failures"（已在 Spec 25 实现）


### 需求 9：WebRTC Peer 连接后强制关键帧

**用户故事：** 作为观看者，我希望连接成功后立即看到画面，而不是等待下一个自然关键帧（可能 10-16 秒后）。

#### 验收标准

1. WHEN 新 peer 的 PeerConnection 状态变为 CONNECTED，THE WebRtcMediaManager SHALL 向 GStreamer 编码器发送 force-keyunit 事件，请求立即生成一个 IDR 帧
2. THE force-keyunit 事件 SHALL 通过 GStreamer 的 `gst_pad_send_event` 或 `gst_element_send_event` 发送到 h264 编码器的 sink pad
3. WHEN force-keyunit 事件发送成功，THE WebRTC_Logger SHALL 输出 info 级别日志：`"Force keyframe requested for new peer {peer_id}"`
4. IF GStreamer pipeline 引用不可用（WebRtcMediaManager 未持有 pipeline 引用），THEN THE force-keyunit 请求 SHALL 被跳过并输出 warn 日志
5. THE force-keyunit 请求 SHALL 不影响正常的 GOP 周期（仅插入一个额外的 IDR，不改变编码器的 key-int-max 设置）

