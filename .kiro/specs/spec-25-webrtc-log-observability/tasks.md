# 实现计划：Spec 25 — WebRTC 日志可观测性优化

## 概述

按依赖顺序实现：先扩展 PeerInfo 结构体（stub + real 同步），再实现 extract_sdp_summary 纯函数，然后逐模块更新日志（webrtc_media.cpp stub → real → webrtc_signaling.cpp），最后补充测试。所有变更仅涉及日志输出，不修改 WebRTC 功能逻辑。

## 任务

- [x] 1. 扩展 PeerInfo 结构体 + extract_sdp_summary 函数
  - [x] 1.1 扩展 stub 实现的 PeerInfo 结构体
    - 修改 `device/src/webrtc_media.cpp` stub 部分（`#ifndef HAVE_KVS_WEBRTC_SDK`）
    - 将 `std::unordered_map<std::string, PeerState> peers` 改为 `std::unordered_map<std::string, PeerInfo> peers`
    - 新增 stub PeerInfo 结构体（不使用 `std::atomic`，避免 GCC 12 不可拷贝问题）：
      - `PeerState state = PeerState::CONNECTING;`
      - `std::chrono::steady_clock::time_point created_at;`
      - `std::chrono::steady_clock::time_point disconnected_at;`
      - `bool first_frame_sent = false;`
      - `std::string disconnect_reason;`
      - `uint32_t sent_candidates = 0;`
      - `uint32_t received_candidates = 0;`
    - 同步更新 stub 中所有访问 peers map 的代码（原来直接访问 PeerState，改为访问 PeerInfo.state）
    - 移除 `disconnect_times` map（时间戳已合并到 PeerInfo.disconnected_at）
    - _需求: 1.5, 2.5, 4.3, 5.4, 6.3_

  - [x] 1.2 扩展 real 实现的 PeerInfo 结构体
    - 修改 `device/src/webrtc_media.cpp` real 部分（`#ifdef HAVE_KVS_WEBRTC_SDK`）
    - 在现有 PeerInfo 中新增字段：
      - `std::chrono::steady_clock::time_point created_at;`
      - `bool first_frame_sent = false;`
      - `std::string disconnect_reason;`
      - `uint32_t sent_candidates = 0;`
      - `uint32_t received_candidates = 0;`
    - _需求: 1.5, 2.5, 4.3, 5.4_

  - [x] 1.3 实现 extract_sdp_summary 纯函数
    - 在 `device/src/webrtc_media.h` 中添加自由函数声明：`std::string extract_sdp_summary(const std::string& sdp);`
    - 在 `device/src/webrtc_media.cpp` 中实现（放在两个条件编译块之前，作为公共代码）：
      - 按行分割 SDP 文本（`\r\n` 或 `\n`）
      - 查找 `a=rtpmap:` 开头的行
      - 从 `a=rtpmap:<pt> <codec>/<rate>` 格式提取 `<codec>`
      - 去重后逗号连接返回
      - 空字符串或无 rtpmap 行返回空字符串，不抛异常
    - _需求: 3.4, 3.5_

- [x] 2. 检查点 - PeerInfo 扩展编译验证
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认编译和现有测试通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [ ] 3. 更新 webrtc_media.cpp 日志 — stub 实现
  - [x] 3.1 更新 stub on_viewer_offer 日志
    - 创建 peer 时设置 `created_at = std::chrono::steady_clock::now()`
    - 清理 DISCONNECTING peer 时输出存活时长和 disconnect_reason：`info("Freed stale peer {} (alive={:.1f}s, reason={})")`
    - _需求: 2.5, 5.2_

  - [x] 3.2 更新 stub on_viewer_ice_candidate 日志
    - 添加 `debug("Stub: received ICE candidate for peer {}")` 日志
    - 递增 `received_candidates` 计数器
    - _需求: 1.1, 1.5_

  - [x] 3.3 更新 stub broadcast_frame 首帧日志
    - 遍历 CONNECTED peer 时，检查 `first_frame_sent` 标记
    - 首帧时输出 `info("First frame sent to peer {} (size={}, keyframe={})")` 并设置 `first_frame_sent = true`
    - _需求: 4.1, 4.3_

  - [x] 3.4 更新 stub remove_peer / cleanup_loop / ~Impl 日志
    - `remove_peer`：设置 disconnect_reason="manual_remove"，输出存活时长 `info("Removed peer {} (alive={:.1f}s, reason=manual_remove)")`
    - `cleanup_loop`：输出存活时长和 disconnect_reason `info("Cleanup: freed peer {} (alive={:.1f}s, reason={})")`
    - `~Impl`：输出每个 peer 的存活时长和最终状态 `info("Shutdown: freed peer {} (alive={:.1f}s, state={})")`
    - _需求: 5.1, 5.2, 5.3, 6.2_

- [ ] 4. 更新 webrtc_media.cpp 日志 — real 实现
  - [x] 4.1 更新 real on_viewer_offer 日志
    - 创建 PeerConnection 后设置 `created_at = std::chrono::steady_clock::now()`
    - 清理 DISCONNECTING peer 时输出存活时长和 disconnect_reason
    - 添加 SDP 摘要日志：`debug("Offer SDP summary for peer {}: {}", peer_id, extract_sdp_summary(sdp_offer))`
    - 发送 answer 后添加：`debug("Answer SDP summary for peer {}: {}", peer_id, extract_sdp_summary(answer_sdp))`
    - _需求: 2.5, 3.1, 3.2, 5.2_

  - [x] 4.2 更新 real on_viewer_ice_candidate 日志
    - 已有 peer 时递增 `received_candidates` 计数器
    - 缓存早到 candidate 时降级为 debug：`debug("Buffered early ICE candidate for peer: {} (buffered={})")`
    - _需求: 1.1, 1.3, 1.5_

  - [x] 4.3 更新 real on_ice_candidate_handler 日志
    - 递增 `sent_candidates` 计数器（需通过 peers_mutex 访问 PeerInfo）
    - _需求: 1.2, 1.5_

  - [x] 4.4 更新 real on_connection_state_change 日志
    - CONNECTED 时：`info("Peer {} connected (elapsed={:.1f}s, ice_sent={}, ice_recv={})")` 包含连接耗时和 ICE 汇总
    - FAILED 时：`warn("Peer {} connection FAILED, marking DISCONNECTING")` + 设置 disconnect_reason="connection_failed"
    - CLOSED 时：`info("Peer {} connection closed, marking DISCONNECTING")` + 设置 disconnect_reason="connection_closed"
    - _需求: 1.4, 2.1, 2.2, 2.3, 2.4, 5.4_

  - [x] 4.5 更新 real broadcast_frame 日志
    - 首帧成功时：`info("First frame sent to peer {} (size={}, keyframe={})")` + `first_frame_sent = true`
    - SRTP 跳过时：`debug("writeFrame skipped for peer {}: SRTP not ready")`
    - 连续失败达 50% 阈值时：`warn("writeFrame failing for peer {}: {}/{} consecutive failures")`
    - 超过 kMaxWriteFailures 时设置 disconnect_reason="max_write_failures"
    - _需求: 4.1, 4.2, 4.3, 4.4_

  - [x] 4.6 更新 real remove_peer / cleanup_loop / ~Impl 日志
    - `remove_peer`：设置 disconnect_reason="manual_remove"，输出存活时长
    - `cleanup_loop`：输出存活时长和 disconnect_reason
    - `~Impl`：输出每个 peer 的存活时长和最终状态
    - _需求: 5.1, 5.2, 5.3_

- [x] 5. 检查点 - webrtc_media.cpp 日志更新编译验证
  - 运行 `cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认编译和测试通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [ ] 6. 更新 webrtc_signaling.cpp 日志
  - [x] 6.1 ICE candidate 日志降级 + SDP 摘要
    - `on_signaling_message_received` — ICE_CANDIDATE：`info` → `debug`
    - `on_signaling_message_received` — OFFER：保留 info，新增 `debug("SDP summary for peer {}: {}", peer_id, extract_sdp_summary(sdp))`
    - `send_answer` — 成功后新增：`debug("Answer SDP summary for peer {}: {}", peer_id, extract_sdp_summary(sdp_answer))`
    - `send_ice_candidate` — 成功：`info` → `debug`
    - 需要在文件顶部 `#include "webrtc_media.h"` 以访问 `extract_sdp_summary`
    - _需求: 1.1, 1.2, 3.1, 3.2_

- [x] 7. 检查点 - 全量编译与现有测试回归
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认全量编译和所有现有测试通过
  - 确认无 ASan 报告
  - 如有问题，询问用户

- [ ] 8. 测试 — extract_sdp_summary + 回归验证
  - [x] 8.1 编写 Property 1 测试：SDP 摘要提取 round-trip
    - **Property 1: SDP 摘要提取 round-trip**
    - 在 `device/tests/webrtc_media_test.cpp` 中新增
    - 生成器：随机生成 1-10 个纯字母数字 codec 名称，构造含 `a=rtpmap:<pt> <codec>/<rate>` 行的 SDP 文本
    - 断言：`extract_sdp_summary` 返回的 codec 列表包含所有生成的 codec 名称（去重后）
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 3.5, 7.3**

  - [x] 8.2 编写 Property 2 测试：extract_sdp_summary 鲁棒性
    - **Property 2: extract_sdp_summary 鲁棒性**
    - 在 `device/tests/webrtc_media_test.cpp` 中新增
    - 生成器：任意字符串（包括空字符串、纯随机字节、不含 `a=rtpmap:` 的文本）
    - 断言：`extract_sdp_summary` 不崩溃，返回值为有效 `std::string`（可能为空）
    - RapidCheck 最少 100 次迭代
    - **验证: 需求 3.3, 7.3**

  - [x] 8.3 编写 extract_sdp_summary Example-based 单元测试
    - 在 `device/tests/webrtc_media_test.cpp` 中新增以下测试：
    - `ExtractSdpSummary_EmptyString`：空字符串 → 返回空
    - `ExtractSdpSummary_NoRtpmap`：无 `a=rtpmap:` 行 → 返回空
    - `ExtractSdpSummary_RealSdp`：含 H264 + opus 的真实 SDP 片段 → 返回 "H264, opus"（或类似）
    - `ExtractSdpSummary_DuplicateCodecs`：重复 codec 名称 → 去重
    - _需求: 3.3, 3.5, 7.3_

- [x] 9. 最终检查点 - 全量编译与测试
  - 运行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure` 确认全量编译和所有测试通过（含新增 PBT + Example-based）
  - 确认现有 webrtc_test + webrtc_media_test 全部通过（回归验证）
  - 确认无 ASan 报告
  - 运行 `git status` 确认无敏感文件（.pem、.key 等）被跟踪
  - 不执行 git commit（由编排层统一执行）
  - 如有问题，询问用户

## 禁止项

- SHALL NOT 修改 WebRTC 连接建立、信令交换、媒体流传输的功能逻辑
- SHALL NOT 在 `broadcast_frame` 的每帧调用中输出 info 级别日志（高频路径）
- SHALL NOT 在日志消息中使用非 ASCII 字符
- SHALL NOT 在 SDP 摘要日志中输出完整 SDP 文本
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息
- SHALL NOT 直接运行测试可执行文件，统一通过 `ctest --test-dir device/build --output-on-failure` 运行
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件，使用 fsWrite / fsAppend 工具
- SHALL NOT 在子代理中执行 git commit
- SHALL NOT 对含 `std::atomic` 成员的结构体使用 `unordered_map::emplace` 或 `insert`（stub 中 PeerInfo 不使用 atomic 避免此问题）

## 备注

- 标记 `*` 的子任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求编号，确保可追溯
- 检查点确保增量验证
- Property 测试验证 extract_sdp_summary 纯函数的通用正确性（2 条），Example-based 测试验证具体 SDP 格式和边界条件
- 所有改动限定在 `webrtc_media.cpp`、`webrtc_media.h`、`webrtc_signaling.cpp` 三个源文件 + `webrtc_media_test.cpp` 测试文件
