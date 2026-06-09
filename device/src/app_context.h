// app_context.h
// Application context: three-phase lifecycle (init/start/stop) with pImpl.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include "config_manager.h"
#include "shutdown_handler.h"

class AppContext {
public:
    AppContext();
    ~AppContext();

    // No copy
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    // Phase 1: config loading + module creation + callback registration
    bool init(const std::string& config_path,
              const ConfigOverrides& overrides,
              std::string* error_msg = nullptr);

    // Phase 2: pipeline build + start + signaling connect + health monitor
    bool start(std::string* error_msg = nullptr);

    // Phase 3: delegate to ShutdownHandler::execute()
    ShutdownSummary stop();

    // Register a callback invoked when the pipeline health enters FATAL.
    // Used by main to request a graceful shutdown (Spec 32 需求 4).
    void set_shutdown_requester(std::function<void()> fn);

    // Returns false only when the health monitor is in FATAL state.
    // Returns true when healthy/degraded/recovering or when no monitor exists.
    // Thread-safe (PipelineHealthMonitor::state() is mutex-protected).
    bool is_healthy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
