// s3_uploader.cpp
// S3 snapshot uploader — SigV4 pure functions + S3Uploader class.
#include "s3_uploader.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <spdlog/spdlog.h>

// ============================================================
// to_hex — byte array to lowercase hex string
// ============================================================

std::string to_hex(const uint8_t* data, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result.push_back(hex_chars[(data[i] >> 4) & 0x0F]);
        result.push_back(hex_chars[data[i] & 0x0F]);
    }
    return result;
}

std::string to_hex(const std::vector<uint8_t>& bytes) {
    return to_hex(bytes.data(), bytes.size());
}

// ============================================================
// sha256_hex — SHA256 hash via OpenSSL EVP API
// ============================================================

std::string sha256_hex(const uint8_t* data, size_t len) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    return to_hex(hash, hash_len);
}

std::string sha256_hex(const std::string& data) {
    return sha256_hex(reinterpret_cast<const uint8_t*>(data.data()),
                      data.size());
}

// ============================================================
// hmac_sha256 — HMAC-SHA256 via OpenSSL HMAC API
// ============================================================

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                  const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         result, &result_len);
    return std::vector<uint8_t>(result, result + result_len);
}

std::vector<uint8_t> hmac_sha256(const std::string& key,
                                  const std::string& data) {
    std::vector<uint8_t> key_bytes(key.begin(), key.end());
    return hmac_sha256(key_bytes, data);
}

// ============================================================
// uri_encode — percent-encode for S3 paths
// ============================================================

std::string uri_encode(const std::string& input, bool encode_slash) {
    std::ostringstream encoded;
    encoded << std::hex << std::uppercase;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else if (c == '/' && !encode_slash) {
            encoded << '/';
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<int>(c);
        }
    }
    return encoded.str();
}

// ============================================================
// build_canonical_request — 6 parts separated by \n
// ============================================================

std::string build_canonical_request(
    const std::string& method,
    const std::string& uri_path,
    const std::string& canonical_headers,
    const std::string& signed_headers,
    const std::string& payload_hash) {
    // method \n uri \n query_string \n canonical_headers \n signed_headers \n payload_hash
    // query_string is always empty for S3 PUT
    return method + "\n" +
           uri_path + "\n" +
           "\n" +
           canonical_headers + "\n" +
           signed_headers + "\n" +
           payload_hash;
}

// ============================================================
// build_string_to_sign — 4 lines
// ============================================================

std::string build_string_to_sign(
    const std::string& timestamp,
    const std::string& scope,
    const std::string& canonical_request_hash) {
    return "AWS4-HMAC-SHA256\n" +
           timestamp + "\n" +
           scope + "\n" +
           canonical_request_hash;
}

// ============================================================
// derive_signing_key — 4-step HMAC chain
// ============================================================

std::vector<uint8_t> derive_signing_key(
    const std::string& secret_key,
    const std::string& date,
    const std::string& region,
    const std::string& service) {
    std::string initial_key = "AWS4" + secret_key;
    std::vector<uint8_t> key_bytes(initial_key.begin(), initial_key.end());

    auto date_key = hmac_sha256(key_bytes, date);
    auto region_key = hmac_sha256(date_key, region);
    auto service_key = hmac_sha256(region_key, service);
    auto signing_key = hmac_sha256(service_key, "aws4_request");
    return signing_key;
}

// ============================================================
// build_authorization_header
// ============================================================

std::string build_authorization_header(
    const std::string& access_key,
    const std::string& scope,
    const std::string& signed_headers,
    const std::string& signature) {
    return "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
           ", SignedHeaders=" + signed_headers +
           ", Signature=" + signature;
}

// ============================================================
// build_s3_key — validate fields and build path
// ============================================================

namespace {

bool is_safe_field(const std::string& field) {
    if (field.empty()) return false;
    for (char c : field) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string build_s3_key(
    const std::string& device_id,
    const std::string& date,
    const std::string& event_id,
    const std::string& filename) {
    if (!is_safe_field(device_id) || !is_safe_field(date) ||
        !is_safe_field(event_id) || !is_safe_field(filename)) {
        return "";
    }
    return device_id + "/" + date + "/" + event_id + "/" + filename;
}

// ============================================================
// scan_closed_events — free function for testability
// ============================================================

std::vector<std::string> scan_closed_events(const std::string& snapshot_dir) {
    auto s3_log = spdlog::get("s3");
    if (!s3_log) s3_log = spdlog::default_logger();

    std::vector<std::string> result;
    if (!std::filesystem::exists(snapshot_dir)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(snapshot_dir)) {
        if (!entry.is_directory()) continue;

        // .uploaded marker present — delete directory directly (crash recovery)
        if (std::filesystem::exists(entry.path() / ".uploaded")) {
            std::error_code ec;
            std::filesystem::remove_all(entry.path(), ec);
            if (ec) {
                s3_log->warn("Failed to remove uploaded directory {}: {}",
                             entry.path().string(), ec.message());
            }
            continue;
        }

        auto event_json_path = entry.path() / "event.json";
        if (!std::filesystem::exists(event_json_path)) continue;

        try {
            std::ifstream ifs(event_json_path);
            if (!ifs.is_open()) {
                s3_log->warn("Cannot open {}", event_json_path.string());
                continue;
            }
            auto j = nlohmann::json::parse(ifs);
            if (j.value("status", "") == "closed" && j.contains("end_time")) {
                result.push_back(entry.path().string());
            }
        } catch (const nlohmann::json::exception& e) {
            s3_log->warn("Failed to parse {}: {}", event_json_path.string(), e.what());
        }
    }
    return result;
}

// ============================================================
// libcurl read callback for PUT upload
// ============================================================

namespace {

struct ReadContext {
    const std::vector<uint8_t>* data;
    size_t offset;
};

size_t curl_read_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<ReadContext*>(userdata);
    size_t remaining = ctx->data->size() - ctx->offset;
    size_t to_copy = std::min(size * nitems, remaining);
    std::memcpy(buffer, ctx->data->data() + ctx->offset, to_copy);
    ctx->offset += to_copy;
    return to_copy;
}

// Default PUT function using libcurl
bool default_put_fn(const std::string& url,
                    const std::vector<uint8_t>& data,
                    const std::vector<std::string>& headers) {
    auto s3_log = spdlog::get("s3");
    if (!s3_log) s3_log = spdlog::default_logger();

    CURL* curl = curl_easy_init();
    if (!curl) {
        s3_log->error("curl_easy_init() failed");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(data.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    ReadContext read_ctx{&data, 0};
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, curl_read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &read_ctx);

    struct curl_slist* header_list = nullptr;
    for (const auto& h : headers) {
        header_list = curl_slist_append(header_list, h.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        s3_log->warn("curl PUT failed: {}", curl_easy_strerror(res));
        return false;
    }
    if (http_code != 200) {
        s3_log->warn("S3 PUT returned HTTP {}", http_code);
        return false;
    }
    return true;
}

// Determine content-type from file extension
std::string content_type_for(const std::string& filename) {
    auto ext = std::filesystem::path(filename).extension().string();
    // lowercase the extension
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".json") return "application/json";
    return "application/octet-stream";
}

}  // namespace

// ============================================================
// S3Uploader — private constructor
// ============================================================

S3Uploader::S3Uploader(const S3Config& config,
                       const std::string& snapshot_dir,
                       const std::string& device_id,
                       std::shared_ptr<CredentialProvider> credential_provider)
    : config_(config),
      snapshot_dir_(snapshot_dir),
      device_id_(device_id),
      credential_provider_(std::move(credential_provider)),
      put_fn_(default_put_fn) {}

// ============================================================
// S3Uploader::create — factory method
// ============================================================

std::unique_ptr<S3Uploader> S3Uploader::create(
    const S3Config& config,
    const std::string& snapshot_dir,
    const std::string& device_id,
    std::shared_ptr<CredentialProvider> credential_provider,
    std::string* error_msg) {

    if (!credential_provider) {
        if (error_msg) *error_msg = "CredentialProvider is null";
        return nullptr;
    }
    if (config.bucket.empty()) {
        if (error_msg) *error_msg = "S3 bucket name is empty";
        return nullptr;
    }

    auto uploader = std::unique_ptr<S3Uploader>(
        new S3Uploader(config, snapshot_dir, device_id, std::move(credential_provider)));

    auto s3_log = spdlog::get("s3");
    if (!s3_log) s3_log = spdlog::default_logger();
    s3_log->info("S3Uploader created: bucket={}, region={}, scan_interval={}s",
                 config.bucket, config.region, config.scan_interval_sec);

    return uploader;
}

// ============================================================
// S3Uploader::set_put_function
// ============================================================

void S3Uploader::set_put_function(S3PutFunction fn) {
    put_fn_ = std::move(fn);
}

// ============================================================
// S3Uploader::start — launch background scan thread
// ============================================================

bool S3Uploader::start(std::string* error_msg) {
    if (scan_thread_.joinable()) {
        if (error_msg) *error_msg = "S3Uploader already started";
        return false;
    }
    stop_requested_ = false;
    scan_thread_ = std::thread(&S3Uploader::scan_loop, this);
    auto s3_log = spdlog::get("s3");
    if (!s3_log) s3_log = spdlog::default_logger();
    s3_log->info("S3Uploader scan thread started");
    return true;
}

// ============================================================
// S3Uploader::stop — signal thread to exit and join
// ============================================================

void S3Uploader::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (scan_thread_.joinable()) {
        scan_thread_.join();
        auto s3_log = spdlog::get("s3");
        if (!s3_log) s3_log = spdlog::default_logger();
        s3_log->info("S3Uploader scan thread stopped");
    }
}

// ============================================================
// S3Uploader::~S3Uploader
// ============================================================

S3Uploader::~S3Uploader() {
    stop();
}

// ============================================================
// S3Uploader::scan_loop — background scan thread entry
// ============================================================

void S3Uploader::scan_loop() {
    auto s3_log = spdlog::get("s3");
    if (!s3_log) s3_log = spdlog::default_logger();

    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::seconds(config_.scan_interval_sec),
                             [this] { return stop_requested_; })) {
                break;  // stop requested
            }
        }

        auto events = scan_closed_events(snapshot_dir_);
        int uploaded_count = 0;
        int skipped_count = 0;

        for (const auto& event_dir : events) {
            // Check stop signal between events
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_requested_) return;
            }

            if (upload_event(event_dir)) {
                ++uploaded_count;
            } else {
                ++skipped_count;
            }
        }

        s3_log->info("S3 scan complete: found={}, uploaded={}, skipped={}",
                     events.size(), uploaded_count, skipped_count);
    }
}

// ============================================================
// S3Uploader::upload_event — upload all files in an event dir
// ============================================================

bool S3Uploader::upload_event(const std::string& event_dir) {
    auto s3_log = spdlog::get("s3");
    if (!s3_log) s3_log = spdlog::default_logger();

    namespace fs = std::filesystem;

    auto event_json_path = fs::path(event_dir) / "event.json";
    if (!fs::exists(event_json_path)) {
        s3_log->warn("event.json not found in {}", event_dir);
        return false;
    }

    // Parse event.json to extract date and event_id
    std::string date_str;
    std::string event_id;
    try {
        std::ifstream ifs(event_json_path);
        auto j = nlohmann::json::parse(ifs);
        event_id = j.value("event_id", "");
        std::string start_time = j.value("start_time", "");
        // Extract YYYY-MM-DD from ISO 8601 timestamp
        if (start_time.size() >= 10) {
            date_str = start_time.substr(0, 10);
        }
    } catch (const nlohmann::json::exception& e) {
        s3_log->error("Failed to parse event.json in {}: {}", event_dir, e.what());
        return false;
    }

    if (event_id.empty() || date_str.empty()) {
        s3_log->error("Missing event_id or start_time in {}", event_dir);
        return false;
    }

    // Upload each file in the event directory
    for (const auto& file_entry : fs::directory_iterator(event_dir)) {
        if (!file_entry.is_regular_file()) continue;
        auto filename = file_entry.path().filename().string();
        if (filename == ".uploaded") continue;

        auto s3_key = build_s3_key(device_id_, date_str, event_id, filename);
        if (s3_key.empty()) {
            s3_log->error("Invalid S3 key for file {} in {}", filename, event_dir);
            return false;
        }

        auto ctype = content_type_for(filename);
        if (!upload_file(file_entry.path().string(), s3_key, ctype)) {
            s3_log->error("Failed to upload {} after retries", filename);
            return false;
        }
    }

    // All files uploaded — write .uploaded marker then delete directory
    auto marker_path = fs::path(event_dir) / ".uploaded";
    {
        std::ofstream marker(marker_path);
        if (!marker.is_open()) {
            s3_log->error("Failed to write .uploaded marker in {}", event_dir);
            return false;
        }
    }

    std::error_code ec;
    fs::remove_all(event_dir, ec);
    if (ec) {
        s3_log->warn("Failed to remove event directory {}: {}", event_dir, ec.message());
    }

    s3_log->info("Event {} uploaded and cleaned up", event_id);
    return true;
}

// ============================================================
// S3Uploader::upload_file — upload single file with SigV4
// ============================================================

bool S3Uploader::upload_file(const std::string& local_path,
                             const std::string& s3_key,
                             const std::string& content_type) {
    auto s3_log = spdlog::get("s3");
    if (!s3_log) s3_log = spdlog::default_logger();

    // Read file content
    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs.is_open()) {
        s3_log->error("Cannot open file: {}", local_path);
        return false;
    }
    std::vector<uint8_t> file_data(
        (std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());
    ifs.close();

    // Compute payload hash
    std::string payload_hash = sha256_hex(file_data.data(), file_data.size());

    // Exponential backoff retry
    int delay_sec = 1;
    for (int retry = 0; retry <= config_.max_retries; ++retry) {
        // Check stop signal
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_) return false;
        }

        // Get STS credentials
        auto creds = credential_provider_->get_credentials();
        if (!creds || credential_provider_->is_expired()) {
            s3_log->warn("Credentials expired, skipping upload of {}", s3_key);
            return false;
        }

        // Build timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        gmtime_r(&time_t_now, &tm_buf);

        char timestamp[17];  // YYYYMMDDTHHMMSSZ
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%dT%H%M%SZ", &tm_buf);

        char date_buf[9];  // YYYYMMDD
        std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", &tm_buf);
        std::string date_str(date_buf);
        std::string timestamp_str(timestamp);

        // Build host and URI
        std::string host = config_.bucket + ".s3." + config_.region + ".amazonaws.com";
        std::string uri_path = "/" + s3_key;

        // Build scope
        std::string scope = date_str + "/" + config_.region + "/s3/aws4_request";

        // Build canonical headers (alphabetically sorted)
        std::string canonical_headers =
            "content-type:" + content_type + "\n" +
            "host:" + host + "\n" +
            "x-amz-content-sha256:" + payload_hash + "\n" +
            "x-amz-date:" + timestamp_str + "\n" +
            "x-amz-security-token:" + creds->session_token + "\n";

        std::string signed_headers =
            "content-type;host;x-amz-content-sha256;x-amz-date;x-amz-security-token";

        // Build canonical request
        std::string canonical_request = build_canonical_request(
            "PUT", uri_path, canonical_headers, signed_headers, payload_hash);

        // Hash canonical request
        std::string canonical_request_hash = sha256_hex(canonical_request);

        // Build string-to-sign
        std::string string_to_sign = build_string_to_sign(
            timestamp_str, scope, canonical_request_hash);

        // Derive signing key
        auto signing_key = derive_signing_key(
            creds->secret_access_key, date_str, config_.region, "s3");

        // Compute signature
        auto sig_bytes = hmac_sha256(signing_key, string_to_sign);
        std::string signature = to_hex(sig_bytes);

        // Build Authorization header
        std::string auth_header = build_authorization_header(
            creds->access_key_id, scope, signed_headers, signature);

        // Build URL
        std::string url = "https://" + host + uri_path;

        // Build headers list
        std::vector<std::string> headers = {
            "Host: " + host,
            "Content-Type: " + content_type,
            "x-amz-date: " + timestamp_str,
            "x-amz-content-sha256: " + payload_hash,
            "x-amz-security-token: " + creds->session_token,
            "Authorization: " + auth_header
        };

        // Attempt upload
        if (put_fn_(url, file_data, headers)) {
            return true;
        }

        // Don't wait after last retry
        if (retry < config_.max_retries) {
            s3_log->warn("Upload attempt {} failed for {}, retrying in {}s",
                         retry + 1, s3_key, delay_sec);
            std::unique_lock<std::mutex> lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::seconds(delay_sec),
                             [this] { return stop_requested_; })) {
                return false;  // stop requested
            }
            delay_sec = std::min(delay_sec * 2, 60);
        }
    }

    return false;
}
