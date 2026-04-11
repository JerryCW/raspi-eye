# 需求文档：Spec 12 — KVS WebRTC 信令通道

## 简介

本 Spec 集成 AWS KVS WebRTC C SDK，实现 WebRTC 信令通道的创建、连接和 ICE 候选交换。这是 WebRTC 实时观看功能的信令层，设备端作为 Master 角色连接到信令通道，等待 Viewer（最多 10 个并发）连接。后续 Spec 13 在此基础上实现媒体流传输。

KVS WebRTC C SDK 是独立的 C 库（不是 GStreamer 插件），需要从源码编译。SDK 提供完整的信令客户端（SignalingClient），内部处理：
- 信令通道的 Describe/GetEndpoint/GetIceServerConfig/ConnectAsMaster 等 API 调用
- WebSocket 连接管理（连接、重连、心跳）
- SDP Offer/Answer 和 ICE Candidate 的消息收发

认证方式：使用 SDK 内置的 IoT 凭证提供者，直接传入 IoT 证书路径，由 SDK 内部处理 STS 临时凭证获取和自动刷新。不通过 CredentialProvider（spec-7）中转 — SDK 的 IoT 凭证提供者已内置此功能。SDK 提供两种 IoT 凭证提供者实现（`createLwsIotCredentialProvider` 基于 libwebsockets，`createCurlIotCredentialProvider` 基于 libcurl），具体选择取决于 Pi 5 上 SDK 的编译配置，在 design 文档中确定。

> 架构决策说明：与 kvssink（spec-8）类似，KVS WebRTC C SDK 原生支持 IoT 证书认证。CredentialProvider（spec-7）保留给不支持 IoT 认证的组件使用（如 S3 上传等）。

平台策略：
- **Pi 5（Linux aarch64）**：使用完整的 KVS WebRTC C SDK，从源码编译产出 `libkvsWebrtcClient.so` 和 `libkvsWebrtcSignalingClient.so`，运行时动态链接
- **macOS（开发环境）**：KVS WebRTC C SDK 在 macOS 上可编译但依赖复杂，本 Spec 使用 stub 实现，保持接口一致

本 Spec 只做信令层（信令通道创建/连接/ICE 交换），不做媒体流传输（Spec 13）。

## 前置条件

- Spec 7（credential-provider）✅ — 设备端凭证模块（IoT 证书体系、config.toml 的 `[aws]` section）
- Spec 5（pipeline-health）✅ — 管道健康监控（PipelineHealthMonitor 接口）

## 术语表

- **KVS WebRTC C SDK**：Amazon Kinesis Video Streams WebRTC SDK in C，从源码编译产出信令客户端和 WebRTC 客户端库
- **SignalingClient**：KVS WebRTC C SDK 提供的信令客户端，封装了信令通道的 Describe、GetEndpoint、GetIceServerConfig、ConnectAsMaster 等 API 调用和 WebSocket 连接管理
- **Signaling Channel**：KVS WebRTC 信令通道资源，通过 AWS API 创建，用于 Master 和 Viewer 之间交换 SDP 和 ICE Candidate
- **Master**：信令通道中的主端角色，发起连接并等待 Viewer 加入。一个信令通道只能有一个 Master
- **Viewer**：信令通道中的观看端角色，连接到 Master 进行媒体交换。一个信令通道最多 10 个并发 Viewer
- **SDP（Session Description Protocol）**：会话描述协议，描述媒体会话的编解码器、格式等参数
- **ICE（Interactive Connectivity Establishment）**：交互式连接建立框架，用于 NAT 穿透和网络路径发现
- **ICE Candidate**：ICE 候选地址，包含 IP/端口对，用于建立点对点连接
- **STUN/TURN**：NAT 穿透协议，STUN 用于发现公网地址，TURN 用于中继媒体流
- **WebRtcConfig**：本 Spec 新增的配置结构体，包含信令通道名称、AWS 区域等参数
- **WebRtcSignaling**：本 Spec 新增的信令管理模块，封装 KVS WebRTC C SDK 的信令客户端生命周期
- **IoT 凭证提供者**：KVS WebRTC C SDK 内置的 IoT 凭证提供者（`createLwsIotCredentialProvider` 或 `createCurlIotCredentialProvider`，取决于 SDK 编译配置），通过 IoT 证书获取 STS 临时凭证
- **PipelineHealthMonitor**：Spec 5 交付的管道健康监控器，本 Spec 中用于监控信令连接状态
- **AwsConfig**：Spec 7 交付的 AWS 配置结构体，包含 thing_name、credential_endpoint、role_alias、证书路径等

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| 信令连接建立 | ≤ 5 秒（从调用 connect 到 SignalingClient 进入 CONNECTED 状态） |
| 最大并发 Viewer | 10（KVS WebRTC 服务限制） |
| 单个测试耗时 | ≤ 5 秒 |
| 代码组织 | .h + .cpp 分离模式 |
| 资源管理 | RAII，SDK 资源（CredentialProvider、SignalingClient）在析构函数中释放 |
| 日志语言 | 英文，不使用非 ASCII 字符 |
| KVS WebRTC C SDK | 仅 Linux 可用（从源码编译，动态链接），macOS 使用 stub |
| 库目录 | `device/plugins/`（统一存放运行时依赖库，和 kvssink 同模式） |
| 认证方式 | SDK 内置 IoT 凭证提供者（具体 API 取决于 SDK 编译配置），证书路径来自 config.toml 的 `[aws]` section |
| 配置来源 | `device/config/config.toml` 的 `[webrtc]` section（信令通道名称、区域）+ `[aws]` section（证书路径，已有） |
| 新增代码量 | 100-500 行 |
| 涉及文件 | 2-5 个 |
| 向后兼容 | 不修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、kvs_test、yolo_test） |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现媒体流传输（属于 Spec 13: webrtc-media）
- SHALL NOT 在本 Spec 中实现自适应码率控制（属于 Spec 14: adaptive-streaming）
- SHALL NOT 在本 Spec 中实现 PeerConnection 的创建和媒体轨道添加（属于 Spec 13）
- SHALL NOT 在本 Spec 中替换 PipelineBuilder 中 webrtc 分支的 fakesink（属于 Spec 13）
- SHALL NOT 在本 Spec 中实现 DataChannel 功能

### Design 层

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret（来源：安全基线）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）
- SHALL NOT 在 macOS 上尝试链接 KVS WebRTC C SDK（macOS 使用 stub 实现）
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、kvs_test、yolo_test）
- SHALL NOT 在不确定 KVS WebRTC C SDK API 用法时凭猜测编写代码（来源：shall-not.md）
- SHALL NOT 通过 CredentialProvider（spec-7）中转凭证给 KVS WebRTC C SDK（SDK 原生 IoT 凭证提供者已内置凭证刷新）

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 KVS WebRTC C SDK C API 外）
- SHALL NOT 编译 KVS WebRTC C SDK 时使用 `-DBUILD_DEPENDENCIES=ON` 保留自编译依赖（来源：spec-8 经验，自编译 libcurl TLS 后端可能与系统 OpenSSL 不兼容）

## 需求

### 需求 1：WebRTC 配置加载

**用户故事：** 作为设备端程序，我需要从配置文件读取 WebRTC 信令相关参数，以便初始化信令客户端。

#### 验收标准

1. THE WebRtcSignaling 模块 SHALL 从 `device/config/config.toml` 的 `[webrtc]` section 读取以下字段：`channel_name`（信令通道名称）、`aws_region`（AWS 区域）
2. IF `[webrtc]` section 不存在，THEN THE WebRtcSignaling 模块 SHALL 返回明确的错误信息，包含缺失的 section 名称
3. IF `[webrtc]` section 中缺少 `channel_name` 或 `aws_region` 字段，THEN THE WebRtcSignaling 模块 SHALL 返回明确的错误信息，包含缺失的字段名称
4. THE WebRtcSignaling 模块 SHALL 复用 Spec 7 的 TOML 解析函数（`parse_toml_section`）读取 `[webrtc]` section
5. THE `device/config/config.toml.example` SHALL 包含 `[webrtc]` section 的示例配置，包含 `channel_name` 和 `aws_region` 字段及注释说明


### 需求 2：信令客户端生命周期管理（平台条件编译）

**用户故事：** 作为设备端程序，我需要根据运行平台创建合适的信令客户端，以便在 Pi 5 上使用真实 KVS WebRTC SDK，在 macOS 上使用 stub 保持接口一致。

#### 验收标准

1. THE WebRtcSignaling 模块 SHALL 定义 `WebRtcSignaling` 类，提供 `create()`、`connect()`、`disconnect()`、`is_connected()` 接口
2. WHEN 在 Linux 平台上运行时，THE WebRtcSignaling 类 SHALL 使用 KVS WebRTC C SDK 创建真实的 SignalingClient
3. WHEN 在 macOS 平台上运行时，THE WebRtcSignaling 类 SHALL 使用 stub 实现，`connect()` 立即返回成功，`is_connected()` 返回 true
4. THE WebRtcSignaling 模块 SHALL 通过条件编译（`#ifdef __linux__` / `#ifdef __APPLE__`）隔离平台特定代码
5. THE WebRtcSignaling 类 SHALL 遵循 RAII 语义，析构函数中释放所有 SDK 资源（CredentialProvider、SignalingClient）
6. THE WebRtcSignaling 类 SHALL 禁用拷贝构造和拷贝赋值（`= delete`）
7. THE WebRtcSignaling 模块 SHALL 通过 spdlog 记录创建的客户端类型（"KVS WebRTC SignalingClient" 或 "WebRTC stub"）

### 需求 3：IoT 凭证提供者初始化

**用户故事：** 作为设备端程序，我需要使用 IoT 证书初始化 KVS WebRTC C SDK 的凭证提供者，以便信令客户端能够认证并连接到 KVS 信令服务。

#### 验收标准

1. WHEN 在 Linux 平台创建 WebRtcSignaling 时，THE WebRtcSignaling 类 SHALL 调用 SDK 提供的 IoT 凭证提供者初始化函数（`createLwsIotCredentialProvider` 或 `createCurlIotCredentialProvider`，取决于 SDK 编译配置），传入以下参数：IoT 凭证端点（`credential_endpoint`）、证书路径（`cert_path`）、私钥路径（`key_path`）、CA 证书路径（`ca_path`）、角色别名（`role_alias`）、Thing 名称（`thing_name`），所有参数来自 config.toml 的 `[aws]` section
2. IF IoT 凭证提供者创建失败，THEN THE WebRtcSignaling 类 SHALL 返回明确的错误信息，包含 SDK 返回的状态码
3. THE WebRtcSignaling 类 SHALL 在析构时调用对应的 `freeIotCredentialProvider` 释放凭证提供者资源
4. THE WebRtcSignaling 类 SHALL 通过 spdlog 记录凭证提供者初始化成功（仅记录 thing_name，不记录证书路径和凭证内容）
5. WHEN 在 macOS 平台时，THE WebRtcSignaling 类 SHALL 跳过凭证提供者初始化（stub 不需要真实凭证）

### 需求 4：信令通道连接（Master 角色）

**用户故事：** 作为设备端程序，我需要以 Master 角色连接到信令通道，以便等待 Viewer 连接并交换 SDP 和 ICE Candidate。

#### 验收标准

1. WHEN 调用 `connect()` 时，THE WebRtcSignaling 类 SHALL 创建 SignalingClient 并以 `SIGNALING_CHANNEL_ROLE_TYPE_MASTER` 角色连接到信令通道
2. THE WebRtcSignaling 类 SHALL 配置 SignalingClient 的以下回调：
   - `signalingClientStateChanged`：信令客户端状态变化回调
   - `signalingMessageReceived`：信令消息接收回调（SDP Offer、ICE Candidate）
3. WHEN SignalingClient 成功连接（状态变为 `SIGNALING_CLIENT_STATE_CONNECTED`）时，THE WebRtcSignaling 类 SHALL 通过 spdlog 记录 "Signaling client connected to channel: {channel_name}"
4. IF SignalingClient 连接失败，THEN THE WebRtcSignaling 类 SHALL 返回明确的错误信息，包含 SDK 返回的状态码和信令通道名称
5. THE WebRtcSignaling 类 SHALL 配置 `trickleIce` 为 true，启用 Trickle ICE 模式（逐步发送 ICE Candidate，减少连接建立延迟）
6. THE WebRtcSignaling 类 SHALL 配置信令通道名称为 config.toml 中的 `channel_name`

### 需求 5：信令消息回调注册

**用户故事：** 作为设备端程序，我需要注册回调函数来处理收到的信令消息（SDP Offer 和 ICE Candidate），以便后续 Spec 13 在此基础上建立 PeerConnection。

#### 验收标准

1. THE WebRtcSignaling 类 SHALL 提供 `set_offer_callback(std::function<void(const std::string& peer_id, const std::string& sdp_offer)>)` 方法，用于注册 SDP Offer 接收回调
2. THE WebRtcSignaling 类 SHALL 提供 `set_ice_candidate_callback(std::function<void(const std::string& peer_id, const std::string& candidate)>)` 方法，用于注册 ICE Candidate 接收回调
3. WHEN 收到 Viewer 的 SDP Offer 消息时，THE WebRtcSignaling 类 SHALL 调用已注册的 offer 回调，传入 Viewer 的 peer_id 和 SDP 内容
4. WHEN 收到 Viewer 的 ICE Candidate 消息时，THE WebRtcSignaling 类 SHALL 调用已注册的 ice_candidate 回调，传入 Viewer 的 peer_id 和 candidate 内容
5. IF 未注册回调函数，THEN THE WebRtcSignaling 类 SHALL 通过 spdlog 记录 warning 并丢弃消息
6. THE WebRtcSignaling 类 SHALL 通过 spdlog 记录收到的信令消息类型和 peer_id（不记录 SDP 和 ICE Candidate 的完整内容，仅记录消息类型）
7. THE 回调函数 SHALL NOT 执行耗时操作（如 PeerConnection 创建、网络请求），回调在 SDK 信令线程中同步调用，阻塞会影响信令消息处理。如需异步处理，调用方应自行投递到工作线程

### 需求 6：信令消息发送

**用户故事：** 作为设备端程序，我需要通过信令通道向 Viewer 发送 SDP Answer 和 ICE Candidate，以便完成 WebRTC 连接协商。

#### 验收标准

1. THE WebRtcSignaling 类 SHALL 提供 `send_answer(const std::string& peer_id, const std::string& sdp_answer)` 方法，用于向指定 Viewer 发送 SDP Answer
2. THE WebRtcSignaling 类 SHALL 提供 `send_ice_candidate(const std::string& peer_id, const std::string& candidate)` 方法，用于向指定 Viewer 发送 ICE Candidate
3. IF 信令客户端未连接时调用发送方法，THEN THE WebRtcSignaling 类 SHALL 返回 false 并通过 spdlog 记录 warning
4. IF 发送失败，THEN THE WebRtcSignaling 类 SHALL 返回 false 并通过 spdlog 记录错误信息，包含 SDK 返回的状态码和目标 peer_id
5. THE WebRtcSignaling 类 SHALL 通过 spdlog 记录发送的消息类型和目标 peer_id（不记录消息完整内容）

### 需求 7：信令连接断开与重连

**用户故事：** 作为设备端程序，我需要能够主动断开信令连接，并在异常断开时支持重连，以便实现 7×24 无人值守运行。

#### 验收标准

1. WHEN 调用 `disconnect()` 时，THE WebRtcSignaling 类 SHALL 释放 SignalingClient 资源并将连接状态设为 false
2. WHEN SignalingClient 状态变为 `SIGNALING_CLIENT_STATE_DISCONNECTED` 时，THE WebRtcSignaling 类 SHALL 通过 spdlog 记录断开事件
3. THE WebRtcSignaling 类 SHALL 提供 `reconnect()` 方法，用于在异常断开后重新连接到信令通道
4. WHEN 调用 `reconnect()` 时，THE WebRtcSignaling 类 SHALL 释放旧的 SignalingClient 并创建新的 SignalingClient 重新连接
5. IF 重连失败，THEN THE WebRtcSignaling 类 SHALL 返回 false 并通过 spdlog 记录错误信息
6. THE WebRtcSignaling 类 SHALL NOT 实现自动重连逻辑（如指数退避、后台重试线程）。重连时机由外部调用方（如 PipelineHealthMonitor 或 main 循环）统一管理，与管道恢复策略对齐

### 需求 8：AWS 资源配置（provision 脚本扩展）

**用户故事：** 作为运维人员，我需要通过 provision 脚本自动创建 KVS WebRTC 信令通道资源并配置 IAM 权限，以便设备端信令客户端能够成功连接。

#### 验收标准

1. THE provision-device.sh SHALL 新增 `--signaling-channel-name` 可选参数（默认值：`{thing-name}Channel`）
2. WHEN 执行 provision 模式时，THE 脚本 SHALL 创建信令通道（`aws kinesisvideo create-signaling-channel --channel-name {name} --channel-type SINGLE_MASTER`）；IF 通道已存在 THEN 跳过并记录 "Signaling channel already exists"
3. WHEN 执行 provision 模式时，THE 脚本 SHALL 为 IAM Role 附加 inline policy（名称：`{project-name}WebRtcPolicy`），包含以下权限（限定到该信令通道 ARN）：
   - `kinesisvideo:DescribeSignalingChannel`
   - `kinesisvideo:GetSignalingChannelEndpoint`
   - `kinesisvideo:GetIceServerConfig`
   - `kinesisvideo:ConnectAsMaster`
4. IF inline policy 已存在 THEN THE 脚本 SHALL 跳过并记录 "WebRTC IAM policy already attached"
5. WHEN 执行 provision 模式时，THE 脚本 SHALL 在 config.toml 中自动生成 `[webrtc]` section，包含 `channel_name` 和 `aws_region` 字段
6. WHEN 执行 verify 模式时，THE 脚本 SHALL 检查信令通道存在性和 IAM inline policy 存在性
7. WHEN 执行 cleanup 模式时，THE 脚本 SHALL 删除信令通道和 IAM inline policy
8. THE provision 脚本的 summary 输出 SHALL 包含信令通道名称

### 需求 9：macOS 开发环境测试

**用户故事：** 作为开发者，我需要在 macOS 上验证 WebRTC 信令集成的代码逻辑（配置加载、stub 创建），无需真实 KVS 环境。

#### 验收标准

1. THE Test_Suite SHALL 验证：WebRtcConfig 从 TOML `[webrtc]` section 正确加载 channel_name 和 aws_region
2. THE Test_Suite SHALL 验证：缺少 `[webrtc]` section 时返回明确错误信息
3. THE Test_Suite SHALL 验证：缺少必要字段时返回明确错误信息
4. THE Test_Suite SHALL 验证：macOS 上 WebRtcSignaling 创建 stub 实例，connect() 返回成功，is_connected() 返回 true
5. THE Test_Suite SHALL 验证：macOS stub 的 disconnect() 将 is_connected() 设为 false
6. THE Test_Suite SHALL 验证：macOS stub 的 send_answer() 和 send_ice_candidate() 在未连接时返回 false
7. THE Test_Suite SHALL 仅使用 stub 实现，不依赖真实 KVS WebRTC SDK
8. WHEN 单个测试执行时，THE Test_Suite SHALL 在 5 秒内完成
9. THE Test_Suite SHALL 在 macOS Debug（ASan）构建下通过，无内存错误报告
10. THE Test_Suite SHALL 使用 RapidCheck 对 WebRTC 配置加载进行 property-based testing：随机生成 channel_name 和 aws_region（非空 ASCII 字符串），写入 TOML `[webrtc]` section → 解析 → 验证字段值一致（1 个 PBT property 即可，复用 spec-7 已验证的 parse_toml_section）

### 需求 10：双平台构建验证

**用户故事：** 作为开发者，我需要确保 WebRTC 信令集成在 macOS 和 Pi 5 上均能成功编译和测试。

#### 验收标准

1. WHEN 在 macOS 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误
2. WHEN 在 Pi 5 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误
3. WHEN 在 macOS 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试（包括现有测试和新增的 webrtc_test），ASan 不报告任何内存错误
4. WHEN 在 Pi 5 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试
5. THE CMakeLists.txt SHALL 在 macOS 上不链接 KVS WebRTC C SDK（macOS 使用 stub，编译时无 SDK 依赖）
6. THE CMakeLists.txt SHALL 在 Linux 上通过 `pkg-config` 或 `find_library` 查找 KVS WebRTC C SDK，找不到时回退到 stub 实现并输出 CMake WARNING

## 参考代码

### KVS WebRTC C SDK IoT 凭证提供者初始化（来源：SDK 官方 README）

```c
// 使用 IoT 证书创建凭证提供者
createLwsIotCredentialProvider(
    "coxxxxxxxx168.credentials.iot.us-west-2.amazonaws.com",  // IoT credentials endpoint
    "/path/to/certificate.pem",   // IoT 证书路径
    "/path/to/private.pem.key",   // IoT 私钥路径
    "/path/to/cacert.pem",        // CA 证书路径
    "KinesisVideoSignalingCameraIoTRoleAlias",  // IoT role alias
    "IoTThingName",               // IoT thing name，建议与 channel name 相同
    &pCredentialProvider);

// 释放
freeIotCredentialProvider(&pCredentialProvider);
```

### SignalingClient 创建和连接（来源：SDK samples/common/Common.c）

```c
// 信令客户端回调
SignalingClientCallbacks signalingClientCallbacks;
signalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
signalingClientCallbacks.customData = (UINT64) pSampleConfiguration;
signalingClientCallbacks.stateChangeFn = signalingClientStateChanged;
signalingClientCallbacks.messageReceivedFn = signalingMessageReceived;

// 信令客户端信息
SignalingClientInfo clientInfo;
MEMSET(&clientInfo, 0, SIZEOF(SignalingClientInfo));
clientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
clientInfo.loggingLevel = LOG_LEVEL_WARN;
STRCPY(clientInfo.clientId, SAMPLE_MASTER_CLIENT_ID);

// 通道信息
ChannelInfo channelInfo;
MEMSET(&channelInfo, 0, SIZEOF(ChannelInfo));
channelInfo.version = CHANNEL_INFO_CURRENT_VERSION;
channelInfo.pChannelName = channelName;
channelInfo.pKmsKeyId = NULL;
channelInfo.tagCount = 0;
channelInfo.pTags = NULL;
channelInfo.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
channelInfo.channelRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER;
channelInfo.cachingPolicy = SIGNALING_API_CALL_CACHE_TYPE_FILE;
channelInfo.pRegion = pRegion;

// 创建信令客户端
STATUS retStatus = createSignalingClientSync(
    &clientInfo,
    &channelInfo,
    &signalingClientCallbacks,
    pCredentialProvider,
    &signalingClientHandle);

// 连接
retStatus = signalingClientConnectSync(signalingClientHandle);

// 释放
freeSignalingClient(&signalingClientHandle);
```

### 信令消息发送（来源：SDK samples/common/Common.c）

```c
// 发送 SDP Answer
SignalingMessage message;
MEMSET(&message, 0, SIZEOF(SignalingMessage));
message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
message.messageType = SIGNALING_MESSAGE_TYPE_ANSWER;
STRCPY(message.peerClientId, peerId);
STRCPY(message.payload, sdpAnswer);
message.payloadLen = (UINT32) STRLEN(sdpAnswer);

STATUS retStatus = signalingClientSendMessageSync(signalingClientHandle, &message);

// 发送 ICE Candidate
message.messageType = SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE;
STRCPY(message.payload, iceCandidate);
message.payloadLen = (UINT32) STRLEN(iceCandidate);
retStatus = signalingClientSendMessageSync(signalingClientHandle, &message);
```

### IAM Policy 示例（来源：SDK 官方 README，WebRTC 信令所需权限）

```json
{
   "Version": "2012-10-17",
   "Statement": [
      {
          "Effect": "Allow",
          "Action": [
            "kinesisvideo:DescribeSignalingChannel",
            "kinesisvideo:GetSignalingChannelEndpoint",
            "kinesisvideo:GetIceServerConfig",
            "kinesisvideo:ConnectAsMaster"
          ],
          "Resource": "arn:aws:kinesisvideo:*:*:channel/${credentials-iot:ThingName}/*"
      }
   ]
}
```

### 条件编译平台隔离示例（参考 spec-8 的 KvsSinkFactory 模式）

```cpp
class WebRtcSignaling {
public:
    static std::unique_ptr<WebRtcSignaling> create(
        const WebRtcConfig& config,
        const AwsConfig& aws_config,
        std::string* error_msg = nullptr);

    bool connect(std::string* error_msg = nullptr);
    void disconnect();
    bool is_connected() const;
    bool reconnect(std::string* error_msg = nullptr);

    // 回调注册
    using OfferCallback = std::function<void(const std::string& peer_id, const std::string& sdp)>;
    using IceCandidateCallback = std::function<void(const std::string& peer_id, const std::string& candidate)>;
    void set_offer_callback(OfferCallback cb);
    void set_ice_candidate_callback(IceCandidateCallback cb);

    // 消息发送
    bool send_answer(const std::string& peer_id, const std::string& sdp_answer);
    bool send_ice_candidate(const std::string& peer_id, const std::string& candidate);

    ~WebRtcSignaling();
    WebRtcSignaling(const WebRtcSignaling&) = delete;
    WebRtcSignaling& operator=(const WebRtcSignaling&) = delete;

private:
    WebRtcSignaling() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

## 验证命令

```bash
# macOS Debug 构建 + 测试（使用 stub，不需要 KVS WebRTC SDK）
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug
cmake --build device/build
ctest --test-dir device/build --output-on-failure

# Pi 5 Release 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release
cmake --build device/build
ctest --test-dir device/build --output-on-failure

# Pi 5 端到端验证（需要 KVS WebRTC C SDK 已编译 + 真实 AWS 环境）
# 前置：
#   1. 编译 KVS WebRTC C SDK：参考 docs/pi-setup.md
#   2. 确保 config.toml 包含 [webrtc] section
#   3. 确保信令通道已创建：aws kinesisvideo create-signaling-channel --channel-name RaspiEyeAlphaChannel --channel-type SINGLE_MASTER
#   4. 运行程序，在 AWS Console 的 KVS WebRTC 页面使用 Test Page 作为 Viewer 连接
```

## 明确不包含

- 媒体流传输（Spec 13: webrtc-media）
- PeerConnection 创建和媒体轨道添加（Spec 13）
- 替换 PipelineBuilder 中 webrtc 分支的 fakesink（Spec 13）
- 自适应码率控制（Spec 14: adaptive-streaming）
- DataChannel 功能
- KVS WebRTC C SDK 的编译安装（由 `docs/pi-setup.md` 覆盖）
- 完整 TOML 配置框架（Spec 18: config-file）
- Viewer 端实现（前端 Spec 20）
- TURN 服务器自建（使用 KVS 提供的 STUN/TURN 服务）
