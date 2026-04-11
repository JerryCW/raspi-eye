// shutdown_handler.cpp
// Implementation of ShutdownHandler: reverse-order cleanup with timeout protection.
#include "shutdown_handler.h"
#include "log_init.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace {

// Per-step timeout
constexpr auto kStepTimeout = std::chrono::seconds(5);

// Total shutdown timeout
constexpr auto kTotalTimeout = std::chrono::seconds(30);

enum class StepStatus { OK, TIMEOUT, EXCEPTION };

const char* status_str(StepStatus s) {
    switch (s) {
        case StepStatus::OK:        return "ok";
        case StepStatus::TIMEOUT:   return "timeout";
        case StepStatus::EXCEPTION: return "exception";
    }
    return "unknown";
}

struct StepResult {
    std::string name;
    StepStatus status;
    int64_t duration_ms;
};

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

void ShutdownHandler::execute() {
    auto logger = spdlog::get("shutdown");
    if (!logger) {
        logger = log_init::create_logger("shutdown");
    }

    const auto& steps = impl_->steps;
    if (steps.empty()) {
        logger->info("shutdown: no steps registered");
        return;
    }

    logger->info("shutdown: starting {} step(s)", steps.size());

    std::vector<StepResult> results;
    results.reserve(steps.size());

    const auto total_start = std::chrono::steady_clock::now();

    // Reverse iteration: last registered step executes first
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        const auto& [name, fn] = *it;

        // Check total timeout before starting next step
        const auto elapsed = std::chrono::steady_clock::now() - total_start;
        if (elapsed >= kTotalTimeout) {
            logger->warn("shutdown: total timeout reached, skipping remaining steps");
            // Record skipped steps as timeout
            for (auto remaining = it; remaining != steps.rend(); ++remaining) {
                if (remaining != it) {
                    results.push_back({remaining->first, StepStatus::TIMEOUT, 0});
                }
            }
            break;
        }

        const auto step_start = std::chrono::steady_clock::now();
        StepStatus status = StepStatus::OK;

        try {
            auto future = std::async(std::launch::async, fn);
            auto wait_result = future.wait_for(kStepTimeout);

            if (wait_result == std::future_status::timeout) {
                status = StepStatus::TIMEOUT;
                logger->warn("shutdown: step [{}] timed out", name);
            } else {
                // Propagate any exception from the async task
                future.get();
            }
        } catch (...) {
            status = StepStatus::EXCEPTION;
            logger->error("shutdown: step [{}] threw exception", name);
        }

        const auto step_end = std::chrono::steady_clock::now();
        const auto duration_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(step_end - step_start).count();

        results.push_back({name, status, duration_ms});
    }

    // Shutdown summary
    logger->info("shutdown summary:");
    for (const auto& r : results) {
        logger->info("  step [{}]: {} ({}ms)", r.name, status_str(r.status),
                     r.duration_ms);
    }
    logger->info("shutdown: complete");
}
