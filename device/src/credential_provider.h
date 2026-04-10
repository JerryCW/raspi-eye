// credential_provider.h
// AWS IoT Credentials Provider client with background refresh.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

// ============================================================
// Data structures
// ============================================================

struct AwsConfig {
    std::string thing_name;
    std::string credential_endpoint;
    std::string role_alias;
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
};

struct StsCredential {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
    std::chrono::system_clock::time_point expiration;
};

struct TlsConfig {
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
};

struct HttpResponse {
    long status_code = 0;
    std::string body;
    std::string error_message;
};

using CredentialCallback = std::function<void()>;

// ============================================================
// TOML 解析（仅 [aws] section）
// ============================================================

std::unordered_map<std::string, std::string> parse_toml_section(
    const std::string& file_path,
    const std::string& section_name,
    std::string* error_msg = nullptr);

bool build_aws_config(
    const std::unordered_map<std::string, std::string>& kv,
    AwsConfig& config,
    std::string* error_msg = nullptr);

// ============================================================
// JSON 解析
// ============================================================

bool parse_credential_json(
    const std::string& json_body,
    StsCredential& credential,
    std::string* error_msg = nullptr);

bool parse_iso8601(
    const std::string& time_str,
    std::chrono::system_clock::time_point& tp,
    std::string* error_msg = nullptr);

// ============================================================
// 证书预检查
// ============================================================

bool file_exists(const std::string& path);
bool is_pem_format(const std::string& file_path);
bool check_key_permissions(const std::string& file_path);
bool validate_cert_files(const AwsConfig& config, std::string* error_msg = nullptr);

// ============================================================
// HttpClient 接口
// ============================================================

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse get(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const TlsConfig& tls) = 0;
};

// ============================================================
// CurlHttpClient
// ============================================================

class CurlHttpClient : public HttpClient {
public:
    CurlHttpClient();
    ~CurlHttpClient() override;
    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;
    HttpResponse get(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers,
        const TlsConfig& tls) override;
private:
    static void ensure_curl_global_init();
    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);
};

// ============================================================
// CredentialProvider
// ============================================================

class CredentialProvider {
public:
    ~CredentialProvider();
    CredentialProvider(const CredentialProvider&) = delete;
    CredentialProvider& operator=(const CredentialProvider&) = delete;

    static std::unique_ptr<CredentialProvider> create(
        const std::string& config_path,
        std::shared_ptr<HttpClient> http_client,
        std::string* error_msg = nullptr);

    std::shared_ptr<const StsCredential> get_credentials() const;
    bool is_expired() const;
    void set_credential_callback(CredentialCallback cb);
    const AwsConfig& config() const { return config_; }

private:
    CredentialProvider(AwsConfig config, std::shared_ptr<HttpClient> http_client);
    bool fetch_credentials(std::string* error_msg = nullptr);
    void refresh_loop();

    AwsConfig config_;
    std::shared_ptr<HttpClient> http_client_;
    mutable std::shared_mutex credential_mutex_;
    std::shared_ptr<const StsCredential> cached_credential_;
    std::thread refresh_thread_;
    std::mutex refresh_mutex_;
    std::condition_variable refresh_cv_;
    bool stop_requested_ = false;
    static constexpr int kRefreshBeforeExpirySec = 300;
    static constexpr int kInitialRetryDelaySec = 1;
    static constexpr int kMaxRetryDelaySec = 60;
    static constexpr int kMaxRetries = 10;
    CredentialCallback credential_cb_;
};
