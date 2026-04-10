# 实施计划：Spec 9 — YOLO 设备端目标检测（独立模块）

## 概述

为 raspi-eye 项目引入独立的 YOLO 目标检测模块。核心类 `YoloDetector` 封装 ONNX Runtime C API，对输入 RGB 像素数据执行 YOLOv11s 推理，输出检测框列表。按依赖顺序：FindOnnxRuntime.cmake → yolo_detector.h → yolo_detector.cpp（独立函数优先）→ CMakeLists.txt 更新 → yolo_test.cpp 测试 → .gitignore + download-model.sh → 双平台验证。实现语言为 C++17，包含 4 个 PBT 属性 + 12 个 example-based 测试 + 性能基线测试。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（ONNX Runtime C API 资源通过 RAII unique_ptr 封装释放）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 修改现有测试文件（smoke_test.cpp、log_test.cpp、tee_test.cpp、camera_test.cpp、health_test.cpp）
- SHALL NOT 在不确定 ONNX Runtime API 用法时凭猜测编写代码，必须先查阅官方文档或头文件确认
- SHALL NOT 将模型文件（.onnx）提交到 git 仓库
- SHALL NOT 将 ONNX Runtime 通过 FetchContent 源码编译引入
- SHALL NOT 依赖 OpenCV（letterbox resize 手写实现）

## 任务

- [x] 1. CMake 基础设施与项目结构
  - [x] 1.1 创建 `device/cmake/FindOnnxRuntime.cmake`
    - 使用 `find_path` 查找 `onnxruntime_c_api.h` 头文件
    - 使用 `find_library` 查找 `onnxruntime` 库
    - 设置 `OnnxRuntime_INCLUDE_DIRS` 和 `OnnxRuntime_LIBRARIES` 变量
    - 使用 `find_package_handle_standard_args` 处理 REQUIRED/QUIET
    - 支持 macOS（Homebrew）和 Linux aarch64（自定义安装路径通过 `CMAKE_PREFIX_PATH` 指定）
    - _需求：1.1, 1.2, 1.3_

  - [x] 1.2 修改 `device/CMakeLists.txt`
    - 添加 `option(ENABLE_YOLO "Build YOLO detector module (requires ONNX Runtime)" ON)`
    - 在 `if(ENABLE_YOLO)` 块中：
      - `list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")`
      - `find_package(OnnxRuntime REQUIRED)`
      - 定义 `YOLO_MODEL_PATH` 变量指向 `${CMAKE_CURRENT_SOURCE_DIR}/models/yolov11s.onnx`
      - 定义 `YOLO_MODEL_PATH_NANO` 变量指向 `${CMAKE_CURRENT_SOURCE_DIR}/models/yolov11n.onnx`
      - 添加 `yolo_module` 静态库（`src/yolo_detector.cpp`），链接 `${OnnxRuntime_LIBRARIES}` 和 `log_module`
      - 添加 `yolo_test` 测试目标，链接 `yolo_module`、`GTest::gtest_main`、`rapidcheck`、`rapidcheck_gtest`
      - 通过 `target_compile_definitions` 传递 `YOLO_MODEL_PATH` 和 `YOLO_MODEL_PATH_NANO`
      - `add_test(NAME yolo_test COMMAND yolo_test)`
    - 不修改现有 `pipeline_manager`、`log_module` 及所有现有测试目标
    - _需求：1.1, 1.4, 1.5, 8.3, 8.4_

  - [x] 1.3 修改 `.gitignore`
    - 添加 `*.onnx` 排除模型文件
    - 添加 `device/models/` 排除模型目录
    - _需求：约束条件（模型文件不提交 git）_

  - [x] 1.4 创建 `scripts/download-model.sh`
    - 脚本检查模型文件是否已存在，存在则跳过
    - 创建 `device/models/` 目录
    - 下载两个模型：`yolov11s.onnx`（small，主力）和 `yolov11n.onnx`（nano，对比基线）
    - 优先使用 `yolo` CLI 导出（`yolo export model=yolo11s.pt format=onnx imgsz=640`，同理 yolo11n）
    - 回退使用 `curl` 从 GitHub Releases 下载预导出的 ONNX 文件
    - 下载后验证文件非空
    - 添加可执行权限（`chmod +x`）
    - _需求：约束条件（模型文件通过脚本下载）_

- [x] 2. YoloDetector 头文件与独立函数实现
  - [x] 2.1 创建 `device/src/yolo_detector.h`
    - 前向声明 ONNX Runtime C 类型（`OrtEnv`、`OrtSession`、`OrtMemoryInfo`）
    - 定义 `Detection` POD 结构体：`float x, y, w, h`（归一化 [0,1]）、`int class_id`、`float confidence`
    - 定义 `DetectorConfig` POD 结构体：`confidence_threshold`（默认 0.25f）、`iou_threshold`（默认 0.45f）、`num_threads`（默认 2）
    - 定义 `InferenceStats` POD 结构体：`preprocess_ms`、`inference_ms`、`postprocess_ms`、`total_ms`
    - 定义 `LetterboxInfo` POD 结构体：`scale`、`pad_x`、`pad_y`、`new_w`、`new_h`
    - 声明独立函数：`nms()`、`compute_iou()`、`letterbox_resize()`、`restore_coordinates()`
    - 声明 `YoloDetector` 类：
      - 工厂方法 `create(model_path, config, error_msg)`
      - 析构函数、移动语义、拷贝 `= delete`
      - `detect()` 和 `detect_with_stats()` 方法
      - 私有：RAII deleter 结构体、unique_ptr 成员、config、缓存的输入/输出名称和形状、可复用 input_buffer
    - _需求：2.1, 2.3, 2.4, 2.5, 3.1, 4.1, 4.4, 5.1, 5.2, 6.1_

  - [x] 2.2 创建 `device/src/yolo_detector.cpp` — 独立函数部分
    - 实现 `compute_iou()`：center (x,y,w,h) → corner (x1,y1,x2,y2) → 交集/并集
    - 实现 `nms()`：按 class_id 分组 → 每组内按 confidence 降序稳定排序 → 贪心选择（IoU > 阈值则抑制）→ 合并所有类别结果按 confidence 降序排序
    - 实现 `letterbox_resize()`：
      - 计算 scale = min(640.0/w, 640.0/h)、new_w、new_h、pad_x、pad_y
      - 分配 3×640×640 float buffer，填充灰色背景（128/255）
      - 双线性插值缩放 + 归一化 uint8→float32 [0,1] + HWC→NCHW 转换
      - 返回 LetterboxInfo
    - 实现 `restore_coordinates()`：letterbox 空间 → 去除 padding → 还原缩放 → 归一化到原图 → clamp [0,1]
    - _需求：3.2, 3.3, 3.4, 3.5, 3.6, 4.2, 4.3, 4.5, 4.7, 6.1_

  - [x] 2.3 在 `device/src/yolo_detector.cpp` 中添加 YoloDetector 类实现
    - 实现 RAII 自定义 deleter（`OrtEnvDeleter`、`OrtSessionDeleter`、`OrtMemoryInfoDeleter`）
    - 实现 `get_ort_api()` 静态函数（线程安全懒初始化）
    - 实现 `create()` 工厂方法：
      - 检查文件存在性
      - 创建 OrtEnv → SessionOptions（设置线程数）→ Session → MemoryInfo
      - 查询输入/输出名称和维度（`SessionGetInputName`、`SessionGetOutputName`、`SessionGetInputTypeInfo`）
      - 错误时设置 error_msg、释放资源、返回 nullptr
      - 成功时 spdlog::info 记录模型路径和维度
    - 实现私有构造函数
    - 实现移动构造/赋值
    - 实现 `detect_with_stats()`：
      - 前处理：调用 `letterbox_resize()`
      - 推理：创建输入 OrtValue → `OrtApi::Run()` → 获取输出 OrtValue
      - 后处理：遍历 [1,84,8400] 输出 → 置信度过滤 → 构造 Detection → NMS → 坐标还原
      - 各阶段 `steady_clock` 计时
      - spdlog::debug 记录耗时分解
      - 错误时返回空向量 + spdlog::error
    - 实现 `detect()`：委托给 `detect_with_stats().first`
    - 注意：ONNX Runtime API 用法必须参照设计文档中的参考代码和需求文档中的参考代码，不可凭猜测
    - _需求：2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 4.1, 4.6, 5.1, 5.2, 5.3, 5.4_

- [x] 3. 检查点 — 编译通过
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
  - 确认编译无错误（需要 ONNX Runtime 已安装：macOS `brew install onnxruntime`）
  - 确认现有测试（smoke_test、log_test、tee_test、camera_test、health_test）全部通过
  - 如有问题，询问用户


- [x] 4. yolo_test.cpp — Example-Based 测试与 PBT
  - [x] 4.1 创建 `device/tests/yolo_test.cpp` — 基础结构与 example-based 测试
    - 包含 `yolo_detector.h`、`gtest/gtest.h`、`rapidcheck.h`、`rapidcheck/gtest.h`
    - 定义 `YOLO_MODEL_PATH` 宏（CMake 传入）
    - 辅助函数 `model_available()`：检查模型文件是否存在（`std::filesystem::exists`）
    - 辅助函数 `random_rgb_image(w, h)`：生成随机 RGB 像素数据
    - NMS example-based 测试：
      - `NmsEmptyInput`：空输入返回空输出 — _需求：6.2_
      - `NmsSingleBox`：单框直接保留 — _需求：6.2_
      - `NmsSameClassOverlap`：同类别重叠框保留高置信度 — _需求：6.2_
      - `NmsDifferentClassOverlap`：不同类别重叠框均保留 — _需求：6.2_
      - `NmsDeterministic`：同输入两次调用结果相同 — _需求：4.7_
    - Letterbox example-based 测试：
      - `LetterboxSquareInput`：640×640 输入无缩放无填充 — _需求：3.2_
      - `LetterboxWideInput`：宽图上下填充 — _需求：3.2_
      - `LetterboxTallInput`：高图左右填充 — _需求：3.2_
    - 模型加载 example-based 测试：
      - `CreateWithInvalidPath`：无效路径返回 nullptr + 错误信息 — _需求：2.3_
      - `CreateWithValidModel`：有效模型成功创建（模型不可用时 `GTEST_SKIP()`）— _需求：2.2_
    - 端到端测试：
      - `DetectEndToEnd`：加载模型 → 输入测试图像 → 获得检测结果 → 验证结果格式正确（模型不可用时 `GTEST_SKIP()`）— _需求：4.1, 6.6_
      - `DetectWithStatsTimings`：`detect_with_stats` 返回非负耗时（模型不可用时 `GTEST_SKIP()`）— _需求：5.1, 5.3_
    - _需求：6.2, 6.6, 6.7, 6.8_

  - [x] 4.2 添加 PBT — Property 1: NMS 单调递减
    - **Property 1: NMS 单调递减**
    - 随机生成检测框列表（长度 ∈ [0, 200]，class_id ∈ [0, 79]，confidence ∈ (0, 1]，x/y/w/h ∈ (0, 1]）
    - 随机 IoU 阈值 ∈ (0, 1]
    - 验证 NMS 输出数量 ≤ 输入数量
    - **验证：需求 4.2, 6.3**

  - [x] 4.3 添加 PBT — Property 2: NMS IoU 不变量
    - **Property 2: NMS IoU 不变量**
    - 随机生成检测框列表
    - 经过 NMS 后，同一 class_id 的任意两个输出框之间的 IoU ≤ iou_threshold
    - **验证：需求 4.3, 6.3**

  - [x] 4.4 添加 PBT — Property 3: Letterbox 输出尺寸恒定
    - **Property 3: Letterbox 输出尺寸恒定**
    - 随机生成图像尺寸 (w, h)（w ∈ [1, 4096]，h ∈ [1, 4096]）和随机像素数据
    - 验证输出 buffer 大小恒为 3×640×640 = 1,228,800 个 float
    - 验证所有值在 [0.0, 1.0] 范围内
    - 验证 `LetterboxInfo.scale` == `min(640.0/w, 640.0/h)`
    - **验证：需求 3.1, 3.2, 3.3, 3.4, 3.5, 3.6**

  - [x] 4.5 添加 PBT — Property 4: 坐标还原 clamp [0, 1]
    - **Property 4: 坐标还原 clamp [0, 1]**
    - 随机生成 letterbox 空间检测框（x/y/w/h ∈ [0, 1]）和合法的 LetterboxInfo
    - 经过 `restore_coordinates` 后，所有坐标字段 clamp 到 [0.0, 1.0]
    - **验证：需求 4.5, 6.5**

- [x] 5. 检查点 — yolo_test 全量通过
  - 执行 `cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过（现有 smoke_test、log_test、tee_test、camera_test、health_test + 新增 yolo_test）
  - 确认 ASan 无内存错误报告
  - 纯逻辑测试（NMS、letterbox、坐标还原）≤ 1 秒
  - 端到端测试在模型不可用时自动跳过
  - 如有问题，询问用户

- [x] 6. 性能基线测试
  - [x] 6.1 在 `device/tests/yolo_test.cpp` 中添加性能基线测试
    - 实现 `get_rss_kb()` 辅助函数（跨平台：Linux `/proc/self/status`、macOS `task_info`）
    - `PerformanceBaseline` 测试（yolov11s）：
      - 模型不可用时 `GTEST_SKIP()`
      - 加载模型，记录加载前后 RSS 差值
      - 生成 640×480 随机 RGB 图像
      - 执行 10 次 `detect_with_stats`，收集每次的 preprocess_ms、inference_ms、postprocess_ms、total_ms
      - 通过 spdlog::info 输出统计摘要：avg/min/max 各阶段耗时 + RSS 内存差值
    - `PerformanceBaselineNano` 测试（yolov11n，对比）：
      - 同上流程，使用 yolov11n.onnx 模型
      - 模型不可用时 `GTEST_SKIP()`
      - 输出 yolov11n 的 avg/min/max 耗时 + RSS，方便与 yolov11s 对比
    - _需求：7.1, 7.2, 7.3, 7.4, 7.5_

- [x] 7. 最终检查点 — 全量验证
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过
  - 确认 ASan 无内存错误报告
  - 确认现有测试（smoke_test、log_test、tee_test、camera_test、health_test）行为不变
  - 确认 `-DENABLE_YOLO=OFF` 时编译和测试正常（YOLO 模块被跳过）
  - Pi 5 Release 验证（`scripts/pi-build.sh` 或手动 SSH）需要 Pi 5 可达，不可达则标注跳过
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：8.1, 8.2, 8.3, 8.4_

## 备注

- 新建文件：`device/cmake/FindOnnxRuntime.cmake`、`device/src/yolo_detector.h`、`device/src/yolo_detector.cpp`、`device/tests/yolo_test.cpp`、`scripts/download-model.sh`
- 修改文件：`device/CMakeLists.txt`、`.gitignore`
- 不修改文件：所有现有 src 文件和测试文件（pipeline_manager、pipeline_builder、camera_source、pipeline_health、log_init、json_formatter、main.cpp、smoke_test、log_test、tee_test、camera_test、health_test）
- ONNX Runtime API 用法必须参照设计文档和需求文档中的参考代码，不可凭猜测编写
- 独立函数（`compute_iou`、`nms`、`letterbox_resize`、`restore_coordinates`）优先实现并测试，不依赖 ONNX Runtime
- 端到端测试和性能基线测试在模型文件不可用时通过 `GTEST_SKIP()` 自动跳过
- PBT 使用 RapidCheck，每个属性最少 100 次迭代
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
