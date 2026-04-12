# Implementation Plan: Pipeline CPU Optimization

## Overview

将 V4L2 USB 摄像头格式优先级从 I420 > YUYV > MJPG 改为 MJPG > I420 > YUYV，消除冗余 videoconvert，并添加三路 enable 开关。实施顺序：先做可测试性重构（暴露纯函数），再改格式优先级和 MJPG 参数化，然后改 pipeline_builder 条件跳过，接着加 config 三路开关，最后改 app_context 传递开关。

## Tasks

- [x] 1. 可测试性重构：将 select_best_format 和 V4L2Format 移到 CameraSource 命名空间
  - [x] 1.1 将 V4L2Format 枚举、v4l2_format_name、select_best_format 从 camera_source.cpp 的 anonymous namespace 移到 CameraSource 命名空间，在 camera_source.h 中声明
    - 在 camera_source.h 中新增 `V4L2Format` 枚举（I420, YUYV, MJPG, UNKNOWN）
    - 在 camera_source.h 中声明 `const char* v4l2_format_name(V4L2Format fmt)`
    - 在 camera_source.h 中声明 `V4L2Format select_best_format(const std::vector<V4L2Format>& formats)`
    - camera_source.cpp 中将这三个定义从 anonymous namespace 移到 `namespace CameraSource`
    - camera_source.cpp 内部调用点更新为使用 CameraSource 命名空间限定
    - _Requirements: 1.5_
  - [x] 1.2 新增 SourceOutputFormat 枚举和 create_source 签名变更
    - 在 camera_source.h 中新增 `SourceOutputFormat` 枚举（UNKNOWN, I420, YUYV）
    - 变更 `create_source` 签名：新增 `SourceOutputFormat* out_format = nullptr` 参数（在 error_msg 之后）
    - 在 create_source 各路径中设置 out_format：videotestsrc→UNKNOWN, libcamerasrc→UNKNOWN, V4L2 MJPG→I420, V4L2 I420→I420, V4L2 YUYV→YUYV, V4L2 无 device→UNKNOWN
    - 现有调用方（不传 out_format）无需修改，默认 nullptr 被忽略
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 5.1, 5.2, 5.3_

- [x] 2. 格式优先级反转与 MJPG Bin 参数化
  - [x] 2.1 修改 select_best_format 优先级为 MJPG > I420 > YUYV
    - 将 camera_source.cpp 中 select_best_format 的遍历顺序从 I420→YUYV→MJPG 改为 MJPG→I420→YUYV
    - 纯函数变更，不影响签名
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_
  - [x] 2.2 Property 1: 格式选择优先级不变式（PBT）
    - **Property 1: 格式选择优先级不变式**
    - 在 camera_test.cpp 中新增 RC_GTEST_PROP 测试
    - 生成器：随机生成非空 V4L2Format 向量（从 {MJPG, I420, YUYV} 中随机选取 1-10 个元素，允许重复）
    - 断言：若含 MJPG 则返回 MJPG，否则若含 I420 则返回 I420，否则若含 YUYV 则返回 YUYV
    - 标签：`Feature: pipeline-cpu-optimization, Property 1: 格式选择优先级不变式`
    - **Validates: Requirements 1.1, 1.2, 1.3, 1.4, 1.5**
  - [x] 2.3 select_best_format 单元测试
    - 在 camera_test.cpp 中新增 example-based 测试
    - MJPG+YUYV → MJPG、MJPG+I420 → MJPG、仅 YUYV → YUYV、仅 I420 → I420、空向量 → UNKNOWN
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_
  - [x] 2.4 create_mjpg_bin 参数化：从 CameraConfig 读取 w/h/fps
    - 变更 create_mjpg_bin 签名：从 `(const std::string& device_path, std::string* error_msg)` 改为 `(const CameraConfig& config, std::string* error_msg)`
    - 内部使用 config.device 设置 v4l2src device 属性
    - 使用 config.width/height/framerate 设置 capsfilter，为 0 时不设置对应字段（让 GStreamer 自动协商）
    - 更新 create_source 中调用 create_mjpg_bin 的地方，传入 config 而非 config.device
    - _Requirements: 2.1, 2.2, 2.3_

- [x] 3. pipeline_builder 条件跳过 videoconvert
  - [x] 3.1 修改 build_tee_pipeline 内部逻辑：根据 SourceOutputFormat 决定是否跳过 videoconvert
    - 调用 create_source 时传入 `&src_format` 获取输出格式
    - 当 src_format == I420 时：跳过 videoconvert，直接 src → capsfilter → raw-tee
    - 其他情况：保留 src → videoconvert → capsfilter → raw-tee
    - 跳过时 spdlog info 日志：`"Skipping videoconvert: source already outputs I420"`
    - 保留时 spdlog info 日志：`"Using videoconvert: source format={}"`
    - build_tee_pipeline 签名不变，rebuild_callback 无需修改
    - 注意：跳过 videoconvert 时，元素数组和 null-check 逻辑需要相应调整（convert 不创建或不加入 pipeline）
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 5.1, 5.4_
  - [x] 3.2 pipeline_builder videoconvert 条件跳过单元测试
    - 在 tee_test.cpp 中新增测试
    - 默认 config（videotestsrc）：pipeline 包含 "convert" 元素，可达 PLAYING/PAUSED
    - 验证现有 tee_test 测试全部通过（向后兼容）
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 5.1, 5.4_

- [x] 4. Checkpoint — 确保格式优先级和 videoconvert 跳过逻辑正确
  - 确保所有测试通过，ask the user if questions arise.

- [x] 5. config_manager 三路 enable 开关
  - [x] 5.1 KvsConfig 和 WebRtcConfig 新增 enabled 字段
    - 在 kvs_sink_factory.h 的 KvsConfig 中新增 `bool enabled = true`
    - 在 webrtc_signaling.h 的 WebRtcConfig 中新增 `bool enabled = true`
    - 在 build_kvs_config 和 build_webrtc_config 中解析 enabled 字段
    - 布尔值解析规则：接受 "true"/"false"（大小写不敏感），其他值返回错误
    - _Requirements: 4.1, 4.2, 4.3, 4.5, 4.6, 4.8_
  - [x] 5.2 新增 AiConfig 结构体和 parse_ai_config 纯函数
    - 在 config_manager.h 中新增 `struct AiConfig { bool enabled = true; }`
    - 在 config_manager.h 中声明 `bool parse_ai_config(const std::unordered_map<std::string, std::string>& kv, AiConfig& config, std::string* error_msg = nullptr)`
    - 在 config_manager.cpp 中实现 parse_ai_config
    - 在 ConfigManager 中新增 `AiConfig ai_config_` 成员和 `const AiConfig& ai_config() const` accessor
    - 在 ConfigManager::load 中新增解析 `[ai]` section（可选，缺失时使用默认值）
    - _Requirements: 4.1, 4.4, 4.7, 5.5_
  - [x] 5.3 Property 2: 非法布尔值被拒绝（PBT）
    - **Property 2: 非法布尔值被拒绝**
    - 在 config_test.cpp 中新增 RC_GTEST_PROP 测试
    - 生成器：随机生成非空字符串，前置条件排除 "true"/"false"（大小写不敏感）
    - 断言：布尔解析函数返回 false
    - 标签：`Feature: pipeline-cpu-optimization, Property 2: 非法布尔值被拒绝`
    - **Validates: Requirements 4.8**
  - [x] 5.4 config_manager enable 开关单元测试
    - 在 config_test.cpp 中新增 example-based 测试
    - parse_ai_config 空 map → enabled=true
    - parse_ai_config enabled="true" → enabled=true
    - parse_ai_config enabled="false" → enabled=false
    - parse_ai_config enabled="TRUE" → enabled=true（大小写不敏感）
    - parse_ai_config enabled="invalid" → 返回 false
    - KvsConfig enabled 字段解析（同上模式）
    - WebRtcConfig enabled 字段解析（同上模式）
    - 现有 config 测试不受影响（向后兼容）
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 4.8, 5.5_

- [ ] 6. app_context enable 开关传递
  - [x] 6.1 修改 app_context.cpp init() 和 start()：根据 enabled 决定模块创建和参数传递
    - Impl 新增 `AiConfig ai_config` 成员，init() 中从 ConfigManager 获取
    - init() 中：webrtc_config.enabled=false 时跳过 signaling 和 media_manager 创建（保持 nullptr）
    - start() 中：kvs_config.enabled=false 时传 nullptr 给 build_tee_pipeline（PipelineBuilder 用 fakesink）
    - start() 中：media_manager 指针直接使用（init 阶段已决定，可能为 nullptr）
    - rebuild_callback 中同样使用 enabled 判断传递 nullptr
    - _Requirements: 4.5, 4.6, 4.7_

- [ ] 7. 更新配置文件模板
  - [x] 7.1 更新 config.toml 和 config.toml.example
    - config.toml：[kvs] 和 [webrtc] section 新增 `enabled = true`，新增 `[ai]` section 含 `enabled = true`
    - config.toml.example：同步更新，添加 enabled 字段注释说明和 [ai] section 模板
    - _Requirements: 4.1, 4.2, 4.3, 4.4_

- [x] 8. 链接 camera_test 到 RapidCheck 并确保 CMakeLists.txt 正确
  - 在 CMakeLists.txt 中为 camera_test 添加 `rapidcheck rapidcheck_gtest` 链接（当前只链接了 GTest::gtest）
    - _Requirements: 1.1, 1.5_

- [x] 9. Final checkpoint — 确保所有测试通过
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确保所有现有测试 + 新增测试通过
  - 确保无 ASan 报告
  - 确保所有测试通过，ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP

- 所有代码使用 C++17，修改范围限于 camera_source.cpp/h、pipeline_builder.cpp/h、config_manager.cpp/h、kvs_sink_factory.h、webrtc_signaling.h、app_context.cpp、config.toml、config.toml.example、CMakeLists.txt
- Property 测试使用 RapidCheck 库，每个属性测试最少 100 次迭代
- 验证方式：macOS `ctest --test-dir device/build --output-on-failure`；Pi 5 手动验证 CPU 占用
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 直接运行测试可执行文件，必须通过 ctest
