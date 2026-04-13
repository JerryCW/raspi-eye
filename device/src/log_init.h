#pragma once
#include <memory>
#include <optional>
#include <string>

#include <spdlog/common.h>

namespace spdlog { class logger; }

struct LoggingConfig;

namespace log_init {

// Initialize the logging system: create shared stderr sink,
// register all named loggers (main, pipeline, app, config, stream, ai, kvs, webrtc, s3).
// json=true uses JSON single-line format, json=false (default) uses pattern format.
// Default log level is info. Idempotent — safe to call multiple times.
void init(bool json = false);

// Initialize from LoggingConfig: sets format (json/text), global log level,
// and per-component levels from config.component_levels.
// Delegates to init(bool) for sink setup, then applies levels.
void init(const LoggingConfig& config);

// Create a new named logger sharing the same stderr sink and current format.
// Returns shared_ptr<spdlog::logger>, also registered in spdlog global registry.
// Retrieve later via spdlog::get(name).
std::shared_ptr<spdlog::logger> create_logger(const std::string& name);

// Convert a level string (trace/debug/info/warn/error) to spdlog level enum.
// Returns nullopt for invalid strings.
std::optional<spdlog::level::level_enum> parse_level(const std::string& level_str);

// Register KVS SDK log callback to redirect SDK logs to the "kvs" spdlog logger.
// No-op on platforms without KVS WebRTC SDK (macOS).
void setup_kvs_log_redirect();

// Shut down the logging system: call spdlog::shutdown() and release all resources.
// Idempotent — safe to call multiple times.
void shutdown();

} // namespace log_init
