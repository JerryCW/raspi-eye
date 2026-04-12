// shutdown_handler.h
// Registered reverse-order shutdown manager with per-step timeout protection.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Step 执行结果枚举
enum class StepStatus { OK, TIMEOUT, EXCEPTION, SKIPPED };

// 将 StepStatus 转为可读字符串
const char* status_str(StepStatus s);

// 单个 step 的执行结果
struct StepResult {
    std::string name;
    StepStatus status;
    int64_t duration_ms;
};

// execute() 的结构化返回值
struct ShutdownSummary {
    std::vector<StepResult> steps;
    int64_t total_duration_ms;
};

class ShutdownHandler {
public:
    ShutdownHandler();
    ~ShutdownHandler();

    // No copy
    ShutdownHandler(const ShutdownHandler&) = delete;
    ShutdownHandler& operator=(const ShutdownHandler&) = delete;

    // Register a cleanup step (name + lambda).
    // Steps are executed in reverse registration order during execute().
    void register_step(const std::string& name, std::function<void()> fn);

    // Execute all registered steps in reverse order.
    // Per-step timeout: 5 seconds. Total timeout: 30 seconds.
    // Exceptions are caught and logged; execution continues.
    // Returns ShutdownSummary with per-step results and total duration.
    ShutdownSummary execute();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
