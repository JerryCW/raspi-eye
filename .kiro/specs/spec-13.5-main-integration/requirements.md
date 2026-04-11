# 需求文档

## 简介

Spec 13.5: main-integration — 架构重构。引入 `AppContext`（应用上下文）和 `ShutdownHandler`（注册式清理），将 `main.cpp` 从"胶水代码集成"升级为"三阶段生命周期管理"。`main.cpp` 变薄为：GStreamer 初始化 → 命令行解析 → `AppContext::init()` → `AppContext::start()` → 主循环 → `AppContext::stop()`。所有模块创建、连线、回调注册、清理逻辑封装在 `AppContext` 内部。后续 Spec（AI 管道 spec-10、systemd 看门狗 spec-19）只需在 `AppContext` 中添加模块，`main.cpp` 不再修改。

## 前置条件

- Spec 8（KVS Producer）✅
- Spec 12（WebRTC 信令）✅
- Spec 13（WebRTC 媒体）✅

## 术语表

- **App_Context**: `AppContext` 类实例，持有所有模块的 `unique_ptr`/`shared_ptr`，提供 `init(config)` → `start()` → `stop()` 三阶段接口
- **Shutdown_Handler**: `ShutdownHandler` 类实例，管理注册式逆序清理，每步有超时保护
- **Shutdown_Step**: 通过 `ShutdownHandler::register_step` 注册的单个清理动作（名称 + lambda）
- **Main_App**: `main.cpp` 中的应用入口逻辑（`run_pipeline` 函数），重构后只负责 GStreamer 初始化、命令行解析、调用 `AppContext` 三阶段接口、运行主循环
- **Config_Loader**: 使用 `parse_toml_section` + `build_*_config` 系列函数从 `config.toml` 加载配置的逻辑，封装在 `AppContext::init()` 内部
- **Signaling_Client**: `WebRtcSignaling` 实例，负责 KVS WebRTC 信令通道连接
- **Media_Manager**: `WebRtcMediaManager` 实例，负责 PeerConnection 生命周期和 H.264 帧广播
- **Health_Monitor**: `PipelineHealthMonitor` 实例，负责管道健康监控与自动恢复
- **Pipeline_Builder**: `PipelineBuilder::build_tee_pipeline` 函数，构建双 tee 管道
- **Config_Path**: `config.toml` 文件路径，默认值 `device/config/config.toml`，可通过 `--config` 命令行参数覆盖
- **GMain_Loop**: GLib 主事件循环（`GMainLoop*`），作为唯一允许的全局变量存在于 `main.cpp` 中

## 需求

### 需求 1：AppContext 三阶段生命周期

**用户故事：** 作为开发者，我希望所有模块的创建、启动、停止逻辑集中在 AppContext 中管理，以便 main.cpp 保持极简，后续添加新模块时不修改 main.cpp。

#### 验收标准

1. THE App_Context SHALL 提供 `init` 方法，接受 Config_Path、`CameraSource::CameraConfig` 参数，按依赖顺序创建所有模块并返回 bool 表示成功或失败
2. THE App_Context SHALL 提供 `start` 方法，启动管道、连接信令、启动健康监控，并返回 bool 表示成功或失败
3. THE App_Context SHALL 提供 `stop` 方法，委托给 Shutdown_Handler 按注册逆序执行所有清理步骤
4. THE App_Context SHALL 通过 `unique_ptr` 或 `shared_ptr` 持有所有模块实例，集中管理生命周期和依赖关系
5. THE App_Context SHALL NOT 使用全局变量或单例模式（GMain_Loop 除外，GMain_Loop 保留在 `main.cpp` 中）
6. THE App_Context SHALL 删除拷贝构造函数和拷贝赋值运算符

### 需求 2：AppContext::init — 配置加载与模块创建

**用户故事：** 作为设备运维人员，我希望 AppContext 在 init 阶段自动读取 config.toml 并按依赖顺序创建所有模块，以便无需修改代码即可调整部署参数。

#### 验收标准

1. WHEN `init` 被调用时，THE App_Context SHALL 从 Config_Path 解析 `[aws]` section 并调用 `build_aws_config` 构建 `AwsConfig` 结构体
2. WHEN `init` 被调用时，THE App_Context SHALL 从 Config_Path 解析 `[kvs]` section 并调用 `build_kvs_config` 构建 `KvsConfig` 结构体
3. WHEN `init` 被调用时，THE App_Context SHALL 从 Config_Path 解析 `[webrtc]` section 并调用 `build_webrtc_config` 构建 `WebRtcConfig` 结构体
4. IF 任一 section 解析失败或 `build_*_config` 返回 false，THEN THE App_Context SHALL 通过 `error_msg` 输出参数报告错误并从 `init` 返回 false
5. WHEN 配置加载成功后，THE App_Context SHALL 按以下顺序创建模块：Signaling_Client → Media_Manager
6. IF `WebRtcSignaling::create` 返回 nullptr，THEN THE App_Context SHALL 通过 `error_msg` 报告错误并从 `init` 返回 false
7. IF `WebRtcMediaManager::create` 返回 nullptr，THEN THE App_Context SHALL 通过 `error_msg` 报告错误并从 `init` 返回 false
8. WHEN 模块创建成功后，THE App_Context SHALL 注册 offer 回调（调用 `media_manager->on_viewer_offer`）和 ICE candidate 回调（调用 `media_manager->on_viewer_ice_candidate`）
9. WHEN 模块创建成功后，THE App_Context SHALL 为每个需要清理的模块向 Shutdown_Handler 注册对应的 Shutdown_Step
10. WHEN `init` 成功完成时，THE App_Context SHALL 记录 info 级别日志，包含 KVS stream name 和 WebRTC channel name

### 需求 3：AppContext::start — 管道启动与信令连接

**用户故事：** 作为设备运维人员，我希望 AppContext 在 start 阶段启动管道并连接信令，以便设备开始录制和实时观看。

#### 验收标准

1. WHEN `start` 被调用时，THE App_Context SHALL 调用 `build_tee_pipeline` 构建管道，传入 `KvsConfig` 指针、`AwsConfig` 指针、`WebRtcMediaManager` 指针
2. WHEN 管道构建成功后，THE App_Context SHALL 创建 `PipelineManager` 并调用 `start` 启动管道
3. WHEN 管道启动成功后，THE App_Context SHALL 创建 Health_Monitor，注册 rebuild 回调和 health 回调，并调用 `health_monitor.start("src")`
4. WHEN rebuild 回调被触发时，THE rebuild 回调 SHALL 将 `KvsConfig` 指针、`AwsConfig` 指针、`WebRtcMediaManager` 指针传入 `build_tee_pipeline`
5. WHEN `start` 被调用时，THE App_Context SHALL 调用 `signaling->connect()` 连接信令通道
6. IF `signaling->connect()` 返回 false，THEN THE App_Context SHALL 记录 warn 级别日志但继续运行（信令连接失败不阻止管道启动）
7. IF `build_tee_pipeline` 返回 nullptr，THEN THE App_Context SHALL 通过 `error_msg` 报告错误并从 `start` 返回 false
8. IF `PipelineManager::start` 返回 false，THEN THE App_Context SHALL 通过 `error_msg` 报告错误并从 `start` 返回 false
9. WHEN `start` 成功完成时，THE App_Context SHALL 记录 info 级别日志，指示管道和各模块已启动

### 需求 4：ShutdownHandler — 注册式逆序清理

**用户故事：** 作为开发者，我希望各模块在 init/start 阶段注册清理步骤，shutdown 时按逆序执行，以便资源释放顺序正确且可扩展。

#### 验收标准

1. THE Shutdown_Handler SHALL 提供 `register_step` 方法，接受步骤名称（`std::string`）和清理动作（`std::function<void()>`）
2. WHEN `execute` 被调用时，THE Shutdown_Handler SHALL 按注册的逆序执行所有 Shutdown_Step
3. WHEN 单个 Shutdown_Step 执行时间超过 5 秒时，THE Shutdown_Handler SHALL 终止该步骤并记录 warn 级别日志，继续执行下一步
4. IF 单个 Shutdown_Step 抛出异常，THEN THE Shutdown_Handler SHALL 捕获异常并记录 error 级别日志，继续执行下一步（不崩溃）
5. WHEN 所有 Shutdown_Step 执行完毕后，THE Shutdown_Handler SHALL 记录 info 级别的 shutdown summary，包含每步的名称、执行结果（成功/超时/异常）和耗时
6. THE Shutdown_Handler SHALL 确保所有 Shutdown_Step 的总执行时间不超过 30 秒
7. THE Shutdown_Handler SHALL 删除拷贝构造函数和拷贝赋值运算符

### 需求 5：main.cpp 变薄

**用户故事：** 作为开发者，我希望 main.cpp 只包含最小的入口逻辑，以便所有业务逻辑集中在 AppContext 中，降低 main.cpp 的修改频率。

#### 验收标准

1. THE Main_App SHALL 仅执行以下步骤：GStreamer 初始化 → 命令行解析（`--log-json`、`--camera`、`--device`、`--config`）→ 日志初始化 → 参数验证 → `AppContext::init()` → `AppContext::start()` → 主循环（`g_main_loop_run`）→ `AppContext::stop()` → 日志关闭
2. THE Main_App SHALL NOT 直接创建或持有任何模块实例（Signaling_Client、Media_Manager、Health_Monitor、PipelineManager 等）
3. THE Main_App SHALL NOT 直接调用任何模块的接口方法（`signaling->connect()`、`media_manager->on_viewer_offer()` 等）
4. WHEN `--config <path>` 命令行参数存在时，THE Main_App SHALL 使用指定路径作为 Config_Path
5. WHEN `--config` 命令行参数不存在时，THE Main_App SHALL 使用默认路径 `device/config/config.toml` 作为 Config_Path
6. IF `AppContext::init()` 返回 false，THEN THE Main_App SHALL 记录错误日志并以退出码 1 终止
7. IF `AppContext::start()` 返回 false，THEN THE Main_App SHALL 记录错误日志并以退出码 1 终止

### 需求 6：信号处理

**用户故事：** 作为设备运维人员，我希望程序在收到 SIGINT/SIGTERM 时优雅退出，以便资源正确释放。

#### 验收标准

1. WHEN Main_App 收到 SIGINT 信号时，THE Main_App SHALL 退出 GMain_Loop
2. WHEN Main_App 收到 SIGTERM 信号时，THE Main_App SHALL 退出 GMain_Loop
3. WHEN GMain_Loop 退出后，THE Main_App SHALL 调用 `AppContext::stop()` 触发 Shutdown_Handler 执行清理
4. THE Main_App SHALL 在主循环中不执行任何阻塞操作（使用 GMain_Loop 事件驱动或 sleep loop）

### 需求 7：日志安全

**用户故事：** 作为安全审计人员，我希望日志中不包含敏感信息，以便日志可以安全地收集和共享。

#### 验收标准

1. THE App_Context SHALL NOT 在日志中输出 config.toml 中的证书路径（`cert_path`、`key_path`、`ca_path`）
2. THE App_Context SHALL NOT 在日志中输出密钥内容、token、SDP 完整内容、ICE candidate 完整内容
3. THE App_Context SHALL 在日志中仅输出资源标识信息（KVS stream name、WebRTC channel name、peer_id）
4. THE Shutdown_Handler SHALL NOT 在 shutdown summary 日志中包含任何敏感信息

### 需求 8：可扩展性

**用户故事：** 作为开发者，我希望后续 Spec 接入新模块时只需修改 AppContext 内部，不修改 main.cpp，以便降低集成风险。

#### 验收标准

1. THE App_Context SHALL 将模块初始化顺序封装在 `init` 方法内部，由内部依赖关系决定，不依赖 Main_App 的调用顺序
2. WHEN 后续 spec-10（AI 管道）接入时，THE App_Context SHALL 仅需在 `init` 方法中添加 AI 模块创建和注册 Shutdown_Step，不修改 Main_App
3. WHEN 后续 spec-19（systemd 看门狗）接入时，THE App_Context SHALL 仅需在 `init` 或 `start` 方法中添加 watchdog 模块，不修改 Main_App
4. THE App_Context SHALL NOT 暴露内部模块的裸指针给外部（Main_App 只通过 `init`/`start`/`stop` 方法与 App_Context 交互）

### 需求 9：性能要求

**用户故事：** 作为设备运维人员，我希望应用启动和关闭在合理时间内完成，以便设备快速就绪和安全关机。

#### 验收标准

1. THE App_Context SHALL 确保 `init` 方法总耗时不超过 3 秒（不含网络请求）
2. THE Shutdown_Handler SHALL 确保所有 Shutdown_Step 总执行时间不超过 30 秒
3. THE Shutdown_Handler SHALL 确保单个 Shutdown_Step 执行时间不超过 5 秒
4. THE Main_App SHALL 在主循环中不执行任何阻塞操作（GMain_Loop 事件驱动）

### 需求 10：本 Spec 集成的模块（KVS + WebRTC）

**用户故事：** 作为设备运维人员，我希望 KVS 录制和 WebRTC 实时观看两路从 fakesink 替换为真实模块，以便设备具备完整的录制和实时观看能力。

#### 验收标准

1. WHEN `init` 被调用时，THE App_Context SHALL 读取 config.toml 的 `[aws]`、`[kvs]`、`[webrtc]` 三个 section
2. WHEN `start` 被调用时，THE App_Context SHALL 将 `KvsConfig`、`AwsConfig`、`WebRtcMediaManager` 传入 `build_tee_pipeline`，替换 KVS 和 WebRTC 分支的 fakesink
3. WHEN Health_Monitor 触发 rebuild 回调时，THE rebuild 回调 SHALL 将完整的 `KvsConfig`、`AwsConfig`、`WebRtcMediaManager` 参数传入 `build_tee_pipeline`
4. THE App_Context SHALL 保持 AI 分支为 fakesink（等 spec-10 完成后再接入）

## 约束

- 目标平台：macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release）
- 语言：C++17
- macOS 上 KVS 和 WebRTC 都是 stub 实现，不需要真实 AWS 环境
- Pi 5 上需要真实 AWS 环境（IoT 证书 + KVS 流 + WebRTC 信令通道）
- 配置文件默认路径 `device/config/config.toml`（已在 `.gitignore` 中排除）
- AI 分支（raw-tee 的 fakesink）保持不变，等 spec-10 完成后再接入
- AppContext::init() 总耗时 ≤ 3 秒（不含网络请求）
- ShutdownHandler 总超时 ≤ 30 秒
- 单个 shutdown step 超时 ≤ 5 秒
- 主循环不做任何阻塞操作（GMainLoop 事件驱动）
- AppContext 不暴露内部模块的裸指针给外部
- ShutdownHandler 的 shutdown step 在异常时不崩溃（catch + log）

## 禁止项

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径
- SHALL NOT 在日志中打印密钥、证书内容、token、SDP 完整内容
- SHALL NOT 修改现有测试文件
- SHALL NOT 在 macOS 上尝试链接 KVS WebRTC C SDK
- SHALL NOT 在本 Spec 中实现自适应码率或流模式切换（spec-14 范围）
- SHALL NOT 在本 Spec 中集成 AI 分支（spec-10 范围）
- SHALL NOT 在 AppContext 中使用全局变量或单例模式（GMainLoop 除外，保留在 main.cpp）
- SHALL NOT 修改 `build_tee_pipeline`、`WebRtcSignaling`、`WebRtcMediaManager`、`KvsSinkFactory`、`PipelineHealthMonitor` 等已有模块的接口或实现
- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符

## 明确不包含

- AI 分支集成（spec-10）
- 自适应码率控制与流模式切换（spec-14）
- WebRtcMediaManager 与 PipelineHealthMonitor 的流状态反馈集成（spec-14）
- CredentialProvider 后台刷新集成（当前 KVS 和 WebRTC 通过 IoT 证书直接认证，不需要 STS 临时凭证中转）
- systemd 看门狗集成（spec-19）
- 配置文件热重载（spec-18）
- AppContext 的模块依赖图自动拓扑排序（当前模块数量少，手动编排顺序即可）
