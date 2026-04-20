# Implementation Plan

- [ ] 1. Write bug condition exploration test
  - **Property 1: Bug Condition** — Signaling 断连后无自动重连
  - **CRITICAL**: This test MUST FAIL on unfixed code — failure confirms the bug exists
  - **DO NOT attempt to fix the test or the code when it fails**
  - **NOTE**: This test encodes the expected behavior — it will validate the fix when it passes after implementation
  - **GOAL**: Surface counterexamples that demonstrate the bug exists
  - **Scoped PBT Approach**: 在 stub 实现上模拟断连场景，验证断连后系统是否自动恢复连接
  - 在 `device/tests/webrtc_test.cpp` 中新增 RC_GTEST_PROP 测试
  - Bug Condition: `isBugCondition(input)` — signaling 处于 CONNECTED 状态 → 断连（disconnect 模拟） → 等待合理时间 → 检查 `is_connected()` 是否恢复为 true
  - Expected Behavior: 断连后 `needs_reconnect_` 被设置，reconnect_thread_ 在退避后调用重连，最终 `is_connected() == true`
  - 测试步骤：create stub signaling → connect() → disconnect()（模拟断连） → sleep 2s → assert `is_connected() == true`
  - 在未修复代码上运行：disconnect() 后无重连机制，`is_connected()` 保持 false → 测试 FAIL（确认 bug 存在）
  - **EXPECTED OUTCOME**: Test FAILS (this is correct — it proves the bug exists)
  - Document counterexamples: "disconnect() 后 is_connected() 保持 false，无自动重连尝试"
  - Mark task complete when test is written, run, and failure is documented
  - _Requirements: 1.1, 1.2, 1.4, 2.1, 2.2_

- [ ] 2. Write preservation property tests (BEFORE implementing fix)
  - **Property 2: Preservation** — 非断连场景行为不变
  - **IMPORTANT**: Follow observation-first methodology
  - 在 `device/tests/webrtc_test.cpp` 中新增 RC_GTEST_PROP 测试
  - Observe on UNFIXED code:
    - connect() → is_connected() == true
    - disconnect() → is_connected() == false
    - 未连接时 send_answer() 返回 false
    - 未连接时 send_ice_candidate() 返回 false
    - 已连接时 send_answer() 返回 true
    - 已连接时 send_ice_candidate() 返回 true
  - Write property-based test: 随机生成 connect/disconnect/send 操作序列，验证 `is_connected()` 状态与 send 返回值的一致性
  - 具体 property: FOR ALL 随机操作序列（connect, disconnect, send_answer, send_ice_candidate），执行后 `is_connected()` 状态正确，且 send 在 connected 时返回 true、在 disconnected 时返回 false
  - Verify test passes on UNFIXED code
  - **EXPECTED OUTCOME**: Tests PASS (this confirms baseline behavior to preserve)
  - Mark task complete when tests are written, run, and passing on unfixed code
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7_

- [ ] 3. Fix: Signaling 断连自动重连

  - [ ] 3.1 修改 macOS stub Impl：新增重连基础设施
    - 在 stub `WebRtcSignaling::Impl` 中新增成员：`needs_reconnect_`、`shutdown_requested_`（atomic<bool>）、`reconnect_thread_`、`reconnect_mutex_`、`reconnect_cv_`、`total_disconnects_`、`total_reconnects_`
    - 新增 `reconnect_loop()` stub 版本：检测 `needs_reconnect_` 后短暂等待，设置 `connected = true`，递增 `total_reconnects_`
    - 新增 `simulate_disconnect()` 方法（仅 stub）：设置 `connected = false`，设置 `needs_reconnect_ = true`，递增 `total_disconnects_`，通知 `reconnect_cv_`
    - 修改 stub `~Impl()`：设置 `shutdown_requested_ = true`，通知 cv，join reconnect_thread_
    - _Bug_Condition: isBugCondition(input) — DISCONNECTED 后无代码调用重连_
    - _Expected_Behavior: reconnect_loop 检测 needs_reconnect_ 后自动重连恢复 connected_
    - _Preservation: stub connect/disconnect/send 基本行为不变_
    - _Requirements: 2.1, 2.2, 2.6, 2.7, 3.1_
    - SHALL NOT 对含 `std::atomic` 成员的结构体使用 `unordered_map::emplace`（改用 `try_emplace`）
    - SHALL NOT 在 SDK 回调线程中调用 `signalingClientConnectSync`

  - [ ] 3.2 修改公共接口方法：connect/disconnect/reconnect
    - `connect()` 方法：连接成功后启动 `reconnect_thread_`（运行 `reconnect_loop()`）
    - `disconnect()` 方法：设置 `shutdown_requested_ = true` → 通知 `reconnect_cv_` → join `reconnect_thread_`（如果 joinable） → 然后执行原有 `release_signaling_client()`
    - `reconnect()` 公共方法：改为设置 `needs_reconnect_ = true` + 通知 cv（触发自动重连流程），而非直接 release + create_and_connect
    - _Bug_Condition: 原 reconnect() 是死代码，disconnect() 不触发重连_
    - _Expected_Behavior: connect 启动重连线程，disconnect 安全停止，reconnect 触发自动重连_
    - _Preservation: connect/disconnect 基本流程不变_
    - _Requirements: 2.6, 2.7, 3.3, 3.4_

  - [ ] 3.3 修改 Linux Impl：新增重连基础设施和 reconnect_loop
    - 在 Linux `WebRtcSignaling::Impl` 中新增成员：`needs_reconnect_`、`shutdown_requested_`（atomic<bool>）、`reconnect_thread_`、`reconnect_mutex_`、`reconnect_cv_`、`connected_at_`、`total_disconnects_`、`total_reconnects_`、`reconnect_attempt_`
    - 新增常量：`kInitialBackoffSec=1`、`kMaxBackoffSec=30`、`kStableConnectionSec=30`、`kHealthCheckIntervalSec=60`
    - 新增 `reconnect_loop()` Linux 版本：
      - 循环 `condition_variable::wait_for(kHealthCheckIntervalSec)` 等待
      - 检查 `shutdown_requested_` → 安全退出
      - 执行健康检查日志（正常 debug，异常 warn）
      - 检查退避重置：`connected == true` 且 `connected_at_` 距今 > 30s → 重置 `reconnect_attempt_ = 0`
      - 检测 `needs_reconnect_`：消费标志 → 计算退避 `min(1 << attempt, 30)` → 等待（可被 shutdown 唤醒） → 调用 `signalingClientConnectSync`
      - 成功：递增 `total_reconnects_`，记录恢复日志
      - 失败：记录错误日志（含 status hex），递增 `reconnect_attempt_`，重设 `needs_reconnect_ = true`
    - 修改 `~Impl()`：`shutdown_requested_ = true` + 通知 cv + join reconnect_thread_ → 然后原有资源释放
    - _Bug_Condition: DISCONNECTED 回调后无代码调用 signalingClientConnectSync_
    - _Expected_Behavior: reconnect_loop 在独立线程中执行指数退避重连_
    - _Preservation: 首次连接流程、资源释放不变_
    - _Requirements: 2.2, 2.3, 2.4, 2.5, 2.6, 2.7_
    - SHALL NOT 在 SDK 回调线程中调用 `signalingClientConnectSync`（会死锁）

  - [ ] 3.4 修改 on_signaling_state_changed 回调（Linux Impl）
    - CONNECTED 状态：记录 `connected_at_` 时间戳
    - DISCONNECTED 状态：设置 `needs_reconnect_ = true`，递增 `total_disconnects_`，通知 `reconnect_cv_`，日志打印断连持续时长（`now - connected_at_`）和累计断连次数
    - READY 状态：日志区分首次连接（`total_disconnects_ == 0`）与断连恢复（`total_disconnects_ > 0`）
    - _Bug_Condition: 原回调仅设置 connected = false，不触发重连_
    - _Expected_Behavior: DISCONNECTED 设置 needs_reconnect_ 通知重连线程_
    - _Requirements: 2.1, 2.8, 2.9_

  - [ ] 3.5 增强日志可观测性
    - `log_health_status()`：正常连接 debug 级别（含连接持续时长、累计断连/重连次数），异常状态 warn 级别
    - `send_answer()` / `send_ice_candidate()`：未连接时打印当前 signaling 状态码（而非仅 "not connected"）
    - 重连尝试日志：打印当前重连次数、退避等待时间（由 reconnect_loop 内部处理）
    - 重连成功/失败日志：成功打印恢复耗时和总重连次数，失败打印错误信息和已尝试次数
    - 所有日志使用 "webrtc" logger
    - _Requirements: 2.8, 2.9, 2.10, 2.11, 2.12, 2.13, 3.7_

  - [ ] 3.6 新增 stub 断连自动重连单元测试
    - 在 `device/tests/webrtc_test.cpp` 中新增测试：
    - `StubAutoReconnectAfterDisconnect`: connect → simulate_disconnect → sleep 适当时间 → assert is_connected() == true
    - `ShutdownPreventsReconnect`: connect → disconnect()（正常关闭） → sleep → assert is_connected() == false（shutdown 阻止重连）
    - `BackoffCalculation`: 验证退避时间计算 1→2→4→8→16→30(cap)
    - `DisconnectSafelyStopsReconnectThread`: connect → disconnect → 无悬挂线程、无 ASan 报告
    - 测试超时：≤ 15 秒（多轮恢复/PBT 分级）
    - SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure`
    - _Requirements: 2.1, 2.2, 2.3, 2.7, 3.1_

  - [ ] 3.7 Verify bug condition exploration test now passes
    - **Property 1: Expected Behavior** — 断连后自动重连恢复 WebSocket
    - **IMPORTANT**: Re-run the SAME test from task 1 — do NOT write a new test
    - The test from task 1 encodes the expected behavior
    - When this test passes, it confirms the expected behavior is satisfied
    - Run bug condition exploration test from step 1
    - **EXPECTED OUTCOME**: Test PASSES (confirms bug is fixed)
    - _Requirements: 2.1, 2.2, 2.3, 2.5, 2.6_

  - [ ] 3.8 Verify preservation tests still pass
    - **Property 2: Preservation** — 非断连场景行为不变
    - **IMPORTANT**: Re-run the SAME tests from task 2 — do NOT write new tests
    - Run preservation property tests from step 2
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions)
    - Confirm all tests still pass after fix (no regressions)

- [ ] 4. Checkpoint — Ensure all tests pass
  - 执行完整构建和测试：`cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过（包括 exploration test、preservation test、新增单元测试、所有现有测试）
  - 确认无 ASan 报告
  - Ensure all tests pass, ask the user if questions arise
  - SHALL NOT 在子代理最终检查点执行 git commit
