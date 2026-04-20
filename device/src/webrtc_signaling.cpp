// webrtc_signaling.cpp
// KVS WebRTC signaling client — pImpl with conditional compilation.
// HAVE_KVS_WEBRTC_SDK defined: real KVS WebRTC C SDK implementation
// HAVE_KVS_WEBRTC_SDK not defined: stub implementation (macOS / Linux without SDK)

#include "webrtc_signaling.h"
#include "webrtc_media.h"  // extract_sdp_summary
#include "config_util.h"  // parse_bool_field

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

// ============================================================
// build_webrtc_config — same pattern as build_kvs_config / build_aws_config
// ============================================================

bool build_webrtc_config(
    const std::unordered_map<std::string, std::string>& kv,
    WebRtcConfig& config,
    std::string* error_msg) {
    static const std::vector<std::pair<std::string, std::string WebRtcConfig::*>> fields = {
        {"channel_name", &WebRtcConfig::channel_name},
        {"aws_region",   &WebRtcConfig::aws_region},
    };

    std::vector<std::string> missing;
    for (const auto& [name, member_ptr] : fields) {
        auto it = kv.find(name);
        if (it == kv.end() || it->second.empty()) {
            missing.push_back(name);
        } else {
            config.*member_ptr = it->second;
        }
    }

    if (!missing.empty()) {
        if (error_msg) {
            std::string msg = "Missing required fields in [webrtc]: ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i > 0) msg += ", ";
                msg += missing[i];
            }
            *error_msg = msg;
        }
        return false;
    }

    // enabled 字段（可选，默认 true）
    if (!parse_bool_field(kv, "enabled", config.enabled, error_msg)) {
        return false;
    }

    return true;
}

// ============================================================
// Conditional compilation: real SDK vs stub
// ============================================================

#ifdef HAVE_KVS_WEBRTC_SDK
// ---- Real implementation: KVS WebRTC C SDK ----

extern "C" {
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
}

struct WebRtcSignaling::Impl {
    WebRtcConfig config;
    AwsConfig aws_config;
    PAwsCredentialProvider credential_provider = nullptr;
    SIGNALING_CLIENT_HANDLE signaling_handle = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
    bool connected = false;

    OfferCallback offer_cb;
    IceCandidateCallback ice_cb;

    // 重连监控
    std::atomic<bool> needs_reconnect_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::thread reconnect_thread_;
    std::mutex reconnect_mutex_;
    std::condition_variable reconnect_cv_;
    std::chrono::steady_clock::time_point connected_at_;
    uint32_t total_disconnects_ = 0;
    uint32_t total_reconnects_ = 0;
    uint32_t reconnect_attempt_ = 0;

    static constexpr int kInitialBackoffSec = 1;
    static constexpr int kMaxBackoffSec = 30;
    static constexpr int kStableConnectionSec = 30;
    static constexpr int kHealthCheckIntervalSec = 60;

    void reconnect_loop() {
        auto logger = spdlog::get("webrtc");
        if (logger) logger->info("Reconnect monitor thread started for channel: {}", config.channel_name);

        while (!shutdown_requested_.load()) {
            std::unique_lock<std::mutex> lock(reconnect_mutex_);
            reconnect_cv_.wait_for(lock, std::chrono::seconds(kHealthCheckIntervalSec), [this] {
                return needs_reconnect_.load() || shutdown_requested_.load();
            });

            if (shutdown_requested_.load()) break;

            // Health check logging
            if (connected) {
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - connected_at_).count();
                if (logger) logger->debug("Signaling health: connected for {}s, disconnects={}, reconnects={}",
                                           duration, total_disconnects_, total_reconnects_);
                // Reset backoff if connection stable > 30s
                if (duration > kStableConnectionSec) {
                    reconnect_attempt_ = 0;
                }
            } else if (!needs_reconnect_.load()) {
                if (logger) logger->warn("Signaling health: NOT connected, disconnects={}, reconnects={}",
                                          total_disconnects_, total_reconnects_);
            }

            if (needs_reconnect_.load()) {
                needs_reconnect_.store(false);
                lock.unlock();

                // Exponential backoff
                int backoff = std::min(kInitialBackoffSec << reconnect_attempt_, kMaxBackoffSec);
                if (logger) logger->warn("Reconnect attempt {} — backoff {}s for channel: {}",
                                          reconnect_attempt_ + 1, backoff, config.channel_name);

                // Wait with shutdown check
                {
                    std::unique_lock<std::mutex> wait_lock(reconnect_mutex_);
                    reconnect_cv_.wait_for(wait_lock, std::chrono::seconds(backoff), [this] {
                        return shutdown_requested_.load();
                    });
                }
                if (shutdown_requested_.load()) break;

                // Attempt reconnect
                STATUS status = signalingClientConnectSync(signaling_handle);
                if (STATUS_SUCCESS(status)) {
                    total_reconnects_++;
                    if (logger) logger->warn("Reconnect successful — total_reconnects={} for channel: {}",
                                              total_reconnects_, config.channel_name);
                    // Note: connected=true is set by SDK callback, not here
                } else {
                    reconnect_attempt_++;
                    needs_reconnect_.store(true);
                    if (logger) logger->error("Reconnect failed — status: 0x{}, attempt={} for channel: {}",
                                               to_hex(status), reconnect_attempt_, config.channel_name);
                }
            }
        }
        if (logger) logger->info("Reconnect monitor thread stopped for channel: {}", config.channel_name);
    }

    // SDK callback: signaling client state changed
    // Forwards to C++ side — no heavy work here.
    static STATUS on_signaling_state_changed(UINT64 custom_data,
                                             SIGNALING_CLIENT_STATE state) {
        auto* self = reinterpret_cast<Impl*>(custom_data);
        auto logger = spdlog::get("webrtc");

        // 所有状态变化都打 warn 级别日志，确保长时间运行时可观测
        static const char* state_names[] = {
            "NEW", "GET_CREDENTIALS", "DESCRIBE", "CREATE",
            "GET_ENDPOINT", "GET_ICE_CONFIG", "READY",
            "CONNECTING", "CONNECTED", "DISCONNECTED",
            "DELETE", "DELETED", "MAX"
        };
        const char* name = (state < sizeof(state_names)/sizeof(state_names[0]))
            ? state_names[state] : "UNKNOWN";

        if (state == SIGNALING_CLIENT_STATE_CONNECTED) {
            self->connected = true;
            self->connected_at_ = std::chrono::steady_clock::now();
            if (logger) logger->warn("Signaling state: {} — channel {}", name, self->config.channel_name);
        } else if (state == SIGNALING_CLIENT_STATE_DISCONNECTED) {
            self->connected = false;
            if (!self->shutdown_requested_.load()) {
                self->needs_reconnect_.store(true);
                self->total_disconnects_++;
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - self->connected_at_).count();
                if (logger) logger->warn("Signaling state: DISCONNECTED — channel {} "
                                         "(was connected for {}s, total_disconnects={}, triggering auto-reconnect)",
                                         self->config.channel_name, duration, self->total_disconnects_);
                self->reconnect_cv_.notify_one();
            } else {
                if (logger) logger->info("Signaling state: DISCONNECTED — channel {} (shutdown requested, no reconnect)",
                                         self->config.channel_name);
            }
        } else {
            if (state == SIGNALING_CLIENT_STATE_READY) {
                if (self->total_disconnects_ == 0) {
                    if (logger) logger->warn("Signaling state: READY (initial connect) — channel {}", self->config.channel_name);
                } else {
                    if (logger) logger->warn("Signaling state: READY (reconnect recovery, disconnects={}) — channel {}",
                                             self->total_disconnects_, self->config.channel_name);
                }
            } else {
                if (logger) logger->warn("Signaling state: {} ({})", name, static_cast<int>(state));
            }
        }
        return STATUS_SUCCESS;
    }

    // SDK callback: signaling message received
    // Forwards to registered C++ callbacks — no heavy work here.
    static STATUS on_signaling_message_received(UINT64 custom_data,
                                                PReceivedSignalingMessage pMsg) {
        auto* self = reinterpret_cast<Impl*>(custom_data);
        auto logger = spdlog::get("webrtc");
        std::string peer_id(pMsg->signalingMessage.peerClientId);

        switch (pMsg->signalingMessage.messageType) {
            case SIGNALING_MESSAGE_TYPE_OFFER:
                if (logger) logger->info("Received SDP offer from peer: {}", peer_id);
                if (self->offer_cb) {
                    std::string sdp(pMsg->signalingMessage.payload,
                                    pMsg->signalingMessage.payloadLen);
                    if (logger) logger->debug("SDP summary for peer {}: {}", peer_id, extract_sdp_summary(sdp));
                    self->offer_cb(peer_id, sdp);
                } else {
                    if (logger) logger->warn(
                        "No offer callback registered, discarding offer from: {}",
                        peer_id);
                }
                break;
            case SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE:
                if (logger) logger->debug("Received ICE candidate from peer: {}", peer_id);
                if (self->ice_cb) {
                    std::string candidate(pMsg->signalingMessage.payload,
                                          pMsg->signalingMessage.payloadLen);
                    self->ice_cb(peer_id, candidate);
                } else {
                    if (logger) logger->warn(
                        "No ICE callback registered, discarding candidate from: {}",
                        peer_id);
                }
                break;
            default:
                if (logger) logger->debug(
                    "Received signaling message type {} from peer: {}",
                    static_cast<int>(pMsg->signalingMessage.messageType), peer_id);
                break;
        }
        return STATUS_SUCCESS;
    }

    bool init_credential_provider(std::string* error_msg) {
        // Initialize KVS WebRTC SDK before any SDK calls
        STATUS init_status = initKvsWebRtc();
        if (STATUS_FAILED(init_status)) {
            if (error_msg) {
                *error_msg = "Failed to initialize KVS WebRTC SDK, status: 0x"
                    + to_hex(init_status);
            }
            return false;
        }

        STATUS status = createLwsIotCredentialProvider(
            const_cast<PCHAR>(aws_config.credential_endpoint.c_str()),
            const_cast<PCHAR>(aws_config.cert_path.c_str()),
            const_cast<PCHAR>(aws_config.key_path.c_str()),
            const_cast<PCHAR>(aws_config.ca_path.c_str()),
            const_cast<PCHAR>(aws_config.role_alias.c_str()),
            const_cast<PCHAR>(aws_config.thing_name.c_str()),
            &credential_provider);

        if (STATUS_FAILED(status)) {
            if (error_msg) {
                *error_msg = "Failed to create IoT credential provider, status: 0x"
                    + to_hex(status);
            }
            return false;
        }
        auto logger = spdlog::get("webrtc");
        if (logger) logger->info("IoT credential provider initialized for thing: {}",
                                 aws_config.thing_name);
        return true;
    }

    bool create_and_connect(std::string* error_msg) {
        // Callbacks
        SignalingClientCallbacks callbacks;
        MEMSET(&callbacks, 0, SIZEOF(SignalingClientCallbacks));
        callbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
        callbacks.customData = reinterpret_cast<UINT64>(this);
        callbacks.stateChangeFn = on_signaling_state_changed;
        callbacks.messageReceivedFn = on_signaling_message_received;

        // Client info
        SignalingClientInfo client_info;
        MEMSET(&client_info, 0, SIZEOF(SignalingClientInfo));
        client_info.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
        client_info.loggingLevel = LOG_LEVEL_WARN;
        client_info.cacheFilePath = NULL;  // Use default cache path
        client_info.signalingClientCreationMaxRetryAttempts =
            CREATE_SIGNALING_CLIENT_RETRY_ATTEMPTS_SENTINEL_VALUE;
        STRCPY(client_info.clientId, "raspi-eye-master");

        // Channel info
        ChannelInfo channel_info;
        MEMSET(&channel_info, 0, SIZEOF(ChannelInfo));
        channel_info.version = CHANNEL_INFO_CURRENT_VERSION;
        channel_info.pChannelName = const_cast<PCHAR>(config.channel_name.c_str());
        channel_info.pKmsKeyId = NULL;
        channel_info.tagCount = 0;
        channel_info.pTags = NULL;
        channel_info.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
        channel_info.channelRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER;
        channel_info.cachingPolicy = SIGNALING_API_CALL_CACHE_TYPE_FILE;
        channel_info.cachingPeriod = SIGNALING_API_CALL_CACHE_TTL_SENTINEL_VALUE;
        channel_info.retry = TRUE;
        channel_info.reconnect = TRUE;
        channel_info.messageTtl = 0;
        channel_info.pRegion = const_cast<PCHAR>(config.aws_region.c_str());
        channel_info.pCertPath = const_cast<PCHAR>(aws_config.ca_path.c_str());

        // Create signaling client
        STATUS status = createSignalingClientSync(
            &client_info, &channel_info, &callbacks,
            credential_provider, &signaling_handle);
        if (STATUS_FAILED(status)) {
            if (error_msg) {
                *error_msg = "Failed to create signaling client for channel '"
                    + config.channel_name + "', status: 0x" + to_hex(status);
            }
            return false;
        }

        // Fetch (Describe/GetEndpoint/GetIceServerConfig)
        status = signalingClientFetchSync(signaling_handle);
        if (STATUS_FAILED(status)) {
            if (error_msg) {
                *error_msg = "Failed to fetch signaling channel '"
                    + config.channel_name + "', status: 0x" + to_hex(status);
            }
            freeSignalingClient(&signaling_handle);
            signaling_handle = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
            return false;
        }

        // Connect (establish WebSocket)
        status = signalingClientConnectSync(signaling_handle);
        if (STATUS_FAILED(status)) {
            if (error_msg) {
                *error_msg = "Failed to connect signaling client for channel '"
                    + config.channel_name + "', status: 0x" + to_hex(status);
            }
            freeSignalingClient(&signaling_handle);
            signaling_handle = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
            return false;
        }

        return true;
    }

    void release_signaling_client() {
        if (IS_VALID_SIGNALING_CLIENT_HANDLE(signaling_handle)) {
            freeSignalingClient(&signaling_handle);
            signaling_handle = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
        }
        connected = false;
    }

    uint32_t ice_config_count() const {
        UINT32 count = 0;
        signalingClientGetIceConfigInfoCount(signaling_handle, &count);
        return count;
    }

    bool ice_config(uint32_t index,
                    std::vector<WebRtcSignaling::IceServerInfo>& servers) const {
        PIceConfigInfo info = nullptr;
        STATUS ret = signalingClientGetIceConfigInfo(signaling_handle, index, &info);
        if (STATUS_FAILED(ret) || info == nullptr) return false;
        servers.clear();
        for (UINT32 j = 0; j < info->uriCount; j++) {
            servers.push_back({
                std::string(info->uris[j]),
                std::string(info->userName),
                std::string(info->password)
            });
        }
        return true;
    }

    ~Impl() {
        shutdown_requested_.store(true);
        reconnect_cv_.notify_all();
        if (reconnect_thread_.joinable()) {
            reconnect_thread_.join();
        }
        release_signaling_client();
        if (credential_provider != nullptr) {
            freeIotCredentialProvider(&credential_provider);
            credential_provider = nullptr;
        }
        deinitKvsWebRtc();
    }

    static std::string to_hex(STATUS status) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08x", status);
        return std::string(buf);
    }
};

#else
// ---- Stub implementation (macOS / Linux without SDK) ----

struct WebRtcSignaling::Impl {
    WebRtcConfig config;
    AwsConfig aws_config;
    bool connected = false;

    OfferCallback offer_cb;
    IceCandidateCallback ice_cb;

    // 重连监控（stub 版本）
    std::atomic<bool> needs_reconnect_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::thread reconnect_thread_;
    std::mutex reconnect_mutex_;
    std::condition_variable reconnect_cv_;
    uint32_t total_disconnects_ = 0;
    uint32_t total_reconnects_ = 0;

    void simulate_disconnect() {
        connected = false;
        needs_reconnect_.store(true);
        total_disconnects_++;
        reconnect_cv_.notify_one();
    }

    void reconnect_loop() {
        auto logger = spdlog::get("webrtc");
        while (!shutdown_requested_.load()) {
            std::unique_lock<std::mutex> lock(reconnect_mutex_);
            reconnect_cv_.wait(lock, [this] {
                return needs_reconnect_.load() || shutdown_requested_.load();
            });
            if (shutdown_requested_.load()) break;
            if (needs_reconnect_.load()) {
                needs_reconnect_.store(false);
                // Stub: short delay then reconnect
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!shutdown_requested_.load()) {
                    connected = true;
                    total_reconnects_++;
                    if (logger) logger->info("Stub: auto-reconnected after disconnect (total_reconnects={})", total_reconnects_);
                }
            }
        }
    }

    bool init_credential_provider(std::string* /*error_msg*/) {
        // Stub: no real credentials needed
        return true;
    }

    bool create_and_connect(std::string* /*error_msg*/) {
        connected = true;
        auto logger = spdlog::get("webrtc");
        if (logger) logger->info("WebRTC stub connected to channel: {}",
                                 config.channel_name);
        return true;
    }

    void release_signaling_client() {
        connected = false;
    }

    uint32_t ice_config_count() const { return 0; }
    bool ice_config(uint32_t, std::vector<WebRtcSignaling::IceServerInfo>&) const { return false; }

    ~Impl() {
        shutdown_requested_.store(true);
        reconnect_cv_.notify_all();
        if (reconnect_thread_.joinable()) {
            reconnect_thread_.join();
        }
    }
};

#endif  // HAVE_KVS_WEBRTC_SDK

// ============================================================
// WebRtcSignaling public interface (platform-independent)
// ============================================================

WebRtcSignaling::WebRtcSignaling() = default;
WebRtcSignaling::~WebRtcSignaling() = default;

std::unique_ptr<WebRtcSignaling> WebRtcSignaling::create(
    const WebRtcConfig& config,
    const AwsConfig& aws_config,
    std::string* error_msg) {
    auto obj = std::unique_ptr<WebRtcSignaling>(new WebRtcSignaling());
    obj->impl_ = std::make_unique<Impl>();
    obj->impl_->config = config;
    obj->impl_->aws_config = aws_config;

    if (!obj->impl_->init_credential_provider(error_msg)) {
        return nullptr;
    }

    auto logger = spdlog::get("webrtc");
#ifdef HAVE_KVS_WEBRTC_SDK
    if (logger) logger->info("Created KVS WebRTC SignalingClient for channel: {}",
                             config.channel_name);
#else
    if (logger) logger->info("Created WebRTC stub for channel: {}",
                             config.channel_name);
#endif

    return obj;
}

bool WebRtcSignaling::connect(std::string* error_msg) {
    // Stop existing reconnect thread if running
    impl_->shutdown_requested_.store(true);
    impl_->reconnect_cv_.notify_all();
    if (impl_->reconnect_thread_.joinable()) {
        impl_->reconnect_thread_.join();
    }
    if (!impl_->create_and_connect(error_msg)) return false;
    impl_->shutdown_requested_.store(false);
    auto* p = impl_.get();
    impl_->reconnect_thread_ = std::thread([p] { p->reconnect_loop(); });
    return true;
}

void WebRtcSignaling::disconnect() {
    impl_->shutdown_requested_.store(true);
    impl_->reconnect_cv_.notify_all();
    if (impl_->reconnect_thread_.joinable()) {
        impl_->reconnect_thread_.join();
    }
    impl_->release_signaling_client();
    auto logger = spdlog::get("webrtc");
    if (logger) logger->info("Signaling client disconnected (shutdown_requested)");
}

bool WebRtcSignaling::is_connected() const {
    return impl_->connected;
}

bool WebRtcSignaling::reconnect(std::string* error_msg) {
    if (impl_->reconnect_thread_.joinable() && !impl_->shutdown_requested_.load()) {
        // Reconnect thread is running — trigger async reconnect
        impl_->needs_reconnect_.store(true);
        impl_->reconnect_cv_.notify_one();
        return true;
    }
    // Thread not running (after disconnect or never started) — full reconnect
    return connect(error_msg);
}

void WebRtcSignaling::set_offer_callback(OfferCallback cb) {
    impl_->offer_cb = std::move(cb);
}

void WebRtcSignaling::set_ice_candidate_callback(IceCandidateCallback cb) {
    impl_->ice_cb = std::move(cb);
}

bool WebRtcSignaling::send_answer(const std::string& peer_id,
                                  const std::string& sdp_answer) {
    auto logger = spdlog::get("webrtc");
    if (!impl_->connected) {
        if (logger) logger->warn("Cannot send answer: signaling not connected (disconnects={}, reconnects={})",
                                  impl_->total_disconnects_, impl_->total_reconnects_);
        return false;
    }
#ifdef HAVE_KVS_WEBRTC_SDK
    if (sdp_answer.size() >= MAX_SIGNALING_MESSAGE_LEN) {
        if (logger) logger->error("SDP answer too large ({} bytes, max {})",
                                  sdp_answer.size(), MAX_SIGNALING_MESSAGE_LEN);
        return false;
    }
    SignalingMessage msg;
    MEMSET(&msg, 0, SIZEOF(SignalingMessage));
    msg.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    msg.messageType = SIGNALING_MESSAGE_TYPE_ANSWER;
    STRNCPY(msg.peerClientId, peer_id.c_str(), MAX_SIGNALING_CLIENT_ID_LEN);
    STRNCPY(msg.payload, sdp_answer.c_str(), MAX_SIGNALING_MESSAGE_LEN);
    msg.payloadLen = (UINT32) STRLEN(msg.payload);
    msg.correlationId[0] = '\0';

    STATUS status = signalingClientSendMessageSync(impl_->signaling_handle, &msg);
    if (STATUS_FAILED(status)) {
        if (logger) logger->error("Failed to send answer to peer {}, status: 0x{:08x}",
                                  peer_id, status);
        return false;
    }
    if (logger) logger->info("Sent SDP answer to peer: {}", peer_id);
    if (logger) logger->debug("Answer SDP summary for peer {}: {}", peer_id, extract_sdp_summary(sdp_answer));
#else
    if (logger) logger->info("Stub: sent SDP answer to peer: {}", peer_id);
    if (logger) logger->debug("Answer SDP summary for peer {}: {}", peer_id, extract_sdp_summary(sdp_answer));
#endif
    return true;
}

bool WebRtcSignaling::send_ice_candidate(const std::string& peer_id,
                                         const std::string& candidate) {
    auto logger = spdlog::get("webrtc");
    if (!impl_->connected) {
        if (logger) logger->warn("Cannot send ICE candidate: signaling not connected (disconnects={}, reconnects={})",
                                  impl_->total_disconnects_, impl_->total_reconnects_);
        return false;
    }
#ifdef HAVE_KVS_WEBRTC_SDK
    if (candidate.size() >= MAX_SIGNALING_MESSAGE_LEN) {
        if (logger) logger->error("ICE candidate too large ({} bytes, max {})",
                                  candidate.size(), MAX_SIGNALING_MESSAGE_LEN);
        return false;
    }
    SignalingMessage msg;
    MEMSET(&msg, 0, SIZEOF(SignalingMessage));
    msg.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    msg.messageType = SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE;
    STRNCPY(msg.peerClientId, peer_id.c_str(), MAX_SIGNALING_CLIENT_ID_LEN);
    STRNCPY(msg.payload, candidate.c_str(), MAX_SIGNALING_MESSAGE_LEN);
    msg.payloadLen = (UINT32) STRLEN(msg.payload);
    msg.correlationId[0] = '\0';

    STATUS status = signalingClientSendMessageSync(impl_->signaling_handle, &msg);
    if (STATUS_FAILED(status)) {
        if (logger) logger->error("Failed to send ICE candidate to peer {}, status: 0x{:08x}",
                                  peer_id, status);
        return false;
    }
    if (logger) logger->debug("Sent ICE candidate to peer: {}", peer_id);
#else
    if (logger) logger->debug("Stub: sent ICE candidate to peer: {}", peer_id);
#endif
    return true;
}

uint32_t WebRtcSignaling::get_ice_config_count() const {
    return impl_->ice_config_count();
}

bool WebRtcSignaling::get_ice_config(uint32_t index,
                                     std::vector<IceServerInfo>& servers) const {
    return impl_->ice_config(index, servers);
}

void WebRtcSignaling::log_health_status() const {
    auto logger = spdlog::get("webrtc");
    if (!logger) return;
    if (impl_->connected) {
#ifdef HAVE_KVS_WEBRTC_SDK
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - impl_->connected_at_).count();
        logger->debug("Signaling health: connected for {}s, channel={}, disconnects={}, reconnects={}",
                       duration, impl_->config.channel_name, impl_->total_disconnects_, impl_->total_reconnects_);
#else
        logger->debug("Signaling health: connected, channel={}, disconnects={}, reconnects={}",
                       impl_->config.channel_name, impl_->total_disconnects_, impl_->total_reconnects_);
#endif
    } else {
        logger->warn("Signaling health: NOT connected, channel={}, disconnects={}, reconnects={}",
                      impl_->config.channel_name, impl_->total_disconnects_, impl_->total_reconnects_);
    }
}
