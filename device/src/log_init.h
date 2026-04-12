#pragma once
#include <memory>
#include <string>

namespace spdlog { class logger; }

struct LoggingConfig;

namespace log_init {

// Initialize the logging system: create shared stderr sink,
// register "main" and "pipeline" loggers.
// json=true uses JSON single-line format, json=false (default) uses pattern format.
// Default log level is info. Idempotent — safe to call multiple times.
void init(bool json = false);

// Initialize from LoggingConfig: sets format (json/text) and global log level.
// Delegates to init(bool) for sink setup, then applies spdlog level.
void init(const LoggingConfig& config);

// Create a new named logger sharing the same stderr sink and current format.
// Returns shared_ptr<spdlog::logger>, also registered in spdlog global registry.
// Retrieve later via spdlog::get(name).
std::shared_ptr<spdlog::logger> create_logger(const std::string& name);

// Shut down the logging system: call spdlog::shutdown() and release all resources.
// Idempotent — safe to call multiple times.
void shutdown();

} // namespace log_init
