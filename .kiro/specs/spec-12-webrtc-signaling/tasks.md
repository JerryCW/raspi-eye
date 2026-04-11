# 实施计划：Spec 12 — KVS WebRTC 信令通道

## 概述

集成 KVS WebRTC C SDK 的信令客户端，实现设备端以 Master 角色连接到 KVS WebRTC 信令通道。核心新增模块 WebRtcSignaling 封装信令客户端的创建、连接、回调注册和消息发送，通过 pImpl + 条件编译（`HAVE_KVS_WEBRTC_SDK` 宏）隔离平台实现。按依赖顺序：webrtc_signaling.h/cpp → CMakeLists.txt 更新 → 编译检查点 → config.toml.example 更新 → webrtc_test.cpp 测试（example-based + 2 个 PBT）→ provision-device.sh 扩展 → 最终检查点。实现语言为 C++17。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 KVS WebRTC C SDK C API 外）
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、kvs_test、yolo_test）
- SHALL NOT 在子代理的最终检查点任务中执行 git commit
- SHALL NOT 在不确定 KVS WebRTC C SDK API 用法时凭猜测编写代码
- SHALL NOT 通过 CredentialProvider（spec-7）中转凭证给 KVS WebRTC C SDK（SDK 原生 IoT 凭证提供者已内置凭证刷新）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token、SDP 完整内容、ICE Candidate 完整内容等敏感信息
- SHALL NOT 在任何日志级别输出证书路径和凭证信息（仅输出 thing_name、channel_name 等资源标识）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 编译 KVS WebRTC C SDK 时使用 `-DBUILD_DEPENDENCIES=ON` 保留自编译依赖
- SHALL NOT 使用 `#ifdef __linux__` 做 SDK 条件编译，必须使用 `#ifdef HAVE_KVS_WEBRTC_SDK` 自定义宏

## 任务

- [x] 1. WebRtcSignaling 模块实现与 CMake 配置
  - [x] 1.1 创建 `device/src/webrtc_signaling.h`
    - 定义 `WebRtcConfig` POD 结构体：`channel_name`（std::string）、`aws_region`（std::string）
    - 声明 `build_webrtc_config()`：从 TOML key-value map 构建 WebRtcConfig，缺失字段时返回 false 并填充 error_msg（包含缺失字段名）
    - 声明 `WebRtcSignaling` 类（pImpl 模式）：`create()`、`connect()`、`disconnect()`、`is_connected()`、`reconnect()`
    - 声明回调注册方法：`set_offer_callback(OfferCallback)`、`set_ice_candidate_callback(IceCandidateCallback)`
    - 声明消息发送方法：`send_answer()`、`send_ice_candidate()`
    - 禁用拷贝构造和拷贝赋值（`= delete`）
    - 包含 `credential_provider.h`（复用 AwsConfig、parse_toml_section）
    - _需求：1.1, 1.2, 1.3, 1.4, 2.1, 2.6, 5.1, 5.2, 6.1, 6.2, 7.1, 7.3_

  - [x] 1.2 创建 `device/src/webrtc_signaling.cpp`
    - 实现 `build_webrtc_config()`：检查 channel_name 和 aws_region 字段，缺失时返回 false + 错误信息含字段名（与 build_kvs_config / build_aws_config 同模式）
    - 条件编译使用 `#ifdef HAVE_KVS_WEBRTC_SDK`（非 `#ifdef __linux__`）：
      - `HAVE_KVS_WEBRTC_SDK` 定义时：Linux 真实实现，包含 SDK 头文件，实现 IoT 凭证提供者初始化（`createLwsIotCredentialProvider`）、SignalingClient 创建/连接（`createSignalingClientSync` + `signalingClientFetchSync` + `signalingClientConnectSync`）、状态回调、消息回调、消息发送（`signalingClientSendMessageSync`）
      - `HAVE_KVS_WEBRTC_SDK` 未定义时：macOS / Linux-无-SDK stub 实现，connect() 立即返回 true，is_connected() 返回 true
    - `send_answer()` / `send_ice_candidate()` 加消息长度检查（`MAX_SIGNALING_MESSAGE_LEN`），超长时返回 false 并记录错误
    - 回调函数（`on_signaling_state_changed`、`on_signaling_message_received`）不执行耗时操作，仅转发到注册的 C++ 回调
    - 未注册回调时收到消息，spdlog::warn 并丢弃
    - RAII：析构函数释放 SignalingClient（`freeSignalingClient`）和凭证提供者（`freeIotCredentialProvider`）
    - reconnect() 实现：释放旧客户端 → 创建新客户端重新连接，不实现自动重连逻辑
    - 日志仅输出 channel_name、thing_name、peer_id、消息类型、SDK 状态码，不输出证书路径和凭证内容
    - _需求：1.1, 1.2, 1.3, 2.1, 2.2, 2.3, 2.4, 2.5, 2.7, 3.1, 3.2, 3.3, 3.4, 3.5, 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7, 6.1, 6.2, 6.3, 6.4, 6.5, 7.1, 7.2, 7.3, 7.4, 7.5, 7.6_

  - [x] 1.3 修改 `device/CMakeLists.txt`
    - 添加 `webrtc_module` 静态库（`src/webrtc_signaling.cpp`），链接 `spdlog::spdlog`、`credential_module`
    - Linux 上查找 KVS WebRTC C SDK：支持 `-DKVS_WEBRTC_SDK_DIR` 自定义 SDK 路径，`find_library` 查找 `kvsWebrtcSignalingClient`，`find_path` 查找 SDK 头文件
    - SDK 找到时：`target_compile_definitions(webrtc_module PRIVATE HAVE_KVS_WEBRTC_SDK=1)`，链接 SDK 库和头文件
    - SDK 未找到时：输出 CMake WARNING "KVS WebRTC C SDK not found, using stub on Linux"，不定义 `HAVE_KVS_WEBRTC_SDK`（编译 stub 路径）
    - macOS 上：不查找 SDK，直接使用 stub，输出 STATUS "WebRTC module: using stub (non-Linux platform)"
    - 添加 `webrtc_test` 测试目标（`tests/webrtc_test.cpp`），链接 `webrtc_module`、`GTest::gtest_main`、`rapidcheck`、`rapidcheck_gtest`
    - `add_test(NAME webrtc_test COMMAND webrtc_test WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")`
    - 不修改现有 `pipeline_manager`、`log_module`、`credential_module`、`kvs_module`、`yolo_module` 及所有现有测试目标
    - _需求：10.1, 10.2, 10.5, 10.6_

- [x] 2. 检查点 — 编译通过与现有测试回归
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
  - 确认编译无错误
  - 执行 `ctest --test-dir device/build --output-on-failure` 确认现有测试（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、kvs_test）全部通过
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户

- [x] 3. 配置文件更新与测试
  - [x] 3.1 修改 `device/config/config.toml.example`
    - 在现有 `[kvs]` section 之后新增 `[webrtc]` section 示例，包含 `channel_name` 和 `aws_region` 字段及注释说明
    - _需求：1.5_

  - [x] 3.2 创建 `device/tests/webrtc_test.cpp` — 基础结构与 example-based 测试
    - 包含 `webrtc_signaling.h`、`gtest/gtest.h`、`rapidcheck.h`、`rapidcheck/gtest.h`
    - 辅助函数 `write_temp_toml()`：写入临时 TOML 文件并返回路径（复用 spec-7/spec-8 测试模式）
    - Example-based 测试：
      - `MissingSectionReturnsError`：空 map → build_webrtc_config 返回 false，错误信息包含 "channel_name" 和 "aws_region" — _需求：1.2, 9.2_
      - `StubCreateAndConnect`：WebRtcSignaling::create() 创建 stub 实例，connect() 返回 true，is_connected() 返回 true — _需求：2.3, 9.4_
      - `StubDisconnect`：disconnect() 将 is_connected() 设为 false — _需求：9.5_
      - `SendFailsWhenNotConnected`：未连接时 send_answer() 和 send_ice_candidate() 返回 false — _需求：6.3, 9.6_
      - `StubReconnect`：disconnect() 后 reconnect() 返回 true，is_connected() 恢复为 true — _需求：7.4_
      - `SendSucceedsWhenConnected`：连接后 send_answer() 和 send_ice_candidate() 返回 true（stub 场景） — _需求：6.1, 6.2_
    - _需求：9.1, 9.4, 9.5, 9.6, 9.7, 9.8, 9.9_

  - [x] 3.3 添加 PBT — Property 1: WebRTC 配置解析 round-trip
    - **Property 1: WebRTC 配置解析 round-trip**
    - 随机生成非空 ASCII 字符串 channel_name 和 aws_region → 写入 TOML `[webrtc]` section → parse_toml_section 解析 → build_webrtc_config 构建 → 验证字段值一致
    - **验证：需求 1.1, 9.1, 9.10**

  - [x] 3.4 添加 PBT — Property 2: WebRTC 配置缺失字段检测
    - **Property 2: WebRTC 配置缺失字段检测**
    - 随机移除 {channel_name, aws_region} 的非空真子集 → build_webrtc_config 返回 false → 错误信息包含所有被移除字段名
    - **验证：需求 1.3, 9.3**

- [x] 4. 检查点 — 测试全部通过
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
  - 执行 `ctest --test-dir device/build --output-on-failure` 确认所有测试通过（含新增 webrtc_test）
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户


- [x] 5. provision-device.sh 扩展
  - [x] 5.1 修改 `scripts/provision-device.sh`
    - 新增 `--signaling-channel-name` 可选参数（默认值：`${THING_NAME}Channel`），新增 `SIGNALING_CHANNEL_NAME` 全局变量
    - 新增 `create_signaling_channel()` 函数：幂等创建信令通道（`aws kinesisvideo create-signaling-channel --channel-name {name} --channel-type SINGLE_MASTER`），已存在则跳过并记录 "Signaling channel already exists"
    - 新增 `attach_webrtc_iam_policy()` 函数：幂等附加 inline policy `{PROJECT_NAME}WebRtcPolicy` 到 IAM Role，包含 `kinesisvideo:DescribeSignalingChannel`、`GetSignalingChannelEndpoint`、`GetIceServerConfig`、`ConnectAsMaster` 权限，限定到该信令通道 ARN
    - 修改 `generate_toml_config()`：在 `[kvs]` section 之后追加 `[webrtc]` section（channel_name + aws_region），幂等处理（awk 先删旧 [webrtc] 再追加新的）
    - 修改 `do_provision()`：在 `attach_kvs_iam_policy` 之后调用 `create_signaling_channel` + `attach_webrtc_iam_policy`
    - 修改 `print_summary()`：追加信令通道名称
    - 修改 `verify_resources()`：新增信令通道存在性 + WebRTC IAM inline policy 存在性检查
    - 修改 `cleanup_resources()`：新增删除 WebRTC IAM inline policy + 删除信令通道（在现有 KVS IAM policy 删除之后、证书 detach 之前执行）
    - 修改 `parse_args()`：处理 `--signaling-channel-name` 参数
    - 修改 `print_usage()`：添加 `--signaling-channel-name` 说明
    - _需求：8.1, 8.2, 8.3, 8.4, 8.5, 8.6, 8.7, 8.8_

- [x] 6. 最终检查点 — 全量验证
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过（现有 smoke_test、log_test、tee_test、camera_test、health_test、credential_test、kvs_test + 可选 yolo_test + 新增 webrtc_test）
  - 确认 ASan 无内存错误报告
  - 确认现有测试行为不变
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：10.1, 10.2, 10.3, 10.4, 10.5, 10.6_

## 备注

- 新建文件：`device/src/webrtc_signaling.h`、`device/src/webrtc_signaling.cpp`、`device/tests/webrtc_test.cpp`
- 修改文件：`device/CMakeLists.txt`、`device/config/config.toml.example`、`scripts/provision-device.sh`
- 不修改文件：所有现有测试文件、credential_provider.h/cpp（仅复用接口）、pipeline_builder.h/cpp（本 Spec 不修改管道）
- 条件编译使用 `HAVE_KVS_WEBRTC_SDK` 自定义宏（非 `__linux__`），CMake 在找到 SDK 时定义此宏
- CMake 支持 `-DKVS_WEBRTC_SDK_DIR=/path/to/sdk` 自定义 SDK 安装路径
- macOS 上全部使用 stub 实现，测试不依赖真实 KVS WebRTC SDK
- Linux 上 SDK 不可用时回退到 stub（CMake WARNING），不阻断编译
- 回调函数不执行耗时操作（SDK 信令线程同步调用），如需异步处理由调用方自行投递到工作线程
- 重连由外部管理（PipelineHealthMonitor 或 main 循环），WebRtcSignaling 不实现自动重连
- send_answer / send_ice_candidate 加消息长度检查（`MAX_SIGNALING_MESSAGE_LEN`）
- cleanup 顺序：WebRTC IAM policy 在 KVS IAM policy 之后、证书 detach 之前删除
- PBT 使用 RapidCheck，每个属性最少 100 次迭代
- 标记 `*` 的子任务为可选（PBT 属性测试），可跳过以加速 MVP
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
