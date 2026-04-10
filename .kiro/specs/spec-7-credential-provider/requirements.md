# 需求文档：Spec 7 — 设备端 AWS IoT 凭证获取模块

## 简介

本 Spec 实现设备端 C++ 凭证模块，通过 libcurl + mTLS 请求 AWS IoT Credentials Provider，获取 STS 临时凭证（accessKeyId、secretAccessKey、sessionToken、expiration）。临时凭证供后续 KVS、S3 等 AWS 服务认证使用。

模块从 `device/config/config.toml` 读取 AWS 连接配置（thing_name、credential_endpoint、role_alias、证书路径），通过 X.509 客户端证书完成 mTLS 认证，解析 JSON 响应提取凭证，并在凭证过期前自动刷新。

## 前置条件

- Spec 6（iot-provisioning）已完成 ✅
- `device/config/config.toml` 包含 `[aws]` section（由 `scripts/provision-device.sh` 生成）
- `device/certs/` 目录包含 X.509 证书文件（device-cert.pem、device-private.key、root-ca.pem）

## 术语表

- **Credential_Provider**：本 Spec 新增的 C++ 模块，负责获取和缓存 STS 临时凭证
- **IoT_Credentials_Endpoint**：AWS IoT Credentials Provider 的 HTTPS 端点，接受 mTLS 认证并返回 STS 临时凭证
- **STS_Credential**：AWS Security Token Service 签发的临时凭证，包含 accessKeyId、secretAccessKey、sessionToken、expiration 四个字段
- **mTLS**：双向 TLS 认证，客户端使用 X.509 证书向服务端证明身份
- **TOML_Config**：`device/config/config.toml` 配置文件，包含 `[aws]` section 的连接信息
- **Credential_Callback**：凭证刷新完成后的回调函数，通知调用方凭证已更新

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| 目标平台 | Raspberry Pi 5（4GB RAM）+ macOS 开发环境 |
| HTTP 库 | libcurl（系统包，pkg-config 或 find_package） |
| TLS 后端 | OpenSSL（libcurl 的 TLS 后端，macOS 可用 Homebrew OpenSSL 或 SecureTransport） |
| JSON 解析 | nlohmann/json v3.11.3（FetchContent，header-only） |
| TOML 解析 | 手动解析 `[aws]` section（仅 key = "value" 格式，Spec 18 再引入 toml++） |
| 凭证刷新 | 不阻塞主线程（GStreamer 管道），使用后台线程或定时器 |
| 代码组织 | .h + .cpp 分离模式 |
| 日志 | spdlog（已有），英文日志，禁止输出敏感信息 |
| 配置文件路径 | `device/config/config.toml`（相对于项目根目录） |
| 新增文件 | `device/src/credential_provider.{h,cpp}` + `device/tests/credential_test.cpp` |

## 禁止项（Requirements 层）

- SHALL NOT 在本 Spec 中实现 KVS、S3 等下游 AWS 服务的调用（推迟到各自 Spec）
- SHALL NOT 实现完整的 TOML 解析器（仅需解析 `[aws]` section 的 key = "value" 格式）
- SHALL NOT 实现证书轮换或证书更新机制（当前证书由 Spec 6 脚本管理）

## 需求

### 需求 1：TOML 配置加载

**用户故事：** 作为设备端程序，我需要从 TOML 配置文件读取 AWS 连接信息，以便初始化凭证获取模块。

#### 验收标准

1. THE Credential_Provider SHALL 从 `device/config/config.toml` 的 `[aws]` section 读取以下字段：`thing_name`、`credential_endpoint`、`role_alias`、`cert_path`、`key_path`、`ca_path`
2. IF 配置文件不存在，THEN THE Credential_Provider SHALL 返回明确的错误信息，包含缺失的文件路径
3. IF `[aws]` section 中缺少必要字段，THEN THE Credential_Provider SHALL 返回明确的错误信息，包含缺失的字段名称
4. THE 解析器 SHALL 支持 `key = "value"` 格式（带双引号）和 `key = value` 格式（不带引号）
5. THE 解析器 SHALL 忽略注释行（以 `#` 开头）和空行

### 需求 2：mTLS HTTPS 请求

**用户故事：** 作为设备端程序，我需要通过 mTLS 认证向 AWS IoT Credentials Provider 发起 HTTPS 请求，以获取 STS 临时凭证。

#### 验收标准

1. THE Credential_Provider SHALL 使用 libcurl 发起 HTTPS GET 请求，URL 格式为 `https://{credential_endpoint}/role-aliases/{role_alias}/credentials`
2. THE Credential_Provider SHALL 设置 HTTP header `x-amzn-iot-thingname` 为配置中的 `thing_name` 值
3. THE Credential_Provider SHALL 使用 `cert_path` 指定的客户端证书和 `key_path` 指定的私钥进行 mTLS 认证
4. THE Credential_Provider SHALL 使用 `ca_path` 指定的 CA 证书验证服务端证书
5. THE Credential_Provider SHALL 设置 HTTP 请求超时为 10 秒
6. IF libcurl 返回非零错误码，THEN THE Credential_Provider SHALL 返回包含 curl 错误描述的错误信息
7. IF HTTP 响应状态码非 200，THEN THE Credential_Provider SHALL 返回包含状态码和响应体的错误信息

### 需求 3：JSON 响应解析

**用户故事：** 作为设备端程序，我需要解析 Credentials Provider 返回的 JSON 响应，提取 STS 临时凭证。

#### 验收标准

1. THE Credential_Provider SHALL 使用 nlohmann/json 从 JSON 响应中提取 `credentials` 对象的四个字段：`accessKeyId`、`secretAccessKey`、`sessionToken`、`expiration`
2. IF JSON 响应格式不合法，THEN THE Credential_Provider SHALL 返回包含解析错误描述的错误信息
3. IF `credentials` 对象缺少任一必要字段，THEN THE Credential_Provider SHALL 返回包含缺失字段名称的错误信息
4. THE Credential_Provider SHALL 将 `expiration` 字段（ISO 8601 格式）解析为可比较的时间点
5. FOR ALL 提取的字段值 SHALL 与原始 JSON 中对应的值完全一致

### 需求 4：凭证缓存与自动刷新

**用户故事：** 作为设备端程序，我需要凭证模块自动在过期前刷新临时凭证，以保证 AWS 服务调用不中断。

#### 验收标准

1. THE Credential_Provider SHALL 缓存最近一次成功获取的 STS_Credential
2. THE Credential_Provider SHALL 提供 `get_credentials()` 方法，返回当前缓存的凭证（供 KVS、S3 等下游模块调用）
3. WHEN 调用方请求凭证时，THE Credential_Provider SHALL 返回缓存的凭证（不发起网络请求）
4. THE Credential_Provider SHALL 在凭证过期前 5 分钟触发后台刷新
5. WHILE 后台刷新进行中，THE Credential_Provider SHALL 继续返回旧的缓存凭证（不阻塞调用方）
6. WHEN 后台刷新成功，THE Credential_Provider SHALL 原子替换缓存的凭证
7. IF 后台刷新失败，THEN THE Credential_Provider SHALL 记录错误日志并按指数退避重试（初始 1 秒，最大 60 秒），最多重试 10 次
8. IF 连续 10 次刷新失败且凭证已过期，THEN THE Credential_Provider SHALL 通过回调通知调用方凭证不可用
9. THE 凭证刷新操作 SHALL 在后台线程执行，不阻塞 GStreamer 管道主线程

### 需求 5：线程安全与生命周期

**用户故事：** 作为设备端程序，我需要凭证模块在多线程环境下安全运行，并正确管理资源生命周期。

#### 验收标准

1. THE Credential_Provider SHALL 支持多线程并发读取缓存凭证
2. THE Credential_Provider SHALL 使用 RAII 管理 libcurl 句柄和后台刷新线程
3. THE Credential_Provider SHALL 负责调用 `curl_global_init` / `curl_global_cleanup`（进程级单次调用，使用 static 保证）
4. WHEN Credential_Provider 析构时，THE 后台刷新线程 SHALL 在 2 秒内优雅退出
5. THE Credential_Provider SHALL 禁用拷贝构造和拷贝赋值（`= delete`）
6. THE Credential_Provider SHALL 在初始化时执行一次同步凭证获取，成功后才返回（确保首次凭证可用）
7. IF 初始化时凭证获取失败，THEN THE Credential_Provider SHALL 返回错误，不启动后台刷新线程

### 需求 6：可测试性

**用户故事：** 作为开发者，我需要在 macOS 开发环境下测试凭证模块的核心逻辑，无需真实 AWS 环境。

#### 验收标准

1. THE Credential_Provider SHALL 通过接口抽象 HTTP 请求层，允许测试时注入 mock 实现
2. THE 测试 SHALL 验证 TOML 解析的正确性（合法输入、缺失字段、格式错误）
3. THE 测试 SHALL 验证 JSON 响应解析的正确性（合法响应、缺失字段、格式错误）
4. THE 测试 SHALL 验证凭证过期判断逻辑（未过期、即将过期、已过期）
5. THE 测试 SHALL 使用 RapidCheck 对 TOML 解析和 JSON 解析进行 property-based testing

## 验证命令

```bash
# macOS 构建 + 测试（使用 mock HTTP 层，不需要真实 AWS 环境）
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug
cmake --build device/build
ctest --test-dir device/build --output-on-failure

# Pi 5 端到端验证（需要真实证书和 AWS 环境）
# 1. 确保 provision-device.sh 已执行
# 2. 构建并运行集成测试（手动验证）
curl --cert device/certs/device-cert.pem \
     --key device/certs/device-private.key \
     -H "x-amzn-iot-thingname: RaspiEyeAlpha" \
     --cacert device/certs/root-ca.pem \
     "https://c19imbvbcm8u20.credentials.iot.ap-southeast-1.amazonaws.com/role-aliases/RaspiEyeRoleAlias/credentials"
```

## 明确不包含

- KVS Producer SDK 集成（Spec 8）
- WebRTC 信令通道（Spec 12）
- 截图上传 S3（Spec 11）
- 完整 TOML 配置文件加载框架（Spec 18: config-file）
- 证书轮换 / OTA 更新
- IaC 资源创建（KVS 流、S3 桶等）
- IAM Role 权限策略追加（各下游 Spec 按需追加）
