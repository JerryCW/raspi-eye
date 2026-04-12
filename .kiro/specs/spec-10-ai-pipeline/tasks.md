# 实施计划：Spec 10 — AI 推理管道集成

## 概述

将 tee pipeline 的 AI 分支从 fakesink 占位替换为 buffer probe 抽帧 + YoloDetector 异步推理管道。核心新增类 `AiPipelineHandler` 封装 probe 安装、I420→RGB 转换、推理线程调度、target_classes 过滤、检测事件管理（JPEG 内存缓存 + 批量写盘）。按依赖顺序：stb_image_write.h 内嵌 → ai_pipeline_handler.h/cpp（独立函数优先）→ config_manager 扩展 → pipeline_builder 扩展 → app_context 集成 → CMakeLists.txt 更新 → ai_pipeline_test.cpp 测试 → config.toml + .gitignore → 双平台验证。实现语言为 C++17，包含 3 个 PBT 属性 + example-based 测试。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（使用 std::unique_ptr / std::make_unique）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 在不确定 GStreamer pad probe API 用法时凭猜测编写代码，必须参照设计文档中的参考代码
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、yolo_test 等）
- SHALL NOT 在 probe 回调中执行 YOLO 推理或分配大块堆内存
- SHALL NOT 让推理线程持有 GStreamer 对象的引用
- SHALL NOT 依赖 GstVideoMeta 获取帧宽高，必须使用 gst_pad_get_current_caps()
- SHALL NOT 在 I420→RGB 转换中使用浮点乘法（使用整数定点 BT.601）

## 任务

- [x] 1. 内嵌第三方库与独立函数实现
  - [x] 1.1 内嵌 `device/third_party/stb/stb_image_write.h`
    - 从 stb 仓库获取 `stb_image_write.h`（MIT 许可证）
    - 放置到 `device/third_party/stb/stb_image_write.h`
    - 仅在 `.cpp` 中定义 `STB_IMAGE_WRITE_IMPLEMENTATION`，避免多重定义
    - _需求：5.5.8_

  - [x] 1.2 创建 `device/src/ai_pipeline_handler.h` — 类声明与独立函数
    - 定义 `AiConfig` POD 结构体（model_path、inference_fps、confidence_threshold、snapshot_dir、event_timeout_sec、max_cache_mb、device_id、target_classes 向量）
    - 定义 `DetectionCallback` 类型别名
    - 声明独立函数 `i420_to_rgb()`（非类成员，便于 PBT 测试）
    - 声明独立函数 `filter_detections()`（纯函数，便于 PBT 测试）
    - 声明 `coco_class_name(int class_id)` 函数
    - 声明 `AiPipelineHandler` 类：工厂方法 create()、install_probe()、remove_probe()、start()、stop()、set_detection_callback()
    - 禁用拷贝构造和拷贝赋值
    - 参照设计文档中的完整类接口定义
    - _需求：1.1, 1.2, 1.3, 1.4, 2.1, 3.4, 4.4, 5.1, 5.2_

  - [x] 1.3 创建 `device/src/ai_pipeline_handler.cpp` — 独立函数部分
    - 实现 `coco_class_name()`：80 类 COCO 类名数组，class_id → name 映射
    - 实现 `i420_to_rgb()`：整数定点 BT.601 转换，输出 clamp [0, 255]，参照设计文档参考代码
    - 实现 `filter_detections()`：纯函数，按 target_classes 和 per-class/全局阈值过滤 Detection 向量
    - 实现 `should_sample()` 内部辅助函数：抽帧节流决策
    - _需求：3.2, 3.3, 3.4, 3.5, 4.4, 8.2, 8.3_

- [x] 2. AiPipelineHandler 核心实现
  - [x] 2.1 在 `device/src/ai_pipeline_handler.cpp` 中实现 AiPipelineHandler 类
    - 实现 `create()` 工厂方法：检查 detector 非空，构造实例，spdlog::info 记录配置
    - 实现 `install_probe()`：通过 gst_bin_get_by_name 获取 "q-ai"，在 src pad 安装 buffer probe，参照设计文档参考代码
    - 实现 `remove_probe()`：安全移除已安装的 probe，处理悬空指针保护
    - 实现 `buffer_probe_cb()`：检查抽帧间隔 + busy flag → gst_pad_get_current_caps 获取宽高 → gst_buffer_map 映射 I420 → i420_to_rgb 转换 → notify 推理线程
    - 实现 `start()` / `stop()`：创建/停止推理线程
    - 实现 `inference_loop()`：cv_.wait 等待 → detect_with_stats → filter_detections → 事件管理 → DetectionCallback
    - 实现 `set_detection_callback()`
    - _需求：1.1, 1.2, 1.5, 1.6, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 3.1, 4.1, 4.2, 4.3, 4.5, 4.6, 4.7, 4.8, 5.3, 5.4, 5.5_

  - [x] 2.2 在 `device/src/ai_pipeline_handler.cpp` 中实现事件管理
    - 实现 `open_event()`：内存初始化事件状态（event_id、start_time、device_id），不创建目录不写盘
    - 实现 `encode_snapshot()`：stb_image_write JPEG 编码到内存（质量 85%），缓存到 cached_frames_
    - 实现 `update_detections_summary()`：内存累积 per-class 检测统计
    - 实现 `check_event_timeout()`：检查超时，超时则调用 close_event()
    - 实现 `close_event()`：批量写盘 — 创建事件目录 → 写入所有缓存 JPEG → 写入 event.json（nlohmann/json）→ 清空缓存
    - 实现中间 flush 逻辑：cached_bytes_ ≥ max_cache_mb 时创建目录 + 写盘 + 清空缓存 + 事件继续
    - stop() 时如有活跃事件，立即 close_event()
    - _需求：5.5.1, 5.5.2, 5.5.3, 5.5.4, 5.5.5, 5.5.6, 5.5.7, 5.5.8, 5.5.9, 5.5.10, 5.5.12, 5.5.13, 5.5.14, 5.5.15, 5.5.17, 5.5.18, 5.5.19_

- [x] 3. 配置管理扩展
  - [x] 3.1 修改 `device/src/config_manager.h` — 新增 AiConfig 支持
    - 前向声明 `AiConfig`（定义在 ai_pipeline_handler.h 中）
    - 声明 `parse_ai_config()` 纯函数
    - 在 `ConfigManager` 类中新增 `ai_config_` 成员和 `ai_config()` 访问器
    - _需求：8.1_

  - [x] 3.2 修改 `device/src/config_manager.cpp` — 实现 parse_ai_config()
    - 实现 `parse_ai_config()`：解析 model_path、inference_fps（1-30 范围校验）、confidence_threshold、snapshot_dir、event_timeout_sec（≥3 校验）、max_cache_mb、target_classes（逗号分隔字符串 → TargetClass 向量）
    - 在 `ConfigManager::load()` 中调用 `parse_ai_config()` 解析 `[ai]` section
    - inference_fps 为 0 或超出 1-30 范围时返回错误
    - event_timeout_sec < 3 时使用默认值 15 并记录 warn 日志
    - _需求：8.1, 8.2, 8.3, 8.4, 8.5, 8.6_

- [x] 4. 管道构建与 AppContext 集成
  - [x] 4.1 修改 `device/src/pipeline_builder.h` — 新增 ai_handler 参数
    - `build_tee_pipeline` 新增最后一个参数 `AiPipelineHandler* ai_handler = nullptr`
    - 前向声明 `AiPipelineHandler`
    - _需求：6.1, 6.5_

  - [x] 4.2 修改 `device/src/pipeline_builder.cpp` — 安装 probe
    - 当 ai_handler 非空时，在管道构建完成后调用 `ai_handler->install_probe(pipeline)`
    - ai_handler 为 nullptr 时保持现有行为
    - 管道拓扑不变（raw-tee → q-ai → fakesink），仅加 probe
    - _需求：6.2, 6.3, 6.4_

  - [x] 4.3 修改 `device/src/app_context.cpp` — 集成 AiPipelineHandler 生命周期
    - Impl 新增 `std::unique_ptr<AiPipelineHandler>` 成员
    - init()：当 model_path 非空且模型文件存在时，创建 YoloDetector + AiPipelineHandler；否则跳过，记录 info 日志
    - start()：将 ai_handler 指针传入 build_tee_pipeline，管道启动后调用 ai_handler->start()
    - stop()：在 ShutdownHandler 中注册 ai_handler 停止步骤（在 pipeline 停止前执行）
    - rebuild 回调：stop → build_tee_pipeline(... ai_handler) → start
    - 条件编译：`#ifdef ENABLE_YOLO` 保护 AI 相关代码
    - _需求：7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7_

- [x] 5. CMake 与配置文件更新
  - [x] 5.1 修改 `device/CMakeLists.txt` — 新增 ai_pipeline_module
    - 在 `if(ENABLE_YOLO AND OnnxRuntime_FOUND)` 块中新增 `ai_pipeline_module` 静态库
    - 源文件：`src/ai_pipeline_handler.cpp`
    - 链接：`yolo_module pipeline_manager nlohmann_json::nlohmann_json`
    - include 目录包含 `third_party/stb`
    - 新增 `ai_pipeline_test` 测试目标，链接 `ai_pipeline_module GTest::gtest_main rapidcheck rapidcheck_gtest`
    - 为 pipeline_manager 添加条件编译定义 `ENABLE_YOLO`（当 YOLO 启用时）
    - _需求：10.3, 10.5_

  - [x] 5.2 修改 `device/config/config.toml` — 新增 [ai] section
    - 添加 model_path、inference_fps、confidence_threshold、snapshot_dir、event_timeout_sec、max_cache_mb、target_classes 配置项
    - 参照设计文档中的 config.toml 示例
    - _需求：8.1_

  - [x] 5.3 修改 `.gitignore` — 排除 events 目录
    - 新增 `device/events/` 排除事件截图目录
    - _需求：5.5.11_

- [x] 6. 检查点 — 编译通过 + 现有测试回归
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认编译无错误，现有测试全部通过
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户

- [x] 7. 测试 — Example-Based 与 PBT
  - [x] 7.1 创建 `device/tests/ai_pipeline_test.cpp` — 基础结构与 example-based 测试
    - I420→RGB 纯黑帧测试：Y=0, U=128, V=128 → R≈0, G≈0, B≈0
    - I420→RGB 纯白帧测试：Y=255, U=128, V=128 → R≈255, G≈255, B≈255
    - create() nullptr 输入测试：传入 nullptr detector → 返回 nullptr + error_msg
    - filter_detections 空 target_classes 测试：所有 detection 按全局阈值过滤
    - filter_detections per-class 覆盖测试：特定类别使用 per-class 阈值
    - coco_class_name 边界测试：class_id 0-79 返回有效名称，超出范围返回 "unknown"
    - parse_ai_config 测试：inference_fps 范围校验、event_timeout_sec 校验、target_classes 解析
    - _需求：9.1, 9.3, 9.4_

  - [x] 7.2 添加 PBT — Property 1: I420→RGB 转换输出不变量
    - **Property 1: I420→RGB 转换输出不变量**
    - 随机偶数宽高 [2, 1920]×[2, 1080]，随机 Y/U/V 像素值 [0, 255]
    - 验证输出缓冲区大小恒等于 width × height × 3
    - 验证所有输出像素值在 [0, 255] 范围内
    - **验证：需求 3.2, 3.5**

  - [x] 7.3 添加 PBT — Property 2: 抽帧节流决策一致性
    - **Property 2: 抽帧节流决策一致性**
    - 随机 elapsed_ms [0, 5000]，随机 fps [1, 30]
    - 验证 should_sample 返回 true 当且仅当 elapsed_ms >= 1000 / fps
    - **验证：需求 2.2, 2.3**

  - [x] 7.4 添加 PBT — Property 3: target_classes 过滤正确性
    - **Property 3: target_classes 过滤正确性**
    - 随机 Detection 向量（0-20 个），随机 target_classes（0-5 个），随机阈值
    - 验证输出中每个 Detection 的 class_name 在 target_classes 中（或 target_classes 为空时保留所有类）
    - 验证输出中每个 Detection 的 confidence ≥ 该类别阈值
    - 验证输出是输入的子集（不新增、不修改元素）
    - **验证：需求 4.4, 8.2, 8.3**

- [x] 8. 检查点 — 全量测试通过
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过（现有 + 新增 ai_pipeline_test）
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户

- [x] 9. 最终检查点 — 双平台与条件编译验证
  - 确认 `-DENABLE_YOLO=OFF` 时编译和测试正常（AI 模块被完全跳过）
  - 确认现有测试（smoke_test、log_test、tee_test、camera_test、health_test、yolo_test 等）行为不变
  - Pi 5 Release 验证（`scripts/pi-build.sh` 或手动 SSH）需要 Pi 5 可达，不可达则标注跳过
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：10.1, 10.2, 10.3, 10.4, 10.5_

## 备注

- 新建文件：`device/third_party/stb/stb_image_write.h`、`device/src/ai_pipeline_handler.h`、`device/src/ai_pipeline_handler.cpp`、`device/tests/ai_pipeline_test.cpp`
- 修改文件：`device/src/config_manager.h`、`device/src/config_manager.cpp`、`device/src/pipeline_builder.h`、`device/src/pipeline_builder.cpp`、`device/src/app_context.cpp`、`device/CMakeLists.txt`、`device/config/config.toml`、`.gitignore`
- 不修改文件：所有现有测试文件、yolo_detector.h/cpp、pipeline_manager.h/cpp
- 独立函数（i420_to_rgb、filter_detections、coco_class_name）优先实现并测试，不依赖 GStreamer 管道
- PBT 使用 RapidCheck，每个属性最少 100 次迭代
- JPEG 编码使用 stb_image_write.h（header-only，MIT 许可证），仅在 .cpp 中定义 IMPLEMENTATION
- event.json 使用 nlohmann/json 序列化（项目已通过 FetchContent 引入）
- ENABLE_YOLO=OFF 时完全跳过 ai_pipeline_module 编译，AppContext 中不创建 AiPipelineHandler
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
