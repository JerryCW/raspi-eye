# 实现计划：Spec 24 — 事件管道优化

## 概述

按依赖顺序实现五个核心改进：先扩展 AiConfig 和配置解析支持自适应 fps + max_snapshots_per_event，再实现 S3Uploader 的 notify_upload() 和上传顺序优化，然后重构 AiPipelineHandler 支持两阶段事件确认 + 自适应 fps + 智能截图选择（Top-K min-heap），最后在 AppContext 中连接事件关闭回调，更新配置文件。

## 任务

- [x] 1. 扩展 AiConfig 和配置解析
  - [x] 1.1 在 AiConfig 中新增 idle_fps / active_fps / max_snapshots_per_event 字段
    - 修改 `device/src/ai_pipeline_handler.h`：在 `AiConfig` 结构体中新增三个字段
    - `int idle_fps = 1;`（空闲模式采样率，1-10）
    - `int active_fps = 3;`（活跃模式采样率，1-30）
    - `int max_snapshots_per_event = 10;`（Top-K 缓存大小）
    - _需求: 1.1, 1.2, 6.6_

  - [x] 1.2 扩展 parse_ai_config 解析新字段 + 向后兼容
    - 修改 `device/src/config_manager.cpp`：在 `parse_ai_config` 中新增解析逻辑
    - 解析 `idle_fps`（缺失默认 1）、`active_fps`（缺失默认 3）、`max_snapshots_per_event`（缺失默认 10，< 1 使用默认值 + warn 日志）
    - idle_fps 范围 [1,10]，超出返回 false + error_msg
    - active_fps 范围 [1,30]，超出返回 false + error_msg
    - idle_fps >= active_fps 返回 false + error_msg
    - 向后兼容：同时存在 idle_fps + active_fps → 忽略 inference_fps；仅存在 inference_fps → active_fps = inference_fps, idle_fps = 1
    - _需求: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 6.6_

  - [x] 1.3 编写 fps 配置解析的 Example-based 单元测试
    - 在 `device/tests/config_test.cpp` 中新增以下测试：
    - `ParseAiConfig_IdleFps_Default`：缺失 idle_fps → AiConfig.idle_fps == 1
    - `ParseAiConfig_ActiveFps_Default`：缺失 active_fps → AiConfig.active_fps == 3
    - `ParseAiConfig_BackwardCompat_OnlyInferenceFps`：仅 inference_fps=5 → active_fps=5, idle_fps=1
    - `ParseAiConfig_BackwardCompat_AllThreeFields`：idle_fps + active_fps + inference_fps → 忽略 inference_fps
    - `ParseAiConfig_MaxSnapshotsDefault`：缺失 → 默认 10
    - `ParseAiConfig_MaxSnapshotsBelowOne`：值 < 1 → 使用默认 10
    - `ParseAiConfig_IdleFpsOutOfRange`：idle_fps=0 或 11 → 返回 false
    - `ParseAiConfig_ActiveFpsOutOfRange`：active_fps=0 或 31 → 返回 false
    - `ParseAiConfig_IdleGteActive`：idle_fps=3, active_fps=3 → 返回 false
    - _需求: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9_

  - [x] 1.4 编写 Property 1 测试：FPS 配置解析正确性
    - **Property 1: FPS 配置解析正确性**
    - 生成器：随机 idle_fps ∈ [1,10]，随机 active_fps ∈ [idle_fps+1, 30]
    - 断言：parse_ai_config 返回 true，AiConfig.idle_fps 和 active_fps 与输入一致
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 1.1, 1.2**

  - [x] 1.5 编写 Property 2 测试：FPS 范围验证
    - **Property 2: FPS 范围验证**
    - 生成器：随机 idle_fps ∈ {<1, >10}，随机 active_fps ∈ {<1, >30}
    - 断言：parse_ai_config 返回 false 且 error_msg 非空
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 1.5, 1.6**

  - [x] 1.6 编写 Property 3 测试：idle_fps < active_fps 交叉验证
    - **Property 3: idle_fps < active_fps 交叉验证**
    - 生成器：随机 (idle, active) 对，idle >= active，两者均在各自有效范围内
    - 断言：parse_ai_config 返回 false 且 error_msg 包含描述性信息
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 1.7**

- [x] 2. 检查点 - 配置解析编译与测试验证
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认编译和测试通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [x] 3. S3Uploader 事件驱动上传 + 上传顺序优化
  - [x] 3.1 实现 S3Uploader::notify_upload()
    - 修改 `device/src/s3_uploader.h`：在 S3Uploader 类中新增 `void notify_upload()` 公共方法声明
    - 修改 `device/src/s3_uploader.cpp`：实现 notify_upload()，直接调用 `cv_.notify_one()` 唤醒扫描线程，不获取额外锁
    - stop() 后调用无效果（cv_.notify_one() 无接收者，安全忽略）
    - 保留现有 scan_interval_sec 周期轮询作为兜底
    - _需求: 3.1, 3.2, 3.3, 3.4, 3.5_

  - [x] 3.2 重构 upload_event() 支持 .jpg 优先上传
    - 修改 `device/src/s3_uploader.cpp`：重构 `upload_event()` 方法
    - 收集事件目录中的文件，分为 jpg_files 和 event_json_file
    - jpg_files 按文件名字典序排序后依次上传
    - 任何 .jpg 上传失败立即返回 false，不上传 event.json
    - 所有 .jpg 上传成功后最后上传 event.json
    - 仅有 event.json 无 .jpg 时正常上传
    - _需求: 5.1, 5.2, 5.3, 5.4_

  - [x] 3.3 编写 S3 上传顺序和 notify_upload 的 Example-based 单元测试
    - 在 `device/tests/s3_test.cpp` 中新增以下测试：
    - `UploadEvent_JpgBeforeJson`：mock put 记录上传顺序，验证 .jpg 在 event.json 之前
    - `UploadEvent_JpgSorted`：多个 .jpg 按字典序上传
    - `UploadEvent_JpgFailAborts`：某 .jpg 失败后 event.json 未上传
    - `UploadEvent_OnlyJson`：仅 event.json 时正常上传
    - `NotifyUpload_WakesScanThread`：notify_upload() 唤醒等待中的扫描线程
    - _需求: 3.1, 5.1, 5.2, 5.3, 5.4_

  - [x] 3.4 编写 Property 4 测试：S3 上传顺序不变量
    - **Property 4: S3 上传顺序不变量**
    - 生成器：随机生成 0-20 个 .jpg 文件名 + event.json，mock put 记录上传顺序
    - 断言：所有 .jpg 按字典序排列在 event.json 之前
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 5.1, 5.3**

  - [x] 3.5 编写 Property 5 测试：S3 上传失败中止
    - **Property 5: S3 上传失败中止**
    - 生成器：随机生成 1-10 个 .jpg 文件名 + event.json，随机选择一个 .jpg 失败位置
    - 断言：upload_event 返回 false，event.json 未被上传，失败位置之后的 .jpg 也未上传
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 5.2**

- [x] 4. 检查点 - S3 改动编译与测试验证
  - 运行 `cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认编译和测试通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [x] 5. AiPipelineHandler 核心重构 — 事件状态机 + 自适应 fps
  - [x] 5.1 新增 EventState 枚举和状态机相关成员
    - 修改 `device/src/ai_pipeline_handler.h`：
    - 新增 `enum class EventState { IDLE, PENDING, CONFIRMED, CLOSING };`
    - 新增成员：`EventState event_state_`、`int consecutive_detection_count_`、`static constexpr int kConfirmationThreshold = 3;`
    - 新增 `std::atomic<int> current_fps_;` 替代原有 `frame_interval_ms_` 的硬编码逻辑
    - 新增 `using EventCloseCallback = std::function<void()>;` 和 `void set_event_close_callback(EventCloseCallback cb);`
    - 新增 `EventCloseCallback event_close_cb_;` 成员
    - _需求: 2.1, 2.5, 4.1, 7.1, 7.2, 7.3_

  - [x] 5.2 实现自适应 fps 切换逻辑
    - 修改 `device/src/ai_pipeline_handler.cpp`：
    - create() 中初始化 `current_fps_` 为 `config.idle_fps`
    - buffer_probe_cb 中使用 `should_sample(elapsed, current_fps_.load())` 替代固定 frame_interval_ms_
    - 检测到目标且 state==IDLE → 切换 current_fps_ 为 active_fps，记录 info 日志：`Inference fps: {idle} -> {active}, reason=detection`
    - 事件超时关闭后 → 切换 current_fps_ 为 idle_fps，记录 info 日志：`Inference fps: {active} -> {idle}, reason=timeout`
    - 准事件中断 → 切换 current_fps_ 为 idle_fps，记录 debug 日志：`Pending event discarded: detection interrupted`
    - _需求: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_

  - [x] 5.3 实现两阶段事件确认逻辑
    - 修改 `device/src/ai_pipeline_handler.cpp`：重构 inference_loop 中的事件管理
    - IDLE + 检测到目标 → 进入 PENDING，consecutive_count=1，切换到 active_fps
    - PENDING + 检测到目标 → consecutive_count++；达到 3 → 升级为 CONFIRMED，记录 info 日志：`Event confirmed: event_id={id}, after {N} consecutive detections`
    - PENDING + 无检测 → 重置 consecutive_count=0，丢弃内存缓存，切回 idle_fps
    - CONFIRMED + 超时 → 进入 CLOSING → 写盘 → 回到 IDLE
    - detection_callback 仅对 CONFIRMED 事件调用
    - detections_summary 从首次检测（PENDING 阶段）开始累积
    - _需求: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7_

  - [x] 5.4 实现事件关闭回调调用
    - 修改 `device/src/ai_pipeline_handler.cpp`：
    - 在 close_event() 磁盘写入成功后调用 `event_close_cb_`（如已注册）
    - 磁盘写入失败时跳过回调，记录 warning 日志
    - 未注册回调时正常继续
    - _需求: 4.1, 4.2, 4.3, 4.4_

- [x] 6. 检查点 - 事件状态机编译验证
  - 运行 `cmake --build device/build` 确认编译通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [x] 7. 智能截图选择 — 1 秒窗口 + Top-K min-heap
  - [x] 7.1 新增 WindowCandidate 和 SnapshotEntry 结构体 + Top-K 纯函数
    - 修改 `device/src/ai_pipeline_handler.h`：
    - 新增 `WindowCandidate` 结构体（rgb_data, width, height, confidence, timestamp）
    - 新增 `SnapshotEntry` 结构体（filename, jpeg_data, confidence, timestamp）
    - 新增 `SnapshotMinHeapCmp` 比较器（min-heap，最低置信度在堆顶）
    - 新增独立纯函数声明 `bool try_submit_to_topk(...)`
    - 新增成员：`std::optional<WindowCandidate> window_candidate_`、`std::chrono::steady_clock::time_point window_start_`、`std::vector<SnapshotEntry> snapshot_heap_`
    - _需求: 6.1, 6.2, 6.3, 6.4, 6.5_

  - [x] 7.2 实现 1 秒滑动窗口最佳帧选择
    - 修改 `device/src/ai_pipeline_handler.cpp`：
    - 每帧推理后，取最高检测置信度与当前窗口候选帧比较
    - 同一 1 秒窗口内：置信度更高则替换候选帧（保留 RGB 原始数据），否则丢弃
    - 窗口结束时：对候选帧进行 JPEG 编码，尝试提交到 Top-K 缓存
    - close_event() 时：flush 未提交的窗口候选帧到 Top-K 缓存
    - _需求: 6.1, 6.8, 6.10_

  - [x] 7.3 实现 Top-K min-heap 缓存和 try_submit_to_topk 纯函数
    - 修改 `device/src/ai_pipeline_handler.cpp`：
    - 实现 `try_submit_to_topk()`：缓存未满 → push_heap 加入；已满且候选 > 最低 → pop_heap + push_heap 替换；已满且候选 ≤ 最低 → 丢弃
    - close_event() 时：按 timestamp 排序写盘，frame_count = 实际写盘截图数
    - JPEG 编码仅在候选帧提交到缓存时进行（新增或替换），不对每帧推理编码
    - _需求: 6.2, 6.3, 6.4, 6.5, 6.7, 6.8, 6.9_

  - [x] 7.4 编写 Property 6 测试：1 秒窗口最佳帧选择
    - **Property 6: 1 秒窗口最佳帧选择**
    - 生成器：随机生成 1-30 个 (timestamp, confidence) 对，所有 timestamp 在同一 1 秒窗口内
    - 断言：窗口结束时选出的候选帧具有最高置信度
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 6.1**

  - [x] 7.5 编写 Property 7 测试：Top-K 缓存不变量
    - **Property 7: Top-K 缓存不变量**
    - 生成器：随机 K ∈ [1,20]，随机操作序列（插入 0-50 个候选，每个候选有随机置信度）
    - 断言：操作后缓存大小 ≤ K；缓存中的条目是所有已提交候选中置信度最高的 min(N, K) 个
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 6.3, 6.4, 6.5**

  - [x] 7.6 编写 Property 8 测试：frame_count 一致性
    - **Property 8: frame_count 一致性**
    - 生成器：随机生成 Top-K 缓存内容（1-20 个 SnapshotEntry），模拟 close_event 写盘
    - 断言：event.json 中 frame_count 等于实际写入的 .jpg 文件数量
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 6.9**

- [x] 8. 检查点 - 智能截图编译与测试验证
  - 运行 `cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认编译和测试通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [x] 9. 事件状态机测试 + 准事件无副作用验证
  - [x] 9.1 编写两阶段事件确认的 Example-based 单元测试
    - 在 `device/tests/ai_pipeline_test.cpp` 中新增以下测试：
    - `EventState_PendingToConfirmed`：连续 3 次检测后状态升级为 CONFIRMED
    - `EventState_PendingInterrupted`：第 2 次检测后无检测 → 重置为 IDLE
    - `EventState_ConfirmedTimeout`：确认事件超时后正常关闭
    - `EventState_CloseCallbackCalled`：close_event 成功后回调被调用
    - `EventState_CloseCallbackNotRegistered`：未注册回调时正常关闭
    - `EventState_DetectionCallbackOnlyForConfirmed`：detection_callback 仅对确认事件调用
    - `EventState_SummaryIncludesPending`：detections_summary 包含准事件阶段的检测结果
    - `EventState_CloseFlushesWindowCandidate`：close_event 时 flush 未提交的窗口候选帧
    - _需求: 4.2, 4.3, 4.4, 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7, 6.10_

  - [x] 9.2 编写 Property 9 测试：准事件无副作用
    - **Property 9: 准事件无副作用**
    - 生成器：随机生成 1-2 次检测后中断的场景（检测帧数 < 3）
    - 断言：无磁盘 I/O（snapshot_dir 无新文件）、无 S3 上传、detection_callback 未被调用
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 7.4, 7.5, 7.6**

- [x] 10. AppContext 回调连接 + 配置文件更新
  - [x] 10.1 在 AppContext::init() 中连接事件关闭回调
    - 修改 `device/src/app_context.cpp`：在 ai_handler_ 和 s3_uploader_ 都创建成功后
    - `ai_handler_->set_event_close_callback([s3 = s3_uploader_]() { s3->notify_upload(); });`
    - 在 `#ifdef ENABLE_YOLO` 条件编译块内
    - _需求: 4.5_

  - [x] 10.2 更新 config.toml 和 config.toml.example
    - 修改 `device/config/config.toml`：在 [ai] section 新增 idle_fps / active_fps / max_snapshots_per_event 字段
    - 修改 `device/config/config.toml.example`：同步更新，添加注释说明各字段含义和取值范围
    - 注释掉旧的 `inference_fps` 字段并标注已废弃
    - _需求: 1.1, 1.2, 6.6_

- [x] 11. 最终检查点 - 全量编译与测试
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认全量编译和所有测试通过
  - 确认无 ASan 报告
  - 运行 `git status` 确认无敏感文件（.pem、.key 等）被跟踪
  - 不执行 git commit（由编排层统一执行）
  - 如有问题，询问用户

## 禁止项

- SHALL NOT 使用 `cat <<` heredoc 方式写入文件，使用 fsWrite / fsAppend 工具
- SHALL NOT 直接运行测试可执行文件，统一通过 `ctest --test-dir device/build --output-on-failure` 运行
- SHALL NOT 在日志消息中使用非 ASCII 字符
- SHALL NOT 在子代理中执行 git commit
- SHALL NOT 修改现有模块的核心逻辑（仅修改事件管道相关代码）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
- SHALL NOT 在不确定外部 SDK/库 API 用法时凭猜测编写代码

## 备注

- 标记 `*` 的子任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求编号，确保可追溯
- 检查点确保增量验证
- Property 测试验证纯函数的通用正确性属性（9 条），单元测试验证特定场景和边界条件
- 所有改动限定在 AiPipelineHandler、S3Uploader、ConfigManager、AppContext 四个文件 + 对应测试文件 + 配置文件
