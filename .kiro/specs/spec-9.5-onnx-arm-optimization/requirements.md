# 需求文档：Spec 9.5 — ONNX Runtime ARM 推理优化

## 简介

本 Spec 针对 Pi 5（Cortex-A76 四核 aarch64）上 ONNX Runtime 推理性能进行优化。Spec 9 采集到的基线数据显示 yolo11s 推理 662ms/帧、yolo11n 推理 249ms/帧（默认 CPU EP，2 线程）。本 Spec 通过 XNNPACK Execution Provider、线程调优、图优化级别、INT8 量化模型等手段降低推理延迟，并建立 A/B 对比基准测试框架，量化每项优化的实际收益。

本 Spec 不改变 YoloDetector 的公开接口（`detect()` / `detect_with_stats()` 签名不变），所有优化通过 `DetectorConfig` 扩展和内部实现调整完成，对 Spec 10（ai-pipeline）透明。

## 前置条件

- Spec 9（yolo-detector）✅ 已完成
- Pi 5 性能基线已采集（yolo11s 662ms, yolo11n 249ms, CPU EP, 2 threads）

## 术语表

- **YoloDetector**：Spec 9 引入的核心类，封装 ONNX Runtime 推理会话
- **Detection**：单个检测结果 POD 结构体（x, y, w, h, class_id, confidence）
- **DetectorConfig**：检测器配置 POD 结构体（confidence_threshold, iou_threshold, num_threads, inter_op_num_threads）
- **InferenceStats**：推理统计 POD 结构体（preprocess_ms, inference_ms, postprocess_ms, total_ms）
- **Execution Provider (EP)**：ONNX Runtime 的后端执行引擎，决定算子在哪个硬件/库上执行
- **CPU EP**：默认的 CPU Execution Provider，使用 ONNX Runtime 内置的 CPU 算子实现
- **XNNPACK EP**：基于 Google XNNPACK 库的 Execution Provider，针对 ARM NEON 指令集优化，适合 Cortex-A76 等 ARM CPU
- **Graph Optimization Level**：ONNX Runtime 的图优化级别（ORT_DISABLE_ALL / ORT_ENABLE_BASIC / ORT_ENABLE_EXTENDED / ORT_ENABLE_ALL）
- **INT8 量化**：将 FP32 模型权重和激活值量化为 INT8，减少计算量和内存占用，可能轻微降低精度
- **Benchmark_Runner**：本 Spec 新增的基准测试框架，执行 A/B 对比测试并输出结构化结果

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| ONNX Runtime 版本 | v1.24.4 预编译 aarch64 包（默认不含 XNNPACK EP；XNNPACK 需在 Pi 5 上手动源码编译 ONNX Runtime 并替换预编译库） |
| XNNPACK 编译 | Pi 5 上手动源码编译（`--use_xnnpack`），本 Spec 只负责代码集成，不自动化编译流程 |
| Pi 5 硬件 | Cortex-A76 四核，4GB RAM |
| Pi 5 内存限制 | 推理时峰值 RSS 增量 ≤ 300MB（与 Spec 9 一致） |
| 接口兼容性 | YoloDetector 公开接口（detect / detect_with_stats 签名）不变，优化通过 DetectorConfig 扩展 |
| 单个测试耗时 | 纯逻辑测试 ≤ 1 秒，含推理测试 ≤ 10 秒，基准测试 ≤ 120 秒 |
| 代码组织 | .h + .cpp 分离模式 |
| 新增代码量 | 100-400 行 |
| 涉及文件 | 3-6 个 |
| 日志语言 | 英文，不使用非 ASCII 字符 |
| 基线对比 | 所有优化必须与 Spec 9 基线数据进行 A/B 对比，量化加速比 |
| 基准测试平台 | 仅在 Pi 5 Release 上运行，macOS Debug 上跳过（ARM 优化在 x86 上无意义） |

## 禁止项

### Requirements 层

- SHALL NOT 改变 YoloDetector 的公开接口签名（`detect()` / `detect_with_stats()` 的参数和返回类型不变）
- SHALL NOT 在本 Spec 中集成到 GStreamer 管道（属于后续 ai-pipeline Spec）
- SHALL NOT 在本 Spec 中实现 GPU/NPU 加速（Pi 5 无 CUDA/OpenCL 可用 EP）
- SHALL NOT 在本 Spec 中实现模型训练、微调或自定义模型导出

### Design 层

- SHALL NOT 将 ONNX Runtime 通过 FetchContent 源码编译引入（编译时间 30+ 分钟，Pi 5 上更久）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
- SHALL NOT 在不确定 ONNX Runtime API 用法时凭猜测编写代码，必须先查阅官方文档确认
- SHALL NOT 破坏现有 NMS、letterbox_resize、restore_coordinates 等独立函数的行为（这些函数已有 PBT 覆盖）

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（ONNX Runtime C API 的资源通过 RAII 封装释放）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在无法本地复现的远程平台问题上凭猜测修复

## 需求

### 需求 1：DetectorConfig 扩展 — 优化选项配置

**用户故事：** 作为开发者，我需要通过配置参数控制 ONNX Runtime 的优化选项，以便在不修改代码的情况下切换不同的优化策略。

#### 验收标准

1. THE DetectorConfig SHALL 新增 `bool use_xnnpack` 字段（默认 false），控制是否启用 XNNPACK Execution Provider
2. THE DetectorConfig SHALL 新增 `int graph_optimization_level` 字段（默认 99，对应 ORT_ENABLE_ALL），控制 ONNX Runtime 图优化级别
3. THE DetectorConfig SHALL 新增 `int inter_op_num_threads` 字段（默认 1），控制 ONNX Runtime 图节点间并行线程数（YOLO 串行结构，默认 1 即可）
4. THE DetectorConfig SHALL 保持向后兼容：所有新增字段有默认值，现有代码无需修改即可编译通过
5. WHEN DetectorConfig 使用默认值创建时，THE YoloDetector SHALL 产生与 Spec 9 基线一致的行为（CPU EP，2 intra-op 线程，1 inter-op 线程，无 XNNPACK）

### 需求 2：XNNPACK Execution Provider 集成

**用户故事：** 作为开发者，我需要在 Pi 5 上启用 XNNPACK EP，以利用 ARM NEON 指令集加速推理。

#### 验收标准

1. WHEN `use_xnnpack` 为 true 且 ONNX Runtime 支持 XNNPACK EP 时，THE YoloDetector SHALL 在创建会话时注册 XNNPACK Execution Provider
2. WHEN `use_xnnpack` 为 true 但 ONNX Runtime 不支持 XNNPACK EP 时，THE YoloDetector SHALL 通过 spdlog（warn 级别）记录回退信息，并使用默认 CPU EP 继续运行
3. THE YoloDetector SHALL 通过 spdlog（info 级别）记录实际使用的 Execution Provider 名称
4. WHEN XNNPACK EP 启用时，THE YoloDetector SHALL 产生与 CPU EP 功能等价的检测结果（检测框坐标和类别一致，置信度允许 ±0.01 浮点误差）

### 需求 3：线程数调优

**用户故事：** 作为开发者，我需要测试不同线程配置对推理性能的影响，以找到 Pi 5 四核 CPU 的最优线程数。

#### 验收标准

1. THE YoloDetector SHALL 支持 `num_threads`（intra-op）值为 1、2、3、4 的配置
2. THE YoloDetector SHALL 支持 `inter_op_num_threads` 值为 1、2 的配置（YOLO 串行结构，默认 1）
3. THE Benchmark_Runner SHALL 对不同 intra-op / inter-op 线程组合执行推理基准测试，记录平均推理耗时
4. THE Benchmark_Runner SHALL 通过 spdlog（info 级别）输出线程配置 vs 推理耗时的对比表

### 需求 4：图优化级别配置

**用户故事：** 作为开发者，我需要控制 ONNX Runtime 的图优化级别，以评估不同优化级别对推理性能的影响。

#### 验收标准

1. WHEN `graph_optimization_level` 为 0 时，THE YoloDetector SHALL 设置 `ORT_DISABLE_ALL`
2. WHEN `graph_optimization_level` 为 1 时，THE YoloDetector SHALL 设置 `ORT_ENABLE_BASIC`
3. WHEN `graph_optimization_level` 为 2 时，THE YoloDetector SHALL 设置 `ORT_ENABLE_EXTENDED`
4. WHEN `graph_optimization_level` 为 99 时，THE YoloDetector SHALL 设置 `ORT_ENABLE_ALL`（默认值）
5. THE YoloDetector SHALL 通过 spdlog（info 级别）记录实际使用的图优化级别

### 需求 5：INT8 量化模型支持

**用户故事：** 作为开发者，我需要支持加载 INT8 量化后的 ONNX 模型，以评估量化对推理速度和检测精度的影响。

#### 验收标准

1. THE YoloDetector SHALL 能够加载 INT8 量化后的 ONNX 模型文件（与 FP32 模型使用相同的 `create()` 接口）
2. WHEN 加载 INT8 量化模型时，THE YoloDetector SHALL 通过 spdlog（info 级别）记录模型的数据类型信息
3. THE `scripts/quantize-model.sh` SHALL 使用 `onnxruntime.quantization` Python 库对 FP32 模型执行 post-training quantization，生成 INT8 量化模型到 `device/models/`
4. THE 量化脚本 SHALL 依赖项目 Python venv（`.venv-raspi-eye/`），需要 `onnxruntime` 和 `onnx` Python 包
5. IF INT8 量化模型文件不可用，THEN THE 相关基准测试 SHALL 跳过，不导致测试失败
6. THE Benchmark_Runner SHALL 对比 FP32 vs INT8 模型的推理速度和检测结果差异（检测框数量、置信度分布），评估量化收益

### 需求 6：A/B 对比基准测试框架

**用户故事：** 作为开发者，我需要一个结构化的基准测试框架，以量化每项优化的实际收益并与 Spec 9 基线对比。

#### 验收标准

1. THE Benchmark_Runner SHALL 对每种优化配置执行 N 次推理（N ≥ 10），记录 preprocess_ms、inference_ms、postprocess_ms、total_ms 的平均值、最小值、最大值
2. THE Benchmark_Runner SHALL 记录每种配置的进程 RSS 内存增量
3. THE Benchmark_Runner SHALL 在 Linux 上读取 CPU 温度（`/sys/class/thermal/thermal_zone0/temp`），每组测试前记录温度，如果温度 ≥ 80°C 则等待冷却至 < 70°C 后再开始
4. THE Benchmark_Runner SHALL 通过 spdlog（info 级别）输出结构化的对比结果表，包含：配置名称、平均推理耗时、相对 Spec 9 基线的加速比、CPU 温度
5. THE Benchmark_Runner SHALL 测试以下配置组合（至少）：
   - 基线：CPU EP, 2 threads, ORT_ENABLE_ALL（复现 Spec 9 基线）
   - 线程调优：CPU EP, 1/3/4 threads, ORT_ENABLE_ALL
   - XNNPACK：XNNPACK EP, 2 threads, ORT_ENABLE_ALL（如果可用）
   - 图优化：CPU EP, 2 threads, ORT_DISABLE_ALL / ORT_ENABLE_BASIC / ORT_ENABLE_EXTENDED
   - INT8 量化：CPU EP, 2 threads, ORT_ENABLE_ALL, INT8 模型（如果可用）
5. IF 某项优化配置不可用（如 XNNPACK EP 未编译、INT8 模型未下载），THEN THE Benchmark_Runner SHALL 跳过该配置并记录跳过原因，不导致测试失败
6. THE 基准测试结果 SHALL 记录到 `docs/development-trace.md`

### 需求 7：双平台构建与测试验证

**用户故事：** 作为开发者，我需要确保优化代码在 macOS 和 Pi 5 上均能成功编译和测试。

#### 验收标准

1. WHEN 在 macOS 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试（包括现有 6 个套件和新增的基准测试），ASan 不报告任何内存错误
2. WHEN 在 Pi 5 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试
3. THE Build_System SHALL 保持现有测试（smoke_test、log_test、tee_test、camera_test、health_test、yolo_test）全部通过，行为不变
4. WHEN XNNPACK EP 不可用时，THE 相关测试 SHALL 优雅跳过，不导致编译或测试失败

## 参考代码

### XNNPACK EP 注册（ONNX Runtime C API）

```cpp
#include <onnxruntime_c_api.h>

// XNNPACK EP 通过 SessionOptionsAppendExecutionProvider 注册
// 注意：需要 ONNX Runtime 编译时启用 --use_xnnpack
OrtSessionOptions* opts = nullptr;
ort->CreateSessionOptions(&opts);

// 方式 1：通过通用 API（v1.14+）
// 键值对配置 XNNPACK 选项
const char* keys[] = {"intra_op_num_threads"};
const char* values[] = {"2"};
OrtStatus* status = OrtSessionOptionsAppendExecutionProvider(
    opts, "XNNPACK", keys, values, 1);
if (status) {
    // XNNPACK 不可用，回退到 CPU EP
    const char* msg = ort->GetErrorMessage(status);
    spdlog::warn("XNNPACK EP not available: {}, falling back to CPU EP", msg);
    ort->ReleaseStatus(status);
}
```

### 图优化级别设置

```cpp
// ORT_DISABLE_ALL = 0, ORT_ENABLE_BASIC = 1,
// ORT_ENABLE_EXTENDED = 2, ORT_ENABLE_ALL = 99
ort->SetSessionGraphOptimizationLevel(opts,
    static_cast<GraphOptimizationLevel>(config.graph_optimization_level));
```

### 进程 RSS 内存获取（已有实现，复用 Spec 9）

```cpp
#ifdef __linux__
// 读取 /proc/self/status 中的 VmRSS 行
#elif defined(__APPLE__)
#include <mach/mach.h>
// task_info(mach_task_self(), MACH_TASK_BASIC_INFO, ...)
#endif
```

## 验证命令

```bash
# macOS Debug 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# Pi 5 Release 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# 跳过 YOLO 模块（ONNX Runtime 未安装时）
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug -DENABLE_YOLO=OFF && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```

预期结果：两个平台均配置成功、编译无错误、所有测试通过（macOS 下 ASan 无报告）。XNNPACK EP 不可用时相关测试自动跳过。INT8 模型不可用时相关基准测试自动跳过。

## 明确不包含

- GStreamer 管道集成（后续 ai-pipeline Spec）
- 抽帧策略与帧率控制（后续 ai-pipeline Spec）
- 截图上传 S3（Spec 11: screenshot-uploader）
- GPU/NPU 加速（Pi 5 无可用硬件加速 EP）
- 模型训练、微调或自定义模型导出
- OpenCV 依赖（保持零额外依赖）
- 模型热更新或运行时切换模型
- 从源码编译 ONNX Runtime（Pi 5 上手动执行，本 Spec 只负责代码集成；编译步骤记录到 `docs/pi-setup.md`）
