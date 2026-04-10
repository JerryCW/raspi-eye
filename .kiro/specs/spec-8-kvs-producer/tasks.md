# 实施计划：Spec 8 — KVS Producer 集成

## 概述

将 H.264 tee 管道中 KVS 分支的 `fakesink("kvs-sink")` 替换为 KVS Producer SDK 提供的 `kvssink` GStreamer element。核心新增模块 KvsSinkFactory 封装 kvssink 的创建、属性配置和平台隔离逻辑。按依赖顺序：kvs_sink_factory.h/cpp → CMakeLists.txt 更新 → pipeline_builder 签名扩展 → config.toml 示例更新 → kvs_test.cpp 测试（3 个 PBT + 6 个 example-based）→ provision-device.sh 扩展 → 双平台验证。实现语言为 C++17。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 GStreamer / KVS C API 外）
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、yolo_test）
- SHALL NOT 在子代理的最终检查点任务中执行 git commit
- SHALL NOT 在 CMakeLists.txt 中链接 KVS Producer SDK（kvssink 通过 GStreamer 插件机制运行时加载）
- SHALL NOT 通过 CredentialProvider 中转凭证给 kvssink（kvssink 原生 iot-certificate 已内置凭证刷新）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
- SHALL NOT 在任何日志级别（包括 debug/trace）输出 iot-certificate 完整字符串
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件

## 任务

- [x] 1. KvsSinkFactory 模块实现与 CMake 配置
  - [x] 1.1 创建 `device/src/kvs_sink_factory.h`
    - 定义 `KvsConfig` POD 结构体：`stream_name`（std::string）、`aws_region`（std::string）
    - 声明 `build_kvs_config()`：从 TOML key-value map 构建 KvsConfig，缺失字段时返回 false 并填充 error_msg（包含缺失字段名）
    - 声明 `build_iot_certificate_string()`：从 AwsConfig 构建 iot-certificate 属性字符串
    - 声明 `create_kvs_sink()`：平台条件编译创建 kvssink（Linux）或 fakesink stub（macOS），设置属性
    - 包含 `credential_provider.h`（复用 AwsConfig、parse_toml_section）
    - _需求：1.1, 1.2, 1.3, 1.4, 2.1, 2.2, 2.3, 3.1, 3.2, 3.3, 3.5_

  - [x] 1.2 创建 `device/src/kvs_sink_factory.cpp`
    - 实现 `build_kvs_config()`：检查 stream_name 和 aws_region 字段，缺失时返回 false + 错误信息含字段名（与 build_aws_config 同模式）
    - 实现 `build_iot_certificate_string()`：拼接 `"iot-certificate,endpoint=...,cert-path=...,key-path=...,ca-path=...,role-aliases=..."` 格式字符串
    - 实现 `create_kvs_sink()`：
      - `#ifdef __linux__`：尝试 `gst_element_factory_make("kvssink", "kvs-sink")`，不可用时回退 fakesink 并 spdlog::warn("kvssink not available, falling back to fakesink")
      - `#else`（macOS）：直接创建 fakesink stub 并 spdlog::info("macOS: using fakesink stub for kvs-sink")
      - 仅 kvssink 时设置 stream-name、aws-region、iot-certificate、restart-on-error=false 属性；fakesink 跳过属性设置
      - 日志仅输出 stream-name 和 aws-region，不输出证书路径、凭证内容、iot-certificate 完整字符串（任何日志级别）
    - _需求：1.1, 1.2, 1.3, 2.1, 2.2, 2.3, 2.4, 2.5, 3.1, 3.2, 3.3, 3.4, 3.5_

  - [x] 1.3 修改 `device/CMakeLists.txt`
    - 添加 `kvs_module` 静态库（`src/kvs_sink_factory.cpp`），链接 `${GST_LIBRARIES}`、`spdlog::spdlog`、`credential_module`
    - 为 `pipeline_manager` 添加 `kvs_module` 链接依赖
    - 添加 `kvs_test` 测试目标（`tests/kvs_test.cpp`），链接 `kvs_module`、`pipeline_manager`、`GTest::gtest_main`、`rapidcheck`、`rapidcheck_gtest`
    - `add_test(NAME kvs_test COMMAND kvs_test WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")`
    - 不链接 KVS Producer SDK（编译时零依赖）
    - 不修改现有 `pipeline_manager`、`log_module`、`credential_module`、`yolo_module` 及所有现有测试目标
    - _需求：7.1, 7.2, 7.5_

- [x] 2. PipelineBuilder 签名扩展与配置文件更新
  - [x] 2.1 修改 `device/src/pipeline_builder.h`
    - 添加 `#include "kvs_sink_factory.h"`
    - 扩展 `build_tee_pipeline()` 签名：新增 `const KvsSinkFactory::KvsConfig* kvs_config = nullptr` 和 `const AwsConfig* aws_config = nullptr` 两个可选参数
    - 默认值为 nullptr，保持向后兼容（不传参时行为与当前一致，使用 fakesink）
    - _需求：4.1, 4.4, 4.5_

  - [x] 2.2 修改 `device/src/pipeline_builder.cpp`
    - 添加 `#include "kvs_sink_factory.h"`
    - 将 KVS sink 创建从 `gst_element_factory_make("fakesink", "kvs-sink")` 改为条件调用：
      - 当 `kvs_config && aws_config` 非空时，调用 `KvsSinkFactory::create_kvs_sink(*kvs_config, *aws_config, error_msg)`
      - 否则保持原有 `gst_element_factory_make("fakesink", "kvs-sink")`
    - 其余管道拓扑代码不变（raw-tee、encoded-tee、AI 分支、WebRTC 分支均不受影响）
    - _需求：4.1, 4.2, 4.3, 4.4, 4.5_

  - [x] 2.3 修改 `device/config/config.toml.example`
    - 在现有 `[aws]` section 之后新增 `[kvs]` section 示例，包含 `stream_name` 和 `aws_region` 字段及注释说明
    - _需求：1.5_

- [x] 3. 检查点 — 编译通过与现有测试回归
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
  - 确认编译无错误
  - 执行 `ctest --test-dir device/build --output-on-failure` 确认现有测试（smoke_test、log_test、tee_test、camera_test、health_test、credential_test）全部通过
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户

- [x] 4. kvs_test.cpp — PBT 与 Example-Based 测试
  - [x] 4.1 创建 `device/tests/kvs_test.cpp` — 基础结构与 example-based 测试
    - 包含 `kvs_sink_factory.h`、`pipeline_builder.h`、`gtest/gtest.h`、`rapidcheck.h`、`rapidcheck/gtest.h`
    - 辅助函数 `write_temp_toml()`：写入临时 TOML 文件并返回路径
    - Example-based 测试：
      - `MissingSectionReturnsError`：空 map → build_kvs_config 返回 false，错误信息包含 "stream_name" 和 "aws_region" — _需求：1.2, 5.2_
      - `MacOsCreatesFakesink`：create_kvs_sink 在当前平台（macOS 测试环境）返回 fakesink，element 名称为 "kvs-sink" — _需求：2.2, 5.4_
      - `BackwardCompatibleWithoutKvsConfig`：不传 KvsConfig 时 build_tee_pipeline 仍成功，管道中 kvs-sink 为 fakesink — _需求：4.4, 5.5_
      - `PipelineBuildsWithKvsConfig`：传入 KvsConfig + AwsConfig 时管道正常构建（macOS stub 场景） — _需求：4.1, 4.2, 5.6_
      - `FakesinkSkipsIotCertificate`：fakesink stub 场景下不设置 iot-certificate 属性，无崩溃 — _需求：3.5_
      - `PipelineTopologyUnchanged`：传入 KvsConfig 后管道仍包含 raw-tee、encoded-tee、ai-sink、webrtc-sink — _需求：4.5_
    - _需求：5.1, 5.4, 5.5, 5.6, 5.7, 5.8, 5.9_

  - [x] 4.2 添加 PBT — Property 1: KVS 配置解析 round-trip
    - **Property 1: KVS 配置解析 round-trip**
    - 随机生成非空 ASCII 字符串 stream_name 和 aws_region → 写入 TOML `[kvs]` section → parse_toml_section 解析 → build_kvs_config 构建 → 验证字段值一致
    - **验证：需求 1.1, 5.1, 5.10**

  - [x] 4.3 添加 PBT — Property 2: KVS 配置缺失字段检测
    - **Property 2: KVS 配置缺失字段检测**
    - 随机移除 {stream_name, aws_region} 的非空真子集 → build_kvs_config 返回 false → 错误信息包含所有被移除字段名
    - **验证：需求 1.3, 5.3**

  - [x] 4.4 添加 PBT — Property 3: iot-certificate 字符串包含所有字段
    - **Property 3: iot-certificate 字符串包含所有字段**
    - 随机生成有效 AwsConfig（5 个字段均为非空字符串）→ build_iot_certificate_string → 验证以 "iot-certificate," 开头，且包含 endpoint=、cert-path=、key-path=、ca-path=、role-aliases= 各字段值子串
    - **验证：需求 3.3**

- [x] 5. provision-device.sh 扩展
  - [x] 5.1 修改 `scripts/provision-device.sh`
    - 新增 `--kvs-stream-name` 可选参数（默认值：`${THING_NAME}Stream`），新增 `KVS_STREAM_NAME` 全局变量
    - 新增 `create_kvs_stream()` 函数：幂等创建 KVS Stream（`aws kinesisvideo create-stream --data-retention-in-hours 2`），已存在则跳过
    - 新增 `attach_kvs_iam_policy()` 函数：幂等附加 inline policy `{PROJECT_NAME}KvsPolicy` 到 IAM Role，包含 kinesisvideo:PutMedia/GetDataEndpoint/DescribeStream 权限，限定到该 Stream ARN
    - 修改 `generate_toml_config()`：在 `[aws]` section 之后追加 `[kvs]` section（stream_name + aws_region），幂等处理（awk 先删旧 [kvs] 再追加新的）
    - 修改 `do_provision()`：在 create_role_alias 之后调用 create_kvs_stream + attach_kvs_iam_policy
    - 修改 `print_summary()`：追加 KVS Stream 名称
    - 修改 `verify_resources()`：新增 KVS Stream 存在性 + IAM inline policy 存在性检查
    - 修改 `cleanup_resources()`：新增删除 IAM inline policy + 删除 KVS Stream（在现有删除步骤之前）
    - 修改 `parse_args()`：处理 `--kvs-stream-name` 参数
    - 修改 `print_usage()`：添加 `--kvs-stream-name` 说明
    - _需求：6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8_

- [x] 6. 最终检查点 — 全量验证
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过（现有 smoke_test、log_test、tee_test、camera_test、health_test、credential_test + 可选 yolo_test + 新增 kvs_test）
  - 确认 ASan 无内存错误报告
  - 确认现有测试行为不变
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：7.1, 7.2, 7.3, 7.4, 7.5_

## 备注

- 新建文件：`device/src/kvs_sink_factory.h`、`device/src/kvs_sink_factory.cpp`、`device/tests/kvs_test.cpp`
- 修改文件：`device/CMakeLists.txt`、`device/src/pipeline_builder.h`、`device/src/pipeline_builder.cpp`、`device/config/config.toml.example`、`scripts/provision-device.sh`、`scripts/pi-build.sh`、`docs/pi-setup.md`、`.gitignore`
- 不修改文件：所有现有测试文件、credential_provider.h/cpp（仅复用接口）
- kvssink 通过 GStreamer 插件机制运行时加载，CMakeLists.txt 不链接 KVS Producer SDK
- 运行时插件统一存放在 `device/plugins/` 目录（.gitignore 排除），`pi-build.sh` 自动检测并设置 `GST_PLUGIN_PATH`
- Pi 5 上编译 KVS Producer SDK 后，需手动复制 `libgstkvssink.so` 到 `device/plugins/`（参考 docs/pi-setup.md）
- 后续 WebRTC 插件也放入 `device/plugins/`，同一套机制
- 所有测试使用 fakesink（macOS 环境），不需要真实 KVS 环境
- PBT 使用 RapidCheck，每个属性最少 100 次迭代
- 标记 `*` 的子任务为可选（PBT 属性测试），可跳过以加速 MVP
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
