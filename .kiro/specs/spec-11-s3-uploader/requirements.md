# 需求文档：Spec 11 — S3 截图上传器

## 简介

本 Spec 实现检测事件截图上传到 S3 的功能。S3Uploader 是一个后台线程模块，定期扫描 `snapshot_dir` 下的事件目录，找到 `event.json` 中 `status` 为 `"closed"` 的事件，将整个事件目录（所有 JPEG + event.json）通过 libcurl HTTP PUT + AWS SigV4 签名逐文件上传到 S3 桶，上传成功后删除本地事件目录。

凭证来源：复用 Spec 7 的 CredentialProvider 获取 STS 临时凭证（access_key_id + secret_access_key + session_token）。认证链路：IoT X.509 证书 → IoT Credentials Provider → STS 临时凭证 → SigV4 签名 → S3 PUT。S3 不支持 IoT 证书直接认证，必须经过 STS 中转。SigV4 签名使用 OpenSSL libcrypto（SHA256 + HMAC-SHA256），不引入 AWS SDK for C++。

## 前置条件

- Spec 10（ai-pipeline）✅ — 提供事件管理、JPEG 截图保存和 event.json 契约（`status: "closed"` 表示事件已关闭可上传）
- Spec 7（credential-provider）✅ — 提供 STS 临时凭证（access_key_id、secret_access_key、session_token）和 libcurl + OpenSSL 基础设施

## 术语表

- **S3Uploader**：本 Spec 新增的核心类，管理后台扫描线程、S3 上传和本地清理
- **SigV4**：AWS Signature Version 4，S3 REST API 的请求签名算法，使用 HMAC-SHA256 计算
- **snapshot_dir**：事件截图保存根目录（默认 `device/events/`），由 Spec 10 的 AiPipelineHandler 写入
- **事件目录**：`snapshot_dir` 下的 `evt_{timestamp}/` 目录，包含 JPEG 文件和 `event.json` 元数据
- **event.json**：事件元数据文件，包含 event_id、device_id、start_time、end_time、status、frame_count、detections_summary。`status: "closed"` 表示事件已关闭可上传
- **CredentialProvider**：Spec 7 实现的凭证模块，提供 `get_credentials()` 返回 `shared_ptr<const StsCredential>`
- **StsCredential**：STS 临时凭证结构体（access_key_id、secret_access_key、session_token、expiration）
- **S3 路径**：上传目标路径格式 `{device_id}/{date}/{event_id}/{filename}`，例如 `RaspiEyeAlpha/2026-04-12/evt_20260412_153045/20260412_153046_001.jpg`。date 从 event.json 的 start_time 中提取（YYYY-MM-DD 格式），便于按日期分区查询和 S3 生命周期策略
- **扫描间隔（scan_interval_sec）**：后台线程扫描 snapshot_dir 的时间间隔，默认 30 秒

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan |
| HTTP 库 | libcurl（已通过 Spec 7 引入） |
| 签名算法 | AWS SigV4（HMAC-SHA256），使用 OpenSSL libcrypto |
| S3 API | REST PUT Object（逐文件上传，非 multipart） |
| 单文件上传超时 | 连接 5s + 传输 60s（JPEG ~50-80KB，event.json ~1KB） |
| 重试策略 | 指数退避（1s → 2s → 4s → ... → 60s），单文件最大重试 3 次 |
| 扫描间隔 | 可配置，默认 30 秒 |
| 新增代码量 | 300-600 行 |
| 涉及文件 | 3-6 个 |
| 单个测试耗时 | 纯逻辑 ≤ 1 秒 |
| 日志语言 | 英文 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现 S3 桶的创建或 IAM 策略配置（属于 infra 模块或手动配置）
- SHALL NOT 在本 Spec 中实现 multipart upload（单个 JPEG 文件 50-80KB，远低于 5MB 的 multipart 阈值）
- SHALL NOT 在本 Spec 中实现上传进度通知或上传队列优先级
- SHALL NOT 在本 Spec 中实现 S3 事件通知触发 Lambda（属于 Spec 18）
- SHALL NOT 在本 Spec 中修改 AiPipelineHandler 的事件管理逻辑或 event.json 格式

### Design 层

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、S3 桶名或任何 secret（来源：安全基线）
  - 建议：S3 桶名和 region 从 config.toml 读取，凭证从 CredentialProvider 获取
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）
  - 建议：日志中只输出 S3 路径、HTTP 状态码、错误描述，不输出凭证内容
- SHALL NOT 引入 AWS SDK for C++（来源：避免额外编译依赖）
  - 原因：项目已有 libcurl + OpenSSL，SigV4 签名可用 OpenSSL libcrypto 实现，不需要引入重量级 SDK
  - 建议：使用 libcurl PUT + 手动 SigV4 签名
- SHALL NOT 在 SigV4 签名中自行实现 SHA256/HMAC 算法（来源：安全基线）
  - 原因：密码学算法自行实现容易出错
  - 建议：使用 OpenSSL libcrypto 的 `EVP_DigestInit/Update/Final` 和 `HMAC()`
- SHALL NOT 在上传线程中持有 GStreamer 对象的引用（来源：GStreamer 线程安全约束）
- SHALL NOT 在扫描或上传过程中删除 `status` 不为 `"closed"` 的事件目录（来源：数据安全）
  - 原因：`status: "active"` 的事件可能正在被 AiPipelineHandler 写入
- SHALL NOT 在 S3 key 构建中直接拼接未校验的字符串（来源：安全基线）
  - 建议：S3 路径构建函数中校验各字段仅包含字母数字、连字符、下划线和点号

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（使用 std::unique_ptr / std::make_unique）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在不确定 S3 REST API 或 SigV4 签名算法时凭猜测编写代码（来源：历史经验）
  - 建议：严格按照 AWS 官方文档实现签名步骤，使用 AWS 提供的测试向量验证

## 需求

### 需求 1：S3Uploader 类 — 生命周期管理

**用户故事：** 作为开发者，我需要一个封装类来管理 S3 上传的完整生命周期（创建 → 启动 → 停止 → 销毁），以便安全地集成到 AppContext 中。

#### 验收标准

1. THE S3Uploader SHALL 通过工厂方法 `create()` 构造，接受 S3 上传配置（桶名、region、snapshot_dir、scan_interval_sec、max_retries、device_id）和 CredentialProvider 的 shared_ptr
2. WHEN 提供有效的配置和 CredentialProvider 时，THE S3Uploader::create() SHALL 返回有效的 unique_ptr
3. IF CredentialProvider 为 nullptr，THEN THE S3Uploader::create() SHALL 返回 nullptr 并通过 error_msg 报告错误
4. IF S3 桶名为空字符串，THEN THE S3Uploader::create() SHALL 返回 nullptr 并通过 error_msg 报告错误
5. THE S3Uploader SHALL 禁用拷贝构造和拷贝赋值（`= delete`）
6. THE S3Uploader SHALL 在析构时自动停止扫描线程并释放所有资源
7. THE S3Uploader SHALL 通过 spdlog 记录创建成功信息，包含桶名、region 和扫描间隔

### 需求 2：AWS SigV4 签名计算

**用户故事：** 作为开发者，我需要实现 AWS Signature Version 4 签名算法，以便使用 STS 临时凭证对 S3 PUT 请求进行身份验证。

#### 验收标准

1. THE SigV4 签名模块 SHALL 实现 SHA256 哈希计算，使用 OpenSSL `EVP_DigestInit_ex/Update/Final` API
2. THE SigV4 签名模块 SHALL 实现 HMAC-SHA256 计算，使用 OpenSSL `HMAC()` API
3. THE SigV4 签名模块 SHALL 构建 canonical request，包含 HTTP 方法（PUT）、URI 路径（URL 编码）、查询字符串（空）、canonical headers（host、x-amz-content-sha256、x-amz-date、x-amz-security-token）和 signed headers 列表
4. THE SigV4 签名模块 SHALL 构建 string-to-sign，包含算法标识（AWS4-HMAC-SHA256）、请求时间戳（ISO 8601 基本格式 YYYYMMDD'T'HHMMSS'Z'）、credential scope（date/region/s3/aws4_request）和 canonical request 的 SHA256 哈希
5. THE SigV4 签名模块 SHALL 通过 4 步 HMAC 链计算 signing key：HMAC(HMAC(HMAC(HMAC("AWS4"+secret_key, date), region), "s3"), "aws4_request")
6. THE SigV4 签名模块 SHALL 计算最终签名：HMAC-SHA256(signing_key, string_to_sign) 的十六进制编码
7. THE SigV4 签名模块 SHALL 构建 Authorization header：`AWS4-HMAC-SHA256 Credential={access_key}/{scope}, SignedHeaders={signed_headers}, Signature={signature}`
8. THE SHA256 和 HMAC-SHA256 函数 SHALL 作为独立的纯函数（非类成员），以便独立测试
9. THE canonical request 构建函数 SHALL 作为独立的纯函数，以便独立测试
10. WHEN STS 凭证包含 session_token 时，THE SigV4 签名 SHALL 在 canonical headers 中包含 `x-amz-security-token` header

### 需求 3：S3 PUT 上传

**用户故事：** 作为开发者，我需要通过 libcurl 将文件上传到 S3 桶，使用 SigV4 签名进行身份验证。

#### 验收标准

1. THE S3Uploader SHALL 通过 libcurl 发起 HTTP PUT 请求将文件内容上传到 S3
2. THE PUT 请求 SHALL 包含以下 headers：`Host`、`x-amz-date`、`x-amz-content-sha256`、`x-amz-security-token`、`Content-Type`（JPEG 为 `image/jpeg`，JSON 为 `application/json`）、`Authorization`（SigV4 签名）
3. THE S3 上传目标路径 SHALL 为 `{device_id}/{date}/{event_id}/{filename}`，其中 date 从 event.json 的 start_time 中提取（YYYY-MM-DD 格式）
4. WHEN S3 返回 HTTP 200 时，THE S3Uploader SHALL 视为上传成功
5. WHEN S3 返回 HTTP 非 200 或 libcurl 返回错误时，THE S3Uploader SHALL 记录 warn 日志并触发重试
6. THE PUT 请求 SHALL 设置连接超时 5 秒和传输超时 60 秒
7. THE S3 上传函数 SHALL 可通过函数指针/std::function 注入，以便测试时 mock
8. THE S3 路径构建函数 SHALL 作为独立的纯函数，接受 device_id、date、event_id、filename，返回 S3 key 字符串，以便独立测试

### 需求 4：事件目录扫描与上传调度

**用户故事：** 作为运维人员，我需要 S3Uploader 自动扫描本地事件目录并上传已关闭的事件，无需人工干预。

#### 验收标准

1. THE S3Uploader SHALL 在 `start()` 时创建后台扫描线程
2. THE 扫描线程 SHALL 每隔 scan_interval_sec 秒扫描一次 snapshot_dir 目录
3. WHEN 扫描到事件目录时，THE S3Uploader SHALL 读取该目录下的 `event.json`，仅处理 `status` 为 `"closed"` 的事件
4. WHEN 事件目录中不存在 `event.json` 或 `event.json` 解析失败或缺少 `end_time` 字段时，THE S3Uploader SHALL 记录 warn 日志并跳过该目录（下次扫描重试）
5. WHEN 事件 status 不为 `"closed"` 时，THE S3Uploader SHALL 跳过该事件目录（不上传、不删除）
6. WHEN 找到可上传的事件时，THE S3Uploader SHALL 逐文件上传事件目录中的所有文件（JPEG + event.json）
7. WHEN 事件目录中所有文件均上传成功时，THE S3Uploader SHALL 先写入 `.uploaded` 标记文件，然后删除本地事件目录。崩溃恢复时检测到 `.uploaded` 标记则直接删除目录
8. WHEN 事件目录中任一文件上传失败（重试耗尽）时，THE S3Uploader SHALL 跳过该事件，记录 error 日志，在下次扫描时重试
9. THE S3Uploader SHALL 在 `stop()` 时通知扫描线程退出并 join 等待线程结束
10. THE S3Uploader SHALL 通过 spdlog 记录每次扫描的结果：发现的事件数、上传成功数、跳过数
11. WHEN snapshot_dir 不存在时，THE S3Uploader SHALL 跳过本次扫描（不报错）

### 需求 5：上传失败重试

**用户故事：** 作为运维人员，我需要上传失败时自动重试，以应对临时网络抖动。

#### 验收标准

1. WHEN 单个文件上传失败时，THE S3Uploader SHALL 使用指数退避策略重试：初始延迟 1 秒，每次翻倍，最大延迟 60 秒
2. THE 单个文件最大重试次数 SHALL 为 3 次（可通过配置调整）
3. WHEN 重试次数耗尽仍失败时，THE S3Uploader SHALL 中止当前事件的上传，记录 error 日志
4. THE 重试等待期间 SHALL 响应 stop 信号（通过 condition_variable wait_for），不阻塞优雅退出
5. WHEN 凭证已过期时，THE S3Uploader SHALL 跳过本次上传尝试，等待下次扫描

### 需求 6：配置文件扩展

**用户故事：** 作为开发者，我需要通过 config.toml 配置 S3 上传参数。

#### 验收标准

1. THE config.toml SHALL 新增 `[s3]` 配置段：`bucket`（S3 桶名）、`region`（默认与 `[kvs].aws_region` 一致）、`scan_interval_sec`（默认 30）、`max_retries`（默认 3）
2. WHEN `[s3].bucket` 为空或未配置时，THE AppContext SHALL 跳过 S3Uploader 创建
3. THE `scan_interval_sec` SHALL 为 ≥ 5 的正整数，超出范围时使用默认值 30
4. THE S3Uploader SHALL 从 `[ai].snapshot_dir` 读取事件目录根路径
5. THE S3Uploader SHALL 从 `[aws].thing_name` 读取 device_id

### 需求 7：AppContext 集成

**用户故事：** 作为开发者，我需要将 S3Uploader 集成到 AppContext 生命周期中。

#### 验收标准

1. THE AppContext::Impl SHALL 新增 `std::unique_ptr<S3Uploader>` 成员
2. WHEN config.toml 中配置了 S3 桶名且 CredentialProvider 可用时，THE AppContext::init() SHALL 创建 S3Uploader
3. IF S3 桶名未配置，THEN 跳过创建，记录 info 日志
4. THE AppContext::start() SHALL 调用 `s3_uploader->start()`
5. THE AppContext::stop() SHALL 通过 ShutdownHandler 停止 S3Uploader
6. THE S3Uploader 的启停 SHALL 独立于 GStreamer 管道

### 需求 8：单元测试

**用户故事：** 作为开发者，我需要通过单元测试验证 S3 上传模块的正确性。

#### 验收标准

1. THE Test_Suite SHALL 包含 SigV4 签名的 example-based 测试：使用 AWS 官方测试向量验证 SHA256、HMAC-SHA256、canonical request、string-to-sign 和最终签名
2. THE Test_Suite SHALL 包含 SigV4 签名的 PBT 属性测试：签名结果恒为 64 字符十六进制字符串
3. THE Test_Suite SHALL 包含事件目录扫描逻辑的测试：模拟 closed/active/无 event.json 三种情况
4. THE Test_Suite SHALL 包含 S3 路径构建的 example-based 和 PBT 测试
5. THE Test_Suite SHALL 包含 parse_s3_config 的测试
6. THE Test_Suite SHALL 保持现有测试全部通过
7. WHEN 在 macOS Debug（ASan）构建下运行时，THE Test_Suite SHALL 无内存错误报告

### 需求 9：双平台构建验证

**用户故事：** 作为开发者，我需要确保 S3 上传模块在 macOS 和 Pi 5 上均能成功编译和测试。

#### 验收标准

1. WHEN 在 macOS 上执行构建和测试时，THE Build_System SHALL 编译成功且所有测试通过
2. WHEN 在 Pi 5 上执行构建和测试时，THE Build_System SHALL 编译成功且所有测试通过
3. THE S3Uploader 源文件 SHALL 始终编译（不依赖 ENABLE_YOLO 开关）
4. THE Build_System SHALL 保持现有所有测试行为不变

## 参考代码

### AWS SigV4 签名步骤（OpenSSL libcrypto）

```cpp
// SHA256 哈希
std::string sha256_hex(const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    return to_hex(hash, hash_len);
}

// HMAC-SHA256
std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                  const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         result, &result_len);
    return std::vector<uint8_t>(result, result + result_len);
}

// Signing key（4 步 HMAC 链）
auto date_key = hmac_sha256(to_bytes("AWS4" + secret_key), date_str);
auto region_key = hmac_sha256(date_key, region);
auto service_key = hmac_sha256(region_key, "s3");
auto signing_key = hmac_sha256(service_key, "aws4_request");
auto signature = to_hex(hmac_sha256(signing_key, string_to_sign));
```

### libcurl PUT 上传

```cpp
bool s3_put_object(CURL* curl, const std::string& url,
                   const std::vector<uint8_t>& data,
                   const std::vector<std::string>& headers) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)data.size());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    // ... set headers, read callback ...
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    return (res == CURLE_OK && http_code == 200);
}
```

### 事件目录扫描

```cpp
std::vector<std::string> scan_closed_events(const std::string& snapshot_dir) {
    std::vector<std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(snapshot_dir)) {
        if (!entry.is_directory()) continue;
        // 检查 .uploaded 标记 → 直接删除
        if (std::filesystem::exists(entry.path() / ".uploaded")) {
            std::filesystem::remove_all(entry.path());
            continue;
        }
        auto event_json_path = entry.path() / "event.json";
        if (!std::filesystem::exists(event_json_path)) continue;
        auto j = nlohmann::json::parse(read_file(event_json_path));
        if (j.value("status", "") == "closed" && j.contains("end_time")) {
            result.push_back(entry.path().string());
        }
    }
    return result;
}
```

## 验证命令

```bash
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```

## 明确不包含

- S3 桶创建或 IAM 策略配置（手动或 infra 模块）
- Multipart upload（文件太小，不需要）
- S3 事件通知触发 Lambda（Spec 18）
- 云端 AI 推理（Spec 17）
- 上传进度通知或上传队列优先级
- 事件目录的自动轮转或磁盘空间管理
- 修改 AiPipelineHandler 的事件管理逻辑或 event.json 格式
- AWS SDK for C++（使用 libcurl + OpenSSL 手动签名）
