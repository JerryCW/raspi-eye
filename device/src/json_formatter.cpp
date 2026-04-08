#include "json_formatter.h"
#include <spdlog/details/log_msg.h>
#include <spdlog/common.h>
#include <chrono>
#include <cstdio>
#include <string_view>

namespace {

// Append a string_view to the memory buffer.
void buf_append(spdlog::memory_buf_t& dest, std::string_view sv) {
    dest.append(sv.data(), sv.data() + sv.size());
}

// Append a JSON-escaped version of `sv` into `dest`.
// Escapes: " -> \", \ -> \\, \n -> \n, \r -> \r, \t -> \t,
// other control chars (0x00-0x1F) -> \uXXXX.
void json_escape(std::string_view sv, spdlog::memory_buf_t& dest) {
    for (unsigned char ch : sv) {
        switch (ch) {
        case '"':
            dest.push_back('\\');
            dest.push_back('"');
            break;
        case '\\':
            dest.push_back('\\');
            dest.push_back('\\');
            break;
        case '\n':
            dest.push_back('\\');
            dest.push_back('n');
            break;
        case '\r':
            dest.push_back('\\');
            dest.push_back('r');
            break;
        case '\t':
            dest.push_back('\\');
            dest.push_back('t');
            break;
        default:
            if (ch < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(ch));
                dest.append(buf, buf + 6);
            } else {
                dest.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
}

} // anonymous namespace

void JsonFormatter::format(const spdlog::details::log_msg& msg,
                           spdlog::memory_buf_t& dest) {
    // 1. Convert msg.time to UTC ISO 8601 with millisecond precision.
    auto epoch = msg.time.time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(epoch);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(epoch)
                  - std::chrono::duration_cast<std::chrono::milliseconds>(secs);
    std::time_t tt = std::chrono::system_clock::to_time_t(msg.time);
    std::tm utc{};
    gmtime_r(&tt, &utc);

    char ts_buf[32];
    std::snprintf(ts_buf, sizeof(ts_buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(millis.count()));

    // 2. Level string
    auto level_sv = spdlog::level::to_string_view(msg.level);

    // 3. Build JSON: {"ts":"...","level":"...","logger":"...","msg":"..."}\n
    buf_append(dest, R"({"ts":")");
    buf_append(dest, ts_buf);
    buf_append(dest, R"(","level":")");
    buf_append(dest, std::string_view(level_sv.data(), level_sv.size()));
    buf_append(dest, R"(","logger":")");
    json_escape(std::string_view(msg.logger_name.data(),
                                 msg.logger_name.size()), dest);
    buf_append(dest, R"(","msg":")");
    json_escape(std::string_view(msg.payload.data(),
                                 msg.payload.size()), dest);
    buf_append(dest, "\"}\n");
}

std::unique_ptr<spdlog::formatter> JsonFormatter::clone() const {
    return std::make_unique<JsonFormatter>();
}
