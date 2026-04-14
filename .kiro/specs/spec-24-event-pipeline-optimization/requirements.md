# 需求文档：Spec 24 — 事件管道优化

## 前置条件

- Spec 10 (ai-pipeline) 已通过验证 ✅
- Spec 11 (s3-uploader) 已通过验证 ✅
- Spec 23 (log-management) 已通过验证 ✅

## 简介

事件管道端到端优化，包含以下改进点：
1. AI 推理自适应频率控制 — idle 时降低推理频率节省 CPU，检测到目标后自动提升
2. S3 事件驱动上传 — 事件关闭后立即通知 S3Uploader 上传，消除轮询延迟
3. S3 上传顺序优化 — 先上传截图后上传 event.json，确保云端 Lambda 触发时所有文件就绪
4. 智能截图选择 — 每秒选置信度最高帧，Top-K 缓存替换，减少上传量
5. 两阶段事件确认 — 连续 3 次检测才确认事件，过滤瞬态误检

## 术语表

- **AiPipelineHandler**: AI 推理管道处理器，负责 GStreamer buffer probe 抽帧、YOLO 推理、事件生命周期管理（`device/src/ai_pipeline_handler.h`）
- **S3Uploader**: S3 截图上传器，后台线程轮询扫描已关闭事件并上传到 S3（`device/src/s3_uploader.h`）
- **ConfigManager**: 统一配置管理器，解析 config.toml 各 section（`device/src/config_manager.h`）
- **AppContext**: 应用上下文，三阶段生命周期管理，负责模块创建和回调连接（`device/src/app_context.h`）
- **Idle_Mode**: AI 推理空闲模式，无活跃事件时以 idle_fps 频率采样
- **Active_Mode**: AI 推理活跃模式，检测到目标后以 active_fps 频率采样
- **Pending_Event**: 准事件，首次检测到目标后进入，尚未确认
- **Confirmed_Event**: 确认事件，连续 3 次检测后升级，允许写盘和上传

## 需求

### 需求 1：自适应推理频率 — 配置解析

**用户故事：** 作为开发者，我希望在 config.toml 的 [ai] section 中配置 idle_fps 和 active_fps，以便推理频率能根据检测状态自适应调整。

#### 验收标准

1. WHEN config.toml [ai] section 包含 `idle_fps` 字段，THE ConfigManager SHALL 将其解析为整数并存入 AiConfig
2. WHEN config.toml [ai] section 包含 `active_fps` 字段，THE ConfigManager SHALL 将其解析为整数并存入 AiConfig
3. WHEN `idle_fps` 缺失，THE ConfigManager SHALL 使用默认值 1
4. WHEN `active_fps` 缺失，THE ConfigManager SHALL 使用默认值 3
5. WHEN `idle_fps` 值小于 1 或大于 10，THE ConfigManager SHALL 返回 false 并附带描述性错误信息
6. WHEN `active_fps` 值小于 1 或大于 30，THE ConfigManager SHALL 返回 false 并附带描述性错误信息
7. WHEN `idle_fps` 值大于等于 `active_fps` 值，THE ConfigManager SHALL 返回 false 并提示 idle_fps 必须小于 active_fps
8. WHEN `idle_fps` 和 `active_fps` 同时存在时，THE ConfigManager SHALL 忽略旧的 `inference_fps` 字段
9. WHEN 仅存在 `inference_fps`（无 `idle_fps` 和 `active_fps`），THE ConfigManager SHALL 将 `inference_fps` 作为 `active_fps`，`idle_fps` 设为默认值 1（向后兼容）

### 需求 2：自适应推理频率 — 运行时状态切换

**用户故事：** 作为运维人员，我希望 AI 管道在无目标时自动降低推理频率，以便将空闲期 CPU 消耗减半。

#### 验收标准

1. WHEN AiPipelineHandler 启动且未检测到目标，THE AiPipelineHandler SHALL 以 idle_fps 频率（默认 1fps）采样
2. WHEN AiPipelineHandler 检测到目标（filtered detections 非空），THE AiPipelineHandler SHALL 在下一个采样周期切换到 active_fps 频率（默认 3fps）
3. WHILE AiPipelineHandler 处于 Active_Mode 且持续检测到目标，THE AiPipelineHandler SHALL 保持 active_fps 采样频率
4. WHEN event_timeout_sec 超时（自上次检测起），THE AiPipelineHandler SHALL 关闭事件后切回 idle_fps
5. THE AiPipelineHandler SHALL 使用单个原子变量跟踪当前 fps 模式（idle 或 active）
6. THE AiPipelineHandler SHALL 在 fps 模式切换时以 info 级别记录日志，格式：`Inference fps: {old_fps} -> {new_fps}, reason={reason}`

### 需求 3：S3 事件驱动上传通知

**用户故事：** 作为运维人员，我希望事件关闭后 S3 上传立即开始，以便事件数据无需等待下一次轮询周期（最长 30 秒）即可到达云端。

#### 验收标准

1. THE S3Uploader SHALL 暴露公共方法 `notify_upload()`，立即唤醒扫描线程
2. WHEN 调用 `notify_upload()` 时，THE S3Uploader SHALL 通过 condition_variable notify_one 唤醒扫描线程，不获取额外锁
3. WHEN 调用 `notify_upload()` 时扫描线程正在处理上传，THE S3Uploader SHALL 在当前上传完成后处理通知
4. THE S3Uploader SHALL 保留现有的 scan_interval_sec 周期轮询作为崩溃恢复的兜底机制
5. WHEN 在 `stop()` 之后调用 `notify_upload()`，THE S3Uploader SHALL 忽略通知，不报错

### 需求 4：事件关闭回调连接

**用户故事：** 作为开发者，我希望 AiPipelineHandler 在事件关闭时通知 S3Uploader，以便自动触发事件驱动上传。

#### 验收标准

1. THE AiPipelineHandler SHALL 支持通过 `set_event_close_callback(std::function<void()> cb)` 注册事件关闭回调
2. WHEN AiPipelineHandler::close_event() 成功完成磁盘写入，THE AiPipelineHandler SHALL 调用已注册的事件关闭回调
3. IF close_event() 磁盘写入失败，THEN THE AiPipelineHandler SHALL 跳过回调调用并记录 warning 日志
4. WHEN 未注册事件关闭回调，THE AiPipelineHandler SHALL 正常继续，不报错（回调可选）
5. THE AppContext SHALL 在 init 阶段将 AiPipelineHandler 的事件关闭回调连接到 S3Uploader::notify_upload()

### 需求 5：S3 上传顺序优化

**用户故事：** 作为云端开发者，我希望 event.json 最后上传，以便 Lambda 被 S3 事件触发时所有截图文件已就绪。

#### 验收标准

1. WHEN S3Uploader::upload_event() 上传事件目录中的文件，THE S3Uploader SHALL 先上传所有 .jpg 文件，最后上传 event.json
2. IF 任何 .jpg 文件上传失败，THEN THE S3Uploader SHALL 中止事件上传并返回 false，不上传 event.json
3. THE S3Uploader SHALL 按文件名字典序上传 .jpg 文件（基于时间戳的文件名确保时间顺序）
4. WHEN 事件目录仅包含 event.json 而无 .jpg 文件，THE S3Uploader SHALL 正常上传 event.json

### 需求 6：智能截图选择 — 每秒最佳帧 + Top-K 替换

**用户故事：** 作为云端开发者，我希望设备只上传每个事件中质量最高的截图，以便最小化存储成本和上传带宽，同时确保最佳图片到达云端用于种类识别。

#### 验收标准

1. THE AiPipelineHandler SHALL 维护 1 秒滑动窗口；每个窗口内仅选择最高检测置信度的帧作为候选截图
2. WHEN 1 秒窗口结束且有候选帧，THE AiPipelineHandler SHALL 将候选帧的置信度与现有截图缓存（按置信度排序的 Top-K 队列）比较
3. IF 截图缓存条目数少于 `max_snapshots_per_event`，THE AiPipelineHandler SHALL 将候选帧加入缓存
4. IF 截图缓存已满且候选帧置信度高于缓存中最低置信度条目，THE AiPipelineHandler SHALL 用候选帧替换最低置信度条目
5. IF 截图缓存已满且候选帧置信度小于等于最低置信度条目，THE AiPipelineHandler SHALL 丢弃候选帧
6. THE `max_snapshots_per_event` SHALL 通过 config.toml [ai] section 配置，默认值 10
7. WHEN 调用 close_event() 时，THE AiPipelineHandler SHALL 仅对缓存中的截图进行 JPEG 编码并按时间戳排序写盘
8. THE AiPipelineHandler SHALL NOT 对每帧推理都进行 JPEG 编码 — JPEG 编码仅在候选帧提交到缓存时（替换或新增）进行
9. THE event.json 的 `frame_count` 字段 SHALL 反映实际写盘的截图数量（关闭时缓存大小），而非总推理次数
10. WHEN 调用 close_event() 时 1 秒窗口仍有未提交的候选帧，THE AiPipelineHandler SHALL 在写盘前将该候选帧提交到缓存

### 需求 7：两阶段事件确认 — 准事件与确认事件

**用户故事：** 作为运维人员，我希望瞬态误检（如阴影闪烁、树叶晃动）在写盘前被过滤掉，以便只有持续的真实检测才产生事件和 S3 上传。

#### 验收标准

1. WHEN AiPipelineHandler 首次检测到目标（无活跃事件），THE AiPipelineHandler SHALL 进入"准事件"状态，切换到 active_fps，开始收集候选帧到内存 Top-K 缓存
2. WHILE 处于"准事件"状态，THE AiPipelineHandler SHALL 维护连续检测计数器，每帧有 filtered detections 时递增
3. WHEN 连续检测计数器达到 3（硬编码），THE AiPipelineHandler SHALL 将准事件升级为"确认事件"，以 info 级别记录：`Event confirmed: event_id={id}, after {N} consecutive detections`
4. WHEN 准事件状态下某帧推理结果为零 filtered detections，THE AiPipelineHandler SHALL 重置连续检测计数器为 0，丢弃内存缓存，切回 idle_fps，以 debug 级别记录：`Pending event discarded: detection interrupted`
5. 仅确认事件 SHALL 在 close_event() 时写盘；未达到 3 次连续检测的准事件 SHALL NOT 产生任何磁盘 I/O 或 S3 上传
6. THE detection_callback（AppContext 用于外部通知）SHALL 仅对确认事件调用，不对准事件调用
7. THE event.json 中的 detections_summary SHALL 包含确认事件的所有检测结果（从触发准事件的首次检测开始），而非仅从确认点开始

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| 目标平台 | macOS（开发）/ Linux aarch64（Pi 5 生产） |
| 硬件限制 | Raspberry Pi 5，4GB RAM |
| 新增依赖 | 无 |
| 日志输出语言 | 英文 |
| idle_fps 默认值 | 1 |
| active_fps 默认值 | 3 |
| max_snapshots_per_event 默认值 | 10 |
| 连续检测确认阈值 | 3（硬编码） |
| 截图窗口 | 1 秒（硬编码） |

## 禁止项

### Requirements 层

- SHALL NOT 实现 config.toml 热重载（仅启动时加载）
- SHALL NOT 实现 S3 多线程并行上传

### Design 层

- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
- SHALL NOT 仅基于单独推理基线测试结论选择生产配置（满负载下 CPU 争抢会改变最优配置）

### Tasks 层

- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 直接运行测试可执行文件，统一通过 `ctest --test-dir device/build --output-on-failure` 运行
- SHALL NOT 在日志消息中使用非 ASCII 字符
- SHALL NOT 修改现有模块的核心逻辑（仅修改事件管道相关代码）

## 明确不包含

- AI 推理模型切换或热更新
- S3 上传并发（多线程并行上传多个事件）
- 云端 Lambda 触发逻辑（后续 Spec 范围）
- 推理性能优化（Spec 9.5 已完成）
- config.toml 热重载
