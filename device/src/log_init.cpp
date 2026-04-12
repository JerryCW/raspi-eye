#include "log_init.h"
#include "config_manager.h"
#include "json_formatter.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>

namespace {
    // Module-level shared sink (thread-safe)
    std::shared_ptr<spdlog::sinks::stderr_color_sink_mt> g_sink;
}

namespace log_init {

void init(bool json) {
    if (g_sink) return;  // idempotent: already initialized

    g_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();

    if (json) {
        g_sink->set_formatter(std::make_unique<JsonFormatter>());
    } else {
        g_sink->set_formatter(
            std::make_unique<spdlog::pattern_formatter>(
                "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v"));
    }

    // Register default loggers
    create_logger("main");
    create_logger("pipeline");
}

std::shared_ptr<spdlog::logger> create_logger(const std::string& name) {
    auto logger = std::make_shared<spdlog::logger>(name, g_sink);
    logger->set_level(spdlog::level::info);
    spdlog::register_logger(logger);
    return logger;
}

void init(const LoggingConfig& config) {
    // Delegate to original overload for sink/formatter setup
    init(config.format == "json");

    // Map config.level string to spdlog level enum
    spdlog::level::level_enum lvl = spdlog::level::info;
    if (config.level == "trace")      lvl = spdlog::level::trace;
    else if (config.level == "debug") lvl = spdlog::level::debug;
    else if (config.level == "info")  lvl = spdlog::level::info;
    else if (config.level == "warn")  lvl = spdlog::level::warn;
    else if (config.level == "error") lvl = spdlog::level::err;

    spdlog::set_level(lvl);
}

void shutdown() {
    spdlog::shutdown();
    g_sink.reset();
}

} // namespace log_init
