# Implementation Plan: Spec 4.5 — Camera Source V2

## Overview

增强 `CameraSource::create_source()` 返回 GstBin + ghost pad，新增 V4L2 格式探测逻辑，main.cpp 强制 V4L2 类型提供 `--device`。接口不变，现有测试零修改通过。

## Tasks

- [x] 1. Refactor camera_source.cpp: Source Bin + ghost pad
  - [x] 1.1 Add V4L2Format enum and helper functions (probe_v4l2_formats, select_best_format)
    - 在 camera_source.cpp 匿名 namespace 中新增 `V4L2Format` 枚举（I420, YUYV, MJPG, UNKNOWN）
    - 实现 `probe_v4l2_formats(device_path, error_msg)`: 创建临时 v4l2src → READY → caps query → 解析格式 → 清理
    - 实现 `select_best_format(formats)`: 按 I420 > YUYV > MJPG 优先级选择
    - Caps 解析: `video/x-raw,format=I420` → I420, `video/x-raw,format=YUY2` → YUYV, `image/jpeg` → MJPG
    - 所有临时 GstElement/GstCaps 引用必须配对释放
    - _Requirements: 1.1, 1.2, 1.5, 1.6_

  - [x] 1.2 Implement create_single_element_bin helper
    - 通用辅助函数: 创建 GstBin("src") + 单个内部元素 + ghost pad("src")
    - 用于 TEST(videotestsrc)、LIBCAMERA(libcamerasrc)、V4L2 raw 格式(v4l2src) 三种场景
    - Bin 命名为 `"src"` 确保 `gst_bin_get_by_name` 兼容
    - Ghost pad 命名为 `"src"` 确保 `gst_element_link_many` 兼容
    - 失败时清理所有资源并返回 nullptr
    - _Requirements: 2.1, 2.2, 2.5, 2.6, 2.7_

  - [x] 1.3 Implement create_mjpg_bin for MJPG devices
    - 构建 GstBin("src") 内含 v4l2src("v4l2-source") → capsfilter("mjpg-caps", image/jpeg 1920x1080@15fps) → jpegdec("jpeg-decoder")
    - Ghost pad 连接到 jpegdec 的 src pad
    - 设置 v4l2src device 属性
    - capsfilter caps: `image/jpeg, width=1920, height=1080, framerate=15/1`
    - 元素创建失败、链接失败、ghost pad 创建失败时清理并返回 nullptr + error_msg
    - _Requirements: 1.3, 2.1, 2.2, 2.3_

  - [x] 1.4 Rewrite create_source to dispatch by CameraType and format
    - TEST → `create_single_element_bin("videotestsrc", "test-source", error_msg)`
    - V4L2 → `probe_v4l2_formats` + `select_best_format` → MJPG: `create_mjpg_bin` / raw: `create_single_element_bin("v4l2src", "v4l2-source", ...)` + 设置 device 属性
    - LIBCAMERA → `create_single_element_bin("libcamerasrc", "libcam-source", error_msg)`
    - 移除旧的 `/dev/video0` 默认回退逻辑
    - 通过 spdlog 记录检测到的格式列表和最终选择
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 2.1, 2.4, 2.5, 2.6, 4.1, 4.2, 4.3_

- [x] 2. Update main.cpp: enforce --device for V4L2
  - 在 Phase 3 (Validate parsed values) 中新增检查: V4L2 类型且 `!has_device` 时 `logger->error(...)` + `return 1`
  - 错误信息: `"V4L2 camera requires --device (e.g. --device /dev/IMX678)"`
  - 移除 V4L2 启动日志中的 `/dev/video0` 默认回退显示
  - _Requirements: 3.3, 3.1, 3.2_

- [x] 3. Checkpoint - Build and regression test
  - `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
  - `ctest --test-dir device/build --output-on-failure`
  - 确保编译零错误、所有现有测试通过（camera_test、tee_test、smoke_test 等）、ASan 无报告
  - Ensure all tests pass, ask the user if questions arise.
  - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.6_

- [x] 4. Final checkpoint - Full verification
  - 再次运行 `cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认最终状态
  - 提醒用户在 Pi 5 上执行端到端验证: `./device/build/raspi-eye --camera v4l2 --device /dev/IMX678 --config device/config/config.toml`
  - 提醒用户验证 V4L2 无 --device 报错: `./device/build/raspi-eye --camera v4l2 --config device/config/config.toml` (预期 error + exit 1)
  - Ensure all tests pass, ask the user if questions arise.
  - _Requirements: 5.4, 5.5_

## Notes

- camera_source.h 接口不变（GstBin 继承自 GstElement，返回类型 GstElement* 兼容）
- pipeline_builder.h/cpp 不修改（gst_element_link_many 自动适配 GstBin ghost pad）
- 不修改任何现有测试文件
- Pi 5 端到端验证需用户在真实硬件上手动执行
- 所有测试必须通过 `ctest --test-dir device/build --output-on-failure` 运行
