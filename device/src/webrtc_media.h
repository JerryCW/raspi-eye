// webrtc_media.h
// WebRTC media manager: PeerConnection lifecycle + H.264 frame broadcast (pImpl).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <gst/gst.h>

// Extract codec names from SDP text (a=rtpmap: lines).
// Returns comma-separated codec list, e.g. "H264, opus".
// Returns empty string if input is empty or contains no rtpmap lines.
std::string extract_sdp_summary(const std::string& sdp);

class WebRtcSignaling;  // Forward declaration to avoid header dependency

class WebRtcMediaManager {
public:
    static std::unique_ptr<WebRtcMediaManager> create(
        WebRtcSignaling& signaling,
        const std::string& aws_region = "",
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

    // 设置 GStreamer pipeline 引用（用于 force-keyunit 事件，不拥有，不 unref）
    void set_pipeline(GstElement* pipeline);

    // 配置仅关键帧模式的 writeFrame 连续失败阈值
    void set_writeframe_fail_threshold(int threshold);

    ~WebRtcMediaManager();
    WebRtcMediaManager(const WebRtcMediaManager&) = delete;
    WebRtcMediaManager& operator=(const WebRtcMediaManager&) = delete;

private:
    WebRtcMediaManager();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
