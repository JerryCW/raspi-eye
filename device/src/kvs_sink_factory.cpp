// kvs_sink_factory.cpp
// KVS sink element factory implementation.
#include "kvs_sink_factory.h"
#include <spdlog/spdlog.h>
#include <vector>

namespace KvsSinkFactory {

bool build_kvs_config(
    const std::unordered_map<std::string, std::string>& kv,
    KvsConfig& config,
    std::string* error_msg) {
    static const std::vector<std::pair<std::string, std::string KvsConfig::*>> fields = {
        {"stream_name", &KvsConfig::stream_name},
        {"aws_region",  &KvsConfig::aws_region},
    };

    std::vector<std::string> missing;
    for (const auto& [name, member_ptr] : fields) {
        auto it = kv.find(name);
        if (it == kv.end() || it->second.empty()) {
            missing.push_back(name);
        } else {
            config.*member_ptr = it->second;
        }
    }

    if (!missing.empty()) {
        if (error_msg) {
            std::string msg = "Missing required fields in [kvs]: ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i > 0) msg += ", ";
                msg += missing[i];
            }
            *error_msg = msg;
        }
        return false;
    }
    return true;
}

std::string build_iot_certificate_string(const AwsConfig& aws_config) {
    return "iot-certificate,"
           "iot-thing-name=" + aws_config.thing_name + ","
           "endpoint=" + aws_config.credential_endpoint + ","
           "cert-path=" + aws_config.cert_path + ","
           "key-path=" + aws_config.key_path + ","
           "ca-path=" + aws_config.ca_path + ","
           "role-aliases=" + aws_config.role_alias;
}

GstElement* create_kvs_sink(
    const KvsConfig& kvs_config,
    const AwsConfig& aws_config,
    std::string* error_msg) {
    auto pl = spdlog::get("pipeline");

#ifdef __linux__
    GstElement* sink = gst_element_factory_make("kvssink", "kvs-sink");
    if (!sink) {
        if (pl) pl->warn("kvssink not available, falling back to fakesink");
        sink = gst_element_factory_make("fakesink", "kvs-sink");
        if (!sink) {
            if (error_msg) *error_msg = "Failed to create fakesink for kvs-sink";
            return nullptr;
        }
        if (pl) pl->info("Created fakesink stub for kvs-sink (Linux fallback)");
        return sink;  // fakesink: skip kvssink property setup
    }
    // Set all kvssink properties in a single call.
    // kvssink internally initializes KVS SDK on first property set;
    // splitting into multiple g_object_set calls can cause the SDK to
    // read uninitialized iot-certificate, leading to 52GB allocation abort.
    std::string iot_cert = build_iot_certificate_string(aws_config);
    g_object_set(G_OBJECT(sink),
        "stream-name", kvs_config.stream_name.c_str(),
        "aws-region",  kvs_config.aws_region.c_str(),
        "iot-certificate", iot_cert.c_str(),
        "restart-on-error", FALSE,
        nullptr);
    // Log only stream-name and aws-region (no credential paths)
    if (pl) pl->info("Created kvssink: stream-name={}, aws-region={}",
                     kvs_config.stream_name, kvs_config.aws_region);
#else
    (void)aws_config;  // unused on macOS
    GstElement* sink = gst_element_factory_make("fakesink", "kvs-sink");
    if (!sink) {
        if (error_msg) *error_msg = "Failed to create fakesink for kvs-sink";
        return nullptr;
    }
    if (pl) pl->info("macOS: using fakesink stub for kvs-sink");
#endif

    return sink;
}

}  // namespace KvsSinkFactory
