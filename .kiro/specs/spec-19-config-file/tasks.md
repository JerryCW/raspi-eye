# 实现计划：ConfigManager 统一配置加载

## 概述

按依赖顺序实现 ConfigManager 模块：先修改现有接口（CameraConfig 扩展、StreamModeController debounce 参数、log_init 重载），再实现 ConfigManager 核心模块，然后更新 CMakeLists.txt 编译验证，接着编写测试，最后集成到 AppContext/main.cpp 并更新配置文件示例。

## 任务

- [x] 1. 修改现有接口以支持 ConfigManager
  - [x] 1.1 扩展 CameraConfig 结构体
    - 在 `device/src/camera_source.h` 的 `CameraConfig` 中新增 `width`、`height`、`framerate` 字段（带默认值）
    - width=1280, height=720, framerate=15
    - 不修改 `camera_source.cpp` 和现有测试文件
    - _需求: 2.1, 2.2_

  - [x] 1.2 为 StreamModeController 构造函数添加 debounce_ms 参数
    - 修改 `device/src/stream_mode_controller.h`：构造函数签名改为 `explicit StreamModeController(GstElement* pipeline, int debounce_ms = 3000)`
    - 修改 `device/src/stream_mode_controller.cpp`：将 `static constexpr int kDebounceMs = 3000` 改为使用构造函数传入的参数，存储到 Impl 成员中
    - 默认值 3000 保持向后兼容，现有调用点无需修改
    - _需求: 6.4_

  - [x] 1.3 为 log_init 添加 LoggingConfig 重载
    - 在 `device/src/log_init.h` 中前向声明 `struct LoggingConfig`，新增 `void init(const LoggingConfig& config)` 重载
    - 在 `device/src/log_init.cpp` 中实现新重载：根据 `config.format` 决定 JSON/text，根据 `config.level` 设置 spdlog 全局日志级别
    - 保留原有 `init(bool json)` 不删除（向后兼容）
    - 有效 level 映射：trace→trace, debug→debug, info→info, warn→warn, error→error
    - _需求: 4.1, 7.2_

- [x] 2. 实现 ConfigManager 核心模块
  - [x] 2.1 创建 config_manager.h
    - 定义 `StreamingConfig`、`LoggingConfig`、`ConfigOverrides` 结构体
    - 声明纯函数：`parse_camera_config`、`parse_streaming_config`、`parse_logging_config`、`validate_streaming_config`、`to_bitrate_config`
    - 声明 `ConfigManager` 类（值类型，非 pImpl）
    - 包含必要的头文件：`camera_source.h`、`credential_provider.h`、`kvs_sink_factory.h`、`webrtc_signaling.h`、`bitrate_adapter.h`
    - 接口签名严格按照设计文档
    - _需求: 1.1, 1.2_

  - [x] 2.2 实现 config_manager.cpp
    - 实现 `parse_camera_config`：从 kv map 解析 type/device/width/height/framerate，缺失字段使用默认值，无效 type 返回 false
    - 实现 `parse_streaming_config`：从 kv map 解析 7 个整数字段，缺失字段使用默认值
    - 实现 `parse_logging_config`：从 kv map 解析 level/format，验证枚举值有效性
    - 实现 `validate_streaming_config`：验证 min <= default <= max
    - 实现 `to_bitrate_config`：StreamingConfig → BitrateConfig 字段映射
    - 实现 `ConfigManager::load()`：按设计文档 14 步流程，必填 section 失败返回 false，可选 section 缺失使用默认值
    - 实现 `ConfigManager::apply_overrides()`：按设计文档 4 步流程
    - 复用现有 `parse_toml_section`、`build_aws_config`、`build_kvs_config`、`build_webrtc_config`
    - 日志中不使用非 ASCII 字符
    - _需求: 1.1, 1.3, 1.4, 2.1, 2.2, 2.3, 2.4, 3.1, 3.2, 3.3, 3.4, 3.5, 4.1, 4.2, 4.3, 4.4, 4.5, 5.1, 5.2, 5.3, 5.4_

- [x] 3. 更新 CMakeLists.txt 并编译验证
  - 在 `device/CMakeLists.txt` 中新增 `config_module` 静态库，源文件为 `src/config_manager.cpp`
  - `config_module` 链接依赖：`credential_module`、`kvs_module`、`webrtc_module`、`log_module`、`pipeline_manager`（或按需最小化依赖）
  - 将 `config_module` 链接到 `pipeline_manager`（因为 app_context 需要使用）
  - 新增 `config_test` 测试可执行文件，源文件 `tests/config_test.cpp`（先创建空壳），链接 `config_module`、`GTest::gtest_main`、`rapidcheck`、`rapidcheck_gtest`
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build` 确认编译通过
  - _需求: 1.1_

- [x] 4. 编写测试
  - [x] 4.1 编写 Example-based 单元测试
    - 在 `device/tests/config_test.cpp` 中编写以下测试用例：
    - 空 kv map 默认值测试：各 `parse_*_config` 传入空 map 返回默认值
    - 有效完整字段测试：各 `parse_*_config` 传入完整有效 kv map，字段正确解析
    - 无效 camera type 测试：`parse_camera_config` 对无效 type 返回 false
    - 无效 logging level/format 测试：`parse_logging_config` 对无效值返回 false
    - streaming 码率范围无效测试：`validate_streaming_config` 对 min > max 返回 false
    - streaming default 超出范围测试：`validate_streaming_config` 对 default 不在 [min, max] 返回 false
    - `to_bitrate_config` 转换测试：验证字段一一对应
    - `apply_overrides` 覆盖测试：--log-json 覆盖 format、--camera 覆盖 type
    - 运行 `ctest --test-dir device/build --output-on-failure` 确认测试通过
    - _需求: 1.3, 1.4, 2.1, 2.2, 2.3, 2.4, 3.1, 3.2, 3.4, 3.5, 4.1, 4.2, 4.3, 4.4, 4.5, 5.1, 5.2, 5.3, 6.3_

  - [x] 4.2 编写 Property 1 测试：parse_camera_config 解析保真
    - **Property 1: parse_camera_config 解析保真**
    - 生成器：随机 kv map 子集（type ∈ {test, v4l2, libcamera}，width/height/framerate 为正整数字符串）
    - 断言：map 中存在的字段被正确解析，缺失字段保持平台默认值
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 2.1, 2.3**

  - [x] 4.3 编写 Property 2 测试：无效 camera type 拒绝
    - **Property 2: 无效 camera type 拒绝**
    - 生成器：随机字符串，排除 {test, v4l2, libcamera}
    - 断言：`parse_camera_config` 返回 false
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 2.4**

  - [x] 4.4 编写 Property 3 测试：parse_streaming_config 解析保真
    - **Property 3: parse_streaming_config 解析保真**
    - 生成器：随机 kv map 子集（所有值为非负整数字符串）
    - 断言：map 中存在的字段被正确解析，缺失字段保持默认值
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 3.1, 3.3**

  - [x] 4.5 编写 Property 4 测试：validate_streaming_config 一致性验证
    - **Property 4: validate_streaming_config 一致性验证**
    - 生成器：随机 StreamingConfig（各字段随机非负整数）
    - 断言：返回 true 当且仅当 min <= default <= max
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 3.4, 3.5**

  - [x] 4.6 编写 Property 5 测试：parse_logging_config 解析保真
    - **Property 5: parse_logging_config 解析保真**
    - 生成器：随机 kv map 子集（level ∈ {trace, debug, info, warn, error}，format ∈ {text, json}）
    - 断言：map 中存在的字段被正确解析，缺失字段保持默认值
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 4.1, 4.3**

  - [x] 4.7 编写 Property 6 测试：无效 logging 值拒绝
    - **Property 6: 无效 logging 值拒绝**
    - 生成器：随机字符串，排除有效枚举值
    - 断言：`parse_logging_config` 返回 false
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 4.4, 4.5**

  - [x] 4.8 编写 Property 7 测试：命令行覆盖优先级
    - **Property 7: 命令行覆盖优先级**
    - 生成器：随机有效 ConfigOverrides（camera_type ∈ {test, v4l2, libcamera} 或空，device 随机字符串或空，log_json 随机 bool）
    - 断言：有覆盖值的字段等于覆盖值，无覆盖值的字段保持原值
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 5.1, 5.2, 5.4**

  - [x] 4.9 编写 Property 8 测试：StreamingConfig → BitrateConfig 转换保真
    - **Property 8: StreamingConfig → BitrateConfig 转换保真**
    - 生成器：随机 StreamingConfig
    - 断言：`to_bitrate_config` 转换后各字段与 StreamingConfig 对应字段一致
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 6.3**

- [x] 5. 检查点 - 编译与测试验证
  - 运行 `cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认所有测试通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [x] 6. 集成到 AppContext 和 main.cpp
  - [x] 6.1 改造 AppContext
    - 修改 `device/src/app_context.h`：`init()` 签名从 `(const std::string& config_path, const CameraSource::CameraConfig& cam_config, ...)` 改为 `(const std::string& config_path, const ConfigOverrides& overrides, ...)`
    - 修改 `device/src/app_context.cpp`：
      - `#include "config_manager.h"`
      - Impl 中新增 `StreamingConfig streaming_config` 和 `LoggingConfig logging_config` 成员
      - `init()` 中创建 ConfigManager，调用 load + apply_overrides，从中获取各模块配置
      - `start()` 中创建 StreamModeController 时传入 `streaming_config.debounce_sec * 1000`
      - `start()` 中创建 BitrateAdapter 时传入 `to_bitrate_config(streaming_config)`
    - _需求: 6.1, 6.2, 6.3, 6.4_

  - [x] 6.2 改造 main.cpp
    - `#include "config_manager.h"`
    - 将命令行解析结果填入 `ConfigOverrides` 而非 `CameraConfig`
    - 调用 `log_init::init(config.logging_config())` 替代 `log_init::init(use_json)`（需在 ConfigManager load 之后）
    - 调整初始化顺序：先解析命令行 → 创建 ConfigManager 加载配置 → 初始化日志 → AppContext::init()
    - 传递 `ConfigOverrides` 给 `AppContext::init()`
    - _需求: 5.1, 5.2, 5.3, 6.1_

  - [x] 6.3 更新 config.toml.example
    - 在 `device/config/config.toml.example` 中添加 `[camera]`、`[streaming]`、`[logging]` section 示例
    - 注释说明各字段含义、有效值范围、默认值
    - _需求: 7.1_

- [x] 7. 最终检查点 - 全量编译与测试
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认全量编译和所有测试通过
  - 确认无 ASan 报告
  - 运行 `git status` 确认无敏感文件（.pem、.key 等）被跟踪
  - 不执行 git commit（由编排层统一执行）
  - 如有问题，询问用户

## 禁止项

- SHALL NOT 使用 `cat <<` heredoc 方式写入文件，使用 fsWrite / fsAppend 工具
- SHALL NOT 直接运行测试可执行文件，统一通过 `ctest --test-dir device/build --output-on-failure` 运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 在子代理中执行 git commit
- SHALL NOT 引入新的 TOML 解析库，必须复用现有 `parse_toml_section`
- SHALL NOT 删除现有 `build_aws_config`、`build_kvs_config`、`build_webrtc_config` 函数
- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息

## 备注

- 标记 `*` 的子任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求编号，确保可追溯
- 检查点确保增量验证
- Property 测试验证纯函数的通用正确性属性，单元测试验证特定场景和边界条件
- CameraConfig 新增的 width/height/framerate 在本 Spec 中只做解析和存储，pipeline 实际使用留给后续 Spec
