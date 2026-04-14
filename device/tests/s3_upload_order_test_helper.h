// s3_upload_order_test_helper.h
// Shared helpers for S3 upload order tests (example-based + PBT).
#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "credential_provider.h"
#include "s3_uploader.h"

namespace fs = std::filesystem;

// ============================================================
// MockHttpClient for creating a valid CredentialProvider
// ============================================================

class S3TestHttpClient : public HttpClient {
public:
    HttpResponse get(
        const std::string& /*url*/,
        const std::unordered_map<std::string, std::string>& /*headers*/,
        const TlsConfig& /*tls*/) override {
        HttpResponse resp;
        resp.status_code = 200;
        resp.body = R"({"credentials":{
            "accessKeyId":"ASIAQEXAMPLE",
            "secretAccessKey":"wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
            "sessionToken":"FwoGZXIvYXdzEBYaDHqa0AP",
            "expiration":"2099-12-31T23:59:59Z"}})";
        return resp;
    }
};

// ============================================================
// Helper: create temp PEM files + TOML config for CredentialProvider
// ============================================================

struct S3TestEnv {
    fs::path tmp_dir;
    std::string config_path;
    std::string cert_path;
    std::string key_path;
    std::string ca_path;

    static S3TestEnv create(const std::string& prefix) {
        S3TestEnv env;
        env.tmp_dir = fs::temp_directory_path() / (prefix + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(env.tmp_dir);

        // Write PEM files
        env.cert_path = (env.tmp_dir / "cert.pem").string();
        env.key_path = (env.tmp_dir / "key.pem").string();
        env.ca_path = (env.tmp_dir / "ca.pem").string();

        {
            std::ofstream f(env.cert_path);
            f << "-----BEGIN CERTIFICATE-----\nTESTDATA\n-----END CERTIFICATE-----\n";
        }
        {
            std::ofstream f(env.key_path);
            f << "-----BEGIN PRIVATE KEY-----\nTESTDATA\n-----END PRIVATE KEY-----\n";
        }
        chmod(env.key_path.c_str(), 0600);
        {
            std::ofstream f(env.ca_path);
            f << "-----BEGIN CERTIFICATE-----\nTESTDATA\n-----END CERTIFICATE-----\n";
        }

        // Write TOML config
        env.config_path = (env.tmp_dir / "config.toml").string();
        {
            std::ofstream f(env.config_path);
            f << "[aws]\n"
              << "thing_name = \"TestThing\"\n"
              << "credential_endpoint = \"c19imbvbcm8u20.credentials.iot.ap-southeast-1.amazonaws.com\"\n"
              << "role_alias = \"TestRoleAlias\"\n"
              << "cert_path = \"" << env.cert_path << "\"\n"
              << "key_path = \"" << env.key_path << "\"\n"
              << "ca_path = \"" << env.ca_path << "\"\n";
        }

        return env;
    }

    void cleanup() {
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
    }
};

// ============================================================
// Helper: create a closed event directory with specified files
// ============================================================

inline fs::path create_event_dir(
    const fs::path& snapshot_dir,
    const std::string& event_name,
    const std::vector<std::string>& jpg_files) {

    auto event_dir = snapshot_dir / event_name;
    fs::create_directories(event_dir);

    // Write jpg files (1 byte content each)
    for (const auto& jpg : jpg_files) {
        std::ofstream f(event_dir / jpg, std::ios::binary);
        f << '\xFF';  // minimal JPEG-like content
    }

    // Write event.json
    nlohmann::json j;
    j["event_id"] = event_name;
    j["device_id"] = "test_device";
    j["start_time"] = "2026-04-12T15:30:45Z";
    j["end_time"] = "2026-04-12T15:31:15Z";
    j["status"] = "closed";
    j["frame_count"] = static_cast<int>(jpg_files.size());

    std::ofstream ofs(event_dir / "event.json");
    ofs << j.dump();

    return event_dir;
}

// ============================================================
// Thread-safe upload recorder for mock put function
// ============================================================

struct UploadRecorder {
    std::mutex mu;
    std::vector<std::string> uploaded_keys;
    int fail_at_index = -1;  // -1 = no failure

    bool put(const std::string& url,
             const std::vector<uint8_t>& /*data*/,
             const std::vector<std::string>& /*headers*/) {
        // Extract the S3 key from the URL (after the host)
        // URL format: https://bucket.s3.region.amazonaws.com/device/date/event/filename
        auto pos = url.find(".amazonaws.com/");
        std::string key;
        if (pos != std::string::npos) {
            key = url.substr(pos + 15);  // skip ".amazonaws.com/"
        } else {
            key = url;
        }

        std::lock_guard<std::mutex> lock(mu);
        int current_index = static_cast<int>(uploaded_keys.size());
        if (fail_at_index >= 0 && current_index == fail_at_index) {
            return false;
        }
        uploaded_keys.push_back(key);
        return true;
    }

    // Extract just the filename from recorded keys
    std::vector<std::string> filenames() {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<std::string> result;
        for (const auto& key : uploaded_keys) {
            auto pos = key.rfind('/');
            if (pos != std::string::npos) {
                result.push_back(key.substr(pos + 1));
            } else {
                result.push_back(key);
            }
        }
        return result;
    }
};

