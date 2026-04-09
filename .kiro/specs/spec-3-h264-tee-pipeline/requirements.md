# 需求文档：Spec 3 — H.264 编码 + tee 三路分流

## 简介

本 Spec 是整个项目的关键转折点。目标是将当前基于 `gst_parse_launch` 的单路管道升级为手动构建的双 tee 分流管道：

- **Raw Tee**：在编码前分出一路 raw 帧给 AI 推理（后续 Spec 9/10 直接使用 raw 帧，无需解码回来）
- **Encoded Tee**：编码后分出两路 H.264 码流给 KVS 和 WebRTC

```
videotestsrc → videoconvert → capsfilter(I420) → raw-tee → queue → fakesink (ai, raw 帧)
                                                          → queue → x264enc(ultrafast/zerolatency) → h264parse → encoded-tee → queue → fakesink (kvs)
                                                                                                                             → queue → fakesink (webrtc)
```

三路分支使用 `fakesink` 占位，后续 Spec 将逐一替换为实际 sink（KVS Producer、WebRTC、AI 推理管道）。

设计决策 — 双 tee 而非单 tee：
- AI 推理需要 raw 帧（YUV/RGB），如果只在编码后分流，后续 Spec 9 需要加解码器将 H.264 解回 raw，白白浪费 CPU
- raw-tee 在编码前分流，AI 分支直接拿到 raw 帧，零额外开销
- KVS 和 WebRTC 都需要 H.264 码流，共享 encoded-tee 即可

`PipelineManager` 需要重构以支持手动构建管道（`gst_element_factory_make` + `gst_bin_add_many` + `gst_element_link`），同时保留现有的 `gst_parse_launch` 工厂方法以确保向后兼容（8 个 smoke test + 7 个 log test 依赖该接口）。

## 前置条件

- Spec 0（gstreamer-capture）✅ 已完成
- Spec 1（spdlog-logging）✅ 已完成
- Spec 2（cross-compile）已完成或可独立验证

## 术语表

- **PipelineManager**：GStreamer 管道生命周期管理器，封装 `GstElement*` 管道指针，提供创建、启动、停止接口，遵循 RAII 语义
- **gst_parse_launch**：GStreamer C API，接收管道描述字符串并创建对应的管道实例（现有工厂方法）
- **gst_element_factory_make**：GStreamer C API，按元素类型名创建单个 GstElement 实例
- **gst_bin_add_many**：GStreamer C API，将多个元素添加到 GstBin（管道容器）中
- **gst_element_link**：GStreamer C API，将两个元素的 src pad 和 sink pad 连接
- **tee**：GStreamer 元素，将单路输入复制为多路输出，每路通过 request pad 连接下游
- **queue**：GStreamer 元素，提供线程隔离的缓冲队列，防止下游阻塞影响上游或其他分支
- **x264enc**：基于 libx264 的软件 H.264 编码器，macOS 和 Linux 均可用。Pi 5（Bookworm）无硬件 H.264 编码支持，x264enc 是唯一可靠方案
- **raw-tee**：编码前的 tee 元素，分出 raw 帧给 AI 推理分支
- **encoded-tee**：编码后的 tee 元素，分出 H.264 码流给 KVS 和 WebRTC 分支
- **fakesink**：GStreamer 测试用 sink 元素，丢弃所有输入数据，适合自动化测试和占位
- **caps**：GStreamer Capabilities，描述媒体数据格式的元数据结构，用于元素间格式协商
- **request pad**：tee 元素的动态输出 pad，需要通过 `gst_element_request_pad_simple` 显式请求创建
- **GstBin**：GStreamer 容器元素，`GstPipeline` 是 `GstBin` 的子类，可包含多个子元素
- **videoconvert**：GStreamer 元素，执行视频色彩空间转换，确保上下游格式兼容
- **h264parse**：GStreamer 元素，解析 H.264 码流并设置正确的 caps 信息
- **ASan**：AddressSanitizer，编译器内存错误检测工具

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| GStreamer 依赖发现 | pkg-config（系统包） |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| Release 构建 | 不开启 ASan |
| 单个测试耗时 | ≤ 5 秒 |
| 代码组织 | .h + .cpp 分离模式，纯 POD 结构体可 header-only |
| 平台隔离 | `#ifdef __APPLE__` / `#ifdef __linux__` 条件编译 |
| GStreamer 资源管理 | RAII，所有 `gst_*_ref` 必须有对应 `gst_object_unref` |
| 向后兼容 | 现有 `PipelineManager::create(string)` 接口和所有现有测试必须继续通过 |
| 管道冷启动 | ≤ 2 秒（初始化到第一个 Buffer 到达 Sink） |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中引入真实摄像头输入（属于 Spec 4: camera-abstraction）
- SHALL NOT 在本 Spec 中实现管道健康监控或自动恢复（属于 Spec 5: pipeline-health）
- SHALL NOT 在本 Spec 中替换 fakesink 为实际 KVS/WebRTC/AI sink（属于 Spec 8/12/9）
- SHALL NOT 在本 Spec 中实现自适应码率控制（属于 Spec 14: adaptive-streaming）

### Design 层

- SHALL NOT 修改现有 `PipelineManager::create(const std::string&, std::string*)` 工厂方法的签名或行为，现有 smoke test 和 log test 依赖该接口
- SHALL NOT 修改现有已验证的 CMakeLists.txt 核心 target 定义（`pipeline_manager`、`raspi-eye`、`smoke_test`、`log_test`）
- SHALL NOT 在 macOS 上直接在 main() 中运行含 autovideosink 的 GStreamer 管道（用 `gst_macos_main()` 包装）
- SHALL NOT 在手动构建管道时遗漏 `gst_caps_unref`，GstCaps 设置到 capsfilter 后必须立即 unref（来源：Gemini review，GstCaps 泄漏是手动管道构建最常见的内存问题）

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 GStreamer 资源（除对接 C API 外），优先 RAII 封装

## 需求

### 需求 1：PipelineManager 手动构建管道支持

**用户故事：** 作为开发者，我需要 PipelineManager 支持通过手动组装 GStreamer 元素来构建管道，以便实现 tee 分流等 `gst_parse_launch` 难以表达的复杂拓扑结构。

#### 验收标准

1. THE PipelineManager SHALL 提供一个新的工厂方法，接收一个预先构建好的 `GstElement*` 管道（`GstPipeline`），返回 `std::unique_ptr<PipelineManager>` 实例
2. WHEN 传入有效的 `GstElement*` 管道时，THE PipelineManager SHALL 接管该管道的所有权，后续通过 RAII 语义管理其生命周期
3. WHEN 传入 `nullptr` 时，THE PipelineManager SHALL 返回 `nullptr` 并通过 `error_msg` 输出错误信息
4. THE PipelineManager SHALL 保留现有的 `create(const std::string&, std::string*)` 工厂方法，所有现有测试（8 个 smoke test + 7 个 log test）继续通过且行为不变
5. WHEN 通过新工厂方法创建的 PipelineManager 实例调用 `start()` 时，THE PipelineManager SHALL 将管道状态设置为 `GST_STATE_PLAYING`，行为与 `gst_parse_launch` 创建的实例一致
6. WHEN 通过新工厂方法创建的 PipelineManager 实例调用 `stop()` 或析构时，THE PipelineManager SHALL 释放所有 GStreamer 资源，ASan 不报告任何内存错误

### 需求 2：H.264 编码器运行时检测与配置

**用户故事：** 作为开发者，我需要管道在不同平台上自动选择合适的 H.264 编码器并配置低延迟参数，以便在 Pi 5 上控制 CPU 占用。

#### 验收标准

1. THE Encoder_Selector SHALL 通过 `gst_element_factory_find` 在运行时检测 `x264enc` 的可用性（Pi 5 Bookworm 无硬件 H.264 编码支持，`x264enc` 是主要方案）
2. WHEN `x264enc` 可用时，THE Encoder_Selector SHALL 创建编码器并设置 `tune=zerolatency`、`speed-preset=ultrafast`、`threads=2`，以最小化 Pi 5 上的 CPU 占用并预留核心给系统调度和后续 AI 推理
3. WHEN `x264enc` 不可用时，THE Encoder_Selector SHALL 返回错误信息，明确说明缺少可用的 H.264 编码器
4. THE Encoder_Selector SHALL 通过 spdlog 记录所选编码器的名称和关键参数（如 `"Selected encoder: x264enc (ultrafast, zerolatency, threads=2)"`）
5. THE Encoder_Selector SHALL 设计为可扩展的候选列表，后续如有硬件编码器可用（如未来固件更新），可通过在候选列表头部添加新编码器实现优先选择

### 需求 3：双 tee 三路分流管道构建

**用户故事：** 作为开发者，我需要构建一个双 tee 三路分流管道（raw-tee + encoded-tee），以便 AI 分支直接获取 raw 帧、KVS 和 WebRTC 分支获取 H.264 码流，避免后续 Spec 中不必要的编解码开销。

#### 验收标准

1. THE Pipeline_Builder SHALL 构建以下拓扑结构的管道：`videotestsrc → videoconvert → capsfilter(I420) → raw-tee → [ai 分支] + [编码链路 → encoded-tee → kvs 分支 + webrtc 分支]`
2. THE Pipeline_Builder SHALL 为 raw-tee 创建 2 个 request pad：一路连接 AI 分支（fakesink），一路连接编码链路（x264enc → h264parse → encoded-tee）
3. THE Pipeline_Builder SHALL 为 encoded-tee 创建 2 个 request pad：分别连接 KVS 分支和 WebRTC 分支（均为 fakesink）
4. WHEN 管道构建完成时，THE Pipeline_Builder SHALL 为每个分支的 fakesink 设置可识别的名称属性（`"kvs-sink"`、`"webrtc-sink"`、`"ai-sink"`）
5. THE Pipeline_Builder SHALL 在每个 tee request pad 与对应下游元素之间插入一个 `queue` 元素，实现线程隔离
6. THE Pipeline_Builder SHALL 为每个 queue 设置 `max-size-buffers=1`，最小化内存占用和线程开销
7. THE Pipeline_Builder SHALL 为 AI 分支和 WebRTC 分支的 queue 设置 `leaky=downstream`，防止下游阻塞时 CPU 积压拖慢整个管道（KVS 分支不设 leaky，保证录制完整性）
8. WHEN 管道启动后，THE Pipeline_Builder 构建的管道 SHALL 在 2 秒内达到 `GST_STATE_PLAYING` 状态
9. WHEN 管道运行时，THE Pipeline_Builder 构建的三个分支 SHALL 各自独立运行，一个分支的阻塞不影响其他分支的数据流转

### 需求 4：caps 协商链路

**用户故事：** 作为开发者，我需要管道各元素之间的 caps 正确协商，以确保视频数据从源到编码器到 tee 到各分支的格式转换链路畅通。

#### 验收标准

1. THE Pipeline_Builder SHALL 通过 capsfilter 强制 `video/x-raw,format=I420` 格式，使 videoconvert 尽可能 pass-through（零转换），最小化 CPU 开销
2. THE Pipeline_Builder SHALL 在 videoconvert 和 raw-tee 之间设置 capsfilter，确保输出格式为 `video/x-raw,format=I420`
3. THE Pipeline_Builder SHALL 在 h264parse 和 encoded-tee 之间确保 caps 为 `video/x-h264` 格式
4. WHEN 管道启动后，THE Pipeline_Builder 构建的管道 SHALL 成功完成所有元素间的 caps 协商，不产生 caps negotiation 错误
5. IF caps 协商失败，THEN THE Pipeline_Builder SHALL 通过 spdlog 记录失败的元素名称和期望的 caps 信息

### 需求 5：main.cpp 管道升级

**用户故事：** 作为开发者，我需要将 main.cpp 中的简单测试管道升级为 H.264 + tee 三路分流管道，以便在实际运行时验证完整管道拓扑。

#### 验收标准

1. THE Main_Entry SHALL 使用需求 3 中定义的 Pipeline_Builder 构建双 tee 三路分流管道，替换当前的 `"videotestsrc ! videoconvert ! autovideosink"` 管道
2. WHEN 管道构建失败时，THE Main_Entry SHALL 通过 spdlog 记录错误信息并以非零退出码退出
3. THE Main_Entry SHALL 继续使用 GMainLoop 事件循环处理 bus 消息（ERROR、EOS）
4. WHEN 在 macOS 上运行时，THE Main_Entry SHALL 继续使用 `gst_macos_main()` 包装管道运行逻辑
5. WHEN 在 Linux 上运行时，THE Main_Entry SHALL 直接运行管道逻辑

### 需求 6：双平台构建验证

**用户故事：** 作为开发者，我需要确保 H.264 + tee 管道在 macOS 和 Pi 5 上均能成功编译和运行，以便尽早发现平台兼容性问题。

#### 验收标准

1. WHEN 在 macOS 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误和警告（ASan 相关警告除外）
2. WHEN 在 Pi 5 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误
3. WHEN 在 macOS 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试（包括现有 smoke test、log test 和新增的 tee 管道测试），ASan 不报告任何内存错误
4. WHEN 在 Pi 5 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试

### 需求 7：双 tee 管道冒烟测试

**用户故事：** 作为开发者，我需要自动化测试来验证双 tee 三路分流管道的核心功能，以便在每次构建后快速确认管道拓扑正确。

#### 验收标准

1. THE Test_Suite SHALL 验证：通过新工厂方法传入手动构建的管道后，PipelineManager 成功接管并可启动
2. THE Test_Suite SHALL 验证：双 tee 管道启动后达到 `GST_STATE_PLAYING` 状态
3. THE Test_Suite SHALL 验证：管道中存在名为 `"kvs-sink"`、`"webrtc-sink"`、`"ai-sink"` 的三个 fakesink 元素
4. THE Test_Suite SHALL 验证：管道停止后所有资源正确释放，ASan 不报告任何内存错误
5. THE Test_Suite SHALL 验证：通过新工厂方法创建的 PipelineManager 实例离开作用域后，RAII 析构正确释放所有 GStreamer 资源
6. THE Test_Suite SHALL 仅使用 `fakesink` 作为 sink 元素，不使用需要显示设备的 sink
7. WHEN 单个测试执行时，THE Test_Suite SHALL 在 5 秒内完成
8. THE Test_Suite SHALL 验证：编码器运行时检测逻辑在当前平台上成功选择一个可用的 H.264 编码器并正确配置参数（ultrafast、zerolatency）

## 验证命令

```bash
# macOS Debug 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# Pi 5 Release 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```

预期结果：两个平台均配置成功、编译无错误、所有测试通过（macOS 下 ASan 无报告）。

## 明确不包含

- 真实摄像头输入（Spec 4: camera-abstraction）
- 管道健康监控与自动恢复（Spec 5: pipeline-health）
- AWS 基础设施（Spec 6: infra-core）
- 实际 KVS Producer sink 替换（Spec 8: kvs-producer）
- 实际 AI 推理管道替换（Spec 9: yolo-detector / Spec 10: ai-pipeline）
- 实际 WebRTC sink 替换（Spec 12: webrtc-signaling / Spec 13: webrtc-media）
- 自适应码率控制（Spec 14: adaptive-streaming）
- 零拷贝缓冲区优化（Spec 15: zero-copy-buffers）
