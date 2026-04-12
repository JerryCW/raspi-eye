# 需求文档

## 简介

当前 raspi-eye 在 Pi 5 上运行 720p@15fps 双 tee 管道（KVS + WebRTC + AI fakesink）时 CPU 占用约 106%，而功能相同的 sample 项目仅约 40%。

通过代码走查和 v4l2-ctl 确认根因：IMX678 USB 3.0 摄像头支持 MJPG 和 YUYV 两种格式（没有 I420），`camera_source.cpp` 的 `select_best_format` 优先级为 `I420 > YUYV > MJPG`，因此选了 YUYV，走裸 v4l2src 路径。随后 `pipeline_builder.cpp` 的 `videoconvert` 做 YUYV→I420 全帧像素转换，非常吃 CPU。sample 项目强制 MJPG + jpegdec，jpegdec 解码开销远小于 videoconvert 的 YUYV→I420 转换。USB 3.0 带宽不是瓶颈（5Gbps），CPU 格式转换开销才是根因。

本 Spec 包含两件事：
1. V4L2 USB 摄像头格式优先级改为 MJPG 优先，消除冗余 videoconvert，预期 CPU 从 ~106% 降到 ~40%
2. 三路分支 enable 配置开关，方便精确诊断每路分支的 CPU 占用

## 术语表

- **CameraSource**: 摄像头源抽象层，定义在 `camera_source.h` 中，负责根据配置创建 GStreamer 视频源元素
- **PipelineBuilder**: 双 tee 管道构建器，定义在 `pipeline_builder.h` 中，构建 src → videoconvert → capsfilter → raw-tee 的主干管道
- **select_best_format**: `camera_source.cpp` 中的格式选择函数，从 V4L2 设备探测到的格式列表中按优先级选择最佳格式
- **V4L2Format**: 枚举类型，表示 V4L2 设备支持的像素格式（I420、YUYV、MJPG）
- **videoconvert**: GStreamer 元素，执行像素格式转换（如 YUYV→I420），CPU 密集型操作
- **jpegdec**: GStreamer 元素，解码 MJPEG 帧为原始像素数据（通常为 I420）
- **Source_Bin**: `camera_source.cpp` 中创建的 GstBin，封装摄像头源元素链并通过 ghost pad 对外暴露
- **CameraConfig**: 摄像头配置 POD 结构体，包含 type、device、width、height、framerate 字段
- **ConfigManager**: 统一配置管理器，负责从 config.toml 解析各 section 的配置
- **fakesink**: GStreamer 元素，丢弃所有接收到的数据，用于占位或禁用分支

## 需求

### 需求 1：V4L2 USB 摄像头格式优先级改为 MJPG 优先

**用户故事：** 作为设备运维人员，我希望 USB 摄像头优先使用 MJPG 格式采集，以大幅降低 CPU 格式转换开销。

#### 验收标准

1. WHEN V4L2 设备同时支持 MJPG 和 YUYV 格式, THE select_best_format SHALL 选择 MJPG 格式
2. WHEN V4L2 设备同时支持 MJPG 和 I420 格式, THE select_best_format SHALL 选择 MJPG 格式
3. WHEN V4L2 设备仅支持 YUYV 格式, THE select_best_format SHALL 选择 YUYV 格式作为回退
4. WHEN V4L2 设备仅支持 I420 格式, THE select_best_format SHALL 选择 I420 格式作为回退
5. THE select_best_format SHALL 按照 MJPG > I420 > YUYV 的优先级顺序选择格式

### 需求 2：MJPG Source Bin 使用 CameraConfig 分辨率和帧率

**用户故事：** 作为开发者，我希望 MJPG 管道的分辨率和帧率从 CameraConfig 读取，而非硬编码 1920x1080@15fps，以支持 config.toml 中配置的任意分辨率。

#### 验收标准

1. WHEN create_source 创建 MJPG Source Bin, THE CameraSource SHALL 使用 CameraConfig 中的 width、height、framerate 值设置 MJPG capsfilter 的分辨率和帧率
2. WHEN CameraConfig 指定 width=1280、height=720、framerate=15, THE MJPG Source Bin 的 capsfilter SHALL 设置为 image/jpeg,width=1280,height=720,framerate=15/1
3. WHEN CameraConfig 指定 width=1920、height=1080、framerate=30, THE MJPG Source Bin 的 capsfilter SHALL 设置为 image/jpeg,width=1920,height=1080,framerate=30/1

### 需求 3：消除 MJPG 路径的冗余 videoconvert

**用户故事：** 作为设备运维人员，我希望当摄像头源已输出 I420 格式时跳过 videoconvert 元素，以减少一次全帧像素拷贝的 CPU 开销。

#### 验收标准

1. WHEN Source Bin 通过 MJPG + jpegdec 路径输出 I420, THE PipelineBuilder SHALL 跳过 videoconvert 元素，直接将 Source Bin 连接到 capsfilter
2. WHEN Source Bin 通过 I420 原始路径输出 I420, THE PipelineBuilder SHALL 跳过 videoconvert 元素，直接将 Source Bin 连接到 capsfilter
3. WHEN Source Bin 输出格式为 YUYV（需要格式转换）, THE PipelineBuilder SHALL 保留 videoconvert 元素执行 YUYV→I420 转换
4. WHEN Source Bin 为 videotestsrc（macOS 开发环境）, THE PipelineBuilder SHALL 保留 videoconvert 元素以确保格式兼容


### 需求 4：三路分支 enable 配置开关

**用户故事：** 作为开发者，我希望在 config.toml 中为 KVS、WebRTC、AI 三路分支分别添加 enable 开关，以便精确诊断每路分支的 CPU 占用，方便后续性能调优。

#### 验收标准

1. THE config.toml SHALL 在 [kvs]、[webrtc]、[ai] section 中分别支持 enabled 字段，类型为布尔值
2. WHEN config.toml 中 [kvs].enabled 未指定, THE ConfigManager SHALL 默认值为 true（向后兼容）
3. WHEN config.toml 中 [webrtc].enabled 未指定, THE ConfigManager SHALL 默认值为 true（向后兼容）
4. WHEN config.toml 中 [ai] section 未指定或 [ai].enabled 未指定, THE ConfigManager SHALL 默认值为 true（向后兼容）
5. WHEN [kvs].enabled 设置为 false, THE PipelineBuilder SHALL 使用 fakesink 替代真实 KVS sink，KVS 分支不建立实际连接
6. WHEN [webrtc].enabled 设置为 false, THE PipelineBuilder SHALL 使用 fakesink 替代 appsink，WebRTC 分支不建立实际连接
7. WHEN [ai].enabled 设置为 false, THE PipelineBuilder SHALL 使用 fakesink 作为 AI 分支 sink（当前 AI 分支已是 fakesink，此条为未来 spec-10 集成后的预留）
8. WHEN enabled 字段值不是合法布尔值（true/false）, THEN THE ConfigManager SHALL 返回解析错误并包含字段名和非法值

### 需求 5：向后兼容与平台隔离

**用户故事：** 作为开发者，我希望优化改动不影响 macOS 开发环境和非 USB 摄像头场景。

#### 验收标准

1. WHEN 运行在 macOS 开发环境（videotestsrc）, THE PipelineBuilder SHALL 保持现有管道结构不变，videoconvert 元素保留
2. WHEN 使用 libcamerasrc（CSI 摄像头）, THE CameraSource SHALL 保持现有行为不变
3. WHEN V4L2 设备未指定 device 路径（config.device 为空）, THE CameraSource SHALL 保持现有行为，不执行格式探测
4. THE 优化改动 SHALL 通过现有 macOS 单元测试（ctest），不引入新的测试失败
5. WHEN config.toml 中不包含 [ai] section, THE ConfigManager SHALL 正常加载，AI 分支 enabled 默认为 true

## 不包含

- dmabuf 零拷贝（MJPG 路径下 jpegdec 输出新 buffer，dmabuf 无法端到端生效）
- GstBufferPool 预分配（jpegdec 内部已有 buffer 管理）
- 硬件 H.264 编码器（Pi 5 没有 v4l2h264enc）
- 分辨率/帧率调整（保持 config.toml 中的配置）
- AI 推理管道集成（spec-10）
- 网络传输优化

## 约束

- C++17，修改范围限于 camera_source.cpp/h、pipeline_builder.cpp/h、config_manager.cpp/h、app_context.cpp、config.toml、config.toml.example
- 格式优先级变更仅影响 V4L2 路径，macOS（videotestsrc）和 libcamera 路径不受影响
- 三路 enable 开关默认全部为 true，确保不改变现有行为
- 不修改 H.264 编码器配置（x264enc 参数保持不变）
- 不包含硬件 H.264 编码器支持（Pi 5 无 v4l2h264enc）

## 禁止项

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret（来源：安全基线）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）
- SHALL NOT 在 macOS 上直接在 main() 中运行含 autovideosink 的 GStreamer 管道（来源：spec-0）
- SHALL NOT 在无法本地复现的远程平台问题上凭猜测修复（来源：spec-5）
