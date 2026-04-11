// webrtc_media.h
// WebRTC media manager: PeerConnection lifecycle + H.264 frame broadcast (pImpl).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class WebRtcSignaling;  // Forward declaration to avoid header dependency

class WebRtcMediaManager {
public:
    static std::unique_ptr<WebRtcMediaManager> create(
        WebRtcSignaling& signaling,
        std::string* error_msg = nullptr);

    bool on_viewer_offer(const std::string& peer_id,
                         const std::string& sdp_offer,
                         std::string* error_msg = nullptr);

    bool on_viewer_ice_candidate(const std::string& peer_id,
                                 const std::string& candidate,
                                 std::string* error_msg = nullptr);

    void remove_peer(const std::string& peer_id);

    void broadcast_frame(const uint8_t* data, size_t size,
                         uint64_t timestamp_100ns, bool is_keyframe);

    size_t peer_count() const;

    ~WebRtcMediaManager();
    WebRtcMediaManager(const WebRtcMediaManager&) = delete;
    WebRtcMediaManager& operator=(const WebRtcMediaManager&) = delete;

private:
    WebRtcMediaManager();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
