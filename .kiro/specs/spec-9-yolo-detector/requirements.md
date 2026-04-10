# 需求文档：Spec 9 — YOLO 设备端目标检测（独立模块）

## 简介

本 Spec 为 raspi-eye 项目引入独立的 YOLO 目标检测模块。使用 ONNX Runtime 加载 YOLOv11s 模型，对输入图像执行目标检测（鸟、人、猫、狗等 COCO 80 类），输出检测框、类别 ID、置信度。

本模块是纯推理模块，不依赖 GStreamer 管道。输入为 RGB 像素数据（`uint8_t*` + 宽高），输出为检测结果向量。管道集成（buffer probe 抽帧 + 检测）属于 Spec 10（ai-pipeline）。

同时采集推理性能基线：Pi 5 上 YOLOv11s 640×640 单帧推理耗时和峰值内存，为后续 AI 管道的抽帧策略提供数据支撑。

## 前置条件

- Spec 0（gstreamer-capture）✅ 已完成
- Spec 1（spdlog-logging）✅ 已完成
- Spec 2（cross-compile）✅ 已完成
- Spec 3（h264-tee-pipeline）✅ 已完成
- Spec 4（camera-abstraction）✅ 已完成
- Spec 5（pipeline-health）✅ 已完成

## 术语表

- **YoloDetector**：本 Spec 新增的核心类，封装 ONNX Runtime 推理会话，提供 detect() 方法
- **Detection**：单个检测结果 POD 结构体，包含 bounding box（x, y, w, h 归一化坐标）、class_id、confidence
- **ONNX Runtime**：微软开源的跨平台推理引擎，支持 ONNX 格式模型
- **YOLOv11s**：Ultralytics YOLO v11 small 模型，ONNX 格式，输入 640×640 RGB，输出检测框+类别+置信度
- **Letterbox Resize**：等比缩放 + 灰色填充到目标尺寸（640×640），保持原图宽高比不变形
- **NMS（Non-Maximum Suppression）**：非极大值抑制，去除重叠检测框，保留置信度最高的框
- **IoU（Intersection over Union）**：两个检测框的交集面积与并集面积之比，用于 NMS 判断重叠程度
- **Confidence Threshold**：置信度阈值，低于此值的检测结果被过滤
- **IoU Threshold**：NMS 中的 IoU 阈值，高于此值的重叠框被抑制
- **InferenceStats**：推理统计 POD 结构体，包含推理耗时（毫秒）和预处理耗时
- **FetchContent**：CMake 内置模块，用于在配置阶段自动下载和构建第三方依赖

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| ONNX Runtime 引入方式 | 预编译库（macOS: Homebrew `brew install onnxruntime`，Pi 5: GitHub Releases 下载 aarch64 包），不使用 FetchContent 源码编译（编译时间过长） |
| 模型文件 | YOLOv11s.onnx（small，主力）+ YOLOv11n.onnx（nano，对比基线），640×640 输入，通过 `scripts/download-model.sh` 脚本下载 |
| 模型文件管理 | 不提交到 git（.gitignore 排除 *.onnx），脚本自动下载到 `device/models/`，路径通过参数传入 |
| Pi 5 内存限制 | 推理时峰值 RSS 增量 ≤ 300MB（4GB 总内存，需留余量给 GStreamer 管道和系统） |
| 单帧推理耗时 | 基线采集，记录实际值到 `docs/development-trace.md`，不设硬性 pass/fail 阈值（Spec 10 根据基线数据设定） |
| 单个测试耗时 | 纯逻辑测试 ≤ 1 秒，含推理测试 ≤ 10 秒 |
| 代码组织 | .h + .cpp 分离模式 |
| 新增代码量 | 200-500 行 |
| 涉及文件 | 3-6 个 |
| 日志语言 | 英文，不使用非 ASCII 字符 |
| 线程安全 | YoloDetector 实例不要求线程安全（单线程调用，线程安全由上层 Spec 10 管道保证） |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中集成到 GStreamer 管道（属于 Spec 10: ai-pipeline）
- SHALL NOT 在本 Spec 中实现抽帧策略或帧率控制（属于 Spec 10）
- SHALL NOT 在本 Spec 中实现截图上传功能（属于 Spec 11: screenshot-uploader）
- SHALL NOT 在本 Spec 中实现模型热更新或运行时切换模型
- SHALL NOT 在本 Spec 中实现 GPU/NPU 加速（Pi 5 无 CUDA，后续可探索 XNNPACK delegate）

### Design 层

- SHALL NOT 将 ONNX Runtime 通过 FetchContent 源码编译引入（编译时间 30+ 分钟，Pi 5 上更久）
- SHALL NOT 将模型文件（.onnx）提交到 git 仓库（文件 > 20MB，使用脚本下载或手动放置）
- SHALL NOT 在 YoloDetector 内部管理图像采集或 GStreamer 元素（纯推理模块，输入为原始像素数据）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
- SHALL NOT 在不确定 ONNX Runtime API 用法时凭猜测编写代码，必须先查阅官方文档确认

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（ONNX Runtime C API 的资源通过 RAII 封装释放）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件

## 需求

### 需求 1：ONNX Runtime 引入与 CMake 集成

**用户故事：** 作为开发者，我需要在 CMake 构建系统中引入 ONNX Runtime，以便 C++ 代码可以加载和运行 ONNX 模型。

#### 验收标准

1. THE Build_System SHALL 通过 CMake 引入 ONNX Runtime 预编译库，支持 macOS（x86_64 / ARM64）和 Linux aarch64 两个平台
2. WHEN 在 macOS 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug` 时，THE Build_System SHALL 成功找到 ONNX Runtime 头文件和库文件，无配置错误
3. WHEN 在 Pi 5 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release` 时，THE Build_System SHALL 成功找到 ONNX Runtime 头文件和库文件，无配置错误
4. THE Build_System SHALL 将 ONNX Runtime 依赖封装为独立的 CMake target（如 `yolo_module`），不影响现有 `pipeline_manager` 库的编译
5. WHEN 编译完成后，THE Build_System SHALL 保持现有测试（smoke_test、log_test、tee_test、camera_test、health_test）全部通过

### 需求 2：YoloDetector 模型加载与会话管理

**用户故事：** 作为开发者，我需要一个封装类来管理 ONNX Runtime 推理会话的生命周期，以便安全地加载模型和释放资源。

#### 验收标准

1. THE YoloDetector SHALL 在构造时接受模型文件路径（`std::string`）和可选的配置参数（置信度阈值、IoU 阈值、线程数）
2. WHEN 提供有效的 ONNX 模型文件路径时，THE YoloDetector SHALL 成功创建 ONNX Runtime 推理会话
3. IF 模型文件路径不存在或文件格式无效，THEN THE YoloDetector SHALL 返回错误信息（通过 `std::optional` 或工厂方法返回 `nullptr` + 错误字符串），不抛出异常
4. THE YoloDetector SHALL 通过 RAII 管理所有 ONNX Runtime 资源（OrtEnv、OrtSession、OrtMemoryInfo），析构时自动释放
5. THE YoloDetector SHALL 禁用拷贝构造和拷贝赋值（`= delete`），支持移动语义
6. THE YoloDetector SHALL 在创建会话时将 ONNX Runtime 推理线程数设置为可配置值（默认 2，Pi 5 四核需留余量给 GStreamer）
7. THE YoloDetector SHALL 通过 spdlog 记录模型加载成功/失败信息，包含模型路径和输入/输出张量维度

### 需求 3：图像前处理（Letterbox Resize）

**用户故事：** 作为开发者，我需要将任意尺寸的输入图像转换为模型要求的 640×640 输入格式，以便正确执行推理。

#### 验收标准

1. THE YoloDetector SHALL 接受任意尺寸的 RGB 图像输入（`const uint8_t* data, int width, int height`）
2. WHEN 输入图像尺寸不是 640×640 时，THE YoloDetector SHALL 执行 letterbox resize：等比缩放到 640×640 内，短边用灰色（128）填充
3. THE letterbox resize SHALL 保持原图宽高比，缩放后的图像居中放置
4. THE YoloDetector SHALL 将 letterbox 后的 uint8 RGB 像素归一化为 float32 [0.0, 1.0] 范围
5. THE YoloDetector SHALL 将归一化后的数据转换为 NCHW 格式（1×3×640×640）作为模型输入张量
6. FOR ALL 有效的输入图像尺寸（宽高均 > 0），letterbox resize 后的输出 SHALL 始终为 640×640×3（round-trip 属性：缩放比例和偏移量可用于还原坐标）

### 需求 4：推理执行与后处理（NMS）

**用户故事：** 作为开发者，我需要执行模型推理并对原始输出进行后处理，以获得最终的检测结果列表。

#### 验收标准

1. WHEN 调用 `detect(const uint8_t* data, int width, int height)` 时，THE YoloDetector SHALL 执行前处理、模型推理、后处理，返回 `std::vector<Detection>`
2. THE YoloDetector SHALL 过滤置信度低于阈值（默认 0.25）的检测结果
3. THE YoloDetector SHALL 对过滤后的检测结果执行 NMS，IoU 阈值默认 0.45
4. THE Detection 结构体 SHALL 包含以下字段：`float x, y, w, h`（归一化到原图坐标，0.0-1.0）、`int class_id`、`float confidence`
5. THE YoloDetector SHALL 将检测框坐标从 letterbox 空间还原到原图归一化坐标（去除 letterbox 填充偏移和缩放）
6. IF 推理过程中 ONNX Runtime 返回错误，THEN THE YoloDetector SHALL 通过 spdlog 记录错误信息并返回空的检测结果向量
7. THE NMS 实现 SHALL 对同一组输入产生确定性结果（相同输入 → 相同输出顺序）

### 需求 5：推理性能统计

**用户故事：** 作为开发者，我需要采集每次推理的耗时数据，以便评估 Pi 5 上的推理性能并为 Spec 10 的抽帧策略提供数据支撑。

#### 验收标准

1. THE YoloDetector SHALL 提供 `detect_with_stats(const uint8_t* data, int width, int height)` 方法，返回 `std::pair<std::vector<Detection>, InferenceStats>`
2. THE InferenceStats 结构体 SHALL 包含以下字段：`double preprocess_ms`（前处理耗时）、`double inference_ms`（模型推理耗时）、`double postprocess_ms`（后处理耗时）、`double total_ms`（总耗时）
3. THE YoloDetector SHALL 使用 `std::chrono::steady_clock` 测量各阶段耗时
4. THE YoloDetector SHALL 通过 spdlog（debug 级别）记录每次推理的耗时分解

### 需求 6：单元测试与 PBT

**用户故事：** 作为开发者，我需要通过单元测试和属性测试验证检测模块的正确性。

#### 验收标准

1. THE NMS 实现 SHALL 作为独立的内部函数（非 YoloDetector 成员），接受 `std::vector<Detection>` 和 IoU 阈值，返回过滤后的 `std::vector<Detection>`
2. THE Test_Suite SHALL 包含 NMS 的 example-based 测试：空输入、单框、同类别重叠框（保留高置信度）、不同类别重叠框（均保留）
3. THE Test_Suite SHALL 包含 NMS 的 PBT 属性测试：输出数量 ≤ 输入数量（单调递减）、同类别输出框 IoU ≤ 阈值（不变量）
4. THE Test_Suite SHALL 包含 letterbox resize 的 PBT 属性测试：任意输入尺寸（1-4096）输出恒为 640×640、缩放比例 = `min(640/w, 640/h)`、填充居中（差 ≤ 1px）
5. THE Test_Suite SHALL 包含 letterbox 坐标还原的 PBT 属性测试：还原后坐标 clamp 到 [0.0, 1.0]
6. WHEN 模型文件可用时，THE Test_Suite SHALL 包含端到端推理测试：加载模型 → 输入测试图像 → 获得检测结果 → 验证结果非空且格式正确
7. IF 模型文件不可用，THEN THE Test_Suite SHALL 跳过端到端推理测试（通过文件存在性检测），不导致测试失败
8. THE Test_Suite SHALL 在 macOS Debug（ASan）构建下通过，无内存错误报告

### 需求 7：性能基线采集测试

**用户故事：** 作为开发者，我需要在 Pi 5 上采集 YOLOv11s 的推理性能基线，以便为 Spec 10 的抽帧间隔决策提供数据。

#### 验收标准

1. WHEN 模型文件可用时，THE Test_Suite SHALL 包含性能基线测试：分别对 YOLOv11s 和 YOLOv11n 执行 10 次推理，记录每次的 preprocess_ms、inference_ms、postprocess_ms、total_ms
2. THE 性能基线测试 SHALL 通过 spdlog（info 级别）输出两个模型的统计摘要：平均值、最小值、最大值，方便对比
3. THE 性能基线测试 SHALL 记录推理前后的进程 RSS 内存差值（通过 `/proc/self/status` 在 Linux 上、`task_info` 在 macOS 上获取）
4. IF 模型文件不可用，THEN THE 性能基线测试 SHALL 跳过，不导致测试失败
5. THE 性能基线数据 SHALL 记录到 `docs/development-trace.md`，不设 pass/fail 阈值

### 需求 8：双平台构建与测试验证

**用户故事：** 作为开发者，我需要确保 YOLO 检测模块在 macOS 和 Pi 5 上均能成功编译和测试。

#### 验收标准

1. WHEN 在 macOS 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试（包括现有 smoke_test、log_test、tee_test、camera_test、health_test 和新增的 yolo_test），ASan 不报告任何内存错误
2. WHEN 在 Pi 5 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试
3. WHEN ONNX Runtime 库未安装时，THE Build_System SHALL 通过 CMake option（如 `-DENABLE_YOLO=OFF`）跳过 YOLO 模块编译，不影响其他模块
4. THE Build_System SHALL 保持现有测试（smoke_test、log_test、tee_test、camera_test、health_test）全部通过，行为不变

## 参考代码

### ONNX Runtime C API 基本用法

```cpp
#include <onnxruntime_c_api.h>

const OrtApi* ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);

// 创建环境
OrtEnv* env = nullptr;
ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "yolo", &env);

// 创建会话选项
OrtSessionOptions* opts = nullptr;
ort->CreateSessionOptions(&opts);
ort->SetIntraOpNumThreads(opts, 2);

// 创建会话
OrtSession* session = nullptr;
ort->CreateSession(env, "model.onnx", opts, &session);

// 推理（简化）
OrtMemoryInfo* mem_info = nullptr;
ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);

// ... 创建输入张量、运行推理、读取输出 ...

// 释放资源（RAII 封装）
ort->ReleaseSession(session);
ort->ReleaseSessionOptions(opts);
ort->ReleaseEnv(env);
```

### Letterbox Resize 伪代码

```cpp
struct LetterboxInfo {
    float scale;      // min(640.0/w, 640.0/h)
    int pad_x, pad_y; // 填充偏移（像素）
    int new_w, new_h; // 缩放后的实际尺寸
};

LetterboxInfo letterbox(const uint8_t* src, int w, int h,
                        std::vector<float>& dst_nchw) {
    float scale = std::min(640.0f / w, 640.0f / h);
    int new_w = static_cast<int>(w * scale);
    int new_h = static_cast<int>(h * scale);
    int pad_x = (640 - new_w) / 2;
    int pad_y = (640 - new_h) / 2;
    // 1. 缩放 src → new_w × new_h
    // 2. 填充灰色(128) → 640 × 640
    // 3. 归一化 uint8 → float32 [0,1]
    // 4. HWC → NCHW 转换
    return {scale, pad_x, pad_y, new_w, new_h};
}
```

### NMS 伪代码

```cpp
std::vector<Detection> nms(std::vector<Detection>& dets, float iou_thresh) {
    // 按类别分组
    // 每组内按 confidence 降序排序
    // 贪心选择：保留最高 confidence 的框，抑制与其 IoU > iou_thresh 的框
    // 合并所有类别的结果
}
```

### 进程 RSS 内存获取

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

预期结果：两个平台均配置成功、编译无错误、所有测试通过（macOS 下 ASan 无报告）。模型文件不可用时端到端推理测试和性能基线测试自动跳过。

## 明确不包含

- GStreamer 管道集成（Spec 10: ai-pipeline，buffer probe 抽帧 + 检测）
- 抽帧策略与帧率控制（Spec 10）
- 截图上传 S3（Spec 11: screenshot-uploader）
- GPU/NPU 加速（Pi 5 无 CUDA，后续探索）
- XNNPACK / 自定义 execution provider 优化（基线采集后按需决定）
- OpenCV 依赖（letterbox resize 手写实现，保持零额外依赖）
- 模型热更新或运行时切换
- 模型训练或微调（属于 model 模块）
- 多模型并行推理
- 视频流处理（本 Spec 只处理单帧图像）
- 推理性能硬性指标（本 Spec 只采集基线，Spec 10 根据数据设定阈值）
