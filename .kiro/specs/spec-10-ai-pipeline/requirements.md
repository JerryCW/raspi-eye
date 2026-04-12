# 需求文档：Spec 10 — AI 推理管道集成

## 简介

本 Spec 将 tee pipeline 的 AI 分支从 fakesink 占位替换为实际的 buffer probe 抽帧 + YoloDetector 推理管道。在 raw-tee 的 AI 分支上安装 GstPad buffer probe，按可配置间隔抽取 I420 原始帧，转换为 RGB 后送入 YoloDetector 执行目标检测。推理在独立线程中异步执行，不阻塞 GStreamer streaming 线程。检测结果通过回调通知上层（为 Spec 11 截图上传做准备）。

本 Spec 新增一个 `AiPipelineHandler` 类，封装 probe 安装、抽帧节流、异步推理、结果回调的完整逻辑。`build_tee_pipeline` 函数签名扩展以接受 `AiPipelineHandler*` 参数，当非空时替换 AI 分支的 fakesink 为带 probe 的 queue + fakesink 组合。

## 前置条件

- Spec 3（h264-tee-pipeline）✅ 已完成 — 提供 raw-tee AI 分支
- Spec 5（pipeline-health）✅ 已完成 — 提供管道健康监控
- Spec 9（yolo-detector）✅ 已完成 — 提供 YoloDetector 推理接口
- Spec 9.5（onnx-arm-optimization）✅ 已完成 — 提供 ARM 优化推理配置
- Spec 13.5（main-integration）✅ 已完成 — 提供 AppContext 集成框架

## 术语表

- **AiPipelineHandler**：本 Spec 新增的核心类，管理 AI 分支的 buffer probe 安装、抽帧节流、异步推理调度和结果回调
- **Buffer Probe**：GStreamer pad probe，在数据流经 pad 时触发回调，可读取 GstBuffer 内容而不影响数据流
- **raw-tee**：双 tee 管道中的第一个 tee，输出 I420 原始帧，AI 分支从此处分出
- **I420**：YUV 4:2:0 平面格式，GStreamer 管道中 videoconvert 输出的标准格式
- **YoloDetector**：Spec 9 中实现的 YOLO 目标检测类，接受 RGB uint8_t* 输入，返回 Detection 向量
- **Detection**：检测结果 POD 结构体（x, y, w, h 归一化坐标 + class_id + confidence）
- **InferenceStats**：推理统计 POD 结构体（preprocess_ms, inference_ms, postprocess_ms, total_ms）
- **DetectionCallback**：检测结果回调函数类型，上层注册后在推理完成时被调用，传递检测结果 + RGB 帧数据
- **抽帧率（inference_fps）**：每秒抽取的帧数，内部转换为间隔毫秒数（1000/fps），用于控制推理频率避免 CPU 过载
- **snapshot_dir**：事件截图保存根目录（默认 `device/events/`），事件目录创建在此目录下，供 Spec 11 S3 uploader 按事件目录为单位读取上传
- **检测事件（Detection Event）**：从首次检测到目标开始，到连续 event_timeout_sec 秒无检测结果结束的一个会话。同一事件的所有截图保存在同一个事件目录中，元数据记录在 `event.json`
- **event.json**：事件目录中的元数据文件，记录事件 ID、设备 ID、起止时间、状态（active/closed）、截图数量、按类别汇总的检测统计。S3 uploader 通过 status 字段判断事件是否可上传
- **event_timeout_sec**：事件超时秒数，连续无检测结果超过此时间则事件结束，默认 15 秒
- **target_classes**：需要探测的目标类别配置（逗号分隔字符串，格式 `name[:confidence]`），每个条目含 COCO 类名和可选 per-class 置信度阈值。只有匹配的类别且置信度达标才触发事件。为空时所有类别使用全局阈值
- **PipelineHealthMonitor**：Spec 5 中实现的管道健康监控类，监控 buffer 流动和管道状态

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan |
| Pi 5 推理耗时 | YOLOv11s 单帧 ~300-600ms（Spec 9.5 基线数据） |
| 默认抽帧率 | inference_fps=2（每秒 2 帧，间隔 500ms，Pi 5 上推理 ~350ms + 余量） |
| GStreamer streaming 线程阻塞 | probe 回调中 SHALL NOT 执行推理，仅做帧拷贝 + 投递到推理线程 |
| probe 回调耗时 | I420→RGB 转换 + memcpy ≤ 5ms（720p 帧） |
| 推理线程 | 单独的 std::thread，生命周期由 AiPipelineHandler 管理 |
| 内存预算 | 推理线程额外 RSS ≤ 50MB（不含 ONNX Runtime 模型本身的内存） |
| 新增代码量 | 200-500 行 |
| 涉及文件 | 3-6 个 |
| 单个测试耗时 | 纯逻辑 ≤ 1 秒，含管道启停 ≤ 5 秒 |
| 日志语言 | 英文 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现截图上传 S3 功能（属于 Spec 11: screenshot-uploader，本 Spec 只保存到本地磁盘）
- SHALL NOT 在本 Spec 中实现截图文件的自动清理或轮转（属于 Spec 11）
- SHALL NOT 在本 Spec 中实现检测结果的持久化存储或数据库写入
- SHALL NOT 在本 Spec 中实现多模型并行推理或模型热切换
- SHALL NOT 在本 Spec 中修改 KVS 或 WebRTC 分支的行为

### Design 层

- SHALL NOT 在 GStreamer streaming 线程（buffer probe 回调）中执行 YOLO 推理（来源：Pi 5 推理耗时 300-600ms，阻塞 streaming 线程会导致所有分支卡顿）
- SHALL NOT 在 probe 回调中分配大块堆内存（使用预分配缓冲区）
- SHALL NOT 让推理线程持有 GStreamer 对象的引用（GStreamer 对象非线程安全，probe 回调中拷贝帧数据后立即释放 GstBuffer 映射）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
- SHALL NOT 在不确定 GStreamer pad probe API 用法时凭猜测编写代码
- SHALL NOT 依赖 GstVideoMeta 获取帧宽高（GstVideoMeta 不一定存在），必须使用 `gst_pad_get_current_caps()` 从 pad 的 negotiated caps 中获取
- SHALL NOT 在 I420→RGB 转换中使用浮点乘法（使用整数定点运算，优化 Pi 5 ARM 性能）

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（使用 std::unique_ptr / std::make_unique）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件

## 需求

### 需求 1：AiPipelineHandler 类 — 生命周期管理

**用户故事：** 作为开发者，我需要一个封装类来管理 AI 推理管道的完整生命周期（创建 → 启动 → 停止 → 销毁），以便安全地集成到 AppContext 中。

#### 验收标准

1. THE AiPipelineHandler SHALL 通过工厂方法 `create()` 构造，接受 YoloDetector 的 unique_ptr 所有权和 AI 管道配置参数（inference_fps 等）
2. WHEN 提供有效的 YoloDetector 实例时，THE AiPipelineHandler::create() SHALL 返回有效的 unique_ptr
3. IF YoloDetector 为 nullptr，THEN THE AiPipelineHandler::create() SHALL 返回 nullptr 并通过 error_msg 报告错误
4. THE AiPipelineHandler SHALL 禁用拷贝构造和拷贝赋值（`= delete`）
5. THE AiPipelineHandler SHALL 在析构时自动停止推理线程并释放所有资源
6. THE AiPipelineHandler SHALL 通过 spdlog 记录创建成功信息，包含抽帧间隔配置值

### 需求 2：Buffer Probe 安装与抽帧节流

**用户故事：** 作为开发者，我需要在 AI 分支的 queue 输出 pad 上安装 buffer probe，按配置间隔抽取原始帧，以便控制推理频率不超过 CPU 承载能力。

#### 验收标准

1. THE AiPipelineHandler SHALL 提供 `install_probe(GstElement* pipeline)` 方法，在名为 "q-ai" 的 queue 元素的 src pad 上安装 buffer probe
2. WHEN buffer probe 触发时，THE AiPipelineHandler SHALL 检查距上次抽帧的时间间隔，仅当间隔 ≥ 1000/inference_fps 毫秒时才抽取帧数据（例如 inference_fps=2 时间隔为 500ms）
3. WHEN 抽帧间隔未到时，THE buffer probe SHALL 返回 GST_PAD_PROBE_OK 直接放行，不做任何额外操作
4. WHEN 抽帧间隔已到且推理线程空闲时，THE buffer probe SHALL 从 GstBuffer 中映射 I420 帧数据，转换为 RGB 格式，拷贝到预分配缓冲区，然后通知推理线程
5. WHEN 推理线程正在忙碌时，THE buffer probe SHALL 跳过本帧（不排队等待），返回 GST_PAD_PROBE_OK
6. THE buffer probe 回调 SHALL 始终返回 GST_PAD_PROBE_OK（不阻塞数据流）
7. THE AiPipelineHandler SHALL 提供 `remove_probe()` 方法，安全移除已安装的 probe

### 需求 3：I420 到 RGB 帧转换

**用户故事：** 作为开发者，我需要将 GStreamer 管道输出的 I420 格式帧转换为 YoloDetector 要求的 RGB 格式，以便正确执行推理。

**背景：** 摄像头原生输出 YUYV/RGB3 等格式，但管道中 `videoconvert → capsfilter(I420) → raw-tee` 保证了 raw-tee 之后一定是 I420。I420 是 x264enc 编码分支的最优输入格式，AI 分支在 probe 中做一次 I420→RGB 软件转换。

#### 验收标准

1. THE AiPipelineHandler SHALL 通过 `gst_pad_get_current_caps()` 从 probe 所在 pad 的 negotiated caps 中获取帧宽高信息（不依赖 GstVideoMeta，因为 GstVideoMeta 不一定存在）
2. THE AiPipelineHandler SHALL 将 I420（YUV 4:2:0 平面格式）帧数据转换为 RGB888（3 字节/像素，R-G-B 顺序）
3. THE I420 到 RGB 转换 SHALL 使用整数定点运算实现 BT.601 系数（避免浮点乘法，优化 Pi 5 ARM 性能），输出值 clamp 到 [0, 255]
4. THE I420 到 RGB 转换函数 SHALL 作为独立的内部函数（非类成员），接受 I420 平面指针 + 宽高 + stride，输出 RGB 缓冲区，以便独立测试
5. FOR ALL 有效的 I420 输入（宽高均 > 0 且为偶数），THE 转换函数 SHALL 输出 width × height × 3 字节的 RGB 数据

### 需求 4：异步推理线程

**用户故事：** 作为开发者，我需要在独立线程中执行 YOLO 推理，以避免阻塞 GStreamer streaming 线程导致视频卡顿。

#### 验收标准

1. THE AiPipelineHandler SHALL 在 `start()` 时创建一个推理工作线程
2. THE 推理线程 SHALL 通过条件变量等待新帧通知，空闲时不消耗 CPU
3. WHEN 收到新帧通知时，THE 推理线程 SHALL 调用 YoloDetector::detect_with_stats() 执行推理
4. WHEN 推理完成后，THE 推理线程 SHALL 根据 `target_classes` 配置过滤检测结果：仅保留 class_name 在 target_classes 中且 confidence ≥ 该类别阈值的 Detection；如果 target_classes 为空则保留所有 confidence ≥ 全局 confidence_threshold 的检测结果
5. WHEN 过滤后检测到目标（Detection 向量非空）时，THE 推理线程 SHALL 调用已注册的 DetectionCallback
6. THE AiPipelineHandler SHALL 通过 spdlog（debug 级别）记录每次推理的耗时统计和检测到的目标数量
7. THE AiPipelineHandler SHALL 在 `stop()` 时通知推理线程退出并 join 等待线程结束
8. IF 推理过程中 YoloDetector 返回空结果，THEN THE 推理线程 SHALL 继续等待下一帧，不触发回调

### 需求 5：检测结果回调接口

**用户故事：** 作为开发者，我需要一个回调接口来接收检测结果和原始帧数据，以便上层模块（Spec 11 截图上传）可以在检测到目标时保存截图并执行后续操作。

#### 验收标准

1. THE AiPipelineHandler SHALL 提供 `set_detection_callback(DetectionCallback cb)` 方法
2. THE DetectionCallback 类型 SHALL 定义为 `std::function<void(const std::vector<Detection>&, const InferenceStats&, const uint8_t* rgb_data, int frame_width, int frame_height)>`，传递检测结果、推理统计、RGB 帧数据指针和帧尺寸
3. WHEN 检测到目标时，THE AiPipelineHandler SHALL 在推理线程中调用 DetectionCallback，rgb_data 指向当前推理帧的 RGB 缓冲区（回调期间有效，回调返回后可能被覆盖）
4. IF 未注册 DetectionCallback，THEN THE AiPipelineHandler SHALL 仅记录日志，不报错
5. THE DetectionCallback SHALL 在推理线程上下文中被调用（回调实现者负责线程安全）

### 需求 5.5：检测事件管理与截图本地保存

**用户故事：** 作为运维人员，我需要系统在检测到目标时自动开启一个检测事件会话，将事件期间所有截图保存到同一个事件目录中，以便后续 Spec 11 的 S3 uploader 按事件为单位上传。

#### 事件生命周期

```
推理检测到目标 → 开启事件（创建事件目录）→ 保存截图到事件目录
                                              ↓
                                    持续检测到目标 → 继续保存到同一事件目录，刷新超时计时器
                                              ↓
                                    连续 N 秒无检测结果 → 事件结束（关闭事件目录）
                                              ↓
                                    再次检测到目标 → 开启新事件（新目录）
```

#### 验收标准

1. WHEN 推理检测到目标（经 target_classes 过滤后 Detection 向量非空）且当前无活跃事件时，THE AiPipelineHandler SHALL 开启新的检测事件：创建事件目录，记录事件开始时间
2. THE 事件目录 SHALL 位于 `snapshot_dir` 下，命名格式为 `evt_{timestamp}`（例如 `evt_20260412_153045`），timestamp 为事件开始时间
3. WHILE 事件活跃期间，THE AiPipelineHandler SHALL 仅将检测到目标（经 target_classes 过滤后非空）的帧 JPEG 编码后缓存到内存中。未检测到目标的帧不保存截图（节省内存和磁盘空间）
4. THE 截图文件名 SHALL 为 `{frame_timestamp}_{seq}.jpg`（例如 `20260412_153046_001.jpg`），seq 为事件内递增序号，文件名在内存中生成，事件关闭时写盘使用
5. WHEN 推理检测到目标（经 target_classes 过滤后非空）时，THE AiPipelineHandler SHALL 刷新事件超时计时器（重置为 event_timeout_sec）
6. WHEN 连续 event_timeout_sec 秒内所有推理结果均为空（经 target_classes 过滤后无检测到目标）时，THE AiPipelineHandler SHALL 结束当前事件，记录 info 日志（事件持续时间、截图数量、事件目录路径）
7. THE event_timeout_sec SHALL 通过 config.toml 的 `[ai].event_timeout_sec` 配置，默认值为 15
8. THE JPEG 编码 SHALL 使用轻量级库（如 stb_image_write.h，header-only，无额外依赖），质量 85%
9. THE AiPipelineHandler SHALL 在截图保存失败时记录 warn 日志但不中断推理，不终止事件
10. THE AiPipelineHandler SHALL 在事件开启时记录 info 日志（事件目录路径 + 触发类别 + 置信度），事件结束时记录 info 日志（持续时间 + 截图数量）
11. THE events 目录及其子目录 SHALL 加入 .gitignore 排除
12. WHEN 推理线程停止（stop）时，IF 有活跃事件，THEN THE AiPipelineHandler SHALL 立即结束该事件
13. THE AiPipelineHandler SHALL 在事件目录中维护 `event.json` 元数据文件，包含以下字段：
    - `event_id`：事件目录名（如 `evt_20260412_153045`）
    - `device_id`：设备标识，从 config.toml 的 `[aws].thing_name` 获取
    - `start_time`：事件开始时间（ISO 8601 格式，UTC）
    - `end_time`：事件结束时间（事件关闭时写入）
    - `status`：`"active"` 或 `"closed"`
    - `frame_count`：截图总数
    - `detections_summary`：按类别汇总的检测统计（每个类别的检测次数和最高置信度）
14. WHEN 事件开启时，THE AiPipelineHandler SHALL 在内存中初始化事件状态（event_id、start_time、device_id），不创建目录、不写盘
15. WHEN 事件结束时，THE AiPipelineHandler SHALL 批量写盘：创建事件目录 → 写入所有缓存的 JPEG 文件 → 写入 `event.json`（`status: "closed"`、`end_time`、`frame_count`、`detections_summary`）→ 清空内存缓存。detections_summary 在事件期间于内存中累积
16. THE Spec 11 S3 uploader SHALL 仅上传 `event.json` 中 `status` 为 `"closed"` 的事件目录（本 Spec 不实现上传，仅定义契约）
17. THE JPEG 编码 SHALL 在推理线程中同步执行（~15ms/帧），编码后的 JPEG 数据缓存到内存（~50-80KB/帧），不做磁盘 I/O。IF 推理 + JPEG 编码的总耗时超过抽帧间隔（1000/inference_fps ms），THEN 下一帧将被自动跳过（推理线程 busy flag 机制保证），不会导致帧积压
18. THE 事件期间内存缓存 SHALL 受 `max_cache_mb` 配置限制（默认 16MB，通过 config.toml `[ai].max_cache_mb` 配置）。WHEN 缓存的 JPEG 数据总大小达到 max_cache_mb 时，THE AiPipelineHandler SHALL 执行一次中间 flush：创建事件目录（如果尚未创建）→ 将当前缓存的所有 JPEG 写盘 → 清空内存缓存 → 事件继续（不关闭）。后续帧继续缓存到内存，直到下次达到上限或事件关闭
19. THE 中间 flush 写盘操作 SHALL 在推理线程中同步执行（此时推理线程 busy，probe 自动跳帧），flush 完成后恢复正常推理节奏

### 需求 6：build_tee_pipeline 扩展

**用户故事：** 作为开发者，我需要扩展 build_tee_pipeline 函数以支持 AI 管道集成，同时保持向后兼容。

#### 验收标准

1. THE build_tee_pipeline 函数 SHALL 新增可选参数 `AiPipelineHandler* ai_handler = nullptr`
2. WHEN ai_handler 为非空时，THE build_tee_pipeline SHALL 在管道构建完成后调用 `ai_handler->install_probe(pipeline)` 安装 buffer probe
3. WHEN ai_handler 为 nullptr 时，THE build_tee_pipeline SHALL 保持现有行为（AI 分支使用 fakesink，无 probe）
4. THE AI 分支的管道拓扑 SHALL 保持不变：raw-tee → q-ai（leaky=downstream, max-size-buffers=1）→ fakesink("ai-sink")，probe 安装在 q-ai 的 src pad 上
5. THE build_tee_pipeline 的现有参数和行为 SHALL 保持完全向后兼容

### 需求 7：AppContext 集成

**用户故事：** 作为开发者，我需要将 AiPipelineHandler 集成到 AppContext 生命周期中，以便 AI 推理管道随应用启动和停止。

#### 验收标准

1. THE AppContext::Impl SHALL 新增 `std::unique_ptr<AiPipelineHandler>` 成员
2. WHEN config.toml 中配置了 YOLO 模型路径且模型文件存在时，THE AppContext::init() SHALL 创建 YoloDetector 和 AiPipelineHandler
3. IF YOLO 模型路径未配置或模型文件不存在，THEN THE AppContext::init() SHALL 跳过 AI 管道创建，记录 info 日志，不视为错误
4. THE AppContext::start() SHALL 将 AiPipelineHandler 指针传入 build_tee_pipeline，并在管道启动后调用 `ai_handler->start()`
5. THE AppContext::stop() SHALL 通过 ShutdownHandler 在管道停止前先停止 AiPipelineHandler
6. THE 健康监控的 rebuild 回调 SHALL 在重建管道后重新安装 AI probe：rebuild 回调中先调用 `build_tee_pipeline(... ai_handler)` 构建新管道（install_probe 在 build_tee_pipeline 内部调用），然后调用 `ai_handler->start()` 重启推理线程
7. THE AiPipelineHandler SHALL 在 `install_probe()` 被再次调用前自动移除旧 probe（如果存在），以支持管道重建场景

### 需求 8：配置文件扩展

**用户故事：** 作为开发者，我需要通过 config.toml 配置 AI 管道参数，以便在不重新编译的情况下调整推理行为。

#### 验收标准

1. THE config.toml SHALL 新增 `[ai]` 配置段，包含以下字段：`model_path`（ONNX 模型文件路径）、`inference_fps`（推理抽帧率，每秒抽取帧数，默认 2，即每秒推理 2 帧，内部转换为间隔 500ms）、`confidence_threshold`（全局默认置信度阈值，默认 0.25）、`snapshot_dir`（事件截图保存根目录，默认 `device/events/`）、`event_timeout_sec`（事件超时秒数，默认 15）、`target_classes`（需要探测的目标类别配置，支持 per-class 置信度覆盖）、`max_cache_mb`（事件 JPEG 内存缓存上限 MB，默认 16，达到上限时中间 flush 写盘）
2. THE `target_classes` 配置 SHALL 为逗号分隔的字符串格式：`name[:confidence]`（例如 `"bird:0.3,person:0.5,cat,dog"`）。未指定 confidence 的类别使用全局 confidence_threshold。空字符串表示所有类别都使用全局阈值
3. WHEN `target_classes` 为空或未配置时，THE AiPipelineHandler SHALL 对所有 COCO 80 类使用全局 confidence_threshold
4. WHEN `model_path` 为空字符串时，THE AppContext SHALL 跳过 AI 管道创建
5. THE `inference_fps` SHALL 为 1-30 范围内的正整数，超出范围时 ConfigManager 返回错误。设为 0 会导致除零，必须拒绝
6. THE `event_timeout_sec` SHALL 为 ≥ 3 的正整数（最小 3 秒，保证超时检测精度在推理间隔的 2 倍以上），超出范围时使用默认值 15 并记录 warn 日志

### 需求 9：单元测试

**用户故事：** 作为开发者，我需要通过单元测试验证 AI 管道集成的正确性。

#### 验收标准

1. THE Test_Suite SHALL 包含 I420 到 RGB 转换的 example-based 测试：纯黑帧（Y=0, U=128, V=128 → R≈0, G≈0, B≈0）、纯白帧（Y=255, U=128, V=128 → R≈255, G≈255, B≈255）
2. THE Test_Suite SHALL 包含 I420 到 RGB 转换的 PBT 属性测试：输出缓冲区大小恒等于 width × height × 3、输出像素值 clamp 到 [0, 255]
3. THE Test_Suite SHALL 包含抽帧节流逻辑的测试：间隔内连续调用只触发一次抽帧
4. THE Test_Suite SHALL 保持现有测试（smoke_test、log_test、tee_test、camera_test、health_test、yolo_test 等）全部通过
5. WHEN 在 macOS Debug（ASan）构建下运行时，THE Test_Suite SHALL 无内存错误报告

### 需求 10：双平台构建验证

**用户故事：** 作为开发者，我需要确保 AI 管道集成在 macOS 和 Pi 5 上均能成功编译和测试。

#### 验收标准

1. WHEN 在 macOS 上执行构建和测试时，THE Build_System SHALL 编译成功且所有测试通过
2. WHEN 在 Pi 5 上执行构建和测试时，THE Build_System SHALL 编译成功且所有测试通过
3. WHEN YOLO 模块未启用（`-DENABLE_YOLO=OFF`）时，THE Build_System SHALL 通过 CMake 条件编译完全跳过 AiPipelineHandler 和 ai_pipeline_test 的编译，AI 分支保持 fakesink 行为，AppContext 中不创建 AiPipelineHandler
4. THE Build_System SHALL 保持现有所有测试行为不变
5. THE AiPipelineHandler 源文件 SHALL 仅在 `ENABLE_YOLO=ON` 且 `OnnxRuntime_FOUND` 时编译（与 yolo_module 相同的条件编译策略）

## 参考代码

### GStreamer Buffer Probe 安装

```cpp
// 在 q-ai 的 src pad 上安装 buffer probe
GstElement* q_ai = gst_bin_get_by_name(GST_BIN(pipeline), "q-ai");
GstPad* src_pad = gst_element_get_static_pad(q_ai, "src");
gulong probe_id = gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                     buffer_probe_cb, user_data, nullptr);
gst_object_unref(src_pad);
gst_object_unref(q_ai);
```

### I420 到 RGB 转换（BT.601 整数定点）

```cpp
// I420 平面布局：Y 平面 (w*h) + U 平面 (w/2 * h/2) + V 平面 (w/2 * h/2)
// 整数定点运算，避免浮点乘法，优化 ARM 性能
void i420_to_rgb(const uint8_t* y_plane, const uint8_t* u_plane, const uint8_t* v_plane,
                 int width, int height, int y_stride, int uv_stride,
                 uint8_t* rgb_out) {
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            int y_val = y_plane[row * y_stride + col];
            int u_val = u_plane[(row / 2) * uv_stride + (col / 2)] - 128;
            int v_val = v_plane[(row / 2) * uv_stride + (col / 2)] - 128;
            // BT.601 定点: R = Y + 1.402*V ≈ Y + (359*V)>>8
            //              G = Y - 0.344*U - 0.714*V ≈ Y - (88*U + 183*V)>>8
            //              B = Y + 1.772*U ≈ Y + (454*U)>>8
            int r = y_val + ((359 * v_val) >> 8);
            int g = y_val - ((88 * u_val + 183 * v_val) >> 8);
            int b = y_val + ((454 * u_val) >> 8);
            int idx = (row * width + col) * 3;
            rgb_out[idx + 0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            rgb_out[idx + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            rgb_out[idx + 2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
        }
    }
}
```

### 从 pad caps 获取帧宽高（不依赖 GstVideoMeta）

```cpp
// 在 probe 回调中获取帧宽高
static bool get_frame_dimensions(GstPad* pad, int& width, int& height) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) return false;
    GstStructure* s = gst_caps_get_structure(caps, 0);
    bool ok = gst_structure_get_int(s, "width", &width) &&
              gst_structure_get_int(s, "height", &height);
    gst_caps_unref(caps);
    return ok;
}
```

### 异步推理线程模式

```cpp
// 推理线程主循环
void inference_loop() {
    while (!stop_flag_) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return frame_ready_ || stop_flag_; });
        if (stop_flag_) break;
        // 交换帧缓冲区（避免拷贝）
        auto frame = std::move(pending_frame_);
        frame_ready_ = false;
        busy_ = true;
        lock.unlock();
        // 执行推理（不持锁）
        auto [detections, stats] = detector_->detect_with_stats(
            frame.data(), frame_width_, frame_height_);
        // 回调通知（含 RGB 帧数据）
        if (!detections.empty() && detection_cb_) {
            detection_cb_(detections, stats, frame.data(), frame_width_, frame_height_);
        }
        std::lock_guard<std::mutex> lg(mutex_);
        busy_ = false;
    }
}
```

## 验证命令

```bash
# macOS Debug 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# Pi 5 Release 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# 跳过 YOLO 模块
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug -DENABLE_YOLO=OFF && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```

预期结果：两个平台均编译成功、所有测试通过（macOS 下 ASan 无报告）。YOLO 未启用时 AI 分支保持 fakesink 行为。

## 明确不包含

- 截图上传 S3（Spec 11: screenshot-uploader，本 Spec 只负责保存到本地磁盘）
- 检测结果持久化存储或数据库写入（Spec 11+）
- 多模型并行推理或模型热切换
- GPU/NPU 加速
- 云端推理集成（Spec 17: sagemaker-endpoint）
- 前端检测结果展示（Spec 21: viewer-mvp）
- AI 分支的管道拓扑变更（保持 raw-tee → q-ai → fakesink，仅加 probe）
- 推理结果的 MQTT/IoT 上报
- 视频叠加检测框（OSD overlay）
- 截图文件的自动清理/轮转（Spec 11 上传成功后删除）
