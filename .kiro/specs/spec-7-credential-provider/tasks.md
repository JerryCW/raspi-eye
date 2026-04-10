# 实施计划：Spec 7 — 设备端 AWS IoT 凭证获取模块

## 概述

为 raspi-eye 项目实现设备端凭证获取模块 `CredentialProvider`。通过 libcurl + mTLS 向 AWS IoT Credentials Provider 发起 HTTPS 请求，获取 STS 临时凭证，并在后台线程自动刷新。按依赖顺序：CMakeLists.txt 更新（libcurl + nlohmann/json）→ credential_provider.h（数据结构 + 接口声明）→ credential_provider.cpp（TOML 解析 → 证书预检查 → CurlHttpClient → JSON 解析 → CredentialProvider 核心）→ credential_test.cpp（9 个 PBT 属性 + 7 个 example-based 测试，含 1 个端到端集成测试）→ 双平台验证。实现语言为 C++17。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中输出密钥、证书内容、token 等敏感信息（仅输出 thing_name、endpoint、role_alias 等非敏感标识）
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件，使用 fsWrite / fsAppend 工具
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、yolo_test）
- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret
- SHALL NOT 在 `get_credentials()` 中发起网络请求（纯内存操作，返回 shared_ptr 快照）
- SHALL NOT 实现完整的 TOML 解析器（仅支持 `[section]` + `key = "value"` / `key = value` + `#` 注释）
- SHALL NOT 在 credential_provider.h 中包含 `<curl/curl.h>`（curl 细节仅在 .cpp 中）
- SHALL NOT 在不确定 libcurl API 用法时凭猜测编写代码，必须参照设计文档中的参考代码

## 任务

- [x] 1. CMake 配置与依赖引入
  - [x] 1.1 修改 `device/CMakeLists.txt` — 添加 nlohmann/json 和 libcurl 依赖
    - 在 RapidCheck FetchContent 之后添加 nlohmann/json v3.11.3（FetchContent，URL 方式下载 json.tar.xz）
    - 添加 `find_package(CURL REQUIRED)` 引入系统 libcurl
    - 添加 `credential_module` 静态库（`src/credential_provider.cpp`），链接 `nlohmann_json::nlohmann_json`、`CURL::libcurl`、`spdlog::spdlog`、`log_module`
    - 添加 `credential_test` 测试目标（`tests/credential_test.cpp`），链接 `credential_module`、`GTest::gtest_main`、`rapidcheck`、`rapidcheck_gtest`
    - `add_test(NAME credential_test COMMAND credential_test)`
    - 不修改现有 `pipeline_manager`、`log_module`、`yolo_module` 及所有现有测试目标
    - _需求：约束条件（libcurl 系统包、nlohmann/json FetchContent）_

- [x] 2. 头文件与数据结构定义
  - [x] 2.1 创建 `device/src/credential_provider.h`
    - 定义 `AwsConfig` POD 结构体：thing_name、credential_endpoint、role_alias、cert_path、key_path、ca_path
    - 定义 `StsCredential` 结构体：access_key_id、secret_access_key、session_token、expiration（system_clock::time_point）
    - 定义 `TlsConfig` 结构体：cert_path、key_path、ca_path
    - 定义 `HttpResponse` 结构体：status_code（long）、body、error_message
    - 定义 `CredentialCallback` 类型别名（`std::function<void()>`）
    - 声明 TOML 解析函数：`parse_toml_section()`、`build_aws_config()`
    - 声明 JSON 解析函数：`parse_credential_json()`、`parse_iso8601()`
    - 声明证书预检查函数：`file_exists()`、`is_pem_format()`、`check_key_permissions()`、`validate_cert_files()`
    - 声明 `HttpClient` 纯虚接口（`virtual HttpResponse get(url, headers, tls) = 0`）
    - 声明 `CurlHttpClient` 类（继承 HttpClient，拷贝 `= delete`）
    - 声明 `CredentialProvider` 类（工厂方法 `create()`、`get_credentials()`、`is_expired()`、`set_credential_callback()`、`config()`，拷贝 `= delete`）
    - 头文件不包含 `<curl/curl.h>`
    - 完整按照设计文档中的头文件定义实现
    - _需求：1.1, 2.1, 2.2, 2.3, 2.4, 3.1, 4.1, 4.2, 5.1, 5.5, 6.1_

- [x] 3. 核心实现
  - [x] 3.1 创建 `device/src/credential_provider.cpp` — TOML 解析器与证书预检查
    - 实现 `trim()` 辅助函数（去除首尾空白）
    - 实现 `parse_toml_section()`：逐行读取文件，检测 `[section]` 头，解析 `key = "value"` / `key = value`，跳过 `#` 注释和空行
    - 实现 `build_aws_config()`：从 key-value map 提取 6 个必要字段，缺失时返回 false 并在 error_msg 中列出缺失字段名
    - 实现 `file_exists()`：检查文件存在性
    - 实现 `is_pem_format()`：检查文件包含 `-----BEGIN` 和 `-----END` 标记
    - 实现 `check_key_permissions()`：检查私钥文件权限（chmod 400/600，group/other 无权限）
    - 实现 `validate_cert_files()`：组合调用上述三个函数验证证书文件
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5_

  - [x] 3.2 在 `device/src/credential_provider.cpp` 中添加 JSON 解析与 ISO 8601 解析
    - 实现 `parse_iso8601()`：解析 `2025-01-01T00:00:00Z` 格式，使用 `std::get_time` + `timegm`
    - 实现 `parse_credential_json()`：使用 nlohmann/json 解析 `credentials` 对象的 4 个字段（accessKeyId、secretAccessKey、sessionToken、expiration），异常时返回 false + 错误信息
    - _需求：3.1, 3.2, 3.3, 3.4, 3.5_

  - [x] 3.3 在 `device/src/credential_provider.cpp` 中添加 CurlHttpClient 实现
    - 实现 `ensure_curl_global_init()`：static lambda + `std::atexit(curl_global_cleanup)` 保证进程级单次调用
    - 实现 `write_callback()`：libcurl 写回调，将数据追加到 `std::string`
    - 实现 `CurlHttpClient::get()`：设置 URL、headers（curl_slist）、mTLS 证书/私钥/CA、TLS 1.2-1.3 版本限制、CAPATH fallback、连接超时 5s + 完成超时 10s、执行请求、获取状态码
    - 按照设计文档中的参考代码实现，不凭猜测
    - _需求：2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 5.2, 5.3_

  - [x] 3.4 在 `device/src/credential_provider.cpp` 中添加 CredentialProvider 核心实现
    - 实现私有构造函数
    - 实现 `create()` 工厂方法：解析 TOML → 验证证书文件 → 同步获取首次凭证 → 启动后台刷新线程；失败返回 nullptr + 错误信息
    - 实现 `fetch_credentials()`：构建 URL + headers + TlsConfig → 调用 http_client_->get() → 检查错误/状态码 → 解析 JSON → 原子替换缓存
    - 实现 `get_credentials()`：shared_lock 读取 cached_credential_，返回 shared_ptr 快照
    - 实现 `is_expired()`：比较 expiration 与当前时间
    - 实现 `set_credential_callback()`
    - 实现 `refresh_loop()`：wait_until(expiration - 5min) → fetch_credentials → 失败时指数退避重试（1s→2s→...→60s，最多 10 次）→ 连续失败且过期时触发回调
    - 实现析构函数：设置 stop_requested_ → notify_all → join 线程
    - _需求：4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 4.8, 4.9, 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7_

- [x] 4. 检查点 — 编译通过
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug` 配置
  - 执行 `cmake --build device/build` 编译
  - 确认编译无错误（需要系统已安装 libcurl：macOS `brew install curl`，Pi 5 `apt install libcurl4-openssl-dev`）
  - 确认现有测试（smoke_test、log_test、tee_test、camera_test、health_test）全部通过
  - 如有问题，询问用户

- [x] 5. 测试实现
  - [x] 5.1 创建 `device/tests/credential_test.cpp` — 测试辅助设施与 example-based 测试
    - 包含 `credential_provider.h`、`gtest/gtest.h`、`rapidcheck.h`、`rapidcheck/gtest.h`
    - 实现 `MockHttpClient`：继承 HttpClient，记录请求参数（last_url、last_headers、last_tls、call_count），返回预设 preset_response
    - 实现 `make_credential_json()` 辅助函数：生成有效的 JSON 凭证响应字符串
    - 实现 `make_toml_aws_section()` 辅助函数：生成有效的 TOML `[aws]` section 字符串
    - 实现 `write_temp_file()` 辅助函数：写入临时文件并返回路径
    - Example-based 测试：
      - `CreateSuccess`：mock 返回有效凭证 → create() 成功 → get_credentials() 有值 — _需求：5.6_
      - `CreateFailsOnHttpError`：mock 返回 curl 错误 → create() 返回 nullptr — _需求：5.7, 2.6_
      - `CreateFailsOnMissingConfig`：配置文件不存在 → create() 返回 nullptr — _需求：1.2_
      - `CreateFailsOnInvalidJson`：mock 返回非法 JSON → create() 返回 nullptr — _需求：3.2_
      - `DestructorGracefulShutdown`：创建后立即析构 → 2 秒内完成 — _需求：5.4_
      - `NoCopyable`：static_assert 验证 CredentialProvider 不可拷贝 — _需求：5.5_
      - `FetchRealCredentials`（集成测试）：检测 `device/config/config.toml` 存在时，使用 CurlHttpClient 发起真实 mTLS 请求获取凭证，验证 access_key_id 非空、session_token 非空、未过期；config.toml 不存在时 GTEST_SKIP — _需求：2.1, 2.2, 2.3, 2.4, 3.1_
    - _需求：6.1, 6.2, 6.3, 6.4, 6.5_

  - [x] 5.2 在 `device/tests/credential_test.cpp` 中添加 PBT — Property 1: TOML 解析 round-trip
    - **Property 1: TOML 解析 round-trip**
    - 随机生成有效 AwsConfig（6 个字段均为非空 ASCII 字符串），序列化为 TOML `[aws]` section（随机选择带引号/不带引号），写入临时文件，解析后验证每个字段值与原始一致
    - **验证：需求 1.1, 1.4**

  - [x] 5.3 在 `device/tests/credential_test.cpp` 中添加 PBT — Property 2: TOML 注释和空行不影响解析
    - **Property 2: TOML 注释和空行不影响解析**
    - 随机在有效 TOML 内容中插入注释行（`#` 开头）和空行，解析结果与未插入版本一致
    - **验证：需求 1.5**

  - [x] 5.4 在 `device/tests/credential_test.cpp` 中添加 PBT — Property 3: TOML 缺失字段检测
    - **Property 3: TOML 缺失字段检测**
    - 随机移除 6 个必要字段的非空真子集，`build_aws_config()` 返回 false，错误信息包含所有被移除字段名
    - **验证：需求 1.3**

  - [x] 5.5 在 `device/tests/credential_test.cpp` 中添加 PBT — Property 4: HTTP 请求参数完整性
    - **Property 4: HTTP 请求参数完整性**
    - 随机生成 AwsConfig，通过 MockHttpClient 捕获 fetch_credentials() 的请求参数，验证 URL 格式、header、TlsConfig 与配置一致
    - **验证：需求 2.1, 2.2, 2.3, 2.4**

  - [x] 5.6 在 `device/tests/credential_test.cpp` 中添加 PBT — Property 5: 非 200 状态码返回错误
    - **Property 5: 非 200 状态码返回错误**
    - 随机生成 HTTP 状态码 N（N ≠ 200，N ∈ [100, 599]），mock 返回该状态码，fetch_credentials() 返回 false，错误信息包含状态码数字
    - **验证：需求 2.7**

  - [x] 5.7 在 `device/tests/credential_test.cpp` 中添加 PBT — Property 6: JSON 凭证解析 round-trip
    - **Property 6: JSON 凭证解析 round-trip**
    - 随机生成有效 StsCredential（非空字符串 + 合理时间范围 time_point），序列化为 AWS JSON 格式，解析后验证每个字段一致（expiration 精确到秒）
    - **验证：需求 3.1, 3.4, 3.5**

  - [x] 5.8 在 `device/tests/credential_test.cpp` 中添加 PBT — Property 7: JSON 缺失字段检测
    - **Property 7: JSON 缺失字段检测**
    - 随机移除 4 个必要 JSON 字段的非空真子集，`parse_credential_json()` 返回 false，错误信息非空
    - **验证：需求 3.3**

  - [x] 5.9 在 `device/tests/credential_test.cpp` 中添加 PBT — Property 8: 缓存读取不触发网络请求
    - **Property 8: 缓存读取不触发网络请求**
    - 初始化成功后连续调用 N 次（N ∈ [2, 100]）get_credentials()，MockHttpClient 的 call_count 仅为 1
    - **验证：需求 4.3**

  - [x] 5.10 在 `device/tests/credential_test.cpp` 中添加 PBT — Property 9: 过期判断正确性
    - **Property 9: 过期判断正确性**
    - 随机生成 StsCredential，expiration 早于当前时间 → is_expired() 返回 true；晚于当前时间超过 1 秒 → 返回 false
    - **验证：需求 6.4**

- [x] 6. 最终检查点 — 全量验证
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug` 配置
  - 执行 `cmake --build device/build` 编译
  - 执行 `ctest --test-dir device/build --output-on-failure` 运行所有测试
  - 确认所有测试通过（现有 smoke_test、log_test、tee_test、camera_test、health_test + 可选 yolo_test + 新增 credential_test）
  - 确认 ASan 无内存错误报告
  - 确认现有测试行为不变
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：全部需求覆盖验证_

## 备注

- 新建文件：`device/src/credential_provider.h`、`device/src/credential_provider.cpp`、`device/tests/credential_test.cpp`
- 修改文件：`device/CMakeLists.txt`
- 不修改文件：所有现有 src 文件和测试文件
- 新增依赖：libcurl（系统包）、nlohmann/json v3.11.3（FetchContent）
- 所有测试使用 MockHttpClient，不需要真实 AWS 环境（集成测试 `FetchRealCredentials` 除外，需要 config.toml + 证书文件，不存在时自动跳过）
- PBT 使用 RapidCheck，每个属性最少 100 次迭代
- 标记 `*` 的子任务为可选（PBT 属性测试），可跳过以加速 MVP
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
