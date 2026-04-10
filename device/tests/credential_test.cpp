// credential_test.cpp
// Credential provider tests: 7 example-based + 9 PBT properties.
#include "credential_provider.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

// ============================================================
// Test helpers
// ============================================================

class MockHttpClient : public HttpClient {
public:
    HttpResponse preset_response;
    std::string last_url;
    std::unordered_map<std::string, std::string> last_headers;
    TlsConfig last_tls;
    int call_count = 0;

    HttpResponse get(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const TlsConfig& tls) override {
        last_url = url;
        last_headers = headers;
        last_tls = tls;
        call_count++;
        return preset_response;
    }
};

// 生成有效的 JSON 凭证响应
static std::string make_credential_json(
    const std::string& access_key = "ASIAQEXAMPLE",
    const std::string& secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
    const std::string& token = "FwoGZXIvYXdzEBYaDHqa0AP",
    const std::string& expiration = "2099-12-31T23:59:59Z") {
    return R"({"credentials":{"accessKeyId":")" + access_key +
           R"(","secretAccessKey":")" + secret_key +
           R"(","sessionToken":")" + token +
           R"(","expiration":")" + expiration + R"("}})";
}

// 生成有效的 TOML [aws] section 字符串
static std::string make_toml_aws_section(const AwsConfig& config, bool use_quotes = true) {
    std::string toml = "[aws]\n";
    auto fmt = [&](const std::string& key, const std::string& val) {
        if (use_quotes) {
            toml += key + " = \"" + val + "\"\n";
        } else {
            toml += key + " = " + val + "\n";
        }
    };
    fmt("thing_name", config.thing_name);
    fmt("credential_endpoint", config.credential_endpoint);
    fmt("role_alias", config.role_alias);
    fmt("cert_path", config.cert_path);
    fmt("key_path", config.key_path);
    fmt("ca_path", config.ca_path);
    return toml;
}

// 写入临时文件并返回路径
static std::string write_temp_file(const std::string& content,
                                   const std::string& suffix = ".toml") {
    std::string path = std::string("/tmp/credential_test_") +
                       std::to_string(std::rand()) + suffix;
    std::ofstream ofs(path);
    ofs << content;
    ofs.close();
    return path;
}

// 创建 PEM 格式的临时文件
static std::string write_temp_pem(const std::string& suffix = "_cert.pem") {
    std::string content = "-----BEGIN CERTIFICATE-----\nTESTDATA\n-----END CERTIFICATE-----\n";
    return write_temp_file(content, suffix);
}

// 创建带正确权限的私钥临时文件
static std::string write_temp_key() {
    std::string content = "-----BEGIN PRIVATE KEY-----\nTESTDATA\n-----END PRIVATE KEY-----\n";
    std::string path = write_temp_file(content, "_key.pem");
    chmod(path.c_str(), 0600);
    return path;
}

// 清理临时文件
static void cleanup_file(const std::string& path) {
    std::remove(path.c_str());
}

// 创建完整的测试环境（TOML 配置 + PEM 证书文件），返回配置文件路径
struct TestEnv {
    std::string config_path;
    std::string cert_path;
    std::string key_path;
    std::string ca_path;

    void cleanup() {
        cleanup_file(config_path);
        cleanup_file(cert_path);
        cleanup_file(key_path);
        cleanup_file(ca_path);
    }
};

static TestEnv create_test_env(const AwsConfig& base_config, bool use_quotes = true) {
    TestEnv env;
    env.cert_path = write_temp_pem("_cert.pem");
    env.key_path = write_temp_key();
    env.ca_path = write_temp_pem("_ca.pem");

    AwsConfig config = base_config;
    config.cert_path = env.cert_path;
    config.key_path = env.key_path;
    config.ca_path = env.ca_path;

    std::string toml = make_toml_aws_section(config, use_quotes);
    env.config_path = write_temp_file(toml);
    return env;
}

static AwsConfig default_aws_config() {
    return AwsConfig{
        "TestThing",
        "c19imbvbcm8u20.credentials.iot.ap-southeast-1.amazonaws.com",
        "TestRoleAlias",
        "/tmp/cert.pem",
        "/tmp/key.pem",
        "/tmp/ca.pem"
    };
}

// ============================================================
// Example-based tests
// ============================================================

// 1. CreateSuccess: mock 返回有效凭证 -> create() 成功 -> get_credentials() 有值
TEST(CredentialProvider, CreateSuccess) {
    auto mock = std::make_shared<MockHttpClient>();
    mock->preset_response.status_code = 200;
    mock->preset_response.body = make_credential_json();

    auto config = default_aws_config();
    auto env = create_test_env(config);

    std::string err;
    auto provider = CredentialProvider::create(env.config_path, mock, &err);
    ASSERT_NE(provider, nullptr) << "create() failed: " << err;

    auto creds = provider->get_credentials();
    ASSERT_NE(creds, nullptr);
    EXPECT_EQ(creds->access_key_id, "ASIAQEXAMPLE");
    EXPECT_FALSE(creds->session_token.empty());
    EXPECT_FALSE(provider->is_expired());

    provider.reset();
    env.cleanup();
}

// 2. CreateFailsOnHttpError: mock 返回 curl 错误 -> create() 返回 nullptr
TEST(CredentialProvider, CreateFailsOnHttpError) {
    auto mock = std::make_shared<MockHttpClient>();
    mock->preset_response.error_message = "Couldn't resolve host name";

    auto config = default_aws_config();
    auto env = create_test_env(config);

    std::string err;
    auto provider = CredentialProvider::create(env.config_path, mock, &err);
    EXPECT_EQ(provider, nullptr);
    EXPECT_FALSE(err.empty());

    env.cleanup();
}

// 3. CreateFailsOnMissingConfig: 配置文件不存在 -> create() 返回 nullptr
TEST(CredentialProvider, CreateFailsOnMissingConfig) {
    auto mock = std::make_shared<MockHttpClient>();
    std::string err;
    auto provider = CredentialProvider::create(
        "/tmp/nonexistent_config_12345.toml", mock, &err);
    EXPECT_EQ(provider, nullptr);
    EXPECT_FALSE(err.empty());
}

// 4. CreateFailsOnInvalidJson: mock 返回非法 JSON -> create() 返回 nullptr
TEST(CredentialProvider, CreateFailsOnInvalidJson) {
    auto mock = std::make_shared<MockHttpClient>();
    mock->preset_response.status_code = 200;
    mock->preset_response.body = "not valid json {{{";

    auto config = default_aws_config();
    auto env = create_test_env(config);

    std::string err;
    auto provider = CredentialProvider::create(env.config_path, mock, &err);
    EXPECT_EQ(provider, nullptr);
    EXPECT_FALSE(err.empty());

    env.cleanup();
}

// 5. DestructorGracefulShutdown: 创建后立即析构 -> 2 秒内完成
TEST(CredentialProvider, DestructorGracefulShutdown) {
    auto mock = std::make_shared<MockHttpClient>();
    mock->preset_response.status_code = 200;
    mock->preset_response.body = make_credential_json();

    auto config = default_aws_config();
    auto env = create_test_env(config);

    std::string err;
    auto provider = CredentialProvider::create(env.config_path, mock, &err);
    ASSERT_NE(provider, nullptr) << "create() failed: " << err;

    auto start = std::chrono::steady_clock::now();
    provider.reset();  // 触发析构
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 2);

    env.cleanup();
}

// 6. NoCopyable: static_assert 验证 CredentialProvider 不可拷贝
TEST(CredentialProvider, NoCopyable) {
    static_assert(!std::is_copy_constructible<CredentialProvider>::value,
                  "CredentialProvider should not be copy constructible");
    static_assert(!std::is_copy_assignable<CredentialProvider>::value,
                  "CredentialProvider should not be copy assignable");
}

// 7. FetchRealCredentials: 集成测试（config.toml + 证书文件都存在时执行，否则 GTEST_SKIP）
TEST(CredentialProvider, FetchRealCredentials) {
    const std::string config_path = "device/config/config.toml";
    if (!file_exists(config_path)) {
        GTEST_SKIP() << "config.toml not found, skipping integration test";
    }

    // 预检查：解析配置并验证证书文件可达，否则 skip
    std::string pre_err;
    auto kv = parse_toml_section(config_path, "aws", &pre_err);
    AwsConfig pre_config;
    if (!build_aws_config(kv, pre_config, &pre_err) ||
        !file_exists(pre_config.cert_path)) {
        GTEST_SKIP() << "Certificate files not reachable from working directory, skipping: " << pre_err;
    }

    auto http = std::make_shared<CurlHttpClient>();
    std::string err;
    auto provider = CredentialProvider::create(config_path, http, &err);
    ASSERT_NE(provider, nullptr) << "create() failed: " << err;

    auto creds = provider->get_credentials();
    ASSERT_NE(creds, nullptr);
    EXPECT_FALSE(creds->access_key_id.empty());
    EXPECT_FALSE(creds->session_token.empty());
    EXPECT_FALSE(provider->is_expired());
}

// ============================================================
// PBT helpers: generators
// ============================================================

// 生成不含 TOML 特殊字符的非空 ASCII 字符串
static rc::Gen<std::string> genSafeTomlString() {
    return rc::gen::suchThat(
        rc::gen::container<std::string>(
            rc::gen::inRange<char>(0x21, 0x7F)),  // 可打印 ASCII（不含空格）
        [](const std::string& s) {
            if (s.empty()) return false;
            for (char c : s) {
                if (c == '=' || c == '"' || c == '#' ||
                    c == '[' || c == ']' || c == '\n' || c == '\r') {
                    return false;
                }
            }
            return true;
        });
}

// 生成有效的 AwsConfig（所有字段为安全 TOML 字符串）
static rc::Gen<AwsConfig> genAwsConfig() {
    return rc::gen::build<AwsConfig>(
        rc::gen::set(&AwsConfig::thing_name, genSafeTomlString()),
        rc::gen::set(&AwsConfig::credential_endpoint, genSafeTomlString()),
        rc::gen::set(&AwsConfig::role_alias, genSafeTomlString()),
        rc::gen::set(&AwsConfig::cert_path, genSafeTomlString()),
        rc::gen::set(&AwsConfig::key_path, genSafeTomlString()),
        rc::gen::set(&AwsConfig::ca_path, genSafeTomlString()));
}

// 生成非空 ASCII 字符串（用于 JSON 字段，不含双引号和反斜杠）
static rc::Gen<std::string> genSafeJsonString() {
    return rc::gen::suchThat(
        rc::gen::container<std::string>(
            rc::gen::inRange<char>(0x20, 0x7F)),
        [](const std::string& s) {
            if (s.empty()) return false;
            for (char c : s) {
                if (c == '"' || c == '\\') return false;
            }
            return true;
        });
}

// ============================================================
// PBT Property 1: TOML 解析 round-trip
// ============================================================

// **Validates: Requirements 1.1, 1.4**
RC_GTEST_PROP(CredentialPBT, TomlRoundTrip, ()) {
    auto config = *genAwsConfig();
    bool use_quotes = *rc::gen::arbitrary<bool>();

    std::string toml = make_toml_aws_section(config, use_quotes);
    std::string path = write_temp_file(toml);

    std::string err;
    auto kv = parse_toml_section(path, "aws", &err);
    cleanup_file(path);

    RC_ASSERT(err.empty());
    RC_ASSERT(kv["thing_name"] == config.thing_name);
    RC_ASSERT(kv["credential_endpoint"] == config.credential_endpoint);
    RC_ASSERT(kv["role_alias"] == config.role_alias);
    RC_ASSERT(kv["cert_path"] == config.cert_path);
    RC_ASSERT(kv["key_path"] == config.key_path);
    RC_ASSERT(kv["ca_path"] == config.ca_path);
}

// ============================================================
// PBT Property 2: TOML 注释和空行不影响解析
// ============================================================

// **Validates: Requirements 1.5**
RC_GTEST_PROP(CredentialPBT, TomlCommentsAndBlankLines, ()) {
    auto config = *genAwsConfig();
    std::string toml = make_toml_aws_section(config, true);

    // 将 TOML 按行拆分
    std::vector<std::string> lines;
    std::istringstream iss(toml);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }

    // 在随机位置插入注释行和空行
    int insertions = *rc::gen::inRange(1, 10);
    for (int i = 0; i < insertions; ++i) {
        int pos = *rc::gen::inRange(0, static_cast<int>(lines.size()));
        bool is_comment = *rc::gen::arbitrary<bool>();
        if (is_comment) {
            auto comment_text = *genSafeTomlString();
            lines.insert(lines.begin() + pos, "# " + comment_text);
        } else {
            lines.insert(lines.begin() + pos, "");
        }
    }

    // 重新组合
    std::string modified;
    for (const auto& l : lines) {
        modified += l + "\n";
    }

    // 解析原始版本
    std::string path_orig = write_temp_file(toml);
    std::string err_orig;
    auto kv_orig = parse_toml_section(path_orig, "aws", &err_orig);
    cleanup_file(path_orig);

    // 解析修改版本
    std::string path_mod = write_temp_file(modified);
    std::string err_mod;
    auto kv_mod = parse_toml_section(path_mod, "aws", &err_mod);
    cleanup_file(path_mod);

    RC_ASSERT(kv_orig == kv_mod);
}

// ============================================================
// PBT Property 3: TOML 缺失字段检测
// ============================================================

// **Validates: Requirements 1.3**
RC_GTEST_PROP(CredentialPBT, TomlMissingFieldsDetected, ()) {
    static const std::vector<std::string> all_fields = {
        "thing_name", "credential_endpoint", "role_alias",
        "cert_path", "key_path", "ca_path"
    };

    // 随机选择要移除的字段（非空真子集）
    auto mask = *rc::gen::suchThat(
        rc::gen::container<std::vector<bool>>(6, rc::gen::arbitrary<bool>()),
        [](const std::vector<bool>& v) {
            // 至少移除一个，但不能全部移除（真子集）
            int removed = 0;
            for (bool b : v) if (b) removed++;
            return removed > 0 && removed < 6;
        });

    std::vector<std::string> removed_fields;
    std::unordered_map<std::string, std::string> kv;

    auto config = *genAwsConfig();
    // 构建完整 kv，然后移除选中的字段
    kv["thing_name"] = config.thing_name;
    kv["credential_endpoint"] = config.credential_endpoint;
    kv["role_alias"] = config.role_alias;
    kv["cert_path"] = config.cert_path;
    kv["key_path"] = config.key_path;
    kv["ca_path"] = config.ca_path;

    for (int i = 0; i < 6; ++i) {
        if (mask[i]) {
            removed_fields.push_back(all_fields[i]);
            kv.erase(all_fields[i]);
        }
    }

    AwsConfig out;
    std::string err;
    bool ok = build_aws_config(kv, out, &err);

    RC_ASSERT(!ok);
    for (const auto& field : removed_fields) {
        RC_ASSERT(err.find(field) != std::string::npos);
    }
}

// ============================================================
// PBT Property 4: HTTP 请求参数完整性
// ============================================================

// **Validates: Requirements 2.1, 2.2, 2.3, 2.4**
RC_GTEST_PROP(CredentialPBT, HttpRequestParams, ()) {
    auto config = *genAwsConfig();

    auto mock = std::make_shared<MockHttpClient>();
    mock->preset_response.status_code = 200;
    mock->preset_response.body = make_credential_json();

    // 创建真实的临时文件环境
    auto env = create_test_env(config);

    std::string err;
    auto provider = CredentialProvider::create(env.config_path, mock, &err);
    RC_ASSERT(provider != nullptr);

    // 验证 URL 格式
    // 需要从实际写入的 TOML 中读取配置（cert_path 等被替换为临时文件路径）
    // 但 endpoint 和 role_alias 保持原始值
    std::string expected_url = "https://" + config.credential_endpoint +
                               "/role-aliases/" + config.role_alias + "/credentials";
    RC_ASSERT(mock->last_url == expected_url);

    // 验证 header
    auto it = mock->last_headers.find("x-amzn-iot-thingname");
    RC_ASSERT(it != mock->last_headers.end());
    RC_ASSERT(it->second == config.thing_name);

    // 验证 TLS 配置（使用临时文件路径）
    RC_ASSERT(mock->last_tls.cert_path == env.cert_path);
    RC_ASSERT(mock->last_tls.key_path == env.key_path);
    RC_ASSERT(mock->last_tls.ca_path == env.ca_path);

    provider.reset();
    env.cleanup();
}

// ============================================================
// PBT Property 5: 非 200 状态码返回错误
// ============================================================

// **Validates: Requirements 2.7**
RC_GTEST_PROP(CredentialPBT, NonOkStatusReturnsError, ()) {
    // 生成非 200 的状态码 [100, 599]
    int code = *rc::gen::suchThat(
        rc::gen::inRange(100, 600),
        [](int c) { return c != 200; });

    auto mock = std::make_shared<MockHttpClient>();
    mock->preset_response.status_code = code;
    mock->preset_response.body = "error body";

    auto config = default_aws_config();
    auto env = create_test_env(config);

    std::string err;
    auto provider = CredentialProvider::create(env.config_path, mock, &err);
    RC_ASSERT(provider == nullptr);
    RC_ASSERT(err.find(std::to_string(code)) != std::string::npos);

    env.cleanup();
}

// ============================================================
// PBT Property 6: JSON 凭证解析 round-trip
// ============================================================

// **Validates: Requirements 3.1, 3.4, 3.5**
RC_GTEST_PROP(CredentialPBT, JsonCredentialRoundTrip, ()) {
    auto access_key = *genSafeJsonString();
    auto secret_key = *genSafeJsonString();
    auto token = *genSafeJsonString();

    // 生成合理时间范围的 time_point（2020-2099）
    int year = *rc::gen::inRange(2020, 2100);
    int month = *rc::gen::inRange(1, 13);
    int day = *rc::gen::inRange(1, 29);  // 避免月末问题
    int hour = *rc::gen::inRange(0, 24);
    int minute = *rc::gen::inRange(0, 60);
    int second = *rc::gen::inRange(0, 60);

    char exp_buf[64];
    std::snprintf(exp_buf, sizeof(exp_buf),
                  "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  year, month, day, hour, minute, second);
    std::string expiration_str(exp_buf);

    // 解析预期的 time_point
    std::chrono::system_clock::time_point expected_tp;
    std::string parse_err;
    bool tp_ok = parse_iso8601(expiration_str, expected_tp, &parse_err);
    RC_ASSERT(tp_ok);

    // 序列化为 JSON
    std::string json = make_credential_json(access_key, secret_key, token, expiration_str);

    // 解析
    StsCredential cred;
    std::string err;
    bool ok = parse_credential_json(json, cred, &err);
    RC_ASSERT(ok);
    RC_ASSERT(cred.access_key_id == access_key);
    RC_ASSERT(cred.secret_access_key == secret_key);
    RC_ASSERT(cred.session_token == token);

    // expiration 精确到秒
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
        cred.expiration - expected_tp).count();
    RC_ASSERT(diff == 0);
}

// ============================================================
// PBT Property 7: JSON 缺失字段检测
// ============================================================

// **Validates: Requirements 3.3**
RC_GTEST_PROP(CredentialPBT, JsonMissingFieldsDetected, ()) {
    static const std::vector<std::string> json_fields = {
        "accessKeyId", "secretAccessKey", "sessionToken", "expiration"
    };

    // 随机选择要移除的字段（非空真子集）
    auto mask = *rc::gen::suchThat(
        rc::gen::container<std::vector<bool>>(4, rc::gen::arbitrary<bool>()),
        [](const std::vector<bool>& v) {
            int removed = 0;
            for (bool b : v) if (b) removed++;
            return removed > 0 && removed < 4;
        });

    // 构建 JSON，移除选中的字段
    std::string json = R"({"credentials":{)";
    bool first = true;
    std::vector<std::pair<std::string, std::string>> field_values = {
        {"accessKeyId", "ASIAQEXAMPLE"},
        {"secretAccessKey", "wJalrXUtnFEMI"},
        {"sessionToken", "FwoGZXIvYXdz"},
        {"expiration", "2099-12-31T23:59:59Z"}
    };

    for (int i = 0; i < 4; ++i) {
        if (!mask[i]) {  // 保留未被移除的字段
            if (!first) json += ",";
            json += "\"" + field_values[i].first + "\":\"" + field_values[i].second + "\"";
            first = false;
        }
    }
    json += "}}";

    StsCredential cred;
    std::string err;
    bool ok = parse_credential_json(json, cred, &err);
    RC_ASSERT(!ok);
    RC_ASSERT(!err.empty());
}

// ============================================================
// PBT Property 8: 缓存读取不触发网络请求
// ============================================================

// **Validates: Requirements 4.3**
RC_GTEST_PROP(CredentialPBT, CacheDoesNotTriggerNetwork, ()) {
    int n = *rc::gen::inRange(2, 101);

    auto mock = std::make_shared<MockHttpClient>();
    mock->preset_response.status_code = 200;
    mock->preset_response.body = make_credential_json();

    auto config = default_aws_config();
    auto env = create_test_env(config);

    std::string err;
    auto provider = CredentialProvider::create(env.config_path, mock, &err);
    RC_ASSERT(provider != nullptr);

    // 连续调用 N 次 get_credentials()
    for (int i = 0; i < n; ++i) {
        auto creds = provider->get_credentials();
        RC_ASSERT(creds != nullptr);
    }

    // mock 仅被调用 1 次（初始化时的同步获取）
    RC_ASSERT(mock->call_count == 1);

    provider.reset();
    env.cleanup();
}

// ============================================================
// PBT Property 9: 过期判断正确性
// ============================================================

// **Validates: Requirements 6.4**
RC_GTEST_PROP(CredentialPBT, ExpirationCheck, ()) {
    // 随机选择：过期 or 未过期
    bool should_be_expired = *rc::gen::arbitrary<bool>();

    auto mock = std::make_shared<MockHttpClient>();
    mock->preset_response.status_code = 200;

    auto now = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point exp_tp;

    if (should_be_expired) {
        // 过期：expiration 在过去（1 秒到 1 小时前）
        int secs_ago = *rc::gen::inRange(1, 3601);
        exp_tp = now - std::chrono::seconds(secs_ago);
    } else {
        // 未过期：expiration 在未来（超过 1 秒）
        int secs_ahead = *rc::gen::inRange(2, 3601);
        exp_tp = now + std::chrono::seconds(secs_ahead);
    }

    // 将 time_point 转为 ISO 8601 字符串
    auto t = std::chrono::system_clock::to_time_t(exp_tp);
    std::tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    std::string exp_str(buf);

    mock->preset_response.body = make_credential_json(
        "ASIAQEXAMPLE", "wJalrXUtnFEMI", "FwoGZXIvYXdz", exp_str);

    auto config = default_aws_config();
    auto env = create_test_env(config);

    std::string err;
    auto provider = CredentialProvider::create(env.config_path, mock, &err);
    RC_ASSERT(provider != nullptr);

    if (should_be_expired) {
        RC_ASSERT(provider->is_expired() == true);
    } else {
        RC_ASSERT(provider->is_expired() == false);
    }

    provider.reset();
    env.cleanup();
}
