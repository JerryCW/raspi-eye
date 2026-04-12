// shutdown_handler.cpp
// Implementation of ShutdownHandler: reverse-order cleanup with timeout protection.
// 使用 std::thread + condition_variable 替代 std::async，超时后 detach 线程避免阻塞。
#include "shutdown_handler.h"
#include "log_init.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <cstdlib>  // EXIT_FAILURE
#include <unistd.h> // _exit

// status_str 公开辅助函数（声明在 shutdown_handler.h）
const char* status_str(StepStatus s) {
    switch (s) {
        case StepStatus::OK:        return "ok";
        case StepStatus::TIMEOUT:   return "timeout";
        case StepStatus::EXCEPTION: return "exception";
        case StepStatus::SKIPPED:   return "skipped";
    }
    return "unknown";
}

namespace {

// Per-step timeout
constexpr auto kStepTimeout = std::chrono::seconds(5);

// Total shutdown timeout
constexpr auto kTotalTimeout = std::chrono::seconds(30);

} // namespace

struct ShutdownHandler::Impl {
    std::vector<std::pair<std::string, std::function<void()>>> steps;
};

ShutdownHandler::ShutdownHandler()
    : impl_(std::make_unique<Impl>()) {}

ShutdownHandler::~ShutdownHandler() = default;

void ShutdownHandler::register_step(const std::string& name,
                                     std::function<void()> fn) {
    impl_->steps.emplace_back(name, std::move(fn));
}

ShutdownSummary ShutdownHandler::execute() {
    auto logger = spdlog::get("shutdown");
    if (!logger) {
        logger = log_init::create_logger("shutdown");
    }

    const auto& steps = impl_->steps;
    if (steps.empty()) {
        logger->info("shutdown: no steps registered");
        return ShutdownSummary{{}, 0};
    }

    logger->info("shutdown: starting {} step(s)", steps.size());

    // Watchdog 线程：全局超时后强制 _exit
    // 使用 shared_ptr 延长 flag 生命周期，避免 detach 后访问已销毁的栈变量
    auto shutdown_complete = std::make_shared<std::atomic<bool>>(false);
    std::thread watchdog([shutdown_complete]() {
        // 以 100ms 为间隔轮询，避免 sleep 整个 kTotalTimeout 后无法响应 flag
        const auto interval = std::chrono::milliseconds(100);
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kTotalTimeout);
        while (remaining_ms.count() > 0) {
            auto sleep_time = std::min(interval, remaining_ms);
            std::this_thread::sleep_for(sleep_time);
            remaining_ms -= sleep_time;
            if (shutdown_complete->load(std::memory_order_acquire)) {
                return;  // 正常完成，watchdog 退出
            }
        }
        // 全局超时，强制退出
        _exit(EXIT_FAILURE);
    });
    watchdog.detach();  // watchdog 不 join

    std::vector<StepResult> results;
    results.reserve(steps.size());

    const auto total_start = std::chrono::steady_clock::now();

    // Reverse iteration: last registered step executes first
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        const auto& [name, fn] = *it;

        // 检查全局超时：剩余 step 标记为 SKIPPED
        const auto elapsed = std::chrono::steady_clock::now() - total_start;
        if (elapsed >= kTotalTimeout) {
            logger->warn("shutdown: total timeout reached, skipping remaining steps");
            for (auto remaining = it; remaining != steps.rend(); ++remaining) {
                results.push_back({remaining->first, StepStatus::SKIPPED, 0});
            }
            break;
        }

        const auto step_start = std::chrono::steady_clock::now();
        StepStatus status = StepStatus::OK;

        // 每个 step 使用 thread + condition_variable 执行
        // 使用 shared_ptr 延长变量生命周期，避免 detach 后访问已销毁的栈变量
        auto step_mtx = std::make_shared<std::mutex>();
        auto step_cv = std::make_shared<std::condition_variable>();
        auto step_done = std::make_shared<std::atomic<bool>>(false);
        auto step_exception_ptr = std::make_shared<std::exception_ptr>(nullptr);

        std::thread worker([&fn, step_done, step_cv, step_exception_ptr]() {
            try {
                fn();
            } catch (...) {
                *step_exception_ptr = std::current_exception();
            }
            step_done->store(true, std::memory_order_release);
            step_cv->notify_one();
        });

        {
            std::unique_lock<std::mutex> lock(*step_mtx);
            if (!step_cv->wait_for(lock, kStepTimeout,
                                  [&] { return step_done->load(std::memory_order_acquire); })) {
                // 超时：detach 线程，标记 TIMEOUT
                worker.detach();
                status = StepStatus::TIMEOUT;
                logger->warn("shutdown: step [{}] timed out", name);
            } else {
                // 正常完成：join 回收线程
                worker.join();
                // 检查是否有异常
                if (*step_exception_ptr) {
                    status = StepStatus::EXCEPTION;
                    logger->error("shutdown: step [{}] threw exception", name);
                }
            }
        }

        const auto step_end = std::chrono::steady_clock::now();
        const auto duration_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(step_end - step_start).count();

        results.push_back({name, status, duration_ms});
    }

    // 通知 watchdog 正常完成
    shutdown_complete->store(true, std::memory_order_release);

    // Shutdown summary
    const auto total_end = std::chrono::steady_clock::now();
    const auto total_duration_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(total_end - total_start).count();

    logger->info("shutdown summary:");
    for (const auto& r : results) {
        logger->info("  step [{}]: {} ({}ms)", r.name, status_str(r.status),
                     r.duration_ms);
    }
    logger->info("shutdown: complete");

    return ShutdownSummary{std::move(results), total_duration_ms};
}
