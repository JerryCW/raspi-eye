// kvs_test.cpp
// KVS sink factory tests: 6 example-based + 3 PBT properties.
#include "kvs_sink_factory.h"
#include "pipeline_builder.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>

#include <gst/gst.h>
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

// ============================================================
// Test helpers
// ============================================================

// Write a temporary TOML file and return its path.
static std::string write_temp_toml(const std::string& content) {
    std::string path = std::string("/tmp/kvs_test_") +
                       std::to_string(std::rand()) + ".toml";
    std::ofstream ofs(path);
    ofs << content;
    ofs.close();
    return path;
}

// Clean up a temporary file.
static void cleanup_file(const std::string& path) {
    std::remove(path.c_str());
}

// ============================================================
// Example-based tests
// ============================================================

// 1. MissingSectionReturnsError: empty map -> build_kvs_config returns false,
//    error message contains "stream_name" and "aws_region"
// **Validates: Requirements 1.2, 5.2**
TEST(KvsSinkFactory, MissingSectionReturnsError) {
    std::unordered_map<std::string, std::string> empty_kv;
    KvsSinkFactory::KvsConfig config;
    std::string err;
    bool ok = KvsSinkFactory::build_kvs_config(empty_kv, config, &err);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("stream_name"), std::string::npos) << "Error should mention stream_name: " << err;
    EXPECT_NE(err.find("aws_region"), std::string::npos) << "Error should mention aws_region: " << err;
}

// 2. MacOsCreatesFakesink: create_kvs_sink on current platform (macOS test env)
//    returns fakesink with element name "kvs-sink"
// **Validates: Requirements 2.2, 5.4**
TEST(KvsSinkFactory, MacOsCreatesFakesink) {
    KvsSinkFactory::KvsConfig kvs_cfg;
    kvs_cfg.stream_name = "TestStream";
    kvs_cfg.aws_region = "us-east-1";

    AwsConfig aws_cfg;
    aws_cfg.credential_endpoint = "endpoint.example.com";
    aws_cfg.cert_path = "/tmp/cert.pem";
    aws_cfg.key_path = "/tmp/key.pem";
    aws_cfg.ca_path = "/tmp/ca.pem";
    aws_cfg.role_alias = "TestAlias";

    std::string err;
    GstElement* sink = KvsSinkFactory::create_kvs_sink(kvs_cfg, aws_cfg, &err);
    ASSERT_NE(sink, nullptr) << "create_kvs_sink failed: " << err;

    // On macOS (or Linux without kvssink), should be fakesink
    GstElementFactory* factory = gst_element_get_factory(sink);
    const gchar* factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    EXPECT_STREQ(factory_name, "fakesink");

    // Element name should be "kvs-sink"
    EXPECT_STREQ(GST_ELEMENT_NAME(sink), "kvs-sink");

    gst_object_unref(sink);
}

// 3. BackwardCompatibleWithoutKvsConfig: build_tee_pipeline without KvsConfig
//    still succeeds, kvs-sink is fakesink
// **Validates: Requirements 4.4, 5.5**
TEST(KvsSinkFactory, BackwardCompatibleWithoutKvsConfig) {
    std::string err;
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(&err);
    ASSERT_NE(pipeline, nullptr) << "build_tee_pipeline failed: " << err;

    GstElement* kvs_sink = gst_bin_get_by_name(GST_BIN(pipeline), "kvs-sink");
    ASSERT_NE(kvs_sink, nullptr) << "kvs-sink not found in pipeline";

    GstElementFactory* factory = gst_element_get_factory(kvs_sink);
    const gchar* factory_name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    EXPECT_STREQ(factory_name, "fakesink");

    gst_object_unref(kvs_sink);
    gst_object_unref(pipeline);
}

// 4. PipelineBuildsWithKvsConfig: pipeline builds with KvsConfig + AwsConfig
//    (macOS stub scenario)
// **Validates: Requirements 4.1, 4.2, 5.6**
TEST(KvsSinkFactory, PipelineBuildsWithKvsConfig) {
    KvsSinkFactory::KvsConfig kvs_cfg;
    kvs_cfg.stream_name = "TestStream";
    kvs_cfg.aws_region = "us-east-1";

    AwsConfig aws_cfg;
    aws_cfg.credential_endpoint = "endpoint.example.com";
    aws_cfg.cert_path = "/tmp/cert.pem";
    aws_cfg.key_path = "/tmp/key.pem";
    aws_cfg.ca_path = "/tmp/ca.pem";
    aws_cfg.role_alias = "TestAlias";

    std::string err;
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(&err, {}, &kvs_cfg, &aws_cfg);
    ASSERT_NE(pipeline, nullptr) << "build_tee_pipeline with KvsConfig failed: " << err;

    GstElement* kvs_sink = gst_bin_get_by_name(GST_BIN(pipeline), "kvs-sink");
    ASSERT_NE(kvs_sink, nullptr) << "kvs-sink not found in pipeline";

    gst_object_unref(kvs_sink);
    gst_object_unref(pipeline);
}

// 5. FakesinkSkipsIotCertificate: fakesink stub does not set iot-certificate
//    property, no crash
// **Validates: Requirements 3.5**
TEST(KvsSinkFactory, FakesinkSkipsIotCertificate) {
    KvsSinkFactory::KvsConfig kvs_cfg;
    kvs_cfg.stream_name = "TestStream";
    kvs_cfg.aws_region = "us-east-1";

    AwsConfig aws_cfg;
    aws_cfg.credential_endpoint = "endpoint.example.com";
    aws_cfg.cert_path = "/tmp/cert.pem";
    aws_cfg.key_path = "/tmp/key.pem";
    aws_cfg.ca_path = "/tmp/ca.pem";
    aws_cfg.role_alias = "TestAlias";

    std::string err;
    GstElement* sink = KvsSinkFactory::create_kvs_sink(kvs_cfg, aws_cfg, &err);
    ASSERT_NE(sink, nullptr) << "create_kvs_sink failed: " << err;

    // On macOS, this is a fakesink. Verify it does NOT have iot-certificate property.
    // Attempting to find the property spec should return nullptr for fakesink.
    GObjectClass* klass = G_OBJECT_GET_CLASS(sink);
    GParamSpec* pspec = g_object_class_find_property(klass, "iot-certificate");
    EXPECT_EQ(pspec, nullptr) << "fakesink should not have iot-certificate property";

    gst_object_unref(sink);
}

// 6. PipelineTopologyUnchanged: pipeline with KvsConfig still contains
//    raw-tee, encoded-tee, ai-sink, webrtc-sink
// **Validates: Requirements 4.5**
TEST(KvsSinkFactory, PipelineTopologyUnchanged) {
    KvsSinkFactory::KvsConfig kvs_cfg;
    kvs_cfg.stream_name = "TestStream";
    kvs_cfg.aws_region = "us-east-1";

    AwsConfig aws_cfg;
    aws_cfg.credential_endpoint = "endpoint.example.com";
    aws_cfg.cert_path = "/tmp/cert.pem";
    aws_cfg.key_path = "/tmp/key.pem";
    aws_cfg.ca_path = "/tmp/ca.pem";
    aws_cfg.role_alias = "TestAlias";

    std::string err;
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(&err, {}, &kvs_cfg, &aws_cfg);
    ASSERT_NE(pipeline, nullptr) << "build_tee_pipeline failed: " << err;

    // Verify all expected elements exist
    const char* expected_elements[] = {
        "raw-tee", "encoded-tee", "ai-sink", "webrtc-sink", "kvs-sink"
    };
    for (const char* name : expected_elements) {
        GstElement* elem = gst_bin_get_by_name(GST_BIN(pipeline), name);
        EXPECT_NE(elem, nullptr) << "Expected element '" << name << "' not found in pipeline";
        if (elem) gst_object_unref(elem);
    }

    gst_object_unref(pipeline);
}

// ============================================================
// PBT helpers: generators
// ============================================================

// Generate non-empty printable ASCII string, excluding TOML special chars: " = [ ] #
static rc::Gen<std::string> genSafeTomlString() {
    return rc::gen::suchThat(
        rc::gen::container<std::string>(
            rc::gen::inRange<char>('!', '~' + 1)),
        [](const std::string& s) {
            if (s.empty()) return false;
            for (char c : s) {
                if (c == '"' || c == '=' || c == '[' || c == ']' || c == '#') {
                    return false;
                }
            }
            return true;
        });
}

// Generate non-empty printable ASCII string, excluding comma (iot-certificate delimiter)
static rc::Gen<std::string> genSafeIotString() {
    return rc::gen::suchThat(
        rc::gen::container<std::string>(
            rc::gen::inRange<char>('!', '~' + 1)),
        [](const std::string& s) {
            if (s.empty()) return false;
            for (char c : s) {
                if (c == ',') return false;
            }
            return true;
        });
}

// ============================================================
// PBT Property 1: KVS config parse round-trip
// ============================================================

// **Validates: Requirements 1.1, 5.1, 5.10**
RC_GTEST_PROP(KvsPBT, ConfigRoundTrip, ()) {
    auto stream_name = *genSafeTomlString();
    auto aws_region = *genSafeTomlString();

    // Write TOML [kvs] section
    std::string toml = "[kvs]\n";
    toml += "stream_name = \"" + stream_name + "\"\n";
    toml += "aws_region = \"" + aws_region + "\"\n";

    std::string path = write_temp_toml(toml);

    // Parse
    std::string parse_err;
    auto kv = parse_toml_section(path, "kvs", &parse_err);
    cleanup_file(path);

    RC_ASSERT(parse_err.empty());

    // Build KvsConfig
    KvsSinkFactory::KvsConfig config;
    std::string build_err;
    bool ok = KvsSinkFactory::build_kvs_config(kv, config, &build_err);

    RC_ASSERT(ok);
    RC_ASSERT(config.stream_name == stream_name);
    RC_ASSERT(config.aws_region == aws_region);
}

// ============================================================
// PBT Property 2: KVS config missing field detection
// ============================================================

// **Validates: Requirements 1.3, 5.3**
RC_GTEST_PROP(KvsPBT, MissingFieldsDetected, ()) {
    // removal_mode: 1=remove stream_name, 2=remove aws_region, 3=remove both
    int removal_mode = *rc::gen::element(1, 2, 3);

    auto stream_name = *genSafeTomlString();
    auto aws_region = *genSafeTomlString();

    std::unordered_map<std::string, std::string> kv;
    if (removal_mode != 1 && removal_mode != 3) {
        kv["stream_name"] = stream_name;
    }
    if (removal_mode != 2 && removal_mode != 3) {
        kv["aws_region"] = aws_region;
    }

    KvsSinkFactory::KvsConfig config;
    std::string err;
    bool ok = KvsSinkFactory::build_kvs_config(kv, config, &err);

    RC_ASSERT(!ok);

    // Verify error message contains all removed field names
    if (removal_mode == 1 || removal_mode == 3) {
        RC_ASSERT(err.find("stream_name") != std::string::npos);
    }
    if (removal_mode == 2 || removal_mode == 3) {
        RC_ASSERT(err.find("aws_region") != std::string::npos);
    }
}

// ============================================================
// PBT Property 3: iot-certificate string contains all fields
// ============================================================

// **Validates: Requirements 3.3**
RC_GTEST_PROP(KvsPBT, IotCertificateContainsAllFields, ()) {
    auto endpoint = *genSafeIotString();
    auto cert_path = *genSafeIotString();
    auto key_path = *genSafeIotString();
    auto ca_path = *genSafeIotString();
    auto role_alias = *genSafeIotString();

    AwsConfig aws_cfg;
    aws_cfg.credential_endpoint = endpoint;
    aws_cfg.cert_path = cert_path;
    aws_cfg.key_path = key_path;
    aws_cfg.ca_path = ca_path;
    aws_cfg.role_alias = role_alias;

    std::string result = KvsSinkFactory::build_iot_certificate_string(aws_cfg);

    // Must start with "iot-certificate,"
    RC_ASSERT(result.substr(0, 16) == "iot-certificate,");

    // Must contain all field values
    RC_ASSERT(result.find("endpoint=" + endpoint) != std::string::npos);
    RC_ASSERT(result.find("cert-path=" + cert_path) != std::string::npos);
    RC_ASSERT(result.find("key-path=" + key_path) != std::string::npos);
    RC_ASSERT(result.find("ca-path=" + ca_path) != std::string::npos);
    RC_ASSERT(result.find("role-aliases=" + role_alias) != std::string::npos);
}

// ============================================================
// Custom main: gst_init before RUN_ALL_TESTS
// ============================================================

int main(int argc, char** argv) {
    gst_init(nullptr, nullptr);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
