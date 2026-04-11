// webrtc_media.cpp
// WebRTC media manager: PeerConnection lifecycle + H.264 frame broadcast (pImpl).
// Conditional compilation: stub (no SDK) / real (KVS WebRTC C SDK).

#ifndef HAVE_KVS_WEBRTC_SDK

#include "webrtc_media.h"
#include "webrtc_signaling.h"
#include <spdlog/spdlog.h>
#include <mutex>
#include <unordered_set>

static constexpr size_t kMaxPeers = 10;
static constexpr size_t kMaxPeerIdLen = 256;

struct WebRtcMediaManager::Impl {
    WebRtcSignaling& signaling;
    std::unordered_set<std::string> peers;
    mutable std::mutex peers_mutex;

    explicit Impl(WebRtcSignaling& sig) : signaling(sig) {}
};

std::unique_ptr<WebRtcMediaManager> WebRtcMediaManager::create(
    WebRtcSignaling& signaling, const std::string& /*aws_region*/,
    std::string* /*error_msg*/) {
    auto obj = std::unique_ptr<WebRtcMediaManager>(new WebRtcMediaManager());
    obj->impl_ = std::make_unique<Impl>(signaling);
    auto logger = spdlog::get("pipeline");
    if (logger) logger->info("Created WebRtcMediaManager stub");
    return obj;
}

bool WebRtcMediaManager::on_viewer_offer(
    const std::string& peer_id, const std::string& /*sdp_offer*/,
    std::string* error_msg) {
    std::lock_guard<std::mutex> lock(impl_->peers_mutex);
    auto logger = spdlog::get("pipeline");

    if (peer_id.size() > kMaxPeerIdLen) {
        if (logger) logger->warn("Rejecting peer with oversized id ({} bytes, max {})",
                                 peer_id.size(), kMaxPeerIdLen);
        if (error_msg) *error_msg = "peer_id too long";
        return false;
    }

    impl_->peers.erase(peer_id);

    if (impl_->peers.size() >= kMaxPeers) {
        if (logger) logger->warn("Max peers ({}) reached, rejecting peer: {}",
                                 kMaxPeers, peer_id);
        if (error_msg) *error_msg = "Max peer count reached";
        return false;
    }
    impl_->peers.insert(peer_id);
    if (logger) logger->info("Stub: added peer {}, count={}", peer_id, impl_->peers.size());
    return true;
}

bool WebRtcMediaManager::on_viewer_ice_candidate(
    const std::string& /*peer_id*/, const std::string& /*candidate*/,
    std::string* /*error_msg*/) {
    return true;
}

void WebRtcMediaManager::remove_peer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(impl_->peers_mutex);
    auto erased = impl_->peers.erase(peer_id);
    auto logger = spdlog::get("pipeline");
    if (logger && erased) {
        logger->info("Stub: removed peer {}, count={}", peer_id, impl_->peers.size());
    }
}

void WebRtcMediaManager::broadcast_frame(
    const uint8_t* /*data*/, size_t /*size*/,
    uint64_t /*timestamp_100ns*/, bool /*is_keyframe*/) {
    // stub: no-op
}

size_t WebRtcMediaManager::peer_count() const {
    std::lock_guard<std::mutex> lock(impl_->peers_mutex);
    return impl_->peers.size();
}

WebRtcMediaManager::WebRtcMediaManager() = default;
WebRtcMediaManager::~WebRtcMediaManager() = default;

#endif  // !HAVE_KVS_WEBRTC_SDK


// ============================================================
// Real implementation: KVS WebRTC C SDK
// ============================================================

#ifdef HAVE_KVS_WEBRTC_SDK

#include "webrtc_media.h"
#include "webrtc_signaling.h"
#include <spdlog/spdlog.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
}

static constexpr size_t kMaxPeers = 10;
static constexpr size_t kMaxPeerIdLen = 256;
static constexpr uint32_t kMaxWriteFailures = 100;

struct PeerInfo {
    PRtcPeerConnection peer_connection = nullptr;
    PRtcRtpTransceiver video_transceiver = nullptr;
    uint32_t consecutive_write_failures = 0;
    bool connected = false;  // true after ICE connected
};

// CallbackContext and SDK callbacks are inside Impl to access private scope.

struct WebRtcMediaManager::Impl {
    // Context passed to SDK callbacks via UINT64 customData.
    struct CallbackContext {
        Impl* impl = nullptr;
        std::string peer_id;
    };
    WebRtcSignaling& signaling;
    std::string region;
    std::unordered_map<std::string, PeerInfo> peers;
    mutable std::mutex peers_mutex;

    // Prevent dangling CallbackContext pointers: store them so we can
    // invalidate on peer removal.  Each entry is heap-allocated because
    // the SDK stores a raw UINT64 cast of the pointer.
    std::unordered_map<std::string, CallbackContext*> callback_contexts;

    // Buffer ICE candidates that arrive before the SDP offer is processed.
    // Key: peer_id, Value: list of raw candidate JSON strings.
    static constexpr size_t kMaxPendingCandidates = 50;
    std::unordered_map<std::string, std::vector<std::string>> pending_candidates;

    explicit Impl(WebRtcSignaling& sig) : signaling(sig) {}

    // SDK callback: local ICE candidate generated.
    static VOID on_ice_candidate_handler(UINT64 custom_data, PCHAR candidate_json) {
        auto* ctx = reinterpret_cast<CallbackContext*>(custom_data);
        if (!ctx || !ctx->impl) return;
        if (candidate_json == NULL) return;

        auto logger = spdlog::get("pipeline");
        if (logger) logger->debug("Sending ICE candidate to peer: {}", ctx->peer_id);
        ctx->impl->signaling.send_ice_candidate(ctx->peer_id,
                                                 std::string(candidate_json));
    }

    // SDK callback: PeerConnection state changed.
    static VOID on_connection_state_change(UINT64 custom_data,
                                           RTC_PEER_CONNECTION_STATE new_state) {
        auto* ctx = reinterpret_cast<CallbackContext*>(custom_data);
        if (!ctx || !ctx->impl) return;

        auto logger = spdlog::get("pipeline");
        if (new_state == RTC_PEER_CONNECTION_STATE_CONNECTED) {
            std::lock_guard<std::mutex> lock(ctx->impl->peers_mutex);
            if (logger) logger->info("Peer {} connected", ctx->peer_id);
            auto it = ctx->impl->peers.find(ctx->peer_id);
            if (it != ctx->impl->peers.end()) {
                it->second.connected = true;
            }
            return;
        }
        if (new_state == RTC_PEER_CONNECTION_STATE_FAILED ||
            new_state == RTC_PEER_CONNECTION_STATE_CLOSED) {
            const char* reason = (new_state == RTC_PEER_CONNECTION_STATE_FAILED)
                                     ? "connection_failed"
                                     : "connection_closed";
            if (logger) logger->info("Peer {} state changed, removing: {}",
                                     ctx->peer_id, reason);
            std::lock_guard<std::mutex> lock(ctx->impl->peers_mutex);
            ctx->impl->remove_peer_locked(ctx->peer_id, reason);
        }
    }

    // Remove a peer while already holding the lock.
    void remove_peer_locked(const std::string& peer_id, const char* reason) {
        auto it = peers.find(peer_id);
        if (it == peers.end()) return;

        auto logger = spdlog::get("pipeline");
        if (it->second.peer_connection) {
            freePeerConnection(&it->second.peer_connection);
        }
        peers.erase(it);

        // Invalidate callback context so stale SDK callbacks become no-ops.
        auto ctx_it = callback_contexts.find(peer_id);
        if (ctx_it != callback_contexts.end()) {
            ctx_it->second->impl = nullptr;  // mark invalid
            delete ctx_it->second;
            callback_contexts.erase(ctx_it);
        }

        if (logger) {
            logger->info("Removed peer {}, reason={}, remaining={}",
                         peer_id, reason, peers.size());
        }

        // Also discard any buffered candidates for this peer.
        pending_candidates.erase(peer_id);
    }

    ~Impl() {
        std::lock_guard<std::mutex> lock(peers_mutex);
        auto logger = spdlog::get("pipeline");
        for (auto& [id, info] : peers) {
            if (info.peer_connection) {
                freePeerConnection(&info.peer_connection);
            }
        }
        peers.clear();
        // Clean up all callback contexts.
        for (auto& [id, ctx] : callback_contexts) {
            ctx->impl = nullptr;
            delete ctx;
        }
        callback_contexts.clear();
        pending_candidates.clear();
        if (logger) logger->info("WebRtcMediaManager destroyed, all peers released");
    }
};

static std::string status_to_hex(STATUS status) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08x", status);
    return std::string(buf);
}

// ---- Factory ----

std::unique_ptr<WebRtcMediaManager> WebRtcMediaManager::create(
    WebRtcSignaling& signaling, const std::string& aws_region,
    std::string* /*error_msg*/) {
    auto obj = std::unique_ptr<WebRtcMediaManager>(new WebRtcMediaManager());
    obj->impl_ = std::make_unique<Impl>(signaling);
    obj->impl_->region = aws_region;
    auto logger = spdlog::get("pipeline");
    if (logger) logger->info("Created WebRtcMediaManager (KVS WebRTC SDK)");
    return obj;
}

// ---- on_viewer_offer ----

bool WebRtcMediaManager::on_viewer_offer(
    const std::string& peer_id, const std::string& sdp_offer,
    std::string* error_msg) {
    std::lock_guard<std::mutex> lock(impl_->peers_mutex);
    auto logger = spdlog::get("pipeline");

    // 1. peer_id length check
    if (peer_id.size() > kMaxPeerIdLen) {
        if (logger) logger->warn("Rejecting peer with oversized id ({} bytes, max {})",
                                 peer_id.size(), kMaxPeerIdLen);
        if (error_msg) *error_msg = "peer_id too long";
        return false;
    }

    // 2. If peer_id already exists, free old PeerConnection
    auto existing = impl_->peers.find(peer_id);
    if (existing != impl_->peers.end()) {
        if (logger) logger->info("Replacing existing peer: {}", peer_id);
        if (existing->second.peer_connection) {
            freePeerConnection(&existing->second.peer_connection);
        }
        impl_->peers.erase(existing);
        // Invalidate old callback context
        auto ctx_it = impl_->callback_contexts.find(peer_id);
        if (ctx_it != impl_->callback_contexts.end()) {
            ctx_it->second->impl = nullptr;
            delete ctx_it->second;
            impl_->callback_contexts.erase(ctx_it);
        }
    }

    // 3. Max peers check
    if (impl_->peers.size() >= kMaxPeers) {
        if (logger) logger->warn("Max peers ({}) reached, rejecting peer: {}",
                                 kMaxPeers, peer_id);
        if (error_msg) *error_msg = "Max peer count reached";
        return false;
    }

    // 4. Configure RtcConfiguration (STUN + TURN servers)
    RtcConfiguration rtc_config;
    MEMSET(&rtc_config, 0x00, SIZEOF(RtcConfiguration));
    rtc_config.iceTransportPolicy = ICE_TRANSPORT_POLICY_ALL;

    // STUN server (slot 0)
    SNPRINTF(rtc_config.iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN,
             "stun:stun.kinesisvideo.%s.amazonaws.com:443",
             impl_->region.c_str());

    // TURN servers (slot 1+) — from signaling client
    uint32_t ice_count = impl_->signaling.get_ice_config_count();
    uint32_t uri_idx = 0;
    uint32_t max_turn = (ice_count > 0) ? 1 : 0;  // use only 1 TURN server
    for (uint32_t i = 0; i < max_turn; i++) {
        std::vector<WebRtcSignaling::IceServerInfo> servers;
        if (impl_->signaling.get_ice_config(i, servers)) {
            for (const auto& s : servers) {
                if (uri_idx + 1 < MAX_ICE_SERVERS_COUNT) {
                    STRNCPY(rtc_config.iceServers[uri_idx + 1].urls,
                             s.uri.c_str(), MAX_ICE_CONFIG_URI_LEN);
                    STRNCPY(rtc_config.iceServers[uri_idx + 1].credential,
                             s.credential.c_str(), MAX_ICE_CONFIG_CREDENTIAL_LEN);
                    STRNCPY(rtc_config.iceServers[uri_idx + 1].username,
                             s.username.c_str(), MAX_ICE_CONFIG_USER_NAME_LEN);
                    uri_idx++;
                }
            }
        }
    }

    // 5. Create PeerConnection
    PRtcPeerConnection peer_connection = NULL;
    STATUS ret = createPeerConnection(&rtc_config, &peer_connection);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("createPeerConnection failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "createPeerConnection failed: " + status_to_hex(ret);
        return false;
    }

    // 6. Register supported codecs (must be before addTransceiver)
    ret = addSupportedCodec(peer_connection,
        RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("addSupportedCodec (H.264) failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "addSupportedCodec (H.264) failed: " + status_to_hex(ret);
        freePeerConnection(&peer_connection);
        return false;
    }

    addSupportedCodec(peer_connection, RTC_CODEC_OPUS);  // non-fatal

    // 7. Add video transceiver (H.264, SENDRECV)
    RtcMediaStreamTrack video_track;
    MEMSET(&video_track, 0, SIZEOF(RtcMediaStreamTrack));
    video_track.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
    video_track.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    STRCPY(video_track.streamId, "myKvsVideoStream");
    STRCPY(video_track.trackId, "myVideoTrack");

    RtcRtpTransceiverInit video_init;
    MEMSET(&video_init, 0, SIZEOF(RtcRtpTransceiverInit));
    video_init.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;

    PRtcRtpTransceiver video_transceiver = NULL;
    ret = addTransceiver(peer_connection, &video_track, &video_init, &video_transceiver);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("addTransceiver (video) failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "addTransceiver (video) failed: " + status_to_hex(ret);
        freePeerConnection(&peer_connection);
        return false;
    }

    // 7b. Add audio transceiver (SENDRECV)
    RtcMediaStreamTrack audio_track;
    MEMSET(&audio_track, 0, SIZEOF(RtcMediaStreamTrack));
    audio_track.kind = MEDIA_STREAM_TRACK_KIND_AUDIO;
    audio_track.codec = RTC_CODEC_OPUS;
    STRCPY(audio_track.streamId, "myKvsVideoStream");
    STRCPY(audio_track.trackId, "myAudioTrack");

    RtcRtpTransceiverInit audio_init;
    MEMSET(&audio_init, 0, SIZEOF(RtcRtpTransceiverInit));
    audio_init.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;

    PRtcRtpTransceiver audio_transceiver = NULL;
    ret = addTransceiver(peer_connection, &audio_track, &audio_init, &audio_transceiver);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->warn("addTransceiver (audio) failed for peer {}, status: {} — continuing without audio",
                                 peer_id, status_to_hex(ret));
        // Non-fatal: continue without audio
    }

    // 8. Create callback context (heap-allocated, SDK stores raw pointer)
    auto* cb_ctx = new Impl::CallbackContext{impl_.get(), peer_id};

    // 9. Register ICE candidate callback
    ret = peerConnectionOnIceCandidate(peer_connection,
                                       reinterpret_cast<UINT64>(cb_ctx),
                                       Impl::on_ice_candidate_handler);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("peerConnectionOnIceCandidate failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "peerConnectionOnIceCandidate failed: " + status_to_hex(ret);
        delete cb_ctx;
        freePeerConnection(&peer_connection);
        return false;
    }

    // 10. Register connection state change callback
    ret = peerConnectionOnConnectionStateChange(peer_connection,
                                                reinterpret_cast<UINT64>(cb_ctx),
                                                Impl::on_connection_state_change);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("peerConnectionOnConnectionStateChange failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "peerConnectionOnConnectionStateChange failed: " + status_to_hex(ret);
        delete cb_ctx;
        freePeerConnection(&peer_connection);
        return false;
    }

    // 11. Set remote description (Viewer's Offer) — use SDK's deserializer
    //     like the official sample (handles JSON {"type":"offer","sdp":"..."})
    RtcSessionDescriptionInit offer_sdp;
    MEMSET(&offer_sdp, 0, SIZEOF(RtcSessionDescriptionInit));

    ret = deserializeSessionDescriptionInit(
        const_cast<PCHAR>(sdp_offer.c_str()),
        static_cast<UINT32>(sdp_offer.size()),
        &offer_sdp);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("deserializeSessionDescriptionInit failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "deserializeSessionDescriptionInit failed: " + status_to_hex(ret);
        delete cb_ctx;
        freePeerConnection(&peer_connection);
        return false;
    }

    ret = setRemoteDescription(peer_connection, &offer_sdp);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("setRemoteDescription failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "setRemoteDescription failed: " + status_to_hex(ret);
        delete cb_ctx;
        freePeerConnection(&peer_connection);
        return false;
    }

    // 12. Set local description (empty answer_sdp — SDK populates it)
    RtcSessionDescriptionInit answer_sdp;
    MEMSET(&answer_sdp, 0x00, SIZEOF(RtcSessionDescriptionInit));
    ret = setLocalDescription(peer_connection, &answer_sdp);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("setLocalDescription failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "setLocalDescription failed: " + status_to_hex(ret);
        delete cb_ctx;
        freePeerConnection(&peer_connection);
        return false;
    }

    // 13. Create answer
    ret = createAnswer(peer_connection, &answer_sdp);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("createAnswer failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "createAnswer failed: " + status_to_hex(ret);
        delete cb_ctx;
        freePeerConnection(&peer_connection);
        return false;
    }

    // 14. Serialize answer to JSON and send via signaling
    UINT32 serialized_len = MAX_SIGNALING_MESSAGE_LEN;
    CHAR serialized_answer[MAX_SIGNALING_MESSAGE_LEN];
    ret = serializeSessionDescriptionInit(&answer_sdp, serialized_answer, &serialized_len);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->error("serializeSessionDescriptionInit failed for peer {}, status: {}",
                                  peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "serializeSessionDescriptionInit failed: " + status_to_hex(ret);
        delete cb_ctx;
        freePeerConnection(&peer_connection);
        return false;
    }

    if (!impl_->signaling.send_answer(peer_id, std::string(serialized_answer, serialized_len))) {
        if (logger) logger->error("Failed to send SDP answer for peer {}", peer_id);
        if (error_msg) *error_msg = "send_answer failed";
        delete cb_ctx;
        freePeerConnection(&peer_connection);
        return false;
    }

    // 15. Store in peers map
    PeerInfo info;
    info.peer_connection = peer_connection;
    info.video_transceiver = video_transceiver;
    info.consecutive_write_failures = 0;
    impl_->peers[peer_id] = info;
    impl_->callback_contexts[peer_id] = cb_ctx;

    if (logger) logger->info("Created PeerConnection for peer {}, count={}",
                             peer_id, impl_->peers.size());

    // 16. Flush any ICE candidates that arrived before the offer was processed.
    auto pending_it = impl_->pending_candidates.find(peer_id);
    if (pending_it != impl_->pending_candidates.end()) {
        auto& pending = pending_it->second;
        if (logger) logger->info("Flushing {} buffered ICE candidates for peer {}",
                                 pending.size(), peer_id);
        for (const auto& cand : pending) {
            RtcIceCandidateInit cand_init;
            MEMSET(&cand_init, 0, SIZEOF(RtcIceCandidateInit));
            STATUS cand_ret = deserializeRtcIceCandidateInit(
                const_cast<PCHAR>(cand.c_str()),
                static_cast<UINT32>(cand.size()),
                &cand_init);
            if (STATUS_FAILED(cand_ret)) {
                if (logger) logger->warn("Failed to deserialize buffered ICE candidate for peer {}, status: {}",
                                         peer_id, status_to_hex(cand_ret));
                continue;
            }
            cand_ret = addIceCandidate(peer_connection, cand_init.candidate);
            if (STATUS_FAILED(cand_ret)) {
                if (logger) logger->warn("Failed to add buffered ICE candidate for peer {}, status: {}",
                                         peer_id, status_to_hex(cand_ret));
            }
        }
        impl_->pending_candidates.erase(pending_it);
    }

    return true;
}

// ---- on_viewer_ice_candidate ----

bool WebRtcMediaManager::on_viewer_ice_candidate(
    const std::string& peer_id, const std::string& candidate,
    std::string* error_msg) {
    std::lock_guard<std::mutex> lock(impl_->peers_mutex);
    auto logger = spdlog::get("pipeline");

    auto it = impl_->peers.find(peer_id);
    if (it == impl_->peers.end()) {
        // Peer not yet created (offer not processed yet) — buffer the candidate.
        auto& pending = impl_->pending_candidates[peer_id];
        if (pending.size() < Impl::kMaxPendingCandidates) {
            pending.push_back(candidate);
            if (logger) logger->info("Buffered early ICE candidate for peer: {} (buffered={})",
                                     peer_id, pending.size());
        } else {
            if (logger) logger->warn("Pending ICE candidate buffer full for peer: {}, dropping",
                                     peer_id);
        }
        return true;  // not an error — will be applied after offer
    }

    RtcIceCandidateInit candidate_init;
    MEMSET(&candidate_init, 0, SIZEOF(RtcIceCandidateInit));
    STATUS ret = deserializeRtcIceCandidateInit(
        const_cast<PCHAR>(candidate.c_str()),
        static_cast<UINT32>(candidate.size()),
        &candidate_init);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->warn("deserializeRtcIceCandidateInit failed for peer {}, status: {}",
                                 peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "deserializeRtcIceCandidateInit failed: " + status_to_hex(ret);
        return false;
    }

    ret = addIceCandidate(it->second.peer_connection, candidate_init.candidate);
    if (STATUS_FAILED(ret)) {
        if (logger) logger->warn("addIceCandidate failed for peer {}, status: {}",
                                 peer_id, status_to_hex(ret));
        if (error_msg) *error_msg = "addIceCandidate failed: " + status_to_hex(ret);
        return false;
    }

    if (logger) logger->debug("Added ICE candidate for peer: {}", peer_id);
    return true;
}

// ---- remove_peer ----

void WebRtcMediaManager::remove_peer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(impl_->peers_mutex);
    impl_->remove_peer_locked(peer_id, "manual_remove");
}

// ---- broadcast_frame ----

void WebRtcMediaManager::broadcast_frame(
    const uint8_t* data, size_t size,
    uint64_t timestamp_100ns, bool is_keyframe) {
    std::lock_guard<std::mutex> lock(impl_->peers_mutex);
    if (impl_->peers.empty()) return;

    auto logger = spdlog::get("pipeline");

    // Collect peer_ids that need cleanup after iteration.
    std::vector<std::string> to_remove;

    for (auto& [id, info] : impl_->peers) {
        if (!info.video_transceiver) continue;
        if (!info.connected) continue;  // skip peers still in ICE negotiation

        Frame frame;
        MEMSET(&frame, 0, SIZEOF(Frame));
        frame.version = FRAME_CURRENT_VERSION;
        frame.trackId = DEFAULT_VIDEO_TRACK_ID;
        frame.duration = 0;
        frame.decodingTs = timestamp_100ns;
        frame.presentationTs = timestamp_100ns;
        frame.frameData = const_cast<PBYTE>(data);
        frame.size = static_cast<UINT32>(size);
        frame.flags = is_keyframe ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;

        STATUS ret = writeFrame(info.video_transceiver, &frame);
        if (STATUS_SUCCEEDED(ret)) {
            info.consecutive_write_failures = 0;
        } else if (ret == 0x5c000003) {
            // STATUS_SRTP_NOT_READY_YET — DTLS handshake still in progress,
            // skip silently without counting as failure.
            continue;
        } else {
            info.consecutive_write_failures++;
            if (logger) logger->warn("writeFrame failed for peer {}, status: {}, failures: {}",
                                     id, status_to_hex(ret), info.consecutive_write_failures);
            if (info.consecutive_write_failures > kMaxWriteFailures) {
                if (logger) logger->warn("Peer {} exceeded max write failures ({}), scheduling removal",
                                         id, kMaxWriteFailures);
                to_remove.push_back(id);
            }
        }
    }

    // Clean up peers that exceeded failure threshold (outside iteration).
    for (const auto& id : to_remove) {
        impl_->remove_peer_locked(id, "write_failures_exceeded");
    }
}

// ---- peer_count ----

size_t WebRtcMediaManager::peer_count() const {
    std::lock_guard<std::mutex> lock(impl_->peers_mutex);
    return impl_->peers.size();
}

// ---- Constructor / Destructor ----

WebRtcMediaManager::WebRtcMediaManager() = default;
WebRtcMediaManager::~WebRtcMediaManager() = default;

#endif  // HAVE_KVS_WEBRTC_SDK
