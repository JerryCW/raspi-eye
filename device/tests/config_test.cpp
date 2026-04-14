// config_test.cpp
// ConfigManager example-based tests + PBT properties.
// Tests pure parse functions and ConfigManager apply_overrides.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include "config_manager.h"
#include <unordered_map>
#include <string>

// ===========================================================================
// Example-based unit tests
// ===========================================================================

// 1. ParseCameraConfig_EmptyMap_Defaults
//    Empty kv map -> returns true, all fields at platform defaults
TEST(ConfigExampleTest, ParseCameraConfig_EmptyMap_Defaults) {
    std::unordered_map<std::string, std::string> kv;
    CameraSource::CameraConfig config;
    std::string err;
    EXPECT_TRUE(parse_camera_config(kv, config, &err));
    EXPECT_EQ(config.type, CameraSource::default_camera_type());
    EXPECT_EQ(config.device, "");
    EXPECT_EQ(config.width, 1280);
    EXPECT_EQ(config.height, 720);
    EXPECT_EQ(config.framerate, 15);
}

// 2. ParseCameraConfig_ValidComplete
//    Complete valid kv map -> fields correctly parsed
TEST(ConfigExampleTest, ParseCameraConfig_ValidComplete) {
    std::unordered_map<std::string, std::string> kv = {
        {"type", "test"},
        {"device", "/dev/video0"},
        {"width", "1920"},
        {"height", "1080"},
        {"framerate", "30"}
    };
    CameraSource::CameraConfig config;
    std::string err;
    EXPECT_TRUE(parse_camera_config(kv, config, &err));
    EXPECT_EQ(config.type, CameraSource::CameraType::TEST);
    EXPECT_EQ(config.device, "/dev/video0");
    EXPECT_EQ(config.width, 1920);
    EXPECT_EQ(config.height, 1080);
    EXPECT_EQ(config.framerate, 30);
}

// 3. ParseCameraConfig_InvalidType
//    type="invalid" -> returns false, error_msg contains "Invalid camera type"
TEST(ConfigExampleTest, ParseCameraConfig_InvalidType) {
    std::unordered_map<std::string, std::string> kv = {{"type", "invalid"}};
    CameraSource::CameraConfig config;
    std::string err;
    EXPECT_FALSE(parse_camera_config(kv, config, &err));
    EXPECT_NE(err.find("Invalid camera type"), std::string::npos);
}

// 4. ParseStreamingConfig_EmptyMap_Defaults
//    Empty kv map -> returns true, all fields at defaults
TEST(ConfigExampleTest, ParseStreamingConfig_EmptyMap_Defaults) {
    std::unordered_map<std::string, std::string> kv;
    StreamingConfig config;
    std::string err;
    EXPECT_TRUE(parse_streaming_config(kv, config, &err));
    EXPECT_EQ(config.bitrate_min_kbps, 1000);
    EXPECT_EQ(config.bitrate_max_kbps, 4000);
    EXPECT_EQ(config.bitrate_step_kbps, 500);
    EXPECT_EQ(config.bitrate_default_kbps, 2500);
    EXPECT_EQ(config.bitrate_eval_interval_sec, 5);
    EXPECT_EQ(config.bitrate_rampup_interval_sec, 30);
    EXPECT_EQ(config.debounce_sec, 3);
}

// 5. ParseStreamingConfig_ValidComplete
//    Complete valid kv map -> fields correctly parsed
TEST(ConfigExampleTest, ParseStreamingConfig_ValidComplete) {
    std::unordered_map<std::string, std::string> kv = {
        {"bitrate_min_kbps", "500"},
        {"bitrate_max_kbps", "8000"},
        {"bitrate_step_kbps", "250"},
        {"bitrate_default_kbps", "3000"},
        {"bitrate_eval_interval_sec", "10"},
        {"bitrate_rampup_interval_sec", "60"},
        {"debounce_sec", "5"}
    };
    StreamingConfig config;
    std::string err;
    EXPECT_TRUE(parse_streaming_config(kv, config, &err));
    EXPECT_EQ(config.bitrate_min_kbps, 500);
    EXPECT_EQ(config.bitrate_max_kbps, 8000);
    EXPECT_EQ(config.bitrate_step_kbps, 250);
    EXPECT_EQ(config.bitrate_default_kbps, 3000);
    EXPECT_EQ(config.bitrate_eval_interval_sec, 10);
    EXPECT_EQ(config.bitrate_rampup_interval_sec, 60);
    EXPECT_EQ(config.debounce_sec, 5);
}

// 6. ParseLoggingConfig_EmptyMap_Defaults
//    Empty kv map -> returns true, level="info", format="text"
TEST(ConfigExampleTest, ParseLoggingConfig_EmptyMap_Defaults) {
    std::unordered_map<std::string, std::string> kv;
    LoggingConfig config;
    std::string err;
    EXPECT_TRUE(parse_logging_config(kv, config, &err));
    EXPECT_EQ(config.level, "info");
    EXPECT_EQ(config.format, "text");
}

// 7. ParseLoggingConfig_ValidComplete
//    level=debug, format=json -> fields correctly parsed
TEST(ConfigExampleTest, ParseLoggingConfig_ValidComplete) {
    std::unordered_map<std::string, std::string> kv = {
        {"level", "debug"},
        {"format", "json"}
    };
    LoggingConfig config;
    std::string err;
    EXPECT_TRUE(parse_logging_config(kv, config, &err));
    EXPECT_EQ(config.level, "debug");
    EXPECT_EQ(config.format, "json");
}

// 8. ParseLoggingConfig_InvalidLevel
//    level="invalid" -> returns false
TEST(ConfigExampleTest, ParseLoggingConfig_InvalidLevel) {
    std::unordered_map<std::string, std::string> kv = {{"level", "invalid"}};
    LoggingConfig config;
    std::string err;
    EXPECT_FALSE(parse_logging_config(kv, config, &err));
}

// 9. ParseLoggingConfig_InvalidFormat
//    format="xml" -> returns false
TEST(ConfigExampleTest, ParseLoggingConfig_InvalidFormat) {
    std::unordered_map<std::string, std::string> kv = {{"format", "xml"}};
    LoggingConfig config;
    std::string err;
    EXPECT_FALSE(parse_logging_config(kv, config, &err));
}

// 10. ValidateStreamingConfig_Valid
//     min=1000, default=2500, max=4000 -> returns true
TEST(ConfigExampleTest, ValidateStreamingConfig_Valid) {
    StreamingConfig config;
    config.bitrate_min_kbps = 1000;
    config.bitrate_default_kbps = 2500;
    config.bitrate_max_kbps = 4000;
    std::string err;
    EXPECT_TRUE(validate_streaming_config(config, &err));
}

// 11. ValidateStreamingConfig_MinGreaterThanMax
//     min=5000, max=1000 -> returns false
TEST(ConfigExampleTest, ValidateStreamingConfig_MinGreaterThanMax) {
    StreamingConfig config;
    config.bitrate_min_kbps = 5000;
    config.bitrate_default_kbps = 2500;
    config.bitrate_max_kbps = 1000;
    std::string err;
    EXPECT_FALSE(validate_streaming_config(config, &err));
}

// 12. ValidateStreamingConfig_DefaultOutOfRange
//     default=5000, min=1000, max=4000 -> returns false
TEST(ConfigExampleTest, ValidateStreamingConfig_DefaultOutOfRange) {
    StreamingConfig config;
    config.bitrate_min_kbps = 1000;
    config.bitrate_default_kbps = 5000;
    config.bitrate_max_kbps = 4000;
    std::string err;
    EXPECT_FALSE(validate_streaming_config(config, &err));
}

// 13. ToBitrateConfig_FieldMapping
//     Verify to_bitrate_config maps each field correctly
TEST(ConfigExampleTest, ToBitrateConfig_FieldMapping) {
    StreamingConfig sc;
    sc.bitrate_min_kbps = 800;
    sc.bitrate_max_kbps = 6000;
    sc.bitrate_step_kbps = 300;
    sc.bitrate_default_kbps = 2000;
    sc.bitrate_eval_interval_sec = 7;
    sc.bitrate_rampup_interval_sec = 45;

    BitrateConfig bc = to_bitrate_config(sc);
    EXPECT_EQ(bc.min_kbps, 800);
    EXPECT_EQ(bc.max_kbps, 6000);
    EXPECT_EQ(bc.step_kbps, 300);
    EXPECT_EQ(bc.default_kbps, 2000);
    EXPECT_EQ(bc.eval_interval_sec, 7);
    EXPECT_EQ(bc.rampup_interval_sec, 45);
}

// 14. ApplyOverrides_CameraType
//     camera_type="test" overrides type
TEST(ConfigExampleTest, ApplyOverrides_CameraType) {
    ConfigManager mgr;
    ConfigOverrides overrides;
    overrides.camera_type = "test";
    std::string err;
    EXPECT_TRUE(mgr.apply_overrides(overrides, &err));
    EXPECT_EQ(mgr.camera_config().type, CameraSource::CameraType::TEST);
}

// 15. ApplyOverrides_LogJson
//     log_json=true overrides format to "json"
TEST(ConfigExampleTest, ApplyOverrides_LogJson) {
    ConfigManager mgr;
    ConfigOverrides overrides;
    overrides.log_json = true;
    std::string err;
    EXPECT_TRUE(mgr.apply_overrides(overrides, &err));
    EXPECT_EQ(mgr.logging_config().format, "json");
}

// 16. ApplyOverrides_InvalidCameraType
//     camera_type="invalid" -> returns false
TEST(ConfigExampleTest, ApplyOverrides_InvalidCameraType) {
    ConfigManager mgr;
    ConfigOverrides overrides;
    overrides.camera_type = "invalid";
    std::string err;
    EXPECT_FALSE(mgr.apply_overrides(overrides, &err));
}

// ===========================================================================
// AiConfig enable 开关测试
// ===========================================================================

// parse_ai_config 空 map → enabled=true（默认值）
TEST(ConfigExampleTest, ParseAiConfig_EmptyMap_EnabledTrue) {
    std::unordered_map<std::string, std::string> kv;
    AiConfig config;
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_TRUE(config.enabled);
}

// parse_ai_config enabled="true" → enabled=true
TEST(ConfigExampleTest, ParseAiConfig_EnabledTrue) {
    std::unordered_map<std::string, std::string> kv = {{"enabled", "true"}};
    AiConfig config;
    config.enabled = false;  // 确保被覆盖
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_TRUE(config.enabled);
}

// parse_ai_config enabled="false" → enabled=false
TEST(ConfigExampleTest, ParseAiConfig_EnabledFalse) {
    std::unordered_map<std::string, std::string> kv = {{"enabled", "false"}};
    AiConfig config;
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_FALSE(config.enabled);
}

// parse_ai_config enabled="TRUE" → enabled=true（大小写不敏感）
TEST(ConfigExampleTest, ParseAiConfig_EnabledTRUE_CaseInsensitive) {
    std::unordered_map<std::string, std::string> kv = {{"enabled", "TRUE"}};
    AiConfig config;
    config.enabled = false;
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_TRUE(config.enabled);
}

// parse_ai_config enabled="invalid" → 返回 false
TEST(ConfigExampleTest, ParseAiConfig_EnabledInvalid) {
    std::unordered_map<std::string, std::string> kv = {{"enabled", "invalid"}};
    AiConfig config;
    std::string err;
    EXPECT_FALSE(parse_ai_config(kv, config, &err));
    EXPECT_NE(err.find("enabled"), std::string::npos);
}

// ===========================================================================
// KvsConfig enable 开关测试
// ===========================================================================

// build_kvs_config enabled 默认 true（提供必填字段，不设 enabled）
TEST(ConfigExampleTest, BuildKvsConfig_EnabledDefault) {
    std::unordered_map<std::string, std::string> kv = {
        {"stream_name", "TestStream"},
        {"aws_region", "us-east-1"}
    };
    KvsSinkFactory::KvsConfig config;
    std::string err;
    EXPECT_TRUE(KvsSinkFactory::build_kvs_config(kv, config, &err));
    EXPECT_TRUE(config.enabled);
}

// build_kvs_config enabled="true" → enabled=true
TEST(ConfigExampleTest, BuildKvsConfig_EnabledTrue) {
    std::unordered_map<std::string, std::string> kv = {
        {"stream_name", "TestStream"},
        {"aws_region", "us-east-1"},
        {"enabled", "true"}
    };
    KvsSinkFactory::KvsConfig config;
    config.enabled = false;
    std::string err;
    EXPECT_TRUE(KvsSinkFactory::build_kvs_config(kv, config, &err));
    EXPECT_TRUE(config.enabled);
}

// build_kvs_config enabled="false" → enabled=false
TEST(ConfigExampleTest, BuildKvsConfig_EnabledFalse) {
    std::unordered_map<std::string, std::string> kv = {
        {"stream_name", "TestStream"},
        {"aws_region", "us-east-1"},
        {"enabled", "false"}
    };
    KvsSinkFactory::KvsConfig config;
    std::string err;
    EXPECT_TRUE(KvsSinkFactory::build_kvs_config(kv, config, &err));
    EXPECT_FALSE(config.enabled);
}

// build_kvs_config enabled="TRUE" → enabled=true（大小写不敏感）
TEST(ConfigExampleTest, BuildKvsConfig_EnabledTRUE_CaseInsensitive) {
    std::unordered_map<std::string, std::string> kv = {
        {"stream_name", "TestStream"},
        {"aws_region", "us-east-1"},
        {"enabled", "TRUE"}
    };
    KvsSinkFactory::KvsConfig config;
    config.enabled = false;
    std::string err;
    EXPECT_TRUE(KvsSinkFactory::build_kvs_config(kv, config, &err));
    EXPECT_TRUE(config.enabled);
}

// build_kvs_config enabled="invalid" → 返回 false
TEST(ConfigExampleTest, BuildKvsConfig_EnabledInvalid) {
    std::unordered_map<std::string, std::string> kv = {
        {"stream_name", "TestStream"},
        {"aws_region", "us-east-1"},
        {"enabled", "invalid"}
    };
    KvsSinkFactory::KvsConfig config;
    std::string err;
    EXPECT_FALSE(KvsSinkFactory::build_kvs_config(kv, config, &err));
    EXPECT_NE(err.find("enabled"), std::string::npos);
}

// ===========================================================================
// WebRtcConfig enable 开关测试
// ===========================================================================

// build_webrtc_config enabled 默认 true（提供必填字段，不设 enabled）
TEST(ConfigExampleTest, BuildWebRtcConfig_EnabledDefault) {
    std::unordered_map<std::string, std::string> kv = {
        {"channel_name", "TestChannel"},
        {"aws_region", "us-east-1"}
    };
    WebRtcConfig config;
    std::string err;
    EXPECT_TRUE(build_webrtc_config(kv, config, &err));
    EXPECT_TRUE(config.enabled);
}

// build_webrtc_config enabled="true" → enabled=true
TEST(ConfigExampleTest, BuildWebRtcConfig_EnabledTrue) {
    std::unordered_map<std::string, std::string> kv = {
        {"channel_name", "TestChannel"},
        {"aws_region", "us-east-1"},
        {"enabled", "true"}
    };
    WebRtcConfig config;
    config.enabled = false;
    std::string err;
    EXPECT_TRUE(build_webrtc_config(kv, config, &err));
    EXPECT_TRUE(config.enabled);
}

// build_webrtc_config enabled="false" → enabled=false
TEST(ConfigExampleTest, BuildWebRtcConfig_EnabledFalse) {
    std::unordered_map<std::string, std::string> kv = {
        {"channel_name", "TestChannel"},
        {"aws_region", "us-east-1"},
        {"enabled", "false"}
    };
    WebRtcConfig config;
    std::string err;
    EXPECT_TRUE(build_webrtc_config(kv, config, &err));
    EXPECT_FALSE(config.enabled);
}

// build_webrtc_config enabled="TRUE" → enabled=true（大小写不敏感）
TEST(ConfigExampleTest, BuildWebRtcConfig_EnabledTRUE_CaseInsensitive) {
    std::unordered_map<std::string, std::string> kv = {
        {"channel_name", "TestChannel"},
        {"aws_region", "us-east-1"},
        {"enabled", "TRUE"}
    };
    WebRtcConfig config;
    config.enabled = false;
    std::string err;
    EXPECT_TRUE(build_webrtc_config(kv, config, &err));
    EXPECT_TRUE(config.enabled);
}

// build_webrtc_config enabled="invalid" → 返回 false
TEST(ConfigExampleTest, BuildWebRtcConfig_EnabledInvalid) {
    std::unordered_map<std::string, std::string> kv = {
        {"channel_name", "TestChannel"},
        {"aws_region", "us-east-1"},
        {"enabled", "invalid"}
    };
    WebRtcConfig config;
    std::string err;
    EXPECT_FALSE(build_webrtc_config(kv, config, &err));
    EXPECT_NE(err.find("enabled"), std::string::npos);
}

// ===========================================================================
// Property-Based Tests (RapidCheck)
// ===========================================================================

// Feature: config-file, Property 1: parse_camera_config fidelity
// **Validates: Requirements 2.1, 2.3**
RC_GTEST_PROP(ConfigPBT, ParseCameraConfigFidelity, ()) {
    // Valid camera types
    static const std::vector<std::string> valid_types = {"test", "v4l2", "libcamera"};

    // Build random kv map subset
    std::unordered_map<std::string, std::string> kv;

    bool has_type = *rc::gen::arbitrary<bool>();
    bool has_device = *rc::gen::arbitrary<bool>();
    bool has_width = *rc::gen::arbitrary<bool>();
    bool has_height = *rc::gen::arbitrary<bool>();
    bool has_framerate = *rc::gen::arbitrary<bool>();

    std::string chosen_type;
    if (has_type) {
        chosen_type = *rc::gen::elementOf(valid_types);
        kv["type"] = chosen_type;
    }
    std::string chosen_device;
    if (has_device) {
        // Non-empty device string
        chosen_device = *rc::gen::nonEmpty<std::string>();
        kv["device"] = chosen_device;
    }
    int chosen_width = 0;
    if (has_width) {
        chosen_width = *rc::gen::inRange(1, 4097);
        kv["width"] = std::to_string(chosen_width);
    }
    int chosen_height = 0;
    if (has_height) {
        chosen_height = *rc::gen::inRange(1, 4097);
        kv["height"] = std::to_string(chosen_height);
    }
    int chosen_framerate = 0;
    if (has_framerate) {
        chosen_framerate = *rc::gen::inRange(1, 4097);
        kv["framerate"] = std::to_string(chosen_framerate);
    }

    CameraSource::CameraConfig config;
    std::string err;
    bool result = parse_camera_config(kv, config, &err);

    RC_ASSERT(result == true);

    // Present fields should be correctly parsed
    if (has_type) {
        CameraSource::CameraType expected_type;
        CameraSource::parse_camera_type(chosen_type, expected_type);
        RC_ASSERT(config.type == expected_type);
    } else {
        RC_ASSERT(config.type == CameraSource::default_camera_type());
    }

    if (has_device) {
        RC_ASSERT(config.device == chosen_device);
    } else {
        RC_ASSERT(config.device == "");
    }

    if (has_width) {
        RC_ASSERT(config.width == chosen_width);
    } else {
        RC_ASSERT(config.width == 1280);
    }

    if (has_height) {
        RC_ASSERT(config.height == chosen_height);
    } else {
        RC_ASSERT(config.height == 720);
    }

    if (has_framerate) {
        RC_ASSERT(config.framerate == chosen_framerate);
    } else {
        RC_ASSERT(config.framerate == 15);
    }
}

// Feature: config-file, Property 2: invalid camera type rejected
// **Validates: Requirements 2.4**
RC_GTEST_PROP(ConfigPBT, InvalidCameraTypeRejected, ()) {
    static const std::vector<std::string> valid_lower = {"test", "v4l2", "libcamera"};

    // Generate random string that is NOT a valid camera type (case-insensitive)
    auto random_str = *rc::gen::nonEmpty<std::string>();

    // Convert to lowercase for comparison
    std::string lower = random_str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Skip if it happens to be a valid type
    RC_PRE(std::find(valid_lower.begin(), valid_lower.end(), lower) == valid_lower.end());

    std::unordered_map<std::string, std::string> kv = {{"type", random_str}};
    CameraSource::CameraConfig config;
    std::string err;
    RC_ASSERT(parse_camera_config(kv, config, &err) == false);
}

// Feature: config-file, Property 3: parse_streaming_config fidelity
// **Validates: Requirements 3.1, 3.3**
RC_GTEST_PROP(ConfigPBT, ParseStreamingConfigFidelity, ()) {
    // Streaming config field names
    static const std::vector<std::string> field_names = {
        "bitrate_min_kbps", "bitrate_max_kbps", "bitrate_step_kbps",
        "bitrate_default_kbps", "bitrate_eval_interval_sec",
        "bitrate_rampup_interval_sec", "debounce_sec"
    };

    // Default values matching StreamingConfig defaults
    static const std::vector<int> defaults = {1000, 4000, 500, 2500, 5, 30, 3};

    // Randomly decide which fields to include
    std::unordered_map<std::string, std::string> kv;
    std::vector<int> chosen_values(7, 0);
    std::vector<bool> has_field(7, false);

    for (size_t i = 0; i < 7; ++i) {
        has_field[i] = *rc::gen::arbitrary<bool>();
        if (has_field[i]) {
            chosen_values[i] = *rc::gen::inRange(0, 10001);
            kv[field_names[i]] = std::to_string(chosen_values[i]);
        }
    }

    StreamingConfig config;
    std::string err;
    bool result = parse_streaming_config(kv, config, &err);
    RC_ASSERT(result == true);

    // Access fields via array of member pointers
    std::vector<int> actual = {
        config.bitrate_min_kbps, config.bitrate_max_kbps,
        config.bitrate_step_kbps, config.bitrate_default_kbps,
        config.bitrate_eval_interval_sec, config.bitrate_rampup_interval_sec,
        config.debounce_sec
    };

    for (size_t i = 0; i < 7; ++i) {
        if (has_field[i]) {
            RC_ASSERT(actual[i] == chosen_values[i]);
        } else {
            RC_ASSERT(actual[i] == defaults[i]);
        }
    }
}

// Feature: config-file, Property 4: validate_streaming_config consistency
// **Validates: Requirements 3.4, 3.5**
RC_GTEST_PROP(ConfigPBT, ValidateStreamingConfigConsistency, ()) {
    StreamingConfig config;
    config.bitrate_min_kbps = *rc::gen::inRange(0, 10001);
    config.bitrate_max_kbps = *rc::gen::inRange(0, 10001);
    config.bitrate_default_kbps = *rc::gen::inRange(0, 10001);

    std::string err;
    bool result = validate_streaming_config(config, &err);

    bool expected = (config.bitrate_min_kbps <= config.bitrate_default_kbps) &&
                    (config.bitrate_default_kbps <= config.bitrate_max_kbps);

    RC_ASSERT(result == expected);
}

// Feature: config-file, Property 5: parse_logging_config fidelity
// **Validates: Requirements 4.1, 4.3**
RC_GTEST_PROP(ConfigPBT, ParseLoggingConfigFidelity, ()) {
    static const std::vector<std::string> valid_levels = {
        "trace", "debug", "info", "warn", "error"
    };
    static const std::vector<std::string> valid_formats = {"text", "json"};

    std::unordered_map<std::string, std::string> kv;

    bool has_level = *rc::gen::arbitrary<bool>();
    bool has_format = *rc::gen::arbitrary<bool>();

    std::string chosen_level;
    if (has_level) {
        chosen_level = *rc::gen::elementOf(valid_levels);
        kv["level"] = chosen_level;
    }
    std::string chosen_format;
    if (has_format) {
        chosen_format = *rc::gen::elementOf(valid_formats);
        kv["format"] = chosen_format;
    }

    LoggingConfig config;
    std::string err;
    bool result = parse_logging_config(kv, config, &err);
    RC_ASSERT(result == true);

    if (has_level) {
        RC_ASSERT(config.level == chosen_level);
    } else {
        RC_ASSERT(config.level == "info");
    }

    if (has_format) {
        RC_ASSERT(config.format == chosen_format);
    } else {
        RC_ASSERT(config.format == "text");
    }
}

// Feature: config-file, Property 6: invalid logging values rejected
// **Validates: Requirements 4.4, 4.5**
RC_GTEST_PROP(ConfigPBT, InvalidLoggingValuesRejected, ()) {
    static const std::vector<std::string> valid_levels = {
        "trace", "debug", "info", "warn", "error"
    };
    static const std::vector<std::string> valid_formats = {"text", "json"};

    // Decide whether to test invalid level or invalid format
    bool test_invalid_level = *rc::gen::arbitrary<bool>();

    std::unordered_map<std::string, std::string> kv;

    if (test_invalid_level) {
        auto random_level = *rc::gen::nonEmpty<std::string>();
        RC_PRE(std::find(valid_levels.begin(), valid_levels.end(), random_level)
               == valid_levels.end());
        kv["level"] = random_level;
    } else {
        auto random_format = *rc::gen::nonEmpty<std::string>();
        RC_PRE(std::find(valid_formats.begin(), valid_formats.end(), random_format)
               == valid_formats.end());
        kv["format"] = random_format;
    }

    LoggingConfig config;
    std::string err;
    RC_ASSERT(parse_logging_config(kv, config, &err) == false);
}

// Feature: config-file, Property 7: CLI override priority
// **Validates: Requirements 5.1, 5.2, 5.4**
RC_GTEST_PROP(ConfigPBT, CliOverridePriority, ()) {
    static const std::vector<std::string> valid_types = {"test", "v4l2", "libcamera", ""};

    // Generate random overrides
    auto camera_type = *rc::gen::elementOf(valid_types);
    auto device = *rc::gen::arbitrary<std::string>();
    auto log_json = *rc::gen::arbitrary<bool>();

    // Create ConfigManager with default state (no load needed for apply_overrides)
    ConfigManager mgr;

    // Record initial state before overrides
    auto initial_cam_type = mgr.camera_config().type;
    auto initial_device = mgr.camera_config().device;
    auto initial_format = mgr.logging_config().format;

    ConfigOverrides overrides;
    overrides.camera_type = camera_type;
    overrides.device = device;
    overrides.log_json = log_json;

    std::string err;
    bool result = mgr.apply_overrides(overrides, &err);
    RC_ASSERT(result == true);

    // Verify: non-empty camera_type overrides type
    if (!camera_type.empty()) {
        CameraSource::CameraType expected_type;
        CameraSource::parse_camera_type(camera_type, expected_type);
        RC_ASSERT(mgr.camera_config().type == expected_type);
    } else {
        RC_ASSERT(mgr.camera_config().type == initial_cam_type);
    }

    // Verify: non-empty device overrides device
    if (!device.empty()) {
        RC_ASSERT(mgr.camera_config().device == device);
    } else {
        RC_ASSERT(mgr.camera_config().device == initial_device);
    }

    // Verify: log_json=true overrides format to "json"
    if (log_json) {
        RC_ASSERT(mgr.logging_config().format == "json");
    } else {
        RC_ASSERT(mgr.logging_config().format == initial_format);
    }
}

// Feature: pipeline-cpu-optimization, Property 2: 非法布尔值被拒绝
// **Validates: Requirements 4.8**
RC_GTEST_PROP(ConfigPBT, InvalidBoolValueRejected, ()) {
    // 随机生成非空字符串
    auto random_str = *rc::gen::nonEmpty<std::string>();

    // 大小写不敏感比较，排除 "true" 和 "false"
    std::string lower = random_str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    RC_PRE(lower != "true" && lower != "false");

    // 构造 kv map，将随机字符串作为布尔字段的值
    std::unordered_map<std::string, std::string> kv = {{"test_field", random_str}};
    bool out = false;
    std::string err;

    // parse_bool_field 应返回 false（拒绝非法布尔值）
    RC_ASSERT(parse_bool_field(kv, "test_field", out, &err) == false);
}

// Feature: config-file, Property 8: StreamingConfig -> BitrateConfig conversion fidelity
// **Validates: Requirements 6.3**
RC_GTEST_PROP(ConfigPBT, StreamingToBitrateConversionFidelity, ()) {
    StreamingConfig sc;
    sc.bitrate_min_kbps = *rc::gen::arbitrary<int>();
    sc.bitrate_max_kbps = *rc::gen::arbitrary<int>();
    sc.bitrate_step_kbps = *rc::gen::arbitrary<int>();
    sc.bitrate_default_kbps = *rc::gen::arbitrary<int>();
    sc.bitrate_eval_interval_sec = *rc::gen::arbitrary<int>();
    sc.bitrate_rampup_interval_sec = *rc::gen::arbitrary<int>();

    BitrateConfig bc = to_bitrate_config(sc);

    RC_ASSERT(bc.min_kbps == sc.bitrate_min_kbps);
    RC_ASSERT(bc.max_kbps == sc.bitrate_max_kbps);
    RC_ASSERT(bc.step_kbps == sc.bitrate_step_kbps);
    RC_ASSERT(bc.default_kbps == sc.bitrate_default_kbps);
    RC_ASSERT(bc.eval_interval_sec == sc.bitrate_eval_interval_sec);
    RC_ASSERT(bc.rampup_interval_sec == sc.bitrate_rampup_interval_sec);
}

// ===========================================================================
// Task 5.1: component_levels Example-based unit tests
// Feature: log-management
// ===========================================================================

// ParseLoggingConfig_ComponentLevels_Valid:
// Normal component_levels parsing
TEST(ConfigExampleTest, ParseLoggingConfig_ComponentLevels_Valid) {
    std::unordered_map<std::string, std::string> kv = {
        {"component_levels", "ai:debug,kvs:warn,webrtc:info"}
    };
    LoggingConfig config;
    std::string err;
    EXPECT_TRUE(parse_logging_config(kv, config, &err));
    ASSERT_EQ(config.component_levels.size(), 3u);
    EXPECT_EQ(config.component_levels.at("ai"), "debug");
    EXPECT_EQ(config.component_levels.at("kvs"), "warn");
    EXPECT_EQ(config.component_levels.at("webrtc"), "info");
}

// ParseLoggingConfig_ComponentLevels_WithSpaces:
// Spaces around names and levels are trimmed
TEST(ConfigExampleTest, ParseLoggingConfig_ComponentLevels_WithSpaces) {
    std::unordered_map<std::string, std::string> kv = {
        {"component_levels", " ai : debug , kvs : warn "}
    };
    LoggingConfig config;
    std::string err;
    EXPECT_TRUE(parse_logging_config(kv, config, &err));
    ASSERT_EQ(config.component_levels.size(), 2u);
    EXPECT_EQ(config.component_levels.at("ai"), "debug");
    EXPECT_EQ(config.component_levels.at("kvs"), "warn");
}

// ParseLoggingConfig_ComponentLevels_Empty:
// Empty string means no component-level overrides
TEST(ConfigExampleTest, ParseLoggingConfig_ComponentLevels_Empty) {
    std::unordered_map<std::string, std::string> kv = {
        {"component_levels", ""}
    };
    LoggingConfig config;
    std::string err;
    EXPECT_TRUE(parse_logging_config(kv, config, &err));
    EXPECT_TRUE(config.component_levels.empty());
}

// ParseLoggingConfig_ComponentLevels_InvalidLevel:
// Invalid level value returns false
TEST(ConfigExampleTest, ParseLoggingConfig_ComponentLevels_InvalidLevel) {
    std::unordered_map<std::string, std::string> kv = {
        {"component_levels", "ai:verbose"}
    };
    LoggingConfig config;
    std::string err;
    EXPECT_FALSE(parse_logging_config(kv, config, &err));
    EXPECT_FALSE(err.empty());
}

// ParseLoggingConfig_ComponentLevels_MalformedEntry:
// Missing colon returns false
TEST(ConfigExampleTest, ParseLoggingConfig_ComponentLevels_MalformedEntry) {
    std::unordered_map<std::string, std::string> kv = {
        {"component_levels", "ai-debug"}
    };
    LoggingConfig config;
    std::string err;
    EXPECT_FALSE(parse_logging_config(kv, config, &err));
    EXPECT_FALSE(err.empty());
}

// ===========================================================================
// Task 5.3: Property 1 - component_levels parsing round-trip
// Feature: log-management, Property 1: component_levels parsing round-trip
// **Validates: Requirements 1.1, 2.3, 6.1, 6.2**
// ===========================================================================

RC_GTEST_PROP(LogManagementPBT, ComponentLevelsParsingRoundTrip, ()) {
    static const std::vector<std::string> valid_levels = {
        "trace", "debug", "info", "warn", "error"
    };

    // Generate a random map of component names to levels
    // Component names: non-empty lowercase alpha strings, 1-10 chars
    auto name_gen = rc::gen::nonEmpty(
        rc::gen::container<std::string>(
            rc::gen::inRange('a', static_cast<char>('z' + 1))));
    auto bounded_name_gen = rc::gen::suchThat(name_gen,
        [](const std::string& s) { return s.size() >= 1 && s.size() <= 10; });

    // Generate 1-5 entries
    auto count = *rc::gen::inRange(1, 6);
    std::unordered_map<std::string, std::string> expected;
    for (int i = 0; i < count; ++i) {
        auto name = *bounded_name_gen;
        auto level = *rc::gen::elementOf(valid_levels);
        expected[name] = level;
    }

    // Serialize to string with random whitespace
    std::string serialized;
    bool first = true;
    for (const auto& [name, level] : expected) {
        if (!first) {
            // Random spaces around comma
            auto spaces_before = *rc::gen::inRange(0, 4);
            auto spaces_after = *rc::gen::inRange(0, 4);
            serialized += std::string(spaces_before, ' ') + "," + std::string(spaces_after, ' ');
        }
        first = false;
        // Random spaces around colon
        auto spaces_before_colon = *rc::gen::inRange(0, 4);
        auto spaces_after_colon = *rc::gen::inRange(0, 4);
        serialized += name + std::string(spaces_before_colon, ' ') + ":" +
                      std::string(spaces_after_colon, ' ') + level;
    }

    // Parse
    std::unordered_map<std::string, std::string> kv = {
        {"component_levels", serialized}
    };
    LoggingConfig config;
    std::string err;
    bool result = parse_logging_config(kv, config, &err);

    RC_ASSERT(result == true);
    RC_ASSERT(config.component_levels.size() == expected.size());
    for (const auto& [name, level] : expected) {
        auto it = config.component_levels.find(name);
        RC_ASSERT(it != config.component_levels.end());
        RC_ASSERT(it->second == level);
    }
}

// ===========================================================================
// Task 5.5: Property 3 - invalid level rejection
// Feature: log-management, Property 3: invalid level rejection
// **Validates: Requirements 1.5**
// ===========================================================================

RC_GTEST_PROP(LogManagementPBT, InvalidLevelRejection, ()) {
    static const std::vector<std::string> valid_levels = {
        "trace", "debug", "info", "warn", "error"
    };

    // Generate a component name (non-empty lowercase alpha, 1-10 chars)
    auto name_gen = rc::gen::nonEmpty(
        rc::gen::container<std::string>(
            rc::gen::inRange('a', static_cast<char>('z' + 1))));
    auto bounded_name_gen = rc::gen::suchThat(name_gen,
        [](const std::string& s) { return s.size() >= 1 && s.size() <= 10; });

    // Generate an invalid level: non-empty string not in valid_levels
    auto invalid_level_gen = rc::gen::suchThat(
        rc::gen::nonEmpty(rc::gen::container<std::string>(
            rc::gen::inRange('a', static_cast<char>('z' + 1)))),
        [](const std::string& s) {
            static const std::vector<std::string> vl = {
                "trace", "debug", "info", "warn", "error"
            };
            return std::find(vl.begin(), vl.end(), s) == vl.end();
        });

    // Optionally prepend some valid entries
    auto valid_count = *rc::gen::inRange(0, 4);
    std::string serialized;
    for (int i = 0; i < valid_count; ++i) {
        if (!serialized.empty()) serialized += ",";
        auto vname = *bounded_name_gen;
        auto vlevel = *rc::gen::elementOf(valid_levels);
        serialized += vname + ":" + vlevel;
    }

    // Append the invalid entry
    if (!serialized.empty()) serialized += ",";
    auto bad_name = *bounded_name_gen;
    auto bad_level = *invalid_level_gen;
    serialized += bad_name + ":" + bad_level;

    // Parse should fail
    std::unordered_map<std::string, std::string> kv = {
        {"component_levels", serialized}
    };
    LoggingConfig config;
    std::string err;
    RC_ASSERT(parse_logging_config(kv, config, &err) == false);
}

// ===========================================================================
// Task 1.3: FPS 配置解析 Example-based 单元测试
// Feature: event-pipeline-optimization
// ===========================================================================

// ParseAiConfig_IdleFps_Default: 缺失 idle_fps → AiConfig.idle_fps == 1
TEST(ConfigExampleTest, ParseAiConfig_IdleFps_Default) {
    std::unordered_map<std::string, std::string> kv = {
        {"active_fps", "5"}
    };
    AiConfig config;
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_EQ(config.idle_fps, 1);
}

// ParseAiConfig_ActiveFps_Default: 缺失 active_fps → AiConfig.active_fps == 3
TEST(ConfigExampleTest, ParseAiConfig_ActiveFps_Default) {
    std::unordered_map<std::string, std::string> kv;
    AiConfig config;
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_EQ(config.active_fps, 3);
}

// ParseAiConfig_BackwardCompat_OnlyInferenceFps: 仅 inference_fps=5 → active_fps=5, idle_fps=1
TEST(ConfigExampleTest, ParseAiConfig_BackwardCompat_OnlyInferenceFps) {
    std::unordered_map<std::string, std::string> kv = {
        {"inference_fps", "5"}
    };
    AiConfig config;
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_EQ(config.active_fps, 5);
    EXPECT_EQ(config.idle_fps, 1);
}

// ParseAiConfig_BackwardCompat_AllThreeFields: idle_fps + active_fps + inference_fps → 忽略 inference_fps
TEST(ConfigExampleTest, ParseAiConfig_BackwardCompat_AllThreeFields) {
    std::unordered_map<std::string, std::string> kv = {
        {"idle_fps", "2"},
        {"active_fps", "8"},
        {"inference_fps", "15"}
    };
    AiConfig config;
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_EQ(config.idle_fps, 2);
    EXPECT_EQ(config.active_fps, 8);
}

// ParseAiConfig_MaxSnapshotsDefault: 缺失 → 默认 10
TEST(ConfigExampleTest, ParseAiConfig_MaxSnapshotsDefault) {
    std::unordered_map<std::string, std::string> kv;
    AiConfig config;
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_EQ(config.max_snapshots_per_event, 10);
}

// ParseAiConfig_MaxSnapshotsBelowOne: 值 < 1 → 使用默认 10
TEST(ConfigExampleTest, ParseAiConfig_MaxSnapshotsBelowOne) {
    std::unordered_map<std::string, std::string> kv = {
        {"max_snapshots_per_event", "0"}
    };
    AiConfig config;
    std::string err;
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_EQ(config.max_snapshots_per_event, 10);
}

// ParseAiConfig_IdleFpsOutOfRange: idle_fps=0 或 11 → 返回 false
TEST(ConfigExampleTest, ParseAiConfig_IdleFpsOutOfRange_Zero) {
    std::unordered_map<std::string, std::string> kv = {
        {"idle_fps", "0"},
        {"active_fps", "5"}
    };
    AiConfig config;
    std::string err;
    EXPECT_FALSE(parse_ai_config(kv, config, &err));
    EXPECT_FALSE(err.empty());
}

TEST(ConfigExampleTest, ParseAiConfig_IdleFpsOutOfRange_Eleven) {
    std::unordered_map<std::string, std::string> kv = {
        {"idle_fps", "11"},
        {"active_fps", "15"}
    };
    AiConfig config;
    std::string err;
    EXPECT_FALSE(parse_ai_config(kv, config, &err));
    EXPECT_FALSE(err.empty());
}

// ParseAiConfig_ActiveFpsOutOfRange: active_fps=0 或 31 → 返回 false
TEST(ConfigExampleTest, ParseAiConfig_ActiveFpsOutOfRange_Zero) {
    std::unordered_map<std::string, std::string> kv = {
        {"active_fps", "0"}
    };
    AiConfig config;
    std::string err;
    EXPECT_FALSE(parse_ai_config(kv, config, &err));
    EXPECT_FALSE(err.empty());
}

TEST(ConfigExampleTest, ParseAiConfig_ActiveFpsOutOfRange_ThirtyOne) {
    std::unordered_map<std::string, std::string> kv = {
        {"active_fps", "31"}
    };
    AiConfig config;
    std::string err;
    EXPECT_FALSE(parse_ai_config(kv, config, &err));
    EXPECT_FALSE(err.empty());
}

// ParseAiConfig_IdleGteActive: idle_fps=3, active_fps=3 → 返回 false
TEST(ConfigExampleTest, ParseAiConfig_IdleGteActive) {
    std::unordered_map<std::string, std::string> kv = {
        {"idle_fps", "3"},
        {"active_fps", "3"}
    };
    AiConfig config;
    std::string err;
    EXPECT_FALSE(parse_ai_config(kv, config, &err));
    EXPECT_FALSE(err.empty());
}

// ===========================================================================
// Task 1.4: Property 1 - FPS 配置解析正确性
// Feature: event-pipeline-optimization, Property 1: FPS config parsing correctness
// **Validates: Requirements 1.1, 1.2**
// ===========================================================================

RC_GTEST_PROP(EventPipelinePBT, FpsConfigParsingCorrectness, ()) {
    // Generator: random idle_fps in [1,10], random active_fps in [idle_fps+1, 30]
    int idle_fps = *rc::gen::inRange(1, 11);
    int active_fps = *rc::gen::inRange(idle_fps + 1, 31);

    std::unordered_map<std::string, std::string> kv = {
        {"idle_fps", std::to_string(idle_fps)},
        {"active_fps", std::to_string(active_fps)}
    };

    AiConfig config;
    std::string err;
    bool result = parse_ai_config(kv, config, &err);

    RC_ASSERT(result == true);
    RC_ASSERT(config.idle_fps == idle_fps);
    RC_ASSERT(config.active_fps == active_fps);
}

// ===========================================================================
// Task 1.5: Property 2 - FPS 范围验证
// Feature: event-pipeline-optimization, Property 2: FPS range validation
// **Validates: Requirements 1.5, 1.6**
// ===========================================================================

RC_GTEST_PROP(EventPipelinePBT, FpsRangeValidation, ()) {
    // Generator: random idle_fps in {<1, >10}, random active_fps in {<1, >30}
    // At least one of them must be out of range
    bool idle_out_of_range = *rc::gen::arbitrary<bool>();
    bool active_out_of_range = *rc::gen::arbitrary<bool>();
    RC_PRE(idle_out_of_range || active_out_of_range);

    int idle_fps;
    if (idle_out_of_range) {
        bool below = *rc::gen::arbitrary<bool>();
        if (below) {
            idle_fps = *rc::gen::inRange(-100, 1);  // < 1
        } else {
            idle_fps = *rc::gen::inRange(11, 200);   // > 10
        }
    } else {
        idle_fps = *rc::gen::inRange(1, 11);  // valid
    }

    int active_fps;
    if (active_out_of_range) {
        bool below = *rc::gen::arbitrary<bool>();
        if (below) {
            active_fps = *rc::gen::inRange(-100, 1);  // < 1
        } else {
            active_fps = *rc::gen::inRange(31, 200);   // > 30
        }
    } else {
        active_fps = *rc::gen::inRange(1, 31);  // valid
    }

    std::unordered_map<std::string, std::string> kv = {
        {"idle_fps", std::to_string(idle_fps)},
        {"active_fps", std::to_string(active_fps)}
    };

    AiConfig config;
    std::string err;
    bool result = parse_ai_config(kv, config, &err);

    RC_ASSERT(result == false);
    RC_ASSERT(!err.empty());
}

// ===========================================================================
// Task 1.6: Property 3 - idle_fps < active_fps 交叉验证
// Feature: event-pipeline-optimization, Property 3: idle_fps < active_fps cross-validation
// **Validates: Requirements 1.7**
// ===========================================================================

RC_GTEST_PROP(EventPipelinePBT, IdleFpsLessThanActiveFpsCrossValidation, ()) {
    // Generator: random (idle, active) pair where idle >= active,
    // both within their valid ranges
    int idle_fps = *rc::gen::inRange(1, 11);   // [1, 10]
    int active_fps = *rc::gen::inRange(1, 31); // [1, 30]
    RC_PRE(idle_fps >= active_fps);

    std::unordered_map<std::string, std::string> kv = {
        {"idle_fps", std::to_string(idle_fps)},
        {"active_fps", std::to_string(active_fps)}
    };

    AiConfig config;
    std::string err;
    bool result = parse_ai_config(kv, config, &err);

    RC_ASSERT(result == false);
    RC_ASSERT(!err.empty());
    // error_msg should contain descriptive information
    RC_ASSERT(err.find("idle_fps") != std::string::npos);
    RC_ASSERT(err.find("active_fps") != std::string::npos);
}

// ===========================================================================
// Task 1.3: 网络自适应配置 Example-based 单元测试
// Feature: network-adaptive-bitrate
// ===========================================================================

// 空 map 默认值验证（6 个新字段都取默认值）
TEST(ConfigExampleTest, ParseStreamingConfig_EmptyMap_NetworkDefaults) {
    std::unordered_map<std::string, std::string> kv;
    StreamingConfig config;
    std::string err;
    EXPECT_TRUE(parse_streaming_config(kv, config, &err));
    EXPECT_EQ(config.buffer_duration_sec, 180);
    EXPECT_EQ(config.latency_pressure_threshold, 5);
    EXPECT_EQ(config.latency_pressure_cooldown_sec, 30);
    EXPECT_TRUE(config.bandwidth_probe_enabled);
    EXPECT_EQ(config.bandwidth_probe_duration_sec, 10);
    EXPECT_EQ(config.writeframe_fail_threshold, 10);
}

// 完整 kv map 解析验证（6 个新字段全部指定非默认值）
TEST(ConfigExampleTest, ParseStreamingConfig_ValidComplete_NetworkFields) {
    std::unordered_map<std::string, std::string> kv = {
        {"buffer_duration_sec", "240"},
        {"latency_pressure_threshold", "8"},
        {"latency_pressure_cooldown_sec", "60"},
        {"bandwidth_probe_enabled", "false"},
        {"bandwidth_probe_duration_sec", "15"},
        {"writeframe_fail_threshold", "20"},
    };
    StreamingConfig config;
    std::string err;
    EXPECT_TRUE(parse_streaming_config(kv, config, &err));
    EXPECT_EQ(config.buffer_duration_sec, 240);
    EXPECT_EQ(config.latency_pressure_threshold, 8);
    EXPECT_EQ(config.latency_pressure_cooldown_sec, 60);
    EXPECT_FALSE(config.bandwidth_probe_enabled);
    EXPECT_EQ(config.bandwidth_probe_duration_sec, 15);
    EXPECT_EQ(config.writeframe_fail_threshold, 20);
}

// 无效值拒绝验证（负数）
TEST(ConfigExampleTest, ParseStreamingConfig_NegativeValue_Rejected) {
    std::unordered_map<std::string, std::string> kv = {
        {"buffer_duration_sec", "-1"},
    };
    StreamingConfig config;
    std::string err;
    EXPECT_FALSE(parse_streaming_config(kv, config, &err));
    EXPECT_FALSE(err.empty());
}

// 无效值拒绝验证（零）
TEST(ConfigExampleTest, ParseStreamingConfig_ZeroValue_Rejected) {
    std::unordered_map<std::string, std::string> kv = {
        {"writeframe_fail_threshold", "0"},
    };
    StreamingConfig config;
    std::string err;
    EXPECT_FALSE(parse_streaming_config(kv, config, &err));
    EXPECT_FALSE(err.empty());
}

// ===========================================================================
// Task 1.4: PBT — 属性 4：配置解析 round-trip
// Feature: network-adaptive-bitrate, Property 4: Config parse round-trip
// **Validates: Requirements 5.1, 5.2**
// ===========================================================================

RC_GTEST_PROP(NetworkAdaptivePBT, ConfigParseRoundTrip, ()) {
    // 生成随机有效 StreamingConfig（所有字段在合法范围内）
    // 原有 7 个字段允许任意非负值
    int bitrate_min_kbps = *rc::gen::inRange(0, 10001);
    int bitrate_max_kbps = *rc::gen::inRange(0, 10001);
    int bitrate_step_kbps = *rc::gen::inRange(0, 10001);
    int bitrate_default_kbps = *rc::gen::inRange(0, 10001);
    int bitrate_eval_interval_sec = *rc::gen::inRange(0, 10001);
    int bitrate_rampup_interval_sec = *rc::gen::inRange(0, 10001);
    int debounce_sec = *rc::gen::inRange(0, 10001);

    // 新增 5 个正整数字段（必须 > 0）
    int buffer_duration_sec = *rc::gen::inRange(1, 10001);
    int latency_pressure_threshold = *rc::gen::inRange(1, 10001);
    int latency_pressure_cooldown_sec = *rc::gen::inRange(1, 10001);
    int bandwidth_probe_duration_sec = *rc::gen::inRange(1, 10001);
    int writeframe_fail_threshold = *rc::gen::inRange(1, 10001);
    bool bandwidth_probe_enabled = *rc::gen::arbitrary<bool>();

    // 序列化为 kv map
    std::unordered_map<std::string, std::string> kv = {
        {"bitrate_min_kbps", std::to_string(bitrate_min_kbps)},
        {"bitrate_max_kbps", std::to_string(bitrate_max_kbps)},
        {"bitrate_step_kbps", std::to_string(bitrate_step_kbps)},
        {"bitrate_default_kbps", std::to_string(bitrate_default_kbps)},
        {"bitrate_eval_interval_sec", std::to_string(bitrate_eval_interval_sec)},
        {"bitrate_rampup_interval_sec", std::to_string(bitrate_rampup_interval_sec)},
        {"debounce_sec", std::to_string(debounce_sec)},
        {"buffer_duration_sec", std::to_string(buffer_duration_sec)},
        {"latency_pressure_threshold", std::to_string(latency_pressure_threshold)},
        {"latency_pressure_cooldown_sec", std::to_string(latency_pressure_cooldown_sec)},
        {"bandwidth_probe_enabled", bandwidth_probe_enabled ? "true" : "false"},
        {"bandwidth_probe_duration_sec", std::to_string(bandwidth_probe_duration_sec)},
        {"writeframe_fail_threshold", std::to_string(writeframe_fail_threshold)},
    };

    // 解析
    StreamingConfig config;
    std::string err;
    bool result = parse_streaming_config(kv, config, &err);
    RC_ASSERT(result == true);

    // 验证等价
    RC_ASSERT(config.bitrate_min_kbps == bitrate_min_kbps);
    RC_ASSERT(config.bitrate_max_kbps == bitrate_max_kbps);
    RC_ASSERT(config.bitrate_step_kbps == bitrate_step_kbps);
    RC_ASSERT(config.bitrate_default_kbps == bitrate_default_kbps);
    RC_ASSERT(config.bitrate_eval_interval_sec == bitrate_eval_interval_sec);
    RC_ASSERT(config.bitrate_rampup_interval_sec == bitrate_rampup_interval_sec);
    RC_ASSERT(config.debounce_sec == debounce_sec);
    RC_ASSERT(config.buffer_duration_sec == buffer_duration_sec);
    RC_ASSERT(config.latency_pressure_threshold == latency_pressure_threshold);
    RC_ASSERT(config.latency_pressure_cooldown_sec == latency_pressure_cooldown_sec);
    RC_ASSERT(config.bandwidth_probe_enabled == bandwidth_probe_enabled);
    RC_ASSERT(config.bandwidth_probe_duration_sec == bandwidth_probe_duration_sec);
    RC_ASSERT(config.writeframe_fail_threshold == writeframe_fail_threshold);
}

// ===========================================================================
// Task 1.5: PBT — 属性 5：无效配置拒绝
// Feature: network-adaptive-bitrate, Property 5: Invalid config rejected
// **Validates: Requirements 5.3**
// ===========================================================================

RC_GTEST_PROP(NetworkAdaptivePBT, InvalidConfigRejected, ()) {
    // 5 个必须为正整数的字段名
    static const std::vector<std::string> positive_fields = {
        "buffer_duration_sec",
        "latency_pressure_threshold",
        "latency_pressure_cooldown_sec",
        "bandwidth_probe_duration_sec",
        "writeframe_fail_threshold",
    };

    // 随机选择一个字段注入无效值
    auto field_name = *rc::gen::elementOf(positive_fields);

    // 生成无效值：负数或零
    bool use_zero = *rc::gen::arbitrary<bool>();
    int invalid_value;
    if (use_zero) {
        invalid_value = 0;
    } else {
        invalid_value = *rc::gen::inRange(-10000, 0);  // 负数
    }

    std::unordered_map<std::string, std::string> kv = {
        {field_name, std::to_string(invalid_value)},
    };

    StreamingConfig config;
    std::string err;
    bool result = parse_streaming_config(kv, config, &err);
    RC_ASSERT(result == false);
    RC_ASSERT(!err.empty());
}

// ===========================================================================
// Task 1.6: PBT — 属性 8：kvssink 属性初始化
// Feature: network-adaptive-bitrate, Property 8: KvsSink config initialization
// **Validates: Requirements 2.1, 2.2**
// ===========================================================================

RC_GTEST_PROP(NetworkAdaptivePBT, KvsSinkConfigInitialization, ()) {
    // 生成随机有效 StreamingConfig
    StreamingConfig sc;
    sc.buffer_duration_sec = *rc::gen::inRange(1, 10001);

    // 生成随机有效 BitrateConfig
    BitrateConfig bc;
    bc.default_kbps = *rc::gen::inRange(1, 10001);

    KvsSinkConfig kc = to_kvssink_config(sc, bc);

    // 验证：avg_bandwidth_bps == default_kbps × 1000
    RC_ASSERT(kc.avg_bandwidth_bps == bc.default_kbps * 1000);
    // 验证：buffer_duration_sec == StreamingConfig.buffer_duration_sec
    RC_ASSERT(kc.buffer_duration_sec == sc.buffer_duration_sec);
}
