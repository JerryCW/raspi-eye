#include "log_init.h"
#include "config_manager.h"
#include "json_formatter.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>
#include <array>
#include <cstdarg>
#include <cstdio>

namespace {
    // Module-level shared sink (thread-safe)
    std::shared_ptr<spdlog::sinks::stderr_color_sink_mt> g_sink;

    // All named loggers created at init time
    constexpr std::array<const char*, 9> kLoggerNames = {
        "main", "pipeline", "app", "config", "stream",
        "ai", "kvs", "webrtc", "s3"
    };
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

    // Register all named loggers
    for (const auto* name : kLoggerNames) {
        create_logger(name);
    }
}

std::shared_ptr<spdlog::logger> create_logger(const std::string& name) {
    auto logger = std::make_shared<spdlog::logger>(name, g_sink);
    logger->set_level(spdlog::level::info);
    spdlog::register_logger(logger);
    return logger;
}

std::optional<spdlog::level::level_enum> parse_level(const std::string& level_str) {
    if (level_str == "trace") return spdlog::level::trace;
    if (level_str == "debug") return spdlog::level::debug;
    if (level_str == "info")  return spdlog::level::info;
    if (level_str == "warn")  return spdlog::level::warn;
    if (level_str == "error") return spdlog::level::err;
    return std::nullopt;
}

void init(const LoggingConfig& config) {
    // Delegate to original overload for sink/formatter setup
    init(config.format == "json");

    // Map config.level string to spdlog level enum using parse_level
    auto global_lvl = parse_level(config.level);
    spdlog::set_level(global_lvl.value_or(spdlog::level::info));

    // Apply per-component levels
    for (const auto& [name, level_str] : config.component_levels) {
        auto logger = spdlog::get(name);
        if (!logger) {
            auto config_log = spdlog::get("config");
            if (config_log) {
                config_log->warn("Unknown component in component_levels: {}", name);
            }
            continue;
        }
        auto lvl = parse_level(level_str);
        if (lvl) {
            logger->set_level(*lvl);
        }
    }
}

void shutdown() {
    spdlog::shutdown();
    g_sink.reset();
}

} // namespace log_init

// --- KVS SDK log redirect ---

#ifdef HAVE_KVS_WEBRTC_SDK
#include <com/amazonaws/kinesis/video/common/PlatformUtils.h>

namespace {
void kvs_log_callback(UINT32 level, PCHAR tag, PCHAR fmt, ...) {
    auto logger = spdlog::get("kvs");
    if (!logger) return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Strip trailing newlines/carriage returns
    std::string msg(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
        msg.pop_back();
    }

    // KVS SDK level mapping:
    // 1 (VERBOSE) -> trace, 2 (DEBUG) -> debug, 3 (INFO) -> info,
    // 4 (WARN) -> warn, 5+ (ERROR/default) -> error
    spdlog::level::level_enum spdlog_level;
    switch (level) {
        case 1: spdlog_level = spdlog::level::trace; break;
        case 2: spdlog_level = spdlog::level::debug; break;
        case 3: spdlog_level = spdlog::level::info; break;
        case 4: spdlog_level = spdlog::level::warn; break;
        default: spdlog_level = spdlog::level::err; break;
    }
    logger->log(spdlog_level, "[{}] {}", tag ? tag : "", msg);
}
}  // namespace

namespace log_init {
void setup_kvs_log_redirect() {
    globalCustomLogPrintFn = kvs_log_callback;
}
}  // namespace log_init

#else

namespace log_init {
void setup_kvs_log_redirect() {
    // No-op: KVS WebRTC SDK not available on this platform
}
}  // namespace log_init

#endif
