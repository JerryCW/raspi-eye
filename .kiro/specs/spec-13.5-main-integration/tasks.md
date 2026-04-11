# Implementation Plan: main-integration

## 概述

将 `main.cpp` 从胶水代码重构为三阶段生命周期管理架构。按依赖顺序实现：ShutdownHandler → AppContext → CMakeLists.txt 更新 → 编译检查点 → main.cpp 重构 → 测试 → 最终检查点。

## 禁止项

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在子代理的最终检查点任务中执行 git commit
- SHALL NOT 修改现有测试文件
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token、SDP 完整内容、ICE Candidate 完整内容等敏感信息
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 C API 外）

## Tasks

- [x] 1. 实现 ShutdownHandler 模块
  - [x] 1.1 创建 `device/src/shutdown_handler.h`
    - 定义 `ShutdownHandler` 类：pImpl 模式，`register_step(name, fn)`、`execute()` 接口
    - 删除拷贝构造和拷贝赋值
    - _Requirements: 4.1, 4.2, 4.7_
  - [x] 1.2 创建 `device/src/shutdown_handler.cpp`
    - `Impl` 内部用 `std::vector<std::pair<std::string, std::function<void()>>>` 存储步骤
    - `execute()` 从 vector 尾部向头部遍历（逆序执行）
    - 每步用 `std::async(std::launch::async, fn)` + `future.wait_for(5s)` 实现超时保护
    - 每步用 `try/catch(...)` 包裹，异常时记录 error 日志并继续
    - 总超时 30 秒：记录 `execute()` 开始时间，每步执行前检查是否已超总时限
    - 执行完毕后输出 shutdown summary：每步名称、结果（ok/timeout/exception）、耗时
    - 日志中不输出任何敏感信息，仅输出步骤名称和状态
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 7.4_

- [x] 2. 实现 AppContext 模块
  - [x] 2.1 创建 `device/src/app_context.h`
    - 定义 `AppContext` 类：pImpl 模式，`init(config_path, cam_config, &err)`、`start(&err)`、`stop()` 三阶段接口
    - 删除拷贝构造和拷贝赋值
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6_
  - [x] 2.2 创建 `device/src/app_context.cpp`
    - `Impl` 持有：`AwsConfig`、`KvsConfig`、`WebRtcConfig`、`CameraConfig`、`unique_ptr<WebRtcSignaling>`、`unique_ptr<WebRtcMediaManager>`、`unique_ptr<PipelineManager>`、`unique_ptr<PipelineHealthMonitor>`、`ShutdownHandler`
    - `init()`: 解析 config.toml 的 [aws]/[kvs]/[webrtc] section → 创建 Signaling → 创建 MediaManager → 注册回调 → 注册 shutdown step → 记录 info 日志（仅含 stream name 和 channel name）
    - `start()`: build_tee_pipeline（传入 kvs_config、aws_config、media_manager 指针）→ PipelineManager::create → start → 创建 HealthMonitor → 注册 rebuild/health 回调 → 注册 shutdown step → signaling->connect()（失败仅 warn 不阻断）→ 记录 info 日志
    - `stop()`: 委托 `ShutdownHandler::execute()`
    - rebuild 回调中传入完整的 KvsConfig、AwsConfig、WebRtcMediaManager 参数
    - 日志中不输出证书路径、密钥、token 等敏感信息
    - _Requirements: 1.1-1.6, 2.1-2.10, 3.1-3.9, 7.1-7.3, 8.1, 8.4, 10.1-10.4_

- [x] 3. 更新 CMakeLists.txt
  - 将 `shutdown_handler.cpp` 和 `app_context.cpp` 添加到 `pipeline_manager` 静态库的源文件列表
  - 确保 `raspi-eye` 可执行文件通过 `pipeline_manager` 库间接链接到新模块
  - 添加 `shutdown_test` 测试可执行文件，链接 `pipeline_manager`、`GTest::gtest_main`、`rapidcheck`、`rapidcheck_gtest`
  - 注册 `shutdown_test` 到 CTest
  - _Requirements: 1.1, 4.1_

- [x] 4. 编译检查点
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
  - 确保编译无错误无警告
  - 运行 `ctest --test-dir device/build --output-on-failure` 确保现有测试全部通过
  - 如有问题，询问用户

- [x] 5. 重构 main.cpp
  - [x] 5.1 重构 `device/src/main.cpp` 的 `run_pipeline` 函数
    - 保留：GStreamer 初始化、命令行解析（`--log-json`、`--camera`、`--device`、`--config`）、日志初始化、参数验证
    - 新增 `--config <path>` 参数解析，默认值 `device/config/config.toml`
    - 替换：直接模块创建 → `AppContext ctx; ctx.init(); ctx.start(); g_main_loop_run(); ctx.stop()`
    - 移除：直接持有的 PipelineManager、PipelineHealthMonitor、rebuild 回调、health 回调等
    - 保留：`GMainLoop*` 全局变量、SIGINT/SIGTERM 信号处理
    - init 失败 → error 日志 + 退出码 1；start 失败 → error 日志 + 退出码 1
    - _Requirements: 5.1-5.7, 6.1-6.4_

- [x] 6. 编写 ShutdownHandler 测试
  - [x] 6.1 创建 `device/tests/shutdown_test.cpp` 测试框架和 example-based 测试
    - 包含 GTest 和 RapidCheck 头文件
    - 测试 1：超时保护 — 注册一个 sleep(10s) 步骤 + 一个正常步骤，验证正常步骤仍被执行
    - 测试 2：空步骤列表 — execute() 不崩溃
    - 测试 3：拷贝禁用 — `static_assert(!std::is_copy_constructible_v<ShutdownHandler>)` 和 `static_assert(!std::is_copy_constructible_v<AppContext>)`
    - _Requirements: 4.3, 4.7, 1.6_
  - [x] 6.2 编写 Property 1 PBT：逆序执行不变量
    - **Property 1: 逆序执行不变量**
    - 生成随机步骤数量（1-20）和随机名称
    - 每步记录执行顺序到共享 vector
    - 验证执行顺序 == 注册顺序的 reverse
    - 最低 100 次迭代
    - **Validates: Requirements 4.2**
  - [x] 6.3 编写 Property 2 PBT：异常隔离不变量
    - **Property 2: 异常隔离不变量**
    - 生成随机步骤数量（2-10）和随机异常位置
    - 异常步骤抛 `std::runtime_error`
    - 非异常步骤记录执行到共享 vector
    - 验证所有非异常步骤都被执行，且顺序为注册逆序
    - 最低 100 次迭代
    - **Validates: Requirements 4.4**

- [x] 7. 最终检查点
  - 运行 `cmake --build device/build` 确保编译通过
  - 运行 `ctest --test-dir device/build --output-on-failure` 确保所有测试通过（含新增 shutdown_test）
  - 确保所有测试通过，如有问题询问用户

## Notes

- 任务标记 `*` 的为可选子任务，可跳过以加速 MVP
- 每个任务引用具体需求编号，确保可追溯
- 检查点确保增量验证
- PBT 验证 ShutdownHandler 的核心不变量（逆序执行、异常隔离）
- Example-based 测试验证边界情况（超时保护、空步骤、拷贝禁用）
