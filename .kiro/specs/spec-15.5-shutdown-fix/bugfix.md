# Bugfix 需求文档

## 简介

Ctrl+C 退出时进程卡死，无法在合理时间内完成退出。根因是 `ShutdownHandler::execute()` 使用 `std::async(std::launch::async, fn)` 执行 shutdown step，而 C++ 标准规定 `std::async` 返回的 `std::future` 在析构时会阻塞等待线程完成。即使 `wait_for` 返回 timeout，future 离开作用域时仍会等待卡住的步骤跑完，导致超时机制形同虚设。同时 `main.cpp` 的信号处理使用 `std::signal` + `GMainLoop`，缺乏二次信号强制退出和全局超时保护。

## Bug 分析

### 当前行为（缺陷）

1.1 WHEN 某个 shutdown step 执行时间超过 kStepTimeout（5秒） THEN `wait_for` 返回 timeout 但 `std::future` 析构时仍阻塞等待该线程完成，导致进程卡死无法继续执行后续 step

1.2 WHEN 用户按下 Ctrl+C 后进程卡在某个 shutdown step THEN 再次按下 Ctrl+C 无法强制退出进程，只能通过 kill -9 终止

1.3 WHEN 全部 shutdown step 的累计执行时间超过 kTotalTimeout（30秒） THEN 进程仍然不会强制退出，因为 total timeout 检查只在启动下一个 step 前生效，当前卡住的 step 仍会阻塞

1.4 WHEN 信号处理函数中调用 `g_main_loop_quit()` THEN GMainLoop 可能不会立即退出（取决于当前 dispatch 状态），且 `std::signal` 在信号处理函数中调用非 async-signal-safe 函数存在未定义行为风险

1.5 WHEN shutdown step 超时后 `std::future` 析构 THEN 被 detach 的工作线程仍在后台运行，但 future 析构的阻塞行为使得"跳过超时 step"的设计意图无法实现

### 期望行为（正确）

2.1 WHEN 某个 shutdown step 执行时间超过每步超时限制 THEN 系统 SHALL 跳过该 step 并继续执行下一个 step，不因单个 step 卡住而阻塞整个 shutdown 流程

2.2 WHEN 用户第二次按下 Ctrl+C（或发送 SIGINT/SIGTERM） THEN 系统 SHALL 立即调用 `_exit()` 强制退出进程

2.3 WHEN 全局超时到达（≤30秒） THEN 系统 SHALL 调用 `_exit()` 强制退出进程，无论当前 step 是否完成

2.4 WHEN 信号处理函数被触发 THEN 系统 SHALL 仅使用 async-signal-safe 操作（如设置 `std::atomic` flag），不在信号处理函数中调用 `g_main_loop_quit()` 等非安全函数

2.5 WHEN `ShutdownHandler::execute()` 完成所有 step（或超时跳过） THEN 系统 SHALL 返回结构化的 `ShutdownSummary`，包含每个 step 的名称、状态（ok/timeout/exception/skipped）和耗时

2.6 WHEN 首次收到 SIGINT/SIGTERM THEN 系统 SHALL 在合理时间内（≤30秒）完成退出流程

### 不变行为（回归防护）

3.1 WHEN 所有 shutdown step 均在超时限制内正常完成 THEN 系统 SHALL 继续按注册的逆序执行所有 step 并正常退出

3.2 WHEN 某个 shutdown step 抛出异常 THEN 系统 SHALL 继续捕获异常并执行后续 step，不因异常中断 shutdown 流程

3.3 WHEN 没有注册任何 shutdown step THEN 系统 SHALL 继续正常完成 shutdown 流程（空操作）

3.4 WHEN 注册了 N 个 shutdown step 且均正常完成 THEN 系统 SHALL 继续按逆序执行且每个 step 恰好执行一次

3.5 WHEN 进程在 macOS 开发环境运行 THEN 系统 SHALL 继续通过 `gst_macos_main()` 包装运行，信号处理和 shutdown 逻辑跨平台兼容

---

## Bug 条件推导

### Bug 条件函数

```pascal
FUNCTION isBugCondition(X)
  INPUT: X of type ShutdownInput  // X = {steps: [Step], step_durations: [Duration]}
  OUTPUT: boolean

  // 当任意一个 shutdown step 的实际执行时间超过 kStepTimeout 时触发 bug
  RETURN EXISTS step IN X.steps WHERE step.actual_duration > kStepTimeout
END FUNCTION
```

### 属性规约 — Fix Checking

```pascal
// 属性：超时 step 不阻塞后续 step 执行
FOR ALL X WHERE isBugCondition(X) DO
  summary ← ShutdownHandler'::execute(X.steps)

  // 超时的 step 被标记为 timeout，不阻塞
  FOR EACH step IN X.steps WHERE step.actual_duration > kStepTimeout DO
    ASSERT summary[step].status = TIMEOUT
  END FOR

  // 非超时的 step 仍然正常执行
  FOR EACH step IN X.steps WHERE step.actual_duration <= kStepTimeout DO
    ASSERT summary[step].status = OK OR summary[step].status = SKIPPED
  END FOR

  // 总耗时不超过全局超时
  ASSERT summary.total_duration <= kTotalTimeout + tolerance
END FOR
```

### 属性规约 — Preservation Checking

```pascal
// 属性：非 bug 输入下行为不变
FOR ALL X WHERE NOT isBugCondition(X) DO
  // 所有 step 均在超时内完成时，新旧实现行为一致
  summary_old ← ShutdownHandler::execute(X.steps)
  summary_new ← ShutdownHandler'::execute(X.steps)

  // 执行顺序一致（逆序）
  ASSERT summary_new.execution_order = summary_old.execution_order

  // 所有 step 状态一致
  FOR EACH step IN X.steps DO
    ASSERT summary_new[step].status = summary_old[step].status
  END FOR
END FOR
```
