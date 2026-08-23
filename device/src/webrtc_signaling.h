// webrtc_signaling.h
// KVS WebRTC signaling client with platform-conditional compilation (pImpl).
// Spec 33: single SignalingOwner worker + bounded command queue + generation-based
// recreate with deadline (decision A3: coexist with the SDK's non-disableable
// single-shot internal reconnect; errorReportFn bridge + liveness detection).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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

// ============================================================
// Spec 33 internal seam: SDK ops / clock / runtime options.
// Production adapter wraps the real KVS WebRTC C SDK (Linux) or a
// stub (macOS); tests inject fake ops and a manual clock.
// The internal interface never exposes SDK handles, credentials or paths.
// ============================================================
namespace webrtc {
namespace internal {

enum class WaitResult { NOTIFIED, DEADLINE };
enum class SdkCallStatus { OK, RETRYABLE, FATAL };

struct SdkCallResult {
    SdkCallStatus status = SdkCallStatus::OK;
    uint32_t code = 0;
};

// Runtime tuning knobs. Defaults follow spec-33 Constraints; all SDK-derived
// upper bounds were confirmed from v1.18.0 sources (design.md SDK Semantic Gates).
struct RuntimeOptions {
    size_t command_capacity = 256;
    size_t message_capacity = 512;
    size_t handle_permits = 16;
    std::chrono::seconds connected_deadline{30};    // connect API chain -> CONNECTED callback
    std::chrono::seconds stable_connection{30};     // reset backoff after this stable time
    std::chrono::seconds caller_wait_timeout{2};    // caller-facing wait (QUERY completion deadline)
    std::chrono::seconds send_completion_deadline{10};  // SEND commands may finish late up to this
    std::chrono::seconds sdk_reconnect_grace{20};   // A3: wait for SDK single-shot reconnect (15s bound)
    std::chrono::seconds liveness_timeout{10800};   // A3: 3h without any callback signal => half-open
    // Confirmed worst-case durations for admission checks (v1.18.0, Linux).
    std::chrono::seconds send_max_duration{5};      // SIGNALING_SEND_TIMEOUT (non-Windows)
    std::chrono::seconds query_max_duration{1};     // in-memory ICE config read
    // connect()/reconnect() caller wait for one staged attempt result:
    // create 10s + fetch 7s (HTTP 2s conn + 5s completion) + connect 15s + margin.
    std::chrono::seconds connect_attempt_wait{40};
    std::vector<int> backoff_schedule_sec{1, 2, 4, 8, 16, 30};  // saturating
};

struct IceServerRecord {
    std::string uri;
    std::string username;
    std::string credential;
    uint32_t group = 0;  // TURN server group index (public get_ice_config keeps per-group semantics)
};

// Shared KVS runtime lifecycle token (design Component 5, req 8.3).
// Process-level refcount: the first live token runs the init hook
// (initKvsWebRtc() on Linux with SDK, no-op stub otherwise); releasing the
// last token runs the deinit hook. Signaling and media adapters each hold
// one token so client recreate never re-inits the global runtime.
class KvsRuntimeToken {
public:
    // Init hook returns false on failure and reports a code; deinit never fails.
    using InitHook = std::function<bool(uint32_t& code)>;
    using DeinitHook = std::function<void()>;

    // Acquire a token (thread-safe). Returns nullptr when the init hook fails
    // for the first token; the failure code is stored in *error_code.
    static std::shared_ptr<KvsRuntimeToken> acquire(uint32_t* error_code = nullptr);

    // Inject test hooks (pass empty functions to restore platform defaults).
    // Must only be called while no token is alive.
    static void set_hooks_for_test(InitHook init_hook, DeinitHook deinit_hook);

    ~KvsRuntimeToken();
    KvsRuntimeToken(const KvsRuntimeToken&) = delete;
    KvsRuntimeToken& operator=(const KvsRuntimeToken&) = delete;

private:
    KvsRuntimeToken() = default;
};

// Injectable clock. wait_until must honor manual-clock advancement in tests.
class RuntimeClock {
public:
    virtual ~RuntimeClock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
    virtual WaitResult wait_until(
        std::condition_variable& cv,
        std::unique_lock<std::mutex>& lock,
        std::chrono::steady_clock::time_point deadline,
        const std::function<bool()>& notified) = 0;
};

// Callbacks from the adapter into the runtime. Implementations only publish
// events (ControlMailbox / message queue) and return; no business work.
struct SignalingCallbacks {
    std::function<void(uint64_t generation, int state,
                       std::chrono::steady_clock::time_point observed_at)> on_state;
    std::function<void(uint64_t generation, int type,
                       std::string_view peer, std::string_view payload)> on_message;
    // SDK errorReportFn bridge (decision A3): RECONNECT_FAILED etc. publish only.
    std::function<void(uint64_t generation, uint32_t status_code,
                       std::chrono::steady_clock::time_point observed_at)> on_error;
};

// Message types surfaced through on_message (mirrors SDK values used).
enum SignalingMessageKind : int {
    kSignalingMsgOffer = 1,
    kSignalingMsgIceCandidate = 2,
    kSignalingMsgOther = 99,
};

// Signaling states surfaced through on_state (adapter-normalized).
enum SignalingStateKind : int {
    kSignalingStateConnected = 1,
    kSignalingStateDisconnected = 2,
    kSignalingStateOther = 99,
};

// Error kinds surfaced through on_error (adapter-normalized; the adapter maps
// SDK status codes such as STATUS_SIGNALING_RECONNECT_FAILED, decision A3).
enum SignalingErrorKind : uint32_t {
    kSignalingErrReconnectFailed = 1,
    kSignalingErrOther = 99,
};

// Staged SDK operations. Each call is synchronous with a finite confirmed
// upper bound (Task 1). create() registers callbacks before returning.
class SignalingSdkOps {
public:
    virtual ~SignalingSdkOps() = default;
    // Factory-time eager validation (before the owner starts). The production
    // adapter creates the IoT credential provider here so invalid credentials
    // fail WebRtcSignaling::create() fast (legacy behavior preserved for the
    // Pi 5 test skip path). Default: no-op success.
    virtual SdkCallResult preflight() { return {}; }
    virtual SdkCallResult create(uint64_t generation, const SignalingCallbacks& cbs) = 0;
    virtual SdkCallResult fetch() = 0;
    virtual SdkCallResult connect() = 0;
    virtual SdkCallResult send_answer(std::string_view peer, std::string_view payload) = 0;
    virtual SdkCallResult send_ice(std::string_view peer, std::string_view payload) = 0;
    virtual SdkCallResult query_ice(std::vector<IceServerRecord>& out) = 0;
    virtual SdkCallResult release() = 0;
};

}  // namespace internal
}  // namespace webrtc

// ============================================================
// KVS WebRTC signaling client.
// - With HAVE_KVS_WEBRTC_SDK: real SignalingClient via KVS WebRTC C SDK
// - Without: stub adapter on the same runtime (no parallel state machine)
// ============================================================
class WebRtcSignaling {
public:
    using OfferCallback = std::function<void(const std::string& peer_id, const std::string& sdp)>;
    using IceCandidateCallback = std::function<void(const std::string& peer_id, const std::string& candidate)>;

    // Factory: create instance based on platform / SDK availability
    static std::unique_ptr<WebRtcSignaling> create(
        const WebRtcConfig& config,
        const AwsConfig& aws_config,
        std::string* error_msg = nullptr);

    // Test factory: same runtime with injected ops/clock/options.
    static std::unique_ptr<WebRtcSignaling> create_for_test(
        const WebRtcConfig& config,
        const AwsConfig& aws_config,
        std::shared_ptr<webrtc::internal::SignalingSdkOps> ops,
        std::shared_ptr<webrtc::internal::RuntimeClock> clock,
        const webrtc::internal::RuntimeOptions& options,
        std::string* error_msg = nullptr);

    // Connect to signaling channel (Master role).
    // true = the staged create/fetch/connect API chain was accepted and returned
    // successfully once; it does NOT imply is_connected()==true (that is set only
    // by the CONNECTED callback of the current generation). On failure the owner
    // keeps recovering with saturating backoff (never gives up until disconnect()).
    bool connect(std::string* error_msg = nullptr);

    // Disconnect from signaling channel (full shutdown of the owner worker).
    void disconnect();

    // Query connection state (CONNECTED callback observed for current generation).
    bool is_connected() const;

    // Force a full recreate (release old client, create new generation).
    // true = request accepted (owner running or started).
    bool reconnect(std::string* error_msg = nullptr);

    // Register SDP Offer received callback
    void set_offer_callback(OfferCallback cb);

    // Register ICE Candidate received callback
    void set_ice_candidate_callback(IceCandidateCallback cb);

    // Send SDP Answer to a specific Viewer (SEND command: caller waits <=2s,
    // command may still complete within the 10s completion deadline).
    bool send_answer(const std::string& peer_id, const std::string& sdp_answer);

    // Send ICE Candidate to a specific Viewer (same SEND semantics).
    bool send_ice_candidate(const std::string& peer_id, const std::string& candidate);

    // Fire-and-forget ICE post for SDK-callback contexts (spec-33 req 5.4):
    // enqueue only, never waits for a reply. Returns false if not enqueued.
    bool try_post_ice_candidate(const std::string& peer_id, const std::string& candidate);

    // ICE server info returned by get_ice_config()
    struct IceServerInfo {
        std::string uri;
        std::string username;
        std::string credential;
    };

    // Get ICE config count (TURN server group count; cached at CONNECTED)
    uint32_t get_ice_config_count() const;

    // Get ICE server info for the given TURN server group (fast read of the
    // cache refreshed by the owner at CONNECTED; false when no cache or the
    // index is out of range). Records with IceServerRecord.group == index
    // are returned as one group.
    bool get_ice_config(uint32_t index,
                        std::vector<IceServerInfo>& servers) const;

    // Health snapshot (spec-33 req 6.1) — immutable copy, no credentials/SDP.
    struct HealthSnapshot {
        std::string state;
        uint64_t generation = 0;
        uint32_t attempt = 0;
        int64_t connected_age_sec = -1;      // -1 when not connected
        int64_t last_signal_age_sec = -1;    // seconds since any callback signal
        size_t command_queue_depth = 0;
        uint64_t stale_events = 0;
        uint64_t commands_expired = 0;
        uint64_t commands_rejected = 0;
        size_t message_queue_depth = 0;     // MessageDispatcher backlog (Task 3)
        uint64_t messages_dropped = 0;      // overflow/eviction/shutdown drops
        uint64_t recreate_count = 0;
        uint64_t total_disconnects = 0;
        uint64_t callback_exceptions = 0;
    };
    HealthSnapshot health_snapshot() const;

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
