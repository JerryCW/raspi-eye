// config_manager.cpp
// Unified configuration manager implementation.

#include "config_manager.h"

#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

// ============================================================
// Helper: get optional spdlog logger (may not exist)
// ============================================================

namespace {

std::shared_ptr<spdlog::logger> config_logger() {
    return spdlog::get("config");
}

// Helper: look up key in kv map, return pointer to value or nullptr
const std::string* find_value(
    const std::unordered_map<std::string, std::string>& kv,
    const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) return nullptr;
    return &it->second;
}

}  // namespace

// ============================================================
// parse_camera_config
// ============================================================

bool parse_camera_config(
    const std::unordered_map<std::string, std::string>& kv,
    CameraSource::CameraConfig& config,
    std::string* error_msg) {

    // type field
    if (auto* val = find_value(kv, "type")) {
        CameraSource::CameraType parsed_type;
        if (!CameraSource::parse_camera_type(*val, parsed_type)) {
            if (error_msg) *error_msg = "Invalid camera type: " + *val;
            return false;
        }
        config.type = parsed_type;
    }

    // device field
    if (auto* val = find_value(kv, "device")) {
        config.device = *val;
    }

    // width field
    if (auto* val = find_value(kv, "width")) {
        config.width = std::stoi(*val);
    }

    // height field
    if (auto* val = find_value(kv, "height")) {
        config.height = std::stoi(*val);
    }

    // framerate field
    if (auto* val = find_value(kv, "framerate")) {
        config.framerate = std::stoi(*val);
    }

    return true;
}

// ============================================================
// parse_streaming_config
// ============================================================

bool parse_streaming_config(
    const std::unordered_map<std::string, std::string>& kv,
    StreamingConfig& config,
    std::string* error_msg) {

    // Map of field names to member pointers
    static const std::vector<std::pair<std::string, int StreamingConfig::*>> fields = {
        {"bitrate_min_kbps",           &StreamingConfig::bitrate_min_kbps},
        {"bitrate_max_kbps",           &StreamingConfig::bitrate_max_kbps},
        {"bitrate_step_kbps",          &StreamingConfig::bitrate_step_kbps},
        {"bitrate_default_kbps",       &StreamingConfig::bitrate_default_kbps},
        {"bitrate_eval_interval_sec",  &StreamingConfig::bitrate_eval_interval_sec},
        {"bitrate_rampup_interval_sec",&StreamingConfig::bitrate_rampup_interval_sec},
        {"debounce_sec",               &StreamingConfig::debounce_sec},
    };

    for (const auto& [name, member_ptr] : fields) {
        if (auto* val = find_value(kv, name)) {
            config.*member_ptr = std::stoi(*val);
        }
    }

    return true;
}

// ============================================================
// parse_logging_config
// ============================================================

bool parse_logging_config(
    const std::unordered_map<std::string, std::string>& kv,
    LoggingConfig& config,
    std::string* error_msg) {

    static const std::unordered_set<std::string> valid_levels = {
        "trace", "debug", "info", "warn", "error"
    };
    static const std::unordered_set<std::string> valid_formats = {
        "text", "json"
    };

    // level field
    if (auto* val = find_value(kv, "level")) {
        if (valid_levels.find(*val) == valid_levels.end()) {
            if (error_msg) *error_msg = "Invalid logging level: " + *val;
            return false;
        }
        config.level = *val;
    }

    // format field
    if (auto* val = find_value(kv, "format")) {
        if (valid_formats.find(*val) == valid_formats.end()) {
            if (error_msg) *error_msg = "Invalid logging format: " + *val;
            return false;
        }
        config.format = *val;
    }

    return true;
}

// ============================================================
// validate_streaming_config
// ============================================================

bool validate_streaming_config(
    const StreamingConfig& config,
    std::string* error_msg) {

    if (!(config.bitrate_min_kbps <= config.bitrate_default_kbps &&
          config.bitrate_default_kbps <= config.bitrate_max_kbps)) {
        if (error_msg) {
            *error_msg = "Invalid streaming config: min_kbps("
                + std::to_string(config.bitrate_min_kbps)
                + ") <= default_kbps("
                + std::to_string(config.bitrate_default_kbps)
                + ") <= max_kbps("
                + std::to_string(config.bitrate_max_kbps)
                + ") not satisfied";
        }
        return false;
    }
    return true;
}

// ============================================================
// to_bitrate_config
// ============================================================

BitrateConfig to_bitrate_config(const StreamingConfig& sc) {
    return BitrateConfig{
        sc.bitrate_min_kbps,
        sc.bitrate_max_kbps,
        sc.bitrate_step_kbps,
        sc.bitrate_default_kbps,
        sc.bitrate_eval_interval_sec,
        sc.bitrate_rampup_interval_sec
    };
}

// ============================================================
// parse_ai_config
// ============================================================

bool parse_ai_config(
    const std::unordered_map<std::string, std::string>& kv,
    AiConfig& config,
    std::string* error_msg) {

    auto log = config_logger();

    // model_path
    if (auto* val = find_value(kv, "model_path")) {
        config.model_path = *val;
    }

    // inference_fps (1-30 range, 0 or out-of-range is error)
    if (auto* val = find_value(kv, "inference_fps")) {
        int fps = std::stoi(*val);
        if (fps < 1 || fps > 30) {
            if (error_msg) *error_msg = "inference_fps must be 1-30, got: " + *val;
            return false;
        }
        config.inference_fps = fps;
    }

    // confidence_threshold
    if (auto* val = find_value(kv, "confidence_threshold")) {
        config.confidence_threshold = std::stof(*val);
    }

    // snapshot_dir
    if (auto* val = find_value(kv, "snapshot_dir")) {
        config.snapshot_dir = *val;
    }

    // event_timeout_sec (< 3 uses default 15 with warn)
    if (auto* val = find_value(kv, "event_timeout_sec")) {
        int timeout = std::stoi(*val);
        if (timeout < 3) {
            if (log) log->warn("event_timeout_sec={} is below minimum 3, using default 15", timeout);
            config.event_timeout_sec = 15;
        } else {
            config.event_timeout_sec = timeout;
        }
    }

    // max_cache_mb
    if (auto* val = find_value(kv, "max_cache_mb")) {
        config.max_cache_mb = std::stoi(*val);
    }

    // target_classes: comma-separated "name[:confidence],..."
    if (auto* val = find_value(kv, "target_classes")) {
        config.target_classes.clear();
        if (!val->empty()) {
            std::istringstream stream(*val);
            std::string entry;
            while (std::getline(stream, entry, ',')) {
                // Trim whitespace
                size_t start = entry.find_first_not_of(" \t");
                size_t end = entry.find_last_not_of(" \t");
                if (start == std::string::npos) continue;
                entry = entry.substr(start, end - start + 1);

                AiConfig::TargetClass tc;
                auto colon_pos = entry.find(':');
                if (colon_pos != std::string::npos) {
                    tc.name = entry.substr(0, colon_pos);
                    // Trim name
                    size_t ns = tc.name.find_first_not_of(" \t");
                    size_t ne = tc.name.find_last_not_of(" \t");
                    if (ns != std::string::npos) tc.name = tc.name.substr(ns, ne - ns + 1);
                    tc.confidence = std::stof(entry.substr(colon_pos + 1));
                } else {
                    tc.name = entry;
                }
                config.target_classes.push_back(std::move(tc));
            }
        }
    }

    return true;
}

// ============================================================
// parse_s3_config
// ============================================================

bool parse_s3_config(
    const std::unordered_map<std::string, std::string>& kv,
    S3Config& config,
    std::string* error_msg) {

    auto log = config_logger();

    // bucket
    if (auto* val = find_value(kv, "bucket")) {
        config.bucket = *val;
    }

    // region
    if (auto* val = find_value(kv, "region")) {
        config.region = *val;
    }

    // scan_interval_sec (< 5 uses default 30)
    if (auto* val = find_value(kv, "scan_interval_sec")) {
        int interval = std::stoi(*val);
        if (interval < 5) {
            if (log) log->warn("scan_interval_sec={} is below minimum 5, using default 30", interval);
            config.scan_interval_sec = 30;
        } else {
            config.scan_interval_sec = interval;
        }
    }

    // max_retries
    if (auto* val = find_value(kv, "max_retries")) {
        config.max_retries = std::stoi(*val);
    }

    return true;
}

// ============================================================
// ConfigManager::load
// ============================================================

bool ConfigManager::load(const std::string& config_path,
                         std::string* error_msg) {
    auto log = config_logger();

    // Step 1-2: Parse [aws] (required)
    auto kv_aws = parse_toml_section(config_path, "aws", error_msg);
    if (kv_aws.empty() && error_msg && !error_msg->empty()) {
        return false;
    }
    if (!build_aws_config(kv_aws, aws_config_, error_msg)) {
        return false;
    }

    // Step 3-4: Parse [kvs] (required)
    auto kv_kvs = parse_toml_section(config_path, "kvs", error_msg);
    if (kv_kvs.empty() && error_msg && !error_msg->empty()) {
        return false;
    }
    if (!KvsSinkFactory::build_kvs_config(kv_kvs, kvs_config_, error_msg)) {
        return false;
    }

    // Step 5-6: Parse [webrtc] (required)
    auto kv_webrtc = parse_toml_section(config_path, "webrtc", error_msg);
    if (kv_webrtc.empty() && error_msg && !error_msg->empty()) {
        return false;
    }
    if (!build_webrtc_config(kv_webrtc, webrtc_config_, error_msg)) {
        return false;
    }

    // Step 7-8: Parse [camera] (optional)
    std::string camera_err;
    auto kv_camera = parse_toml_section(config_path, "camera", &camera_err);
    if (!parse_camera_config(kv_camera, camera_config_, error_msg)) {
        return false;
    }

    // Step 9-10: Parse [streaming] (optional)
    std::string streaming_err;
    auto kv_streaming = parse_toml_section(config_path, "streaming", &streaming_err);
    if (!parse_streaming_config(kv_streaming, streaming_config_, error_msg)) {
        return false;
    }

    // Step 11: Validate streaming config
    if (!validate_streaming_config(streaming_config_, error_msg)) {
        return false;
    }

    // Step 12-13: Parse [logging] (optional)
    std::string logging_err;
    auto kv_logging = parse_toml_section(config_path, "logging", &logging_err);
    if (!parse_logging_config(kv_logging, logging_config_, error_msg)) {
        return false;
    }

    // Parse [ai] (optional)
    std::string ai_err;
    auto kv_ai = parse_toml_section(config_path, "ai", &ai_err);
    if (!parse_ai_config(kv_ai, ai_config_, error_msg)) {
        return false;
    }
    // Set device_id from aws config
    ai_config_.device_id = aws_config_.thing_name;

    // Parse [s3] (optional)
    std::string s3_err;
    auto kv_s3 = parse_toml_section(config_path, "s3", &s3_err);
    if (!parse_s3_config(kv_s3, s3_config_, error_msg)) {
        return false;
    }

    if (log) {
        log->info("Configuration loaded from {}", config_path);
    }

    // Step 14: Success
    return true;
}

// ============================================================
// ConfigManager::apply_overrides
// ============================================================

bool ConfigManager::apply_overrides(const ConfigOverrides& overrides,
                                    std::string* error_msg) {
    // Step 1: Override camera type
    if (!overrides.camera_type.empty()) {
        CameraSource::CameraType type;
        if (!CameraSource::parse_camera_type(overrides.camera_type, type)) {
            if (error_msg) *error_msg = "Invalid camera type override: " + overrides.camera_type;
            return false;
        }
        camera_config_.type = type;
    }

    // Step 2: Override device
    if (!overrides.device.empty()) {
        camera_config_.device = overrides.device;
    }

    // Step 3: Override log format
    if (overrides.log_json) {
        logging_config_.format = "json";
    }

    // Step 4: Success
    return true;
}
