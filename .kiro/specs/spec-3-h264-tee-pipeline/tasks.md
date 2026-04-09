# 实施计划：Spec 3 — H.264 编码 + tee 三路分流

## 概述

将当前基于 `gst_parse_launch` 的单路管道升级为手动构建的双 tee 分流管道。按依赖顺序：PipelineManager 新工厂方法 → PipelineBuilder 实现 → CMakeLists.txt 更新 → tee_test 冒烟测试 → main.cpp 升级 → 双平台验证 → 最终检查点。实现语言为 C++17，不涉及 PBT。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 GStreamer 资源（除对接 C API 外），优先 RAII 封装
- SHALL NOT 修改现有 `PipelineManager::create(const std::string&, std::string*)` 签名或行为
- SHALL NOT 修改现有 CMakeLists.txt 核心 target 定义（`pipeline_manager`、`raspi-eye`、`smoke_test`、`log_test`）
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 遗漏 `gst_caps_unref`，GstCaps 设置到 capsfilter 后必须立即 unref

## 任务

- [x] 1. PipelineManager 新工厂方法
  - [x] 1.1 提取 `ensure_gst_init()` 私有静态方法
    - 在 `device/src/pipeline_manager.h` 中添加 `static bool ensure_gst_init(std::string* error_msg)` 私有声明
    - 在 `device/src/pipeline_manager.cpp` 中将现有 `create(string)` 里的 GStreamer 初始化逻辑提取到 `ensure_gst_init()`
    - 现有 `create(string)` 改为调用 `ensure_gst_init()`，行为不变
    - _需求：1.4_
  - [x] 1.2 添加 `create(GstElement*, std::string*)` 工厂方法重载
    - 在 `device/src/pipeline_manager.h` 中添加新工厂方法声明
    - 在 `device/src/pipeline_manager.cpp` 中实现：nullptr 检查 → `ensure_gst_init()` → spdlog 日志 → 构造 PipelineManager
    - 传入 nullptr 时返回 nullptr 并设置 error_msg `"Pipeline pointer is null"`
    - 传入有效 `GstElement*` 时接管所有权，返回 `unique_ptr<PipelineManager>`
    - _需求：1.1, 1.2, 1.3, 1.5, 1.6_

- [x] 2. PipelineBuilder 实现
  - [x] 2.1 创建 `device/src/pipeline_builder.h`
    - 声明 `namespace PipelineBuilder`，包含 `GstElement* build_tee_pipeline(std::string* error_msg = nullptr)` 函数
    - 头文件注释说明管道拓扑结构
    - _需求：3.1_
  - [x] 2.2 创建 `device/src/pipeline_builder.cpp` — 编码器检测与辅助函数
    - 实现匿名命名空间内的 `EncoderCandidate` 结构体和 `kEncoderCandidates` 候选列表
    - 实现 `create_encoder(std::string*)` 函数：遍历候选列表，`gst_element_factory_find` 检测可用性，创建 `x264enc` 并设置 `tune=zerolatency`、`speed-preset=ultrafast`、`threads=2`，通过 spdlog 记录所选编码器
    - 实现 `link_tee_to_element(GstElement* tee, GstElement* element, std::string*)` 辅助函数：请求 tee pad → 获取下游 sink pad → 链接 → 释放两个 pad 引用
    - `gst_element_factory_find` 返回的引用必须 `gst_object_unref`
    - _需求：2.1, 2.2, 2.3, 2.4, 2.5_
  - [x] 2.3 实现 `build_tee_pipeline()` 函数
    - 创建管道容器 `gst_pipeline_new("tee-pipeline")`
    - 创建所有 14 个元素（videotestsrc、videoconvert、capsfilter、raw-tee、q-ai、ai-sink、q-enc、encoder、h264parse、encoded-tee、q-kvs、kvs-sink、q-web、webrtc-sink）
    - 元素创建失败检查：任一元素为 nullptr 时清理已创建资源并返回 nullptr
    - 设置 capsfilter caps `video/x-raw,format=I420`，设置后立即 `gst_caps_unref(caps)`
    - 设置 queue 参数：q-ai 和 q-web 设置 `max-size-buffers=1, leaky=downstream(2)`；q-enc 和 q-kvs 设置 `max-size-buffers=1`（无 leaky）
    - 设置三个 fakesink 名称：`"ai-sink"`、`"kvs-sink"`、`"webrtc-sink"`
    - `gst_bin_add_many` 添加所有元素到管道
    - `gst_element_link_many` 链接主干：src → convert → capsfilter → raw-tee
    - `gst_element_link_many` 链接编码链路：q-enc → encoder → parser → enc-tee
    - `link_tee_to_element` 链接 4 个 tee request pad（raw-tee→q-ai、raw-tee→q-enc、enc-tee→q-kvs、enc-tee→q-web）
    - `gst_element_link` 链接分支末端（q-ai→ai-sink、q-kvs→kvs-sink、q-web→webrtc-sink）
    - 任何链接失败时清理管道并返回 nullptr，error_msg 输出失败详情
    - _需求：3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 4.1, 4.2, 4.3, 4.5_

- [x] 3. CMakeLists.txt 更新与编译验证
  - [x] 3.1 更新 `device/CMakeLists.txt`
    - 修改 `pipeline_manager` 静态库的源文件列表，添加 `src/pipeline_builder.cpp`
    - 新增 `tee_test` 测试目标：`add_executable(tee_test tests/tee_test.cpp)` + `target_link_libraries(tee_test PRIVATE pipeline_manager GTest::gtest_main)` + `add_test(NAME tee_test COMMAND tee_test)`
    - 不修改现有 `pipeline_manager`、`raspi-eye`、`smoke_test`、`log_test` 的核心定义
    - _需求：6.1_
  - [x] 3.2 macOS Debug 编译验证
    - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
    - 确认编译无错误（ASan 相关警告除外）
    - _需求：6.1_

- [x] 4. tee_test 冒烟测试
  - [x] 4.1 创建 `device/tests/tee_test.cpp` — 8 个测试用例
    - `AdoptValidPipeline`：`gst_pipeline_new` 创建简单管道 → `PipelineManager::create(GstElement*)` 返回非 nullptr — _需求：1.1, 1.2_
    - `AdoptNullPipeline`：`PipelineManager::create(nullptr)` 返回 nullptr，error_msg 非空 — _需求：1.3_
    - `AdoptedPipelineStart`：通过新工厂方法创建实例 → `start()` → `current_state()` 返回 `GST_STATE_PLAYING` — _需求：1.5_
    - `TeePipelinePlaying`：`build_tee_pipeline()` → `PipelineManager::create` → `start()` → `current_state()` 返回 `GST_STATE_PLAYING` — _需求：3.1, 3.8, 4.4, 7.2_
    - `TeePipelineNamedSinks`：管道启动后通过 `gst_bin_get_by_name` 验证存在 `"kvs-sink"`、`"webrtc-sink"`、`"ai-sink"` 三个元素，获取后 `gst_object_unref` — _需求：3.4, 7.3_
    - `TeePipelineStop`：管道启动后 `stop()` → `pipeline()` 返回 nullptr，ASan 无报告 — _需求：1.6, 7.4_
    - `TeePipelineRAII`：在作用域内创建并启动 tee 管道，离开作用域后 ASan 无报告 — _需求：7.5_
    - `EncoderDetection`：`build_tee_pipeline()` 成功返回非 nullptr（间接验证编码器检测），通过 `gst_bin_get_by_name` 获取 `"encoder"` 元素并查询 `tune`、`speed-preset`、`threads` 属性值 — _需求：2.1, 2.2, 7.8_
    - 所有测试仅使用 `fakesink`，不使用需要显示设备的 sink
    - 每个测试 ≤ 5 秒
    - _需求：7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7, 7.8_
  - [x] 4.2 运行全量测试
    - 执行 `ctest --test-dir device/build --output-on-failure`
    - 确认 23 个测试全部通过（8 smoke + 7 log + 8 tee）
    - 确认 ASan 无内存错误报告
    - _需求：6.3, 7.1–7.8_

- [x] 5. 检查点 — 确认现有测试回归通过
  - 确认 smoke_test（8 个）和 log_test（7 个）全部通过，行为不变
  - 确认 tee_test（8 个）全部通过
  - 如有问题，询问用户

- [x] 6. main.cpp 管道升级
  - [x] 6.1 修改 `device/src/main.cpp`
    - 添加 `#include "pipeline_builder.h"`
    - 在 `run_pipeline()` 中替换 `PipelineManager::create("videotestsrc ! videoconvert ! autovideosink", &err_msg)` 为：
      1. `GstElement* raw_pipeline = PipelineBuilder::build_tee_pipeline(&err_msg)` — 失败时 spdlog error + return 1
      2. `auto pm = PipelineManager::create(raw_pipeline, &err_msg)` — 失败时 spdlog error + return 1
    - GMainLoop、bus_callback、SIGINT handler、`gst_macos_main` 包装逻辑不变
    - _需求：5.1, 5.2, 5.3, 5.4, 5.5_
  - [x] 6.2 编译验证
    - 执行 `cmake --build device/build`
    - 确认编译无错误
    - 执行 `ctest --test-dir device/build --output-on-failure` 确认所有测试通过
    - _需求：6.1, 6.3_

- [x] 7. 最终检查点 — 全量验证
  - 确认以下全部通过：
    - `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` — macOS 编译测试通过、ASan 无报告
    - 23 个测试全部通过（8 smoke + 7 log + 8 tee）
  - Pi 5 Release 验证（`scripts/pi-build.sh` 或手动 SSH）需要 Pi 5 可达，如不可达则标注跳过
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：6.1, 6.2, 6.3, 6.4_

## 备注

- 本 Spec 不涉及 PBT，所有测试为 example-based 冒烟测试 + ASan 运行时检查
- 新建文件：`device/src/pipeline_builder.h`、`device/src/pipeline_builder.cpp`、`device/tests/tee_test.cpp`
- 修改文件：`device/src/pipeline_manager.h`、`device/src/pipeline_manager.cpp`、`device/src/main.cpp`、`device/CMakeLists.txt`
- 不修改文件：`device/tests/smoke_test.cpp`、`device/tests/log_test.cpp`
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
- Pi 5 远程验证需要 Pi 5 可达（通过 SSH），不可达时相关验证标注跳过
