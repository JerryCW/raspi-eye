# 需求文档：Spec 8 — KVS Producer SDK 集成

## 简介

本 Spec 将 H.264 tee 管道中 KVS 分支的 `fakesink("kvs-sink")` 替换为 KVS Producer SDK 提供的 `kvssink` GStreamer element，实现视频流上传到 Amazon Kinesis Video Streams。

当前管道拓扑（来自 spec-3）：
```
videotestsrc -> videoconvert -> capsfilter(I420) -> raw-tee
  -> queue(leaky) -> fakesink("ai-sink")           [AI 分支, raw frames]
  -> queue -> x264enc -> h264parse -> encoded-tee
    -> queue -> fakesink("kvs-sink")                [KVS 分支, H.264] ← 本 Spec 替换目标
    -> queue(leaky) -> fakesink("webrtc-sink")      [WebRTC 分支, H.264]
```

替换后 KVS 分支变为：
```
encoded-tee -> queue -> kvssink("kvs-sink")
```

认证方式：使用 kvssink 原生的 `iot-certificate` 参数，直接传入 IoT 证书路径（cert-path、key-path、ca-path、endpoint、role-aliases），由 kvssink 内部处理凭证获取和自动刷新。不通过 CredentialProvider 中转 — kvssink 的 IoT 凭证提供者已内置此功能。

> 架构决策说明：kvssink 支持三种认证方式（[官方文档](https://docs.aws.amazon.com/kinesisvideostreams/latest/dg/examples-gstreamer-plugin-parameters.html)）：
> 1. `iot-certificate` — 直接传入 IoT 证书路径，SDK 内部自动获取和刷新 STS 凭证 ← **本 Spec 采用**
> 2. `access-key` + `secret-key` + `session-token` — 需要外部管理凭证刷新
> 3. `credential-path` — 指向凭证文件，需要外部更新文件
>
> 选择方案 1 的理由：我们已有 IoT 证书体系（spec-6），kvssink 原生支持 IoT 认证且内置刷新机制，无需额外的凭证中转层。CredentialProvider（spec-7）保留给不支持 IoT 认证的组件使用（如 S3 上传等）。

平台策略：
- **Pi 5（Linux aarch64）**：使用完整的 KVS Producer SDK（`kvssink` GStreamer element，通过 `GST_PLUGIN_PATH` 加载 `libgstkvssink.so`）。插件统一存放在 `device/plugins/` 目录，`pi-build.sh` 自动设置 `GST_PLUGIN_PATH` 指向该目录
- **macOS（开发环境）**：KVS Producer SDK 不支持 macOS，使用 `fakesink` 作为 stub，保持管道拓扑一致

## 前置条件

- Spec 3（h264-tee-pipeline）✅ 已完成 — 双 tee 管道，KVS 分支当前为 fakesink("kvs-sink")
- Spec 5（pipeline-health）✅ 已完成 — 管道健康监控与自动恢复
- Spec 6（iot-provisioning）✅ 已完成 — AWS IoT Thing + X.509 证书 + IAM Role + Role Alias

## 术语表

- **KVS Producer SDK**：Amazon Kinesis Video Streams Producer SDK C++，从源码编译产出 `libgstkvssink.so` GStreamer 插件
- **kvssink**：KVS Producer SDK 提供的 GStreamer sink element，接收 H.264 编码流并上传到指定的 KVS 流
- **iot-certificate**：kvssink 的认证参数，格式为 `"iot-certificate,endpoint=...,cert-path=...,key-path=...,ca-path=...,role-aliases=..."` 的逗号分隔字符串
- **KVS Stream**：Amazon Kinesis Video Streams 中的视频流资源，由 stream-name 标识
- **KvsConfig**：本 Spec 新增的配置结构体，包含 KVS 流名称、AWS 区域等参数
- **KvsSinkFactory**：本 Spec 新增的工厂模块，根据平台创建 kvssink（Linux）或 fakesink stub（macOS）
- **PipelineBuilder**：Spec 3 交付的双 tee 管道构建器，本 Spec 修改其 KVS 分支 sink 创建逻辑
- **PipelineHealthMonitor**：Spec 5 交付的管道健康监控器，检测 kvssink 错误并触发恢复
- **GST_PLUGIN_PATH**：环境变量，指向 `device/plugins/` 目录（统一插件存放点），GStreamer 通过此路径加载 kvssink 等运行时插件。`pi-build.sh` 自动设置此变量

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| 管道冷启动 | ≤ 2 秒（初始化到第一个 Buffer 到达 Sink） |
| 单个测试耗时 | ≤ 5 秒 |
| 代码组织 | .h + .cpp 分离模式 |
| GStreamer 资源管理 | RAII，所有引用计数必须配对 |
| 日志语言 | 英文，不使用非 ASCII 字符 |
| KVS Producer SDK | 仅 Linux 可用（`libgstkvssink.so` 通过 `GST_PLUGIN_PATH` 加载），macOS 使用 fakesink stub |
| 插件目录 | `device/plugins/`（统一存放运行时加载的 GStreamer 插件，.gitignore 排除，`pi-build.sh` 自动设置 `GST_PLUGIN_PATH`） |
| 认证方式 | kvssink 原生 `iot-certificate` 参数，证书路径来自 config.toml 的 `[aws]` section |
| 配置来源 | `device/config/config.toml` 的 `[kvs]` section（流名称、区域）+ `[aws]` section（证书路径，已有） |
| 新增代码量 | 100-500 行 |
| 涉及文件 | 2-5 个 |
| 向后兼容 | 不修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、yolo_test） |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现自适应码率控制（属于 Spec 14: adaptive-streaming）
- SHALL NOT 在本 Spec 中实现 WebRTC 信令或媒体流（属于 Spec 12/13）
- SHALL NOT 在本 Spec 中实现 KVS 流的回放或消费端功能
- SHALL NOT 通过 CredentialProvider 中转凭证给 kvssink（kvssink 原生支持 iot-certificate，无需中转）

### Design 层

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret（来源：安全基线）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）
- SHALL NOT 在 macOS 上尝试加载或链接 KVS Producer SDK（macOS 使用 fakesink stub）
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、yolo_test）
- SHALL NOT 在不确定 kvssink 属性名称和语义时凭猜测编写代码（来源：shall-not.md，属性名已通过官方文档确认，见参考代码）

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 GStreamer / KVS C API 外）

## 需求

### 需求 1：KVS 配置加载

**用户故事：** 作为设备端程序，我需要从配置文件读取 KVS 相关参数，以便初始化 kvssink element。

#### 验收标准

1. THE KvsSinkFactory SHALL 从 `device/config/config.toml` 的 `[kvs]` section 读取以下字段：`stream_name`（KVS 流名称）、`aws_region`（AWS 区域）
2. IF `[kvs]` section 不存在，THEN THE KvsSinkFactory SHALL 返回明确的错误信息，包含缺失的 section 名称
3. IF `[kvs]` section 中缺少 `stream_name` 或 `aws_region` 字段，THEN THE KvsSinkFactory SHALL 返回明确的错误信息，包含缺失的字段名称
4. THE KvsSinkFactory SHALL 复用 Spec 7 的 TOML 解析函数（`parse_toml_section`）读取 `[kvs]` section
5. THE `device/config/config.toml.example` SHALL 包含 `[kvs]` section 的示例配置，包含 `stream_name` 和 `aws_region` 字段及注释说明

### 需求 2：kvssink Element 创建（平台条件编译）

**用户故事：** 作为设备端程序，我需要根据运行平台创建合适的 KVS sink element，以便在 Pi 5 上使用真实 kvssink，在 macOS 上使用 fakesink stub 保持管道拓扑一致。

#### 验收标准

1. WHEN 在 Linux 平台上运行时，THE KvsSinkFactory SHALL 尝试创建 `kvssink` GStreamer element 并设置名称为 `"kvs-sink"`
2. WHEN 在 macOS 平台上运行时，THE KvsSinkFactory SHALL 创建 `fakesink` GStreamer element 并设置名称为 `"kvs-sink"`，作为 stub 占位
3. THE KvsSinkFactory SHALL 通过条件编译（`#ifdef __linux__` / `#ifdef __APPLE__`）隔离平台特定代码
4. WHEN 在 Linux 上 `kvssink` element 不可用时（SDK 未安装或 `GST_PLUGIN_PATH` 未设置），THE KvsSinkFactory SHALL 回退到 `fakesink` 并通过 spdlog 记录警告信息，包含 "kvssink not available, falling back to fakesink"
5. THE KvsSinkFactory SHALL 通过 spdlog 记录创建的 sink 类型（"kvssink" 或 "fakesink stub"）

### 需求 3：kvssink 属性配置

**用户故事：** 作为设备端程序，我需要将 KVS 流名称、AWS 区域和 IoT 证书认证信息设置到 kvssink element，以便 kvssink 能够认证并上传视频流到正确的 KVS 流。

> 属性名来源：[AWS 官方文档 — GStreamer element parameter reference](https://docs.aws.amazon.com/kinesisvideostreams/latest/dg/examples-gstreamer-plugin-parameters.html)

#### 验收标准

1. WHEN 创建 kvssink element 后，THE KvsSinkFactory SHALL 设置 `stream-name` 属性为配置中的 KVS 流名称
2. WHEN 创建 kvssink element 后，THE KvsSinkFactory SHALL 设置 `aws-region` 属性为配置中的 AWS 区域
3. WHEN 创建 kvssink element 后，THE KvsSinkFactory SHALL 构建 `iot-certificate` 属性字符串，格式为：`"iot-certificate,iot-thing-name={thing_name},endpoint={credential_endpoint},cert-path={cert_path},key-path={key_path},ca-path={ca_path},role-aliases={role_alias}"`，其中 `thing_name` 来自 config.toml 的 `[aws]` section，其余字段同样来自 `[aws]` section
4. THE KvsSinkFactory SHALL 通过 spdlog 记录 stream-name 和 aws-region（不记录证书路径和凭证内容）
5. IF 当前 sink 为 fakesink stub（macOS 或 Linux 回退场景），THEN THE KvsSinkFactory SHALL 跳过 `iot-certificate` 属性设置（fakesink 不支持该属性）
6. WHEN 创建 kvssink element 后，THE KvsSinkFactory SHALL 设置 `restart-on-error` 属性为 false，禁用 kvssink 内置重试机制，由 PipelineHealthMonitor（spec-5）统一管理运行时错误恢复
7. THE KvsSinkFactory SHALL NOT 在任何日志级别输出 iot-certificate 完整字符串（包含证书路径，泄露风险高）

### 需求 4：PipelineBuilder 集成

**用户故事：** 作为设备端程序，我需要 PipelineBuilder 使用 KvsSinkFactory 替换 KVS 分支的 fakesink，以便管道构建时自动创建正确的 KVS sink。

#### 验收标准

1. THE PipelineBuilder SHALL 调用 KvsSinkFactory 创建 KVS 分支的 sink element，替换当前的 `gst_element_factory_make("fakesink", "kvs-sink")`
2. WHEN KvsSinkFactory 返回有效的 GstElement 时，THE PipelineBuilder SHALL 将该 element 添加到管道并连接到 encoded-tee 的 queue 下游
3. WHEN KvsSinkFactory 返回失败时，THE PipelineBuilder SHALL 返回 nullptr 并通过 error_msg 输出错误信息
4. THE PipelineBuilder 的函数签名 SHALL 扩展为接受可选的 KvsConfig 参数，保持向后兼容（默认值为空，此时行为与当前一致，使用 fakesink）
5. THE PipelineBuilder SHALL 保持现有管道拓扑不变（raw-tee、encoded-tee、AI 分支、WebRTC 分支均不受影响）

> 注意：后续 spec-10（AI pipeline）和 spec-12/13（WebRTC）也需要扩展 PipelineBuilder 签名。当参数超过 3 个时，应考虑重构为 `PipelineConfig` 聚合结构体，但该重构不在本 Spec 范围内。

### 需求 5：macOS 开发环境测试

**用户故事：** 作为开发者，我需要在 macOS 上验证 KVS 集成的代码逻辑（配置加载、工厂创建），无需真实 KVS 环境。

#### 验收标准

1. THE Test_Suite SHALL 验证：KvsConfig 从 TOML `[kvs]` section 正确加载 stream_name 和 aws_region
2. THE Test_Suite SHALL 验证：缺少 `[kvs]` section 时返回明确错误信息
3. THE Test_Suite SHALL 验证：缺少必要字段时返回明确错误信息
4. THE Test_Suite SHALL 验证：macOS 上 KvsSinkFactory 创建 fakesink 并设置名称为 "kvs-sink"
5. THE Test_Suite SHALL 验证：不传入 KvsConfig 时 PipelineBuilder 仍使用 fakesink（向后兼容）
6. THE Test_Suite SHALL 验证：传入 KvsConfig 时（macOS stub 场景），管道正常构建并启动
7. THE Test_Suite SHALL 仅使用 fakesink 作为 sink element，不使用需要显示设备的 sink
8. WHEN 单个测试执行时，THE Test_Suite SHALL 在 5 秒内完成
9. THE Test_Suite SHALL 在 macOS Debug（ASan）构建下通过，无内存错误报告
10. THE Test_Suite SHALL 使用 RapidCheck 对 KVS 配置加载进行 property-based testing：随机生成 stream_name 和 aws_region（非空 ASCII 字符串），写入 TOML `[kvs]` section → 解析 → 验证字段值一致（1 个 PBT property 即可，复用 spec-7 已验证的 parse_toml_section）

### 需求 6：AWS 资源配置（provision 脚本扩展）

**用户故事：** 作为运维人员，我需要通过 provision 脚本自动创建 KVS Stream 资源并配置 IAM 权限，以便设备端 kvssink 能够成功上传视频流。

#### 验收标准

1. THE provision-device.sh SHALL 新增 `--kvs-stream-name` 可选参数（默认值：`{thing-name}Stream`）
2. WHEN 执行 provision 模式时，THE 脚本 SHALL 创建 KVS Stream（`aws kinesisvideo create-stream`），设置 `--data-retention-in-hours 2`；IF 流已存在 THEN 跳过并记录 "KVS Stream already exists"
3. WHEN 执行 provision 模式时，THE 脚本 SHALL 为 IAM Role 附加 inline policy（名称：`{project-name}KvsPolicy`），包含以下权限：
   - `kinesisvideo:PutMedia` — 限定到该 KVS Stream ARN
   - `kinesisvideo:GetDataEndpoint` — 限定到该 KVS Stream ARN
   - `kinesisvideo:DescribeStream` — 限定到该 KVS Stream ARN
4. IF inline policy 已存在 THEN THE 脚本 SHALL 跳过并记录 "KVS IAM policy already attached"
5. WHEN 执行 provision 模式时，THE 脚本 SHALL 在 config.toml 中自动生成 `[kvs]` section，包含 `stream_name` 和 `aws_region` 字段
6. WHEN 执行 verify 模式时，THE 脚本 SHALL 检查 KVS Stream 存在性和 IAM inline policy 存在性
7. WHEN 执行 cleanup 模式时，THE 脚本 SHALL 删除 KVS Stream 和 IAM inline policy
8. THE provision 脚本的 summary 输出 SHALL 包含 KVS Stream 名称和 ARN

### 需求 7：双平台构建验证

**用户故事：** 作为开发者，我需要确保 KVS 集成在 macOS 和 Pi 5 上均能成功编译和测试。

#### 验收标准

1. WHEN 在 macOS 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误
2. WHEN 在 Pi 5 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误
3. WHEN 在 macOS 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试（包括现有测试和新增的 kvs_test），ASan 不报告任何内存错误
4. WHEN 在 Pi 5 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试
5. THE CMakeLists.txt SHALL 不链接 KVS Producer SDK（kvssink 通过 GStreamer 插件机制在运行时加载，编译时无依赖）

## 参考代码

### kvssink 属性设置（属性名已通过 [AWS 官方文档](https://docs.aws.amazon.com/kinesisvideostreams/latest/dg/examples-gstreamer-plugin-parameters.html) 确认）

```cpp
// stream-name 和 aws-region 是 kvssink 的标准属性
g_object_set(G_OBJECT(kvs_sink),
    "stream-name", config.stream_name.c_str(),
    "aws-region",  config.aws_region.c_str(),
    nullptr);

// iot-certificate 是逗号分隔的认证参数字符串
// 格式：iot-certificate,endpoint=...,cert-path=...,key-path=...,ca-path=...,role-aliases=...
std::string iot_cert = "iot-certificate,"
    "endpoint=" + aws_config.credential_endpoint + ","
    "cert-path=" + aws_config.cert_path + ","
    "key-path=" + aws_config.key_path + ","
    "ca-path=" + aws_config.ca_path + ","
    "role-aliases=" + aws_config.role_alias;
g_object_set(G_OBJECT(kvs_sink),
    "iot-certificate", iot_cert.c_str(),
    nullptr);
```

### gst-launch 命令行验证示例（Pi 5 端到端）

```bash
# 需要先设置 GST_PLUGIN_PATH 指向 libgstkvssink.so 所在目录
export GST_PLUGIN_PATH=~/kvs-producer-sdk-cpp/build

gst-launch-1.0 -v videotestsrc ! videoconvert ! \
  x264enc tune=zerolatency speed-preset=ultrafast ! h264parse ! \
  kvssink stream-name="RaspiEyeStream" aws-region="ap-southeast-1" \
  iot-certificate="iot-certificate,endpoint=c19imbvbcm8u20.credentials.iot.ap-southeast-1.amazonaws.com,cert-path=device/certs/device-cert.pem,key-path=device/certs/device-private.key,ca-path=device/certs/root-ca.pem,role-aliases=RaspiEyeRoleAlias"
```

### 条件编译平台隔离示例

```cpp
GstElement* create_kvs_sink(const KvsConfig& config, std::string* error_msg) {
#ifdef __linux__
    GstElement* sink = gst_element_factory_make("kvssink", "kvs-sink");
    if (!sink) {
        spdlog::warn("kvssink not available, falling back to fakesink");
        sink = gst_element_factory_make("fakesink", "kvs-sink");
        // fakesink 不需要设置 iot-certificate 等属性
        return sink;
    }
    // 设置 kvssink 属性...
#else
    GstElement* sink = gst_element_factory_make("fakesink", "kvs-sink");
    spdlog::info("macOS: using fakesink stub for kvs-sink");
#endif
    return sink;
}
```

## 验证命令

```bash
# macOS Debug 构建 + 测试（使用 fakesink stub，不需要 KVS SDK）
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug
cmake --build device/build
ctest --test-dir device/build --output-on-failure

# Pi 5 Release 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release
cmake --build device/build
ctest --test-dir device/build --output-on-failure

# Pi 5 端到端验证（需要 KVS Producer SDK 已编译 + 真实 AWS 环境）
# 前置：
#   1. 编译 KVS Producer SDK：参考 docs/pi-setup.md
#   2. 复制 libgstkvssink.so 到 device/plugins/（pi-build.sh 自动设置 GST_PLUGIN_PATH）
#   3. 确保 config.toml 包含 [kvs] section
#   4. 确保 KVS 流已创建：aws kinesis-video create-stream --stream-name RaspiEyeStream --data-retention-in-hours 2 --region ap-southeast-1
# 验证：运行程序，在 AWS Console 的 KVS 页面验证视频流
```

## 明确不包含

- 自适应码率控制（Spec 14: adaptive-streaming）
- WebRTC 信令或媒体流（Spec 12/13）
- KVS 流的回放或消费端功能
- AI 推理管道集成（Spec 10: ai-pipeline）
- 截图上传 S3（Spec 11: screenshot-uploader）
- KVS Producer SDK 的编译安装（由 `docs/pi-setup.md` 覆盖）
- 完整 TOML 配置框架（Spec 18: config-file）
- PipelineBuilder 签名重构为 PipelineConfig 聚合结构体（等参数超过 3 个时再做）
- 通过 CredentialProvider 中转凭证给 kvssink（kvssink 原生 iot-certificate 已内置凭证刷新）
