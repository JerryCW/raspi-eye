# 需求文档：Spec 23 — 统一日志管理

## 简介

当前系统的日志管理存在三个问题：

1. **全局级别控制粒度不足**：`log_init::init(const LoggingConfig&)` 通过 `spdlog::set_level()` 设置全局级别，无法按模块独立控制。AI 推理结果用 `spdlog::debug` 输出，想看推理详情必须把全局级别调到 debug，导致其他模块（WebRTC、KVS）的 debug 日志也涌出来。
2. **KVS SDK 日志独立输出**：KVS WebRTC C SDK 有自己的日志系统（`platformCallbacksProvider` + `logPrintFunc`），当前未重定向到 spdlog，导致 SDK 日志格式不统一、级别不可控。
3. **AI 检测结果日志级别不合理**：检测到目标时只有 event 级别的 info 日志（"Event opened"），每帧推理结果（检测数量、耗时）用 `spdlog::debug` 输出，默认 info 级别下看不到推理详情。

本 Spec 实现 per-component 日志级别配置、KVS SDK 日志重定向、AI 检测结果日志级别调整，统一所有模块的日志格式和级别控制。

## 前置条件

- Spec 1 (spdlog-logging) 已通过验证 ✅
- Spec 19 (config-file) 已通过验证 ✅

## 术语表

- **Component_Logger**: 按模块命名的 spdlog logger 实例，每个模块持有独立的 logger，可独立设置日志级别。当前已有 `"main"`、`"pipeline"`、`"app"`、`"config"`、`"stream"` 五个 logger
- **Per_Component_Level**: 按模块独立配置日志级别的能力，通过 config.toml 的 `[logging]` section 中的 `component_levels` 字段实现
- **KVS_SDK_Logger**: KVS WebRTC C SDK 内部的日志系统，通过 `logPrintFunc` 回调函数输出日志。SDK 使用 `DLOGV`/`DLOGI`/`DLOGW`/`DLOGE` 等宏输出日志
- **Log_Redirector**: 将外部 SDK 的日志回调重定向到 spdlog 的适配层，统一日志格式和级别控制
- **LoggingConfig**: 已有的日志配置结构体（定义在 `config_manager.h`），当前包含 `level`（全局级别）和 `format`（text/json）两个字段
- **log_init**: 已有的日志初始化命名空间（定义在 `log_init.h`），提供 `init()`、`create_logger()`、`shutdown()` 接口

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| 目标平台 | macOS（开发）/ Linux aarch64（Pi 5 生产） |
| 硬件限制 | Raspberry Pi 5，4GB RAM |
| spdlog 版本 | v1.15.0（FetchContent） |
| KVS WebRTC C SDK | 条件编译（`HAVE_KVS_WEBRTC_SDK`），macOS 无 SDK |
| Debug 构建 | 开启 ASan |
| 代码组织 | .h + .cpp 分离模式 |
| 日志输出语言 | 英文（禁止非 ASCII 字符） |
| 高性能路径 | 禁止同步磁盘 I/O |
| 依赖 | 不引入新的外部库，复用现有 spdlog + parse_toml_section |

## 禁止项

### Requirements 层

- SHALL NOT 实现日志文件轮转或文件 sink（Pi 5 上 stderr 由 systemd journal 管理）
- SHALL NOT 实现运行时动态修改日志级别（热重载），仅支持启动时从 config.toml 加载
- SHALL NOT 实现远程日志收集或 CloudWatch 集成

### Design 层

- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
  - 原因：日志可能被收集到云端或共享给他人
  - 建议：日志中只输出资源标识（如 thing-name），不输出凭证内容
- SHALL NOT 在不确定外部 SDK/库 API 用法时凭猜测编写代码
  - 原因：KVS SDK 的日志回调 API 需要查阅 SDK 头文件确认签名
  - 建议：先查阅 `com/amazonaws/kinesis/video/webrtcclient/Include.h` 确认 API

### Tasks 层

- SHALL NOT 在日志消息中使用非 ASCII 字符
- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 修改现有模块的核心逻辑（仅修改日志相关代码）

## 需求

### 需求 1：Per-Component 日志级别配置

**用户故事：** 作为开发者，我希望在 config.toml 中按模块配置不同的日志级别，以便在调试 AI 推理时将 ai 模块设为 debug 级别，同时保持 webrtc 模块为 info 级别，避免被无关日志淹没。

#### 验收标准

1. WHEN config.toml 的 `[logging]` section 包含 `component_levels` 字段（格式为 `"component:level,component:level"`），THE LoggingConfig SHALL 解析该字段为组件名到级别的映射表
2. WHEN log_init 初始化完成后，THE log_init SHALL 根据 `component_levels` 映射表为每个已注册的 Component_Logger 设置独立的日志级别
3. WHEN `component_levels` 中指定了某个组件的级别，THE log_init SHALL 使用该组件级别覆盖全局 `level` 设置；未指定的组件使用全局 `level`
4. WHEN `component_levels` 中包含未注册的组件名，THE log_init SHALL 忽略该条目并以 warn 级别记录一条日志
5. IF `component_levels` 中包含无效的级别值（不是 trace/debug/info/warn/error），THEN THE ConfigManager SHALL 返回 false 并通过 error_msg 报告无效的级别值和对应的组件名
6. WHEN config.toml 的 `[logging]` section 缺少 `component_levels` 字段，THE log_init SHALL 对所有 Component_Logger 使用全局 `level` 设置（向后兼容）

### 需求 2：扩展 LoggingConfig 结构体

**用户故事：** 作为开发者，我希望 LoggingConfig 结构体支持 per-component 级别映射，以便 ConfigManager 能解析并传递组件级别配置。

#### 验收标准

1. THE LoggingConfig SHALL 新增 `component_levels` 字段，类型为 `std::unordered_map<std::string, std::string>`，存储组件名到级别字符串的映射
2. WHEN config.toml 缺少 `component_levels` 字段，THE LoggingConfig SHALL 保持 `component_levels` 为空映射（默认值）
3. THE parse_logging_config 函数 SHALL 解析 `component_levels` 字段值（逗号分隔的 `name:level` 对），并对每个 level 值进行合法性校验

### 需求 3：KVS SDK 日志重定向到 spdlog

**用户故事：** 作为开发者，我希望 KVS WebRTC C SDK 的内部日志通过 spdlog 输出，以便所有日志使用统一格式、统一级别控制，方便在 systemd journal 中 grep 过滤。

#### 验收标准

1. WHEN KVS WebRTC C SDK 可用（`HAVE_KVS_WEBRTC_SDK` 已定义），THE Log_Redirector SHALL 注册自定义的 `logPrintFunc` 回调，将 SDK 日志转发到名为 `"kvs"` 的 spdlog logger
2. WHEN SDK 输出日志时，THE Log_Redirector SHALL 将 SDK 的日志级别映射到 spdlog 级别：SDK VERBOSE → spdlog trace，SDK DEBUG → spdlog debug，SDK INFO → spdlog info，SDK WARN → spdlog warn，SDK ERROR → spdlog error
3. WHEN KVS WebRTC C SDK 不可用（macOS stub），THE Log_Redirector SHALL 不注册任何回调，不影响编译和运行
4. THE `"kvs"` logger SHALL 支持通过 `component_levels` 独立配置级别，与其他模块的 logger 互不影响
5. WHEN SDK 日志消息包含换行符，THE Log_Redirector SHALL 去除尾部换行符后再转发给 spdlog，确保每条日志为单行输出

### 需求 4：AI 检测结果日志级别调整

**用户故事：** 作为用户，我希望在默认 info 级别下能看到每帧推理耗时、检测结果和加载的模型信息，以便实时了解 AI 推理性能和检测状态，而不需要把全局日志调到 debug。

#### 验收标准

1. WHEN AI 推理完成且检测到目标（filtered 非空），THE AiPipelineHandler SHALL 使用 info 级别输出检测结果摘要，格式为：`Inference: {耗时}ms, detected: {类别名}({置信度}), ...`
2. WHEN AI 推理完成但未检测到目标（filtered 为空），THE AiPipelineHandler SHALL 使用 debug 级别输出推理完成日志（耗时、原始检测数），避免在无目标时产生大量 info 日志
2. WHEN AiPipelineHandler 创建成功，THE AiPipelineHandler SHALL 使用 info 级别输出模型加载信息，包含：模型文件路径、推理 FPS 配置、置信度阈值
3. WHEN YoloDetector 初始化完成，THE YoloDetector SHALL 使用 info 级别输出模型详情，包含：模型输入尺寸、线程数、execution provider
4. THE AiPipelineHandler SHALL 使用名为 `"ai"` 的 Component_Logger 输出所有日志，替代当前直接使用 `spdlog::debug` / `spdlog::info` 的全局调用方式
5. WHEN ai logger 级别设为 debug，THE AiPipelineHandler SHALL 额外输出：I420 到 RGB 转换耗时、NMS 处理耗时等推理子步骤详情

### 需求 5：各模块 Logger 命名规范化

**用户故事：** 作为开发者，我希望所有模块使用统一命名的 Component_Logger，以便通过 `component_levels` 精确控制每个模块的日志级别。

#### 验收标准

1. THE log_init SHALL 在初始化时创建以下命名 logger：`"main"`、`"pipeline"`、`"app"`、`"config"`、`"stream"`、`"ai"`、`"kvs"`、`"webrtc"`、`"s3"`
2. WHEN 各模块输出日志时，THE 各模块 SHALL 使用对应的命名 logger 而非 `spdlog::info()` 等全局函数：
   - `ai_pipeline_handler.cpp` → `"ai"` logger
   - `s3_uploader.cpp` → `"s3"` logger
   - `webrtc_media.cpp` / `webrtc_signaling.cpp` → `"webrtc"` logger
   - `pipeline_builder.cpp` / `pipeline_health.cpp` / `pipeline_manager.cpp` → `"pipeline"` logger
3. WHEN 模块通过 `spdlog::get("name")` 获取 logger 且 logger 不存在时，THE 模块 SHALL 回退到 spdlog 默认 logger，不崩溃

### 需求 6：配置文件格式扩展

**用户故事：** 作为运维人员，我希望在 config.toml 中直观地配置各模块的日志级别，格式简洁易懂。

#### 验收标准

1. THE config.toml 的 `[logging]` section SHALL 支持以下格式：
   ```toml
   [logging]
   level = "info"
   format = "text"
   component_levels = "ai:debug,kvs:warn,webrtc:info"
   ```
2. WHEN `component_levels` 字段中的组件名或级别值包含空格，THE parse_logging_config SHALL 去除空格后正常解析（容错处理）
3. WHEN `component_levels` 字段为空字符串，THE parse_logging_config SHALL 将其视为无组件级别配置（等同于缺失该字段）
4. THE config.toml.example SHALL 更新，包含 `component_levels` 字段的示例和注释说明

### 需求 7：向后兼容

**用户故事：** 作为现有用户，我希望现有的 config.toml 无需修改即可继续使用，新增的 `component_levels` 字段完全可选。

#### 验收标准

1. WHEN 现有 config.toml 的 `[logging]` section 不包含 `component_levels` 字段，THE 系统 SHALL 使用全局 `level` 设置所有 logger 的级别，行为与当前版本完全一致
2. WHEN config.toml 完全缺少 `[logging]` section，THE 系统 SHALL 使用默认值（level=info, format=text, component_levels 为空），行为与当前版本完全一致
3. THE 系统 SHALL 保持现有命令行参数 `--log-json` 的覆盖行为不变

### 需求 8：冒烟测试

**用户故事：** 作为开发者，我需要自动化测试验证 per-component 级别配置和 KVS 日志重定向的核心功能。

#### 验收标准

1. THE Test_Suite SHALL 验证：parse_logging_config 能正确解析 `component_levels` 字段，包括正常值、空值、含空格值、无效级别值
2. THE Test_Suite SHALL 验证：log_init 初始化后，通过 `component_levels` 设置的组件级别生效，未指定的组件使用全局级别
3. THE Test_Suite SHALL 验证：设置某个 logger 为 debug 级别后，该 logger 输出 debug 日志，其他 logger（info 级别）不输出 debug 日志
4. THE Test_Suite SHALL 验证：现有测试全部通过，日志管理改动未破坏已有功能
5. WHEN 执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 运行所有测试并报告结果，ASan 无报告

## 配置文件示例

```toml
[logging]
level = "info"                                    # 全局默认级别
format = "text"                                   # text | json
component_levels = "ai:debug,kvs:warn,s3:info"   # 按模块覆盖级别（可选）
```

效果：
- `"ai"` logger → debug 级别（能看到每帧推理详情）
- `"kvs"` logger → warn 级别（只看 KVS SDK 的警告和错误）
- `"s3"` logger → info 级别（显式指定，与全局一致）
- 其他 logger（main、pipeline、webrtc 等）→ info 级别（使用全局默认）

## 验证命令

```bash
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```

预期结果：编译无错误、所有测试通过（含现有测试）、ASan 无报告。

## 明确不包含

- 日志文件轮转或文件 sink（stderr 由 systemd journal 管理）
- 运行时动态修改日志级别（热重载）
- 远程日志收集（CloudWatch 集成）
- GStreamer 内部日志重定向到 spdlog（复杂度高，GST_DEBUG 环境变量已够用）
- 日志采样或限流（当前日志量不大，Pi 5 性能足够）
