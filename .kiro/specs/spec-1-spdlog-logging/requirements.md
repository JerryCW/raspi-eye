# 需求文档：Spec 1 — spdlog 结构化日志

## 简介

本 Spec 为 device 模块引入 spdlog 结构化日志基础设施，替换现有的 `g_print`/`g_printerr` 调用。目标是建立一套模块化、可按模块独立控制级别、支持 pattern/JSON 双格式切换的日志系统，为后续所有模块（KVS、WebRTC、AI 等）提供统一的诊断基准。

spdlog 是 C++ 高性能日志库，支持同步/异步模式、多种 sink、自定义格式化。本 Spec 采用同步模式（stderr 输出场景下性能足够），默认使用人眼友好的 pattern 格式，可通过命令行参数 `--log-json` 切换为 JSON 单行格式（为后续 CloudWatch 集成做准备）。后续有文件 sink 需求时再升级为异步模式。

## 前置条件

- Spec 0 (gstreamer-capture) 已通过验证 ✅

## 术语表

- **spdlog**：C++ 高性能日志库，支持同步/异步模式、多种 sink、自定义格式化
- **Logger**：spdlog 中的命名日志实例，通过 `spdlog::get("name")` 获取，每个模块持有独立的 Logger
- **Sink**：spdlog 中的日志输出目标，如 stderr、文件等；多个 Logger 可共享同一个 Sink
- **Sync_Logger**：spdlog 同步日志模式，日志消息直接写入 Sink，无额外线程开销；stderr 输出场景下性能足够，后续有文件 sink 需求时再升级为异步模式
- **Log_Level**：日志级别，spdlog 支持 trace、debug、info、warn、err、critical、off 七个级别
- **JSON_Format**：日志输出的可选格式，每条日志为一行 JSON 对象，包含时间戳、级别、模块名、消息等字段，便于 CloudWatch 或 grep 过滤；通过 `--log-json` 命令行参数启用
- **Pattern_Format**：日志输出的默认格式，人眼友好的文本格式，如 `[2025-01-01 12:00:00.123] [pipeline] [info] Pipeline created`
- **Log_Initializer**：日志系统初始化模块，负责创建共享 Sink、注册各模块 Logger、根据参数选择格式
- **FetchContent**：CMake 模块，用于在配置阶段自动下载和集成外部依赖
- **PipelineManager**：GStreamer 管道生命周期管理器（Spec 0 已实现）
- **bus_callback**：main.cpp 中的 GStreamer bus 消息回调函数，处理 ERROR 和 EOS 消息

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（开发）/ Linux aarch64（Pi 5 生产） |
| 硬件限制 | Raspberry Pi 5，4GB RAM |
| spdlog 版本 | v1.15.0（FetchContent 锁定） |
| spdlog 集成方式 | FetchContent |
| Debug 构建 | 开启 ASan |
| 代码组织 | .h + .cpp 分离模式 |
| 日志输出语言 | 英文（禁止非 ASCII 字符） |
| 高性能路径 | 禁止同步磁盘 I/O |

## 禁止项

### Design 层

- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
  - 原因：日志可能被收集到云端或共享给他人
  - 建议：日志中只输出资源标识（如 thing-name），不输出凭证内容

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret
  - 原因：泄露风险，一旦提交到 git 无法撤回

### Tasks 层

- SHALL NOT 在日志消息中使用非 ASCII 字符
  - 原因：部分终端环境下不正确处理 UTF-8，显示为乱码
  - 建议：日志消息统一使用英文

- SHALL NOT 将 C++ 测试编译为独立可执行文件后逐个运行
  - 原因：容易遗漏测试、运行方式不统一
  - 建议：通过 GTest + CTest 统一管理

- SHALL NOT 将新建文件直接放到项目根目录
  - 原因：根目录应保持干净
  - 建议：日志模块源码放 `device/src/`，测试放 `device/tests/`

## 需求

### 需求 1：spdlog CMake 集成

**用户故事：** 作为开发者，我需要 spdlog 通过 FetchContent 自动集成到 CMake 构建系统中，以便无需手动安装即可使用 spdlog 库。

#### 验收标准

1. WHEN 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug` 时，THE CMake_Build_System SHALL 通过 FetchContent 自动下载 spdlog 并完成配置
2. WHEN 执行 `cmake --build device/build` 时，THE CMake_Build_System SHALL 成功编译所有依赖 spdlog 的源文件，无编译错误和链接错误
3. THE CMake_Build_System SHALL 将 spdlog 链接到 pipeline_manager 静态库和 raspi-eye 可执行文件，使所有模块均可使用 spdlog API

### 需求 2：日志系统初始化

**用户故事：** 作为开发者，我需要一个集中的日志初始化模块，以便在应用启动时一次性完成所有 Logger 的创建和配置。

#### 验收标准

1. THE Log_Initializer SHALL 创建一个共享的 stderr Sink，所有模块的 Logger 共用该 Sink 输出日志
2. THE Log_Initializer SHALL 使用 spdlog 同步模式创建 Logger，日志消息直接写入 stderr Sink（stderr 为 unbuffered，写入不阻塞调用线程；后续有文件 sink 需求时再升级为异步模式）
3. THE Log_Initializer SHALL 注册至少两个命名 Logger：`"main"` 和 `"pipeline"`，分别供 main.cpp 和 PipelineManager 使用
4. WHEN Log_Initializer 初始化完成后，THE Log_Initializer SHALL 使各模块能通过 `spdlog::get("logger_name")` 获取对应的 Logger 实例
5. THE Log_Initializer SHALL 提供 `.h` 头文件声明初始化接口，`.cpp` 文件实现初始化逻辑
6. THE Log_Initializer SHALL 提供工厂函数，后续模块可通过该函数创建新的命名 Logger 并自动共享同一个 Sink 和当前格式

### 需求 3：日志输出格式（默认 pattern + 可选 JSON）

**用户故事：** 作为开发者，我需要日志默认以人眼友好的 pattern 格式输出，同时支持通过命令行参数切换为 JSON 格式，以便在开发调试时直观阅读，在生产环境中支持结构化分析。

#### 验收标准

1. THE Log_Initializer SHALL 默认使用 pattern 格式输出日志，格式为：`[时间戳] [模块名] [级别] 消息`，其中时间戳精确到毫秒
2. WHEN 应用启动时传入 `--log-json` 命令行参数时，THE Log_Initializer SHALL 切换为 JSON 单行格式，每条日志输出为一行 JSON 对象，包含 `ts`（ISO 8601 时间戳）、`level`（日志级别）、`logger`（模块名）、`msg`（日志消息）字段
3. WHEN 使用 JSON 格式且日志消息包含双引号或反斜杠字符时，THE JSON_Format SHALL 对特殊字符进行 JSON 转义，确保输出为合法 JSON
4. THE Log_Initializer SHALL 确保日志系统自身产生的消息使用英文 ASCII 字符；外部库（如 GStreamer）传入的错误消息原样输出，不做字符过滤
5. WHEN 使用 JSON 格式时，THE JSON_Format SHALL 确保每条日志输出为单行，JSON 对象内不包含换行符

### 需求 4：按模块独立控制日志级别

**用户故事：** 作为开发者，我需要能够按模块独立设置日志级别，以便在调试特定模块时提高该模块的日志详细度而不被其他模块的日志淹没。

#### 验收标准

1. THE Log_Initializer SHALL 为每个命名 Logger 设置独立的默认日志级别（默认为 `info`）
2. WHEN 调用 spdlog API 修改某个 Logger 的日志级别时，THE Logger SHALL 仅改变该 Logger 的输出级别，不影响其他 Logger 的级别设置
3. WHEN 某个 Logger 的级别设置为 `warn` 时，THE Logger SHALL 仅输出 `warn`、`err`、`critical` 级别的日志，过滤掉 `info`、`debug`、`trace` 级别的日志

### 需求 5：替换现有 g_printerr 调用并建立模块日志使用范例

**用户故事：** 作为开发者，我需要将 main.cpp 中所有 `g_printerr` 调用替换为 spdlog 日志调用，并在 PipelineManager 中添加关键操作的诊断日志，以便统一日志输出格式并为后续模块建立使用范例。

#### 验收标准

1. WHEN GStreamer bus 上出现 ERROR 消息时，THE Main_Entry SHALL 使用 `"main"` Logger 以 `err` 级别输出包含 Element 名称和错误描述的日志
2. WHEN GStreamer bus 上出现 ERROR 消息且存在 debug 信息时，THE Main_Entry SHALL 使用 `"main"` Logger 以 `debug` 级别输出 debug 详情
3. WHEN GStreamer bus 上出现 EOS 消息时，THE Main_Entry SHALL 使用 `"main"` Logger 以 `info` 级别输出 end-of-stream 日志
4. WHEN 管道创建失败时，THE Main_Entry SHALL 使用 `"main"` Logger 以 `err` 级别输出包含失败原因的日志
5. WHEN 管道启动失败时，THE Main_Entry SHALL 使用 `"main"` Logger 以 `err` 级别输出包含失败原因的日志
6. WHEN main.cpp 中所有替换完成后，THE Main_Entry SHALL 不再包含任何 `g_print` 或 `g_printerr` 调用
7. THE PipelineManager SHALL 使用 `"pipeline"` Logger 在 create 成功时以 `info` 级别输出管道描述，在 start/stop 时以 `info` 级别输出状态变更日志，在错误路径以 `err` 级别输出错误详情

### 需求 6：日志系统关闭

**用户故事：** 作为开发者，我需要在应用退出前正确关闭日志系统，以确保异步缓冲区中的日志全部刷新到 stderr，且不触发 ASan 内存错误报告。

#### 验收标准

1. WHEN 应用正常退出时，THE Log_Initializer SHALL 提供关闭接口，调用 `spdlog::shutdown()` 释放所有 Logger 和 Sink 资源
2. WHEN 日志系统关闭完成后，THE Log_Initializer SHALL 确保 ASan 运行时不报告任何与 spdlog 相关的内存泄漏或 use-after-free 错误
3. THE Log_Initializer SHALL 确保关闭接口可安全多次调用（幂等），不触发崩溃

### 需求 7：冒烟测试

**用户故事：** 作为开发者，我需要自动化测试来验证日志系统的核心功能，以便在每次构建后快速确认日志基础设施正常工作。

#### 验收标准

1. WHEN 执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 运行所有日志相关测试并报告结果
2. THE Test_Suite SHALL 验证：Log_Initializer 初始化后，通过 `spdlog::get("main")` 和 `spdlog::get("pipeline")` 能获取到非空的 Logger 实例
3. THE Test_Suite SHALL 验证：使用 JSON 格式时，Logger 输出的日志为合法的单行 JSON 格式，包含 `ts`、`level`、`logger`、`msg` 字段（通过 `ostream_sink` 或自定义 test sink 捕获输出后解析验证）
4. THE Test_Suite SHALL 验证：使用默认 pattern 格式时，Logger 输出的日志包含时间戳、模块名、级别和消息内容
5. THE Test_Suite SHALL 验证：修改某个 Logger 的日志级别后，该 Logger 按新级别过滤日志，其他 Logger 不受影响
6. THE Test_Suite SHALL 验证：日志系统初始化和关闭后，ASan 不报告任何内存错误
7. THE Test_Suite SHALL 验证：Spec 0 的冒烟测试（PipelineManager 创建、启动、停止）仍然通过，日志替换未破坏已有功能

## 验证命令

```bash
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```

预期结果：配置成功（spdlog 自动下载）、编译无错误、所有测试通过（含 Spec 0 冒烟测试）、ASan 无报告。

## 明确不包含

- 交叉编译工具链（Spec 2）
- H.264 编码和 tee 分流（Spec 3）
- 日志文件轮转（Pi 5 上暂不需要，stderr 由 systemd journal 管理）
- 远程日志收集（CloudWatch 集成）
- GStreamer 内部日志重定向到 spdlog（复杂度高，后续考虑）
- 环境变量动态配置日志级别（后续 Spec 按需添加）
