// kvs_sink_factory.h
// KVS sink element factory with platform-conditional compilation.
#pragma once

#include <gst/gst.h>
#include <string>
#include <unordered_map>
#include "credential_provider.h"  // AwsConfig, parse_toml_section

namespace KvsSinkFactory {

// KVS stream configuration (parsed from TOML [kvs] section)
struct KvsConfig {
    std::string stream_name;   // KVS stream name
    std::string aws_region;    // AWS region
    bool enabled = true;       // KVS 分支是否启用
};

// Build KvsConfig from TOML key-value map.
// Returns false and fills error_msg (with missing field names) on failure.
bool build_kvs_config(
    const std::unordered_map<std::string, std::string>& kv,
    KvsConfig& config,
    std::string* error_msg = nullptr);

// Build iot-certificate property string from AwsConfig.
// Format: "iot-certificate,endpoint=...,cert-path=...,key-path=...,ca-path=...,role-aliases=..."
std::string build_iot_certificate_string(const AwsConfig& aws_config);

// Create KVS sink element (platform-conditional compilation).
// - Linux: try kvssink, fall back to fakesink if unavailable
// - macOS: create fakesink stub
// Sets stream-name, aws-region, iot-certificate properties (kvssink only).
// Returns nullptr and fills error_msg on failure.
GstElement* create_kvs_sink(
    const KvsConfig& kvs_config,
    const AwsConfig& aws_config,
    std::string* error_msg = nullptr);

}  // namespace KvsSinkFactory
