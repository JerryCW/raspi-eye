# 实施计划：Spec 4 — 摄像头接口抽象层

## 概述

将 `build_tee_pipeline()` 中硬编码的 `videotestsrc` 解耦为可配置的摄像头抽象层 `CameraSource`。按依赖顺序：camera_source.h/cpp 新建 → pipeline_builder.h/cpp 签名扩展 → CMakeLists.txt 更新 → camera_test 测试 → main.cpp 命令行集成 → 双平台验证 → 最终检查点。实现语言为 C++17，不涉及 PBT。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 GStreamer C API 外）
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 修改现有 `smoke_test.cpp`、`log_test.cpp`

## 任务

- [x] 1. CameraSource 模块实现
  - [x] 1.1 创建 `device/src/camera_source.h`
    - 定义 `CameraType` 枚举：`TEST`、`V4L2`、`LIBCAMERA`
    - 定义 `CameraConfig` POD 结构体：`CameraType type = default_camera_type()`、`std::string device`
    - 声明 `default_camera_type()`、`camera_type_name(CameraType)`、`create_source(const CameraConfig&, std::string*)`、`parse_camera_type(const std::string&, CameraType&)`
    - 所有声明在 `namespace CameraSource` 内
    - _需求：1.1, 1.2, 1.3, 1.4, 2.1_
  - [x] 1.2 创建 `device/src/camera_source.cpp`
    - 实现 `default_camera_type()`：`#ifdef __APPLE__` 返回 `TEST`，否则返回 `V4L2`
    - 实现 `camera_type_name()`：switch 返回 `"videotestsrc"` / `"v4l2src"` / `"libcamerasrc"`
    - 实现 `parse_camera_type()`：将输入转小写后匹配 `"test"` / `"v4l2"` / `"libcamera"`，成功设置 `out_type` 返回 true，失败返回 false
    - 实现 `create_source()`：
      - 调用 `gst_element_factory_make(camera_type_name(config.type), "src")`
      - 失败时返回 nullptr，error_msg 输出 `"Failed to create camera source: {factory_name} (plugin not available)"`
      - V4L2 类型时设置 `device` 属性（空字符串回退到 `/dev/video0`）
      - 通过 spdlog `"pipeline"` logger 记录创建的视频源类型
    - _需求：1.3, 1.4, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 4.1, 4.2, 4.3_

- [x] 2. PipelineBuilder 签名扩展
  - [x] 2.1 修改 `device/src/pipeline_builder.h`
    - 添加 `#include "camera_source.h"`
    - 修改 `build_tee_pipeline` 签名为 `GstElement* build_tee_pipeline(std::string* error_msg = nullptr, CameraSource::CameraConfig config = CameraSource::CameraConfig{})`
    - `error_msg` 在前保持与现有 `build_tee_pipeline(&err)` 调用兼容
    - _需求：3.1, 3.2_
  - [x] 2.2 修改 `device/src/pipeline_builder.cpp`
    - 添加 `#include "camera_source.h"`
    - 修改 `build_tee_pipeline` 函数签名匹配头文件
    - 将硬编码的 `gst_element_factory_make("videotestsrc", "src")` 替换为 `CameraSource::create_source(config, error_msg)`
    - `create_source` 返回 nullptr 时清理已创建资源并返回 nullptr
    - 管道拓扑结构（双 tee 三路分流）保持不变
    - _需求：3.1, 3.2, 3.3, 3.4, 3.5_

- [x] 3. CMakeLists.txt 更新与编译验证
  - [x] 3.1 更新 `device/CMakeLists.txt`
    - 修改 `pipeline_manager` 静态库源文件列表，添加 `src/camera_source.cpp`
    - 新增 `camera_test` 测试目标：`add_executable(camera_test tests/camera_test.cpp)` + `target_link_libraries(camera_test PRIVATE pipeline_manager GTest::gtest)` + `add_test(NAME camera_test COMMAND camera_test)`
    - 注意：`camera_test` 链接 `GTest::gtest`（非 `gtest_main`），因为需要自定义 `main()` 调用 `gst_init`
    - 不修改现有 `smoke_test`、`log_test`、`tee_test` 的定义
    - _需求：6.1_
  - [x] 3.2 macOS Debug 编译验证
    - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
    - 确认编译无错误（ASan 相关警告除外）
    - _需求：4.4, 6.1_

- [x] 4. camera_test 测试
  - [x] 4.1 创建 `device/tests/camera_test.cpp` — 约 10 个测试用例
    - 自定义 `main()`：调用 `gst_init` 后运行 `RUN_ALL_TESTS()`
    - `DefaultCameraType`：`default_camera_type()` 在 macOS 返回 `TEST`，在 Linux 返回 `V4L2`（通过 `#ifdef __APPLE__` 条件断言）— _需求：1.3, 6.3_
    - `CameraTypeName`：`camera_type_name()` 对三种类型分别返回 `"videotestsrc"`、`"v4l2src"`、`"libcamerasrc"` — _需求：1.4_
    - `ParseCameraTypeValid`：`parse_camera_type("test"/"v4l2"/"libcamera")` 返回 true 并设置正确的 `CameraType` — _需求：5.1_
    - `ParseCameraTypeCaseInsensitive`：`parse_camera_type("TEST"/"V4L2"/"LibCamera")` 大小写不敏感解析成功 — _需求：5.1_
    - `ParseCameraTypeInvalid`：`parse_camera_type("usb"/"")` 返回 false — _需求：5.4_
    - `CreateSourceTest`：`create_source({TEST})` 成功创建元素，元素名为 `"src"`，用完后 `gst_object_unref` — _需求：2.1, 2.2, 6.4_
    - `CreateSourceUnavailable`：macOS 上 `create_source({V4L2})` 和 `create_source({LIBCAMERA})` 返回 nullptr 并输出错误信息（`#ifdef __APPLE__` 条件编译仅在 macOS 执行）— _需求：2.5, 6.5_
    - `TeePipelineDefaultConfig`：默认 `CameraConfig` 调用 `build_tee_pipeline()` → `PipelineManager::create` → `start()` → `current_state()` 返回 `PLAYING` — _需求：3.1, 3.2, 6.6_
    - `TeePipelineExplicitTest`：显式 `CameraType::TEST` 配置调用 `build_tee_pipeline(&err, config)` → 管道达到 `PLAYING` — _需求：3.3, 6.7_
    - `TeePipelineSourceElement`：传入 TEST 配置后管道中 `gst_bin_get_by_name(..."src")` 返回非 nullptr，获取后 `gst_object_unref` — _需求：3.3, 3.5_
    - 所有测试仅使用 `fakesink`，每个测试 ≤ 5 秒
    - _需求：6.1, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8, 6.9_
  - [x] 4.2 运行全量测试
    - 执行 `ctest --test-dir device/build --output-on-failure`
    - 确认约 33 个测试全部通过（8 smoke + 7 log + 8 tee + ~10 camera）
    - 确认 ASan 无内存错误报告
    - _需求：6.1, 6.2_

- [x] 5. 检查点 — 确认现有测试回归通过
  - 确认 smoke_test（8 个）、log_test（7 个）、tee_test（8 个）全部通过，行为不变
  - 确认 camera_test（~10 个）全部通过
  - 如有问题，询问用户

- [x] 6. main.cpp 命令行集成
  - [x] 6.1 修改 `device/src/main.cpp`
    - 添加 `#include "camera_source.h"`
    - 在 `run_pipeline()` 中扩展命令行解析循环，新增 `--camera` 和 `--device` 参数处理：
      - `--camera <type>`：调用 `parse_camera_type()` 解析，无效值时 spdlog error + `return 1`
      - `--device <path>`：设置 `cam_config.device`
    - `--device` 在非 v4l2 类型时通过 spdlog warn 提示被忽略
    - 启动时通过 spdlog 记录摄像头类型（如 `"Starting with camera: v4l2src (device=/dev/video0)"`）
    - 将 `cam_config` 传入 `build_tee_pipeline(&err_msg, cam_config)`
    - 其余逻辑（GMainLoop、bus_callback、SIGINT handler、`gst_macos_main` 包装）不变
    - _需求：5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7_
  - [x] 6.2 编译验证
    - 执行 `cmake --build device/build`
    - 确认编译无错误
    - 执行 `ctest --test-dir device/build --output-on-failure` 确认所有测试通过
    - _需求：4.4, 6.1_

- [x] 7. 最终检查点 — 全量验证
  - 确认以下全部通过：
    - `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` — macOS 编译测试通过、ASan 无报告
    - 约 33 个测试全部通过（8 smoke + 7 log + 8 tee + ~10 camera）
  - Pi 5 Release 验证（`scripts/pi-build.sh` 或手动 SSH）需要 Pi 5 可达，如不可达则标注跳过
  - [x] 7.1 Pi 5 手动运行验证（可选，Pi 5 不可达则跳过）
    - 在 Pi 5 上运行 `./raspi-eye --camera v4l2` 约 30 秒
    - 用 `top -b -n 5 -d 2` 记录 CPU 基线
    - 结果记录到 `docs/development-trace.md`
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：4.4, 4.5, 6.1, 6.2_

## 备注

- 本 Spec 不涉及 PBT，所有测试为 example-based 单元测试 + ASan 运行时检查
- 新建文件：`device/src/camera_source.h`、`device/src/camera_source.cpp`、`device/tests/camera_test.cpp`
- 修改文件：`device/src/pipeline_builder.h`、`device/src/pipeline_builder.cpp`、`device/src/main.cpp`、`device/CMakeLists.txt`
- 不修改文件：`device/tests/smoke_test.cpp`、`device/tests/log_test.cpp`、`device/tests/tee_test.cpp`
- `tee_test.cpp` 中 `build_tee_pipeline(&err)` 调用兼容新签名（`&err` 匹配第一个参数 `error_msg`，第二个参数 `config` 使用默认值）
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
- Pi 5 远程验证需要 Pi 5 可达（通过 SSH），不可达时相关验证标注跳过
