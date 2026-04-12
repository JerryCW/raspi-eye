# Shutdown 卡死修复 — Bugfix 设计文档

## 概述

`ShutdownHandler::execute()` 使用 `std::async` 执行 shutdown step，但 C++ 标准规定 `std::future` 析构时会阻塞等待线程完成，导致超时机制形同虚设。同时 `main.cpp` 的信号处理使用 `std::signal` + `g_main_loop_quit()`，缺乏二次信号强制退出和全局超时保护。

修复策略：
1. `ShutdownHandler::execute()` 从 `std::async` 改为 `std::thread` + `join` with timeout，超时后 `detach`
2. 信号处理改为 `sigaction` + `std::atomic` flag，第二次信号直接 `_exit()`
3. 主循环退出改为 atomic flag 驱动（GMainLoop 通过 idle callback 轮询 flag）
4. 全局超时到达后 `_exit()` 强制退出
5. `execute()` 返回 `ShutdownSummary` 结构化报告
6. 保持 macOS `gst_macos_main()` 兼容

## 术语表

- **Bug_Condition (C)**：任意 shutdown step 实际执行时间超过 `kStepTimeout`（5秒），触发 `std::future` 析构阻塞
- **Property (P)**：超时 step 被跳过，后续 step 继续执行，总耗时不超过 `kTotalTimeout`
- **Preservation**：所有 step 均在超时内完成时，新旧实现行为一致（逆序执行、异常隔离、空操作）
- **ShutdownHandler**：`device/src/shutdown_handler.{h,cpp}` 中的注册式逆序 shutdown 管理器
- **ShutdownSummary**：新增的结构化返回值，包含每个 step 的名称、状态和耗时
- **StepStatus**：step 执行结果枚举：`OK`、`TIMEOUT`、`EXCEPTION`、`SKIPPED`
- **kStepTimeout**：每步超时限制，5 秒
- **kTotalTimeout**：全局超时限制，30 秒

## Bug 详情

### Bug 条件

当任意 shutdown step 的实际执行时间超过 `kStepTimeout` 时，`std::async` 返回的 `std::future` 在离开作用域时阻塞等待线程完成，导致"超时跳过"的设计意图无法实现，进程卡死。

**形式化规约：**
```
FUNCTION isBugCondition(input)
  INPUT: input of type ShutdownInput  // {steps: [Step], step_durations: [Duration]}
  OUTPUT: boolean

  RETURN EXISTS step IN input.steps
         WHERE step.actual_duration > kStepTimeout
END FUNCTION
```

### 示例

- 用户按 Ctrl+C → `signaling->disconnect()` 卡在网络 I/O 超过 5 秒 → `wait_for` 返回 timeout → `std::future` 析构阻塞 → 进程卡死，后续 `pipeline->stop()` 永远不执行
- 用户按 Ctrl+C → `media_manager.reset()` 等待 WebRTC peer connection 关闭超过 5 秒 → 同上
- 用户按 Ctrl+C → 所有 step 均在 5 秒内完成 → 正常退出（非 bug 条件）
- 用户按 Ctrl+C → 进程卡死后再按 Ctrl+C → 无法强制退出，只能 `kill -9`

## 期望行为

### Preservation 需求

**不变行为：**
- 所有 step 均在超时内完成时，逆序执行顺序不变
- 异常 step 被捕获并记录，后续 step 继续执行
- 无注册 step 时正常完成（空操作）
- 每个 step 恰好执行一次
- macOS 通过 `gst_macos_main()` 包装运行，信号处理跨平台兼容

**范围：**
所有不涉及 step 超时的 shutdown 输入应完全不受此修复影响，包括：
- 所有 step 均正常完成的场景
- step 抛出异常的场景
- 空 step 列表的场景

## 假设根因

基于 bug 分析，最可能的问题：

1. **`std::async` + `std::future` 析构阻塞**：C++ 标准规定 `std::async` 返回的 `std::future` 在析构时必须等待关联线程完成。即使 `wait_for` 返回 `timeout`，`future` 离开 try-catch 块作用域时仍会阻塞。这是根本原因。

2. **信号处理不安全**：`std::signal` 的 handler 中调用 `g_main_loop_quit()` 是非 async-signal-safe 操作，存在未定义行为风险。且没有二次信号强制退出机制。

3. **全局超时检查时机不当**：`kTotalTimeout` 检查仅在启动下一个 step 前生效，当前卡住的 step 不受全局超时约束。

4. **GMainLoop 退出不可控**：`g_main_loop_quit()` 在信号 handler 中调用时，GMainLoop 可能不会立即退出（取决于当前 dispatch 状态）。

## 正确性属性

Property 1: Bug Condition — 超时 step 不阻塞后续执行

_For any_ shutdown 输入，其中存在至少一个 step 的实际执行时间超过 kStepTimeout（isBugCondition 返回 true），修复后的 `ShutdownHandler::execute()` SHALL 在 kStepTimeout + tolerance 内跳过该 step 并继续执行后续 step，超时 step 标记为 TIMEOUT，非超时 step 正常执行，总耗时不超过 kTotalTimeout + tolerance。

**Validates: Requirements 2.1, 2.3, 2.5**

Property 2: Preservation — 非超时输入行为不变

_For any_ shutdown 输入，其中所有 step 均在 kStepTimeout 内完成（isBugCondition 返回 false），修复后的 `ShutdownHandler::execute()` SHALL 产生与原实现相同的执行顺序（逆序）、相同的 step 状态（OK 或 EXCEPTION），保持异常隔离和空操作行为不变。

**Validates: Requirements 3.1, 3.2, 3.3, 3.4**

## 修复实现

### 变更概览

假设根因分析正确，需要修改以下文件：

**文件**: `device/src/shutdown_handler.h`

**变更**:
1. **新增 `StepStatus` 枚举和 `StepResult`/`ShutdownSummary` POD 结构体**：公开到头文件，供调用方使用
2. **`execute()` 返回值改为 `ShutdownSummary`**：从 `void` 改为返回结构化报告

**文件**: `device/src/shutdown_handler.cpp`

**函数**: `ShutdownHandler::execute()`

**具体变更**:
1. **替换 `std::async` 为 `std::thread`**：`std::thread` 的 `detach()` 不会阻塞，超时后可安全放弃该线程
   - 创建 `std::thread` 执行 step lambda
   - 使用 `std::condition_variable` + `std::mutex` 等待 step 完成或超时
   - 超时后调用 `thread.detach()` 放弃该线程，继续下一个 step
   - 正常完成后调用 `thread.join()` 回收线程

2. **全局超时强制退出**：在 `execute()` 入口启动一个 watchdog 线程
   - watchdog 线程 sleep `kTotalTimeout` 后调用 `_exit(EXIT_FAILURE)`
   - `execute()` 正常完成时设置 atomic flag 通知 watchdog 线程退出
   - watchdog 线程 detach（不 join，避免阻塞）

3. **返回 `ShutdownSummary`**：收集每个 step 的名称、状态、耗时，返回结构化报告

4. **新增 `SKIPPED` 状态**：全局超时到达后，剩余未执行的 step 标记为 `SKIPPED`

**文件**: `device/src/main.cpp`

**具体变更**:
1. **信号处理改为 `sigaction` + atomic flag**：
   - 使用 `std::atomic<int> g_signal_count{0}` 记录信号次数
   - 使用 `std::atomic<bool> g_shutdown_requested{false}` 作为退出 flag
   - 信号 handler 仅设置 atomic flag（async-signal-safe）
   - 第二次信号直接调用 `_exit(EXIT_FAILURE)`

2. **GMainLoop 退出改为 idle callback 轮询**：
   - 注册 `g_idle_add()` 回调，轮询 `g_shutdown_requested` flag
   - flag 为 true 时在 idle callback 中调用 `g_main_loop_quit()`（在 GLib 主线程上下文中调用，安全）

3. **`ctx.stop()` 后处理 `ShutdownSummary`**：记录 shutdown 报告到日志

**文件**: `device/src/app_context.h` / `device/src/app_context.cpp`

**具体变更**:
1. **`stop()` 返回值改为 `ShutdownSummary`**：透传 `ShutdownHandler::execute()` 的返回值

### 接口变更

```cpp
// shutdown_handler.h — 新增公开类型

enum class StepStatus { OK, TIMEOUT, EXCEPTION, SKIPPED };

struct StepResult {
    std::string name;
    StepStatus status;
    int64_t duration_ms;
};

struct ShutdownSummary {
    std::vector<StepResult> steps;
    int64_t total_duration_ms;
};

class ShutdownHandler {
public:
    // ...（不变）
    ShutdownSummary execute();  // 返回值从 void 改为 ShutdownSummary
};
```

```cpp
// app_context.h — 变更
#include "shutdown_handler.h"  // 需要 ShutdownSummary

class AppContext {
public:
    // ...
    ShutdownSummary stop();  // 返回值从 void 改为 ShutdownSummary
};
```

### 跨平台兼容

- `sigaction` 在 macOS 和 Linux 上均可用，无需条件编译
- `_exit()` 是 POSIX 标准，macOS 和 Linux 均支持
- `std::atomic`、`std::thread`、`std::condition_variable` 是 C++17 标准，跨平台
- macOS 上 `gst_macos_main()` 包装不受影响，信号处理在 `run_pipeline()` 内部注册

### 禁止项（Design 层）

- SHALL NOT 在信号处理函数中调用非 async-signal-safe 函数（如 `g_main_loop_quit()`、`spdlog` 日志、`malloc`）
- SHALL NOT 使用 `std::async` 执行 shutdown step（根因所在）
- SHALL NOT 在 `execute()` 中使用 `std::future`（析构阻塞问题）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息

## 测试策略

### 验证方法

测试策略分两阶段：先在未修复代码上验证 bug 存在（探索性测试），再在修复后验证 fix 正确性和 preservation。

### 探索性 Bug 条件验证

**目标**：在未修复代码上复现 bug，确认或否定根因分析。如果否定，需要重新假设根因。

**测试计划**：编写测试注册一个 sleep 超过 kStepTimeout 的 step，验证 `execute()` 的实际耗时是否远超 kStepTimeout（即 future 析构阻塞）。在未修复代码上运行，预期观察到阻塞。

**测试用例**：
1. **单个超时 step 测试**：注册一个 sleep 8 秒的 step，验证 `execute()` 耗时 > 7 秒（未修复代码上会阻塞等待 8 秒）
2. **超时 step + 正常 step 测试**：注册一个 sleep 8 秒的 step 和一个正常 step，验证正常 step 是否被阻塞延迟执行
3. **二次信号测试**：手动测试，按两次 Ctrl+C 验证进程是否仍然卡死

**预期反例**：
- `execute()` 耗时约等于超时 step 的实际 sleep 时间（而非 kStepTimeout），证明 future 析构阻塞
- 可能原因：`std::async` 返回的 `std::future` 析构时 `wait()` 阻塞

### Fix Checking

**目标**：验证对所有满足 bug 条件的输入，修复后的函数产生期望行为。

**伪代码：**
```
FOR ALL input WHERE isBugCondition(input) DO
  summary := ShutdownHandler'::execute(input.steps)

  // 超时 step 标记为 TIMEOUT
  FOR EACH step IN input.steps WHERE step.actual_duration > kStepTimeout DO
    ASSERT summary[step].status = TIMEOUT
  END FOR

  // 非超时 step 正常执行
  FOR EACH step IN input.steps WHERE step.actual_duration <= kStepTimeout DO
    ASSERT summary[step].status = OK OR summary[step].status = SKIPPED
  END FOR

  // 总耗时受控
  ASSERT summary.total_duration_ms <= kTotalTimeout_ms + tolerance_ms
END FOR
```

### Preservation Checking

**目标**：验证对所有不满足 bug 条件的输入，修复后的函数与原函数行为一致。

**伪代码：**
```
FOR ALL input WHERE NOT isBugCondition(input) DO
  summary_new := ShutdownHandler'::execute(input.steps)

  // 执行顺序一致（逆序）
  ASSERT summary_new.execution_order = reverse(input.steps)

  // 所有 step 状态一致
  FOR EACH step IN input.steps DO
    ASSERT summary_new[step].status = OK OR summary_new[step].status = EXCEPTION
  END FOR
END FOR
```

**测试方法**：Property-based testing 适合 preservation checking，因为：
- 可自动生成大量随机 step 配置（数量、是否抛异常）
- 覆盖手动测试难以穷举的边界组合
- 强保证非 bug 输入下行为不变

**测试计划**：在未修复代码上先观察正常 step 的行为（逆序执行、异常隔离），然后编写 PBT 验证修复后行为一致。

**测试用例**：
1. **逆序执行 Preservation**：生成随机数量（1-20）的正常 step，验证修复后仍按逆序执行
2. **异常隔离 Preservation**：生成随机 step 配置（部分抛异常），验证修复后异常 step 不影响其他 step 执行
3. **空 step 列表 Preservation**：验证无注册 step 时正常完成

### 单元测试

- 单个超时 step：验证 `execute()` 在 kStepTimeout + tolerance 内返回，step 状态为 TIMEOUT
- 多个 step 混合超时：验证超时 step 不阻塞后续 step，非超时 step 正常执行
- ShutdownSummary 结构：验证返回值包含正确的 step 名称、状态、耗时
- 全局超时：验证累计超时后剩余 step 标记为 SKIPPED
- 二次信号强制退出：验证 atomic flag 机制（信号处理逻辑的单元测试）

### Property-Based 测试

- 生成随机 step 配置（数量 1-20，每个 step 随机 sleep 0-100ms 或抛异常），验证逆序执行和异常隔离不变
- 生成包含超时 step 的随机配置，验证超时 step 被正确标记且不阻塞后续 step
- 生成随机 step 数量，验证 ShutdownSummary 中 step 数量与注册数量一致

### 集成测试

- 完整 shutdown 流程：通过 AppContext 注册真实 shutdown step，验证 `stop()` 返回正确的 ShutdownSummary
- 信号处理集成：验证 sigaction 注册后，atomic flag 正确设置
- GMainLoop 退出集成：验证 idle callback 轮询 flag 后 GMainLoop 正常退出
