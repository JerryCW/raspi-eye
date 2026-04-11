// webrtc_signaling.cpp
// KVS WebRTC signaling client — pImpl with conditional compilation.
// HAVE_KVS_WEBRTC_SDK defined: real KVS WebRTC C SDK implementation
// HAVE_KVS_WEBRTC_SDK not defined: stub implementation (macOS / Linux without SDK)

#include "webrtc_signaling.h"

#include <spdlog/spdlog.h>
#include <vector>

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

    // SDK callback: signaling client state changed
    // Forwards to C++ side — no heavy work here.
    static STATUS on_signaling_state_changed(UINT64 custom_data,
                                             SIGNALING_CLIENT_STATE state) {
        auto* self = reinterpret_cast<Impl*>(custom_data);
        auto logger = spdlog::get("pipeline");
        if (state == SIGNALING_CLIENT_STATE_CONNECTED) {
            self->connected = true;
            if (logger) logger->info("Signaling client connected to channel: {}",
                                     self->config.channel_name);
        } else if (state == SIGNALING_CLIENT_STATE_DISCONNECTED) {
            self->connected = false;
            if (logger) logger->info("Signaling client disconnected from channel: {}",
                                     self->config.channel_name);
        }
        return STATUS_SUCCESS;
    }

    // SDK callback: signaling message received
    // Forwards to registered C++ callbacks — no heavy work here.
    static STATUS on_signaling_message_received(UINT64 custom_data,
                                                PReceivedSignalingMessage pMsg) {
        auto* self = reinterpret_cast<Impl*>(custom_data);
        auto logger = spdlog::get("pipeline");
        std::string peer_id(pMsg->signalingMessage.peerClientId);

        switch (pMsg->signalingMessage.messageType) {
            case SIGNALING_MESSAGE_TYPE_OFFER:
                if (logger) logger->info("Received SDP offer from peer: {}", peer_id);
                if (self->offer_cb) {
                    std::string sdp(pMsg->signalingMessage.payload,
                                    pMsg->signalingMessage.payloadLen);
                    self->offer_cb(peer_id, sdp);
                } else {
                    if (logger) logger->warn(
                        "No offer callback registered, discarding offer from: {}",
                        peer_id);
                }
                break;
            case SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE:
                if (logger) logger->info("Received ICE candidate from peer: {}", peer_id);
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
        STATUS status = createLwsIotCredentialProvider(
            aws_config.credential_endpoint.c_str(),
            aws_config.cert_path.c_str(),
            aws_config.key_path.c_str(),
            aws_config.ca_path.c_str(),
            aws_config.role_alias.c_str(),
            aws_config.thing_name.c_str(),
            &credential_provider);

        if (STATUS_FAILED(status)) {
            if (error_msg) {
                *error_msg = "Failed to create IoT credential provider, status: 0x"
                    + to_hex(status);
            }
            return false;
        }
        auto logger = spdlog::get("pipeline");
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
        channel_info.pRegion = const_cast<PCHAR>(config.aws_region.c_str());

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

    ~Impl() {
        release_signaling_client();
        if (credential_provider != nullptr) {
            freeIotCredentialProvider(&credential_provider);
            credential_provider = nullptr;
        }
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

    bool init_credential_provider(std::string* /*error_msg*/) {
        // Stub: no real credentials needed
        return true;
    }

    bool create_and_connect(std::string* /*error_msg*/) {
        connected = true;
        auto logger = spdlog::get("pipeline");
        if (logger) logger->info("WebRTC stub connected to channel: {}",
                                 config.channel_name);
        return true;
    }

    void release_signaling_client() {
        connected = false;
    }

    ~Impl() = default;
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

    auto logger = spdlog::get("pipeline");
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
    return impl_->create_and_connect(error_msg);
}

void WebRtcSignaling::disconnect() {
    impl_->release_signaling_client();
    auto logger = spdlog::get("pipeline");
    if (logger) logger->info("Signaling client disconnected");
}

bool WebRtcSignaling::is_connected() const {
    return impl_->connected;
}

bool WebRtcSignaling::reconnect(std::string* error_msg) {
    impl_->release_signaling_client();
    return impl_->create_and_connect(error_msg);
}

void WebRtcSignaling::set_offer_callback(OfferCallback cb) {
    impl_->offer_cb = std::move(cb);
}

void WebRtcSignaling::set_ice_candidate_callback(IceCandidateCallback cb) {
    impl_->ice_cb = std::move(cb);
}

bool WebRtcSignaling::send_answer(const std::string& peer_id,
                                  const std::string& sdp_answer) {
    auto logger = spdlog::get("pipeline");
    if (!impl_->connected) {
        if (logger) logger->warn("Cannot send answer: signaling client not connected");
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
    msg.payloadLen = static_cast<UINT32>(sdp_answer.size());

    STATUS status = signalingClientSendMessageSync(impl_->signaling_handle, &msg);
    if (STATUS_FAILED(status)) {
        if (logger) logger->error("Failed to send answer to peer {}, status: 0x{:08x}",
                                  peer_id, status);
        return false;
    }
    if (logger) logger->info("Sent SDP answer to peer: {}", peer_id);
#else
    if (logger) logger->info("Stub: sent SDP answer to peer: {}", peer_id);
#endif
    return true;
}

bool WebRtcSignaling::send_ice_candidate(const std::string& peer_id,
                                         const std::string& candidate) {
    auto logger = spdlog::get("pipeline");
    if (!impl_->connected) {
        if (logger) logger->warn("Cannot send ICE candidate: signaling client not connected");
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
    msg.payloadLen = static_cast<UINT32>(candidate.size());

    STATUS status = signalingClientSendMessageSync(impl_->signaling_handle, &msg);
    if (STATUS_FAILED(status)) {
        if (logger) logger->error("Failed to send ICE candidate to peer {}, status: 0x{:08x}",
                                  peer_id, status);
        return false;
    }
    if (logger) logger->info("Sent ICE candidate to peer: {}", peer_id);
#else
    if (logger) logger->info("Stub: sent ICE candidate to peer: {}", peer_id);
#endif
    return true;
}
