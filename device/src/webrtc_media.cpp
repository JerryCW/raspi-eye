// webrtc_media.cpp
// WebRTC media manager: PeerConnection lifecycle + H.264 frame broadcast (pImpl).
// Conditional compilation: stub (no SDK) / real (KVS WebRTC C SDK).

// --- Common code (extract_sdp_summary) moved to sdp_util.cpp ---

#include "webrtc_media.h"

#ifndef HAVE_KVS_WEBRTC_SDK

#include "webrtc_signaling.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

static constexpr size_t kMaxPeers = 10;
static constexpr size_t kMaxPeerIdLen = 256;
static constexpr int kCleanupIntervalMs = 1000;
static constexpr int kDisconnectGracePeriodSec = 10;

enum class PeerState { CONNECTING, CONNECTED, DISCONNECTING };

struct PeerInfo {
    PeerState state = PeerState::CONNECTING;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point disconnected_at;
    bool first_frame_sent = false;
    std::string disconnect_reason;
    uint32_t sent_candidates = 0;
    uint32_t received_candidates = 0;
    // Spec 26: 仅关键帧模式字段
    bool keyframe_only_mode = false;
    int keyframe_mode_success_count = 0;
    uint32_t consecutive_write_failures = 0;
};

// 待释放 peer 的信息（在锁外输出日志）
struct StubPeerToFree {
    std::string peer_id;
    double alive_sec = 0.0;
    std::string disconnect_reason;
};

struct WebRtcMediaManager::Impl {
    WebRtcSignaling& signaling;
    std::unordered_map<std::string, PeerInfo> peers;
    mutable std::shared_mutex peers_mutex;
    std::atomic<bool> running{true};
    std::thread cleanup_thread;
    std::condition_variable_any cv;

    // Spec 26: pipeline 引用（不拥有，不 unref）和仅关键帧模式阈值
    GstElement* pipeline_ = nullptr;
    int writeframe_fail_threshold_ = 10;

    explicit Impl(WebRtcSignaling& sig) : signaling(sig) {
        cleanup_thread = std::thread([this]() { cleanup_loop(); });
    }

    void cleanup_loop() {
        while (running.load()) {
            std::vector<StubPeerToFree> to_free;
            {
                std::unique_lock<std::shared_mutex> lock(peers_mutex);
                cv.wait_for(lock, std::chrono::milliseconds(kCleanupIntervalMs),
                           [this]() { return !running.load(); });
                if (!running.load()) break;

                auto now = std::chrono::steady_clock::now();
                for (auto it = peers.begin(); it != peers.end(); ) {
                    if (it->second.state == PeerState::DISCONNECTING) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - it->second.disconnected_at).count();
                        if (elapsed >= kDisconnectGracePeriodSec) {
                            StubPeerToFree ptf;
                            ptf.peer_id = it->first;
                            ptf.alive_sec = std::chrono::duration<double>(
                                now - it->second.created_at).count();
                            ptf.disconnect_reason = it->second.disconnect_reason;
                            to_free.push_back(std::move(ptf));
                            it = peers.erase(it);
                            continue;
                        }
                    }
                    ++it;
                }
            }
            // 锁外清理（stub 中只是日志）
            if (!to_free.empty()) {
                auto logger = spdlog::get("webrtc");
                for (const auto& ptf : to_free) {
                    if (logger) logger->info("Cleanup: freed peer {} (alive={:.1f}s, reason={})",
                                             ptf.peer_id, ptf.alive_sec, ptf.disconnect_reason);
                }
            }
        }
    }

    ~Impl() {
        running.store(false);
        cv.notify_all();
        if (cleanup_thread.joinable()) cleanup_thread.join();

        // 安全清理所有 peer
        struct ShutdownPeerInfo {
            std::string peer_id;
            double alive_sec = 0.0;
            std::string state_str;
        };
        std::vector<ShutdownPeerInfo> remaining;
        {
            std::unique_lock<std::shared_mutex> lock(peers_mutex);
            auto now = std::chrono::steady_clock::now();
            for (const auto& [id, info] : peers) {
                ShutdownPeerInfo spi;
                spi.peer_id = id;
                spi.alive_sec = std::chrono::duration<double>(now - info.created_at).count();
                switch (info.state) {
                    case PeerState::CONNECTING:    spi.state_str = "CONNECTING"; break;
                    case PeerState::CONNECTED:     spi.state_str = "CONNECTED"; break;
                    case PeerState::DISCONNECTING: spi.state_str = "DISCONNECTING"; break;
                }
                remaining.push_back(std::move(spi));
            }
            peers.clear();
        }
        // 锁外清理（stub 中只是日志）
        if (!remaining.empty()) {
            auto logger = spdlog::get("webrtc");
            for (const auto& spi : remaining) {
                if (logger) logger->info("Shutdown: freed peer {} (alive={:.1f}s, state={})",
                                         spi.peer_id, spi.alive_sec, spi.state_str);
            }
        }
    }
};

std::unique_ptr<WebRtcMediaManager> WebRtcMediaManager::create(
    WebRtcSignaling& signaling, const std::string& /*aws_region*/,
    std::string* /*error_msg*/) {
    auto obj = std::unique_ptr<WebRtcMediaManager>(new WebRtcMediaManager());
    obj->impl_ = std::make_unique<Impl>(signaling);
    auto logger = spdlog::get("webrtc");
    if (logger) logger->info("Created WebRtcMediaManager stub");
    return obj;
}

bool WebRtcMediaManager::on_viewer_offer(
    const std::string& peer_id, const std::string& /*sdp_offer*/,
    std::string* error_msg) {
    auto logger = spdlog::get("webrtc");

    // peer_id 长度检查（不需要锁）
    if (peer_id.size() > kMaxPeerIdLen) {
        if (logger) logger->warn("Rejecting peer with oversized id ({} bytes, max {})",
                                 peer_id.size(), kMaxPeerIdLen);
        if (error_msg) *error_msg = "peer_id too long";
        return false;
    }

    // 第一阶段（unique_lock）：收集 DISCONNECTING peers + 旧同 peer_id peer 到 to_free
    std::vector<StubPeerToFree> to_free;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->peers_mutex);
        auto now = std::chrono::steady_clock::now();
        // 收集所有 DISCONNECTING peers
        for (auto it = impl_->peers.begin(); it != impl_->peers.end(); ) {
            if (it->second.state == PeerState::DISCONNECTING) {
                StubPeerToFree ptf;
                ptf.peer_id = it->first;
                ptf.alive_sec = std::chrono::duration<double>(
                    now - it->second.created_at).count();
                ptf.disconnect_reason = it->second.disconnect_reason;
                to_free.push_back(std::move(ptf));
                it = impl_->peers.erase(it);
            } else {
                ++it;
            }
        }
        // 收集旧同 peer_id peer（如果存在且非 DISCONNECTING）
        auto existing = impl_->peers.find(peer_id);
        if (existing != impl_->peers.end()) {
            StubPeerToFree ptf;
            ptf.peer_id = existing->first;
            ptf.alive_sec = std::chrono::duration<double>(
                now - existing->second.created_at).count();
            ptf.disconnect_reason = "replaced";
            to_free.push_back(std::move(ptf));
            impl_->peers.erase(existing);
        }
    }

    // 第二阶段（无锁）：清理（stub 中只是日志）
    for (const auto& ptf : to_free) {
        if (logger) logger->info("Freed stale peer {} (alive={:.1f}s, reason={})",
                                 ptf.peer_id, ptf.alive_sec, ptf.disconnect_reason);
    }

    // 第三阶段（unique_lock）：检查 max peers，创建新 peer
    {
        std::unique_lock<std::shared_mutex> lock(impl_->peers_mutex);
        if (impl_->peers.size() >= kMaxPeers) {
            if (logger) logger->warn("Max peers ({}) reached, rejecting peer: {}",
                                     kMaxPeers, peer_id);
            if (error_msg) *error_msg = "Max peer count reached";
            return false;
        }
        // stub 中 peer 创建后立即设为 CONNECTED（无 ICE 协商过程）
        auto& info = impl_->peers[peer_id];
        info.state = PeerState::CONNECTED;
        info.created_at = std::chrono::steady_clock::now();
        if (logger) logger->info("Stub: added peer {}, count={}", peer_id, impl_->peers.size());
    }
    return true;
}

bool WebRtcMediaManager::on_viewer_ice_candidate(
    const std::string& peer_id, const std::string& /*candidate*/,
    std::string* /*error_msg*/) {
    auto logger = spdlog::get("webrtc");
    {
        std::unique_lock<std::shared_mutex> lock(impl_->peers_mutex);
        auto it = impl_->peers.find(peer_id);
        if (it != impl_->peers.end()) {
            it->second.received_candidates++;
        }
    }
    if (logger) logger->debug("Stub: received ICE candidate for peer {}", peer_id);
    return true;
}

void WebRtcMediaManager::remove_peer(const std::string& peer_id) {
    auto logger = spdlog::get("webrtc");
    std::vector<StubPeerToFree> to_free;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->peers_mutex);
        auto it = impl_->peers.find(peer_id);
        if (it != impl_->peers.end()) {
            StubPeerToFree ptf;
            ptf.peer_id = it->first;
            ptf.alive_sec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - it->second.created_at).count();
            ptf.disconnect_reason = "manual_remove";
            to_free.push_back(std::move(ptf));
            impl_->peers.erase(it);
        }
    }
    // 锁外清理（stub 中只是日志）
    for (const auto& ptf : to_free) {
        if (logger) logger->info("Removed peer {} (alive={:.1f}s, reason=manual_remove)",
                                 ptf.peer_id, ptf.alive_sec);
    }
}

void WebRtcMediaManager::broadcast_frame(
    const uint8_t* /*data*/, size_t size,
    uint64_t /*timestamp_100ns*/, bool is_keyframe) {
    // stub: no-op，但仍需使用 shared_lock 保持与 real 实现一致的锁策略
    std::shared_lock<std::shared_mutex> lock(impl_->peers_mutex);
    auto logger = spdlog::get("webrtc");
    // 遍历 peers，跳过非 CONNECTED peer（stub 中无实际操作）
    for (auto& [id, info] : impl_->peers) {
        if (info.state != PeerState::CONNECTED) continue;
        if (!info.first_frame_sent) {
            if (logger) logger->info("First frame sent to peer {} (size={}, keyframe={})",
                                     id, size, is_keyframe);
            info.first_frame_sent = true;
        }
        // stub: no-op
    }
}

size_t WebRtcMediaManager::peer_count() const {
    std::shared_lock<std::shared_mutex> lock(impl_->peers_mutex);
    size_t count = 0;
    for (const auto& [id, info] : impl_->peers) {
        if (info.state == PeerState::CONNECTED) ++count;
    }
    return count;
}

void WebRtcMediaManager::set_pipeline(GstElement* pipeline) {
    impl_->pipeline_ = pipeline;
}

void WebRtcMediaManager::set_writeframe_fail_threshold(int threshold) {
    impl_->writeframe_fail_threshold_ = threshold;
}

WebRtcMediaManager::WebRtcMediaManager() = default;
WebRtcMediaManager::~WebRtcMediaManager() = default;

#endif  // !HAVE_KVS_WEBRTC_SDK


// ============================================================
// Real implementation: KVS WebRTC C SDK
// ============================================================

#ifdef HAVE_KVS_WEBRTC_SDK

#include "webrtc_signaling.h"
#include <spdlog/spdlog.h>
#include <gst/video/video-event.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
}

static constexpr size_t kMaxPeers = 10;
static constexpr size_t kMaxPeerIdLen = 256;
static constexpr uint32_t kMaxWriteFailures = 100;
static constexpr int kCleanupIntervalMs = 1000;
static constexpr int kDisconnectGracePeriodSec = 10;
static constexpr int kShutdownDrainSec = 1;

enum class PeerState { CONNECTING, CONNECTED, DISCONNECTING };

struct PeerInfo {
    PRtcPeerConnection peer_connection = nullptr;
    PRtcRtpTransceiver video_transceiver = nullptr;
    uint32_t consecutive_write_failures = 0;
    std::atomic<PeerState> state{PeerState::CONNECTING};
    std::chrono::steady_clock::time_point disconnected_at;
    // Spec 25: observability fields
    std::chrono::steady_clock::time_point created_at;
    bool first_frame_sent = false;
    std::string disconnect_reason;
    uint32_t sent_candidates = 0;
    uint32_t received_candidates = 0;
    // Spec 26: 仅关键帧模式字段
    bool keyframe_only_mode = false;
    int keyframe_mode_success_count = 0;
};

// 待释放 peer 的信息（在锁外执行 close + free）
struct PeerToFree {
    PRtcPeerConnection peer_connection;
    struct CallbackContextFwd;  // forward — 实际用 Impl::CallbackContext*
    void* callback_context;     // 存储 CallbackContext* 以便锁外 delete
    std::string peer_id;
    double alive_sec = 0.0;
    std::string disconnect_reason;
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
    mutable std::shared_mutex peers_mutex;

    // 控制清理线程生命周期
    std::atomic<bool> running{true};
    std::thread cleanup_thread;
    std::condition_variable_any cv;
    std::atomic<size_t> viewer_count{0};

    // Prevent dangling CallbackContext pointers: store them so we can
    // invalidate on peer removal.  Each entry is heap-allocated because
    // the SDK stores a raw UINT64 cast of the pointer.
    std::unordered_map<std::string, CallbackContext*> callback_contexts;

    // Buffer ICE candidates that arrive before the SDP offer is processed.
    // Key: peer_id, Value: list of raw candidate JSON strings.
    static constexpr size_t kMaxPendingCandidates = 50;
    std::unordered_map<std::string, std::vector<std::string>> pending_candidates;

    // Spec 26: pipeline 引用（不拥有，不 unref）和仅关键帧模式阈值
    GstElement* pipeline_ = nullptr;
    int writeframe_fail_threshold_ = 10;

    explicit Impl(WebRtcSignaling& sig) : signaling(sig) {
        cleanup_thread = std::thread([this]() { cleanup_loop(); });
    }

    // 清理线程：每 1 秒扫描 DISCONNECTING 超过 grace period 的 peer
    void cleanup_loop() {
        while (running.load()) {
            std::vector<PeerToFree> to_free;
            {
                std::unique_lock<std::shared_mutex> lock(peers_mutex);
                cv.wait_for(lock, std::chrono::milliseconds(kCleanupIntervalMs),
                           [this]() { return !running.load(); });
                if (!running.load()) break;

                auto now = std::chrono::steady_clock::now();
                for (auto it = peers.begin(); it != peers.end(); ) {
                    if (it->second.state.load() == PeerState::DISCONNECTING) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - it->second.disconnected_at).count();
                        if (elapsed >= kDisconnectGracePeriodSec) {
                            PeerToFree ptf;
                            ptf.peer_connection = it->second.peer_connection;
                            ptf.peer_id = it->first;
                            ptf.alive_sec = std::chrono::duration<double>(
                                now - it->second.created_at).count();
                            ptf.disconnect_reason = it->second.disconnect_reason;
                            // 失效 callback context
                            auto ctx_it = callback_contexts.find(it->first);
                            if (ctx_it != callback_contexts.end()) {
                                ctx_it->second->impl = nullptr;
                                ptf.callback_context = ctx_it->second;
                                callback_contexts.erase(ctx_it);
                            } else {
                                ptf.callback_context = nullptr;
                            }
                            pending_candidates.erase(it->first);
                            it = peers.erase(it);
                            to_free.push_back(std::move(ptf));
                            continue;
                        }
                    }
                    ++it;
                }
            }
            // 锁外：close + free
            if (!to_free.empty()) {
                auto logger = spdlog::get("webrtc");
                for (auto& ptf : to_free) {
                    if (ptf.peer_connection) {
                        closePeerConnection(ptf.peer_connection);
                        freePeerConnection(&ptf.peer_connection);
                    }
                    if (ptf.callback_context) {
                        delete static_cast<CallbackContext*>(ptf.callback_context);
                    }
                    if (logger) logger->info("Cleanup: freed peer {} (alive={:.1f}s, reason={})",
                                             ptf.peer_id, ptf.alive_sec, ptf.disconnect_reason);
                }
            }
        }
    }

    // SDK callback: local ICE candidate generated.
    static VOID on_ice_candidate_handler(UINT64 custom_data, PCHAR candidate_json) {
        auto* ctx = reinterpret_cast<CallbackContext*>(custom_data);
        if (!ctx || !ctx->impl) return;
        if (candidate_json == NULL) return;

        auto logger = spdlog::get("webrtc");
        if (logger) logger->debug("Sending ICE candidate to peer: {}", ctx->peer_id);
        {
            std::unique_lock<std::shared_mutex> lock(ctx->impl->peers_mutex);
            auto it = ctx->impl->peers.find(ctx->peer_id);
            if (it != ctx->impl->peers.end()) {
                it->second.sent_candidates++;
            }
        }
        ctx->impl->signaling.send_ice_candidate(ctx->peer_id,
                                                 std::string(candidate_json));
    }

    // SDK callback: PeerConnection state changed.
    // 仅做状态标记，不调用 freePeerConnection。
    static VOID on_connection_state_change(UINT64 custom_data,
                                           RTC_PEER_CONNECTION_STATE new_state) {
        auto* ctx = reinterpret_cast<CallbackContext*>(custom_data);
        if (!ctx || !ctx->impl) return;

        auto logger = spdlog::get("webrtc");
        if (new_state == RTC_PEER_CONNECTION_STATE_CONNECTED) {
            std::unique_lock<std::shared_mutex> lock(ctx->impl->peers_mutex);
            auto it = ctx->impl->peers.find(ctx->peer_id);
            if (it != ctx->impl->peers.end()) {
                auto elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - it->second.created_at).count();
                if (logger) logger->info("Peer {} connected (elapsed={:.1f}s, ice_sent={}, ice_recv={})",
                                         ctx->peer_id, elapsed,
                                         it->second.sent_candidates, it->second.received_candidates);
                auto prev = it->second.state.exchange(PeerState::CONNECTED);
                if (prev != PeerState::CONNECTED) {
                    ctx->impl->viewer_count.fetch_add(1);
                }
            }
            // Spec 26: 新 peer 连接后请求关键帧
            if (ctx->impl->pipeline_) {
                GstElement* encoder = gst_bin_get_by_name(
                    GST_BIN(ctx->impl->pipeline_), "encoder");
                if (encoder) {
                    GstEvent* event = gst_video_event_new_upstream_force_key_unit(
                        GST_CLOCK_TIME_NONE, TRUE, 0);
                    gst_element_send_event(encoder, event);
                    gst_object_unref(encoder);
                    if (logger) logger->info("Force keyframe requested for new peer {}",
                                             ctx->peer_id);
                } else {
                    if (logger) logger->warn("Cannot force keyframe: encoder element not found in pipeline");
                }
            } else {
                if (logger) logger->warn("Cannot force keyframe: pipeline reference not set");
            }
            return;
        }
        if (new_state == RTC_PEER_CONNECTION_STATE_FAILED ||
            new_state == RTC_PEER_CONNECTION_STATE_CLOSED) {
            const bool is_failed = (new_state == RTC_PEER_CONNECTION_STATE_FAILED);
            const char* reason = is_failed ? "connection_failed" : "connection_closed";
            if (is_failed) {
                if (logger) logger->warn("Peer {} connection FAILED, marking DISCONNECTING", ctx->peer_id);
            } else {
                if (logger) logger->info("Peer {} connection closed, marking DISCONNECTING", ctx->peer_id);
            }
            std::unique_lock<std::shared_mutex> lock(ctx->impl->peers_mutex);
            auto it = ctx->impl->peers.find(ctx->peer_id);
            if (it != ctx->impl->peers.end()) {
                auto prev = it->second.state.exchange(PeerState::DISCONNECTING);
                if (prev == PeerState::CONNECTED) {
                    ctx->impl->viewer_count.fetch_sub(1);
                }
                it->second.disconnected_at = std::chrono::steady_clock::now();
                it->second.disconnect_reason = reason;
            }
            // 不调用 freePeerConnection！由清理线程或 on_viewer_offer 入口处理。
        }
    }

    ~Impl() {
        // 停止清理线程
        running.store(false);
        cv.notify_all();
        if (cleanup_thread.joinable()) cleanup_thread.join();

        auto logger = spdlog::get("webrtc");

        // unique_lock 内：closePeerConnection 所有 peer，收集到 to_free
        std::vector<PeerToFree> to_free;
        // 收集每个 peer 的状态字符串用于 shutdown 日志
        std::vector<std::string> peer_state_strs;
        {
            std::unique_lock<std::shared_mutex> lock(peers_mutex);
            auto now = std::chrono::steady_clock::now();
            for (auto& [id, info] : peers) {
                if (info.peer_connection) {
                    closePeerConnection(info.peer_connection);
                }
                PeerToFree ptf;
                ptf.peer_connection = info.peer_connection;
                ptf.peer_id = id;
                ptf.alive_sec = std::chrono::duration<double>(now - info.created_at).count();
                auto ctx_it = callback_contexts.find(id);
                if (ctx_it != callback_contexts.end()) {
                    ctx_it->second->impl = nullptr;
                    ptf.callback_context = ctx_it->second;
                } else {
                    ptf.callback_context = nullptr;
                }
                to_free.push_back(std::move(ptf));
                // 记录状态字符串
                switch (info.state.load()) {
                    case PeerState::CONNECTING:    peer_state_strs.push_back("CONNECTING"); break;
                    case PeerState::CONNECTED:     peer_state_strs.push_back("CONNECTED"); break;
                    case PeerState::DISCONNECTING: peer_state_strs.push_back("DISCONNECTING"); break;
                }
            }
            peers.clear();
            callback_contexts.clear();
            pending_candidates.clear();
        }

        // 等待 SDK 内部线程完成
        std::this_thread::sleep_for(std::chrono::seconds(kShutdownDrainSec));

        // 锁外：freePeerConnection + 清理 callback context
        for (size_t i = 0; i < to_free.size(); ++i) {
            auto& ptf = to_free[i];
            if (ptf.peer_connection) {
                freePeerConnection(&ptf.peer_connection);
            }
            if (ptf.callback_context) {
                delete static_cast<CallbackContext*>(ptf.callback_context);
            }
            if (logger) logger->info("Shutdown: freed peer {} (alive={:.1f}s, state={})",
                                     ptf.peer_id, ptf.alive_sec, peer_state_strs[i]);
        }

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
    auto logger = spdlog::get("webrtc");
    if (logger) logger->info("Created WebRtcMediaManager (KVS WebRTC SDK)");
    return obj;
}

// ---- on_viewer_offer ----

bool WebRtcMediaManager::on_viewer_offer(
    const std::string& peer_id, const std::string& sdp_offer,
    std::string* error_msg) {
    auto logger = spdlog::get("webrtc");

    // peer_id 长度检查（不需要锁）
    if (peer_id.size() > kMaxPeerIdLen) {
        if (logger) logger->warn("Rejecting peer with oversized id ({} bytes, max {})",
                                 peer_id.size(), kMaxPeerIdLen);
        if (error_msg) *error_msg = "peer_id too long";
        return false;
    }

    // 第一阶段（unique_lock）：收集 DISCONNECTING peers + 旧同 peer_id peer 到 to_free
    std::vector<PeerToFree> to_free;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->peers_mutex);
        auto now = std::chrono::steady_clock::now();
        // 收集所有 DISCONNECTING peers
        for (auto it = impl_->peers.begin(); it != impl_->peers.end(); ) {
            if (it->second.state.load() == PeerState::DISCONNECTING) {
                PeerToFree ptf;
                ptf.peer_connection = it->second.peer_connection;
                ptf.peer_id = it->first;
                ptf.alive_sec = std::chrono::duration<double>(now - it->second.created_at).count();
                ptf.disconnect_reason = it->second.disconnect_reason;
                auto ctx_it = impl_->callback_contexts.find(it->first);
                if (ctx_it != impl_->callback_contexts.end()) {
                    ctx_it->second->impl = nullptr;
                    ptf.callback_context = ctx_it->second;
                    impl_->callback_contexts.erase(ctx_it);
                } else {
                    ptf.callback_context = nullptr;
                }
                impl_->pending_candidates.erase(it->first);
                it = impl_->peers.erase(it);
                to_free.push_back(std::move(ptf));
            } else {
                ++it;
            }
        }
        // 收集旧同 peer_id peer（如果存在且非 DISCONNECTING — 已在上面收集）
        auto existing = impl_->peers.find(peer_id);
        if (existing != impl_->peers.end()) {
            if (logger) logger->info("Replacing existing peer: {}", peer_id);
            PeerToFree ptf;
            ptf.peer_connection = existing->second.peer_connection;
            ptf.peer_id = existing->first;
            ptf.alive_sec = std::chrono::duration<double>(now - existing->second.created_at).count();
            ptf.disconnect_reason = "replaced";
            auto prev_state = existing->second.state.load();
            if (prev_state == PeerState::CONNECTED) {
                impl_->viewer_count.fetch_sub(1);
            }
            auto ctx_it = impl_->callback_contexts.find(peer_id);
            if (ctx_it != impl_->callback_contexts.end()) {
                ctx_it->second->impl = nullptr;
                ptf.callback_context = ctx_it->second;
                impl_->callback_contexts.erase(ctx_it);
            } else {
                ptf.callback_context = nullptr;
            }
            impl_->pending_candidates.erase(peer_id);
            impl_->peers.erase(existing);
            to_free.push_back(std::move(ptf));
        }
    }

    // 第二阶段（无锁）：close + free
    for (auto& ptf : to_free) {
        if (ptf.peer_connection) {
            closePeerConnection(ptf.peer_connection);
            freePeerConnection(&ptf.peer_connection);
        }
        if (ptf.callback_context) {
            delete static_cast<Impl::CallbackContext*>(ptf.callback_context);
        }
        if (logger) logger->info("Freed stale peer {} (alive={:.1f}s, reason={})",
                                 ptf.peer_id, ptf.alive_sec, ptf.disconnect_reason);
    }

    // 第三阶段（unique_lock）：检查 max peers，创建新 PeerConnection
    std::unique_lock<std::shared_mutex> lock(impl_->peers_mutex);

    // Max peers check
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

    if (logger) logger->debug("Offer SDP summary for peer {}: {}", peer_id, extract_sdp_summary(sdp_offer));

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

    if (logger) logger->debug("Answer SDP summary for peer {}: {}", peer_id,
                              extract_sdp_summary(std::string(serialized_answer, serialized_len)));

    // 15. Store in peers map (state = CONNECTING, 等待 on_connection_state_change 设为 CONNECTED)
    // 注意：PeerInfo 含 std::atomic（不可拷贝/移动），用 try_emplace 就地构造后逐字段赋值
    auto [it_peer, inserted] = impl_->peers.try_emplace(peer_id);
    auto& info = it_peer->second;
    info.peer_connection = peer_connection;
    info.video_transceiver = video_transceiver;
    info.consecutive_write_failures = 0;
    info.state.store(PeerState::CONNECTING);
    info.created_at = std::chrono::steady_clock::now();
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
    std::unique_lock<std::shared_mutex> lock(impl_->peers_mutex);
    auto logger = spdlog::get("webrtc");

    auto it = impl_->peers.find(peer_id);
    if (it == impl_->peers.end()) {
        // Peer not yet created (offer not processed yet) — buffer the candidate.
        auto& pending = impl_->pending_candidates[peer_id];
        if (pending.size() < Impl::kMaxPendingCandidates) {
            pending.push_back(candidate);
            if (logger) logger->debug("Buffered early ICE candidate for peer: {} (buffered={})",
                                     peer_id, pending.size());
        } else {
            if (logger) logger->warn("Pending ICE candidate buffer full for peer: {}, dropping",
                                     peer_id);
        }
        return true;  // not an error — will be applied after offer
    }

    RtcIceCandidateInit candidate_init;
    MEMSET(&candidate_init, 0, SIZEOF(RtcIceCandidateInit));
    it->second.received_candidates++;
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
    auto logger = spdlog::get("webrtc");
    std::vector<PeerToFree> to_free;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->peers_mutex);
        auto it = impl_->peers.find(peer_id);
        if (it == impl_->peers.end()) return;

        it->second.disconnect_reason = "manual_remove";
        PeerToFree ptf;
        ptf.peer_connection = it->second.peer_connection;
        ptf.peer_id = it->first;
        ptf.alive_sec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - it->second.created_at).count();
        ptf.disconnect_reason = it->second.disconnect_reason;
        auto prev_state = it->second.state.load();
        if (prev_state == PeerState::CONNECTED) {
            impl_->viewer_count.fetch_sub(1);
        }
        // 失效 callback context
        auto ctx_it = impl_->callback_contexts.find(peer_id);
        if (ctx_it != impl_->callback_contexts.end()) {
            ctx_it->second->impl = nullptr;
            ptf.callback_context = ctx_it->second;
            impl_->callback_contexts.erase(ctx_it);
        } else {
            ptf.callback_context = nullptr;
        }
        impl_->pending_candidates.erase(peer_id);
        impl_->peers.erase(it);
        to_free.push_back(std::move(ptf));
    }
    // 锁外：close + free
    for (auto& ptf : to_free) {
        if (ptf.peer_connection) {
            closePeerConnection(ptf.peer_connection);
            freePeerConnection(&ptf.peer_connection);
        }
        if (ptf.callback_context) {
            delete static_cast<Impl::CallbackContext*>(ptf.callback_context);
        }
        if (logger) logger->info("Removed peer {} (alive={:.1f}s, reason=manual_remove)",
                                 ptf.peer_id, ptf.alive_sec);
    }
}

// ---- broadcast_frame ----

void WebRtcMediaManager::broadcast_frame(
    const uint8_t* data, size_t size,
    uint64_t timestamp_100ns, bool is_keyframe) {
    std::shared_lock<std::shared_mutex> lock(impl_->peers_mutex);
    if (impl_->peers.empty()) return;

    auto logger = spdlog::get("webrtc");

    for (auto& [id, info] : impl_->peers) {
        if (!info.video_transceiver) continue;
        if (info.state.load() != PeerState::CONNECTED) continue;

        // Spec 26: 仅关键帧模式 — 跳过非关键帧
        if (info.keyframe_only_mode && !is_keyframe) {
            continue;
        }

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
            if (!info.first_frame_sent) {
                info.first_frame_sent = true;
                if (logger) logger->info("First frame sent to peer {} (size={}, keyframe={})",
                                         id, size, is_keyframe);
            }
            // Spec 26: 仅关键帧模式恢复逻辑
            if (info.keyframe_only_mode) {
                info.keyframe_mode_success_count++;
                if (info.keyframe_mode_success_count >= 10) {
                    info.keyframe_only_mode = false;
                    info.keyframe_mode_success_count = 0;
                    if (logger) logger->info("Peer {} recovered from keyframe-only mode", id);
                }
            }
        } else if (ret == 0x5c000003) {
            // STATUS_SRTP_NOT_READY_YET — DTLS handshake still in progress,
            // skip without counting as failure.
            if (logger) logger->debug("writeFrame skipped for peer {}: SRTP not ready", id);
            continue;
        } else {
            info.consecutive_write_failures++;
            info.keyframe_mode_success_count = 0;
            if (info.consecutive_write_failures == kMaxWriteFailures / 2) {
                if (logger) logger->warn("writeFrame failing for peer {}: {}/{} consecutive failures",
                                         id, info.consecutive_write_failures, kMaxWriteFailures);
            }
            if (logger) logger->warn("writeFrame failed for peer {}, status: {}, failures: {}",
                                     id, status_to_hex(ret), info.consecutive_write_failures);
            // Spec 26: 进入仅关键帧模式
            if (info.consecutive_write_failures >= static_cast<uint32_t>(impl_->writeframe_fail_threshold_)
                && !info.keyframe_only_mode) {
                info.keyframe_only_mode = true;
                info.keyframe_mode_success_count = 0;
                if (logger) logger->info("Peer {} entered keyframe-only mode after {} consecutive failures",
                                         id, info.consecutive_write_failures);
            }
            if (info.consecutive_write_failures > kMaxWriteFailures) {
                if (logger) logger->warn("Peer {} exceeded max write failures ({}), marking DISCONNECTING",
                                         id, kMaxWriteFailures);
                info.disconnect_reason = "max_write_failures";
                // 原子设置 DISCONNECTING（std::atomic 无需升级锁）
                auto prev = info.state.exchange(PeerState::DISCONNECTING);
                if (prev == PeerState::CONNECTED) {
                    impl_->viewer_count.fetch_sub(1);
                }
                info.disconnected_at = std::chrono::steady_clock::now();
                // 不调用 remove_peer_locked！由清理线程处理。
            }
        }
    }
}

// ---- peer_count ----

size_t WebRtcMediaManager::peer_count() const {
    std::shared_lock<std::shared_mutex> lock(impl_->peers_mutex);
    size_t count = 0;
    for (const auto& [id, info] : impl_->peers) {
        if (info.state.load() == PeerState::CONNECTED) ++count;
    }
    return count;
}

// ---- set_pipeline / set_writeframe_fail_threshold ----

void WebRtcMediaManager::set_pipeline(GstElement* pipeline) {
    impl_->pipeline_ = pipeline;
}

void WebRtcMediaManager::set_writeframe_fail_threshold(int threshold) {
    impl_->writeframe_fail_threshold_ = threshold;
}

// ---- Constructor / Destructor ----

WebRtcMediaManager::WebRtcMediaManager() = default;
WebRtcMediaManager::~WebRtcMediaManager() = default;

#endif  // HAVE_KVS_WEBRTC_SDK
