#pragma once
#include <spdlog/formatter.h>
#include <spdlog/details/log_msg.h>
#include <memory>

// Custom JSON single-line formatter for spdlog.
// Output format: {"ts":"2025-01-01T00:00:00.000Z","level":"info","logger":"main","msg":"..."}\n
class JsonFormatter final : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg,
                spdlog::memory_buf_t& dest) override;
    std::unique_ptr<spdlog::formatter> clone() const override;
};
