# 需求文档：Spec 0 — GStreamer Capture

## 简介

本 Spec 是 device 模块的第一个 Spec，目标是从零搭建 CMake 项目骨架，实现 GStreamer 管道生命周期管理器 `PipelineManager`，并通过冒烟测试验证基本功能。

`PipelineManager` 是整个 device 模块的核心骨架类。它将 GStreamer 纯 C API 封装为 C++ RAII 类，负责管道的创建、启动、停止和资源释放。后续所有视频处理逻辑（H.264 编码、tee 分流、AI 推理管道、KVS/WebRTC sink）都将挂载在这个类上。

## 前置条件

- 无（这是第一个 Spec）

## 术语表

- **PipelineManager**：GStreamer 管道生命周期管理器，封装 `GstElement*` 管道指针，提供创建、启动、停止接口，遵循 RAII 语义
- **GStreamer**：开源多媒体框架，提供视频采集、编码、分流等管道式处理能力
- **gst_parse_launch**：GStreamer C API，接收管道描述字符串并创建对应的管道实例
- **RAII**：Resource Acquisition Is Initialization，C++ 资源管理模式，在构造时获取资源、析构时释放资源
- **fakesink**：GStreamer 测试用 sink 元素，丢弃所有输入数据，不产生实际输出，适合自动化测试
- **videotestsrc**：GStreamer 测试用 source 元素，生成测试视频信号，macOS 开发环境替代真实摄像头
- **GMainLoop**：GLib 主事件循环，用于处理 GStreamer bus 消息和信号
- **ASan**：AddressSanitizer，编译器内存错误检测工具，检测 heap-use-after-free、buffer-overflow 等问题
- **CMake**：跨平台构建系统，本项目最低要求 3.16
- **CTest**：CMake 内置测试运行器，统一管理和执行 Google Test 测试
- **pkg-config**：系统库依赖发现工具，用于查找 GStreamer 头文件和链接库路径
- **FetchContent**：CMake 模块，用于在配置阶段自动下载和集成外部依赖（如 Google Test）
- **gst_macos_main()**：GStreamer macOS 专用 API，在主线程创建 NSApplication 运行循环，解决 GStreamer-GL 在 macOS 上的线程限制

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（开发）/ Linux aarch64（Pi 5 生产） |
| 硬件限制 | Raspberry Pi 5，4GB RAM |
| GStreamer 依赖发现 | pkg-config（系统包） |
| Google Test 集成 | FetchContent |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| 单个冒烟测试耗时 | ≤ 5 秒 |
| 代码组织 | .h + .cpp 分离模式 |
| 测试 sink | fakesink（禁止使用 autovideosink） |

## 禁止项

### Design 层

- SHALL NOT 在 macOS 上直接在 main() 中运行含 autovideosink 的 GStreamer 管道
  - 原因：macOS 要求 NSApplication 在主线程运行，否则 GStreamer-GL 报警告且可能崩溃
  - 建议：用 `gst_macos_main()` 包装管道运行逻辑

### Tasks 层

- SHALL NOT 将 C++ 测试编译为独立可执行文件后逐个运行
  - 原因：容易遗漏测试、运行方式不统一、CI 集成困难
  - 建议：通过 GTest + CTest 统一管理，使用 `ctest --test-dir build --output-on-failure`

- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符
  - 原因：GLib 输出函数在部分终端环境下不正确处理 UTF-8
  - 建议：日志和用户提示信息统一使用英文

- SHALL NOT 将新建文件直接放到项目根目录
  - 原因：根目录应保持干净
  - 建议：源码放 `device/src/`，测试放 `device/tests/`


## 需求

### 需求 1：CMake 项目骨架

**用户故事：** 作为开发者，我需要一个配置完整的 CMake 构建系统，以便能够编译 C++17 代码、链接 GStreamer 库、运行测试，并在 Debug 模式下自动检测内存错误。

#### 验收标准

1. WHEN 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug` 时，THE CMake_Build_System SHALL 成功完成配置，在 `device/build/` 目录下生成 `compile_commands.json` 文件
2. WHEN `CMAKE_BUILD_TYPE` 设置为 `Debug` 时，THE CMake_Build_System SHALL 在编译和链接阶段添加 `-fsanitize=address -fno-omit-frame-pointer` 标志
3. WHEN 执行 `cmake --build device/build` 时，THE CMake_Build_System SHALL 通过 pkg-config 找到 GStreamer 依赖并成功编译所有源文件和测试文件
4. WHEN 执行 `cmake --build device/build` 时，THE CMake_Build_System SHALL 通过 FetchContent 自动下载 Google Test 并编译测试可执行文件
5. THE CMake_Build_System SHALL 将 C++ 标准设置为 C++17，并将 `CMAKE_EXPORT_COMPILE_COMMANDS` 设置为 `ON`

### 需求 2：PipelineManager 管道创建

**用户故事：** 作为开发者，我需要通过管道描述字符串创建 GStreamer 管道实例，以便后续对管道进行启动和停止操作。

#### 验收标准

1. WHEN 传入有效的管道描述字符串（如 `"videotestsrc ! fakesink"`）时，THE PipelineManager SHALL 调用 `gst_parse_launch` 创建管道实例，并初始化 GStreamer 运行时
2. WHEN 传入无效的管道描述字符串（如空字符串或包含不存在元素的字符串）时，THE PipelineManager SHALL 返回错误信息，且不持有任何 GStreamer 资源
3. THE PipelineManager SHALL 在头文件（.h）中声明接口，在源文件（.cpp）中实现逻辑

### 需求 3：PipelineManager 管道启动与停止

**用户故事：** 作为开发者，我需要控制管道的运行状态，以便在需要时启动视频处理流程并在结束时安全停止。

#### 验收标准

1. WHEN 调用启动接口时，THE PipelineManager SHALL 将管道状态设置为 `GST_STATE_PLAYING`
2. WHEN 调用停止接口时，THE PipelineManager SHALL 将管道状态设置为 `GST_STATE_NULL` 并释放所有 GStreamer 资源（管道指针通过 `gst_object_unref` 释放）
3. WHEN 管道尚未创建时调用启动接口，THE PipelineManager SHALL 返回错误，且不改变内部状态
4. WHEN 管道已停止后再次调用停止接口，THE PipelineManager SHALL 安全返回（幂等），不触发崩溃或 ASan 报告

### 需求 4：PipelineManager 资源安全

**用户故事：** 作为开发者，我需要 PipelineManager 自动管理 GStreamer 资源的生命周期，以防止内存泄漏和 double-free 问题。

#### 验收标准

1. THE PipelineManager SHALL 遵循 RAII 语义，在析构函数中自动调用停止逻辑，释放所有持有的 GStreamer 资源
2. THE PipelineManager SHALL 通过 `= delete` 禁止拷贝构造和拷贝赋值操作，防止多个实例持有同一管道指针导致 double-free
3. WHEN PipelineManager 实例离开作用域时，THE PipelineManager SHALL 确保所有 GStreamer 引用计数正确归零，ASan 运行时不报告任何内存错误

### 需求 5：main.cpp 应用入口

**用户故事：** 作为开发者，我需要一个可运行的应用入口程序，以便手动验证管道在实际运行环境中的表现。

#### 验收标准

1. THE Main_Entry SHALL 使用 PipelineManager 创建一个 `videotestsrc ! videoconvert ! autovideosink` 管道，并启动 GMainLoop 事件循环
2. WHEN 在 macOS 上运行时，THE Main_Entry SHALL 使用 `gst_macos_main()` 包装管道运行逻辑，确保 GStreamer-GL 在主线程正确运行
3. WHEN 在 Linux 上运行时，THE Main_Entry SHALL 直接运行管道逻辑，不使用 `gst_macos_main()` 包装
4. WHEN 收到 SIGINT 信号（Ctrl+C）时，THE Main_Entry SHALL 退出 GMainLoop 并触发 PipelineManager 的停止和资源释放流程
5. WHEN GStreamer bus 上出现 ERROR 或 EOS 消息时，THE Main_Entry SHALL 退出 GMainLoop 并输出包含 Element 名称和错误描述的英文日志信息

### 需求 6：冒烟测试

**用户故事：** 作为开发者，我需要自动化冒烟测试来验证 PipelineManager 的核心功能，以便在每次构建后快速确认基本功能正常。

#### 验收标准

1. WHEN 执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 运行所有冒烟测试并报告结果
2. THE Test_Suite SHALL 验证：传入 `"videotestsrc ! fakesink"` 时 PipelineManager 成功创建管道
3. THE Test_Suite SHALL 验证：创建管道后调用启动接口，管道进入 PLAYING 状态
4. THE Test_Suite SHALL 验证：调用停止接口后，管道进入 NULL 状态且资源被释放
5. THE Test_Suite SHALL 验证：PipelineManager 实例离开作用域后，ASan 不报告任何内存错误
6. THE Test_Suite SHALL 仅使用 `fakesink` 作为 sink 元素，不使用 `autovideosink` 或其他需要显示设备的 sink
7. WHEN 单个冒烟测试执行时，THE Test_Suite SHALL 在 5 秒内完成

## 验证命令

```bash
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```

预期结果：配置成功、编译无错误、所有测试通过、ASan 无报告。

## 明确不包含

- spdlog 结构化日志（Spec 1）
- 交叉编译工具链（Spec 2）
- H.264 编码和 tee 分流（Spec 3）
- 摄像头抽象层（Spec 4）
- 管道健康监控与自动恢复（Spec 5）
- vcpkg 包管理（后续 Spec）
