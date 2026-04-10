// credential_provider.cpp
// AWS IoT Credentials Provider — TOML parser, cert validation, HTTP client, core logic.

#include "credential_provider.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

// ============================================================
// Internal helpers
// ============================================================

namespace {

// Remove leading and trailing whitespace
std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

}  // namespace

// ============================================================
// TOML parser
// ============================================================

std::unordered_map<std::string, std::string> parse_toml_section(
    const std::string& file_path,
    const std::string& section_name,
    std::string* error_msg) {
    std::unordered_map<std::string, std::string> result;
    std::ifstream file(file_path);
    if (!file.is_open()) {
        if (error_msg) *error_msg = "Cannot open config file: " + file_path;
        return result;
    }

    std::string target = "[" + section_name + "]";
    bool in_section = false;
    std::string line;

    while (std::getline(file, line)) {
        auto trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        if (trimmed[0] == '[') {
            in_section = (trimmed == target);
            continue;
        }
        if (!in_section) continue;
        auto eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) continue;
        auto key = trim(trimmed.substr(0, eq_pos));
        auto val = trim(trimmed.substr(eq_pos + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        result[key] = val;
    }
    return result;
}

bool build_aws_config(
    const std::unordered_map<std::string, std::string>& kv,
    AwsConfig& config,
    std::string* error_msg) {
    static const std::vector<std::pair<std::string, std::string AwsConfig::*>> fields = {
        {"thing_name",           &AwsConfig::thing_name},
        {"credential_endpoint",  &AwsConfig::credential_endpoint},
        {"role_alias",           &AwsConfig::role_alias},
        {"cert_path",            &AwsConfig::cert_path},
        {"key_path",             &AwsConfig::key_path},
        {"ca_path",              &AwsConfig::ca_path},
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
            std::string msg = "Missing required fields: ";
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

// ============================================================
// Certificate pre-checks
// ============================================================

bool file_exists(const std::string& path) {
    struct stat st{};
    return (stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
}

bool is_pem_format(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return content.find("-----BEGIN") != std::string::npos &&
           content.find("-----END") != std::string::npos;
}

bool check_key_permissions(const std::string& file_path) {
    struct stat st{};
    if (stat(file_path.c_str(), &st) != 0) return false;
    // group and other must have no permissions
    mode_t perms = st.st_mode & 0777;
    return (perms & 0077) == 0;  // chmod 400 or 600
}

bool validate_cert_files(const AwsConfig& config, std::string* error_msg) {
    // Check cert file
    if (!file_exists(config.cert_path)) {
        if (error_msg) *error_msg = "Certificate file not found: " + config.cert_path;
        return false;
    }
    if (!is_pem_format(config.cert_path)) {
        if (error_msg) *error_msg = "Certificate file is not PEM format: " + config.cert_path;
        return false;
    }

    // Check key file
    if (!file_exists(config.key_path)) {
        if (error_msg) *error_msg = "Private key file not found: " + config.key_path;
        return false;
    }
    if (!is_pem_format(config.key_path)) {
        if (error_msg) *error_msg = "Private key file is not PEM format: " + config.key_path;
        return false;
    }
    if (!check_key_permissions(config.key_path)) {
        if (error_msg) *error_msg = "Private key file has insecure permissions (expected chmod 400 or 600): " + config.key_path;
        return false;
    }

    // Check CA file
    if (!file_exists(config.ca_path)) {
        if (error_msg) *error_msg = "CA certificate file not found: " + config.ca_path;
        return false;
    }
    if (!is_pem_format(config.ca_path)) {
        if (error_msg) *error_msg = "CA certificate file is not PEM format: " + config.ca_path;
        return false;
    }

    return true;
}


// ============================================================
// ISO 8601 parser
// ============================================================

bool parse_iso8601(
    const std::string& time_str,
    std::chrono::system_clock::time_point& tp,
    std::string* error_msg) {
    std::tm tm = {};
    std::istringstream ss(time_str);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
        if (error_msg) *error_msg = "Invalid ISO 8601 format: " + time_str;
        return false;
    }
    time_t t = timegm(&tm);
    if (t == -1) {
        if (error_msg) *error_msg = "timegm failed for: " + time_str;
        return false;
    }
    tp = std::chrono::system_clock::from_time_t(t);
    return true;
}

// ============================================================
// JSON credential parser
// ============================================================

bool parse_credential_json(
    const std::string& json_body,
    StsCredential& credential,
    std::string* error_msg) {
    try {
        auto j = nlohmann::json::parse(json_body);
        auto& creds = j.at("credentials");
        credential.access_key_id = creds.at("accessKeyId").get<std::string>();
        credential.secret_access_key = creds.at("secretAccessKey").get<std::string>();
        credential.session_token = creds.at("sessionToken").get<std::string>();
        std::string exp_str = creds.at("expiration").get<std::string>();
        if (!parse_iso8601(exp_str, credential.expiration, error_msg)) {
            return false;
        }
        return true;
    } catch (const nlohmann::json::exception& e) {
        if (error_msg) *error_msg = std::string("JSON parse error: ") + e.what();
        return false;
    }
}

// ============================================================
// CurlHttpClient
// ============================================================

void CurlHttpClient::ensure_curl_global_init() {
    static bool initialized = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        std::atexit(curl_global_cleanup);
        return true;
    }();
    (void)initialized;
}

size_t CurlHttpClient::write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

CurlHttpClient::CurlHttpClient() {
    ensure_curl_global_init();
}

CurlHttpClient::~CurlHttpClient() = default;

HttpResponse CurlHttpClient::get(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const TlsConfig& tls) {

    ensure_curl_global_init();
    HttpResponse response;

    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error_message = "curl_easy_init failed";
        return response;
    }

    // URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Headers
    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // mTLS 配置
    curl_easy_setopt(curl, CURLOPT_SSLCERT, tls.cert_path.c_str());
    curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLKEY, tls.key_path.c_str());
    curl_easy_setopt(curl, CURLOPT_CAINFO, tls.ca_path.c_str());
    // 系统 CA 库作为 fallback（Linux: /etc/ssl/certs）
    curl_easy_setopt(curl, CURLOPT_CAPATH, "/etc/ssl/certs");

    // TLS 版本限制：最低 TLS 1.2，最高 TLS 1.3
    constexpr long CURL_SSLVERSION_MAX_TLSv1_3_VAL = (7L << 16);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION,
                     CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_3_VAL);

    // 超时：连接 5s，完成 10s
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    // 响应体写入
    std::string body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    // 执行请求
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        response.error_message = curl_easy_strerror(res);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
        response.body = std::move(body);
    }

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    return response;
}

// ============================================================
// CredentialProvider
// ============================================================

CredentialProvider::CredentialProvider(AwsConfig config,
                                       std::shared_ptr<HttpClient> http_client)
    : config_(std::move(config)),
      http_client_(std::move(http_client)) {}

CredentialProvider::~CredentialProvider() {
    {
        std::lock_guard lock(refresh_mutex_);
        stop_requested_ = true;
    }
    refresh_cv_.notify_all();
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
}

std::unique_ptr<CredentialProvider> CredentialProvider::create(
    const std::string& config_path,
    std::shared_ptr<HttpClient> http_client,
    std::string* error_msg) {
    // 1. 解析 TOML
    auto kv = parse_toml_section(config_path, "aws", error_msg);
    if (kv.empty() && error_msg && !error_msg->empty()) {
        return nullptr;
    }

    AwsConfig config;
    if (!build_aws_config(kv, config, error_msg)) {
        return nullptr;
    }

    // 2. 验证证书文件
    if (!validate_cert_files(config, error_msg)) {
        return nullptr;
    }

    // 3. 创建实例
    auto provider = std::unique_ptr<CredentialProvider>(
        new CredentialProvider(std::move(config), std::move(http_client)));

    // 4. 同步获取首次凭证
    if (!provider->fetch_credentials(error_msg)) {
        return nullptr;
    }

    // 5. 启动后台刷新线程
    provider->refresh_thread_ = std::thread(&CredentialProvider::refresh_loop, provider.get());

    spdlog::info("CredentialProvider initialized for thing: {}", provider->config_.thing_name);
    return provider;
}

bool CredentialProvider::fetch_credentials(std::string* error_msg) {
    std::string url = "https://" + config_.credential_endpoint
        + "/role-aliases/" + config_.role_alias + "/credentials";

    std::unordered_map<std::string, std::string> headers;
    headers["x-amzn-iot-thingname"] = config_.thing_name;

    TlsConfig tls{config_.cert_path, config_.key_path, config_.ca_path};

    auto response = http_client_->get(url, headers, tls);

    if (!response.error_message.empty()) {
        if (error_msg) *error_msg = "HTTP request failed: " + response.error_message;
        return false;
    }

    if (response.status_code != 200) {
        if (error_msg) {
            *error_msg = "HTTP " + std::to_string(response.status_code) + ": " + response.body;
        }
        return false;
    }

    auto credential = std::make_shared<StsCredential>();
    if (!parse_credential_json(response.body, *credential, error_msg)) {
        return false;
    }

    {
        std::unique_lock lock(credential_mutex_);
        cached_credential_ = std::move(credential);
    }
    return true;
}

std::shared_ptr<const StsCredential> CredentialProvider::get_credentials() const {
    std::shared_lock lock(credential_mutex_);
    return cached_credential_;
}

bool CredentialProvider::is_expired() const {
    std::shared_lock lock(credential_mutex_);
    if (!cached_credential_) return true;
    return std::chrono::system_clock::now() >= cached_credential_->expiration;
}

void CredentialProvider::set_credential_callback(CredentialCallback cb) {
    credential_cb_ = std::move(cb);
}

void CredentialProvider::refresh_loop() {
    while (true) {
        std::chrono::system_clock::time_point refresh_at;
        {
            std::shared_lock lock(credential_mutex_);
            if (cached_credential_) {
                refresh_at = cached_credential_->expiration
                    - std::chrono::seconds(kRefreshBeforeExpirySec);
            }
        }

        {
            std::unique_lock lock(refresh_mutex_);
            auto now = std::chrono::system_clock::now();
            if (refresh_at > now) {
                refresh_cv_.wait_until(lock, refresh_at, [this] {
                    return stop_requested_;
                });
            }
            if (stop_requested_) return;
        }

        int retries = 0;
        int delay_sec = kInitialRetryDelaySec;
        bool success = false;
        while (retries < kMaxRetries && !stop_requested_) {
            std::string err;
            if (fetch_credentials(&err)) {
                success = true;
                spdlog::info("Credential refreshed successfully");
                break;
            }
            retries++;
            spdlog::warn("Credential refresh failed (attempt {}/{}): {}",
                         retries, kMaxRetries, err);

            {
                std::unique_lock lock(refresh_mutex_);
                refresh_cv_.wait_for(lock,
                    std::chrono::seconds(delay_sec),
                    [this] { return stop_requested_; });
                if (stop_requested_) return;
            }
            delay_sec = std::min(delay_sec * 2, kMaxRetryDelaySec);
        }

        if (!success && is_expired()) {
            spdlog::error("Credential expired and refresh failed after {} retries",
                          kMaxRetries);
            if (credential_cb_) {
                credential_cb_();
            }
        }
    }
}
