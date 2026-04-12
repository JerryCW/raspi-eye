// app_context.h
// Application context: three-phase lifecycle (init/start/stop) with pImpl.
#pragma once

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
