# Bugfix Tasks: spec-14-webrtc-sdp-fix

## 任务列表

### 任务 1: WebRtcSignaling 新增 ICE 配置接口

- [x] 1.1 在 `webrtc_signaling.h` 中新增 `IceServerInfo` 结构体、`get_ice_config_count()` 和 `get_ice_config()` 声明
- [x] 1.2 在 `webrtc_signaling.cpp` SDK 分支中实现 `get_ice_config_count()`（调用 `signalingClientGetIceConfigInfoCount`）和 `get_ice_config()`（调用 `signalingClientGetIceConfigInfo`）
- [x] 1.3 在 `webrtc_signaling.cpp` stub 分支中实现 stub 版本（返回 0 / false）

### 任务 2: WebRtcMediaManager::create 签名扩展

- [x] 2.1 在 `webrtc_media.h` 中修改 `create()` 签名，新增 `const std::string& aws_region = ""` 参数
- [x] 2.2 在 `webrtc_media.cpp` stub 分支中更新 `create()` 签名（忽略 region 参数）
- [x] 2.3 在 `webrtc_media.cpp` SDK 分支中更新 `create()` 签名，将 region 存入 `Impl::region` 成员
- [x] 2.4 在 `app_context.cpp` 中更新 `WebRtcMediaManager::create` 调用，传入 `impl_->webrtc_config.aws_region`

### 任务 3: 重写 on_viewer_offer（5 个修复点）

- [x] 3.1 步骤 2: 配置 `RtcConfiguration`（STUN + TURN 服务器），使用 `signaling.get_ice_config_count()` / `get_ice_config()` 获取 TURN 信息
- [x] 3.2 步骤 4: 在 `addTransceiver` 之前调用 `addSupportedCodec`（H.264 致命 + OPUS 非致命）
- [x] 3.3 步骤 5: 视频和音频 transceiver direction 都改为 `RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV`
- [x] 3.4 步骤 8-9: SDP answer 流程改为 SDK 顺序 `setLocalDescription（空 answer）→ createAnswer`
- [x] 3.5 整体检查：确认错误处理路径完整（每个失败点都 cleanup cb_ctx + freePeerConnection）

### 任务 4: 修复 on_viewer_ice_candidate

- [x] 4.1 使用 `deserializeRtcIceCandidateInit()` 替代直接 `STRNCPY`，反序列化失败时 warn 日志并返回 false

### 任务 5: 编译验证 + 测试回归

- [x] 5.1 macOS Debug 编译通过（`cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build`）
- [x] 5.2 macOS 11/11 测试通过（`ctest --test-dir device/build --output-on-failure`）
- [x] 5.3 `git status` 确认无敏感文件

## 验证

macOS: `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` → 11/11 通过

Pi 5 端到端验证（用户手动）：
1. `git pull && cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build`
2. `./device/build/raspi-eye --camera v4l2 --device /dev/IMX678 --config device/config/config.toml`
3. AWS Console → KVS WebRTC → 选择 channel → Start Viewer → 确认视频流显示

## 涉及文件

| 文件 | 修改类型 |
|------|---------|
| `device/src/webrtc_signaling.h` | 新增接口 |
| `device/src/webrtc_signaling.cpp` | 新增实现 |
| `device/src/webrtc_media.h` | 签名变更 |
| `device/src/webrtc_media.cpp` | 重写 on_viewer_offer + on_viewer_ice_candidate |
| `device/src/app_context.cpp` | 更新 create 调用 |
