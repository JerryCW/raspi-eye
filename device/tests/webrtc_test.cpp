// webrtc_test.cpp
// WebRTC signaling tests: config example/PBT + legacy stub tests (spec-13.7,
// updated to spec-33 semantics) + spec-33 runtime tests (SignalingOwner,
// bridge, dispatcher, virtual-time 72h equivalence). No fixed sleeps: manual
// clock + event gates + wait_until_true only.
#include "webrtc_signaling.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

// ============================================================
// Test helpers
// ============================================================

static std::string write_temp_toml(const std::string& content) {
    char tmpl[] = "/tmp/webrtc_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd == -1) return "";
    std::string path(tmpl);
    std::ofstream ofs(path);
    ofs << content;
    ofs.close();
    close(fd);
    return path;
}

// Busy-yield until pred is true or the real-time budget expires (event-driven
// wait on published atomics; not a fixed sleep). Shared by the legacy stub
// tests and the spec-33 runtime tests below.
static bool wait_until_true(const std::function<bool()>& pred,
                            std::chrono::milliseconds budget = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return pred();
}

// ============================================================
// Example-based tests
// ============================================================

// 1. Empty map -> build_webrtc_config returns false, error contains field names
TEST(WebRtcConfigTest, MissingSectionReturnsError) {
    std::unordered_map<std::string, std::string> empty_kv;
    WebRtcConfig config;
    std::string err;
    EXPECT_FALSE(build_webrtc_config(empty_kv, config, &err));
    EXPECT_NE(err.find("channel_name"), std::string::npos);
    EXPECT_NE(err.find("aws_region"), std::string::npos);
}

// Helper: create stub signaling, GTEST_SKIP if real SDK rejects fake creds
static std::unique_ptr<WebRtcSignaling> create_test_signaling(std::string* err = nullptr) {
    WebRtcConfig config;
    config.channel_name = "test-channel";
    config.aws_region = "us-east-1";
    AwsConfig aws_config;
    aws_config.thing_name = "test-thing";
    return WebRtcSignaling::create(config, aws_config, err);
}

// 2. Stub create + connect -> is_connected true
TEST(WebRtcSignalingTest, StubCreateAndConnect) {
    std::string err;
    auto sig = create_test_signaling(&err);
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds: " << err;

    EXPECT_TRUE(sig->connect(&err)) << "connect() failed: " << err;
    EXPECT_TRUE(sig->is_connected());
}

// 3. Stub disconnect -> is_connected false
TEST(WebRtcSignalingTest, StubDisconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";
    sig->connect();

    sig->disconnect();
    EXPECT_FALSE(sig->is_connected());
}

// 4. Send fails when not connected
TEST(WebRtcSignalingTest, SendFailsWhenNotConnected) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";
    // Do NOT connect — verify send fails
    EXPECT_FALSE(sig->send_answer("peer1", "sdp-answer"));
    EXPECT_FALSE(sig->send_ice_candidate("peer1", "ice-candidate"));
}

// 5. Stub reconnect after disconnect
TEST(WebRtcSignalingTest, StubReconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";
    sig->connect();
    EXPECT_TRUE(sig->is_connected());

    sig->disconnect();
    EXPECT_FALSE(sig->is_connected());

    EXPECT_TRUE(sig->reconnect()) << "reconnect() failed";
    EXPECT_TRUE(sig->is_connected());
}

// 6. Send succeeds when connected (stub)
TEST(WebRtcSignalingTest, SendSucceedsWhenConnected) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";
    sig->connect();
    EXPECT_TRUE(sig->is_connected());

    EXPECT_TRUE(sig->send_answer("peer1", "sdp-answer"));
    EXPECT_TRUE(sig->send_ice_candidate("peer1", "ice-candidate"));
}

// ============================================================
// Property-based tests
// ============================================================

// Property 1: WebRTC config round-trip
// Validates: Requirements 1.1, 9.1, 9.10
RC_GTEST_PROP(WebRtcConfigPBT, RoundTrip, ()) {
    // Generate non-empty ASCII strings (avoid quotes, newlines, TOML special chars)
    auto gen_ascii = rc::gen::suchThat(rc::gen::string<std::string>(), [](const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) {
            if (c < 0x20 || c > 0x7e || c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '#' || c == '[' || c == ']' || c == '=')
                return false;
        }
        return true;
    });
    auto channel = *gen_ascii;
    auto region = *gen_ascii;

    // Write TOML
    std::string toml = "[webrtc]\nchannel_name = \"" + channel + "\"\naws_region = \"" + region + "\"\n";
    auto path = write_temp_toml(toml);
    RC_ASSERT(!path.empty());

    // Parse
    auto kv = parse_toml_section(path, "webrtc");
    std::remove(path.c_str());

    WebRtcConfig config;
    std::string err;
    RC_ASSERT(build_webrtc_config(kv, config, &err));
    RC_ASSERT(config.channel_name == channel);
    RC_ASSERT(config.aws_region == region);
}

// Property 2: Missing fields detected
// Validates: Requirements 1.3, 9.3
RC_GTEST_PROP(WebRtcConfigPBT, MissingFieldsDetected, ()) {
    // All field names
    std::vector<std::string> all_fields = {"channel_name", "aws_region"};

    // Randomly select fields to remove (at least one removed)
    auto subset = *rc::gen::suchThat(
        rc::gen::container<std::vector<bool>>(all_fields.size(), rc::gen::arbitrary<bool>()),
        [](const std::vector<bool>& v) {
            bool any_removed = false;
            for (bool b : v) { if (b) any_removed = true; }
            return any_removed;  // at least one removed
        });

    std::unordered_map<std::string, std::string> kv;
    std::vector<std::string> removed;
    for (size_t i = 0; i < all_fields.size(); ++i) {
        if (subset[i]) {
            removed.push_back(all_fields[i]);
        } else {
            kv[all_fields[i]] = "some_value";
        }
    }

    WebRtcConfig config;
    std::string err;
    RC_ASSERT(!build_webrtc_config(kv, config, &err));
    for (const auto& field : removed) {
        RC_ASSERT(err.find(field) != std::string::npos);
    }
}

// ============================================================
// Preservation Property Test (spec-13.7)
// ============================================================

// Property 2: Preservation — 非断连场景行为不变
// Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7
// 在未修复代码上此测试 MUST PASS — 确认基线行为。
// spec-33 语义注记：connect()==true 表示 staged API 链成功；is_connected()
// 由 CONNECTED callback 发布。stub adapter 同步发布 CONNECTED，因此
// connect() 返回后 is_connected() 立即为 true，公共行为保持不变。
RC_GTEST_PROP(WebRtcSignalingPreservation, ConnectDisconnectSendConsistency, ()) {
    auto sig = create_test_signaling();
    if (!sig) {
        // Pi 5 上有真实 SDK，假凭证被拒——跳过整个 PBT
        RC_SUCCEED("Real SDK rejects fake creds, skipping on this platform");
        return;
    }

    // 随机生成操作序列
    enum class Op { Connect, Disconnect, SendAnswer, SendIce };
    auto len = *rc::gen::inRange(3, 15);
    auto ops = *rc::gen::container<std::vector<Op>>(
        len,
        rc::gen::element(Op::Connect, Op::Disconnect, Op::SendAnswer, Op::SendIce));

    bool expected_connected = false;
    std::string err;

    for (auto op : ops) {
        switch (op) {
            case Op::Connect:
                sig->connect(&err);
                expected_connected = true;
                break;
            case Op::Disconnect:
                sig->disconnect();
                expected_connected = false;
                break;
            case Op::SendAnswer:
                RC_ASSERT(sig->send_answer("peer1", "sdp") == expected_connected);
                break;
            case Op::SendIce:
                RC_ASSERT(sig->send_ice_candidate("peer1", "ice") == expected_connected);
                break;
        }
        RC_ASSERT(sig->is_connected() == expected_connected);
    }
}

// ============================================================
// Bug Condition Exploration Test (spec-13.7, updated for spec-33)
// ============================================================

// Bug Condition Exploration: 意外断连后自动重连恢复
// Validates: Requirements 1.1, 1.2, 1.4, 2.1, 2.2
// spec-13.7 时代此测试探测 reconnect_loop 的缺失；spec-33 之后由
// SignalingOwner 的 RECONNECT 命令（release -> recreate 下一 generation）
// 承担同一职责。语义更新：reconnect()==true 只代表 staged API 链成功，
// is_connected() 由 CONNECTED callback 发布（stub 是同步 CONNECTED，
// 行为不变）。等待改为事件驱动（wait_until_true 轮询可观测状态）。
TEST(WebRtcSignalingBugCondition, AutoReconnectAfterUnexpectedDisconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err)) << "connect() failed: " << err;
    EXPECT_TRUE(sig->is_connected());

    // 模拟意外断连恢复路径：reconnect() 公共 API 触发 owner 的全量 recreate
    // （真实场景中由 errorReportFn/grace 超时驱动同一条 fail_and_backoff 路径）
    EXPECT_TRUE(sig->reconnect(&err));

    // EXPECTED BEHAVIOR: recreate 后 CONNECTED callback 恢复连接
    EXPECT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "Expected: owner recreate restores connection after unexpected disconnect";
}

// ============================================================
// Reconnect Unit Tests (spec-13.7, Task 3.6; updated for spec-33)
// 等待方式从固定 sleep 改为事件驱动（wait_until_true 轮询 is_connected
// 等可观测条件）。语义：connect()/reconnect()==true 表示 staged API 链
// 成功，is_connected() 只由当前 generation 的 CONNECTED callback 发布；
// stub adapter 在 connect() 内同步发布 CONNECTED，行为保持不变。
// ============================================================

// Test: reconnect() 触发 owner 的全量 recreate 并恢复连接
// Validates: Requirements 2.1, 2.2
TEST(WebRtcSignalingReconnect, ReconnectTriggersAutoReconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err)) << "connect() failed: " << err;
    EXPECT_TRUE(sig->is_connected());

    // reconnect() 提交 RECONNECT 命令：owner release 旧 client 后 recreate
    EXPECT_TRUE(sig->reconnect(&err));

    EXPECT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "reconnect() should recreate the client and restore the connection";
}

// Test: disconnect() 执行固定顺序 shutdown（join owner），之后无自动重连
// Validates: Requirements 2.7
TEST(WebRtcSignalingReconnect, ShutdownPreventsReconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err));
    EXPECT_TRUE(sig->is_connected());

    // disconnect() 是同步的固定顺序 shutdown：关闭 admission -> join
    // dispatcher -> 带外 SHUTDOWN -> join owner。返回后 owner 已销毁，
    // 不存在任何可自动重连的后台线程（确定性，无需等待）。
    sig->disconnect();
    EXPECT_FALSE(sig->is_connected());
    EXPECT_EQ(sig->health_snapshot().state, "STOPPED")
        << "after disconnect() the owner must be fully stopped";

    // 有界负向观察窗口：确认没有后台路径重新发布 is_connected。
    EXPECT_FALSE(wait_until_true([&] { return sig->is_connected(); },
                                 std::chrono::milliseconds(100)))
        << "disconnect() must prevent any auto-reconnect";
    EXPECT_FALSE(sig->send_answer("peer1", "sdp"))
        << "admission must stay closed after shutdown";
}

// Test: disconnect() 安全停止 owner worker，无崩溃/悬挂
// Validates: Requirements 2.7, 3.1
TEST(WebRtcSignalingReconnect, DisconnectSafelyStopsReconnectThread) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err));

    // disconnect 应安全 join owner worker（固定 shutdown 顺序）
    sig->disconnect();
    EXPECT_FALSE(sig->is_connected());

    // 析构也应安全（无 double-join）
    sig.reset();
    // 到达此处无崩溃/悬挂即通过
}

// Test: disconnect() 后调用 reconnect() 重启 owner 并执行完整 staged connect
// Validates: Requirements 2.1, 2.2, 3.4
TEST(WebRtcSignalingReconnect, ReconnectAfterDisconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err));
    sig->disconnect();
    EXPECT_FALSE(sig->is_connected());

    // disconnect 后 owner 已 join；reconnect() 重启 owner 并执行完整
    // create/fetch/connect 链（新 generation）。
    EXPECT_TRUE(sig->reconnect(&err));
    EXPECT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "reconnect() after disconnect() should restore connection";
}

// ============================================================
// Spec 33 Task 2 — SignalingOwner runtime tests
// ManualClock + FakeSignalingOps drive the shared runtime through
// create_for_test; no fixed sleeps (event gates + manual time).
// ============================================================

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

namespace wi = webrtc::internal;

namespace {

// Manual clock: advance() moves time and wakes every registered waiter under
// the waiter's own mutex, so a waiter that checked the clock but has not yet
// entered wait() cannot miss the wakeup (no lost-wakeup race).
class ManualClock : public wi::RuntimeClock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    ManualClock() : now_ns_(1000000000LL) {}

    TimePoint now() const override {
        return TimePoint(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(now_ns_.load())));
    }

    void advance(std::chrono::steady_clock::duration d) {
        now_ns_.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
        std::vector<Waiter> snapshot;
        {
            std::lock_guard<std::mutex> lock(waiters_mutex_);
            snapshot = waiters_;
        }
        for (auto& w : snapshot) {
            std::lock_guard<std::mutex> lock(*w.mutex);
            w.cv->notify_all();
        }
    }

    wi::WaitResult wait_until(std::condition_variable& cv,
                              std::unique_lock<std::mutex>& lock,
                              TimePoint deadline,
                              const std::function<bool()>& notified) override {
        {
            std::lock_guard<std::mutex> g(waiters_mutex_);
            waiters_.push_back(Waiter{&cv, lock.mutex()});
        }
        wi::WaitResult result = wi::WaitResult::NOTIFIED;
        for (;;) {
            if (notified()) { result = wi::WaitResult::NOTIFIED; break; }
            if (deadline != TimePoint::max() && now() >= deadline) {
                result = wi::WaitResult::DEADLINE;
                break;
            }
            // 500ms slice is a defensive safety net only; the advance()
            // notify-under-mutex protocol makes lost wakeups impossible.
            cv.wait_for(lock, std::chrono::milliseconds(500));
        }
        {
            std::lock_guard<std::mutex> g(waiters_mutex_);
            for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
                if (it->cv == &cv) { waiters_.erase(it); break; }
            }
        }
        return result;
    }

private:
    struct Waiter {
        std::condition_variable* cv;
        std::mutex* mutex;
    };
    std::atomic<int64_t> now_ns_;
    std::mutex waiters_mutex_;
    std::vector<Waiter> waiters_;
};

// Fake adapter: staged failure injection, send event gate (no sleeps),
// per-call thread recording, manual-clock timestamps, callback emit helpers.
class FakeSignalingOps : public wi::SignalingSdkOps {
public:
    explicit FakeSignalingOps(std::shared_ptr<ManualClock> clock)
        : clock_(std::move(clock)) {}

    // --- failure injection (consumed one per call) ---
    std::atomic<int> create_failures{0};
    std::atomic<int> fetch_failures{0};
    std::atomic<int> connect_failures{0};
    std::atomic<bool> sync_connected{true};  // publish CONNECTED inside connect()
    std::atomic<int> gate_entered{0};        // incremented on entering a gated send

    // --- send event gate ---
    void close_send_gate() {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        gate_open_ = false;
    }
    void open_send_gate() {
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            gate_open_ = true;
        }
        gate_cv_.notify_all();
    }

    // --- recorded observations ---
    std::set<std::thread::id> call_threads() const {
        std::lock_guard<std::mutex> lock(mu_);
        return threads_;
    }
    int create_calls() const { std::lock_guard<std::mutex> l(mu_); return create_calls_; }
    int connect_calls() const { std::lock_guard<std::mutex> l(mu_); return connect_calls_; }
    int send_calls() const { std::lock_guard<std::mutex> l(mu_); return send_calls_; }
    int release_calls() const { std::lock_guard<std::mutex> l(mu_); return release_calls_; }
    std::vector<ManualClock::TimePoint> connect_times() const {
        std::lock_guard<std::mutex> lock(mu_);
        return connect_times_;
    }

    // --- callback emit helpers (as the SDK would) ---
    void emit_state(int state, ManualClock::TimePoint observed_at) {
        wi::SignalingCallbacks cbs;
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs = cbs_;
            gen = gen_;
        }
        if (cbs.on_state) cbs.on_state(gen, state, observed_at);
    }
    void emit_message(int type, std::string_view peer, std::string_view payload) {
        wi::SignalingCallbacks cbs;
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs = cbs_;
            gen = gen_;
        }
        if (cbs.on_message) cbs.on_message(gen, type, peer, payload);
    }
    // Emit with an explicit (possibly stale) generation.
    void emit_message_with_gen(uint64_t gen, int type, std::string_view peer,
                               std::string_view payload) {
        wi::SignalingCallbacks cbs;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs = cbs_;
        }
        if (cbs.on_message) cbs.on_message(gen, type, peer, payload);
    }
    uint64_t current_gen() const {
        std::lock_guard<std::mutex> lock(mu_);
        return gen_;
    }
    void emit_error(uint32_t code) {
        wi::SignalingCallbacks cbs;
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs = cbs_;
            gen = gen_;
        }
        if (cbs.on_error) cbs.on_error(gen, code, clock_->now());
    }

    // --- SignalingSdkOps ---
    wi::SdkCallResult create(uint64_t generation, const wi::SignalingCallbacks& cbs) override {
        record();
        std::lock_guard<std::mutex> lock(mu_);
        ++create_calls_;
        if (create_failures.load() > 0) {
            create_failures.fetch_sub(1);
            return {wi::SdkCallStatus::RETRYABLE, 0xF001};
        }
        gen_ = generation;
        cbs_ = cbs;
        has_handle_ = true;
        return {};
    }

    wi::SdkCallResult fetch() override {
        record();
        if (fetch_failures.load() > 0) {
            fetch_failures.fetch_sub(1);
            return {wi::SdkCallStatus::RETRYABLE, 0xF002};
        }
        return {};
    }

    wi::SdkCallResult connect() override {
        record();
        wi::SignalingCallbacks cbs;
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++connect_calls_;
            connect_times_.push_back(clock_->now());
            cbs = cbs_;
            gen = gen_;
        }
        if (connect_failures.load() > 0) {
            connect_failures.fetch_sub(1);
            return {wi::SdkCallStatus::RETRYABLE, 0xF003};
        }
        if (sync_connected.load() && cbs.on_state) {
            cbs.on_state(gen, wi::kSignalingStateConnected, clock_->now());
        }
        return {};
    }

    wi::SdkCallResult send_answer(std::string_view, std::string_view) override {
        return gated_send();
    }
    wi::SdkCallResult send_ice(std::string_view, std::string_view) override {
        return gated_send();
    }

    wi::SdkCallResult query_ice(std::vector<wi::IceServerRecord>& out) override {
        record();
        std::lock_guard<std::mutex> lock(mu_);
        out = ice_records_;
        return {};
    }

    wi::SdkCallResult release() override {
        record();
        std::lock_guard<std::mutex> lock(mu_);
        has_handle_ = false;
        ++release_calls_;
        return {};
    }

private:
    void record() {
        std::lock_guard<std::mutex> lock(mu_);
        threads_.insert(std::this_thread::get_id());
    }

    wi::SdkCallResult gated_send() {
        record();
        gate_entered.fetch_add(1);
        std::unique_lock<std::mutex> lock(gate_mutex_);
        // Event gate (bounded to avoid hanging a failed test run).
        gate_cv_.wait_for(lock, std::chrono::seconds(10), [this] { return gate_open_; });
        lock.unlock();
        std::lock_guard<std::mutex> l(mu_);
        ++send_calls_;
        return {};
    }

    std::shared_ptr<ManualClock> clock_;
    mutable std::mutex mu_;
    std::set<std::thread::id> threads_;
    int create_calls_ = 0;
    int connect_calls_ = 0;
    int send_calls_ = 0;
    int release_calls_ = 0;
    std::vector<ManualClock::TimePoint> connect_times_;
    wi::SignalingCallbacks cbs_;
    uint64_t gen_ = 0;
    bool has_handle_ = false;
    std::vector<wi::IceServerRecord> ice_records_;

    std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    bool gate_open_ = true;
};

std::unique_ptr<WebRtcSignaling> make_runtime_signaling(
    std::shared_ptr<wi::SignalingSdkOps> ops,
    std::shared_ptr<wi::RuntimeClock> clock,
    const wi::RuntimeOptions& options) {
    WebRtcConfig config;
    config.channel_name = "rt-channel";
    config.aws_region = "us-east-1";
    AwsConfig aws;
    aws.thing_name = "rt-thing";
    std::string err;
    auto sig = WebRtcSignaling::create_for_test(config, aws, std::move(ops),
                                                std::move(clock), options, &err);
    EXPECT_NE(sig, nullptr) << err;
    return sig;
}

}  // namespace

// 1. Startup with every stage failing once: first connect() returns false,
//    the owner keeps recovering with backoff and eventually connects.
// Validates: Requirements 1.1, 2.1, 2.2
TEST(SignalingRuntime, StagedFailureStartupRecovery) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    fake->create_failures = 1;
    fake->fetch_failures = 1;
    fake->connect_failures = 1;
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);

    std::string err;
    EXPECT_FALSE(sig->connect(&err)) << "first staged attempt must fail (create fails)";
    EXPECT_FALSE(sig->is_connected());

    // Backoff 1s -> attempt 2 fails at FETCH.
    ASSERT_TRUE(wait_until_true([&] { return fake->release_calls() >= 2; }));
    clock->advance(std::chrono::seconds(1));
    ASSERT_TRUE(wait_until_true([&] { return fake->release_calls() >= 3; }));
    // Backoff 2s -> attempt 3 fails at CONNECT.
    clock->advance(std::chrono::seconds(2));
    ASSERT_TRUE(wait_until_true([&] { return fake->release_calls() >= 4; }));
    // Backoff 4s -> attempt 4 succeeds end to end.
    clock->advance(std::chrono::seconds(4));
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "owner must keep recovering after a fully failed startup";
    sig->disconnect();
}

// 2. 10,000 externally triggered recoveries complete within the 15s budget.
// Validates: Requirements 2.1, 2.2, 7.2
TEST(SignalingRuntime, TenThousandRecoveriesWithinBudget) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    ASSERT_TRUE(sig->connect());
    ASSERT_TRUE(sig->is_connected());

    const auto start = std::chrono::steady_clock::now();
    constexpr int kCycles = 10000;
    for (int i = 1; i <= kCycles; ++i) {
        fake->emit_error(wi::kSignalingErrReconnectFailed);
        // fail_and_backoff released the handle: deterministic cycle marker.
        ASSERT_TRUE(wait_until_true([&] { return fake->release_calls() >= 1 + i; },
                                    std::chrono::milliseconds(1000)))
            << "cycle " << i << ": owner did not release after RECONNECT_FAILED";
        clock->advance(std::chrono::seconds(31));  // covers saturated backoff
        ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); },
                                    std::chrono::milliseconds(1000)))
            << "cycle " << i << ": owner did not reconnect";
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LE(elapsed.count(), 15) << "10,000 recovery cycles must finish within 15s";

    auto snap = sig->health_snapshot();
    EXPECT_GE(snap.recreate_count, static_cast<uint64_t>(kCycles));
    sig->disconnect();
}

// 3. CONNECTED with observed_at == deadline wins (boundary inclusive).
// Validates: Requirements 2.3
TEST(SignalingRuntime, ConnectedDeadlineBoundaryWins) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    fake->sync_connected = false;  // CONNECTED arrives only via emit_state
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);

    const auto t0 = clock->now();
    EXPECT_TRUE(sig->connect()) << "staged chain succeeds even without CONNECTED";
    EXPECT_FALSE(sig->is_connected()) << "is_connected only from the CONNECTED callback";

    // observed_at exactly equals the deadline: CONNECTED must win.
    fake->emit_state(wi::kSignalingStateConnected, t0 + options.connected_deadline);
    EXPECT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "CONNECTED with observed_at == deadline must be accepted";
    sig->disconnect();
}

// 4. CONNECTED with observed_at > deadline is dropped; deadline expiry recovers.
// Validates: Requirements 2.3
TEST(SignalingRuntime, ConnectedAfterDeadlineDropped) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    fake->sync_connected = false;
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);

    const auto t0 = clock->now();
    EXPECT_TRUE(sig->connect());
    EXPECT_FALSE(sig->is_connected());

    // observed_at one tick past the deadline: deadline wins, event dropped.
    fake->emit_state(wi::kSignalingStateConnected,
                     t0 + options.connected_deadline + std::chrono::milliseconds(1));
    ASSERT_TRUE(wait_until_true([&] { return sig->health_snapshot().stale_events >= 1; }))
        << "late CONNECTED must be counted as stale, not accepted";
    EXPECT_FALSE(sig->is_connected());

    // Deadline expiry pushes the owner into RECOVERING.
    clock->advance(options.connected_deadline + std::chrono::seconds(1));
    ASSERT_TRUE(wait_until_true([&] { return fake->release_calls() >= 2; }))
        << "deadline expiry must release the handle and start recovery";
    EXPECT_FALSE(sig->is_connected());
    sig->disconnect();
}

// 5. A send flood must not starve the control plane: RECONNECT_FAILED is
//    consumed while the command queue is still loaded.
// Validates: Requirements 1.4, 2.1
TEST(SignalingRuntime, SendFloodDoesNotStarveControlPlane) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    ASSERT_TRUE(sig->connect());
    ASSERT_TRUE(sig->is_connected());

    // Block the owner inside one gated send, then flood the queue.
    fake->close_send_gate();
    (void)sig->try_post_ice_candidate("peer-gate", "ice");  // owner blocks here
    ASSERT_TRUE(wait_until_true([&] { return fake->call_threads().size() >= 1; }));
    int flooded = 0;
    for (int i = 0; i < 200; ++i) {
        if (sig->try_post_ice_candidate("peer-flood", "ice")) ++flooded;
    }
    EXPECT_GT(flooded, 100) << "flood setup should enqueue a large batch";

    fake->emit_error(wi::kSignalingErrReconnectFailed);
    fake->open_send_gate();

    // Control plane reacts (handle released) while the flood is still queued.
    ASSERT_TRUE(wait_until_true([&] { return fake->release_calls() >= 2; }))
        << "RECONNECT_FAILED must be processed ahead of the queued send flood";
    clock->advance(std::chrono::seconds(31));
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "owner must recover despite the send flood";
    sig->disconnect();
}

// 6. SEND caller timeout does not cancel the command; it still completes
//    exactly once within the completion deadline (decision D).
// Validates: Requirements 1.2, 1.3
TEST(SignalingRuntime, SendCallerTimeoutLateCompletionExactlyOnce) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    options.caller_wait_timeout = std::chrono::seconds(0);  // caller returns immediately
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    ASSERT_TRUE(sig->connect());
    ASSERT_TRUE(sig->is_connected());

    fake->close_send_gate();
    EXPECT_FALSE(sig->send_answer("peer-1", "sdp"))
        << "caller must observe a timeout while the command is still running";
    EXPECT_EQ(fake->send_calls(), 0) << "SDK send still gated";

    fake->open_send_gate();
    ASSERT_TRUE(wait_until_true([&] { return fake->send_calls() == 1; }))
        << "the command must still complete after the caller gave up";
    // Exactly once: no duplicate execution afterwards.
    EXPECT_FALSE(wait_until_true([&] { return fake->send_calls() > 1; },
                                 std::chrono::milliseconds(100)));
    sig->disconnect();
}

// 7. A command whose completion deadline cannot be met never starts an SDK
//    call (admission re-check, SHALL NOT cross the deadline).
// Validates: Requirements 1.2, 1.5
TEST(SignalingRuntime, ExpiredCommandNeverStartsSdkCall) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    options.caller_wait_timeout = std::chrono::seconds(0);
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    ASSERT_TRUE(sig->connect());

    fake->close_send_gate();
    EXPECT_FALSE(sig->send_answer("peer-1", "sdp"));  // owner blocks in gate
    // Deterministic gate: the owner must be INSIDE the gated send (command 1
    // already admitted) before we move time past command 2's deadline.
    ASSERT_TRUE(wait_until_true([&] { return fake->gate_entered.load() >= 1; }));
    ASSERT_TRUE(sig->try_post_ice_candidate("peer-2", "ice"));  // queued behind

    // While the owner is blocked, move time past the second command deadline.
    clock->advance(options.send_completion_deadline + std::chrono::seconds(1));
    fake->open_send_gate();

    ASSERT_TRUE(wait_until_true([&] { return fake->send_calls() == 1; }));
    ASSERT_TRUE(wait_until_true([&] { return sig->health_snapshot().commands_expired >= 1; }))
        << "the expired command must be counted";
    EXPECT_FALSE(wait_until_true([&] { return fake->send_calls() > 1; },
                                 std::chrono::milliseconds(100)))
        << "the expired command must never reach the SDK";
    sig->disconnect();
}

// 8. Every SDK handle operation runs on exactly one owner thread.
// Validates: Requirements 1.1, 8.2
TEST(SignalingRuntime, SingleOwnerThreadForAllHandleOps) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);

    ASSERT_TRUE(sig->connect());
    ASSERT_TRUE(sig->is_connected());
    EXPECT_TRUE(sig->send_answer("peer-1", "sdp"));
    EXPECT_TRUE(sig->send_ice_candidate("peer-1", "ice"));
    EXPECT_TRUE(sig->reconnect());
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }));
    sig->disconnect();

    auto threads = fake->call_threads();
    EXPECT_EQ(threads.size(), 1u)
        << "create/fetch/connect/send/query/release must all run on the single owner thread";
    EXPECT_TRUE(threads.count(std::this_thread::get_id()) == 0)
        << "the test (caller) thread must never touch the SDK ops";
}

// 9. After disconnect (release), no further ops calls are made.
// Validates: Requirements 1.1, 8.4
TEST(SignalingRuntime, NoOpsCallsAfterRelease) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    ASSERT_TRUE(sig->connect());
    sig->disconnect();

    const int creates = fake->create_calls();
    const int connects = fake->connect_calls();
    const int sends = fake->send_calls();
    const int releases = fake->release_calls();

    EXPECT_FALSE(sig->send_answer("peer-1", "sdp"));
    EXPECT_FALSE(sig->try_post_ice_candidate("peer-1", "ice"));
    EXPECT_FALSE(sig->is_connected());

    EXPECT_EQ(fake->create_calls(), creates);
    EXPECT_EQ(fake->connect_calls(), connects);
    EXPECT_EQ(fake->send_calls(), sends);
    EXPECT_EQ(fake->release_calls(), releases);
}

// 10. The normal command queue is bounded at 256; overflow is rejected fast.
// Validates: Requirements 1.4
TEST(SignalingRuntime, CommandQueueBounded256) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    ASSERT_TRUE(sig->connect());

    fake->close_send_gate();
    EXPECT_TRUE(sig->try_post_ice_candidate("peer-gate", "ice"));  // owner dequeues + blocks
    ASSERT_TRUE(wait_until_true(
        [&] { return sig->health_snapshot().command_queue_depth == 0; }));

    int accepted = 0;
    for (int i = 0; i < 300; ++i) {
        if (sig->try_post_ice_candidate("peer-flood", "ice")) ++accepted;
    }
    EXPECT_EQ(accepted, 256) << "queue capacity must be exactly 256";
    EXPECT_GE(sig->health_snapshot().commands_rejected, 44u);

    fake->open_send_gate();
    sig->disconnect();
}

// 11. Backoff follows the constant table {1,2,4,8,16,30} with saturation.
// Validates: Requirements 2.2
TEST(SignalingRuntime, BackoffFollowsConstantSchedule) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    fake->connect_failures = 6;  // attempts 1..6 fail, attempt 7 succeeds
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);

    EXPECT_FALSE(sig->connect());
    ASSERT_TRUE(wait_until_true([&] { return fake->connect_calls() >= 1; }));

    const std::vector<int> expected = {1, 2, 4, 8, 16, 30};  // last = saturated
    for (size_t i = 0; i < expected.size(); ++i) {
        clock->advance(std::chrono::seconds(expected[i]));
        const int want = static_cast<int>(i) + 2;
        ASSERT_TRUE(wait_until_true([&] { return fake->connect_calls() >= want; }))
            << "attempt " << want << " did not run after " << expected[i] << "s backoff";
    }
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }));

    auto times = fake->connect_times();
    ASSERT_EQ(times.size(), 7u);
    for (size_t i = 0; i + 1 < times.size(); ++i) {
        const auto delta = std::chrono::duration_cast<std::chrono::seconds>(
            times[i + 1] - times[i]).count();
        EXPECT_EQ(delta, expected[i])
            << "backoff step " << i << " must follow the constant schedule";
    }
    sig->disconnect();
}

// 12. Decision A3: CONNECTED inside the SDK reconnect grace window recovers
//     the session and resets backoff without a recreate.
// Validates: Requirements 2.4
TEST(SignalingRuntime, GraceWindowRecoveryResetsBackoff) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    fake->connect_failures = 1;  // build up attempt != 0 first
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);

    EXPECT_FALSE(sig->connect());
    clock->advance(std::chrono::seconds(1));
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }));
    EXPECT_GE(sig->health_snapshot().attempt, 1u) << "attempt not yet reset (no stable window)";

    const uint64_t recreates_before = sig->health_snapshot().recreate_count;
    fake->emit_state(wi::kSignalingStateDisconnected, clock->now());
    ASSERT_TRUE(wait_until_true([&] { return !sig->is_connected(); }));
    ASSERT_TRUE(wait_until_true([&] { return sig->health_snapshot().total_disconnects >= 1; }));

    // SDK internal reconnect succeeds inside the 20s grace window.
    clock->advance(std::chrono::seconds(5));
    fake->emit_state(wi::kSignalingStateConnected, clock->now());
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "same-generation CONNECTED inside the grace window must recover";
    EXPECT_EQ(sig->health_snapshot().attempt, 0u) << "grace recovery must reset backoff";
    EXPECT_EQ(sig->health_snapshot().recreate_count, recreates_before)
        << "grace recovery must not recreate the client";
    sig->disconnect();
}

// 13. Decision A3: liveness timeout (half-open connection) triggers recreate.
// Validates: Requirements 2.5
TEST(SignalingRuntime, LivenessTimeoutTriggersRecreate) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    options.liveness_timeout = std::chrono::seconds(60);
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    ASSERT_TRUE(sig->connect());
    ASSERT_TRUE(sig->is_connected());

    // No callback signal at all for longer than the liveness timeout.
    clock->advance(std::chrono::seconds(61));
    ASSERT_TRUE(wait_until_true([&] { return fake->release_calls() >= 2; }))
        << "liveness violation must release the half-open client";
    clock->advance(std::chrono::seconds(2));
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "owner must recreate and reconnect after the half-open detection";
    auto snap = sig->health_snapshot();
    EXPECT_GE(snap.recreate_count, 1u);
    EXPECT_GE(snap.total_disconnects, 1u);
    sig->disconnect();
}

// 14. Shared KVS runtime token: init exactly once for the first token, deinit
//     only after the last token is released; recreates never re-init.
// Validates: Requirements 8.3
TEST(SignalingRuntime, RuntimeTokenRefcount) {
    static std::atomic<int> init_count{0};
    static std::atomic<int> deinit_count{0};
    init_count = 0;
    deinit_count = 0;
    wi::KvsRuntimeToken::set_hooks_for_test(
        [](uint32_t& code) { code = 0; init_count.fetch_add(1); return true; },
        [] { deinit_count.fetch_add(1); });

    {
        auto clock = std::make_shared<ManualClock>();
        auto fake_a = std::make_shared<FakeSignalingOps>(clock);
        auto fake_b = std::make_shared<FakeSignalingOps>(clock);
        wi::RuntimeOptions options;
        auto sig_a = make_runtime_signaling(fake_a, clock, options);
        ASSERT_NE(sig_a, nullptr);
        EXPECT_EQ(init_count.load(), 1);

        auto sig_b = make_runtime_signaling(fake_b, clock, options);
        ASSERT_NE(sig_b, nullptr);
        EXPECT_EQ(init_count.load(), 1) << "second token must not re-init";

        // Client recreate cycles never re-init/deinit the shared runtime.
        ASSERT_TRUE(sig_a->connect());
        EXPECT_TRUE(sig_a->reconnect());
        ASSERT_TRUE(wait_until_true([&] { return sig_a->is_connected(); }));
        EXPECT_EQ(init_count.load(), 1);
        EXPECT_EQ(deinit_count.load(), 0);

        sig_a->disconnect();
        sig_a.reset();
        EXPECT_EQ(deinit_count.load(), 0) << "deinit only after the LAST token";
        sig_b.reset();
        EXPECT_EQ(deinit_count.load(), 1);
    }

    // Restore platform default hooks for the remaining tests.
    wi::KvsRuntimeToken::set_hooks_for_test(nullptr, nullptr);
}

// 15. Cancelled QUEUED commands never execute (zero side effects): the fixed
//     shutdown order drains and cancels the queue before SHUTDOWN, and the
//     owner's QUEUED->RUNNING CAS refuses commands cancelled by callers.
// Validates: Requirements 1.2, 1.3, 1.5
TEST(SignalingRuntime, CancelledQueuedCommandNeverExecutes) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    options.caller_wait_timeout = std::chrono::seconds(0);
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    ASSERT_TRUE(sig->connect());

    // Owner blocks inside command 1; commands 2..N stay QUEUED.
    fake->close_send_gate();
    EXPECT_FALSE(sig->send_answer("peer-1", "sdp"));
    ASSERT_TRUE(wait_until_true([&] { return fake->gate_entered.load() >= 1; }));
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(sig->try_post_ice_candidate("peer-q", "ice"));
    }

    // disconnect() drains + cancels every QUEUED command, then shuts down.
    // Deterministic ordering: the helper opens the gate only AFTER the drain
    // emptied the queue (depth==0 happens-before gate open), so the owner can
    // never dequeue a command that was destined for cancellation.
    std::thread opener([&] {
        wait_until_true([&] { return sig->health_snapshot().command_queue_depth == 0; },
                        std::chrono::milliseconds(5000));
        fake->open_send_gate();
    });
    sig->disconnect();
    opener.join();

    EXPECT_EQ(fake->send_calls(), 1)
        << "cancelled QUEUED commands must never reach the SDK (zero side effects)";
    EXPECT_FALSE(sig->is_connected());
}

// ============================================================
// Spec 33 Task 3 — SignalingCallbackBridge + MessageDispatcher tests
// ============================================================

namespace {

// Shared handler gate + delivery recorder for dispatcher tests.
struct HandlerProbe {
    std::mutex mu;
    std::condition_variable cv;
    bool gate_open = true;
    std::vector<std::string> delivered;          // "type:peer:payload"
    std::set<std::thread::id> handler_threads;
    std::atomic<int> started{0};

    void close_gate() {
        std::lock_guard<std::mutex> lock(mu);
        gate_open = false;
    }
    void open_gate() {
        {
            std::lock_guard<std::mutex> lock(mu);
            gate_open = true;
        }
        cv.notify_all();
    }
    void on(const char* type, const std::string& peer, const std::string& payload) {
        started.fetch_add(1);
        std::unique_lock<std::mutex> lock(mu);
        handler_threads.insert(std::this_thread::get_id());
        cv.wait_for(lock, std::chrono::seconds(10), [this] { return gate_open; });
        delivered.push_back(std::string(type) + ":" + peer + ":" + payload);
    }
    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return delivered;
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(mu);
        return delivered.size();
    }
    bool has(const std::string& entry) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& d : delivered) {
            if (d == entry) return true;
        }
        return false;
    }
};

}  // namespace

// 16. Structural non-blocking: the SDK callback thread never runs the
//     handler; messages queue while the handler is blocked; FIFO delivery.
// Validates: Requirements 3.1, 3.2, 3.4
TEST(SignalingDispatcher, SdkCallbackNeverRunsHandlerAndFifo) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    auto probe = std::make_shared<HandlerProbe>();
    sig->set_offer_callback([probe](const std::string& p, const std::string& s) {
        probe->on("offer", p, s);
    });
    ASSERT_TRUE(sig->connect());

    probe->close_gate();
    // emit_message runs the C-ABI callback on THIS thread; it must return
    // immediately (enqueue only) even though the handler is blocked.
    fake->emit_message(wi::kSignalingMsgOffer, "p1", "sdp1");
    // Reaching this line while the handler is gated proves the callback did
    // not execute the handler inline.
    ASSERT_TRUE(wait_until_true([&] { return probe->started.load() >= 1; }));
    for (int i = 2; i <= 5; ++i) {
        fake->emit_message(wi::kSignalingMsgOffer, "p" + std::to_string(i), "sdp");
    }
    EXPECT_GE(sig->health_snapshot().message_queue_depth, 3u)
        << "messages must queue while the handler is blocked";

    probe->open_gate();
    ASSERT_TRUE(wait_until_true([&] { return probe->count() == 5; }));
    auto delivered = probe->snapshot();
    ASSERT_EQ(delivered.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(delivered[i], "offer:p" + std::to_string(i + 1) + ":" +
                                (i == 0 ? "sdp1" : "sdp")) << "FIFO order violated at " << i;
    }
    // Structural assertion: handler thread is never the SDK callback thread.
    EXPECT_EQ(probe->handler_threads.size(), 1u);
    EXPECT_EQ(probe->handler_threads.count(std::this_thread::get_id()), 0u)
        << "the SDK callback (emit) thread must never run the business handler";
    sig->disconnect();
}

// 17. Overflow policy at capacity 512: OFFER evicts the oldest ICE; a new
//     ICE on a full queue is dropped; depth never exceeds 512.
// Validates: Requirements 3.3
TEST(SignalingDispatcher, OverflowOfferEvictsOldestIce) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;  // message_capacity = 512
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    auto probe = std::make_shared<HandlerProbe>();
    sig->set_offer_callback([probe](const std::string& p, const std::string& s) {
        probe->on("offer", p, s);
    });
    sig->set_ice_candidate_callback([probe](const std::string& p, const std::string& s) {
        probe->on("ice", p, s);
    });
    ASSERT_TRUE(sig->connect());

    probe->close_gate();
    fake->emit_message(wi::kSignalingMsgOffer, "gate", "sdp");  // occupies the worker
    ASSERT_TRUE(wait_until_true([&] { return probe->started.load() >= 1; }));

    for (int i = 0; i < 512; ++i) {  // fill the queue exactly to capacity
        fake->emit_message(wi::kSignalingMsgIceCandidate, "ice-" + std::to_string(i), "c");
    }
    EXPECT_EQ(sig->health_snapshot().message_queue_depth, 512u);

    // 513th message: OFFER evicts the OLDEST ICE (ice-0), depth stays 512.
    fake->emit_message(wi::kSignalingMsgOffer, "late", "sdp");
    EXPECT_EQ(sig->health_snapshot().message_queue_depth, 512u);
    // New ICE on a full queue is dropped.
    fake->emit_message(wi::kSignalingMsgIceCandidate, "lost", "c");
    EXPECT_EQ(sig->health_snapshot().message_queue_depth, 512u);
    EXPECT_GE(sig->health_snapshot().messages_dropped, 2u);

    probe->open_gate();
    ASSERT_TRUE(wait_until_true([&] { return probe->count() == 513; },
                                std::chrono::milliseconds(5000)));
    EXPECT_FALSE(probe->has("ice:ice-0:c")) << "oldest ICE must have been evicted";
    EXPECT_FALSE(probe->has("ice:lost:c")) << "new ICE on a full queue must be dropped";
    EXPECT_TRUE(probe->has("offer:late:sdp")) << "the OFFER must have been accepted";
    sig->disconnect();
}

// 18. All-OFFER full queue rejects the new OFFER (small capacity variant).
// Validates: Requirements 3.3
TEST(SignalingDispatcher, AllOfferQueueRejectsNewOffer) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    options.message_capacity = 4;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    auto probe = std::make_shared<HandlerProbe>();
    sig->set_offer_callback([probe](const std::string& p, const std::string& s) {
        probe->on("offer", p, s);
    });
    ASSERT_TRUE(sig->connect());

    probe->close_gate();
    fake->emit_message(wi::kSignalingMsgOffer, "o-gate", "sdp");
    ASSERT_TRUE(wait_until_true([&] { return probe->started.load() >= 1; }));
    for (int i = 0; i < 4; ++i) {
        fake->emit_message(wi::kSignalingMsgOffer, "o" + std::to_string(i), "sdp");
    }
    EXPECT_EQ(sig->health_snapshot().message_queue_depth, 4u);
    fake->emit_message(wi::kSignalingMsgOffer, "o-rejected", "sdp");
    EXPECT_EQ(sig->health_snapshot().message_queue_depth, 4u);

    probe->open_gate();
    ASSERT_TRUE(wait_until_true([&] { return probe->count() == 5; }));
    EXPECT_FALSE(probe->has("offer:o-rejected:sdp"))
        << "an all-OFFER full queue must reject the new OFFER";
    sig->disconnect();
}

// 19. A throwing handler is contained: counted, dispatcher survives.
// Validates: Requirements 3.2, 4.2
TEST(SignalingDispatcher, HandlerExceptionContained) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    auto probe = std::make_shared<HandlerProbe>();
    std::atomic<int> calls{0};
    sig->set_offer_callback([probe, &calls](const std::string& p, const std::string& s) {
        if (calls.fetch_add(1) == 0) throw std::runtime_error("handler boom");
        probe->on("offer", p, s);
    });
    ASSERT_TRUE(sig->connect());

    fake->emit_message(wi::kSignalingMsgOffer, "boom", "sdp");
    fake->emit_message(wi::kSignalingMsgOffer, "ok", "sdp");
    ASSERT_TRUE(wait_until_true([&] { return probe->count() == 1; }))
        << "the dispatcher must survive a throwing handler";
    EXPECT_TRUE(probe->has("offer:ok:sdp"));
    EXPECT_GE(sig->health_snapshot().callback_exceptions, 1u);
    sig->disconnect();
}

// 20. Old-generation messages only count as stale (never dispatched).
// Validates: Requirements 2.3, 3.4, 4.1
TEST(SignalingDispatcher, OldGenerationMessageOnlyStale) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    auto probe = std::make_shared<HandlerProbe>();
    sig->set_offer_callback([probe](const std::string& p, const std::string& s) {
        probe->on("offer", p, s);
    });
    ASSERT_TRUE(sig->connect());
    const uint64_t old_gen = fake->current_gen();

    // Queue a message behind a gated one, then recreate (generation changes).
    probe->close_gate();
    fake->emit_message(wi::kSignalingMsgOffer, "m1", "sdp");
    ASSERT_TRUE(wait_until_true([&] { return probe->started.load() >= 1; }));
    fake->emit_message(wi::kSignalingMsgOffer, "m2", "sdp");  // queued, old gen

    ASSERT_TRUE(sig->reconnect());  // recreate: generation increments
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }));
    const uint64_t stale_before = sig->health_snapshot().stale_events;

    probe->open_gate();
    ASSERT_TRUE(wait_until_true([&] { return probe->count() >= 1; }));
    // m2 must never dispatch (old generation), only counted stale.
    EXPECT_FALSE(wait_until_true([&] { return probe->has("offer:m2:sdp"); },
                                 std::chrono::milliseconds(200)));
    ASSERT_TRUE(wait_until_true([&] {
        return sig->health_snapshot().stale_events > stale_before;
    })) << "old-generation message must be counted stale";

    // Bridge-entry rejection: an emit stamped with the OLD generation is
    // rejected at the lease check (never enqueued).
    const auto depth_before = sig->health_snapshot().message_queue_depth;
    fake->emit_message_with_gen(old_gen, wi::kSignalingMsgOffer, "m3", "sdp");
    EXPECT_EQ(sig->health_snapshot().message_queue_depth, depth_before);
    EXPECT_FALSE(wait_until_true([&] { return probe->has("offer:m3:sdp"); },
                                 std::chrono::milliseconds(100)));
    sig->disconnect();
}

// 21. Handler replacement uses immutable snapshots: after the swap, new
//     messages go only to the new handler.
// Validates: Requirements 3.4
TEST(SignalingDispatcher, HandlerReplacementSnapshot) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    auto probe_a = std::make_shared<HandlerProbe>();
    auto probe_b = std::make_shared<HandlerProbe>();
    sig->set_offer_callback([probe_a](const std::string& p, const std::string& s) {
        probe_a->on("offer", p, s);
    });
    ASSERT_TRUE(sig->connect());

    fake->emit_message(wi::kSignalingMsgOffer, "m1", "sdp");
    ASSERT_TRUE(wait_until_true([&] { return probe_a->count() == 1; }));

    sig->set_offer_callback([probe_b](const std::string& p, const std::string& s) {
        probe_b->on("offer", p, s);
    });
    fake->emit_message(wi::kSignalingMsgOffer, "m2", "sdp");
    ASSERT_TRUE(wait_until_true([&] { return probe_b->count() == 1; }));
    EXPECT_TRUE(probe_b->has("offer:m2:sdp"));
    EXPECT_EQ(probe_a->count(), 1u) << "the old handler must not receive post-swap messages";
    sig->disconnect();
}

// 22. Shutdown drops unstarted messages, joins the dispatcher, and never
//     calls a user handler afterwards; oversize messages are dropped early.
// Validates: Requirements 3.1, 3.3, 4.1
TEST(SignalingDispatcher, ShutdownDropsQueuedAndOversizeRejected) {
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);
    auto probe = std::make_shared<HandlerProbe>();
    sig->set_offer_callback([probe](const std::string& p, const std::string& s) {
        probe->on("offer", p, s);
    });
    ASSERT_TRUE(sig->connect());

    // Oversize validation happens before any allocation/enqueue.
    const std::string big_peer(300, 'p');
    const std::string big_payload(17 * 1024, 'x');
    fake->emit_message(wi::kSignalingMsgOffer, big_peer, "sdp");
    fake->emit_message(wi::kSignalingMsgOffer, "p-big", big_payload);
    fake->emit_message(wi::kSignalingMsgOffer, "p-ok", "sdp");
    ASSERT_TRUE(wait_until_true([&] { return probe->count() == 1; }));
    EXPECT_TRUE(probe->has("offer:p-ok:sdp"));

    // Queue messages behind a gated one, then disconnect: unstarted items
    // are dropped and no handler runs after join.
    probe->close_gate();
    fake->emit_message(wi::kSignalingMsgOffer, "g1", "sdp");
    ASSERT_TRUE(wait_until_true([&] { return probe->started.load() >= 2; }));
    for (int i = 0; i < 4; ++i) {
        fake->emit_message(wi::kSignalingMsgOffer, "q" + std::to_string(i), "sdp");
    }
    std::thread opener([&] {
        wait_until_true([&] { return sig->health_snapshot().message_queue_depth == 0; },
                        std::chrono::milliseconds(5000));
        probe->open_gate();
    });
    sig->disconnect();
    opener.join();

    EXPECT_EQ(probe->count(), 2u)
        << "only p-ok and the in-progress g1 may complete; queued q0..q3 are dropped";
    // After shutdown the bridge gate is closed: further emits do nothing.
    fake->emit_message(wi::kSignalingMsgOffer, "after", "sdp");
    EXPECT_FALSE(wait_until_true([&] { return probe->has("offer:after:sdp"); },
                                 std::chrono::milliseconds(100)));
}

// ============================================================
// Spec 33 Task 6 — virtual-time 72h equivalence
// ============================================================

// 23. Virtual 72h equivalence (design Property 8): drive the full runtime
// through 72 hours of manual-clock time with periodic normal signal
// (2-hourly SDK-like state heartbeat = ICE config refresh analogue, hourly
// application message, one transient DISCONNECTED recovered inside the SDK
// reconnect grace window, one 3h signal blackout detected by liveness).
// Asserts: stable-window recreate rate == 0 (no false-positive recreate and
// no false liveness trigger while signal is present), half-open detection
// recovers after 3h of silence, all counters are monotonic and bounded,
// queues stay within capacity, and shutdown is clean afterwards. Runs in
// seconds of real time (manual clock only, no fixed sleeps).
// **Validates: Requirements 2.5, 6.4, 7.4, 8.4** (Property 8)
TEST(SignalingRuntime, VirtualTime72HourEquivalence) {
    const auto real_start = std::chrono::steady_clock::now();
    auto clock = std::make_shared<ManualClock>();
    auto fake = std::make_shared<FakeSignalingOps>(clock);
    wi::RuntimeOptions options;  // defaults: liveness 3h, grace 20s, backoff {1..30}
    auto sig = make_runtime_signaling(fake, clock, options);
    ASSERT_NE(sig, nullptr);

    // Hourly application traffic delivered through the dispatcher.
    std::atomic<int> ice_delivered{0};
    sig->set_ice_candidate_callback([&](const std::string&, const std::string&) {
        ice_delivered.fetch_add(1);
    });

    ASSERT_TRUE(sig->connect());
    ASSERT_TRUE(sig->is_connected());

    // Monotonicity + bounds checked on every 10-minute step.
    WebRtcSignaling::HealthSnapshot prev = sig->health_snapshot();
    int hours_elapsed = 0;
    int ice_sent = 0;
    auto check_step = [&] {
        auto s = sig->health_snapshot();
        ASSERT_GE(s.recreate_count, prev.recreate_count) << "counter must be monotonic";
        ASSERT_GE(s.total_disconnects, prev.total_disconnects) << "counter must be monotonic";
        ASSERT_GE(s.stale_events, prev.stale_events) << "counter must be monotonic";
        ASSERT_GE(s.messages_dropped, prev.messages_dropped) << "counter must be monotonic";
        ASSERT_GE(s.commands_expired, prev.commands_expired) << "counter must be monotonic";
        ASSERT_LE(s.command_queue_depth, options.command_capacity) << "queue must stay bounded";
        ASSERT_LE(s.message_queue_depth, options.message_capacity) << "queue must stay bounded";
        prev = s;
    };

    // One virtual hour = 6 x 10min steps. The SDK-like heartbeat every 2h
    // keeps the observed signal gap well under the 3h liveness timeout, so
    // liveness must never fire while signal is present.
    auto run_stable_hours = [&](int hours) {
        for (int h = 0; h < hours; ++h) {
            for (int step = 0; step < 6; ++step) {
                clock->advance(std::chrono::minutes(10));
                check_step();
                ASSERT_TRUE(sig->is_connected())
                    << "connection must stay up in a stable window (hour "
                    << hours_elapsed << ")";
            }
            ++hours_elapsed;
            if (hours_elapsed % 2 == 0) {
                fake->emit_state(wi::kSignalingStateOther, clock->now());
            }
            fake->emit_message(wi::kSignalingMsgIceCandidate, "viewer-hb", "candidate");
            ++ice_sent;
        }
    };

    // ---- Phase A: 24h stable network ----
    run_stable_hours(24);
    EXPECT_EQ(sig->health_snapshot().recreate_count, 0u)
        << "stable-window recreate rate must be 0 (no false-positive recreate)";

    // ---- Phase B: transient DISCONNECTED, SDK internal reconnect succeeds
    //      inside the 20s grace window (recovery without recreate) ----
    fake->emit_state(wi::kSignalingStateDisconnected, clock->now());
    ASSERT_TRUE(wait_until_true([&] { return !sig->is_connected(); }));
    clock->advance(std::chrono::seconds(5));
    fake->emit_state(wi::kSignalingStateConnected, clock->now());
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "same-generation CONNECTED inside the grace window must recover";
    EXPECT_EQ(sig->health_snapshot().recreate_count, 0u)
        << "grace-window recovery must not recreate the client";
    EXPECT_GE(sig->health_snapshot().total_disconnects, 1u);
    check_step();

    // ---- Phase C: 24 more stable hours ----
    run_stable_hours(24);
    EXPECT_EQ(sig->health_snapshot().recreate_count, 0u)
        << "48h of stable operation must not produce any recreate";
    ASSERT_TRUE(wait_until_true([&] { return ice_delivered.load() == ice_sent; }))
        << "all hourly messages must be dispatched (sent " << ice_sent << ")";

    // ---- Phase D: total signal blackout -> half-open (liveness) detection
    //      after 3h, then recreate + reconnect ----
    const int releases_before = fake->release_calls();
    for (int step = 0; step < 19; ++step) {  // 3h10m of silence (>= 3h timeout)
        clock->advance(std::chrono::minutes(10));
    }
    ASSERT_TRUE(wait_until_true([&] { return fake->release_calls() > releases_before; }))
        << "3h without any callback signal must release the half-open client";
    clock->advance(std::chrono::minutes(10));  // covers the 1s backoff
    ASSERT_TRUE(wait_until_true([&] { return sig->is_connected(); }))
        << "the owner must recreate and reconnect after half-open detection";
    EXPECT_EQ(sig->health_snapshot().recreate_count, 1u)
        << "exactly one recreate: the liveness-triggered one";
    hours_elapsed += 3;
    check_step();

    // ---- Phase E: stable network to the 72h mark ----
    run_stable_hours(72 - hours_elapsed);
    ASSERT_TRUE(wait_until_true([&] { return ice_delivered.load() == ice_sent; }));
    auto end_snap = sig->health_snapshot();
    EXPECT_EQ(end_snap.recreate_count, 1u)
        << "no further recreate in the final stable window";
    EXPECT_EQ(end_snap.messages_dropped, 0u) << "no message may be dropped in 72h";
    EXPECT_EQ(end_snap.command_queue_depth, 0u);
    EXPECT_EQ(end_snap.message_queue_depth, 0u);

    // ---- Clean shutdown after 72 virtual hours ----
    sig->disconnect();
    EXPECT_FALSE(sig->is_connected());
    EXPECT_EQ(sig->health_snapshot().state, "STOPPED");

    const auto real_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - real_start);
    EXPECT_LE(real_elapsed.count(), 15)
        << "72 virtual hours must complete within 15s of real time";
}
