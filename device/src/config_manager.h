// config_manager.h
// Unified configuration manager for raspi-eye.
#pragma once

#include <string>
#include <unordered_map>
#include "camera_source.h"
#include "credential_provider.h"  // AwsConfig, parse_toml_section
#include "kvs_sink_factory.h"     // KvsSinkFactory::KvsConfig
#include "webrtc_signaling.h"     // WebRtcConfig
#include "bitrate_adapter.h"      // BitrateConfig

// Streaming configuration (parsed from TOML [streaming] section)
struct StreamingConfig {
    int bitrate_min_kbps = 1000;
    int bitrate_max_kbps = 4000;
    int bitrate_step_kbps = 500;
    int bitrate_default_kbps = 2500;
    int bitrate_eval_interval_sec = 5;
    int bitrate_rampup_interval_sec = 30;
    int debounce_sec = 3;
};

// Logging configuration (parsed from TOML [logging] section)
struct LoggingConfig {
    std::string level = "info";    // trace|debug|info|warn|error
    std::string format = "text";   // text|json
};

// --- Pure parse functions (testable / PBT-friendly) ---

// Parse camera config from kv map. Missing fields keep platform defaults.
// Returns false with error_msg on invalid values (e.g. unknown type).
bool parse_camera_config(
    const std::unordered_map<std::string, std::string>& kv,
    CameraSource::CameraConfig& config,
    std::string* error_msg = nullptr);

// Parse streaming config from kv map. Missing fields keep defaults.
// Returns false with error_msg on invalid values.
bool parse_streaming_config(
    const std::unordered_map<std::string, std::string>& kv,
    StreamingConfig& config,
    std::string* error_msg = nullptr);

// Parse logging config from kv map. Missing fields keep defaults.
// Returns false with error_msg on invalid values (e.g. unknown level).
bool parse_logging_config(
    const std::unordered_map<std::string, std::string>& kv,
    LoggingConfig& config,
    std::string* error_msg = nullptr);

// Validate streaming config consistency: min <= default <= max.
bool validate_streaming_config(
    const StreamingConfig& config,
    std::string* error_msg = nullptr);

// Convert StreamingConfig to BitrateConfig (pure function).
BitrateConfig to_bitrate_config(const StreamingConfig& sc);

// --- CLI override parameters ---

struct ConfigOverrides {
    std::string camera_type;   // Non-empty overrides [camera].type
    std::string device;        // Non-empty overrides [camera].device
    bool log_json = false;     // true overrides [logging].format to "json"
};

// --- ConfigManager ---

class ConfigManager {
public:
    // Load all sections from config.toml.
    // Returns false with error_msg on failure.
    bool load(const std::string& config_path,
              std::string* error_msg = nullptr);

    // Apply CLI overrides (call after load).
    // Returns false with error_msg on invalid override values.
    bool apply_overrides(const ConfigOverrides& overrides,
                         std::string* error_msg = nullptr);

    // Accessors (const references)
    const AwsConfig& aws_config() const { return aws_config_; }
    const KvsSinkFactory::KvsConfig& kvs_config() const { return kvs_config_; }
    const WebRtcConfig& webrtc_config() const { return webrtc_config_; }
    const CameraSource::CameraConfig& camera_config() const { return camera_config_; }
    const StreamingConfig& streaming_config() const { return streaming_config_; }
    const LoggingConfig& logging_config() const { return logging_config_; }

private:
    AwsConfig aws_config_;
    KvsSinkFactory::KvsConfig kvs_config_;
    WebRtcConfig webrtc_config_;
    CameraSource::CameraConfig camera_config_;
    StreamingConfig streaming_config_;
    LoggingConfig logging_config_;
};
