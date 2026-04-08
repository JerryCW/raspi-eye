# 实施计划：Spec 0 — GStreamer Capture

## 概述

从零搭建 device 模块的 CMake 项目骨架，实现 PipelineManager 类（GStreamer 管道生命周期管理器），提供 main.cpp 应用入口，并通过 fakesink 冒烟测试验证核心功能。按依赖顺序：CMake 骨架 → PipelineManager → main.cpp → 冒烟测试 → 检查点。

## 禁止项（Tasks 层）

- SHALL NOT 将 C++ 测试编译为独立可执行文件后逐个运行，必须通过 GTest + CTest 统一管理
- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符，日志统一使用英文
- SHALL NOT 将新建文件直接放到项目根目录，源码放 `device/src/`，测试放 `device/tests/`
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行

## 任务

- [x] 1. 搭建 CMake 项目骨架
  - [x] 1.1 创建 `device/CMakeLists.txt`
    - 设置 `cmake_minimum_required(VERSION 3.16)`、`project(raspi-eye LANGUAGES CXX)`
    - 设置 C++17 标准、`CMAKE_EXPORT_COMPILE_COMMANDS ON`
    - Debug 构建添加 ASan 标志：`-fsanitize=address -fno-omit-frame-pointer`
    - 通过 `pkg_check_modules(GST REQUIRED gstreamer-1.0)` 发现 GStreamer
    - 通过 `FetchContent` 集成 Google Test v1.14.0
    - 定义 `pipeline_manager` 静态库 target（src/pipeline_manager.cpp）
    - 定义 `raspi-eye` 可执行文件 target（src/main.cpp），链接 `pipeline_manager`
    - 定义 `smoke_test` 测试可执行文件 target（tests/smoke_test.cpp），链接 `pipeline_manager` 和 `GTest::gtest_main`
    - 调用 `enable_testing()` 和 `add_test()`
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5_
  - [x] 1.2 创建目录结构和占位文件
    - 创建 `device/src/` 和 `device/tests/` 目录
    - 创建 `device/src/pipeline_manager.h`、`device/src/pipeline_manager.cpp`、`device/src/main.cpp`、`device/tests/smoke_test.cpp` 的最小占位内容（确保 CMake 配置和编译通过）
    - _需求：2.3_

- [x] 2. 实现 PipelineManager
  - [x] 2.1 实现 `device/src/pipeline_manager.h` 接口声明
    - 声明 `PipelineManager` 类：工厂函数 `create()`、`start()`、`stop()`、`current_state()`、析构函数
    - 禁止拷贝（`= delete`），允许移动
    - 私有构造函数，持有 `GstElement* pipeline_` 成员
    - _需求：2.1, 2.2, 2.3, 4.2_
  - [x] 2.2 实现 `device/src/pipeline_manager.cpp`
    - `create()`：空字符串检查 → `gst_init_check()` 初始化（`static bool` 保证只初始化一次）→ `gst_parse_launch()` 创建管道 → 失败返回 nullptr + error_msg
    - `start()`：检查 `pipeline_` 非空 → `gst_element_set_state(PLAYING)` → 失败返回 false + error_msg
    - `stop()`：幂等，检查 `pipeline_` 非空 → `gst_element_set_state(NULL)` → `gst_object_unref()` → 置 nullptr
    - `current_state()`：调用 `gst_element_get_state()` 带超时查询
    - 析构函数调用 `stop()`
    - 移动构造和移动赋值：转移 `pipeline_` 所有权，源对象置 nullptr
    - GError 资源通过 `g_error_free()` 释放
    - _需求：2.1, 2.2, 3.1, 3.2, 3.3, 3.4, 4.1, 4.2, 4.3_

- [x] 3. 检查点 — 确认 PipelineManager 编译通过
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
  - 确认配置成功、编译无错误
  - 如有问题，询问用户

- [x] 4. 实现 main.cpp 应用入口
  - 使用 PipelineManager 创建 `videotestsrc ! videoconvert ! autovideosink` 管道
  - 创建 GMainLoop，注册 SIGINT handler（退出主循环）
  - 获取 bus，注册 `bus_callback` 处理 `GST_MESSAGE_ERROR` 和 `GST_MESSAGE_EOS`
  - 错误日志输出 Element 名称和错误描述（英文）
  - `#ifdef __APPLE__` 使用 `gst_macos_main()` 包装，Linux 直接调用 `run_pipeline()`
  - 所有 `g_printerr` 输出使用英文，禁止非 ASCII 字符
  - _需求：5.1, 5.2, 5.3, 5.4, 5.5_

- [x] 5. 实现冒烟测试
  - [x] 5.1 实现 `device/tests/smoke_test.cpp`
    - `CreateValidPipeline`：`create("videotestsrc ! fakesink")` 返回非 nullptr — _需求：2.1, 6.2_
    - `CreateInvalidPipeline`：`create("")` 返回 nullptr，error_msg 非空 — _需求：2.2, 6.2_
    - `CreateUnknownElement`：`create("nonexistent_element ! fakesink")` 返回 nullptr 或状态异常 — _需求：2.2_
    - `StartPipeline`：创建后 `start()`，`current_state()` 返回 `GST_STATE_PLAYING` — _需求：3.1, 6.3_
    - `StopPipeline`：启动后 `stop()`，验证资源释放 — _需求：3.2, 6.4_
    - `StopIdempotent`：`stop()` 两次，无崩溃无 ASan 报告 — _需求：3.4_
    - `RAIICleanup`：作用域内创建并启动管道，离开作用域后 ASan 无报告 — _需求：4.1, 4.3, 6.5_
    - `NoCopy`：`static_assert(!std::is_copy_constructible_v<PipelineManager>)` — _需求：4.2_
    - 所有测试仅使用 `fakesink`，不使用 `autovideosink` — _需求：6.6_
    - _需求：6.1, 6.7_

- [x] 6. 最终检查点 — 全量验证
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认：CMake 配置成功、编译无错误、所有冒烟测试通过、ASan 无报告
  - 如有问题，询问用户

## 备注

- 所有源码遵循 .h + .cpp 分离模式
- 日志输出统一使用英文
- 测试通过 GTest + CTest 统一管理，禁止独立运行
- Debug 构建自动开启 ASan，内存错误会导致测试失败
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
