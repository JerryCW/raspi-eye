// webrtc_signaling.h
// KVS WebRTC signaling client with platform-conditional compilation (pImpl).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "credential_provider.h"  // AwsConfig, parse_toml_section

// WebRTC signaling configuration (parsed from TOML [webrtc] section)
struct WebRtcConfig {
    std::string channel_name;
    std::string aws_region;
    bool enabled = true;       // WebRTC 分支是否启用
};

// Build WebRtcConfig from TOML key-value map.
// Returns false with error_msg containing missing field names on failure.
bool build_webrtc_config(
    const std::unordered_map<std::string, std::string>& kv,
    WebRtcConfig& config,
    std::string* error_msg = nullptr);

// KVS WebRTC signaling client.
// - With HAVE_KVS_WEBRTC_SDK: real SignalingClient via KVS WebRTC C SDK
// - Without: stub implementation
class WebRtcSignaling {
public:
    using OfferCallback = std::function<void(const std::string& peer_id, const std::string& sdp)>;
    using IceCandidateCallback = std::function<void(const std::string& peer_id, const std::string& candidate)>;

    // Factory: create instance based on platform / SDK availability
    static std::unique_ptr<WebRtcSignaling> create(
        const WebRtcConfig& config,
        const AwsConfig& aws_config,
        std::string* error_msg = nullptr);

    // Connect to signaling channel (Master role)
    bool connect(std::string* error_msg = nullptr);

    // Disconnect from signaling channel
    void disconnect();

    // Query connection state
    bool is_connected() const;

    // Reconnect (release old client, create new one)
    bool reconnect(std::string* error_msg = nullptr);

    // Register SDP Offer received callback
    void set_offer_callback(OfferCallback cb);

    // Register ICE Candidate received callback
    void set_ice_candidate_callback(IceCandidateCallback cb);

    // Send SDP Answer to a specific Viewer
    bool send_answer(const std::string& peer_id, const std::string& sdp_answer);

    // Send ICE Candidate to a specific Viewer
    bool send_ice_candidate(const std::string& peer_id, const std::string& candidate);

    // ICE server info returned by get_ice_config()
    struct IceServerInfo {
        std::string uri;
        std::string username;
        std::string credential;
    };

    // Get ICE config count (TURN server count from signaling channel)
    // stub returns 0
    uint32_t get_ice_config_count() const;

    // Get ICE server info for the i-th config entry
    // Populates servers vector with URI/username/credential
    // stub returns false
    bool get_ice_config(uint32_t index,
                        std::vector<IceServerInfo>& servers) const;

    // Log current signaling health status (for periodic health check)
    void log_health_status() const;

    ~WebRtcSignaling();
    WebRtcSignaling(const WebRtcSignaling&) = delete;
    WebRtcSignaling& operator=(const WebRtcSignaling&) = delete;

private:
    WebRtcSignaling();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
