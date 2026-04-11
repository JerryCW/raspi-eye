# 需求文档：Spec 4.5 — 摄像头源管道增强（Camera Source V2）

## 简介

Spec-4 实现了摄像头抽象层（CameraSource），支持 TEST/V4L2/LIBCAMERA 三种类型。Pi 5 端到端验证发现三个问题：

1. **V4L2 格式不匹配**：IMX678 USB 摄像头只输出 MJPG 格式，v4l2src 默认尝试 YUYV/I420 协商失败。需要查询设备支持的格式列表，根据格式选择最优的源管道段（MJPG 需要 jpegdec 解码链）。
2. **设备路径不稳定**：默认设备 `/dev/video0` 在 Pi 5 上指向 CSI 的 `rp1-cfe` 驱动而不是 USB 摄像头，设备编号不稳定。V4L2 类型必须显式指定 `--device` 路径（支持 udev symlink 如 `/dev/IMX678`）。
3. **Source Bin 封装**：当前 `create_source` 只返回单个 GstElement*，无法表达包含解码链的源管道段（如 MJPG 需要 v4l2src + capsfilter + jpegdec）。

本 Spec 增强 `CameraSource::create_source` 的返回值，使其返回一个包含完整源管道段的 GstBin（内含 v4l2src + capsfilter + jpegdec 等必要元素），对外暴露统一的 src pad，可直接连接到 `videoconvert`。

## 前置条件

- Spec 4（camera-abstraction）✅ 已完成
- Spec 13.5（main-integration）✅ 已完成

## 术语表

- **CameraSource**：摄像头抽象层，负责根据配置创建对应的 GStreamer 视频源管道段
- **Source_Bin**：封装视频源及其必要解码/转换链的 GstBin，对外暴露单个 src ghost pad，可直接连接到 videoconvert
- **V4L2_Format_Probe**：查询 V4L2 设备支持的像素格式列表的操作，通过 GStreamer caps query 或 v4l2 ioctl 实现
- **MJPG**：Motion-JPEG 压缩格式，IMX678 USB 摄像头的唯一输出格式，需要 jpegdec 解码后才能进入 videoconvert
- **YUYV**：YUV 4:2:2 未压缩格式，部分 USB 摄像头的原生输出格式
- **I420**：YUV 4:2:0 planar 格式，x264enc 编码器要求的输入格式，管道 capsfilter 的目标格式
- **jpegdec**：GStreamer JPEG 解码元素，将 MJPG 帧解码为 raw 视频帧
- **udev_symlink**：Linux udev 规则创建的设备符号链接（如 `/dev/IMX678`），提供稳定的设备路径
- **Format_Priority**：格式选择优先级规则：raw I420 > YUYV > MJPG，优先选择无需解码的格式以减少 CPU 开销
- **Ghost_Pad**：GstBin 上的代理 pad，将 bin 内部元素的 pad 暴露给外部，使 bin 可以像单个元素一样连接
- **IMX678**：Sony IMX678 图像传感器（Sunplus DECXIN Camera），通过 USB 接入 Pi 5，只输出 MJPG，主力摄像头
- **IMX216**：Sony IMX216 图像传感器，通过 CSI 接口接入 Pi 5，备用摄像头

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| 单个测试耗时 | ≤ 5 秒 |
| 代码组织 | .h + .cpp 分离模式 |
| 平台隔离 | `#ifdef __APPLE__` / `#ifdef __linux__` 条件编译 |
| GStreamer 资源管理 | RAII，所有引用计数必须配对 |
| 向后兼容 | 不修改现有测试文件，不修改 build_tee_pipeline 的函数签名 |
| 管道拓扑不变 | build_tee_pipeline 的管道结构（双 tee 三路分流）保持不变，仅替换视频源创建部分 |
| create_source 返回值 | 返回的 GstElement* 必须能直接连接到 videoconvert（或已经包含了必要的解码链） |
| MJPG 默认分辨率 | MJPG 源的 capsfilter 默认限制为 1920x1080@15fps，避免 4K MJPG 解码打满 CPU |
| 新增代码量 | 100-500 行 |
| 涉及文件 | 2-5 个 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现多摄像头同时运行（当前阶段单摄像头）
- SHALL NOT 在本 Spec 中实现运行时热切换摄像头（切换需要重启管道）
- SHALL NOT 在本 Spec 中实现摄像头分辨率/帧率的动态调整（属于 Spec 14 自适应码率范畴）
- SHALL NOT 在本 Spec 中实现 V4L2 设备自动枚举与发现（用户通过 --device 显式指定）
- SHALL NOT 在本 Spec 中实现 config.toml [camera] 段配置（推迟到 Spec 18: config-file）

### Design 层

- SHALL NOT 修改 PipelineManager 的任何公开接口（create、start、stop、current_state、pipeline）
- SHALL NOT 修改 build_tee_pipeline 的函数签名（参数列表和返回类型保持不变）
- SHALL NOT 修改 build_tee_pipeline 的管道拓扑结构（双 tee 三路分流），仅替换视频源创建部分
- SHALL NOT 在 macOS 上引入 V4L2 或 libcamera 的平台头文件或链接库
- SHALL NOT 在手动构建管道时遗漏 GStreamer 引用计数释放
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 GStreamer C API 外）
- SHALL NOT 修改任何现有测试文件（camera_test.cpp、tee_test.cpp、smoke_test.cpp 等）

## 需求

### 需求 1：V4L2 格式自动检测

**用户故事：** 作为开发者，我需要 V4L2 源自动检测摄像头支持的像素格式，以便根据格式选择最优的源管道段，避免 caps 协商失败。

#### 验收标准

1. WHEN CameraType 为 V4L2 时，THE CameraSource 模块 SHALL 查询指定设备支持的像素格式列表
2. THE CameraSource 模块 SHALL 按以下优先级选择格式：raw I420 优先，YUYV 次之，MJPG 最低（减少解码开销）
3. WHEN 设备仅支持 MJPG 格式（如 IMX678 USB）时，THE CameraSource 模块 SHALL 构建包含 `v4l2src ! capsfilter(image/jpeg,width=1920,height=1080,framerate=15/1) ! jpegdec` 的源管道段
4. WHEN 设备支持 YUYV 或其他 raw 格式时，THE CameraSource 模块 SHALL 构建仅包含 `v4l2src` 的源管道段（由下游 videoconvert 完成格式转换）
5. IF 格式查询失败（如设备不存在或无权限），THEN THE CameraSource 模块 SHALL 返回 nullptr 并通过 error_msg 输出包含设备路径的错误信息
6. THE CameraSource 模块 SHALL 通过 spdlog 记录检测到的格式列表和最终选择的格式（如 `"V4L2 device /dev/IMX678: detected formats [MJPG], selected MJPG -> jpegdec pipeline"`）

### 需求 2：Source Bin 封装

**用户故事：** 作为开发者，我需要 create_source 返回一个封装了完整源管道段的 GstBin，以便 MJPG 解码链等内部细节对 build_tee_pipeline 透明，保持管道构建代码不变。

#### 验收标准

1. THE CameraSource::create_source 函数 SHALL 返回一个 GstBin（命名为 `"src"`），内部包含视频源元素及其必要的解码/转换链
2. THE Source_Bin SHALL 暴露一个名为 `"src"` 的 ghost pad，连接到 bin 内最后一个元素的 src pad
3. WHEN CameraType 为 V4L2 且设备仅支持 MJPG 时，THE Source_Bin SHALL 内部包含 `v4l2src → capsfilter(image/jpeg,width=1920,height=1080,framerate=15/1) → jpegdec` 三个元素
4. WHEN CameraType 为 V4L2 且设备支持 raw 格式时，THE Source_Bin SHALL 内部仅包含 `v4l2src` 一个元素
5. WHEN CameraType 为 TEST 时，THE Source_Bin SHALL 内部仅包含 `videotestsrc` 一个元素
6. WHEN CameraType 为 LIBCAMERA 时，THE Source_Bin SHALL 内部仅包含 `libcamerasrc` 一个元素（libcamerasrc 直接输出 raw 帧）
7. THE Source_Bin 返回的 GstElement* SHALL 能直接通过 `gst_element_link` 连接到 `videoconvert`，与当前 build_tee_pipeline 的链接逻辑兼容

### 需求 3：多摄像头设备路径支持

**用户故事：** 作为开发者，我需要通过 `--device` 参数指定任意设备路径（包括 udev symlink），以便在多摄像头环境中稳定选择目标摄像头。

#### 验收标准

1. THE Main_Entry SHALL 支持 `--device /dev/IMX678` 形式的 udev symlink 路径
2. THE Main_Entry SHALL 支持 `--device /dev/video4` 形式的标准设备节点路径
3. WHEN 未提供 `--device` 参数且 CameraType 为 V4L2 时，THE Main_Entry SHALL 报错退出并提示用户必须通过 `--device` 指定设备路径（Pi 5 上 `/dev/video0` 可能指向 CSI 而非 USB 摄像头，默认值不安全）
4. THE CameraSource 模块 SHALL 将 `--device` 指定的路径直接传递给 v4l2src 的 `device` 属性，不做路径解析或验证（由 v4l2src 在运行时验证）
5. WHEN `--device` 指定的路径不存在或无权限时，THE CameraSource 模块 SHALL 在格式查询阶段返回 nullptr 并通过 error_msg 输出包含设备路径的错误信息

### 需求 4：libcamerasrc CSI 支持（可选）

**用户故事：** 作为开发者，我需要 `--camera libcamera` 正确创建 libcamerasrc 源管道段，以便在 Pi 5 上使用 IMX216 CSI 摄像头。

#### 验收标准

1. WHEN CameraType 为 LIBCAMERA 时，THE CameraSource 模块 SHALL 创建包含 `libcamerasrc` 的 Source_Bin
2. THE libcamerasrc Source_Bin SHALL 不包含 jpegdec（libcamerasrc 直接输出 raw 帧，由下游 videoconvert 处理）
3. IF libcamerasrc GStreamer 插件不存在（如 macOS 上），THEN THE CameraSource 模块 SHALL 返回 nullptr 并通过 error_msg 输出错误信息

_注：本需求为可选（MVP 优先确保 V4L2 USB 路径跑通）。libcamerasrc 在 Pi 5 Bookworm 上可能需要额外的 media pipeline 配置，如果端到端验证遇到问题可推迟到后续 Spec。_

### 需求 5：向后兼容与回归防护

**用户故事：** 作为开发者，我需要确保本次增强不破坏现有功能，所有现有测试零修改通过。

#### 验收标准

1. WHEN CameraType 为 TEST 时，THE CameraSource 模块 SHALL 创建包含 videotestsrc 的 Source_Bin，行为与 Spec 4 一致
2. THE build_tee_pipeline 函数 SHALL 保持函数签名不变，现有调用点（tee_test.cpp、app_context.cpp）无需修改
3. WHEN 使用默认 CameraConfig 调用 build_tee_pipeline 时，THE PipelineBuilder SHALL 成功构建管道并达到 PLAYING 状态
4. WHEN 在 macOS 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有现有测试（smoke_test、log_test、tee_test、camera_test 等），ASan 不报告任何内存错误
5. WHEN 在 Pi 5 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有现有测试
6. THE Source_Bin 的 ghost pad 命名和 bin 命名 SHALL 确保 build_tee_pipeline 中 `gst_element_link_many(src, convert, capsfilter, raw_tee, nullptr)` 链接调用无需修改

## 已知风险

### 风险 1：GstBin ghost pad 与 HealthMonitor probe 兼容性

spec-5 的 PipelineHealthMonitor 在 rebuild 时通过 `gst_bin_get_by_name(pipeline, "src")` 查找源元素并安装 buffer probe。如果 src 变成 GstBin，probe 安装在 ghost pad 上，行为可能与直接安装在元素 src pad 上不同。

**缓解措施：** 在 Pi 5 端到端验证时确认 HealthMonitor 的 probe 和 rebuild 回调正常工作。如果 ghost pad probe 有问题，可以在 Source_Bin 内部暴露一个额外的 `"probe-pad"` 供 HealthMonitor 使用。

### 风险 2：MJPG 解码 CPU 开销

IMX678 支持 3840x2160@60fps MJPG。如果 capsfilter 没有限制分辨率/帧率，jpegdec 解码 4K@60fps 会把 Pi 5 的 CPU 打满。

**缓解措施：** MJPG 源的 capsfilter 默认限制为 1920x1080@15fps。后续 spec-14（adaptive-streaming）可根据实际 CPU 负载动态调整。

### 风险 3：现有测试中 `gst_bin_get_by_name` 语义变化

`camera_test.cpp` 中 `TeePipelineSourceElement` 测试用 `gst_bin_get_by_name(GST_BIN(raw), "src")` 查找 src 元素。如果 src 变成嵌套 GstBin，`gst_bin_get_by_name` 会返回 bin 本身（因为 bin 名为 "src"），测试逻辑不变但返回的是 GstBin 而不是 videotestsrc 元素。

**缓解措施：** 测试断言只检查 `src != nullptr`，不检查元素类型，所以不受影响。

### 风险 4：libcamerasrc 在 Pi 5 上的可用性

libcamerasrc 需要 libcamera daemon 和正确的 media pipeline 配置。Pi 5 Bookworm 上 libcamerasrc 是否开箱即用未经验证。

**缓解措施：** 需求 4（libcamerasrc）标记为可选，MVP 优先确保 V4L2 USB 路径跑通。

## 验证命令

```bash
# macOS Debug 构建 + 测试（验证向后兼容）
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# Pi 5 Release 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# Pi 5 上指定 USB 摄像头运行（IMX678 MJPG 自动检测）
./device/build/raspi-eye --camera v4l2 --device /dev/IMX678 --config device/config/config.toml

# Pi 5 上指定 CSI 摄像头运行（可选）
./device/build/raspi-eye --camera libcamera --config device/config/config.toml
```

预期结果：两个平台均配置成功、编译无错误、所有现有测试通过（macOS 下 ASan 无报告）。Pi 5 上 IMX678 USB 摄像头通过 MJPG→jpegdec 管道正常采集。

## 明确不包含

- 多摄像头同时运行（后期扩展）
- 运行时热切换摄像头（Spec 5: pipeline-health 管道重建能力）
- 摄像头分辨率/帧率动态调整（Spec 14: adaptive-streaming）
- V4L2 设备自动枚举与发现（用户通过 --device 显式指定）
- libcamera 特定参数配置（如曝光、白平衡，可作为后续增强）
- config.toml [camera] 段配置（Spec 18: config-file 统一处理）
- config.toml 的完整配置文件加载框架（Spec 18: config-file 统一处理）
