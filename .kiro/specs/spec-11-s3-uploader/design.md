# 设计文档：Spec 11 — S3 截图上传器

## 概述

本设计实现 S3Uploader 模块，负责将 Spec 10 AiPipelineHandler 产生的检测事件截图上传到 AWS S3。核心设计决策：

1. **libcurl + 手动 SigV4 签名**：复用项目已有的 libcurl 和 OpenSSL 基础设施，不引入 AWS SDK for C++。SigV4 签名使用 OpenSSL libcrypto 的 SHA256 和 HMAC-SHA256 API
2. **凭证来源**：复用 Spec 7 的 CredentialProvider 获取 STS 临时凭证（access_key_id + secret_access_key + session_token）
3. **后台扫描线程**：独立线程定期扫描 `snapshot_dir`，找到 `status: "closed"` 的事件目录，逐文件 PUT 上传到 S3
4. **崩溃安全**：上传完成后先写 `.uploaded` 标记文件再删除本地目录，崩溃恢复时检测标记直接清理
5. **纯函数设计**：SigV4 签名函数（sha256_hex、hmac_sha256、build_canonical_request、build_string_to_sign、build_authorization_header）和 S3 路径构建函数均为独立纯函数，便于 PBT 测试

认证链路：IoT X.509 证书 → IoT Credentials Provider → STS 临时凭证 → SigV4 签名 → S3 PUT。

## 架构

### 数据流

```mermaid
graph LR
    subgraph AiPipelineHandler 写入
        EVT[事件目录<br/>evt_YYYYMMDD_HHMMSS/] --> EJ[event.json<br/>status: closed]
        EVT --> JPG[JPEG 截图文件]
    end

    subgraph S3Uploader 后台线程
        SCAN[扫描 snapshot_dir] --> CHECK{event.json<br/>status == closed?}
        CHECK -->|是| UPLOAD[逐文件 PUT 上传]
        CHECK -->|否| SKIP[跳过]
        UPLOAD --> MARK[写入 .uploaded 标记]
        MARK --> DEL[删除本地目录]
    end

    subgraph AWS
        UPLOAD -->|SigV4 签名<br/>libcurl PUT| S3[S3 Bucket]
        CRED[CredentialProvider] -->|STS 临时凭证| UPLOAD
    end
```

### 线程模型

```mermaid
sequenceDiagram
    participant Main as Main Thread
    participant Scan as Scan Thread
    participant S3 as S3 (libcurl)

    Main->>Scan: start() 启动扫描线程
    loop 每 scan_interval_sec 秒
        Scan->>Scan: 扫描 snapshot_dir
        Scan->>Scan: 检查 .uploaded 标记 → 直接删除
        Scan->>Scan: 读取 event.json → 过滤 closed
        loop 每个 closed 事件的每个文件
            Scan->>Scan: 读取文件内容 + SHA256 哈希
            Scan->>Scan: 获取 STS 凭证
            Scan->>Scan: 构建 SigV4 签名
            Scan->>S3: libcurl PUT
            S3-->>Scan: HTTP 200 / 错误
            alt 失败
                Scan->>Scan: 指数退避重试
            end
        end
        Scan->>Scan: 全部成功 → .uploaded → 删除目录
    end
    Main->>Scan: stop() 通知退出
    Scan-->>Main: join 完成
```

### SigV4 签名流程

```mermaid
graph TD
    A[文件内容] --> B[SHA256 哈希<br/>= payload_hash]
    B --> C[构建 Canonical Request<br/>PUT + URI + headers + payload_hash]
    C --> D[SHA256 哈希 Canonical Request]
    D --> E[构建 String-to-Sign<br/>AWS4-HMAC-SHA256 + timestamp + scope + hash]
    F[Secret Key] --> G[4步 HMAC 链<br/>date → region → s3 → aws4_request]
    G --> H[Signing Key]
    H --> I[HMAC-SHA256<br/>signing_key, string_to_sign]
    E --> I
    I --> J[Authorization Header]
```

## 组件与接口

### 新增文件

| 文件 | 职责 |
|------|------|
| `device/src/s3_uploader.h` | S3Uploader 类声明、S3Config POD、SigV4 纯函数声明 |
| `device/src/s3_uploader.cpp` | S3Uploader 实现：扫描线程、SigV4 签名、libcurl PUT |
| `device/tests/s3_test.cpp` | 单元测试 + PBT |

### 修改文件

| 文件 | 变更 |
|------|------|
| `device/src/config_manager.h` | 新增 `S3Config` 结构体、`parse_s3_config()` 函数声明 |
| `device/src/config_manager.cpp` | 实现 `parse_s3_config()`，`ConfigManager::load()` 解析 `[s3]` section |
| `device/src/app_context.h` | 无变更（pImpl 隐藏实现） |
| `device/src/app_context.cpp` | 集成 S3Uploader 到 init/start/stop 生命周期 |
| `device/CMakeLists.txt` | 新增 s3_module 库和 s3_test |
| `device/config/config.toml` | 新增 `[s3]` section |

### S3Config 结构体

```cpp
// config_manager.h 新增
struct S3Config {
    std::string bucket;                    // S3 桶名
    std::string region;                    // AWS region（默认与 kvs.aws_region 一致）
    int scan_interval_sec = 30;            // 扫描间隔（秒），≥ 5
    int max_retries = 3;                   // 单文件最大重试次数
};
```

### S3Uploader 类接口

```cpp
// s3_uploader.h
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "credential_provider.h"

struct S3Config;

// S3 PUT 上传函数类型（可注入 mock）
using S3PutFunction = std::function<bool(
    const std::string& url,
    const std::vector<uint8_t>& data,
    const std::vector<std::string>& headers)>;

class S3Uploader {
public:
    // 工厂方法
    static std::unique_ptr<S3Uploader> create(
        const S3Config& config,
        const std::string& snapshot_dir,
        const std::string& device_id,
        std::shared_ptr<CredentialProvider> credential_provider,
        std::string* error_msg = nullptr);

    ~S3Uploader();

    // 禁用拷贝
    S3Uploader(const S3Uploader&) = delete;
    S3Uploader& operator=(const S3Uploader&) = delete;

    // 启动后台扫描线程
    bool start(std::string* error_msg = nullptr);

    // 停止扫描线程（通知 + join）
    void stop();

    // 注入自定义 PUT 函数（测试用，必须在 start() 前调用）
    void set_put_function(S3PutFunction fn);

private:
    S3Uploader(const S3Config& config,
               const std::string& snapshot_dir,
               const std::string& device_id,
               std::shared_ptr<CredentialProvider> credential_provider);

    void scan_loop();
    bool upload_event(const std::string& event_dir);
    bool upload_file(const std::string& local_path,
                     const std::string& s3_key,
                     const std::string& content_type);

    // 配置
    S3Config config_;
    std::string snapshot_dir_;
    std::string device_id_;
    std::shared_ptr<CredentialProvider> credential_provider_;
    S3PutFunction put_fn_;

    // 线程同步
    std::thread scan_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_requested_ = false;
};
```

### SigV4 纯函数接口

```cpp
// s3_uploader.h — 独立纯函数（非类成员）

// SHA256 哈希，返回 64 字符小写十六进制字符串
std::string sha256_hex(const std::string& data);
std::string sha256_hex(const uint8_t* data, size_t len);

// HMAC-SHA256，返回原始字节
std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                  const std::string& data);

// HMAC-SHA256，key 为字符串
std::vector<uint8_t> hmac_sha256(const std::string& key,
                                  const std::string& data);

// 字节数组转小写十六进制字符串
std::string to_hex(const std::vector<uint8_t>& bytes);
std::string to_hex(const uint8_t* data, size_t len);

// URI 编码（S3 路径中 '/' 不编码）
std::string uri_encode(const std::string& input, bool encode_slash = true);

// 构建 canonical request
// 返回格式：PUT\n/{bucket_key}\n\n{canonical_headers}\n{signed_headers}\n{payload_hash}
std::string build_canonical_request(
    const std::string& method,
    const std::string& uri_path,
    const std::string& canonical_headers,
    const std::string& signed_headers,
    const std::string& payload_hash);

// 构建 string-to-sign
std::string build_string_to_sign(
    const std::string& timestamp,       // YYYYMMDD'T'HHMMSS'Z'
    const std::string& scope,           // YYYYMMDD/region/s3/aws4_request
    const std::string& canonical_request_hash);

// 计算 signing key（4 步 HMAC 链）
std::vector<uint8_t> derive_signing_key(
    const std::string& secret_key,
    const std::string& date,            // YYYYMMDD
    const std::string& region,
    const std::string& service);        // "s3"

// 构建完整 Authorization header 值
std::string build_authorization_header(
    const std::string& access_key,
    const std::string& scope,
    const std::string& signed_headers,
    const std::string& signature);

// S3 key 构建（纯函数）
// 返回 "{device_id}/{date}/{event_id}/{filename}"
// date 格式 YYYY-MM-DD，从 event.json 的 start_time 提取
// 校验各字段仅包含 [a-zA-Z0-9._-]，非法字符返回空字符串
std::string build_s3_key(
    const std::string& device_id,
    const std::string& date,
    const std::string& event_id,
    const std::string& filename);
```

### ConfigManager 扩展

```cpp
// config_manager.h 新增

bool parse_s3_config(
    const std::unordered_map<std::string, std::string>& kv,
    S3Config& config,
    std::string* error_msg = nullptr);

// ConfigManager 新增
class ConfigManager {
public:
    // ... 现有接口 ...
    const S3Config& s3_config() const { return s3_config_; }
private:
    S3Config s3_config_;
};
```

### config.toml [s3] section

```toml
[s3]
bucket = "raspi-eye-snapshots"
region = "ap-southeast-1"
scan_interval_sec = 30
max_retries = 3
```

### AppContext 集成

```cpp
// app_context.cpp — Impl 新增成员
struct AppContext::Impl {
    // ... 现有成员 ...
    std::unique_ptr<S3Uploader> s3_uploader_;
};

// init() 中：
// 1. 解析 [s3] 配置
// 2. 如果 bucket 非空且 CredentialProvider 可用，创建 S3Uploader
// 3. 否则跳过，记录 info 日志

// start() 中：
// s3_uploader_->start()

// stop() 中：
// shutdown_handler 注册 s3_uploader->stop()
```

### CMake 集成

```cmake
# S3 上传模块（始终编译，不依赖 ENABLE_YOLO）
add_library(s3_module STATIC src/s3_uploader.cpp)
target_include_directories(s3_module PUBLIC src)
target_link_libraries(s3_module PUBLIC
    credential_module
    nlohmann_json::nlohmann_json
    CURL::libcurl
    spdlog::spdlog)

# OpenSSL libcrypto
find_package(OpenSSL REQUIRED)
target_link_libraries(s3_module PUBLIC OpenSSL::Crypto)

# Link to pipeline_manager
target_link_libraries(pipeline_manager PUBLIC s3_module)

# S3 测试
add_executable(s3_test tests/s3_test.cpp)
target_link_libraries(s3_test PRIVATE
    s3_module
    GTest::gtest_main
    rapidcheck
    rapidcheck_gtest)
add_test(NAME s3_test COMMAND s3_test)
```

## 数据模型

### event.json 结构（Spec 10 定义，本 Spec 只读）

```json
{
    "event_id": "evt_20260412_153045",
    "device_id": "RaspiEyeAlpha",
    "start_time": "2026-04-12T15:30:45Z",
    "end_time": "2026-04-12T15:31:15Z",
    "status": "closed",
    "frame_count": 12,
    "detections_summary": {
        "bird": { "count": 8, "max_confidence": 0.92 },
        "cat": { "count": 3, "max_confidence": 0.78 }
    }
}
```

### S3 路径格式

```
{device_id}/{date}/{event_id}/{filename}
```

示例：
```
RaspiEyeAlpha/2026-04-12/evt_20260412_153045/event.json
RaspiEyeAlpha/2026-04-12/evt_20260412_153045/20260412_153046_001.jpg
RaspiEyeAlpha/2026-04-12/evt_20260412_153045/20260412_153046_002.jpg
```

- `date` 从 `event.json` 的 `start_time` 中提取（YYYY-MM-DD 格式），便于按日期分区查询和 S3 生命周期策略
- `device_id` 从 `config.toml` 的 `[aws].thing_name` 读取
- 各字段仅允许 `[a-zA-Z0-9._-]` 字符，`build_s3_key()` 校验不通过返回空字符串

### S3 PUT 请求结构

```
PUT /{bucket_key} HTTP/1.1
Host: {bucket}.s3.{region}.amazonaws.com
x-amz-date: 20260412T153045Z
x-amz-content-sha256: {payload_sha256_hex}
x-amz-security-token: {session_token}
Content-Type: image/jpeg | application/json
Authorization: AWS4-HMAC-SHA256 Credential={access_key}/{scope}, SignedHeaders={signed_headers}, Signature={signature}
```

Canonical headers（按字母序排列）：
```
content-type:{content_type}
host:{bucket}.s3.{region}.amazonaws.com
x-amz-content-sha256:{payload_hash}
x-amz-date:{timestamp}
x-amz-security-token:{session_token}
```

Signed headers：
```
content-type;host;x-amz-content-sha256;x-amz-date;x-amz-security-token
```

### 事件目录生命周期

```
1. AiPipelineHandler 创建事件目录 + 写入 JPEG + event.json (status: "closed")
2. S3Uploader 扫描发现 closed 事件
3. 逐文件上传到 S3
4. 全部成功 → 写入 .uploaded 标记文件
5. 删除本地事件目录
```

崩溃恢复：下次扫描检测到 `.uploaded` 标记 → 直接删除目录（文件已在 S3）。

### 指数退避策略

```
重试 1: 等待 1 秒
重试 2: 等待 2 秒
重试 3: 等待 4 秒
...
最大等待: 60 秒
```

等待期间通过 `condition_variable::wait_for` 响应 stop 信号，不阻塞优雅退出。


## 正确性属性

*正确性属性是在系统所有有效执行中都应成立的特征或行为——本质上是对系统应做什么的形式化陈述。属性是人类可读规格与机器可验证正确性保证之间的桥梁。*

### Property 1: SigV4 密码学输出不变量

*For any* 字符串输入，`sha256_hex()` 的输出恒为 64 字符且仅包含小写十六进制字符 `[0-9a-f]`。*For any* key 和 data 输入，`hmac_sha256()` 的输出恒为 32 字节。*For any* secret_key、date、region、service 输入，`derive_signing_key()` 的输出恒为 32 字节。

**Validates: Requirements 2.1, 2.2, 2.5, 2.6**

### Property 2: SigV4 签名构建格式不变量

*For any* 有效的 HTTP 方法、URI 路径、canonical headers、signed headers 和 payload hash：
- `build_canonical_request()` 的输出包含恰好 5 个 `\n` 分隔符（6 个部分），第一行为 HTTP 方法
- `build_string_to_sign()` 的输出第一行恒为 `"AWS4-HMAC-SHA256"`，共 4 行
- `build_authorization_header()` 的输出以 `"AWS4-HMAC-SHA256 Credential="` 开头，包含 `"SignedHeaders="` 和 `"Signature="` 子串

**Validates: Requirements 2.3, 2.4, 2.7**

### Property 3: S3 key 构建格式与安全性

*For any* 仅包含 `[a-zA-Z0-9._-]` 字符的 device_id、date（YYYY-MM-DD 格式）、event_id 和 filename，`build_s3_key()` 的输出恰好包含 3 个 `/` 分隔符，且各段分别等于输入的 device_id、date、event_id、filename。*For any* 包含非法字符（如 `../`、空格、特殊符号）的输入，`build_s3_key()` 返回空字符串。

**Validates: Requirements 3.3, 3.8**

### Property 4: 事件扫描只返回 closed 事件

*For any* 事件目录集合（包含 closed、active、无 event.json、缺少 end_time 等混合状态），`scan_closed_events()` 的返回结果中每个事件目录的 `event.json` 的 `status` 字段恒为 `"closed"` 且包含 `end_time` 字段。返回结果是输入中 closed 事件的子集。

**Validates: Requirements 4.3, 4.4, 4.5**

### Property 5: 指数退避计算

*For any* 非负整数重试次数 n 和正整数初始延迟 initial_delay_sec，指数退避延迟恒等于 `min(initial_delay_sec * 2^n, 60)`，且结果始终在 `[initial_delay_sec, 60]` 范围内。

**Validates: Requirements 5.1**

### Property 6: S3 配置解析与校验

*For any* 包含有效 bucket 和 region 的 kv map，`parse_s3_config()` 返回 true 且 `S3Config.bucket` 和 `S3Config.region` 与输入一致。*For any* 整数 n，解析后的 `scan_interval_sec` 满足：若 n ≥ 5 则等于 n，否则等于默认值 30。

**Validates: Requirements 6.1, 6.3**

## 错误处理

| 场景 | 处理策略 | 日志级别 |
|------|---------|---------|
| CredentialProvider 为 nullptr | `create()` 返回 nullptr + error_msg | error |
| S3 桶名为空 | `create()` 返回 nullptr + error_msg | error |
| snapshot_dir 不存在 | 跳过本次扫描，不报错 | debug |
| event.json 不存在 | 跳过该事件目录 | warn |
| event.json 解析失败 | 跳过该事件目录 | warn |
| event.json 缺少 end_time | 跳过该事件目录 | warn |
| event.json status 非 closed | 跳过该事件目录（静默） | — |
| 凭证已过期 | 跳过本次上传，等待下次扫描 | warn |
| S3 PUT 返回非 200 | 指数退避重试，最多 max_retries 次 | warn |
| libcurl 连接/传输错误 | 指数退避重试，最多 max_retries 次 | warn |
| 重试耗尽仍失败 | 中止当前事件上传，下次扫描重试 | error |
| .uploaded 标记写入失败 | 记录日志，不删除目录（下次扫描重新上传） | error |
| 本地目录删除失败 | 记录日志，下次扫描检测 .uploaded 标记重试删除 | warn |
| 文件读取失败 | 中止当前事件上传 | error |

关键原则：S3 上传的任何错误都不应影响主 streaming 管道（KVS + WebRTC + AI 推理）的正常运行。S3Uploader 的启停完全独立于 GStreamer 管道。

## 测试策略

### 测试框架

- Google Test + RapidCheck（PBT）
- 属性测试最少 100 次迭代
- 标签格式：`Feature: spec-11-s3-uploader, Property N: {property_text}`

### 属性测试（PBT）

| 属性 | 测试内容 | 生成器 |
|------|---------|--------|
| Property 1 | SHA256 输出格式 + HMAC 输出长度 + signing key 长度 | 随机字符串 [0, 1024] 字节，随机 key [1, 64] 字节 |
| Property 2 | canonical request / string-to-sign / authorization header 格式 | 随机 HTTP 方法（PUT/GET）、随机 URI 路径、随机 headers、随机 payload hash |
| Property 3 | S3 key 格式 + 非法字符拒绝 | 随机合法字符串 [a-zA-Z0-9._-] + 随机含非法字符的字符串 |
| Property 4 | 事件扫描过滤 | 随机事件目录集合：closed/active/无 event.json/缺少 end_time 混合 |
| Property 5 | 指数退避计算 | 随机 retry 次数 [0, 20]，随机 initial_delay [1, 10] |
| Property 6 | S3 配置解析 | 随机 kv map，随机 scan_interval_sec 值 [-100, 1000] |

### Example-based 测试

| 测试 | 内容 |
|------|------|
| SHA256 AWS 测试向量 | 使用 AWS 官方文档中的测试数据验证 sha256_hex 正确性 |
| HMAC-SHA256 AWS 测试向量 | 使用 AWS 官方 signing key 测试向量验证 hmac_sha256 和 derive_signing_key |
| SigV4 完整签名 AWS 测试向量 | 使用 AWS 官方 PUT Object 示例验证完整签名流程（access_key=AKIAIOSFODNN7EXAMPLE） |
| S3 key 构建 | 验证 `build_s3_key("RaspiEyeAlpha", "2026-04-12", "evt_20260412_153045", "001.jpg")` |
| S3 key 非法输入 | 验证含 `../`、空格、中文等字符返回空字符串 |
| create() nullptr 输入 | 传入 nullptr CredentialProvider → 返回 nullptr |
| create() 空桶名 | 传入空 bucket → 返回 nullptr |
| 扫描 closed 事件 | 模拟 3 个目录：closed + active + 无 event.json |
| 扫描 .uploaded 标记 | 模拟含 .uploaded 的目录 → 直接删除 |
| parse_s3_config 有效输入 | 验证默认值和自定义值 |
| parse_s3_config 无效 scan_interval | 验证 < 5 时使用默认值 30 |
| URI 编码 | 验证特殊字符编码（`$` → `%24`，`/` 在路径中不编码） |

### 不在本 Spec 测试的内容

- 真实 S3 上传（需要 AWS 凭证和网络，属于端到端测试）
- AiPipelineHandler 的事件管理逻辑（Spec 10 已测试）
- CredentialProvider 的凭证获取（Spec 7 已测试）
- AppContext 集成测试（依赖完整管道环境）

### 验证命令

```bash
# macOS Debug + ASan
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```
