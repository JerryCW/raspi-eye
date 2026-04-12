# 实施计划：Spec 11 — S3 截图上传器

## 概述

实现 S3Uploader 模块，将 Spec 10 AiPipelineHandler 产生的检测事件截图通过 libcurl PUT + AWS SigV4 签名上传到 S3。按依赖顺序：SigV4 纯函数（sha256_hex、hmac_sha256、derive_signing_key 等）→ S3 路径构建纯函数 → S3Uploader 类（扫描线程、libcurl PUT、重试逻辑）→ ConfigManager 扩展（parse_s3_config + [s3] section）→ AppContext 集成 → CMakeLists.txt 更新（s3_module + s3_test + OpenSSL::Crypto）→ config.toml 新增 [s3] section → 测试（AWS 官方测试向量 + PBT + 扫描逻辑 + 路径构建）。实现语言为 C++17，包含 6 个 PBT 属性 + example-based 测试。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（使用 std::unique_ptr / std::make_unique）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 在不确定 S3 REST API 或 SigV4 签名算法时凭猜测编写代码，严格按照 AWS 官方文档和设计文档中的参考代码实现
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、yolo_test、ai_pipeline_test 等）
- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、S3 桶名或任何 secret
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
- SHALL NOT 引入 AWS SDK for C++，使用 libcurl PUT + 手动 SigV4 签名
- SHALL NOT 在 SigV4 签名中自行实现 SHA256/HMAC 算法，必须使用 OpenSSL libcrypto
- SHALL NOT 在上传线程中持有 GStreamer 对象的引用
- SHALL NOT 在 S3 key 构建中直接拼接未校验的字符串

## 任务

- [x] 1. SigV4 纯函数与 S3 路径构建
  - [x] 1.1 创建 `device/src/s3_uploader.h` — SigV4 纯函数声明与 S3Uploader 类声明
    - 声明 `sha256_hex()` 两个重载（string 和 uint8_t* + len）
    - 声明 `hmac_sha256()` 两个重载（vector key 和 string key）
    - 声明 `to_hex()` 两个重载
    - 声明 `uri_encode()`
    - 声明 `build_canonical_request()`、`build_string_to_sign()`、`derive_signing_key()`、`build_authorization_header()`
    - 声明 `build_s3_key()` 纯函数
    - 声明 `S3PutFunction` 类型别名
    - 声明 `S3Uploader` 类：工厂方法 create()、start()、stop()、set_put_function()
    - 禁用拷贝构造和拷贝赋值
    - 参照设计文档中的完整接口定义
    - _需求：1.1, 1.5, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.7, 3.8_

  - [x] 1.2 创建 `device/src/s3_uploader.cpp` — SigV4 纯函数实现
    - 实现 `to_hex()`：字节数组转小写十六进制字符串
    - 实现 `sha256_hex()`：使用 OpenSSL `EVP_DigestInit_ex/Update/Final` API，参照设计文档参考代码
    - 实现 `hmac_sha256()`：使用 OpenSSL `HMAC()` API，参照设计文档参考代码
    - 实现 `uri_encode()`：S3 路径中 '/' 不编码，其他特殊字符百分号编码
    - 实现 `build_canonical_request()`：PUT + URI + 空查询 + canonical headers + signed headers + payload hash，6 部分用 `\n` 分隔
    - 实现 `build_string_to_sign()`：AWS4-HMAC-SHA256 + timestamp + scope + canonical request hash，4 行
    - 实现 `derive_signing_key()`：4 步 HMAC 链（"AWS4"+secret → date → region → service → "aws4_request"）
    - 实现 `build_authorization_header()`：拼接 Credential + SignedHeaders + Signature
    - 实现 `build_s3_key()`：校验各字段仅包含 `[a-zA-Z0-9._-]`，非法字符返回空字符串，合法时返回 `{device_id}/{date}/{event_id}/{filename}`
    - _需求：2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.3, 3.8_

- [x] 2. S3Uploader 类核心实现
  - [x] 2.1 在 `device/src/s3_uploader.cpp` 中实现 S3Uploader 类
    - 实现 `create()` 工厂方法：检查 CredentialProvider 非空、bucket 非空，构造实例，spdlog::info 记录桶名、region、扫描间隔
    - 实现私有构造函数：初始化配置、snapshot_dir、device_id、credential_provider
    - 实现默认 `put_fn_`：libcurl PUT 上传，设置 CURLOPT_UPLOAD、CURLOPT_CONNECTTIMEOUT=5、CURLOPT_TIMEOUT=60，read callback 读取 vector<uint8_t> 数据
    - 实现 `set_put_function()`：注入自定义 PUT 函数（测试用）
    - 实现 `start()`：创建后台扫描线程
    - 实现 `stop()`：设置 stop_requested_ + cv_.notify_all + join
    - 实现析构函数：调用 stop()
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 3.1, 3.6, 3.7_

  - [x] 2.2 在 `device/src/s3_uploader.cpp` 中实现扫描与上传逻辑
    - 实现 `scan_loop()`：cv_.wait_for 等待 scan_interval_sec，每次扫描 snapshot_dir
    - 实现 `scan_closed_events()` 辅助函数：遍历 snapshot_dir，检查 .uploaded 标记（直接删除）、读取 event.json、过滤 status=="closed" 且包含 end_time
    - 实现 `upload_event()`：从 event.json 提取 date 和 event_id，遍历事件目录中所有文件，逐文件调用 upload_file()
    - 实现 `upload_file()`：读取文件内容 → sha256_hex 计算 payload hash → 获取 STS 凭证 → 构建 SigV4 签名 → 调用 put_fn_
    - 实现指数退避重试：初始 1s，每次翻倍，最大 60s，通过 cv_.wait_for 响应 stop 信号
    - 全部成功 → 写入 .uploaded 标记 → 删除本地目录
    - spdlog 记录每次扫描结果：发现事件数、上传成功数、跳过数
    - _需求：3.1, 3.2, 3.3, 3.4, 3.5, 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 4.8, 4.9, 4.10, 4.11, 5.1, 5.2, 5.3, 5.4, 5.5_

- [x] 3. 配置管理扩展
  - [x] 3.1 修改 `device/src/config_manager.h` — 新增 S3Config 支持
    - 定义 `S3Config` POD 结构体（bucket、region、scan_interval_sec=30、max_retries=3）
    - 声明 `parse_s3_config()` 纯函数
    - 在 `ConfigManager` 类中新增 `s3_config_` 成员和 `s3_config()` 访问器
    - _需求：6.1_

  - [x] 3.2 修改 `device/src/config_manager.cpp` — 实现 parse_s3_config()
    - 实现 `parse_s3_config()`：解析 bucket、region、scan_interval_sec（< 5 时使用默认值 30）、max_retries
    - 在 `ConfigManager::load()` 中调用 `parse_s3_config()` 解析 `[s3]` section
    - _需求：6.1, 6.3_

- [x] 4. AppContext 集成与 CMake 更新
  - [x] 4.1 修改 `device/src/app_context.cpp` — 集成 S3Uploader 生命周期
    - Impl 新增 `std::unique_ptr<S3Uploader>` 成员
    - init()：当 s3_config.bucket 非空且 CredentialProvider 可用时，创建 S3Uploader（snapshot_dir 从 ai_config 读取，device_id 从 aws_config.thing_name 读取）；否则跳过，记录 info 日志
    - start()：调用 s3_uploader->start()
    - stop()：在 ShutdownHandler 中注册 s3_uploader->stop()
    - S3Uploader 的启停独立于 GStreamer 管道
    - _需求：7.1, 7.2, 7.3, 7.4, 7.5, 7.6_

  - [x] 4.2 修改 `device/CMakeLists.txt` — 新增 s3_module 和 s3_test
    - 新增 `s3_module` 静态库：源文件 `src/s3_uploader.cpp`，链接 `credential_module nlohmann_json::nlohmann_json CURL::libcurl spdlog::spdlog`
    - `find_package(OpenSSL REQUIRED)` + `target_link_libraries(s3_module PUBLIC OpenSSL::Crypto)`
    - `target_link_libraries(pipeline_manager PUBLIC s3_module)`
    - 新增 `s3_test` 测试目标，链接 `s3_module GTest::gtest_main rapidcheck rapidcheck_gtest`
    - s3_module 始终编译，不依赖 ENABLE_YOLO 开关
    - _需求：9.3, 9.4_

  - [x] 4.3 修改 `device/config/config.toml` — 新增 [s3] section
    - 添加 bucket、region、scan_interval_sec、max_retries 配置项
    - 参照设计文档中的 config.toml 示例
    - _需求：6.1, 6.4, 6.5_

- [x] 5. 检查点 — 编译通过 + 现有测试回归
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认编译无错误，现有测试全部通过
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户

- [x] 6. 测试 — Example-Based 与 PBT
  - [x] 6.1 创建 `device/tests/s3_test.cpp` — SigV4 example-based 测试
    - SHA256 AWS 官方测试向量：验证 sha256_hex 对已知输入的输出
    - HMAC-SHA256 AWS 官方测试向量：验证 hmac_sha256 和 derive_signing_key
    - SigV4 完整签名 AWS 官方测试向量：使用 AWS 官方 PUT Object 示例（access_key=AKIAIOSFODNN7EXAMPLE）验证完整签名流程
    - build_s3_key 正常输入：验证 `build_s3_key("RaspiEyeAlpha", "2026-04-12", "evt_20260412_153045", "001.jpg")` 返回正确路径
    - build_s3_key 非法输入：验证含 `../`、空格、中文等字符返回空字符串
    - create() nullptr 输入：传入 nullptr CredentialProvider → 返回 nullptr + error_msg
    - create() 空桶名：传入空 bucket → 返回 nullptr + error_msg
    - URI 编码测试：验证特殊字符编码（`$` → `%24`，`/` 在路径中不编码）
    - parse_s3_config 有效输入：验证默认值和自定义值
    - parse_s3_config 无效 scan_interval：验证 < 5 时使用默认值 30
    - _需求：8.1, 8.2, 8.3, 8.4, 8.5_

  - [x] 6.2 添加 PBT — Property 1: SigV4 密码学输出不变量
    - **Property 1: SigV4 密码学输出不变量**
    - 随机字符串 [0, 1024] 字节：sha256_hex 输出恒为 64 字符且仅包含 `[0-9a-f]`
    - 随机 key [1, 64] 字节 + 随机 data：hmac_sha256 输出恒为 32 字节
    - 随机 secret_key、date、region、service：derive_signing_key 输出恒为 32 字节
    - **验证：需求 2.1, 2.2, 2.5, 2.6**

  - [x] 6.3 添加 PBT — Property 2: SigV4 签名构建格式不变量
    - **Property 2: SigV4 签名构建格式不变量**
    - 随机 HTTP 方法（PUT/GET）、随机 URI 路径、随机 headers、随机 payload hash
    - build_canonical_request 输出包含恰好 5 个 `\n` 分隔符（6 部分），第一行为 HTTP 方法
    - build_string_to_sign 输出第一行恒为 `"AWS4-HMAC-SHA256"`，共 4 行
    - build_authorization_header 输出以 `"AWS4-HMAC-SHA256 Credential="` 开头，包含 `"SignedHeaders="` 和 `"Signature="` 子串
    - **验证：需求 2.3, 2.4, 2.7**

  - [x] 6.4 添加 PBT — Property 3: S3 key 构建格式与安全性
    - **Property 3: S3 key 构建格式与安全性**
    - 随机合法字符串 `[a-zA-Z0-9._-]`：build_s3_key 输出恰好包含 3 个 `/` 分隔符，各段分别等于输入
    - 随机含非法字符的字符串（`../`、空格、特殊符号）：build_s3_key 返回空字符串
    - **验证：需求 3.3, 3.8**

  - [x] 6.5 添加 PBT — Property 4: 事件扫描只返回 closed 事件
    - **Property 4: 事件扫描只返回 closed 事件**
    - 随机事件目录集合：closed/active/无 event.json/缺少 end_time 混合
    - 在临时目录中创建模拟事件目录，调用 scan_closed_events
    - 验证返回结果中每个事件的 event.json status 恒为 "closed" 且包含 end_time
    - 验证返回结果是输入中 closed 事件的子集
    - **验证：需求 4.3, 4.4, 4.5**

  - [x] 6.6 添加 PBT — Property 5: 指数退避计算
    - **Property 5: 指数退避计算**
    - 随机 retry 次数 [0, 20]，随机 initial_delay [1, 10]
    - 验证退避延迟恒等于 `min(initial_delay * 2^n, 60)`
    - 验证结果始终在 `[initial_delay, 60]` 范围内
    - **验证：需求 5.1**

  - [x] 6.7 添加 PBT — Property 6: S3 配置解析与校验
    - **Property 6: S3 配置解析与校验**
    - 随机 kv map（含有效 bucket 和 region）：parse_s3_config 返回 true 且字段一致
    - 随机 scan_interval_sec 值 [-100, 1000]：≥ 5 时等于输入值，< 5 时等于默认值 30
    - **验证：需求 6.1, 6.3**

- [x] 7. 检查点 — 全量测试通过
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过（现有 + 新增 s3_test）
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户

- [x] 8. 最终检查点 — 双平台与回归验证
  - 确认 `-DENABLE_YOLO=OFF` 时 s3_module 仍然编译（不依赖 ENABLE_YOLO 开关）
  - 确认现有测试（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、config_test 等）行为不变
  - Pi 5 Release 验证（`scripts/pi-build.sh` 或手动 SSH）需要 Pi 5 可达，不可达则标注跳过
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：9.1, 9.2, 9.3, 9.4_

## 备注

- 新建文件：`device/src/s3_uploader.h`、`device/src/s3_uploader.cpp`、`device/tests/s3_test.cpp`
- 修改文件：`device/src/config_manager.h`、`device/src/config_manager.cpp`、`device/src/app_context.cpp`、`device/CMakeLists.txt`、`device/config/config.toml`
- 不修改文件：所有现有测试文件、credential_provider.h/cpp、ai_pipeline_handler.h/cpp
- SigV4 纯函数（sha256_hex、hmac_sha256、derive_signing_key 等）和 build_s3_key 优先实现并测试，不依赖网络或 S3
- PBT 使用 RapidCheck，每个属性最少 100 次迭代
- 标签格式：`Feature: spec-11-s3-uploader, Property N: {property_text}`
- SigV4 签名使用 OpenSSL libcrypto（EVP_DigestInit/Update/Final + HMAC），不自行实现密码学算法
- libcurl PUT 上传通过 S3PutFunction 注入，测试时 mock，不发起真实 S3 请求
- s3_module 始终编译（不依赖 ENABLE_YOLO），因为 S3 上传功能独立于 AI 推理
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
