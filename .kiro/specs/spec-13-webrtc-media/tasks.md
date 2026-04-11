# 实施计划：Spec 13 — WebRTC 媒体流传输

## 概述

在 Spec 12（信令层）基础上实现 WebRTC 媒体流传输。核心新增模块 WebRtcMediaManager 管理多个 PeerConnection 的生命周期和 H.264 帧广播，通过 pImpl + 条件编译（`HAVE_KVS_WEBRTC_SDK`）隔离平台实现。按依赖顺序：webrtc_media.h/cpp → CMakeLists.txt 更新（含 gstreamer-app-1.0）→ 编译检查点 → pipeline_builder 修改（appsink 替换 fakesink）→ webrtc_media_test.cpp 测试（6 个 example-based + 1 个 PBT）→ 最终检查点。main.cpp 集成（KVS + WebRTC + AI 三路接入）由后续独立 Spec 完成。实现语言为 C++17。

## 禁止项（Tasks 层）

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 KVS WebRTC C SDK C API 外）
- SHALL NOT 修改现有测试文件（smoke_test、log_test、tee_test、camera_test、health_test、credential_test、kvs_test、webrtc_test、yolo_test）
- SHALL NOT 在子代理的最终检查点任务中执行 git commit
- SHALL NOT 在不确定 KVS WebRTC C SDK API 用法时凭猜测编写代码
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token、SDP 完整内容、ICE Candidate 完整内容等敏感信息
- SHALL NOT 在任何日志级别输出证书路径和凭证信息（仅输出 peer_id、channel_name 等资源标识）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 使用 `#ifdef __linux__` 做 SDK 条件编译，必须使用 `#ifdef HAVE_KVS_WEBRTC_SDK` 自定义宏
- SHALL NOT 在测试中用假凭证数据调用可能创建真实 PeerConnection 的函数

## 任务

- [x] 1. WebRtcMediaManager 模块实现
  - [x] 1.1 创建 `device/src/webrtc_media.h`
    - 前向声明 `WebRtcSignaling`，避免头文件依赖
    - 声明 `WebRtcMediaManager` 类（pImpl 模式）：`create(WebRtcSignaling&, error_msg*)`、`on_viewer_offer(peer_id, sdp_offer, error_msg*)`、`on_viewer_ice_candidate(peer_id, candidate, error_msg*)`、`remove_peer(peer_id)`、`broadcast_frame(data, size, timestamp_100ns, is_keyframe)`、`peer_count()`
    - 禁用拷贝构造和拷贝赋值（`= delete`）
    - 析构函数声明（pImpl 需要在 .cpp 中定义）
    - 私有构造函数 + `struct Impl` 前向声明 + `std::unique_ptr<Impl> impl_`
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5_

  - [x] 1.2 创建 `device/src/webrtc_media.cpp` — stub 实现
    - `#ifndef HAVE_KVS_WEBRTC_SDK` 分支：stub 实现
    - `Impl` 结构体：`WebRtcSignaling& signaling`、`std::unordered_set<std::string> peers`、`mutable std::mutex peers_mutex`
    - 常量：`kMaxPeers = 10`、`kMaxPeerIdLen = 256`
    - `create()`：构造 Impl，spdlog 记录 "Created WebRtcMediaManager stub"
    - `on_viewer_offer()`：lock_guard 保护；peer_id 长度 > 256 → 返回 false + warn 日志；同一 peer_id 已存在 → 先 erase 再 insert（替换语义，count 不变）；peers.size() >= 10 → 返回 false + warn 日志；成功时 insert + info 日志（含 peer_id 和 count）
    - `on_viewer_ice_candidate()`：stub 返回 true
    - `remove_peer()`：lock_guard 保护，erase（幂等），erased 时 info 日志
    - `broadcast_frame()`：stub 空操作
    - `peer_count()`：lock_guard 保护，返回 peers.size()
    - 析构函数：默认（Impl 析构自动清理 set）
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5, 2.1, 2.6, 2.7, 2.8, 2.9, 3.3, 3.4, 5.1, 5.3, 5.4, 5.5_

  - [x] 1.3 创建 `device/src/webrtc_media.cpp` — 真实实现骨架
    - `#ifdef HAVE_KVS_WEBRTC_SDK` 分支：真实实现
    - `PeerInfo` 结构体：`PRtcPeerConnection peer_connection`、`PRtcRtpTransceiver video_transceiver`、`uint32_t consecutive_write_failures = 0`
    - `Impl` 结构体：`WebRtcSignaling& signaling`、`std::unordered_map<std::string, PeerInfo> peers`、`mutable std::mutex peers_mutex`
    - 常量：`kMaxPeers = 10`、`kMaxPeerIdLen = 256`、`kMaxWriteFailures = 100`
    - `create()`：构造 Impl
    - `on_viewer_offer()` 流程：lock → peer_id 长度检查 → 已存在则 freePeerConnection 旧的 → 上限检查 → createPeerConnection → addTransceiver(H.264) → peerConnectionOnIceCandidate 回调 → peerConnectionOnConnectionStateChange 回调 → setRemoteDescription → createAnswer → setLocalDescription → signaling.send_answer → 存入 peers map；任何 SDK API 失败时 freePeerConnection 回滚
    - `broadcast_frame()`：lock → 遍历 peers → 构造 Frame（零拷贝）→ writeFrame → 成功重置 failures=0 / 失败递增 failures → 超过 kMaxWriteFailures 标记待清理 → 遍历后清理标记的 peer
    - `remove_peer()`：lock → freePeerConnection → erase
    - 析构：lock → 遍历 freePeerConnection → clear
    - _需求：2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 5.1, 5.2, 5.3, 5.4, 5.5_

- [x] 2. CMakeLists.txt 更新
  - [x] 2.1 修改 `device/CMakeLists.txt`
    - 添加 `pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)` 查找 appsink 依赖
    - 添加 `webrtc_media_module` 静态库（`src/webrtc_media.cpp`），链接 `spdlog::spdlog`、`webrtc_module`
    - Linux + SDK 可用时（`KVS_WEBRTC_SIGNALING_LIB AND KVS_WEBRTC_INCLUDE_DIR`）：`target_compile_definitions(webrtc_media_module PRIVATE HAVE_KVS_WEBRTC_SDK=1)`，链接 SDK 库和头文件
    - `pipeline_manager` 链接 `webrtc_media_module`、`${GST_APP_LIBRARIES}`，添加 `${GST_APP_INCLUDE_DIRS}` 和 `${GST_APP_LIBRARY_DIRS}`
    - 添加 `webrtc_media_test` 测试目标（`tests/webrtc_media_test.cpp`），链接 `webrtc_media_module`、`pipeline_manager`、`GTest::gtest`、`rapidcheck`、`rapidcheck_gtest`
    - `add_test(NAME webrtc_media_test COMMAND webrtc_media_test WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")`
    - 不修改现有 `pipeline_manager`、`webrtc_module`、`kvs_module` 及所有现有测试目标的定义
    - _需求：7.1, 7.2, 7.3, 7.4_

- [x] 3. 检查点 — 编译通过与现有测试回归
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
  - 确认编译无错误（webrtc_media_module 编译 stub 路径）
  - 执行 `ctest --test-dir device/build --output-on-failure` 确认现有测试全部通过
  - 确认 ASan 无内存错误报告
  - 如有问题，询问用户

- [x] 4. 管道集成 — appsink 替换 fakesink
  - [x] 4.1 修改 `device/src/pipeline_builder.h`
    - 前向声明 `class WebRtcMediaManager`
    - `build_tee_pipeline()` 新增参数 `WebRtcMediaManager* webrtc_media = nullptr`（末尾默认参数，向后兼容）
    - _需求：4.1, 4.2_

  - [x] 4.2 修改 `device/src/pipeline_builder.cpp`
    - 新增 `#include <gst/app/gstappsink.h>` 和 `#include "webrtc_media.h"`
    - 新增静态回调函数 `on_new_sample(GstElement* sink, gpointer user_data)`：
      - `gst_app_sink_pull_sample` → `gst_sample_get_buffer` → `gst_buffer_map(GST_MAP_READ)`
      - 从 buffer flags 检测关键帧（`GST_BUFFER_FLAG_DELTA_UNIT` 未设置 = 关键帧）
      - PTS 转换：`GST_BUFFER_PTS(buffer) / 100`（ns → 100ns）
      - 调用 `manager->broadcast_frame(map.data, map.size, timestamp_100ns, is_keyframe)`
      - `gst_buffer_unmap` → `gst_sample_unref` → 返回 `GST_FLOW_OK`
      - `gst_buffer_map` 失败时跳过本帧，unref sample，返回 `GST_FLOW_OK`
    - 修改 `build_tee_pipeline()`：
      - 接受新参数 `WebRtcMediaManager* webrtc_media`
      - `webrtc_media != nullptr` 时：创建 appsink（`gst_element_factory_make("appsink", "webrtc-sink")`），设置 `emit-signals=TRUE, drop=TRUE, max-buffers=1, sync=FALSE`，`g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), webrtc_media)`
      - `webrtc_media == nullptr` 时：保持 fakesink（向后兼容）
    - _需求：4.1, 4.2, 4.3, 4.4, 4.5, 4.6_

- [x] 5. 测试实现
  - [x] 5.1 创建 `device/tests/webrtc_media_test.cpp` — 基础结构与 example-based 测试
    - 自定义 `main()`：调用 `gst_init(&argc, &argv)` 后 `RUN_ALL_TESTS()`（管道冒烟测试需要 GStreamer 初始化）
    - 包含 `webrtc_media.h`、`webrtc_signaling.h`、`pipeline_builder.h`、`gtest/gtest.h`、`rapidcheck.h`、`rapidcheck/gtest.h`、`gst/gst.h`
    - 辅助：创建 stub WebRtcSignaling 实例用于测试（通过 `WebRtcSignaling::create()` 创建 stub）
    - 6 个 example-based 测试：
      1. `StubCreateSuccess`：create() 返回非 nullptr，peer_count() == 0 — _需求：6.1_
      2. `BroadcastFrameNoPeers`：无 peer 时 broadcast_frame() 不崩溃 — _需求：6.3_
      3. `AppsinkReplacesFakesink`：传入 WebRtcMediaManager 时管道中 "webrtc-sink" 元素是 appsink 类型 — _需求：4.1, 6.7_
      4. `FakesinkPreservedWhenNull`：不传入时管道中 "webrtc-sink" 元素是 fakesink 类型 — _需求：4.2_
      5. `AppsinkProperties`：appsink 的 emit-signals/drop/max-buffers/sync 属性值正确 — _需求：4.3_
      6. `PipelineSmokeWithAppsink`：appsink 管道启动后能接收 buffer（冒烟测试，≤ 5 秒超时） — _需求：6.7_
    - _需求：6.1, 6.2, 6.3, 6.7, 6.8, 6.9_

  - [x] 5.2 添加 PBT — Property 1: PeerCountInvariant
    - **Property 1: PeerConnection 管理不变量（Model-Based Testing）**
    - **验证：需求 2.1, 2.6, 2.7, 5.1, 5.4, 6.2, 6.4, 6.5, 6.6, 6.10**
    - 随机生成操作序列（`on_viewer_offer(random_peer_id)` 和 `remove_peer(random_peer_id)` 混合）
    - 维护 reference `std::unordered_set<std::string>`，模拟 add/remove 语义
    - 每次操作后验证：`peer_count()` == reference set 实际大小 且 `peer_count() ≤ 10`
    - on_viewer_offer 成功时 reference set 包含该 id；达到 10 上限且 id 不在 set 中时返回 false
    - remove_peer 后 reference set 不包含该 id
    - 同一 peer_id 重复 offer 不增加 count（set 语义）
    - RapidCheck 配置：≥ 100 次迭代，≤ 15 秒超时
    - 标签格式：`Feature: spec-13-webrtc-media, Property 1: PeerConnection management invariant`

- [x] 6. 最终检查点 — 全量验证
  - 执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`
  - 执行 `ctest --test-dir device/build --output-on-failure` 确认所有测试通过（现有 smoke_test、log_test、tee_test、camera_test、health_test、credential_test、kvs_test、webrtc_test + 可选 yolo_test + 新增 webrtc_media_test）
  - 确认 ASan 无内存错误报告
  - 确认现有测试行为不变
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：7.1, 7.2, 7.3, 7.4_

## 备注

- 新建文件：`device/src/webrtc_media.h`、`device/src/webrtc_media.cpp`、`device/tests/webrtc_media_test.cpp`
- 修改文件：`device/src/pipeline_builder.h`、`device/src/pipeline_builder.cpp`、`device/CMakeLists.txt`
- 不修改文件：所有现有测试文件、webrtc_signaling.h/cpp（仅复用接口）
- 条件编译使用 `HAVE_KVS_WEBRTC_SDK` 自定义宏（非 `__linux__`），CMake 在找到 SDK 时定义此宏
- macOS 上全部使用 stub 实现，测试不依赖真实 KVS WebRTC SDK
- Linux 上 SDK 不可用时回退到 stub（CMake WARNING），不阻断编译
- webrtc_media_test 使用自定义 main()（gst_init），不使用 GTest::gtest_main
- PBT 使用 RapidCheck，Property 1 最少 100 次迭代
- 标记 `*` 的子任务为可选（PBT 属性测试），可跳过以加速 MVP
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
- std::mutex 保护 peer 映射：broadcast_frame 在 streaming 线程，on_viewer_offer/remove_peer 在 SDK 线程
- writeFrame 连续失败 100 次自动 remove_peer（自愈），仅在真实实现中生效
- appsink new-sample 回调同步调用 broadcast_frame，buffer 生命周期由 gst_buffer_map/unmap 保证
