# 实施计划：Spec 9.5 — ONNX Runtime ARM 推理优化

## 概述

在 Spec 9（yolo-detector）基础上，通过 DetectorConfig 扩展、XNNPACK EP 运行时注册、图优化级别配置、INT8 量化脚本和 A/B 对比基准测试框架，优化 Pi 5 上的 ONNX Runtime 推理性能。所有优化通过配置字段控制，不改变 YoloDetector 公开接口。实现语言为 C++17，不新增 PBT（本 Spec 无新纯函数）。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（ONNX Runtime C API 资源通过 RAII unique_ptr 封装释放）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 在不确定 ONNX Runtime API 用法时凭猜测编写代码，必须先查阅官方文档或头文件确认
- SHALL NOT 破坏现有 NMS、letterbox_resize、restore_coordinates 等独立函数的行为（已有 PBT 覆盖）
- SHALL NOT 改变 YoloDetector 的公开接口签名（detect / detect_with_stats 的参数和返回类型不变）
- SHALL NOT 修改现有测试文件（smoke_test.cpp、log_test.cpp、tee_test.cpp、camera_test.cpp、health_test.cpp）

## 任务

- [x] 0. 文档：Pi 5 上源码编译带 XNNPACK EP 的 ONNX Runtime
  - [x] 0.1 修改 `docs/pi-setup.md` — 新增 ONNX Runtime XNNPACK 源码编译章节
    - 在现有 "Install ONNX Runtime" 章节之后新增 "Build ONNX Runtime with XNNPACK EP (Optional)" 章节
    - 包含：前置依赖安装（cmake ≥ 3.26、python3-dev、python3-numpy）
    - 包含：git clone onnxruntime v1.24.4 + 递归子模块
    - 包含：编译命令 `./build.sh --config Release --parallel --build_shared_lib --use_xnnpack --skip_tests`
    - 包含：Pi 5 4GB 内存注意事项（可能需要 `--parallel 1` 或增加 swap）
    - 包含：安装编译产物到 /usr/local（替换预编译版本）
    - 包含：验证 XNNPACK EP 可用性的方法
    - _需求：2.1（XNNPACK EP 集成的前置条件）_

- [x] 1. DetectorConfig 扩展与 create() 工厂方法变更
  - [x] 1.1 修改 `device/src/yolo_detector.h` — DetectorConfig 新增字段
    - 在 `DetectorConfig` 结构体中新增 `bool use_xnnpack = false` 字段
    - 在 `DetectorConfig` 结构体中新增 `int graph_optimization_level = 99` 字段
    - 在 `DetectorConfig` 结构体中新增 `int inter_op_num_threads = 1` 字段（图节点间并行，YOLO 串行结构默认 1）
    - 保持现有 3 个字段（confidence_threshold、iou_threshold、num_threads）不变
    - 所有新增字段有默认值，现有代码 `DetectorConfig{}` 无需修改即可编译
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5_

  - [x] 1.2 修改 `device/src/yolo_detector.cpp` — inter-op 线程数 + 图优化级别设置
    - 在 `create()` 中 `SetIntraOpNumThreads` 之后，新增 `ort->SetInterOpNumThreads(opts, config.inter_op_num_threads)`
    - 设置失败时 spdlog::warn 记录（非致命，继续运行）
    - spdlog::info 记录 "Threads: intra-op={}, inter-op={}"
    - 继续新增图优化级别设置逻辑（在 inter-op 之后、CreateSession 之前）
    - 使用 `ort->SetSessionGraphOptimizationLevel(opts, static_cast<GraphOptimizationLevel>(config.graph_optimization_level))`
    - 设置失败时释放资源、spdlog::error 记录、返回 nullptr
    - 设置成功时 spdlog::info 记录实际使用的图优化级别
    - 参照设计文档中的图优化级别设置代码片段
    - _需求：4.1, 4.2, 4.3, 4.4, 4.5_

  - [x] 1.3 修改 `device/src/yolo_detector.cpp` — XNNPACK EP 运行时注册
    - 在图优化级别设置之后、`CreateSession` 之前，新增 XNNPACK EP 注册逻辑
    - 当 `config.use_xnnpack` 为 true 时，调用 `OrtSessionOptionsAppendExecutionProvider(opts, "XNNPACK", nullptr, nullptr, 0)`
    - 注册成功时 spdlog::info 记录 "Execution Provider: XNNPACK"
    - 注册失败时 spdlog::warn 记录回退信息（含 ORT 错误消息），释放 OrtStatus，继续使用 CPU EP（不返回错误）
    - 当 `config.use_xnnpack` 为 false 时 spdlog::info 记录 "Execution Provider: CPU"
    - 注意：`OrtSessionOptionsAppendExecutionProvider` 是全局函数（非 OrtApi 成员），在 `onnxruntime_c_api.h` 中声明
    - 参照设计文档中的 XNNPACK EP 注册代码片段和需求文档中的参考代码
    - _需求：2.1, 2.2, 2.3_

- [x] 2. 检查点 — 编译通过 + 现有测试回归
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认编译无错误，现有 6 个测试套件（smoke_test、log_test、tee_test、camera_test、health_test、yolo_test）全部通过
  - 确认 ASan 无内存错误报告
  - 确认 Spec 9 的 4 个 PBT 属性（NMS 单调递减、NMS IoU 不变量、Letterbox 输出尺寸恒定、坐标还原 clamp）仍然通过
  - 如有问题，询问用户

- [x] 3. 配置测试与图优化级别测试
  - [x] 3.1 修改 `device/tests/yolo_test.cpp` — 新增配置扩展测试
    - `ConfigDefaultValues` 测试：验证默认 DetectorConfig 的 use_xnnpack==false、graph_optimization_level==99
    - `ConfigBackwardCompatible` 测试：验证旧式初始化 `{0.25f, 0.45f, 2}` 编译通过（C++ aggregate init 向后兼容）
    - _需求：1.1, 1.2, 1.3_

  - [x] 3.2 修改 `device/tests/yolo_test.cpp` — 新增图优化级别测试
    - `GraphOptLevelAll` 测试：graph_optimization_level=99 创建检测器成功（模型不可用时 GTEST_SKIP）
    - `GraphOptLevelDisable` 测试：graph_optimization_level=0 创建检测器成功
    - `GraphOptLevelBasic` 测试：graph_optimization_level=1 创建检测器成功
    - `GraphOptLevelExtended` 测试：graph_optimization_level=2 创建检测器成功
    - _需求：4.1, 4.2, 4.3, 4.4_

  - [x] 3.3 修改 `device/tests/yolo_test.cpp` — 新增 XNNPACK 回退测试
    - `XnnpackFallbackOnUnsupported` 测试：use_xnnpack=true 但 XNNPACK 不可用时，create() 仍返回有效检测器（非 nullptr）
    - 模型不可用时 GTEST_SKIP
    - _需求：2.2, 2.4_

- [x] 4. CMakeLists.txt 更新与量化脚本
  - [x] 4.1 修改 `device/CMakeLists.txt` — 新增 INT8 模型路径编译定义
    - 新增 `YOLO_MODEL_PATH_SMALL_INT8` 和 `YOLO_MODEL_PATH_NANO_INT8` 变量
    - 在 `target_compile_definitions(yolo_test ...)` 中追加 `YOLO_MODEL_PATH_SMALL_INT8` 和 `YOLO_MODEL_PATH_NANO_INT8`
    - 不修改现有编译定义和链接配置
    - _需求：5.1, 5.5_

  - [x] 4.2 创建 `scripts/quantize-model.sh` — INT8 量化脚本
    - 脚本开头 `#!/usr/bin/env bash` + `set -euo pipefail`
    - 检查 `.venv-raspi-eye/` venv 存在，不存在则报错退出
    - `source .venv-raspi-eye/bin/activate` 激活 venv
    - 定义 `quantize_model()` 函数：输入 FP32 模型路径 + 输出 INT8 模型路径
      - 输入不存在则 SKIP
      - 输出已存在则 SKIP（不覆盖）
      - 调用 `python3 -c "from onnxruntime.quantization import quantize_dynamic, QuantType; quantize_dynamic('input', 'output', weight_type=QuantType.QInt8)"`
    - 对 yolo11s.onnx 和 yolo11n.onnx 分别执行量化
    - 添加可执行权限
    - _需求：5.3, 5.4_

  - [x] 4.3 修改 `.gitignore` — 确认 INT8 模型文件被排除
    - 确认 `*.onnx` 或 `device/models/` 已在 .gitignore 中（Spec 9 已添加）
    - 如果未覆盖 INT8 模型文件，补充排除规则
    - _需求：约束条件（模型文件不提交 git）_

- [x] 5. 检查点 — 编译通过 + 配置测试通过
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过（现有 + 新增配置测试 + 图优化级别测试 + XNNPACK 回退测试）
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户

- [x] 6. A/B 对比基准测试框架
  - [x] 6.1 修改 `device/tests/yolo_test.cpp` — 新增 BenchConfig 结构体、温度监控和 run_benchmark 函数
    - 定义 `BenchConfig` 结构体：label（string）、detector_config（DetectorConfig）、model_path（string）
    - 实现 `get_cpu_temp_celsius()` 辅助函数：Linux 读取 `/sys/class/thermal/thermal_zone0/temp`（除以 1000），macOS 返回 -1
    - 实现 `wait_for_cool_cpu()` 辅助函数：Linux 上循环检查温度，≥ 80°C 时 sleep 5 秒等待冷却至 < 70°C，macOS 上直接返回
    - 实现 `run_benchmark(const BenchConfig& bench, int runs = 10)` 函数：
      - 模型文件不存在则 spdlog::info SKIPPED 并 return
      - 调用 `wait_for_cool_cpu()` 等待冷却
      - 记录测试前 CPU 温度
      - 创建检测器失败则 spdlog::info SKIPPED 并 return
      - 记录 RSS 内存增量
      - 执行 N 次 detect_with_stats，收集各阶段耗时
      - 通过 spdlog::info 输出结构化结果（配置名称、runs 数、avg/min/max 耗时、RSS delta、CPU 温度）
    - 复用现有 `get_rss_kb()` 和 `random_rgb_image()` 辅助函数
    - _需求：6.1, 6.2, 6.3, 6.4_

  - [x] 6.2 修改 `device/tests/yolo_test.cpp` — 新增 OptimizationComparison 测试
    - `YoloBenchmark::OptimizationComparison` 测试：
      - macOS 上 `GTEST_SKIP()` 跳过（`#ifdef __APPLE__`）
      - 模型不可用时 `GTEST_SKIP()`
      - 定义配置矩阵（至少 9 种配置）：
        - baseline-cpu-2t-all：CPU EP, intra=2, inter=1, ORT_ENABLE_ALL（复现 Spec 9 基线）
        - cpu-1t-all / cpu-3t-all / cpu-4t-all：intra-op 线程调优（inter=1）
        - cpu-4t-inter2-all：intra=4, inter=2（测试 inter-op 影响）
        - xnnpack-2t-all：XNNPACK EP（如果可用）
        - cpu-2t-disable / cpu-2t-basic / cpu-2t-extended：图优化级别对比
      - 遍历配置矩阵调用 run_benchmark
    - _需求：3.1, 3.2, 3.3, 6.4, 6.5_

  - [x] 6.3 修改 `device/tests/yolo_test.cpp` — 新增 Int8ModelBenchmark 测试
    - `YoloBenchmark::Int8ModelBenchmark` 测试：
      - macOS 上 `GTEST_SKIP()` 跳过
      - FP32 和 INT8 模型均不可用时 `GTEST_SKIP()`
      - 对比 FP32 vs INT8 模型（CPU EP, 2 threads, ORT_ENABLE_ALL）
      - 使用 `YOLO_MODEL_PATH_SMALL_INT8` 编译定义
    - _需求：5.1, 5.5, 5.6, 6.4_

- [x] 7. 最终检查点 — 全量验证
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过（现有 6 个套件 + 新增配置测试 + 图优化级别测试 + XNNPACK 回退测试 + 基准测试在 macOS 上跳过）
  - 确认 ASan 无内存错误报告
  - 确认现有测试（smoke_test、log_test、tee_test、camera_test、health_test）行为不变
  - 确认 Spec 9 的 4 个 PBT 属性仍然通过
  - 确认 `-DENABLE_YOLO=OFF` 时编译和测试正常
  - Pi 5 Release 验证（`scripts/pi-build.sh` 或手动 SSH）需要 Pi 5 可达，不可达则标注跳过
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：7.1, 7.2, 7.3, 7.4_

## 备注

- 修改文件：`device/src/yolo_detector.h`、`device/src/yolo_detector.cpp`、`device/tests/yolo_test.cpp`、`device/CMakeLists.txt`、`.gitignore`、`docs/pi-setup.md`
- 新建文件：`scripts/quantize-model.sh`
- 不修改文件：所有现有 src 文件（pipeline_manager、pipeline_builder、camera_source、pipeline_health、log_init、json_formatter、main.cpp）和现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test）
- ONNX Runtime API 用法必须参照设计文档和需求文档中的参考代码，不可凭猜测编写
- 现有独立函数（nms、compute_iou、letterbox_resize、restore_coordinates）不修改，已有 PBT 覆盖
- 基准测试仅在 Pi 5 Release 上运行，macOS 上通过 GTEST_SKIP 跳过
- INT8 模型文件由 quantize-model.sh 脚本生成，不提交 git
- 基准测试结果记录到 `docs/development-trace.md`（Pi 5 验证时手动记录）
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
