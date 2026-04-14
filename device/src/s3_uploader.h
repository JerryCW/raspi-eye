// s3_uploader.h
// S3 snapshot uploader with AWS SigV4 signing.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "credential_provider.h"

// ============================================================
// S3Config POD (will move to config_manager.h in task 3.1)
// ============================================================

struct S3Config {
    std::string bucket;
    std::string region;
    int scan_interval_sec = 30;
    int max_retries = 3;
};

// ============================================================
// SigV4 pure functions (non-member, independently testable)
// ============================================================

// Byte array to lowercase hex string
std::string to_hex(const std::vector<uint8_t>& bytes);
std::string to_hex(const uint8_t* data, size_t len);

// SHA256 hash, returns 64-char lowercase hex string (OpenSSL EVP API)
std::string sha256_hex(const std::string& data);
std::string sha256_hex(const uint8_t* data, size_t len);

// HMAC-SHA256, returns raw 32 bytes (OpenSSL HMAC API)
std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                  const std::string& data);
std::vector<uint8_t> hmac_sha256(const std::string& key,
                                  const std::string& data);

// URI encode (S3 path: '/' not encoded by default)
std::string uri_encode(const std::string& input, bool encode_slash = true);

// Build canonical request (6 parts separated by \n)
// PUT\n/{uri_path}\n\n{canonical_headers}\n{signed_headers}\n{payload_hash}
std::string build_canonical_request(
    const std::string& method,
    const std::string& uri_path,
    const std::string& canonical_headers,
    const std::string& signed_headers,
    const std::string& payload_hash);

// Build string-to-sign (4 lines)
std::string build_string_to_sign(
    const std::string& timestamp,
    const std::string& scope,
    const std::string& canonical_request_hash);

// Derive signing key via 4-step HMAC chain
std::vector<uint8_t> derive_signing_key(
    const std::string& secret_key,
    const std::string& date,
    const std::string& region,
    const std::string& service);

// Build Authorization header value
std::string build_authorization_header(
    const std::string& access_key,
    const std::string& scope,
    const std::string& signed_headers,
    const std::string& signature);

// S3 key builder (pure function)
// Returns "{device_id}/{date}/{event_id}/{filename}"
// Each field must match [a-zA-Z0-9._-], otherwise returns empty string
std::string build_s3_key(
    const std::string& device_id,
    const std::string& date,
    const std::string& event_id,
    const std::string& filename);

// ============================================================
// Event scanning (free function, testable)
// ============================================================

// Scan snapshot_dir for closed events ready to upload.
// Directories with .uploaded marker are deleted directly.
// Returns paths of event directories with status=="closed" and end_time present.
std::vector<std::string> scan_closed_events(const std::string& snapshot_dir);

// ============================================================
// S3PutFunction type alias (injectable for testing)
// ============================================================

using S3PutFunction = std::function<bool(
    const std::string& url,
    const std::vector<uint8_t>& data,
    const std::vector<std::string>& headers)>;

// ============================================================
// S3Uploader class
// ============================================================

class S3Uploader {
public:
    static std::unique_ptr<S3Uploader> create(
        const S3Config& config,
        const std::string& snapshot_dir,
        const std::string& device_id,
        std::shared_ptr<CredentialProvider> credential_provider,
        std::string* error_msg = nullptr);

    ~S3Uploader();

    S3Uploader(const S3Uploader&) = delete;
    S3Uploader& operator=(const S3Uploader&) = delete;

    bool start(std::string* error_msg = nullptr);
    void stop();
    void set_put_function(S3PutFunction fn);

    // Event-driven upload notification.
    // Thread-safe, can be called from any thread.
    // Wakes up the scan thread via cv_.notify_one() without acquiring mutex_.
    // Safe to call after stop() (notify has no receiver, silently ignored).
    void notify_upload();

private:
    S3Uploader(const S3Config& config,
               const std::string& snapshot_dir,
               const std::string& device_id,
               std::shared_ptr<CredentialProvider> credential_provider);

    void scan_loop();
    bool upload_event(const std::string& event_dir);
    bool upload_file(const std::string& local_path,
                     const std::string& s3_key,
                     const std::string& content_type);

    S3Config config_;
    std::string snapshot_dir_;
    std::string device_id_;
    std::shared_ptr<CredentialProvider> credential_provider_;
    S3PutFunction put_fn_;

    std::thread scan_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_requested_ = false;
    bool upload_notified_ = false;  // event-driven upload trigger
};
