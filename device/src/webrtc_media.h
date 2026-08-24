// webrtc_media.h
// WebRTC media manager: PeerSession lifecycle + H.264 frame broadcast (pImpl).
// Spec 33 Task 4: one shared runtime for both platforms behind the PeerSdkOps
// seam (ProductionPeerSdkOps wraps the KVS WebRTC C SDK on Linux;
// StubPeerSdkOps elsewhere). Fixed PeerCallbackBridge slots (decision B) +
// HandlePermit pool (16) + single Reaper worker replace the legacy parallel
// stub state machine and cleanup thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gst/gst.h>

#include "webrtc_signaling.h"  // webrtc::internal seam: SdkCallResult, RuntimeClock, RuntimeOptions

// Extract codec names from SDP text (a=rtpmap: lines).
// Returns comma-separated codec list, e.g. "H264, opus".
// Returns empty string if input is empty or contains no rtpmap lines.
std::string extract_sdp_summary(const std::string& sdp);

// ============================================================
// Spec 33 internal seam for the peer runtime (design Component 1).
// The internal interface never exposes SDK handles, credentials or paths.
// ============================================================
namespace webrtc {
namespace internal {

// Peer connection states surfaced through PeerCallbacks::on_state
// (adapter-normalized; the production adapter maps RTC_PEER_CONNECTION_STATE).
enum PeerStateKind : int {
    kPeerStateConnected = 1,
    kPeerStateFailed = 2,
    kPeerStateClosed = 3,
    kPeerStateOther = 99,
};

// Opaque peer handle: produced by PeerSdkOps::create, owned by the runtime,
// consumed by PeerSdkOps::release. The runtime never sees SDK types.
class PeerHandle {
public:
    virtual ~PeerHandle() = default;
};

// Callbacks from the adapter into the runtime's fixed bridge slot.
// Implementations only update session atomics or submit retirement to the
// Reaper; they never call Peer SDK APIs and never wait on business locks
// (Task 1: onNewIceLocalCandidate fires while the SDK holds
// peerConnectionObjLock — blocking inside it blocks the SDK).
struct PeerCallbacks {
    std::function<void(uint64_t generation, int state)> on_state;
    std::function<void(uint64_t generation, std::string_view candidate)> on_local_ice;
};

// Staged peer SDK operations. Each call is synchronous with a finite
// confirmed upper bound (Task 1). None of these may be invoked while the
// peer map mutex is held.
class PeerSdkOps {
public:
    virtual ~PeerSdkOps() = default;
    // Creates the peer connection and registers callbacks before returning.
    // Callbacks may fire synchronously DURING this call (Task 1 confirmed).
    virtual SdkCallResult create(uint64_t generation, const PeerCallbacks& cbs,
                                 std::unique_ptr<PeerHandle>& out) = 0;
    // Consumes the remote offer, produces the serialized local answer.
    // An empty answer means "nothing to send via signaling" (stub adapter).
    virtual SdkCallResult negotiate(PeerHandle& handle, std::string_view sdp_offer,
                                    std::string& answer) = 0;
    virtual SdkCallResult add_ice(PeerHandle& handle, std::string_view candidate) = 0;
    // frameData is consumed synchronously (Task 1): the buffer may be
    // released as soon as the call returns. RETRYABLE = transient skip
    // (SRTP not ready), FATAL = counted write failure.
    virtual SdkCallResult write_frame(PeerHandle& handle, const uint8_t* data,
                                      size_t size, uint64_t timestamp_100ns,
                                      bool keyframe) = 0;
    virtual SdkCallResult close(PeerHandle& handle) = 0;
    virtual SdkCallResult release(std::unique_ptr<PeerHandle> handle) = 0;
};

}  // namespace internal
}  // namespace webrtc

class WebRtcMediaManager {
public:
    static std::unique_ptr<WebRtcMediaManager> create(
        WebRtcSignaling& signaling,
        const std::string& aws_region = "",
        std::string* error_msg = nullptr);

    // Test factory: same runtime with injected ops/clock/options.
    static std::unique_ptr<WebRtcMediaManager> create_for_test(
        WebRtcSignaling& signaling,
        const std::string& region,
        std::shared_ptr<webrtc::internal::PeerSdkOps> ops,
        std::shared_ptr<webrtc::internal::RuntimeClock> clock,
        const webrtc::internal::RuntimeOptions& options,
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

    // Health snapshot (spec-33 req 6.1/6.4) — immutable copy, no credentials,
    // no SDP, no ICE payloads. CONNECTED signaling + 0 viewers is healthy;
    // nothing here feeds a recovery decision.
    struct HealthSnapshot {
        size_t active_peers = 0;        // map sessions not DISCONNECTING
        size_t connected_peers = 0;     // subset in CONNECTED
        size_t live_handles = 0;        // permits in use (active+creating+retired)
        size_t reaper_queue_depth = 0;  // retirement jobs pending
        size_t pending_ice_peers = 0;   // peers with buffered early ICE
        size_t pending_ice_depth = 0;   // total buffered candidates
        int64_t oldest_session_age_sec = -1;  // age of the oldest live session
        uint64_t stale_callbacks = 0;   // late/mismatched-generation callbacks
        uint64_t callback_exceptions = 0;
        uint64_t local_ice_posted = 0;  // fire-and-forget posts accepted
        uint64_t local_ice_dropped_oversized = 0;  // > 4KiB local candidates
        uint64_t local_ice_rejected = 0;    // queue full / signaling not connected
        uint64_t pending_ice_expired = 0;   // TTL-expired buffered candidates
        uint64_t pending_ice_evicted = 0;   // LRU/FIFO-evicted buffered candidates
        uint64_t pending_ice_rejected_oversized = 0;  // > 4KiB viewer candidates
        uint64_t connecting_timeouts = 0;   // CONNECTING > 30s transitions
        uint64_t reaped_total = 0;          // sessions retired by the Reaper
        std::vector<std::pair<std::string, uint64_t>> reap_reasons;  // reason -> count
    };
    HealthSnapshot health_snapshot() const;

    // Log current media health status (for periodic health check).
    void log_health_status() const;

    ~WebRtcMediaManager();
    WebRtcMediaManager(const WebRtcMediaManager&) = delete;
    WebRtcMediaManager& operator=(const WebRtcMediaManager&) = delete;

private:
    WebRtcMediaManager();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
