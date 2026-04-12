# 实现计划

- [x] 1. 编写 Bug Condition 探索性测试
  - **Property 1: Bug Condition** — 超时 step 阻塞 execute() 返回
  - **重要**：此测试必须在修复代码之前编写并运行
  - **目标**：复现 `std::async` + `std::future` 析构阻塞的 bug，确认根因分析正确
  - **Scoped PBT 方法**：将属性范围限定到具体的失败场景——注册一个 sleep 超过 kStepTimeout 的 step
  - 在 `device/tests/shutdown_test.cpp` 中新增测试用例 `BugCondition_TimeoutStepBlocks`
  - 注册一个 sleep 8 秒的 step（超过 kStepTimeout=5s），再注册一个正常 step
  - 测试断言：`execute()` 的总耗时应 ≤ kStepTimeout + 1s tolerance（约 6 秒）
  - 测试断言：正常 step 应被执行（因为逆序执行，正常 step 先执行，超时 step 后执行）
  - 在未修复代码上运行——预期 FAIL（`execute()` 耗时约 8 秒，因为 future 析构阻塞）
  - 失败即确认 bug 存在：`std::future` 析构等待线程完成，超时机制形同虚设
  - 记录反例：`execute()` 实际耗时 vs 预期耗时
  - 测试通过 `ctest --test-dir device/build --output-on-failure -R shutdown_test` 运行
  - SHALL NOT 直接运行测试可执行文件
  - _Requirements: 1.1, 1.5_

- [x] 2. 编写 Preservation 属性测试（修复前）
  - **Property 2: Preservation** — 非超时输入行为不变
  - **重要**：遵循观察优先方法论
  - 在 `device/tests/shutdown_test.cpp` 中新增 PBT 测试
  - 观察：在未修复代码上，所有 step 均在超时内完成时的行为
    - 观察：逆序执行顺序不变
    - 观察：异常 step 被捕获，后续 step 继续执行
    - 观察：空 step 列表正常完成
  - 编写 PBT 测试 `Preservation_ReverseOrderWithSummary`：生成随机数量（1-20）的正常 step，验证 `execute()` 返回的 `ShutdownSummary` 中 step 按逆序排列，所有状态为 OK
  - 编写 PBT 测试 `Preservation_ExceptionIsolationWithSummary`：生成随机 step 配置（部分抛异常），验证异常 step 状态为 EXCEPTION，非异常 step 状态为 OK，执行顺序为逆序
  - 编写示例测试 `Preservation_EmptyStepsReturnsSummary`：验证空 step 列表返回空 ShutdownSummary
  - **注意**：这些测试依赖 `ShutdownSummary` 返回值，需要先完成接口变更（头文件中新增 `StepStatus`/`StepResult`/`ShutdownSummary` 类型，`execute()` 返回 `ShutdownSummary`），但 execute() 内部实现暂不修改（仍用 `std::async`）
  - 在未修复代码上运行——预期 PASS（非超时场景下旧实现行为正确）
  - 测试通过 `ctest --test-dir device/build --output-on-failure -R shutdown_test` 运行
  - SHALL NOT 直接运行测试可执行文件
  - SHALL NOT 使用 `cat <<` heredoc 方式写入文件
  - _Requirements: 3.1, 3.2, 3.3, 3.4_

- [x] 3. Shutdown 卡死修复

  - [x] 3.1 修改 `shutdown_handler.h`：新增公开类型和接口变更
    - 将 `StepStatus` 枚举从 .cpp 匿名命名空间移到头文件，新增 `SKIPPED` 状态
    - 新增 `StepResult` POD 结构体（name, status, duration_ms）
    - 新增 `ShutdownSummary` POD 结构体（steps, total_duration_ms）
    - `execute()` 返回值从 `void` 改为 `ShutdownSummary`
    - 新增 `status_str()` 公开辅助函数
    - _Bug_Condition: isBugCondition(input) WHERE EXISTS step.actual_duration > kStepTimeout_
    - _Expected_Behavior: execute() 返回 ShutdownSummary，超时 step 标记 TIMEOUT_
    - _Preservation: 非超时输入下逆序执行、异常隔离、空操作行为不变_
    - _Requirements: 2.5_

  - [x] 3.2 修改 `shutdown_handler.cpp`：替换 `std::async` 为 `std::thread` + condition_variable
    - 移除 `std::async` 和 `std::future` 的使用
    - 每个 step 创建 `std::thread` 执行 lambda
    - 使用 `std::condition_variable` + `std::mutex` + `std::atomic<bool>` 等待 step 完成或超时
    - 超时后调用 `thread.detach()` 放弃线程，标记 step 为 TIMEOUT，继续下一个 step
    - 正常完成后调用 `thread.join()` 回收线程
    - 全局超时到达后剩余 step 标记为 SKIPPED
    - 在 `execute()` 入口启动 watchdog 线程：sleep kTotalTimeout 后调用 `_exit(EXIT_FAILURE)`
    - `execute()` 正常完成时设置 atomic flag 通知 watchdog 退出
    - watchdog 线程 detach（不 join）
    - 收集每个 step 的 StepResult，返回 ShutdownSummary
    - SHALL NOT 使用 `std::async` 执行 shutdown step
    - SHALL NOT 使用 `std::future`
    - _Bug_Condition: isBugCondition(input) WHERE EXISTS step.actual_duration > kStepTimeout_
    - _Expected_Behavior: 超时 step 在 kStepTimeout + tolerance 内被跳过，后续 step 继续执行_
    - _Preservation: 非超时输入下行为与原实现一致_
    - _Requirements: 2.1, 2.3, 2.5_

  - [x] 3.3 修改 `main.cpp`：信号处理改为 `sigaction` + atomic flag
    - 使用 `std::atomic<int> g_signal_count{0}` 记录信号次数
    - 使用 `std::atomic<bool> g_shutdown_requested{false}` 作为退出 flag
    - 信号 handler 仅设置 atomic flag 和递增计数器（async-signal-safe）
    - 第二次信号直接调用 `_exit(EXIT_FAILURE)`
    - GMainLoop 退出改为 `g_idle_add()` 回调轮询 `g_shutdown_requested` flag
    - idle callback 中检测 flag 为 true 时调用 `g_main_loop_quit()`
    - `ctx.stop()` 返回 `ShutdownSummary` 后记录到日志
    - 保持 macOS `gst_macos_main()` 兼容
    - SHALL NOT 在信号处理函数中调用非 async-signal-safe 函数
    - _Bug_Condition: 二次信号无法强制退出_
    - _Expected_Behavior: 第二次信号直接 _exit()_
    - _Preservation: macOS gst_macos_main() 包装不受影响_
    - _Requirements: 2.2, 2.4, 2.6, 3.5_

  - [x] 3.4 修改 `app_context.h` / `app_context.cpp`：`stop()` 返回 `ShutdownSummary`
    - `stop()` 返回值从 `void` 改为 `ShutdownSummary`
    - 透传 `ShutdownHandler::execute()` 的返回值
    - _Requirements: 2.5_

  - [x] 3.5 验证 Bug Condition 探索性测试现在通过
    - **Property 1: Expected Behavior** — 超时 step 不阻塞后续执行
    - **重要**：重新运行任务 1 中的同一个测试，不要编写新测试
    - 任务 1 的测试编码了期望行为：`execute()` 在 kStepTimeout + tolerance 内返回
    - 当此测试通过时，确认期望行为已满足
    - 运行 `ctest --test-dir device/build --output-on-failure -R shutdown_test`
    - **预期结果**：测试 PASS（确认 bug 已修复）
    - _Requirements: 2.1, 2.5_

  - [x] 3.6 验证 Preservation 测试仍然通过
    - **Property 2: Preservation** — 非超时输入行为不变
    - **重要**：重新运行任务 2 中的同一批测试，不要编写新测试
    - 运行 `ctest --test-dir device/build --output-on-failure -R shutdown_test`
    - **预期结果**：测试 PASS（确认无回归）
    - 确认修复后所有测试仍然通过

- [x] 4. 检查点 — 确保所有测试通过
  - 运行完整测试套件：`ctest --test-dir device/build --output-on-failure -E yolo_test`
  - 确认所有测试通过，包括 shutdown_test 中的新旧测试
  - 确认无 ASan 报告（heap-use-after-free、buffer-overflow 等）
  - 如有问题，询问用户
  - SHALL NOT 在子代理中执行 git commit
