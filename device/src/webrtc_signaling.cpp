// webrtc_signaling.cpp
// KVS WebRTC signaling client — pImpl with conditional compilation.
// HAVE_KVS_WEBRTC_SDK defined: real KVS WebRTC C SDK implementation
// HAVE_KVS_WEBRTC_SDK not defined: stub implementation (macOS / Linux without SDK)

#include "webrtc_signaling.h"
#include "webrtc_media.h"  // extract_sdp_summary
#include "config_util.h"  // parse_bool_field

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <optional>

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
// Conditional compilation: real SDK vs stub adapter (same runtime)
// ============================================================

#ifdef HAVE_KVS_WEBRTC_SDK
extern "C" {
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
}
#endif  // HAVE_KVS_WEBRTC_SDK

// ============================================================
// Spec 33 runtime (Task 2).
// Single shared runtime for both platforms: SignalingOwner (one resident
// worker) + ControlMailbox (bounded commands, out-of-band shutdown, merged
// state slots) + platform adapters behind the SignalingSdkOps seam.
// The legacy dual Impl (real SDK + stub, each with its own
// reconnect_thread_/needs_reconnect_/reconnect_loop) was deleted here.
// ============================================================

namespace webrtc {
namespace internal {

// ---- KvsRuntimeToken (design Component 5, req 8.3) ----
// Process-level refcount guarded by a process-level mutex. The first live
// token runs the init hook; releasing the last token runs the deinit hook.
// Client recreate holds its token across generations, so the global runtime
// is never re-initialized per recreate.

namespace {

std::mutex g_kvs_runtime_mutex;
size_t g_kvs_runtime_refcount = 0;
KvsRuntimeToken::InitHook g_kvs_runtime_init_hook;      // empty => platform default
KvsRuntimeToken::DeinitHook g_kvs_runtime_deinit_hook;  // empty => platform default

bool kvs_runtime_default_init(uint32_t& code) {
#ifdef HAVE_KVS_WEBRTC_SDK
    STATUS status = initKvsWebRtc();
    if (STATUS_FAILED(status)) {
        code = static_cast<uint32_t>(status);
        return false;
    }
#endif
    code = 0;
    return true;
}

void kvs_runtime_default_deinit() {
#ifdef HAVE_KVS_WEBRTC_SDK
    deinitKvsWebRtc();
#endif
}

}  // namespace

std::shared_ptr<KvsRuntimeToken> KvsRuntimeToken::acquire(uint32_t* error_code) {
    std::lock_guard<std::mutex> lock(g_kvs_runtime_mutex);
    if (g_kvs_runtime_refcount == 0) {
        uint32_t code = 0;
        bool ok = g_kvs_runtime_init_hook ? g_kvs_runtime_init_hook(code)
                                          : kvs_runtime_default_init(code);
        if (!ok) {
            if (error_code) *error_code = code;
            auto logger = spdlog::get("webrtc");
            if (logger) logger->error("KVS runtime init failed, code: 0x{:08x}", code);
            return nullptr;
        }
        auto logger = spdlog::get("webrtc");
        if (logger) logger->info("KVS runtime initialized (first token acquired)");
    }
    ++g_kvs_runtime_refcount;
    if (error_code) *error_code = 0;
    return std::shared_ptr<KvsRuntimeToken>(new KvsRuntimeToken());
}

KvsRuntimeToken::~KvsRuntimeToken() {
    std::lock_guard<std::mutex> lock(g_kvs_runtime_mutex);
    if (g_kvs_runtime_refcount == 0) return;  // defensive: never underflow
    --g_kvs_runtime_refcount;
    if (g_kvs_runtime_refcount == 0) {
        if (g_kvs_runtime_deinit_hook) {
            g_kvs_runtime_deinit_hook();
        } else {
            kvs_runtime_default_deinit();
        }
        auto logger = spdlog::get("webrtc");
        if (logger) logger->info("KVS runtime deinitialized (last token released)");
    }
}

void KvsRuntimeToken::set_hooks_for_test(InitHook init_hook, DeinitHook deinit_hook) {
    std::lock_guard<std::mutex> lock(g_kvs_runtime_mutex);
    g_kvs_runtime_init_hook = std::move(init_hook);
    g_kvs_runtime_deinit_hook = std::move(deinit_hook);
}

// ---- Command / ControlMailbox (design Component 2) ----

enum class CommandState { QUEUED, RUNNING, COMPLETED, CANCELLED };
enum class CommandType { CONNECT, RECONNECT, SEND_ANSWER, SEND_ICE, QUERY_ICE };

// Reply payload fulfilled through Command::reply (exactly-once by the owner).
struct CommandResult {
    bool ok = false;
    uint32_t code = 0;
    std::vector<IceServerRecord> ice;  // QUERY_ICE only
};

// Commands are queued as shared_ptr: the std::atomic member makes the struct
// non-copyable/non-movable (SHALL NOT: no emplace of atomic-bearing values).
struct Command {
    uint64_t id = 0;
    CommandType type = CommandType::CONNECT;  // SEND_* late-completion harmless; QUERY_* request/reply
    uint64_t submit_generation = 0;
    std::chrono::steady_clock::time_point completion_deadline{};  // QUERY: +2s / SEND: +10s
    std::string peer_id;
    std::string payload;
    std::atomic<CommandState> state{CommandState::QUEUED};
    std::shared_ptr<std::promise<CommandResult>> reply;
};

// Latest state observed for one generation (merged slot, no unbounded events).
struct StateEvent {
    uint64_t generation = 0;
    int state = 0;
    std::chrono::steady_clock::time_point observed_at{};
};

// Error event published by the errorReportFn bridge (RECONNECT_FAILED etc.).
struct ErrorEvent {
    uint64_t generation = 0;
    uint32_t code = 0;
    std::chrono::steady_clock::time_point observed_at{};
};

// Owner-facing mailbox. Bounded command queue (reject when full), out-of-band
// SHUTDOWN flag, per-generation merged state slots and a single error slot.
// All waits go through RuntimeClock so tests can drive a manual clock.
class ControlMailbox {
public:
    ControlMailbox(std::shared_ptr<RuntimeClock> clock, size_t command_capacity)
        : clock_(std::move(clock)), command_capacity_(command_capacity) {}

    ControlMailbox(const ControlMailbox&) = delete;
    ControlMailbox& operator=(const ControlMailbox&) = delete;

    // Bounded admission: false when full, admission closed, or after shutdown
    // (caller fails fast).
    bool push_command(std::shared_ptr<Command> cmd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_.load(std::memory_order_relaxed) || admission_closed_ ||
                commands_.size() >= command_capacity_) {
                ++rejected_;
                return false;
            }
            commands_.push_back(std::move(cmd));
        }
        cv_.notify_all();
        return true;
    }

    // Close normal command admission without full shutdown (fixed shutdown
    // order step 1: no new command may sneak in between drain and SHUTDOWN).
    void close_admission() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            admission_closed_ = true;
        }
        cv_.notify_all();
    }

    // Merged slot: only the latest state + observed_at per generation is kept.
    // Bounded because poll() drains every slot each round.
    void publish_state(uint64_t generation, int state,
                       std::chrono::steady_clock::time_point observed_at) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_slots_[generation] = StateEvent{generation, state, observed_at};
        }
        cv_.notify_all();
    }

    // Single error slot; the latest error wins (owner treats it as a trigger,
    // not as a lossless log — RECONNECT_FAILED handling is idempotent).
    void publish_error(uint64_t generation, uint32_t code,
                       std::chrono::steady_clock::time_point observed_at) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_slot_ = ErrorEvent{generation, code, observed_at};
            has_error_ = true;
        }
        cv_.notify_all();
    }

    // Out-of-band shutdown: never queued, observed before any other event.
    void request_shutdown() {
        shutdown_.store(true);
        cv_.notify_all();
    }

    // Re-arm the mailbox for a fresh owner run (connect() after disconnect()).
    // Only called while no owner worker is alive.
    void reopen() {
        std::lock_guard<std::mutex> lock(mutex_);
        admission_closed_ = false;
        shutdown_.store(false, std::memory_order_relaxed);
    }

    bool shutdown_requested() const { return shutdown_.load(); }

    struct PollResult {
        bool shutdown = false;
        std::vector<StateEvent> states;    // all merged slots, drained
        bool has_error = false;
        ErrorEvent error{};
        std::shared_ptr<Command> command;  // at most ONE per poll (anti-starvation)
    };

    // Wait until deadline (RuntimeClock-driven) for any event, then drain in
    // owner priority order: shutdown first, then states/error, then one command.
    PollResult poll(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        clock_->wait_until(cv_, lock, deadline, [this] { return has_event_locked(); });
        PollResult out;
        out.shutdown = shutdown_.load(std::memory_order_relaxed);
        out.states.reserve(state_slots_.size());
        for (const auto& [generation, event] : state_slots_) {
            (void)generation;
            out.states.push_back(event);
        }
        state_slots_.clear();
        if (has_error_) {
            out.has_error = true;
            out.error = error_slot_;
            has_error_ = false;
        }
        if (!out.shutdown && !commands_.empty()) {
            out.command = std::move(commands_.front());
            commands_.pop_front();
        }
        return out;
    }

    // Non-blocking drain of state slots and the error slot WITHOUT consuming
    // any command. Used by the owner right after a staged connect chain so
    // synchronously published CONNECTED (stub adapter) is processed before
    // the CONNECT command promise is fulfilled.
    PollResult take_events() {
        std::lock_guard<std::mutex> lock(mutex_);
        PollResult out;
        out.shutdown = shutdown_.load(std::memory_order_relaxed);
        out.states.reserve(state_slots_.size());
        for (const auto& [generation, event] : state_slots_) {
            (void)generation;
            out.states.push_back(event);
        }
        state_slots_.clear();
        if (has_error_) {
            out.has_error = true;
            out.error = error_slot_;
            has_error_ = false;
        }
        return out;
    }

    // Wake the owner without publishing an event (e.g. after option changes).
    void wake() { cv_.notify_all(); }

    // Remove and return all queued commands (shutdown path: owner cancels
    // QUEUED entries and fulfills their promises exactly once).
    std::vector<std::shared_ptr<Command>> drain_commands() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::shared_ptr<Command>> out(commands_.begin(), commands_.end());
        commands_.clear();
        return out;
    }

    size_t command_depth() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_.size();
    }

    uint64_t rejected_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejected_;
    }

private:
    bool has_event_locked() const {
        return shutdown_.load(std::memory_order_relaxed) ||
               !state_slots_.empty() || has_error_ || !commands_.empty();
    }

    std::shared_ptr<RuntimeClock> clock_;
    const size_t command_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Command>> commands_;
    std::map<uint64_t, StateEvent> state_slots_;  // generation -> latest state
    ErrorEvent error_slot_{};
    bool has_error_ = false;
    bool admission_closed_ = false;
    std::atomic<bool> shutdown_{false};
    uint64_t rejected_ = 0;
};

// ---- StubSignalingOps (fake adapter, macOS / Linux without SDK) ----
// Every staged call succeeds synchronously. connect() publishes CONNECTED
// through SignalingCallbacks::on_state before returning, preserving the
// existing stub semantics (connect -> is_connected() true).
class StubSignalingOps : public SignalingSdkOps {
public:
    explicit StubSignalingOps(std::shared_ptr<RuntimeClock> clock = nullptr)
        : clock_(std::move(clock)) {}

    SdkCallResult create(uint64_t generation, const SignalingCallbacks& cbs) override {
        generation_ = generation;
        callbacks_ = cbs;
        auto logger = spdlog::get("webrtc");
        if (logger) logger->debug("Stub signaling ops: create, generation={}", generation);
        return {};
    }

    SdkCallResult fetch() override { return {}; }

    SdkCallResult connect() override {
        if (callbacks_.on_state) {
            callbacks_.on_state(generation_, kSignalingStateConnected, now());
        }
        auto logger = spdlog::get("webrtc");
        if (logger) logger->debug("Stub signaling ops: connect, generation={}", generation_);
        return {};
    }

    SdkCallResult send_answer(std::string_view peer, std::string_view) override {
        auto logger = spdlog::get("webrtc");
        if (logger) logger->debug("Stub signaling ops: send_answer to peer: {}", std::string(peer));
        return {};
    }

    SdkCallResult send_ice(std::string_view peer, std::string_view) override {
        auto logger = spdlog::get("webrtc");
        if (logger) logger->debug("Stub signaling ops: send_ice to peer: {}", std::string(peer));
        return {};
    }

    SdkCallResult query_ice(std::vector<IceServerRecord>& out) override {
        out.clear();  // stub has no TURN servers (matches legacy stub count 0)
        return {};
    }

    SdkCallResult release() override {
        auto logger = spdlog::get("webrtc");
        if (logger) logger->debug("Stub signaling ops: release, generation={}", generation_);
        return {};
    }

private:
    std::chrono::steady_clock::time_point now() const {
        return clock_ ? clock_->now() : std::chrono::steady_clock::now();
    }

    std::shared_ptr<RuntimeClock> clock_;
    SignalingCallbacks callbacks_;
    uint64_t generation_ = 0;
};

#ifdef HAVE_KVS_WEBRTC_SDK
// ---- ProductionSignalingOps (real KVS WebRTC C SDK adapter, Linux) ----
// Hides SDK handle / credential provider behind the SignalingSdkOps seam.
// Staged calls mirror the verified spec-12/13.7 call chain. create()
// additionally registers errorReportFn (decision A3: its absence in the
// legacy code was permanent-failure path 1). Signature confirmed from SDK
// v1.18.0 Include.h: STATUS (*)(UINT64, STATUS, PCHAR, UINT32).
class ProductionSignalingOps : public SignalingSdkOps {
public:
    ProductionSignalingOps(WebRtcConfig config, AwsConfig aws_config)
        : config_(std::move(config)), aws_config_(std::move(aws_config)) {}

    ~ProductionSignalingOps() override {
        release();
        if (credential_provider_ != nullptr) {
            freeIotCredentialProvider(&credential_provider_);
            credential_provider_ = nullptr;
        }
    }

    // Factory-time validation: create the IoT credential provider eagerly so
    // invalid credentials fail WebRtcSignaling::create() fast (preserves the
    // legacy init_credential_provider semantics and the Pi 5 test skip path).
    SdkCallResult preflight() override {
        return ensure_credential_provider();
    }

    SdkCallResult create(uint64_t generation, const SignalingCallbacks& cbs) override {
        generation_ = generation;
        callbacks_ = cbs;

        // Credential provider survives recreate (token semantics live one
        // level up in KvsRuntimeToken; provider is per-adapter).
        SdkCallResult cred = ensure_credential_provider();
        if (cred.status != SdkCallStatus::OK) return cred;

        SignalingClientCallbacks callbacks;
        MEMSET(&callbacks, 0, SIZEOF(SignalingClientCallbacks));
        callbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
        callbacks.customData = reinterpret_cast<UINT64>(this);
        callbacks.stateChangeFn = on_state_changed;
        callbacks.messageReceivedFn = on_message_received;
        callbacks.errorReportFn = on_error_report;  // A3 bridge

        SignalingClientInfo client_info;
        MEMSET(&client_info, 0, SIZEOF(SignalingClientInfo));
        client_info.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
        client_info.loggingLevel = LOG_LEVEL_WARN;
        client_info.cacheFilePath = NULL;  // default cache path
        client_info.signalingClientCreationMaxRetryAttempts =
            CREATE_SIGNALING_CLIENT_RETRY_ATTEMPTS_SENTINEL_VALUE;
        STRCPY(client_info.clientId, "raspi-eye-master");

        ChannelInfo channel_info;
        MEMSET(&channel_info, 0, SIZEOF(ChannelInfo));
        channel_info.version = CHANNEL_INFO_CURRENT_VERSION;
        channel_info.pChannelName = const_cast<PCHAR>(config_.channel_name.c_str());
        channel_info.pKmsKeyId = NULL;
        channel_info.tagCount = 0;
        channel_info.pTags = NULL;
        channel_info.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
        channel_info.channelRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER;
        channel_info.cachingPolicy = SIGNALING_API_CALL_CACHE_TYPE_FILE;
        channel_info.cachingPeriod = SIGNALING_API_CALL_CACHE_TTL_SENTINEL_VALUE;
        channel_info.retry = TRUE;
        channel_info.reconnect = TRUE;  // dead config in v1.18 (Task 1); kept for parity
        channel_info.messageTtl = 0;
        channel_info.pRegion = const_cast<PCHAR>(config_.aws_region.c_str());
        channel_info.pCertPath = const_cast<PCHAR>(aws_config_.ca_path.c_str());

        STATUS status = createSignalingClientSync(
            &client_info, &channel_info, &callbacks,
            credential_provider_, &handle_);
        if (STATUS_FAILED(status)) {
            handle_ = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
            return fail("createSignalingClientSync", status);
        }
        return {};
    }

    SdkCallResult fetch() override {
        STATUS status = signalingClientFetchSync(handle_);
        if (STATUS_FAILED(status)) return fail("signalingClientFetchSync", status);
        return {};
    }

    SdkCallResult connect() override {
        STATUS status = signalingClientConnectSync(handle_);
        if (STATUS_FAILED(status)) return fail("signalingClientConnectSync", status);
        return {};
    }

    SdkCallResult send_answer(std::string_view peer, std::string_view payload) override {
        return send_message(SIGNALING_MESSAGE_TYPE_ANSWER, peer, payload);
    }

    SdkCallResult send_ice(std::string_view peer, std::string_view payload) override {
        return send_message(SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE, peer, payload);
    }

    SdkCallResult query_ice(std::vector<IceServerRecord>& out) override {
        out.clear();
        UINT32 count = 0;
        STATUS status = signalingClientGetIceConfigInfoCount(handle_, &count);
        if (STATUS_FAILED(status)) return fail("signalingClientGetIceConfigInfoCount", status);
        for (UINT32 i = 0; i < count; i++) {
            PIceConfigInfo info = nullptr;
            status = signalingClientGetIceConfigInfo(handle_, i, &info);
            if (STATUS_FAILED(status) || info == nullptr) {
                return fail("signalingClientGetIceConfigInfo", status);
            }
            for (UINT32 j = 0; j < info->uriCount; j++) {
                IceServerRecord rec;
                rec.uri = info->uris[j];
                rec.username = info->userName;
                rec.credential = info->password;
                rec.group = static_cast<uint32_t>(i);
                out.push_back(std::move(rec));
            }
        }
        return {};
    }

    SdkCallResult release() override {
        if (IS_VALID_SIGNALING_CLIENT_HANDLE(handle_)) {
            STATUS status = freeSignalingClient(&handle_);
            handle_ = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
            if (STATUS_FAILED(status)) return fail("freeSignalingClient", status);
        }
        return {};
    }

private:
    SdkCallResult ensure_credential_provider() {
        if (credential_provider_ != nullptr) return {};
        STATUS status = createLwsIotCredentialProvider(
            const_cast<PCHAR>(aws_config_.credential_endpoint.c_str()),
            const_cast<PCHAR>(aws_config_.cert_path.c_str()),
            const_cast<PCHAR>(aws_config_.key_path.c_str()),
            const_cast<PCHAR>(aws_config_.ca_path.c_str()),
            const_cast<PCHAR>(aws_config_.role_alias.c_str()),
            const_cast<PCHAR>(aws_config_.thing_name.c_str()),
            &credential_provider_);
        if (STATUS_FAILED(status)) return fail("createLwsIotCredentialProvider", status);
        auto logger = spdlog::get("webrtc");
        if (logger) logger->info("IoT credential provider initialized for thing: {}",
                                 aws_config_.thing_name);
        return {};
    }

    SdkCallResult send_message(SIGNALING_MESSAGE_TYPE type,
                               std::string_view peer, std::string_view payload) {
        auto logger = spdlog::get("webrtc");
        if (peer.size() >= MAX_SIGNALING_CLIENT_ID_LEN) {
            if (logger) logger->error("Signaling peer id too large ({} bytes, max {})",
                                      peer.size(), MAX_SIGNALING_CLIENT_ID_LEN);
            return {SdkCallStatus::FATAL, 0};
        }
        if (payload.size() >= MAX_SIGNALING_MESSAGE_LEN) {
            if (logger) logger->error("Signaling payload too large ({} bytes, max {})",
                                      payload.size(), MAX_SIGNALING_MESSAGE_LEN);
            return {SdkCallStatus::FATAL, 0};
        }
        SignalingMessage msg;
        MEMSET(&msg, 0, SIZEOF(SignalingMessage));
        msg.version = SIGNALING_MESSAGE_CURRENT_VERSION;
        msg.messageType = type;
        // string_view is not NUL-terminated: bounded copy + explicit terminator
        // (arrays are [MAX + 1], checked sizes above keep the terminator in range).
        memcpy(msg.peerClientId, peer.data(), peer.size());
        msg.peerClientId[peer.size()] = '\0';
        memcpy(msg.payload, payload.data(), payload.size());
        msg.payload[payload.size()] = '\0';
        // SHALL NOT (spec-14): payloadLen must exclude the NUL terminator.
        msg.payloadLen = (UINT32) STRLEN(msg.payload);
        msg.correlationId[0] = '\0';

        STATUS status = signalingClientSendMessageSync(handle_, &msg);
        if (STATUS_FAILED(status)) return fail("signalingClientSendMessageSync", status);
        return {};
    }

    SdkCallResult fail(const char* api, STATUS status) {
        auto logger = spdlog::get("webrtc");
        if (logger) logger->error("Signaling ops: {} failed, status: 0x{:08x}, channel: {}",
                                  api, static_cast<uint32_t>(status), config_.channel_name);
        return {SdkCallStatus::RETRYABLE, static_cast<uint32_t>(status)};
    }

    // SDK C callbacks: publish-only, wrapped in try/catch (never throw across
    // the C boundary). Business handling lives in the owner (next stage).
    static STATUS on_state_changed(UINT64 custom_data, SIGNALING_CLIENT_STATE state) {
        auto* self = reinterpret_cast<ProductionSignalingOps*>(custom_data);
        try {
            int kind = kSignalingStateOther;
            if (state == SIGNALING_CLIENT_STATE_CONNECTED) {
                kind = kSignalingStateConnected;
            } else if (state == SIGNALING_CLIENT_STATE_DISCONNECTED) {
                kind = kSignalingStateDisconnected;
            }
            if (self->callbacks_.on_state) {
                self->callbacks_.on_state(self->generation_, kind,
                                          std::chrono::steady_clock::now());
            }
        } catch (...) {
            // Swallow: exception accounting is handled by the runtime bridge.
        }
        return STATUS_SUCCESS;
    }

    static STATUS on_message_received(UINT64 custom_data, PReceivedSignalingMessage pMsg) {
        auto* self = reinterpret_cast<ProductionSignalingOps*>(custom_data);
        try {
            if (pMsg != nullptr && self->callbacks_.on_message) {
                int kind = kSignalingMsgOther;
                switch (pMsg->signalingMessage.messageType) {
                    case SIGNALING_MESSAGE_TYPE_OFFER:
                        kind = kSignalingMsgOffer;
                        break;
                    case SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE:
                        kind = kSignalingMsgIceCandidate;
                        break;
                    default:
                        break;
                }
                self->callbacks_.on_message(
                    self->generation_, kind,
                    std::string_view(pMsg->signalingMessage.peerClientId),
                    std::string_view(pMsg->signalingMessage.payload,
                                     pMsg->signalingMessage.payloadLen));
            }
        } catch (...) {
        }
        return STATUS_SUCCESS;
    }

    static STATUS on_error_report(UINT64 custom_data, STATUS status,
                                  PCHAR msg, UINT32 msg_len) {
        auto* self = reinterpret_cast<ProductionSignalingOps*>(custom_data);
        try {
            auto logger = spdlog::get("webrtc");
            if (logger) {
                logger->warn("Signaling errorReportFn: status 0x{:08x}, msg: {}",
                             static_cast<uint32_t>(status),
                             std::string(msg != nullptr ? msg : "",
                                         msg != nullptr ? msg_len : 0));
            }
            uint32_t kind = (status == STATUS_SIGNALING_RECONNECT_FAILED)
                ? kSignalingErrReconnectFailed : kSignalingErrOther;
            if (self->callbacks_.on_error) {
                self->callbacks_.on_error(self->generation_, kind,
                                          std::chrono::steady_clock::now());
            }
        } catch (...) {
        }
        return STATUS_SUCCESS;
    }

    WebRtcConfig config_;
    AwsConfig aws_config_;
    PAwsCredentialProvider credential_provider_ = nullptr;
    SIGNALING_CLIENT_HANDLE handle_ = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
    SignalingCallbacks callbacks_;
    uint64_t generation_ = 0;
};
#endif  // HAVE_KVS_WEBRTC_SDK
// ---- RateLimitedLog (Task 3) ----
// Per-category log rate limiting: the first event logs immediately, then at
// most one line per 60s window (reporting the suppressed count); a 10min
// quiet period resets back to "first logs immediately".
class RateLimitedLog {
public:
    bool acquire(uint64_t& suppressed) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mu_);
        if (armed_ && now - last_event_ >= std::chrono::minutes(10)) {
            armed_ = false;
            suppressed_ = 0;
        }
        last_event_ = now;
        if (!armed_) {
            armed_ = true;
            last_log_ = now;
            suppressed = 0;
            return true;
        }
        if (now - last_log_ >= std::chrono::seconds(60)) {
            suppressed = suppressed_;
            suppressed_ = 0;
            last_log_ = now;
            return true;
        }
        ++suppressed_;
        return false;
    }

private:
    std::mutex mu_;
    std::chrono::steady_clock::time_point last_log_{};
    std::chrono::steady_clock::time_point last_event_{};
    uint64_t suppressed_ = 0;
    bool armed_ = false;
};

// ---- MessageDispatcher (design Component 4, Task 3) ----
// Single resident worker delivering signaling messages FIFO to an immutable
// handler snapshot (same-peer OFFERs are naturally serialized by the single
// worker). Bounded queue: a new OFFER on a full queue evicts the oldest ICE;
// an all-OFFER full queue rejects the new OFFER; a new ICE on a full queue
// is dropped. Old-generation items only count as stale, never dispatch.
class MessageDispatcher {
public:
    struct Handlers {
        std::function<void(const std::string& peer, const std::string& payload)> on_offer;
        std::function<void(const std::string& peer, const std::string& payload)> on_ice;
    };

    explicit MessageDispatcher(size_t capacity) : capacity_(capacity) {}
    ~MessageDispatcher() { shutdown_and_join(); }
    MessageDispatcher(const MessageDispatcher&) = delete;
    MessageDispatcher& operator=(const MessageDispatcher&) = delete;

    void set_generation_source(const std::atomic<uint64_t>* gen) { gen_source_ = gen; }

    // Immutable snapshot swap: each dispatch copies the current shared_ptr,
    // so a running handler never observes a half-replaced handler set.
    void set_handlers(std::shared_ptr<const Handlers> handlers) {
        std::lock_guard<std::mutex> lock(mu_);
        handlers_ = std::move(handlers);
    }

    // (Re)start the worker and re-arm admission after a previous shutdown.
    void start() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (running_) return;
            admission_open_ = true;
            stop_ = false;
            running_ = true;
        }
        worker_ = std::thread([this] { run(); });
    }

    // Enqueue from the SDK callback thread. The bridge has already validated
    // sizes and generation; this only applies the overflow policy.
    bool try_enqueue(uint64_t generation, int type, std::string peer, std::string payload) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!admission_open_) {
                ++dropped_;
                return false;
            }
            if (queue_.size() >= capacity_) {
                if (type == kSignalingMsgOffer) {
                    bool evicted = false;
                    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                        if (it->type == kSignalingMsgIceCandidate) {
                            queue_.erase(it);  // evict the OLDEST ICE
                            evicted = true;
                            ++dropped_;
                            break;
                        }
                    }
                    if (!evicted) {
                        ++dropped_;  // all-OFFER queue: reject the new OFFER
                        log_overflow("offer rejected (queue full of offers)");
                        return false;
                    }
                    log_overflow("oldest ice evicted for offer");
                } else {
                    ++dropped_;  // new ICE on a full queue is dropped
                    log_overflow("ice dropped (queue full)");
                    return false;
                }
            }
            Item item;
            item.generation = generation;
            item.type = type;
            item.peer = std::move(peer);
            item.payload = std::move(payload);
            queue_.push_back(std::move(item));
        }
        cv_.notify_all();
        return true;
    }

    void close_admission() {
        std::lock_guard<std::mutex> lock(mu_);
        admission_open_ = false;
    }

    // Fixed shutdown order step (before the owner SHUTDOWN): close admission,
    // drop not-yet-started items, join the worker. An in-progress handler
    // completes; no user handler runs after this returns.
    void shutdown_and_join() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            admission_open_ = false;
            stop_ = true;
            dropped_ += queue_.size();
            queue_.clear();
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard<std::mutex> lock(mu_);
            running_ = false;
        }
    }

    size_t depth() const {
        std::lock_guard<std::mutex> lock(mu_);
        return queue_.size();
    }
    uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mu_);
        return dropped_;
    }
    uint64_t stale() const { return stale_.load(); }
    uint64_t exceptions() const { return exceptions_.load(); }

private:
    struct Item {
        uint64_t generation = 0;
        int type = 0;
        std::string peer;
        std::string payload;
    };

    void run() {
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            // Old-generation messages only count as stale (never dispatched).
            if (gen_source_ != nullptr && item.generation != gen_source_->load()) {
                stale_.fetch_add(1);
                continue;
            }
            std::shared_ptr<const Handlers> handlers;
            {
                std::lock_guard<std::mutex> lock(mu_);
                handlers = handlers_;  // immutable snapshot per dispatch
            }
            if (!handlers) continue;
            try {
                if (item.type == kSignalingMsgOffer && handlers->on_offer) {
                    handlers->on_offer(item.peer, item.payload);
                } else if (item.type == kSignalingMsgIceCandidate && handlers->on_ice) {
                    handlers->on_ice(item.peer, item.payload);
                }
            } catch (...) {
                exceptions_.fetch_add(1);
            }
        }
    }

    void log_overflow(const char* what) {
        uint64_t suppressed = 0;
        if (!overflow_log_.acquire(suppressed)) return;
        auto logger = spdlog::get("webrtc");
        if (logger) logger->warn("Signaling message queue overflow: {} (suppressed {})",
                                 what, suppressed);
    }

    const size_t capacity_;
    const std::atomic<uint64_t>* gen_source_ = nullptr;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::shared_ptr<const Handlers> handlers_;
    bool admission_open_ = false;
    bool stop_ = false;
    bool running_ = false;
    std::thread worker_;
    uint64_t dropped_ = 0;
    std::atomic<uint64_t> stale_{0};
    std::atomic<uint64_t> exceptions_{0};
    RateLimitedLog overflow_log_;
};

// ---- SignalingCallbackBridge (design Component 4, decision B) ----
// Fixed slot inside the Impl: ready before SDK registration and never freed
// while the Impl lives. Every C ABI callback entry takes an in-flight lease,
// validates open/generation, then only publishes (ControlMailbox) or
// copies + try-enqueues (MessageDispatcher). No business work, no external
// API calls, no business locks inside SDK callbacks.
class SignalingCallbackBridge {
public:
    static constexpr size_t kMaxPeerIdBytes = 256;
    static constexpr size_t kMaxPayloadBytes = 16 * 1024;

    void attach(ControlMailbox* mailbox, MessageDispatcher* dispatcher,
                std::shared_ptr<RuntimeClock> clock) {
        mailbox_ = mailbox;
        dispatcher_ = dispatcher;
        clock_ = std::move(clock);
    }

    const std::atomic<uint64_t>* generation_source() const { return &generation_; }

    // Recreate protocol: open=false -> (adapter release) -> generation++ ->
    // wait in-flight==0 -> open=true. reopen() blocks until quiescent so the
    // slot never enters the next generation with callbacks still in flight.
    void close_gate() { open_.store(false); }
    void reopen(uint64_t generation) {
        generation_.store(generation);
        while (in_flight_.load() != 0) std::this_thread::yield();
        open_.store(true);
    }

    int64_t last_signal_ns() const { return last_signal_ns_.load(); }
    uint64_t stale_events() const { return stale_.load(); }
    uint64_t exceptions() const { return exceptions_.load(); }

    // Stable callbacks handed to the adapter; they capture only this slot,
    // whose address never changes for the Impl lifetime (decision B).
    SignalingCallbacks sdk_callbacks() {
        SignalingCallbacks cbs;
        cbs.on_state = [this](uint64_t gen, int state,
                              std::chrono::steady_clock::time_point observed_at) {
            try {
                Lease lease(*this, gen);
                if (!lease.valid()) return;
                touch();
                mailbox_->publish_state(gen, state, observed_at);
            } catch (...) {
                exceptions_.fetch_add(1);  // never let C++ cross the C ABI
            }
        };
        cbs.on_message = [this](uint64_t gen, int type,
                                std::string_view peer, std::string_view payload) {
            try {
                Lease lease(*this, gen);
                if (!lease.valid()) return;
                touch();
                if (peer.size() > kMaxPeerIdBytes || payload.size() > kMaxPayloadBytes) {
                    log_oversize(peer.size(), payload.size());
                    return;
                }
                dispatcher_->try_enqueue(gen, type,
                                         std::string(peer), std::string(payload));
            } catch (...) {
                exceptions_.fetch_add(1);
            }
        };
        cbs.on_error = [this](uint64_t gen, uint32_t code,
                              std::chrono::steady_clock::time_point observed_at) {
            try {
                Lease lease(*this, gen);
                if (!lease.valid()) return;
                touch();
                mailbox_->publish_error(gen, code, observed_at);
            } catch (...) {
                exceptions_.fetch_add(1);
            }
        };
        return cbs;
    }

private:
    // In-flight lease: fetch_add FIRST, then validate open/generation; a
    // mismatch decrements and returns (counted stale, zero side effects).
    class Lease {
    public:
        Lease(SignalingCallbackBridge& bridge, uint64_t gen) : bridge_(bridge) {
            bridge_.in_flight_.fetch_add(1);
            valid_ = bridge_.open_.load() && gen == bridge_.generation_.load();
            if (!valid_) bridge_.stale_.fetch_add(1);
        }
        ~Lease() { bridge_.in_flight_.fetch_sub(1); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        bool valid() const { return valid_; }
    private:
        SignalingCallbackBridge& bridge_;
        bool valid_ = false;
    };

    void touch() {
        const auto now = clock_ ? clock_->now() : std::chrono::steady_clock::now();
        last_signal_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
    }

    void log_oversize(size_t peer_bytes, size_t payload_bytes) {
        uint64_t suppressed = 0;
        if (!oversize_log_.acquire(suppressed)) return;
        auto logger = spdlog::get("webrtc");
        if (logger) logger->warn(
            "Dropped oversize signaling message (peer {}B, payload {}B, suppressed {})",
            peer_bytes, payload_bytes, suppressed);
    }

    ControlMailbox* mailbox_ = nullptr;
    MessageDispatcher* dispatcher_ = nullptr;
    std::shared_ptr<RuntimeClock> clock_;
    std::atomic<bool> open_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<int64_t> in_flight_{0};
    std::atomic<int64_t> last_signal_ns_{0};
    std::atomic<uint64_t> stale_{0};
    std::atomic<uint64_t> exceptions_{0};
    RateLimitedLog oversize_log_;
};

// ---- SignalingOwner (design Components 2/3) ----
// Single resident worker owning every SDK handle operation. All staged calls
// (create/fetch/connect/send/query/release) run on this thread only; other
// threads communicate exclusively through the ControlMailbox. Recovery
// follows decision A3: coexist with the SDK's non-disableable single-shot
// internal reconnect (DISCONNECTED grace window), consume errorReportFn
// events, and use liveness detection as the half-open backstop.
class SignalingOwner {
public:
    enum class Phase : int {
        STOPPED = 0, CREATING, FETCHING, CONNECTING, CONNECTED, RECOVERING, STOPPING
    };

    SignalingOwner(std::shared_ptr<SignalingSdkOps> ops,
                   std::shared_ptr<RuntimeClock> clock,
                   ControlMailbox& mailbox,
                   SignalingCallbackBridge* bridge,
                   const RuntimeOptions& options,
                   std::string channel_name,
                   uint64_t start_generation)
        : ops_(std::move(ops)),
          clock_(std::move(clock)),
          mailbox_(mailbox),
          bridge_(bridge),
          options_(options),
          channel_name_(std::move(channel_name)),
          generation_(start_generation) {}

    SignalingOwner(const SignalingOwner&) = delete;
    SignalingOwner& operator=(const SignalingOwner&) = delete;

    // Safety net only: the Impl drives the fixed shutdown order (close
    // admission -> cancel queued -> out-of-band SHUTDOWN -> join).
    ~SignalingOwner() {
        if (worker_.joinable()) {
            mailbox_.request_shutdown();
            worker_.join();
        }
    }

    void start() {
        running_.store(true);
        worker_ = std::thread([this] { run(); });
    }

    // Join after SHUTDOWN was requested through the mailbox.
    void join() {
        if (worker_.joinable()) worker_.join();
        running_.store(false);
    }

    bool running() const { return running_.load(); }
    bool is_connected() const { return connected_.load(); }
    uint64_t current_generation() const { return generation_.load(); }

    std::vector<IceServerRecord> ice_cache() const {
        std::lock_guard<std::mutex> lock(ice_mutex_);
        return ice_cache_;
    }

    WebRtcSignaling::HealthSnapshot snapshot() const {
        WebRtcSignaling::HealthSnapshot s;
        s.state = phase_name(static_cast<Phase>(phase_pub_.load()));
        s.generation = generation_.load();
        s.attempt = attempt_.load();
        const auto now = clock_->now();
        if (connected_.load()) {
            s.connected_age_sec = std::chrono::duration_cast<std::chrono::seconds>(
                now - ns_to_tp(connected_at_ns_.load())).count();
        }
        const int64_t last_ns = bridge_->last_signal_ns();
        if (last_ns != 0) {
            s.last_signal_age_sec = std::chrono::duration_cast<std::chrono::seconds>(
                now - ns_to_tp(last_ns)).count();
        }
        s.command_queue_depth = mailbox_.command_depth();
        s.stale_events = stale_events_.load();
        s.commands_expired = commands_expired_.load();
        s.commands_rejected = commands_rejected_.load() + mailbox_.rejected_count();
        s.recreate_count = recreate_count_.load();
        s.total_disconnects = total_disconnects_.load();
        s.callback_exceptions = callback_exceptions_.load();
        return s;
    }

private:
    using TimePoint = std::chrono::steady_clock::time_point;

    static const char* phase_name(Phase p) {
        switch (p) {
            case Phase::STOPPED:    return "STOPPED";
            case Phase::CREATING:   return "CREATING";
            case Phase::FETCHING:   return "FETCHING";
            case Phase::CONNECTING: return "CONNECTING";
            case Phase::CONNECTED:  return "CONNECTED";
            case Phase::RECOVERING: return "RECOVERING";
            case Phase::STOPPING:   return "STOPPING";
        }
        return "UNKNOWN";
    }

    static int64_t tp_to_ns(TimePoint tp) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            tp.time_since_epoch()).count();
    }
    static TimePoint ns_to_tp(int64_t ns) {
        return TimePoint(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(ns)));
    }

    void set_phase(Phase p) {
        phase_ = p;
        phase_pub_.store(static_cast<int>(p));
    }

    void run() {
        auto logger = spdlog::get("webrtc");
        if (logger) logger->info("Signaling owner started for channel: {}", channel_name_);
        for (;;) {
            auto pr = mailbox_.poll(next_deadline());
            if (pr.shutdown) {
                handle_shutdown();
                break;
            }
            for (const auto& ev : pr.states) process_state_event(ev);
            if (pr.has_error) process_error_event(pr.error);
            process_timers();
            if (pr.command) execute_command(pr.command);
        }
        if (logger) logger->info("Signaling owner stopped for channel: {}", channel_name_);
    }

    TimePoint next_deadline() const {
        auto d = TimePoint::max();
        if (phase_ == Phase::CONNECTING) d = std::min(d, connected_deadline_);
        if (in_grace_) d = std::min(d, grace_deadline_);
        if (phase_ == Phase::RECOVERING) d = std::min(d, backoff_deadline_);
        if (connected_.load()) {
            if (attempt_.load() != 0) d = std::min(d, stable_deadline_);
            const int64_t last_ns = bridge_->last_signal_ns();
            if (last_ns != 0) d = std::min(d, ns_to_tp(last_ns) + options_.liveness_timeout);
        }
        return d;
    }

    // Staged connect for the CURRENT generation: create -> fetch -> connect.
    // Never calls connect twice on one handle; any stage failure releases the
    // handle. Success arms the CONNECTED deadline and immediately drains
    // synchronously published events (the stub publishes CONNECTED inside
    // connect()), so the caller promise observes the final state.
    bool staged_connect() {
        auto logger = spdlog::get("webrtc");
        const uint64_t gen = generation_.load();
        set_phase(Phase::CREATING);
        auto r = ops_->create(gen, bridge_->sdk_callbacks());
        if (r.status != SdkCallStatus::OK) {
            if (logger) logger->error("Staged connect failed at CREATE, gen={}, code: 0x{:08x}", gen, r.code);
            return false;  // caller always follows with fail_and_backoff (releases)
        }
        set_phase(Phase::FETCHING);
        r = ops_->fetch();
        if (r.status != SdkCallStatus::OK) {
            if (logger) logger->error("Staged connect failed at FETCH, gen={}, code: 0x{:08x}", gen, r.code);
            return false;  // caller always follows with fail_and_backoff (releases)
        }
        set_phase(Phase::CONNECTING);
        r = ops_->connect();
        if (r.status != SdkCallStatus::OK) {
            if (logger) logger->error("Staged connect failed at CONNECT, gen={}, code: 0x{:08x}", gen, r.code);
            return false;  // caller always follows with fail_and_backoff (releases)
        }
        connected_deadline_ = clock_->now() + options_.connected_deadline;
        auto ev = mailbox_.take_events();
        for (const auto& e : ev.states) process_state_event(e);
        if (ev.has_error) process_error_event(ev.error);
        return true;
    }

    void process_state_event(const StateEvent& ev) {
        if (ev.generation != generation_.load()) {
            stale_events_.fetch_add(1);
            return;
        }
        auto logger = spdlog::get("webrtc");
        if (ev.state == kSignalingStateConnected) {
            if (in_grace_) {
                // SDK internal single-shot reconnect succeeded inside the
                // grace window: recover and reset backoff (decision A3).
                in_grace_ = false;
                set_phase(Phase::CONNECTED);
                connected_.store(true);
                connected_at_ns_.store(tp_to_ns(clock_->now()));
                attempt_.store(0);
                if (logger) logger->warn("Signaling recovered within SDK reconnect grace window, gen={}", ev.generation);
                refresh_ice_cache();
            } else if (phase_ == Phase::CONNECTING) {
                if (ev.observed_at <= connected_deadline_) {
                    set_phase(Phase::CONNECTED);
                    connected_.store(true);
                    connected_at_ns_.store(tp_to_ns(clock_->now()));
                    stable_deadline_ = clock_->now() + options_.stable_connection;
                    if (logger) logger->warn("Signaling CONNECTED, gen={}, channel: {}", ev.generation, channel_name_);
                    refresh_ice_cache();
                } else {
                    // Deadline wins: observed_at arrived too late.
                    stale_events_.fetch_add(1);
                    if (logger) logger->warn("Dropped late CONNECTED (observed after deadline), gen={}", ev.generation);
                }
            } else if (phase_ == Phase::CONNECTED) {
                connected_.store(true);
            } else {
                stale_events_.fetch_add(1);
            }
        } else if (ev.state == kSignalingStateDisconnected) {
            if (phase_ == Phase::CONNECTED && !in_grace_) {
                connected_.store(false);
                total_disconnects_.fetch_add(1);
                in_grace_ = true;
                grace_deadline_ = clock_->now() + options_.sdk_reconnect_grace;
                if (logger) logger->warn(
                    "Signaling DISCONNECTED, gen={} — giving SDK internal reconnect {}s grace",
                    ev.generation,
                    std::chrono::duration_cast<std::chrono::seconds>(options_.sdk_reconnect_grace).count());
            }
        }
        // kSignalingStateOther: observability only; last_signal already refreshed.
    }

    void process_error_event(const ErrorEvent& ev) {
        if (ev.generation != generation_.load()) {
            stale_events_.fetch_add(1);
            return;
        }
        if (ev.code == kSignalingErrReconnectFailed) {
            auto logger = spdlog::get("webrtc");
            if (logger) logger->error(
                "SDK reported RECONNECT_FAILED, gen={} — releasing and scheduling recreate", ev.generation);
            in_grace_ = false;
            fail_and_backoff();
        }
        // kSignalingErrOther: logged at the adapter; no state change here.
    }

    void process_timers() {
        const auto now = clock_->now();
        auto logger = spdlog::get("webrtc");
        if (phase_ == Phase::CONNECTING && now >= connected_deadline_) {
            if (logger) logger->error(
                "CONNECTED callback did not arrive within deadline, gen={} — recovering", generation_.load());
            fail_and_backoff();
        }
        if (in_grace_ && now >= grace_deadline_) {
            in_grace_ = false;
            if (logger) logger->error(
                "SDK reconnect grace window expired, gen={} — treating as RECONNECT_FAILED", generation_.load());
            fail_and_backoff();
        }
        if (phase_ == Phase::RECOVERING && now >= backoff_deadline_) {
            recreate_count_.fetch_add(1);
            bridge_->reopen(generation_.fetch_add(1) + 1);
            if (logger) logger->warn("Recreating signaling client, gen={}, attempt={}",
                                     generation_.load(), attempt_.load());
            if (!staged_connect()) fail_and_backoff();
        }
        if (connected_.load() && attempt_.load() != 0 && now >= stable_deadline_) {
            attempt_.store(0);
            if (logger) logger->info("Signaling connection stable for {}s — backoff reset",
                std::chrono::duration_cast<std::chrono::seconds>(options_.stable_connection).count());
        }
        if (connected_.load()) {
            const int64_t last_ns = bridge_->last_signal_ns();
            if (last_ns != 0 && now - ns_to_tp(last_ns) >= options_.liveness_timeout) {
                if (logger) logger->error(
                    "No signaling callback signal within liveness timeout — half-open connection, recreating");
                connected_.store(false);
                total_disconnects_.fetch_add(1);
                in_grace_ = false;
                fail_and_backoff();
            }
        }
    }

    // Failure path shared by every trigger: release the handle immediately,
    // then schedule the next recreate after saturating backoff (constant
    // table {1,2,4,8,16,30}; SHALL NOT use 1 << attempt).
    void fail_and_backoff() {
        if (connected_.exchange(false)) total_disconnects_.fetch_add(1);
        set_phase(Phase::RECOVERING);
        const auto& sched = options_.backoff_schedule_sec;
        const size_t idx = sched.empty() ? 0
            : std::min<size_t>(attempt_.load(), sched.size() - 1);
        const int secs = sched.empty() ? 30 : sched[idx];
        backoff_deadline_ = clock_->now() + std::chrono::seconds(secs);
        attempt_.fetch_add(1);
        auto logger = spdlog::get("webrtc");
        if (logger) logger->warn("Signaling recovering: next attempt in {}s (attempt {}), channel: {}",
                                 secs, attempt_.load(), channel_name_);
        // Close the bridge gate first (open=false; in-flight leases drain),
        // then release LAST: the release call is the externally observable
        // "backoff armed" marker; the deadline is set before it is visible
        // (deterministic manual-clock tests depend on this ordering).
        bridge_->close_gate();
        ops_->release();
    }

    void refresh_ice_cache() {
        std::vector<IceServerRecord> ice;
        auto r = ops_->query_ice(ice);
        if (r.status == SdkCallStatus::OK) {
            std::lock_guard<std::mutex> lock(ice_mutex_);
            ice_cache_ = std::move(ice);
        }
    }

    void execute_command(const std::shared_ptr<Command>& cmd) {
        const auto now = clock_->now();
        auto logger = spdlog::get("webrtc");
        if (mailbox_.shutdown_requested()) {
            cancel_queued(cmd);
            return;
        }
        std::chrono::seconds max_duration{0};
        switch (cmd->type) {
            case CommandType::CONNECT:
            case CommandType::RECONNECT:
                max_duration = kConnectChainMaxDuration;
                break;
            case CommandType::SEND_ANSWER:
            case CommandType::SEND_ICE:
                max_duration = options_.send_max_duration;
                break;
            case CommandType::QUERY_ICE:
                max_duration = options_.query_max_duration;
                break;
        }
        // Admission re-check: never start an SDK call that cannot finish
        // inside the command completion deadline (SHALL NOT cross it).
        if (now + max_duration > cmd->completion_deadline) {
            commands_expired_.fetch_add(1);
            if (logger) logger->warn("Command {} expired before execution (type {})",
                                     cmd->id, static_cast<int>(cmd->type));
            fulfill(cmd, CommandResult{});
            return;
        }
        // Generation re-check for data-plane commands.
        if ((cmd->type == CommandType::SEND_ANSWER || cmd->type == CommandType::SEND_ICE ||
             cmd->type == CommandType::QUERY_ICE) &&
            cmd->submit_generation != generation_.load()) {
            commands_rejected_.fetch_add(1);
            fulfill(cmd, CommandResult{});
            return;
        }
        // QUEUED -> RUNNING; a lost CAS means the caller cancelled (QUERY
        // timeout path): zero side effects, never call the SDK.
        auto expected = CommandState::QUEUED;
        if (!cmd->state.compare_exchange_strong(expected, CommandState::RUNNING)) {
            return;
        }
        CommandResult result;
        switch (cmd->type) {
            case CommandType::CONNECT: {
                if (connected_.load()) {
                    result.ok = true;
                    break;
                }
                bridge_->close_gate();
                ops_->release();
                bridge_->reopen(generation_.fetch_add(1) + 1);
                result.ok = staged_connect();
                if (!result.ok) fail_and_backoff();
                break;
            }
            case CommandType::RECONNECT: {
                in_grace_ = false;
                if (connected_.exchange(false)) total_disconnects_.fetch_add(1);
                recreate_count_.fetch_add(1);
                bridge_->close_gate();
                ops_->release();
                bridge_->reopen(generation_.fetch_add(1) + 1);
                result.ok = staged_connect();
                if (!result.ok) fail_and_backoff();
                break;
            }
            case CommandType::SEND_ANSWER: {
                if (!connected_.load()) break;
                auto r = ops_->send_answer(cmd->peer_id, cmd->payload);
                result.ok = (r.status == SdkCallStatus::OK);
                result.code = r.code;
                break;
            }
            case CommandType::SEND_ICE: {
                if (!connected_.load()) break;
                auto r = ops_->send_ice(cmd->peer_id, cmd->payload);
                result.ok = (r.status == SdkCallStatus::OK);
                result.code = r.code;
                break;
            }
            case CommandType::QUERY_ICE: {
                auto r = ops_->query_ice(result.ice);
                result.ok = (r.status == SdkCallStatus::OK);
                result.code = r.code;
                if (result.ok) {
                    std::lock_guard<std::mutex> lock(ice_mutex_);
                    ice_cache_ = result.ice;
                }
                break;
            }
        }
        fulfill(cmd, std::move(result));
    }

    // Exactly-once completion through the atomic command state.
    static void fulfill(const std::shared_ptr<Command>& cmd, CommandResult result) {
        auto st = cmd->state.load();
        for (;;) {
            if (st == CommandState::COMPLETED || st == CommandState::CANCELLED) return;
            if (cmd->state.compare_exchange_weak(st, CommandState::COMPLETED)) break;
        }
        if (cmd->reply) {
            try { cmd->reply->set_value(std::move(result)); } catch (...) {}
        }
    }

    static void cancel_queued(const std::shared_ptr<Command>& cmd) {
        auto expected = CommandState::QUEUED;
        if (cmd->state.compare_exchange_strong(expected, CommandState::CANCELLED)) {
            if (cmd->reply) {
                try { cmd->reply->set_value(CommandResult{}); } catch (...) {}
            }
        }
    }

    void handle_shutdown() {
        set_phase(Phase::STOPPING);
        bridge_->close_gate();
        for (auto& cmd : mailbox_.drain_commands()) cancel_queued(cmd);
        in_grace_ = false;
        ops_->release();
        connected_.store(false);
        set_phase(Phase::STOPPED);
    }

    // create 10s + fetch 7s (HTTP 2s conn + 5s completion) + connect 15s (Task 1).
    static constexpr std::chrono::seconds kConnectChainMaxDuration{32};

    std::shared_ptr<SignalingSdkOps> ops_;
    std::shared_ptr<RuntimeClock> clock_;
    ControlMailbox& mailbox_;
    SignalingCallbackBridge* bridge_ = nullptr;
    RuntimeOptions options_;
    std::string channel_name_;

    std::thread worker_;
    std::atomic<bool> running_{false};

    // Owner-thread state (only touched on the worker thread).
    Phase phase_ = Phase::STOPPED;
    bool in_grace_ = false;
    TimePoint connected_deadline_{};
    TimePoint grace_deadline_{};
    TimePoint backoff_deadline_{};
    TimePoint stable_deadline_{};

    // Published state (readable from any thread).
    std::atomic<int> phase_pub_{0};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> generation_;
    std::atomic<uint32_t> attempt_{0};
    std::atomic<int64_t> connected_at_ns_{0};

    // Statistics (req 6.1).
    std::atomic<uint64_t> stale_events_{0};
    std::atomic<uint64_t> commands_expired_{0};
    std::atomic<uint64_t> commands_rejected_{0};
    std::atomic<uint64_t> recreate_count_{0};
    std::atomic<uint64_t> total_disconnects_{0};
    std::atomic<uint64_t> callback_exceptions_{0};

    mutable std::mutex ice_mutex_;
    std::vector<IceServerRecord> ice_cache_;

};

}  // namespace internal
}  // namespace webrtc

// ============================================================
// WebRtcSignaling — public pImpl on the shared spec-33 runtime.
// One implementation for both platforms; only the injected SignalingSdkOps
// adapter differs (ProductionSignalingOps vs StubSignalingOps).
// ============================================================

namespace {

namespace wi = webrtc::internal;

std::string hex32(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", v);
    return std::string(buf);
}

// Production clock: real steady_clock + condition_variable waits.
class SteadyRuntimeClock : public wi::RuntimeClock {
public:
    std::chrono::steady_clock::time_point now() const override {
        return std::chrono::steady_clock::now();
    }
    wi::WaitResult wait_until(std::condition_variable& cv,
                              std::unique_lock<std::mutex>& lock,
                              std::chrono::steady_clock::time_point deadline,
                              const std::function<bool()>& notified) override {
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            cv.wait(lock, notified);
            return wi::WaitResult::NOTIFIED;
        }
        if (cv.wait_until(lock, deadline, notified)) return wi::WaitResult::NOTIFIED;
        return wi::WaitResult::DEADLINE;
    }
};

// Monotonic seeds shared across owner restarts so a fresh owner never reuses
// generations of a previous run (stale callbacks stay stale forever).
constexpr uint64_t kGenerationSpacing = 1000000;
std::atomic<uint64_t> g_generation_seed{0};
std::atomic<uint64_t> g_command_id_seed{1};

}  // namespace

struct WebRtcSignaling::Impl {
    // Fixed callback slot (decision B): declared FIRST so it is destroyed
    // LAST — its address must stay valid until the adapter and dispatcher
    // are gone (SDK callbacks capture only this slot).
    wi::SignalingCallbackBridge bridge;

    WebRtcConfig config;
    AwsConfig aws_config;
    wi::RuntimeOptions options;
    std::shared_ptr<wi::KvsRuntimeToken> runtime_token;
    std::shared_ptr<wi::SignalingSdkOps> ops;
    std::shared_ptr<wi::RuntimeClock> clock;
    std::unique_ptr<wi::ControlMailbox> mailbox;
    std::unique_ptr<wi::MessageDispatcher> dispatcher;
    std::unique_ptr<wi::SignalingOwner> owner;

    // Serializes owner lifecycle transitions (connect/reconnect/disconnect).
    std::mutex lifecycle_mutex;

    std::mutex cb_mutex;
    OfferCallback offer_cb;
    IceCandidateCallback ice_cb;

    bool owner_running() const { return owner && owner->running(); }

    // Build and publish an immutable handler snapshot to the dispatcher.
    // Caller holds cb_mutex. Handlers capture the callback VALUES at set
    // time (snapshot semantics); only the SDP summary is logged, never the
    // full SDP / credentials / tokens.
    void publish_handlers_locked() {
        auto handlers = std::make_shared<wi::MessageDispatcher::Handlers>();
        OfferCallback ocb = offer_cb;
        IceCandidateCallback icb = ice_cb;
        handlers->on_offer = [ocb](const std::string& peer, const std::string& payload) {
            auto logger = spdlog::get("webrtc");
            if (!ocb) {
                if (logger) logger->warn(
                    "No offer callback registered, discarding offer from: {}", peer);
                return;
            }
            if (logger) {
                logger->info("Received SDP offer from peer: {}", peer);
                logger->debug("SDP summary for peer {}: {}", peer,
                              extract_sdp_summary(payload));
            }
            ocb(peer, payload);
        };
        handlers->on_ice = [icb](const std::string& peer, const std::string& payload) {
            auto logger = spdlog::get("webrtc");
            if (!icb) {
                if (logger) logger->warn(
                    "No ICE callback registered, discarding candidate from: {}", peer);
                return;
            }
            if (logger) logger->debug("Received ICE candidate from peer: {}", peer);
            icb(peer, payload);
        };
        if (dispatcher) dispatcher->set_handlers(std::move(handlers));
    }

    // Start (or restart) the owner worker. Caller holds lifecycle_mutex.
    void start_owner_locked() {
        if (owner_running()) return;
        owner.reset();          // previous run was fully joined in shutdown path
        mailbox->reopen();      // re-arm admission + shutdown flag
        dispatcher->start();    // re-arm message admission + dispatcher worker
        owner = std::make_unique<wi::SignalingOwner>(
            ops, clock, *mailbox, &bridge, options, config.channel_name,
            g_generation_seed.fetch_add(kGenerationSpacing));
        owner->start();
    }

    std::shared_ptr<wi::Command> make_command(wi::CommandType type,
                                              std::string peer,
                                              std::string payload,
                                              std::chrono::steady_clock::duration ttl) {
        auto cmd = std::make_shared<wi::Command>();
        cmd->id = g_command_id_seed.fetch_add(1);
        cmd->type = type;
        cmd->submit_generation = owner ? owner->current_generation() : 0;
        cmd->completion_deadline = clock->now() + ttl;
        cmd->peer_id = std::move(peer);
        cmd->payload = std::move(payload);
        cmd->reply = std::make_shared<std::promise<wi::CommandResult>>();
        return cmd;
    }

    // connect()/reconnect(): submit a control command and wait for the first
    // staged attempt result. true = staged API chain succeeded once; it does
    // NOT imply is_connected (only the CONNECTED callback publishes that).
    bool submit_control(wi::CommandType type, std::string* error_msg) {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        start_owner_locked();
        auto cmd = make_command(type, "", "", options.connect_attempt_wait);
        auto fut = cmd->reply->get_future();
        if (!mailbox->push_command(cmd)) {
            if (error_msg) *error_msg = "signaling command queue rejected the request";
            return false;
        }
        if (fut.wait_for(options.connect_attempt_wait) != std::future_status::ready) {
            // Do not cancel: the owner keeps recovering with saturating backoff.
            if (error_msg) *error_msg = "signaling connect attempt timed out";
            return false;
        }
        auto res = fut.get();
        if (!res.ok && error_msg) {
            *error_msg = "signaling staged connect failed, code: 0x" + hex32(res.code);
        }
        return res.ok;
    }

    // SEND commands: the caller waits at most caller_wait_timeout; on timeout
    // the command is NOT cancelled — the owner may still complete it within
    // the send completion deadline (late completion is harmless, decision D).
    bool submit_send(wi::CommandType type, const std::string& peer,
                     const std::string& payload, bool wait_for_reply) {
        auto cmd = make_command(type, peer, payload, options.send_completion_deadline);
        std::future<wi::CommandResult> fut;
        if (wait_for_reply) fut = cmd->reply->get_future();
        if (!mailbox->push_command(cmd)) {
            auto logger = spdlog::get("webrtc");
            if (logger) logger->warn("Signaling command queue full — send rejected (peer {})", peer);
            return false;
        }
        if (!wait_for_reply) return true;
        if (fut.wait_for(options.caller_wait_timeout) != std::future_status::ready) {
            auto logger = spdlog::get("webrtc");
            if (logger) logger->warn(
                "Send caller wait timed out (command may still complete), peer {}", peer);
            return false;
        }
        return fut.get().ok;
    }

    // Fixed shutdown order (design Component 2):
    // 1. close normal admission  2. (Task 3) dispatcher close/join placeholder
    // 3. cancel QUEUED commands  4. out-of-band SHUTDOWN
    // 5. owner releases + exits  6. join owner
    // (the shared runtime token is released when the Impl is destroyed)
    void shutdown_owner() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        if (!owner) return;
        mailbox->close_admission();
        // Task 3: close message admission, drop unstarted items and join the
        // dispatcher BEFORE the owner releases the client (fixed order).
        dispatcher->shutdown_and_join();
        for (auto& cmd : mailbox->drain_commands()) cancel_command(cmd);
        mailbox->request_shutdown();
        owner->join();
        owner.reset();
    }

    static void cancel_command(const std::shared_ptr<wi::Command>& cmd) {
        auto expected = wi::CommandState::QUEUED;
        if (cmd->state.compare_exchange_strong(expected, wi::CommandState::CANCELLED)) {
            if (cmd->reply) {
                try { cmd->reply->set_value(wi::CommandResult{}); } catch (...) {}
            }
        }
    }
};

// ============================================================
// WebRtcSignaling public interface
// ============================================================

WebRtcSignaling::WebRtcSignaling() = default;

WebRtcSignaling::~WebRtcSignaling() {
    if (impl_) impl_->shutdown_owner();
}

std::unique_ptr<WebRtcSignaling> WebRtcSignaling::create(
    const WebRtcConfig& config,
    const AwsConfig& aws_config,
    std::string* error_msg) {
    auto obj = std::unique_ptr<WebRtcSignaling>(new WebRtcSignaling());
    obj->impl_ = std::make_unique<Impl>();
    auto& impl = *obj->impl_;
    impl.config = config;
    impl.aws_config = aws_config;
    impl.clock = std::make_shared<SteadyRuntimeClock>();

    uint32_t code = 0;
    impl.runtime_token = wi::KvsRuntimeToken::acquire(&code);
    if (!impl.runtime_token) {
        if (error_msg) {
            *error_msg = "Failed to initialize KVS WebRTC runtime, code: 0x" + hex32(code);
        }
        return nullptr;
    }

#ifdef HAVE_KVS_WEBRTC_SDK
    impl.ops = std::make_shared<wi::ProductionSignalingOps>(config, aws_config);
#else
    impl.ops = std::make_shared<wi::StubSignalingOps>(impl.clock);
#endif

    // Factory-time validation: invalid credentials fail fast here (preserves
    // the legacy init_credential_provider semantics on Pi 5).
    auto pre = impl.ops->preflight();
    if (pre.status != wi::SdkCallStatus::OK) {
        if (error_msg) {
            *error_msg = "Failed to create IoT credential provider, status: 0x" + hex32(pre.code);
        }
        return nullptr;
    }

    impl.mailbox = std::make_unique<wi::ControlMailbox>(impl.clock, impl.options.command_capacity);
    impl.dispatcher = std::make_unique<wi::MessageDispatcher>(impl.options.message_capacity);
    impl.dispatcher->set_generation_source(impl.bridge.generation_source());
    impl.bridge.attach(impl.mailbox.get(), impl.dispatcher.get(), impl.clock);

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

std::unique_ptr<WebRtcSignaling> WebRtcSignaling::create_for_test(
    const WebRtcConfig& config,
    const AwsConfig& aws_config,
    std::shared_ptr<wi::SignalingSdkOps> ops,
    std::shared_ptr<wi::RuntimeClock> clock,
    const wi::RuntimeOptions& options,
    std::string* error_msg) {
    if (!ops || !clock) {
        if (error_msg) *error_msg = "create_for_test requires ops and clock";
        return nullptr;
    }
    auto obj = std::unique_ptr<WebRtcSignaling>(new WebRtcSignaling());
    obj->impl_ = std::make_unique<Impl>();
    auto& impl = *obj->impl_;
    impl.config = config;
    impl.aws_config = aws_config;
    impl.options = options;
    impl.clock = std::move(clock);
    impl.ops = std::move(ops);

    uint32_t code = 0;
    impl.runtime_token = wi::KvsRuntimeToken::acquire(&code);
    if (!impl.runtime_token) {
        if (error_msg) {
            *error_msg = "Failed to initialize KVS WebRTC runtime, code: 0x" + hex32(code);
        }
        return nullptr;
    }
    auto pre = impl.ops->preflight();
    if (pre.status != wi::SdkCallStatus::OK) {
        if (error_msg) *error_msg = "test ops preflight failed, code: 0x" + hex32(pre.code);
        return nullptr;
    }
    impl.mailbox = std::make_unique<wi::ControlMailbox>(impl.clock, impl.options.command_capacity);
    impl.dispatcher = std::make_unique<wi::MessageDispatcher>(impl.options.message_capacity);
    impl.dispatcher->set_generation_source(impl.bridge.generation_source());
    impl.bridge.attach(impl.mailbox.get(), impl.dispatcher.get(), impl.clock);
    return obj;
}

bool WebRtcSignaling::connect(std::string* error_msg) {
    return impl_->submit_control(wi::CommandType::CONNECT, error_msg);
}

void WebRtcSignaling::disconnect() {
    impl_->shutdown_owner();
    auto logger = spdlog::get("webrtc");
    if (logger) logger->info("Signaling client disconnected (owner stopped)");
}

bool WebRtcSignaling::is_connected() const {
    return impl_->owner && impl_->owner->is_connected();
}

bool WebRtcSignaling::reconnect(std::string* error_msg) {
    return impl_->submit_control(wi::CommandType::RECONNECT, error_msg);
}

void WebRtcSignaling::set_offer_callback(OfferCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->cb_mutex);
    impl_->offer_cb = std::move(cb);
    impl_->publish_handlers_locked();
}

void WebRtcSignaling::set_ice_candidate_callback(IceCandidateCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->cb_mutex);
    impl_->ice_cb = std::move(cb);
    impl_->publish_handlers_locked();
}

bool WebRtcSignaling::send_answer(const std::string& peer_id,
                                  const std::string& sdp_answer) {
    auto logger = spdlog::get("webrtc");
    if (!is_connected()) {
        if (logger) logger->warn("Cannot send answer: signaling not connected");
        return false;
    }
    bool ok = impl_->submit_send(wi::CommandType::SEND_ANSWER, peer_id, sdp_answer, true);
    if (ok && logger) {
        logger->info("Sent SDP answer to peer: {}", peer_id);
        logger->debug("Answer SDP summary for peer {}: {}", peer_id,
                      extract_sdp_summary(sdp_answer));
    }
    return ok;
}

bool WebRtcSignaling::send_ice_candidate(const std::string& peer_id,
                                         const std::string& candidate) {
    auto logger = spdlog::get("webrtc");
    if (!is_connected()) {
        if (logger) logger->warn("Cannot send ICE candidate: signaling not connected");
        return false;
    }
    bool ok = impl_->submit_send(wi::CommandType::SEND_ICE, peer_id, candidate, true);
    if (ok && logger) logger->debug("Sent ICE candidate to peer: {}", peer_id);
    return ok;
}

bool WebRtcSignaling::try_post_ice_candidate(const std::string& peer_id,
                                             const std::string& candidate) {
    if (!is_connected()) return false;
    return impl_->submit_send(wi::CommandType::SEND_ICE, peer_id, candidate, false);
}

uint32_t WebRtcSignaling::get_ice_config_count() const {
    if (!impl_->owner) return 0;
    auto cache = impl_->owner->ice_cache();
    uint32_t max_group = 0;
    bool any = false;
    for (const auto& rec : cache) {
        any = true;
        max_group = std::max(max_group, rec.group);
    }
    return any ? (max_group + 1) : 0;
}

bool WebRtcSignaling::get_ice_config(uint32_t index,
                                     std::vector<IceServerInfo>& servers) const {
    if (!impl_->owner) return false;
    auto cache = impl_->owner->ice_cache();
    servers.clear();
    for (const auto& rec : cache) {
        if (rec.group == index) {
            servers.push_back({rec.uri, rec.username, rec.credential});
        }
    }
    return !servers.empty();
}

WebRtcSignaling::HealthSnapshot WebRtcSignaling::health_snapshot() const {
    HealthSnapshot s;
    if (impl_->owner) {
        s = impl_->owner->snapshot();
    } else {
        s.state = "STOPPED";
    }
    s.stale_events += impl_->bridge.stale_events() +
        (impl_->dispatcher ? impl_->dispatcher->stale() : 0);
    s.callback_exceptions += impl_->bridge.exceptions() +
        (impl_->dispatcher ? impl_->dispatcher->exceptions() : 0);
    if (impl_->dispatcher) {
        s.message_queue_depth = impl_->dispatcher->depth();
        s.messages_dropped = impl_->dispatcher->dropped();
    }
    return s;
}

void WebRtcSignaling::log_health_status() const {
    auto logger = spdlog::get("webrtc");
    if (!logger) return;
    auto s = health_snapshot();
    if (is_connected()) {
        logger->debug(
            "Signaling health: state={}, gen={}, connected {}s, queue={}, disconnects={}, recreates={}",
            s.state, s.generation, s.connected_age_sec, s.command_queue_depth,
            s.total_disconnects, s.recreate_count);
    } else {
        logger->warn(
            "Signaling health: NOT connected (state={}, gen={}, attempt={}, disconnects={}, recreates={})",
            s.state, s.generation, s.attempt, s.total_disconnects, s.recreate_count);
    }
}
