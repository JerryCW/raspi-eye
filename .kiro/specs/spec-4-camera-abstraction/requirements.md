# 需求文档：Spec 4 — 摄像头接口抽象层

## 简介

本 Spec 将 `build_tee_pipeline()` 中硬编码的 `videotestsrc` 视频源解耦为可配置的摄像头抽象层，支持三种视频源的灵活切换：

- **videotestsrc** — macOS 开发环境（唯一可用源）
- **v4l2src** — IMX678 USB 摄像头（Pi 5 主力）
- **libcamerasrc** — IMX216 CSI 摄像头（Pi 5 备用）

当前 `build_tee_pipeline()` 硬编码 `videotestsrc` 作为视频源。本 Spec 引入 `CameraSource` 抽象，通过配置或平台检测选择合适的视频源，并将创建好的 `GstElement*` 注入 `build_tee_pipeline()`，使管道构建器与具体视频源解耦。

设计目标：
- macOS 上自动使用 `videotestsrc`（唯一可用源）
- Pi 5 上默认使用 `v4l2src`（IMX678 USB 主力），可切换到 `libcamerasrc`（IMX216 CSI 备用）
- 不修改 `PipelineManager` 核心接口
- 不修改 `PipelineBuilder::build_tee_pipeline()` 的管道拓扑结构（仅替换视频源创建部分）

## 前置条件

- Spec 0（gstreamer-capture）✅ 已完成
- Spec 1（spdlog-logging）✅ 已完成
- Spec 2（cross-compile）✅ 已完成
- Spec 3（h264-tee-pipeline）✅ 已完成

## 术语表

- **CameraSource**：摄像头抽象层，负责根据配置创建对应的 GStreamer 视频源元素
- **videotestsrc**：GStreamer 测试视频源元素，生成合成测试图案，macOS 开发环境使用
- **v4l2src**：GStreamer Video4Linux2 视频源元素，通过 V4L2 API 接入 USB 摄像头，Linux 专用
- **libcamerasrc**：GStreamer libcamera 视频源元素，通过 libcamera API 接入 CSI 摄像头，Linux 专用
- **IMX678**：Sony IMX678 图像传感器，通过 USB 接入 Pi 5，主力摄像头，使用 v4l2src
- **IMX216**：Sony IMX216 图像传感器，通过 CSI 接口接入 Pi 5，备用摄像头，使用 libcamerasrc
- **PipelineBuilder**：双 tee 管道构建器（Spec 3 交付），当前硬编码 videotestsrc
- **PipelineManager**：管道生命周期管理器（Spec 0/3 交付），RAII 语义管理 GstElement*
- **CameraType**：枚举类型，表示三种视频源：TEST（videotestsrc）、V4L2（v4l2src）、LIBCAMERA（libcamerasrc）

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| 单个测试耗时 | ≤ 5 秒 |
| 代码组织 | .h + .cpp 分离模式，纯 POD 结构体（如 CameraConfig）可 header-only |
| 平台隔离 | `#ifdef __APPLE__` / `#ifdef __linux__` 条件编译 |
| GStreamer 资源管理 | RAII，所有引用计数必须配对 |
| 向后兼容 | 不修改 PipelineManager 核心接口，不修改现有测试 |
| 管道拓扑不变 | build_tee_pipeline 的管道结构（双 tee 三路分流）保持不变，仅替换视频源创建部分 |
| 新增代码量 | 100-500 行 |
| 涉及文件 | 2-5 个 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现多摄像头同时运行（当前阶段单摄像头，后期扩展）
- SHALL NOT 在本 Spec 中实现运行时热切换摄像头（切换需要重启管道，属于 Spec 5 管道健康监控范畴）
- SHALL NOT 在本 Spec 中实现摄像头分辨率/帧率的动态调整（属于 Spec 14 自适应码率范畴）

### Design 层

- SHALL NOT 修改 PipelineManager 的任何公开接口（create、start、stop、current_state、pipeline）
- SHALL NOT 修改 build_tee_pipeline 的管道拓扑结构（双 tee 三路分流），仅替换视频源创建部分
- SHALL NOT 在 macOS 上引入 V4L2 或 libcamera 的平台头文件或链接库（GStreamer 工厂调用 `gst_element_factory_make` 在插件不存在时安全返回 nullptr，不需要条件编译阻止调用）
- SHALL NOT 在手动构建管道时遗漏 GStreamer 引用计数释放
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 GStreamer C API 外）

## 需求

### 需求 1：摄像头类型枚举与配置结构

**用户故事：** 作为开发者，我需要一个类型安全的摄像头配置结构，以便在代码中明确指定使用哪种视频源，避免字符串拼写错误。

#### 验收标准

1. THE CameraSource 模块 SHALL 定义 `CameraType` 枚举，包含三个值：`TEST`（videotestsrc）、`V4L2`（v4l2src）、`LIBCAMERA`（libcamerasrc）
2. THE CameraSource 模块 SHALL 定义 `CameraConfig` 结构体，包含 `CameraType type` 字段和 `std::string device` 字段（v4l2src 的设备路径，如 `/dev/video0`）
3. THE CameraSource 模块 SHALL 提供 `default_camera_type()` 函数，在 macOS 上返回 `CameraType::TEST`，在 Linux 上返回 `CameraType::V4L2`
4. THE CameraSource 模块 SHALL 提供 `camera_type_name(CameraType)` 函数，返回对应的人类可读名称（如 `"videotestsrc"`、`"v4l2src"`、`"libcamerasrc"`），用于日志输出

### 需求 2：视频源元素创建

**用户故事：** 作为开发者，我需要根据 CameraConfig 创建对应的 GStreamer 视频源元素，以便将不同摄像头统一为相同的 GstElement* 接口注入管道。

#### 验收标准

1. THE CameraSource 模块 SHALL 提供 `create_source(const CameraConfig&, std::string*)` 函数，根据配置创建对应的 GStreamer 视频源元素并返回 `GstElement*`
2. WHEN `CameraType` 为 `TEST` 时，THE create_source 函数 SHALL 创建 `videotestsrc` 元素，元素名称为 `"src"`
3. WHEN `CameraType` 为 `V4L2` 时，THE create_source 函数 SHALL 创建 `v4l2src` 元素，元素名称为 `"src"`，并设置 `device` 属性为 `CameraConfig::device` 的值（默认 `/dev/video0`）
4. WHEN `CameraType` 为 `LIBCAMERA` 时，THE create_source 函数 SHALL 创建 `libcamerasrc` 元素，元素名称为 `"src"`
5. IF 请求的 GStreamer 元素工厂不存在（如 macOS 上请求 v4l2src），THEN THE create_source 函数 SHALL 返回 `nullptr` 并通过 `error_msg` 输出错误信息，包含请求的元素类型名称
6. THE create_source 函数 SHALL 通过 spdlog 记录所创建的视频源类型（如 `"Camera source created: v4l2src (device=/dev/video0)"`）

### 需求 3：管道构建器集成

**用户故事：** 作为开发者，我需要 build_tee_pipeline 使用 CameraSource 抽象层创建视频源，以便通过配置切换摄像头而无需修改管道构建代码。

#### 验收标准

1. THE PipelineBuilder::build_tee_pipeline 函数 SHALL 接受一个带默认值的 `CameraConfig` 参数（默认参数而非重载，确保现有无参调用点和 tee_test 无需修改）
2. WHEN 调用方未传入 `CameraConfig` 参数时，THE build_tee_pipeline 函数 SHALL 使用 `default_camera_type()` 构造的默认配置（macOS 上为 videotestsrc，Linux 上为 v4l2src）
3. THE build_tee_pipeline 函数 SHALL 调用 `CameraSource::create_source()` 创建视频源元素，替换当前硬编码的 `gst_element_factory_make("videotestsrc", "src")`
4. WHEN CameraSource::create_source() 返回 nullptr 时，THE build_tee_pipeline 函数 SHALL 返回 nullptr 并通过 error_msg 输出错误信息
5. THE build_tee_pipeline 函数 SHALL 保持管道拓扑结构不变：`[视频源] → videoconvert → capsfilter(I420) → raw-tee → [三路分流]`

### 需求 4：平台条件编译隔离

**用户故事：** 作为开发者，我需要确保 Linux 专用的摄像头代码不会在 macOS 上编译，以避免缺少头文件或库的编译错误。

#### 验收标准

1. WHILE 在 macOS 上编译时，THE CameraSource 模块 SHALL 不引入 V4L2 或 libcamera 相关的平台头文件或链接库（GStreamer 工厂调用本身不需要条件编译，`gst_element_factory_make` 在插件不存在时安全返回 nullptr）
2. WHILE 在 Linux 上编译时，THE CameraSource 模块 SHALL 编译所有三种视频源的创建代码，可按需引入 V4L2/libcamera 平台头文件
3. THE default_camera_type() 函数 SHALL 通过 `#ifdef __APPLE__` / `#ifdef __linux__` 条件编译返回平台对应的默认值
4. WHEN 在 macOS 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误
5. WHEN 在 Pi 5 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误

### 需求 5：main.cpp 摄像头配置集成

**用户故事：** 作为开发者，我需要通过命令行参数指定摄像头类型，以便在 Pi 5 上灵活切换 IMX678（USB）和 IMX216（CSI）。

#### 验收标准

1. THE Main_Entry SHALL 支持 `--camera` 命令行参数，接受 `test`、`v4l2`、`libcamera` 三个值
2. THE Main_Entry SHALL 支持 `--device` 命令行参数，指定 v4l2src 的设备路径（如 `--device /dev/video0`）
3. WHEN 未提供 `--camera` 参数时，THE Main_Entry SHALL 使用 `default_camera_type()` 返回的平台默认值
4. WHEN 提供无效的 `--camera` 值时，THE Main_Entry SHALL 通过 spdlog 记录错误信息并以非零退出码退出
5. THE Main_Entry SHALL 将解析后的 CameraConfig 传递给 `build_tee_pipeline()`
6. WHEN `--camera` 不是 `v4l2` 但提供了 `--device` 参数时，THE Main_Entry SHALL 通过 spdlog warn 提示 `--device` 参数被忽略（仅 v4l2src 使用设备路径）
7. THE Main_Entry SHALL 通过 spdlog 记录启动时使用的摄像头类型（如 `"Starting with camera: v4l2src (device=/dev/video0)"`）

### 需求 6：双平台构建与测试验证

**用户故事：** 作为开发者，我需要确保摄像头抽象层在 macOS 和 Pi 5 上均能成功编译和测试，以便尽早发现平台兼容性问题。

#### 验收标准

1. WHEN 在 macOS 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试（包括现有 smoke test、log test、tee test 和新增的 camera source 测试），ASan 不报告任何内存错误
2. WHEN 在 Pi 5 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试
3. THE Test_Suite SHALL 验证：在当前平台上 `default_camera_type()` 返回正确的默认值（macOS 返回 TEST，Linux 返回 V4L2）
4. THE Test_Suite SHALL 验证：`create_source()` 使用 `CameraType::TEST` 配置时成功创建 videotestsrc 元素（双平台均可用）
5. THE Test_Suite SHALL 验证：在 macOS 上 `create_source()` 使用 `CameraType::V4L2` 或 `CameraType::LIBCAMERA` 配置时返回 nullptr 并输出错误信息（Pi 5 上三种均可用，此测试通过 `#ifdef __APPLE__` 条件编译仅在 macOS 上执行）
6. THE Test_Suite SHALL 验证：使用默认 CameraConfig 调用 `build_tee_pipeline()` 后管道成功启动并达到 PLAYING 状态
7. THE Test_Suite SHALL 验证：使用显式 `CameraType::TEST` 配置调用 `build_tee_pipeline()` 后管道成功启动
8. THE Test_Suite SHALL 仅使用 `fakesink` 作为 sink 元素，不使用需要显示设备的 sink
9. WHEN 单个测试执行时，THE Test_Suite SHALL 在 5 秒内完成

## 验证命令

```bash
# macOS Debug 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# Pi 5 Release 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# Pi 5 上指定 USB 摄像头运行
./device/build/raspi-eye --camera v4l2 --device /dev/video0

# Pi 5 上指定 CSI 摄像头运行
./device/build/raspi-eye --camera libcamera
```

预期结果：两个平台均配置成功、编译无错误、所有测试通过（macOS 下 ASan 无报告）。

## 明确不包含

- 多摄像头同时运行（后期扩展）
- 运行时热切换摄像头（Spec 5: pipeline-health 管道重建能力）
- 摄像头分辨率/帧率动态调整（Spec 14: adaptive-streaming）
- 管道健康监控与自动恢复（Spec 5: pipeline-health）
- V4L2 设备枚举与自动发现（可作为后续增强）
- libcamera 特定参数配置（如曝光、白平衡，可作为后续增强）
