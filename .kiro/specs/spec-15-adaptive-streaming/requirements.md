# 需求文档：自适应码率控制 + 流模式切换

## 简介

本 Spec 为 Smart Camera 设备端实现自适应流控能力：根据各分支（KVS 录制、WebRTC 实时观看）的健康状态自动切换流模式（FULL / KVS_ONLY / WEBRTC_ONLY / DEGRADED），并根据网络状况动态调整 H.264 编码参数（码率）。

当前系统通过 GStreamer tee 分出三路（KVS、WebRTC、AI），但所有分支始终以固定参数运行，无法根据网络状况做出响应。本 Spec 引入流模式状态机和自适应码率控制器，在不重建 GStreamer pipeline 的前提下，通过动态调整 element 属性实现平滑切换。

## 前置条件

- spec-8（KVS Producer）✅
- spec-13（WebRTC Media）✅
- spec-5（Pipeline Health）✅
- spec-13.5（main.cpp KVS+WebRTC 集成）✅
- spec-14（WebRTC SDP bugfix）✅

## 术语表

- **Stream_Mode_Controller**：流模式状态机控制器，与 PipelineHealthMonitor 协作，根据各分支健康信号决定当前流模式
- **Bitrate_Adapter**：自适应码率控制器，根据分支健康状态动态调整编码参数
- **Stream_Mode**：流模式枚举（FULL / KVS_ONLY / WEBRTC_ONLY / DEGRADED）
- **Branch_Status**：分支健康状态枚举（HEALTHY / UNHEALTHY）

## 硬件与管道约束

- 摄像头：IMX678（USB，V4L2，主力）支持 YUYV 和 MJPG 格式
- 当前 pipeline 分辨率由摄像头源决定（未在 capsfilter 中限制分辨率），实际输出取决于 V4L2 设备默认分辨率
- 后续将通过配置文件（config.toml `[camera]` section）输入分辨率信息，本 Spec 的码率档位基于 720p~1080p 设计，配置文件加载在 spec-19（config-file）中实现
- 编码器：x264enc（软编码，ultrafast + zerolatency，2 线程）
- x264enc 的 `bitrate` 属性可在运行时动态修改（单位 kbps）
- 帧率由摄像头源决定，x264enc 不控制帧率。动态帧率调整需要 `videorate` element 或 capsfilter，本 Spec 不做帧率调整（复杂度高，收益有限）
- kvssink 的 `avg-bandwidth-bps` 和 `framerate` 属性是否支持运行时修改待验证

## 明确不包含

- AI 推理分支的自适应控制（等 spec-10 完成后再纳入）
- broadcast_frame 异步帧分发（等性能数据决定，可能在 spec-16）
- 零拷贝缓冲区优化（spec-16 的范围）
- 网络带宽主动探测（本 Spec 基于被动观测：KVS 上传成功率、WebRTC 连接状态）
- 动态帧率调整（需要 videorate element，复杂度高，推迟到后续 Spec）
- 动态分辨率降级（需要重建 pipeline 或 videoscale element，推迟到后续 Spec）

## 需求

### 需求 1：流模式状态机

**用户故事：** 作为设备运维人员，我希望系统根据各分支的健康状态自动切换流模式，以便在网络异常时保持最关键的数据通路。

#### 验收标准

1. THE Stream_Mode_Controller SHALL 支持四种流模式：FULL、KVS_ONLY、WEBRTC_ONLY、DEGRADED
2. WHEN Stream_Mode_Controller 初始化完成，THE Stream_Mode_Controller SHALL 进入 FULL 模式
3. WHEN WebRTC 分支报告 UNHEALTHY 且 KVS 分支 HEALTHY，THE Stream_Mode_Controller SHALL 切换到 KVS_ONLY 模式
4. WHEN KVS 分支报告 UNHEALTHY 且 WebRTC 分支 HEALTHY，THE Stream_Mode_Controller SHALL 切换到 WEBRTC_ONLY 模式
5. WHEN KVS 分支和 WebRTC 分支同时报告 UNHEALTHY，THE Stream_Mode_Controller SHALL 切换到 DEGRADED 模式
6. WHEN 之前 UNHEALTHY 的分支恢复为 HEALTHY，THE Stream_Mode_Controller SHALL 切换回 FULL 模式
7. WHEN 流模式发生切换，THE Stream_Mode_Controller SHALL 在 1 秒内完成切换操作
8. WHEN 流模式发生切换，THE Stream_Mode_Controller SHALL 通过回调通知已注册的观察者，回调参数包含旧模式、新模式、触发原因
9. THE Stream_Mode_Controller SHALL 实现防抖（debounce）：分支状态变化后等待 3 秒确认稳定，才触发模式切换，避免网络波动导致频繁切换
10. THE Stream_Mode_Controller SHALL 与现有 PipelineHealthMonitor 协作而非替代：PipelineHealthMonitor 负责管道级别的错误检测和重建，Stream_Mode_Controller 负责分支级别的流模式切换

### 需求 2：分支数据流控制

**用户故事：** 作为设备运维人员，我希望流模式切换时能动态控制对应分支的数据流，以便节省不必要的资源消耗。

#### 验收标准

1. WHEN 流模式切换到 KVS_ONLY，THE Stream_Mode_Controller SHALL 将 WebRTC 分支的 queue（q-web）设置为 leaky=downstream + max-size-buffers=0，使其丢弃所有数据
2. WHEN 流模式切换到 WEBRTC_ONLY，THE Stream_Mode_Controller SHALL 将 KVS 分支的 queue（q-kvs）设置为 leaky=downstream + max-size-buffers=0，使其丢弃所有数据
3. WHEN 流模式切换到 DEGRADED，THE Stream_Mode_Controller SHALL 将 WebRTC 分支的 queue 设为丢弃模式，仅保留 KVS 分支以最低码率运行
4. WHEN 流模式从非 FULL 切换回 FULL，THE Stream_Mode_Controller SHALL 恢复所有分支 queue 的原始参数（max-size-buffers=1, leaky=downstream for q-web; max-size-buffers=1, leaky=0 for q-kvs）
5. THE Stream_Mode_Controller SHALL 在不重建 GStreamer pipeline 的前提下完成分支数据流控制
6. THE Stream_Mode_Controller SHALL 通过 `gst_bin_get_by_name` 获取 pipeline 中的 queue element，不持有额外的 element 指针

### 需求 3：自适应码率控制

**用户故事：** 作为设备运维人员，我希望系统根据网络状况自动调整编码码率，以便在带宽受限时维持流畅传输。

#### 验收标准

1. THE Bitrate_Adapter SHALL 支持码率调整范围：最低 1000kbps，最高 4000kbps，档位间距 500kbps（共 7 档：1000/1500/2000/2500/3000/3500/4000）
2. WHEN KVS 分支报告 UNHEALTHY（上传失败），THE Bitrate_Adapter SHALL 降低目标码率一个档位
3. WHEN KVS 分支持续 HEALTHY 超过 30 秒且当前码率低于最大值，THE Bitrate_Adapter SHALL 提升目标码率一个档位
4. WHEN 流模式为 DEGRADED，THE Bitrate_Adapter SHALL 将码率设置为最低值（1000kbps）
5. WHEN 流模式从 DEGRADED 恢复到 FULL，THE Bitrate_Adapter SHALL 从当前最低码率开始逐步恢复（每 30 秒提升一档），不直接跳到最高值
6. THE Bitrate_Adapter SHALL 通过 `g_object_set(encoder, "bitrate", value_kbps, nullptr)` 动态调整 x264enc 的 bitrate 参数
7. THE Bitrate_Adapter SHALL 通过 `gst_bin_get_by_name(pipeline, "encoder")` 获取编码器 element
8. THE Bitrate_Adapter SHALL 每 5 秒执行一次码率评估周期（避免过于频繁的调整）

### 需求 4：KVS 分支健康上报

**用户故事：** 作为系统开发者，我希望 KVS 模块能向流模式控制器报告健康状态，以便触发流模式切换。

#### 验收标准

1. WHEN kvssink 连续 3 次发出 GST_MESSAGE_ERROR 或 GST_MESSAGE_WARNING（含 "timeout"、"connection" 关键词），THE KVS 分支 SHALL 报告 UNHEALTHY
2. WHEN kvssink 在报告 UNHEALTHY 后成功传输数据超过 10 秒无错误，THE KVS 分支 SHALL 报告 HEALTHY
3. THE KVS 分支健康检测 SHALL 通过 GStreamer bus message 监听实现，不修改 kvssink 内部逻辑
4. IF kvssink 不可用（macOS fakesink），THEN KVS 分支 SHALL 始终报告 HEALTHY

### 需求 5：WebRTC 分支健康上报

**用户故事：** 作为系统开发者，我希望 WebRTC 模块能向流模式控制器报告健康状态，以便触发流模式切换。

#### 验收标准

1. WHEN 所有 WebRTC peer 断开连接（peer_count() 返回 0）持续超过 5 秒，THE WebRTC 分支 SHALL 报告 UNHEALTHY
2. WHEN 新的 WebRTC peer 成功连接（peer_count() > 0），THE WebRTC 分支 SHALL 报告 HEALTHY
3. WHEN WebRTC 分支报告 UNHEALTHY，THE Stream_Mode_Controller SHALL 不再向 WebRTC appsink 推送数据（通过 queue 丢弃模式）
4. WHEN 没有 WebRTC viewer 连接时（正常的无人观看状态），THE WebRTC 分支 SHALL 在 5 秒后报告 UNHEALTHY，触发 KVS_ONLY 模式以节省 WebRTC 编码开销

### 需求 6：kvssink 参数同步

**用户故事：** 作为设备运维人员，我希望 kvssink 的 avg-bandwidth-bps 参数与实际编码码率匹配，以便 KVS SDK 内部缓冲区分配更合理。

#### 验收标准

1. WHEN Bitrate_Adapter 调整码率后，THE Bitrate_Adapter SHALL 尝试同步更新 kvssink 的 avg-bandwidth-bps 属性为当前目标码率值（bps = kbps × 1000）
2. IF kvssink 不支持运行时修改 avg-bandwidth-bps，THEN THE Bitrate_Adapter SHALL 仅在日志中记录当前码率，不产生错误
3. IF kvssink 不可用（macOS fakesink 替代），THEN THE Bitrate_Adapter SHALL 跳过 kvssink 属性更新

### 需求 7：模式切换日志与可观测性

**用户故事：** 作为设备运维人员，我希望所有流模式切换和码率调整都有结构化日志记录，以便远程诊断问题。

#### 验收标准

1. WHEN 流模式发生切换，THE Stream_Mode_Controller SHALL 输出 info 级别日志，包含旧模式、新模式、触发原因
2. WHEN Bitrate_Adapter 调整编码参数，THE Bitrate_Adapter SHALL 输出 info 级别日志，包含调整前后的码率值
3. WHEN 分支健康状态变化，SHALL 输出 warn 级别日志，包含分支名称和新状态
4. THE Stream_Mode_Controller SHALL 提供 `current_mode()` 查询接口，返回当前流模式
5. THE Bitrate_Adapter SHALL 提供 `current_bitrate_kbps()` 查询接口，返回当前目标码率
