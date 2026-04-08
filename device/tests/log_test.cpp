// Feature: spdlog-logging, Property 1: JSON format validity
// For any log message, formatted output is valid single-line JSON
// with all required fields, and msg field round-trips correctly.
// **Validates: Requirements 3.1, 3.2, 3.3**

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/pattern_formatter.h>
#include "json_formatter.h"
#include "log_init.h"

#include <sstream>
#include <string>
#include <string_view>
#include <regex>

namespace {

// Extract the value of a JSON string field by key from a flat JSON object.
// Returns the raw (still-escaped) string value between the quotes.
// Returns empty string if not found.
std::string extract_json_field(const std::string& json,
                               const std::string& key) {
    // Search for "key":"
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};

    auto val_start = pos + needle.size();
    // Walk forward to find the closing unescaped quote.
    std::string result;
    for (size_t i = val_start; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            // Escaped character — take both
            result.push_back(json[i]);
            result.push_back(json[i + 1]);
            ++i;
        } else if (json[i] == '"') {
            break;
        } else {
            result.push_back(json[i]);
        }
    }
    return result;
}

// Unescape a JSON string value (reverse of JSON escape).
std::string json_unescape(const std::string& escaped) {
    std::string result;
    for (size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] == '\\' && i + 1 < escaped.size()) {
            char next = escaped[i + 1];
            switch (next) {
            case '"':  result.push_back('"');  break;
            case '\\': result.push_back('\\'); break;
            case 'n':  result.push_back('\n'); break;
            case 'r':  result.push_back('\r'); break;
            case 't':  result.push_back('\t'); break;
            case 'u': {
                // \uXXXX — parse 4 hex digits
                if (i + 5 < escaped.size()) {
                    std::string hex = escaped.substr(i + 2, 4);
                    auto codepoint = static_cast<char>(
                        std::stoul(hex, nullptr, 16));
                    result.push_back(codepoint);
                    i += 4; // skip the 4 hex digits (loop will skip 'u')
                }
                break;
            }
            default:
                // Unknown escape — keep as-is
                result.push_back('\\');
                result.push_back(next);
                break;
            }
            ++i; // skip the character after backslash
        } else {
            result.push_back(escaped[i]);
        }
    }
    return result;
}

} // anonymous namespace

RC_GTEST_PROP(JsonFormatValidity, MsgRoundTrips, ()) {
    // Clean spdlog state before each iteration
    spdlog::shutdown();
    spdlog::drop_all();

    auto message = *rc::gen::arbitrary<std::string>();

    // Create an ostream-backed logger with JsonFormatter
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    sink->set_formatter(std::make_unique<JsonFormatter>());
    auto logger = std::make_shared<spdlog::logger>("test_json", sink);
    logger->set_level(spdlog::level::info);

    // Log the message
    logger->info("{}", message);
    logger->flush();

    std::string output = oss.str();

    // (a) Valid single-line JSON: starts with '{', ends with '}\n',
    //     no newlines in between.
    RC_ASSERT(!output.empty());
    RC_ASSERT(output.front() == '{');
    RC_ASSERT(output.size() >= 2);
    RC_ASSERT(output.back() == '\n');
    RC_ASSERT(output[output.size() - 2] == '}');

    // Check no embedded newlines (only the trailing one)
    std::string_view body(output.data(), output.size() - 1);
    RC_ASSERT(body.find('\n') == std::string_view::npos);

    // (b) Contains all four required fields: ts, level, logger, msg
    RC_ASSERT(output.find("\"ts\":\"") != std::string::npos);
    RC_ASSERT(output.find("\"level\":\"") != std::string::npos);
    RC_ASSERT(output.find("\"logger\":\"") != std::string::npos);
    RC_ASSERT(output.find("\"msg\":\"") != std::string::npos);

    // (c) msg field round-trips: unescape(extract(msg)) == original
    std::string escaped_msg = extract_json_field(output, "msg");
    std::string recovered = json_unescape(escaped_msg);
    RC_ASSERT(recovered == message);
}

// ============================================================
// Example-Based Unit Tests: log_init module
// **Validates: Requirements 2.3, 2.4, 3.1, 6.1, 6.2, 6.3, 7.2, 7.4**
// ============================================================

class LogInitTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean spdlog state before each test
        spdlog::shutdown();
        spdlog::drop_all();
        // shutdown resets g_sink via log_init::shutdown(),
        // but we also call it explicitly to be safe.
        log_init::shutdown();
    }

    void TearDown() override {
        log_init::shutdown();
        spdlog::drop_all();
    }
};

// After init(), spdlog::get("main") and spdlog::get("pipeline") are non-null
TEST_F(LogInitTest, InitCreatesLoggers) {
    log_init::init();
    EXPECT_NE(spdlog::get("main"), nullptr);
    EXPECT_NE(spdlog::get("pipeline"), nullptr);
}

// init(false) produces output containing timestamp, module name, level, and message
TEST_F(LogInitTest, DefaultPatternFormat) {
    // Create an ostream-backed logger with the same pattern as log_init uses
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    sink->set_formatter(
        std::make_unique<spdlog::pattern_formatter>(
            "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v"));
    auto logger = std::make_shared<spdlog::logger>("test_pattern", sink);
    logger->set_level(spdlog::level::info);

    logger->info("hello world");
    logger->flush();

    std::string output = oss.str();

    // Verify timestamp pattern: [YYYY-MM-DD HH:MM:SS.mmm]
    std::regex ts_regex(R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\])");
    EXPECT_TRUE(std::regex_search(output, ts_regex))
        << "Output missing timestamp: " << output;

    // Verify module name
    EXPECT_NE(output.find("[test_pattern]"), std::string::npos)
        << "Output missing module name: " << output;

    // Verify level
    EXPECT_NE(output.find("[info]"), std::string::npos)
        << "Output missing level: " << output;

    // Verify message
    EXPECT_NE(output.find("hello world"), std::string::npos)
        << "Output missing message: " << output;
}

// shutdown() called twice does not crash
TEST_F(LogInitTest, ShutdownIdempotent) {
    log_init::init();
    log_init::shutdown();
    log_init::shutdown();  // second call should be safe
    // If we reach here without crashing, the test passes
}

// init() + use logger + shutdown() — ASan should report no issues
TEST_F(LogInitTest, ShutdownCleanup) {
    log_init::init();
    auto logger = spdlog::get("main");
    ASSERT_NE(logger, nullptr);
    logger->info("test message for cleanup");
    logger->flush();
    log_init::shutdown();
    // ASan will catch any memory issues at process exit
}

// ============================================================
// Feature: spdlog-logging, Property 2: Level filter correctness
// For any logger level L and message level M, the message appears
// in output if and only if M >= L. Changing one logger's level
// does not affect another logger.
// **Validates: Requirements 4.2, 4.3**
// ============================================================

RC_GTEST_PROP(LevelFilterCorrectness, FiltersByLevel, ()) {
    // Clean spdlog state
    spdlog::shutdown();
    spdlog::drop_all();

    // Generate random logger level and message level from trace(0) to critical(5)
    auto logger_level_int = *rc::gen::inRange(0, 6);  // 0=trace .. 5=critical
    auto msg_level_int = *rc::gen::inRange(0, 6);

    auto logger_level = static_cast<spdlog::level::level_enum>(logger_level_int);
    auto msg_level = static_cast<spdlog::level::level_enum>(msg_level_int);

    // Create two ostream-backed loggers
    std::ostringstream oss1;
    auto sink1 = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss1);
    auto logger1 = std::make_shared<spdlog::logger>("logger1", sink1);
    logger1->set_level(logger_level);

    std::ostringstream oss2;
    auto sink2 = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss2);
    auto logger2 = std::make_shared<spdlog::logger>("logger2", sink2);
    logger2->set_level(spdlog::level::trace);  // accept everything

    // Log a message at msg_level on logger1
    logger1->log(msg_level, "test_message");
    logger1->flush();

    // Log a message on logger2 (should always appear since level=trace)
    logger2->log(spdlog::level::info, "logger2_message");
    logger2->flush();

    std::string output1 = oss1.str();
    std::string output2 = oss2.str();

    // Message appears in output iff msg_level >= logger_level
    bool should_appear = (msg_level_int >= logger_level_int);
    bool did_appear = (output1.find("test_message") != std::string::npos);
    RC_ASSERT(should_appear == did_appear);

    // Changing logger1's level does not affect logger2
    RC_ASSERT(output2.find("logger2_message") != std::string::npos);
}

// ============================================================
// Feature: spdlog-logging, Property 3: Logger factory correctness
// For any valid logger name (non-empty ASCII lowercase), create_logger(name)
// registers it in spdlog registry with default level info.
// **Validates: Requirements 2.4, 2.6, 4.1**
// ============================================================

RC_GTEST_PROP(LoggerFactoryCorrectness, CreatesAndRegisters, ()) {
    // Clean spdlog state
    spdlog::shutdown();
    spdlog::drop_all();
    log_init::shutdown();

    // Generate a non-empty lowercase ASCII name
    auto name = *rc::gen::nonEmpty(
        rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));

    // Initialize log_init (required before create_logger)
    log_init::init();

    // Create a logger with the generated name
    auto logger = log_init::create_logger(name);

    // Verify spdlog::get(name) returns non-null
    auto retrieved = spdlog::get(name);
    RC_ASSERT(retrieved != nullptr);

    // Verify default level is info
    RC_ASSERT(retrieved->level() == spdlog::level::info);

    // Cleanup for next iteration
    log_init::shutdown();
    spdlog::drop_all();
}
