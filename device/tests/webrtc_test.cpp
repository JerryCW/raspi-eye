// webrtc_test.cpp
// WebRTC signaling tests: 6 example-based + 2 PBT properties.
#include "webrtc_signaling.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
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
// 在未修复代码上此测试 MUST PASS — 确认基线行为
RC_GTEST_PROP(WebRtcSignalingPreservation, ConnectDisconnectSendConsistency, ()) {
    auto sig = create_test_signaling();
    if (!sig) RC_DISCARD("Real SDK rejects fake creds");

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
// Bug Condition Exploration Test (spec-13.7)
// ============================================================

// Bug Condition Exploration: 意外断连后自动重连恢复
// Validates: Requirements 1.1, 1.2, 1.4, 2.1, 2.2
// 在未修复代码上此测试 FAIL（无自动重连机制）
// 在修复后此测试 PASS（reconnect_loop 自动恢复连接）
TEST(WebRtcSignalingBugCondition, AutoReconnectAfterUnexpectedDisconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err)) << "connect() failed: " << err;
    EXPECT_TRUE(sig->is_connected());

    // 模拟意外断连：通过 reconnect() 公共 API 触发 needs_reconnect_
    // 在真实场景中，SDK 回调 on_signaling_state_changed(DISCONNECTED) 设置此标志
    // reconnect() 设置 needs_reconnect_=true 并通知 reconnect_loop
    EXPECT_TRUE(sig->reconnect(&err));

    // 等待 reconnect_loop 处理（stub 使用 100ms 延迟）
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // EXPECTED BEHAVIOR: 断连后 reconnect_loop 自动恢复连接
    EXPECT_TRUE(sig->is_connected())
        << "Expected: auto-reconnect restores connection after unexpected disconnect";
}

// ============================================================
// Reconnect Unit Tests (spec-13.7, Task 3.6)
// ============================================================

// Test: reconnect() 触发 reconnect_loop 自动重连
// Validates: Requirements 2.1, 2.2
TEST(WebRtcSignalingReconnect, ReconnectTriggersAutoReconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err)) << "connect() failed: " << err;
    EXPECT_TRUE(sig->is_connected());

    // reconnect() 设置 needs_reconnect_ 并通知 reconnect_loop
    EXPECT_TRUE(sig->reconnect(&err));

    // 等待 reconnect_loop 处理（stub 使用 100ms 延迟）
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_TRUE(sig->is_connected())
        << "reconnect() should trigger auto-reconnect via reconnect_loop";
}

// Test: disconnect() 设置 shutdown_requested_，阻止自动重连
// Validates: Requirements 2.7
TEST(WebRtcSignalingReconnect, ShutdownPreventsReconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err));
    EXPECT_TRUE(sig->is_connected());

    // disconnect() 设置 shutdown_requested_ = true，停止 reconnect 线程
    sig->disconnect();
    EXPECT_FALSE(sig->is_connected());

    // 等待确认无自动重连发生
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(sig->is_connected())
        << "disconnect() should prevent auto-reconnect (shutdown_requested)";
}

// Test: disconnect() 安全停止 reconnect 线程，无崩溃/悬挂
// Validates: Requirements 2.7, 3.1
TEST(WebRtcSignalingReconnect, DisconnectSafelyStopsReconnectThread) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err));

    // disconnect 应安全停止 reconnect 线程
    sig->disconnect();
    EXPECT_FALSE(sig->is_connected());

    // 析构也应安全（无 double-join）
    sig.reset();
    // 到达此处无崩溃/悬挂即通过
}

// Test: disconnect() 后调用 reconnect() 执行完整重连
// Validates: Requirements 2.1, 2.2, 3.4
TEST(WebRtcSignalingReconnect, ReconnectAfterDisconnect) {
    auto sig = create_test_signaling();
    if (!sig) GTEST_SKIP() << "Real SDK rejects fake creds";

    std::string err;
    EXPECT_TRUE(sig->connect(&err));
    sig->disconnect();
    EXPECT_FALSE(sig->is_connected());

    // disconnect 后 reconnect 线程已停止，reconnect() 应执行完整 connect
    EXPECT_TRUE(sig->reconnect(&err));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(sig->is_connected())
        << "reconnect() after disconnect() should restore connection";
}
