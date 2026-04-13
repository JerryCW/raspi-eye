# 实现计划：Spec 23 — 统一日志管理

## 概述

按依赖顺序实现三个核心改进：先扩展 LoggingConfig 和 parse_logging_config 支持 component_levels，再扩展 log_init 创建所有命名 logger 并应用 per-component 级别，然后迁移各模块到命名 logger、添加 KVS SDK 日志重定向、调整 AI 检测结果日志级别，最后编写测试并更新配置文件示例。

## 任务

- [x] 1. 扩展 LoggingConfig 和 parse_logging_config
  - [x] 1.1 在 LoggingConfig 中新增 component_levels 字段
    - 修改 `device/src/config_manager.h`：在 `LoggingConfig` 结构体中新增 `std::unordered_map<std::string, std::string> component_levels` 字段，默认为空 map
    - _需求: 2.1, 2.2_

  - [x] 1.2 扩展 parse_logging_config 解析 component_levels
    - 修改 `device/src/config_manager.cpp`：在 `parse_logging_config` 中新增对 `component_levels` 字段的解析
    - 格式：`"ai:debug,kvs:warn,webrtc:info"`，逗号分隔，冒号分隔组件名和级别
    - 前后空格自动 trim，空字符串视为无配置
    - 无效级别（不在 trace/debug/info/warn/error 中）返回 false + error_msg
    - 缺少冒号的条目返回 false + error_msg
    - _需求: 1.1, 1.5, 2.3, 6.1, 6.2, 6.3_

- [x] 2. 扩展 log_init 支持 per-component 级别和命名 logger
  - [x] 2.1 扩展 log_init::init(const LoggingConfig&) 创建所有命名 logger
    - 修改 `device/src/log_init.cpp`：在 `init(const LoggingConfig&)` 中创建所有 9 个命名 logger：main、pipeline、app、config、stream、ai、kvs、webrtc、s3
    - 设置全局级别后，遍历 `config.component_levels`，对每个条目通过 `spdlog::get(name)` 查找 logger 并设置独立级别
    - 未注册组件名以 warn 级别记录日志后忽略
    - _需求: 1.2, 1.3, 1.4, 1.6, 5.1_

  - [x] 2.2 新增 log_init::parse_level 辅助函数
    - 修改 `device/src/log_init.h`：声明 `std::optional<spdlog::level::level_enum> parse_level(const std::string&)`
    - 修改 `device/src/log_init.cpp`：实现字符串到 spdlog level enum 的转换，无效返回 nullopt
    - 复用此函数替代 init 中的 if-else 链
    - _需求: 1.2_

  - [x] 2.3 新增 KVS SDK 日志重定向
    - 修改 `device/src/log_init.h`：声明 `void setup_kvs_log_redirect()`
    - 修改 `device/src/log_init.cpp`：在 `#ifdef HAVE_KVS_WEBRTC_SDK` 条件编译块中实现 `kvs_log_callback` 和 `setup_kvs_log_redirect()`
    - macOS 无 SDK 时编译为空函数
    - 回调内使用栈上 1024 字节缓冲区，去除尾部换行符
    - KVS 级别映射：1→trace, 2→debug, 3→info, 4→warn, 5→error
    - _需求: 3.1, 3.2, 3.3, 3.5_

- [x] 3. 检查点 - 编译验证
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build` 确认编译通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [x] 4. 迁移模块 Logger 和调整 AI 日志级别
  - [x] 4.1 迁移 ai_pipeline_handler.cpp 到 "ai" logger
    - 修改 `device/src/ai_pipeline_handler.cpp`：将所有 `spdlog::info/debug/warn/error(...)` 替换为 `spdlog::get("ai")` 命名 logger 调用
    - 检测到目标时使用 info 级别输出摘要：`Inference: {耗时}ms, detected: {类别名}({置信度}), ...`
    - 未检测到目标时使用 debug 级别输出：`Inference: {耗时}ms, {原始检测数} raw detections, 0 after filter`
    - create() 成功时使用 info 级别输出模型加载信息（模型路径、推理 FPS、置信度阈值）
    - logger 不存在时回退到默认 logger
    - _需求: 4.1, 4.2, 4.4, 4.5, 5.2, 5.3_

  - [x] 4.2 迁移 yolo_detector.cpp 到 "ai" logger
    - 修改 `device/src/yolo_detector.cpp`：将所有 `spdlog::info/debug/warn/error(...)` 替换为 `spdlog::get("ai")` 命名 logger 调用
    - 初始化完成时使用 info 级别输出模型详情（输入尺寸、线程数、execution provider）
    - logger 不存在时回退到默认 logger
    - _需求: 4.3, 5.2_

  - [x] 4.3 迁移 s3_uploader.cpp 到 "s3" logger
    - 修改 `device/src/s3_uploader.cpp`：将所有 `spdlog::info/warn/error(...)` 替换为 `spdlog::get("s3")` 命名 logger 调用
    - logger 不存在时回退到默认 logger
    - _需求: 5.2_

- [x] 5. 编写测试
  - [x] 5.1 编写 component_levels 解析的 Example-based 单元测试
    - 在 `device/tests/config_test.cpp` 中新增以下测试：
    - `ParseLoggingConfig_ComponentLevels_Valid`：正常 component_levels 解析
    - `ParseLoggingConfig_ComponentLevels_WithSpaces`：含空格的 component_levels 正确 trim
    - `ParseLoggingConfig_ComponentLevels_Empty`：空字符串视为无配置
    - `ParseLoggingConfig_ComponentLevels_InvalidLevel`：无效级别返回 false
    - `ParseLoggingConfig_ComponentLevels_MalformedEntry`：缺少冒号返回 false
    - _需求: 1.1, 1.5, 6.2, 6.3, 8.1_

  - [x] 5.2 编写 log_init per-component 级别的 Example-based 单元测试
    - 在 `device/tests/log_test.cpp` 中新增以下测试：
    - `InitCreatesAllLoggers`：init 后所有 9 个命名 logger 存在
    - `EmptyComponentLevels`：空 component_levels 不影响全局级别
    - `UnknownComponentIgnored`：未知组件名被忽略，不崩溃
    - `ComponentLevelIsolation`：设置 ai=debug 后 ai 输出 debug，其他 logger 不输出 debug
    - `MissingLoggingSection`：缺少 [logging] section 使用默认值
    - _需求: 1.4, 1.6, 5.1, 7.1, 7.2, 8.2, 8.3_

  - [x] 5.3 编写 Property 1 测试：component_levels 解析往返
    - **Property 1: component_levels 解析往返**
    - 生成器：随机有效组件名到级别的映射表（组件名为非空小写字母字符串，级别为 trace/debug/info/warn/error 之一），序列化为含随机空格的字符串
    - 断言：通过 parse_logging_config 解析后，component_levels 映射表与原始映射表完全一致
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 1.1, 2.3, 6.1, 6.2**

  - [x] 5.4 编写 Property 2 测试：Per-component 级别应用
    - **Property 2: Per-component 级别应用**
    - 生成器：随机全局级别 + 随机 component_levels（组件名从已注册 logger 列表中选取）
    - 断言：调用 log_init::init(config) 后，指定组件的 logger 级别等于 component_levels 中的级别，未指定的 logger 级别等于全局级别
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 1.2, 1.3, 3.4**

  - [x] 5.5 编写 Property 3 测试：无效级别拒绝
    - **Property 3: 无效级别拒绝**
    - 生成器：随机 component_levels 字符串，其中至少一个条目的级别值不在 {trace, debug, info, warn, error} 中
    - 断言：parse_logging_config 返回 false
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 1.5**

- [x] 6. 检查点 - 编译与测试验证
  - 运行 `cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认所有测试通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [x] 7. 更新配置文件示例和集成
  - [x] 7.1 更新 config.toml.example
    - 修改 `device/config/config.toml.example`：在 `[logging]` section 中新增 `component_levels` 字段示例和注释说明
    - 示例：`# component_levels = "ai:debug,kvs:warn,webrtc:info"`
    - _需求: 6.4_

  - [x] 7.2 在 main.cpp 中调用 setup_kvs_log_redirect
    - 修改 `device/src/main.cpp`：在 log_init::init(config) 之后调用 `log_init::setup_kvs_log_redirect()`
    - _需求: 3.1, 3.4_

- [x] 8. 最终检查点 - 全量编译与测试
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
- SHALL NOT 修改现有模块的核心逻辑（仅修改日志相关代码）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
- SHALL NOT 在不确定外部 SDK/库 API 用法时凭猜测编写代码（KVS SDK 日志回调需查阅头文件确认签名）

## 备注

- 标记 `*` 的子任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求编号，确保可追溯
- 检查点确保增量验证
- Property 测试验证纯函数的通用正确性属性，单元测试验证特定场景和边界条件
- 已使用 `spdlog::get("pipeline")` 的模块（webrtc_media、webrtc_signaling、pipeline_health 等）无需改动
