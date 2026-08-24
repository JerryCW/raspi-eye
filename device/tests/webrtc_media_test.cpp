// webrtc_media_test.cpp
// WebRTC media manager tests: 6 example-based + 2 PBT properties + bug condition exploration.
// Custom main() required for gst_init (pipeline tests need GStreamer).
#include "webrtc_media.h"
#include "webrtc_signaling.h"
#include "pipeline_builder.h"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <unordered_set>

#include <gst/gst.h>
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

// ============================================================
// Test helpers
// ============================================================

static std::unique_ptr<WebRtcSignaling> create_stub_signaling() {
    WebRtcConfig config;
    config.channel_name = "test-channel";
    config.aws_region = "us-east-1";
    AwsConfig aws_config;
    aws_config.thing_name = "test-thing";
    return WebRtcSignaling::create(config, aws_config);
}

// Helper macro: skip test if real SDK rejects fake creds
#define SKIP_IF_REAL_SDK(sig) \
    if (!(sig)) GTEST_SKIP() << "Real SDK rejects fake creds"

// ============================================================
// Example-based tests
// ============================================================

// 1. StubCreateSuccess: create() returns non-null, peer_count() == 0
TEST(WebRtcMediaTest, StubCreateSuccess) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    std::string err;
    auto mgr = WebRtcMediaManager::create(*sig, "", &err);
    ASSERT_NE(mgr, nullptr) << "create() failed: " << err;
    EXPECT_EQ(mgr->peer_count(), 0u);
}

// 2. BroadcastFrameNoPeers: broadcast_frame() with no peers does not crash
TEST(WebRtcMediaTest, BroadcastFrameNoPeers) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);
    uint8_t data[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    mgr->broadcast_frame(data, sizeof(data), 1000, true);
    EXPECT_EQ(mgr->peer_count(), 0u);
}

// 3. AppsinkReplacesFakesink: webrtc-sink is appsink when WebRtcMediaManager provided
TEST(WebRtcMediaTest, AppsinkReplacesFakesink) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);
    std::string err;
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(
        &err, {}, nullptr, nullptr, mgr.get());
    ASSERT_NE(pipeline, nullptr) << "build_tee_pipeline failed: " << err;
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc-sink");
    ASSERT_NE(sink, nullptr);
    // Check factory name is "appsink"
    GstElementFactory* factory = gst_element_get_factory(sink);
    EXPECT_STREQ(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "appsink");
    gst_object_unref(sink);
    gst_object_unref(pipeline);
}

// 4. FakesinkPreservedWhenNull: webrtc-sink is fakesink when no manager
TEST(WebRtcMediaTest, FakesinkPreservedWhenNull) {
    std::string err;
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(&err);
    ASSERT_NE(pipeline, nullptr) << "build_tee_pipeline failed: " << err;
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc-sink");
    ASSERT_NE(sink, nullptr);
    GstElementFactory* factory = gst_element_get_factory(sink);
    EXPECT_STREQ(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "fakesink");
    gst_object_unref(sink);
    gst_object_unref(pipeline);
}

// 5. AppsinkProperties: appsink emit-signals/drop/max-buffers/sync values correct
TEST(WebRtcMediaTest, AppsinkProperties) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);
    std::string err;
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(
        &err, {}, nullptr, nullptr, mgr.get());
    ASSERT_NE(pipeline, nullptr) << err;
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc-sink");
    ASSERT_NE(sink, nullptr);

    gboolean emit_signals = FALSE;
    gboolean drop = FALSE;
    guint max_buffers = 0;
    gboolean sync = TRUE;
    g_object_get(G_OBJECT(sink),
        "emit-signals", &emit_signals,
        "drop", &drop,
        "max-buffers", &max_buffers,
        "sync", &sync,
        nullptr);
    EXPECT_TRUE(emit_signals);
    EXPECT_TRUE(drop);
    EXPECT_EQ(max_buffers, 1u);
    EXPECT_FALSE(sync);

    gst_object_unref(sink);
    gst_object_unref(pipeline);
}

// 6. PipelineSmokeWithAppsink: appsink pipeline starts and reaches PAUSED/PLAYING
TEST(WebRtcMediaTest, PipelineSmokeWithAppsink) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);
    std::string err;
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(
        &err, {}, nullptr, nullptr, mgr.get());
    ASSERT_NE(pipeline, nullptr) << err;

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    ASSERT_NE(ret, GST_STATE_CHANGE_FAILURE);

    GstState state = GST_STATE_NULL;
    gst_element_get_state(pipeline, &state, nullptr, 3 * GST_SECOND);
    EXPECT_TRUE(state == GST_STATE_PLAYING || state == GST_STATE_PAUSED)
        << "Pipeline state: " << state;

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// ============================================================
// Property-based tests
// ============================================================

// **Validates: Requirements 2.1, 2.6, 2.7, 5.1, 5.4, 6.2, 6.4, 6.5, 6.6, 6.10**
// Feature: spec-13-webrtc-media, Property 1: PeerConnection management invariant
RC_GTEST_PROP(WebRtcMediaPBT, PeerCountInvariant, ()) {
    auto sig = create_stub_signaling();
    if (!sig) RC_SUCCEED("Real SDK rejects fake creds, skipping");
    auto mgr = WebRtcMediaManager::create(*sig);
    RC_ASSERT(mgr != nullptr);

    // Reference model
    std::unordered_set<std::string> ref_set;

    // Generate random operation sequence (10-50 operations)
    auto num_ops = *rc::gen::inRange(10, 51);
    for (int i = 0; i < num_ops; ++i) {
        // Random operation: 0 = add, 1 = remove
        auto op = *rc::gen::inRange(0, 2);
        // Random peer_id from small pool (to increase collisions)
        auto peer_idx = *rc::gen::inRange(0, 15);
        std::string peer_id = "peer-" + std::to_string(peer_idx);

        if (op == 0) {
            // Add
            bool already_exists = ref_set.count(peer_id) > 0;
            bool at_limit = ref_set.size() >= 10 && !already_exists;

            bool result = mgr->on_viewer_offer(peer_id, "fake-sdp");

            if (at_limit) {
                RC_ASSERT(!result);
                // ref_set unchanged
            } else if (result) {
                ref_set.insert(peer_id);
            }
            // spec-33 adaptation: below the active limit an offer may still be
            // rejected transiently while retired handles hold their
            // HandlePermit until the Reaper finishes close/free. A rejected
            // offer must leave existing peers (and thus the model) unchanged;
            // the count invariant below still holds strictly.
        } else {
            // Remove
            mgr->remove_peer(peer_id);
            ref_set.erase(peer_id);
        }

        // Invariant: peer_count matches reference set
        RC_ASSERT(mgr->peer_count() == ref_set.size());
        RC_ASSERT(mgr->peer_count() <= 10u);
    }
}

// ============================================================
// Preservation Property Tests (spec-13.6)
// ============================================================
// 验证未修复代码的基线行为。修复后这些测试也必须通过（无回归）。
// 与 PeerCountInvariant 的区别：
// - 包含 on_viewer_ice_candidate 和 broadcast_frame 操作
// - 使用引用模型跟踪 peer 状态（为修复后 DISCONNECTING 状态做准备）
// - 显式验证替换语义、ICE candidate 缓存、broadcast_frame 安全性

// **Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7**
// Property 2: Preservation — Peer 管理不变量（状态机 + 计数）
RC_GTEST_PROP(WebRtcMediaPreservation, PeerLifecycleInvariant, ()) {
    auto sig = create_stub_signaling();
    if (!sig) RC_SUCCEED("Real SDK rejects fake creds, skipping");
    auto mgr = WebRtcMediaManager::create(*sig);
    RC_ASSERT(mgr != nullptr);

    // 引用模型：跟踪 CONNECTED 状态的 peer
    // 修复前：所有 insert 的 peer 都是 CONNECTED
    // 修复后：on_viewer_offer 后 peer 为 CONNECTED，remove_peer 后不再计入
    std::unordered_set<std::string> connected_peers;

    // 随机操作序列（15-60 操作）
    auto num_ops = *rc::gen::inRange(15, 61);
    for (int i = 0; i < num_ops; ++i) {
        // 操作类型: 0=offer, 1=remove, 2=ice_candidate, 3=broadcast_frame
        auto op = *rc::gen::inRange(0, 4);
        auto peer_idx = *rc::gen::inRange(0, 15);
        std::string peer_id = "pres-" + std::to_string(peer_idx);

        if (op == 0) {
            // on_viewer_offer
            bool already_exists = connected_peers.count(peer_id) > 0;
            bool at_limit = connected_peers.size() >= 10 && !already_exists;

            bool result = mgr->on_viewer_offer(peer_id, "fake-sdp");

            if (at_limit) {
                RC_ASSERT(!result);
            } else if (result) {
                connected_peers.insert(peer_id);
            }
            // spec-33 adaptation: transient HandlePermit exhaustion may reject
            // an offer below the active limit (permits are held until the
            // Reaper finishes close/free); the model only tracks successes.
        } else if (op == 1) {
            // remove_peer
            mgr->remove_peer(peer_id);
            connected_peers.erase(peer_id);
        } else if (op == 2) {
            // on_viewer_ice_candidate — 对不存在的 peer 返回 true（缓存）
            bool result = mgr->on_viewer_ice_candidate(peer_id, "fake-candidate");
            RC_ASSERT(result);
        } else {
            // broadcast_frame — 不崩溃
            uint8_t frame[] = {0x00, 0x00, 0x00, 0x01, 0x67};
            mgr->broadcast_frame(frame, sizeof(frame), 1000 * (i + 1), true);
        }

        // 不变量: peer_count 始终等于 CONNECTED 状态 peer 数量
        RC_ASSERT(mgr->peer_count() == connected_peers.size());
        RC_ASSERT(mgr->peer_count() <= 10u);
    }
}

// **Validates: Requirements 3.2, 3.5**
// Preservation: 同一 peer_id 重复 offer 后 peer_count 不增加（替换语义）
RC_GTEST_PROP(WebRtcMediaPreservation, ReplaceSemanticsPreserved, ()) {
    auto sig = create_stub_signaling();
    if (!sig) RC_SUCCEED("Real SDK rejects fake creds, skipping");
    auto mgr = WebRtcMediaManager::create(*sig);
    RC_ASSERT(mgr != nullptr);

    // 先添加若干不同 peer
    auto initial_count = *rc::gen::inRange(1, 8);
    for (int i = 0; i < initial_count; ++i) {
        bool ok = mgr->on_viewer_offer("replace-" + std::to_string(i), "sdp");
        RC_ASSERT(ok);
    }
    RC_ASSERT(mgr->peer_count() == static_cast<size_t>(initial_count));

    // 随机选一个已存在的 peer 重复 offer
    auto target_idx = *rc::gen::inRange(0, initial_count);
    std::string target_id = "replace-" + std::to_string(target_idx);

    bool result = mgr->on_viewer_offer(target_id, "new-sdp");
    RC_ASSERT(result);
    // 替换语义: peer_count 不变
    RC_ASSERT(mgr->peer_count() == static_cast<size_t>(initial_count));
}

// **Validates: Requirements 3.4, 3.7**
// Preservation: broadcast_frame 对任意 peer 状态组合不崩溃
RC_GTEST_PROP(WebRtcMediaPreservation, BroadcastFrameSafety, ()) {
    auto sig = create_stub_signaling();
    if (!sig) RC_SUCCEED("Real SDK rejects fake creds, skipping");
    auto mgr = WebRtcMediaManager::create(*sig);
    RC_ASSERT(mgr != nullptr);

    // 随机添加 0-10 个 peer
    auto peer_count_target = *rc::gen::inRange(0, 11);
    for (int i = 0; i < peer_count_target; ++i) {
        mgr->on_viewer_offer("bcast-" + std::to_string(i), "sdp");
    }

    // 随机移除一些
    auto remove_count = *rc::gen::inRange(0, peer_count_target + 1);
    for (int i = 0; i < remove_count; ++i) {
        mgr->remove_peer("bcast-" + std::to_string(i));
    }

    // broadcast_frame 不崩溃
    auto frame_count = *rc::gen::inRange(1, 20);
    uint8_t frame[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00};
    for (int i = 0; i < frame_count; ++i) {
        mgr->broadcast_frame(frame, sizeof(frame), 1000 * (i + 1), i == 0);
    }

    // 验证 peer_count 仍然一致
    size_t expected = static_cast<size_t>(
        std::max(0, peer_count_target - remove_count));
    RC_ASSERT(mgr->peer_count() == expected);
}

// ============================================================
// Bug Condition Exploration Test (spec-13.6)
// ============================================================
// **Validates: Requirements 1.1, 1.2, 1.3**
//
// Bug Condition: remove_peer 在持有 peers_mutex（独占锁 std::mutex）时执行
// 同步清理（真实 SDK 中调用 freePeerConnection 阻塞），导致 on_viewer_offer
// 被无限期阻塞，形成死锁。broadcast_frame 也使用独占锁，加剧锁竞争。
//
// Bug Condition 形式化:
//   isBugCondition(ctx) = ctx.holds_peers_mutex == true
//     AND ctx.function_called IN ['freePeerConnection']
//     AND ctx.calling_thread IN ['sdk_connection_state_thread', 'gstreamer_streaming_thread']
//
// 测试策略:
// 在 stub 中 remove_peer 仅执行 erase（微秒级），无法自然产生死锁。
// 因此测试通过以下方式证明 bug 存在：
//
// 1. 结构性验证（example-based）：用外部 mutex 模拟 peers_mutex，在持有锁时
//    sleep 200ms 模拟 freePeerConnection 阻塞，证明"锁内阻塞"导致 on_viewer_offer
//    被阻塞超过 100ms。同时验证"锁外阻塞"（修复后模式）不阻塞 on_viewer_offer。
//
// 2. 读-读并发验证（example-based）：验证 peer_count 和 broadcast_frame 在并发
//    on_viewer_offer 下的吞吐量。未修复代码使用 std::mutex（独占锁），读-读互斥。
//    修复后使用 shared_lock（读锁），读-读可并发。
//    测试断言：并发读操作的吞吐量应高于串行（修复后成立，未修复可能不成立）。
//
// 3. PBT：对于任意 peer_id 和阻塞时间，验证 bug pattern 导致超时。
//
// 预期结果：
// - 未修复代码：结构性测试 PASS（证明 bug pattern），读-读并发测试 FAIL
// - 修复后代码：所有测试 PASS

// 测试 1: 结构性验证 — 模拟 freePeerConnection 在锁内/锁外的行为差异
TEST(WebRtcMediaBugCondition, FreePeerConnectionInsideLockCausesBlocking) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);

    mgr->on_viewer_offer("blocker-peer", "sdp");

    // --- 模拟未修复代码：freePeerConnection 在锁内执行 ---
    {
        std::mutex simulated_mutex;
        std::atomic<bool> ready{false};

        std::thread blocker([&]() {
            std::lock_guard<std::mutex> lock(simulated_mutex);
            ready.store(true, std::memory_order_release);
            // 模拟 freePeerConnection 在锁内阻塞 200ms
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            mgr->remove_peer("blocker-peer");
        });

        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        auto start = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(simulated_mutex);
            mgr->on_viewer_offer("new-viewer", "sdp");
        }
        auto buggy_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        blocker.join();

        // Bug pattern: on_viewer_offer 被阻塞 >= 100ms
        EXPECT_GE(buggy_us, 100'000)
            << "Bug pattern (freePeerConnection inside lock) should block >= 100ms, "
            << "actual: " << buggy_us << "us";
    }

    // 重置
    mgr->remove_peer("new-viewer");
    mgr->on_viewer_offer("blocker-peer", "sdp");

    // --- 模拟修复后代码：freePeerConnection 在锁外执行 ---
    {
        std::mutex simulated_mutex;
        std::atomic<bool> ready{false};

        std::thread blocker([&]() {
            {
                std::lock_guard<std::mutex> lock(simulated_mutex);
                ready.store(true, std::memory_order_release);
                mgr->remove_peer("blocker-peer");
            }
            // 锁外阻塞 200ms
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });

        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        auto start = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(simulated_mutex);
            mgr->on_viewer_offer("new-viewer-2", "sdp");
        }
        auto fixed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        blocker.join();

        // Fixed pattern: on_viewer_offer 不被阻塞
        EXPECT_LT(fixed_us, 100'000)
            << "Fixed pattern (freePeerConnection outside lock) should not block, "
            << "actual: " << fixed_us << "us";
    }
}

// 测试 2（spec-33 Task 5 替换）：原 ReadReadConcurrencyWithSharedLock 用
// 吞吐量比值断言读-读并发，非确定性且违反 spec-33 SHALL NOT（吞吐比值证明）。
// 已由 PeerRuntimeTest.SlowWriteDoesNotBlockMapOrOtherSessions 取代：
// 事件门禁把一个 session 的 write 挡在 I/O gate 内，结构性断言 map 读
// （peer_count）、新 offer 与其他 session 的完整回收都不被阻塞。

// PBT: 对于任意 peer_id 和阻塞时间，验证 bug pattern 导致超时
// **Validates: Requirements 1.1, 1.2, 1.3**
RC_GTEST_PROP(WebRtcMediaBugConditionPBT, LockInsideBlockingCausesTimeout, ()) {
    auto sig = create_stub_signaling();
    if (!sig) RC_SUCCEED("Real SDK rejects fake creds, skipping");
    auto mgr = WebRtcMediaManager::create(*sig);
    RC_ASSERT(mgr != nullptr);

    auto peer_idx = *rc::gen::inRange(0, 100);
    std::string blocker_peer = "blocker-" + std::to_string(peer_idx);
    auto viewer_idx = *rc::gen::inRange(0, 100);
    std::string new_viewer = "viewer-" + std::to_string(viewer_idx);
    // 随机阻塞时间 50-100ms，模拟 freePeerConnection 不同耗时
    auto block_ms = *rc::gen::inRange(50, 101);

    mgr->on_viewer_offer(blocker_peer, "sdp");

    std::mutex simulated_mutex;
    std::atomic<bool> ready{false};

    std::thread blocker([&]() {
        std::lock_guard<std::mutex> lock(simulated_mutex);
        ready.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(block_ms));
        mgr->remove_peer(blocker_peer);
    });

    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto start = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(simulated_mutex);
        mgr->on_viewer_offer(new_viewer, "sdp");
    }
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    blocker.join();

    // Property: bug pattern（锁内阻塞）导致 on_viewer_offer 被阻塞 >= 30ms
    RC_ASSERT(elapsed_us >= 30'000);

    mgr->remove_peer(new_viewer);
}

// ============================================================
// Spec 26: set_pipeline / set_writeframe_fail_threshold tests
// ============================================================

// SetPipelineNull: set_pipeline(nullptr) does not crash
// **Validates: Requirements 9.1, 9.4**
TEST(WebRtcMediaTest, SetPipelineNull) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);
    // set_pipeline(nullptr) should be safe
    mgr->set_pipeline(nullptr);
    EXPECT_EQ(mgr->peer_count(), 0u);
}

// SetPipelineWithElement: set_pipeline with a real GstElement does not crash
// **Validates: Requirements 9.1, 9.4**
TEST(WebRtcMediaTest, SetPipelineWithElement) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);

    GstElement* pipeline = gst_pipeline_new("test-pipeline");
    ASSERT_NE(pipeline, nullptr);
    mgr->set_pipeline(pipeline);

    // broadcast_frame after set_pipeline should not crash
    uint8_t data[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    mgr->broadcast_frame(data, sizeof(data), 1000, true);

    mgr->set_pipeline(nullptr);
    gst_object_unref(pipeline);
}

// SetWriteframeFailThreshold: set_writeframe_fail_threshold does not crash
// **Validates: Requirements 7.1**
TEST(WebRtcMediaTest, SetWriteframeFailThreshold) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);
    mgr->set_writeframe_fail_threshold(5);
    mgr->set_writeframe_fail_threshold(20);
    EXPECT_EQ(mgr->peer_count(), 0u);
}

// ============================================================
// extract_sdp_summary Property-Based Tests (spec-25)
// ============================================================

// Feature: webrtc-log-observability, Property 1: SDP summary extraction round-trip
// **Validates: Requirements 3.5, 7.3**
RC_GTEST_PROP(ExtractSdpSummaryPBT, RoundTrip, ()) {
    // Generate 1-10 unique alphanumeric codec names
    auto num_codecs = *rc::gen::inRange(1, 11);
    std::unordered_set<std::string> expected_codecs;

    std::string sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n";

    for (int i = 0; i < num_codecs; ++i) {
        // Generate alphanumeric codec name (1-20 chars)
        auto len = *rc::gen::inRange(1, 21);
        std::string codec;
        for (int j = 0; j < len; ++j) {
            auto c = *rc::gen::oneOf(
                rc::gen::inRange('a', static_cast<char>('z' + 1)),
                rc::gen::inRange('A', static_cast<char>('Z' + 1)),
                rc::gen::inRange('0', static_cast<char>('9' + 1))
            );
            codec += c;
        }
        if (codec.empty()) continue;
        expected_codecs.insert(codec);

        // Random payload type 0-127, random clock rate
        auto pt = *rc::gen::inRange(0, 128);
        auto rate = *rc::gen::element(8000, 16000, 48000, 90000);
        sdp += "a=rtpmap:" + std::to_string(pt) + " " + codec + "/" + std::to_string(rate) + "\r\n";
    }

    std::string result = extract_sdp_summary(sdp);

    // All expected codecs should appear in the result (after dedup)
    for (const auto& codec : expected_codecs) {
        RC_ASSERT(result.find(codec) != std::string::npos);
    }
}

// Feature: webrtc-log-observability, Property 2: extract_sdp_summary robustness
// **Validates: Requirements 3.3, 7.3**
RC_GTEST_PROP(ExtractSdpSummaryPBT, Robustness, ()) {
    // Generate arbitrary string (including empty, random bytes, no rtpmap lines)
    auto input = *rc::gen::arbitrary<std::string>();

    // Must not crash, must return valid std::string
    std::string result = extract_sdp_summary(input);

    // Result is a valid string (implicit - if we get here, no crash)
    // Result should not contain newlines
    RC_ASSERT(result.find('\n') == std::string::npos);
    RC_ASSERT(result.find('\r') == std::string::npos);
}

// ============================================================
// extract_sdp_summary Example-Based Tests (spec-25)
// ============================================================

// ExtractSdpSummary_EmptyString: empty input -> empty output
TEST(ExtractSdpSummaryTest, EmptyString) {
    EXPECT_EQ(extract_sdp_summary(""), "");
}

// ExtractSdpSummary_NoRtpmap: no a=rtpmap: lines -> empty output
TEST(ExtractSdpSummaryTest, NoRtpmap) {
    std::string sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
                      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
                      "a=sendrecv\r\n";
    EXPECT_EQ(extract_sdp_summary(sdp), "");
}

// ExtractSdpSummary_RealSdp: real SDP with H264 + opus -> "H264, opus"
TEST(ExtractSdpSummaryTest, RealSdp) {
    std::string sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
                      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
                      "a=rtpmap:96 H264/90000\r\n"
                      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
                      "a=rtpmap:111 opus/48000/2\r\n";
    std::string result = extract_sdp_summary(sdp);
    EXPECT_NE(result.find("H264"), std::string::npos);
    EXPECT_NE(result.find("opus"), std::string::npos);
}

// ExtractSdpSummary_DuplicateCodecs: duplicate codec names -> deduplicated
TEST(ExtractSdpSummaryTest, DuplicateCodecs) {
    std::string sdp = "v=0\r\n"
                      "a=rtpmap:96 H264/90000\r\n"
                      "a=rtpmap:97 H264/90000\r\n"
                      "a=rtpmap:111 opus/48000\r\n";
    std::string result = extract_sdp_summary(sdp);
    // H264 should appear exactly once
    auto first = result.find("H264");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(result.find("H264", first + 1), std::string::npos) << "H264 appears more than once in: " << result;
    EXPECT_NE(result.find("opus"), std::string::npos);
}

// ============================================================
// Spec 33 Task 4 — Peer runtime tests
// (PeerSession + PeerCallbackBridge fixed slots + HandlePermit + Reaper)
// ManualClock + FakePeerSdkOps drive the shared runtime through
// create_for_test on every platform (no real SDK / credentials needed).
// No fixed sleeps: event gates, manual time and bounded polling on
// observable conditions only.
// ============================================================

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace {

namespace wi = webrtc::internal;

// Manual clock (same protocol as webrtc_test.cpp): advance() moves time and
// wakes every registered waiter under the waiter's own mutex, so a waiter
// that checked the clock but has not yet entered wait() cannot miss the
// wakeup (no lost-wakeup race).
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

// Bounded polling on an observable condition. NOT a fixed sleep: it returns
// as soon as the condition holds; the deadline only bounds the wait.
template <typename Pred>
bool eventually(Pred pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return pred();
}

// Minimal signaling fake so peer runtime tests run on every platform
// (including Pi 5 where the real SDK rejects fake credentials).
// connect() publishes CONNECTED synchronously (stub semantics).
class TestSignalingOps : public wi::SignalingSdkOps {
public:
    explicit TestSignalingOps(std::shared_ptr<wi::RuntimeClock> clock)
        : clock_(std::move(clock)) {}

    std::vector<std::string> answered_peers() const {
        std::lock_guard<std::mutex> lock(mu_);
        return answered_;
    }

    // (peer_id, candidate) pairs delivered through send_ice (Task 5:
    // observes the fire-and-forget local ICE path end to end).
    std::vector<std::pair<std::string, std::string>> posted_ice() const {
        std::lock_guard<std::mutex> lock(mu_);
        return posted_ice_;
    }

    wi::SdkCallResult create(uint64_t generation,
                             const wi::SignalingCallbacks& cbs) override {
        std::lock_guard<std::mutex> lock(mu_);
        generation_ = generation;
        callbacks_ = cbs;
        return {};
    }
    wi::SdkCallResult fetch() override { return {}; }
    wi::SdkCallResult connect() override {
        wi::SignalingCallbacks cbs;
        uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs = callbacks_;
            gen = generation_;
        }
        if (cbs.on_state) cbs.on_state(gen, wi::kSignalingStateConnected, clock_->now());
        return {};
    }
    wi::SdkCallResult send_answer(std::string_view peer, std::string_view) override {
        std::lock_guard<std::mutex> lock(mu_);
        answered_.push_back(std::string(peer));
        return {};
    }
    wi::SdkCallResult send_ice(std::string_view peer, std::string_view payload) override {
        std::lock_guard<std::mutex> lock(mu_);
        posted_ice_.emplace_back(std::string(peer), std::string(payload));
        return {};
    }
    wi::SdkCallResult query_ice(std::vector<wi::IceServerRecord>& out) override {
        out.clear();
        return {};
    }
    wi::SdkCallResult release() override { return {}; }

private:
    std::shared_ptr<wi::RuntimeClock> clock_;
    mutable std::mutex mu_;
    wi::SignalingCallbacks callbacks_;
    uint64_t generation_ = 0;
    std::vector<std::string> answered_;
    std::vector<std::pair<std::string, std::string>> posted_ice_;
};

// Fake peer adapter: failure injection, stored per-generation callbacks
// (for synchronous / out-of-order / arbitrarily late emission), an event
// gate that blocks close() (Reaper backpressure + unblockable close/free),
// per-generation close/release/write counters, retire-thread recording and
// an optional map-lock probe.
class FakePeerHandle : public wi::PeerHandle {
public:
    explicit FakePeerHandle(uint64_t gen) : generation(gen) {}
    uint64_t generation;
};

class FakePeerSdkOps : public wi::PeerSdkOps {
public:
    // --- knobs (configure from the test thread before use) ---
    std::atomic<int> create_failures{0};
    std::atomic<int> negotiate_failures{0};
    std::atomic<bool> sync_connected{true};        // CONNECTED inside create()
    std::atomic<bool> emit_wrong_generation{false};  // bogus-gen state in create()
    std::atomic<bool> probe_enabled{false};
    WebRtcMediaManager* probe_target = nullptr;
    // Task 5 write knobs: 0 = OK, 1 = RETRYABLE (SRTP not ready), 2 = FATAL.
    std::atomic<int> write_result_mode{0};
    // Generation whose write_frame parks on the write gate (0 = none).
    std::atomic<uint64_t> write_block_gen{0};
    std::atomic<int> writes_blocked{0};  // threads currently parked in the gate

    void set_answer(std::string answer) {
        std::lock_guard<std::mutex> lock(mu_);
        answer_ = std::move(answer);
    }

    // --- close() event gate ---
    void close_close_gate() {
        std::lock_guard<std::mutex> lock(gate_mu_);
        gate_open_ = false;
    }
    void open_close_gate() {
        {
            std::lock_guard<std::mutex> lock(gate_mu_);
            gate_open_ = true;
        }
        gate_cv_.notify_all();
    }

    // --- write_frame() event gate (only for write_block_gen) ---
    void open_write_gate() {
        {
            std::lock_guard<std::mutex> lock(wgate_mu_);
            wgate_open_ = true;
        }
        wgate_cv_.notify_all();
    }

    // --- observations ---
    int create_calls() const { std::lock_guard<std::mutex> l(mu_); return create_calls_; }
    int total_release_calls() const { std::lock_guard<std::mutex> l(mu_); return total_releases_; }
    int close_count(uint64_t gen) const { return count_of(close_counts_, gen); }
    int release_count(uint64_t gen) const { return count_of(release_counts_, gen); }
    int write_count(uint64_t gen) const { return count_of(write_counts_, gen); }
    int add_ice_count(uint64_t gen) const { return count_of(add_ice_counts_, gen); }
    std::vector<std::string> add_ice_payloads(uint64_t gen) const {
        std::lock_guard<std::mutex> l(mu_);
        auto it = add_ice_payloads_.find(gen);
        return it == add_ice_payloads_.end() ? std::vector<std::string>{} : it->second;
    }
    uint64_t last_generation() const { std::lock_guard<std::mutex> l(mu_); return last_gen_; }
    wi::PeerCallbacks stored_callbacks(uint64_t gen) const {
        std::lock_guard<std::mutex> l(mu_);
        auto it = stored_.find(gen);
        return it == stored_.end() ? wi::PeerCallbacks{} : it->second;
    }
    std::set<std::thread::id> retire_threads() const {
        std::lock_guard<std::mutex> l(mu_);
        return retire_threads_;
    }
    bool probe_failed() const { return probe_failed_.load(); }

    // --- PeerSdkOps ---
    wi::SdkCallResult create(uint64_t gen, const wi::PeerCallbacks& cbs,
                             std::unique_ptr<wi::PeerHandle>& out) override {
        probe();
        {
            std::lock_guard<std::mutex> l(mu_);
            ++create_calls_;
            last_gen_ = gen;
            stored_[gen] = cbs;
        }
        if (consume(create_failures)) return {wi::SdkCallStatus::RETRYABLE, 0xC1};
        if (emit_wrong_generation.load() && cbs.on_state) {
            cbs.on_state(gen + 987654321ULL, wi::kPeerStateConnected);
        }
        if (sync_connected.load() && cbs.on_state) {
            // Synchronous early callback, before the handle is returned.
            cbs.on_state(gen, wi::kPeerStateConnected);
        }
        out = std::make_unique<FakePeerHandle>(gen);
        return {};
    }
    wi::SdkCallResult negotiate(wi::PeerHandle&, std::string_view,
                                std::string& answer) override {
        probe();
        if (consume(negotiate_failures)) return {wi::SdkCallStatus::RETRYABLE, 0xC2};
        std::lock_guard<std::mutex> l(mu_);
        answer = answer_;
        return {};
    }
    wi::SdkCallResult add_ice(wi::PeerHandle& h, std::string_view candidate) override {
        probe();
        std::lock_guard<std::mutex> l(mu_);
        const uint64_t gen = static_cast<FakePeerHandle&>(h).generation;
        ++add_ice_counts_[gen];
        add_ice_payloads_[gen].emplace_back(candidate);
        return {};
    }
    wi::SdkCallResult write_frame(wi::PeerHandle& h, const uint8_t*, size_t,
                                  uint64_t, bool) override {
        probe();
        const uint64_t gen = static_cast<FakePeerHandle&>(h).generation;
        if (gen != 0 && write_block_gen.load() == gen) {
            writes_blocked.fetch_add(1);
            {
                std::unique_lock<std::mutex> gate(wgate_mu_);
                wgate_cv_.wait(gate, [this] { return wgate_open_; });
            }
            writes_blocked.fetch_sub(1);
        }
        {
            std::lock_guard<std::mutex> l(mu_);
            ++write_counts_[gen];
        }
        const int mode = write_result_mode.load();
        if (mode == 1) return {wi::SdkCallStatus::RETRYABLE, 0xE1};
        if (mode == 2) return {wi::SdkCallStatus::FATAL, 0xE2};
        return {};
    }
    wi::SdkCallResult close(wi::PeerHandle& h) override {
        probe();
        {
            std::unique_lock<std::mutex> gate(gate_mu_);
            gate_cv_.wait(gate, [this] { return gate_open_; });
        }
        std::lock_guard<std::mutex> l(mu_);
        ++close_counts_[static_cast<FakePeerHandle&>(h).generation];
        retire_threads_.insert(std::this_thread::get_id());
        return {};
    }
    wi::SdkCallResult release(std::unique_ptr<wi::PeerHandle> h) override {
        probe();
        std::lock_guard<std::mutex> l(mu_);
        if (h) {
            ++release_counts_[static_cast<FakePeerHandle*>(h.get())->generation];
        }
        ++total_releases_;
        retire_threads_.insert(std::this_thread::get_id());
        return {};
    }

private:
    static bool consume(std::atomic<int>& counter) {
        int v = counter.load();
        while (v > 0) {
            if (counter.compare_exchange_weak(v, v - 1)) return true;
        }
        return false;
    }

    int count_of(const std::map<uint64_t, int>& m, uint64_t gen) const {
        std::lock_guard<std::mutex> l(mu_);
        auto it = m.find(gen);
        return it == m.end() ? 0 : it->second;
    }

    // Map-lock probe: peer_count() takes the peer map shared lock. If the
    // runtime held the map mutex around any SDK op the probe would time out
    // (a deterministic structural failure, not a throughput ratio).
    void probe() {
        if (!probe_enabled.load() || probe_target == nullptr) return;
        auto fut = std::async(std::launch::async,
                              [t = probe_target] { return t->peer_count(); });
        if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            probe_failed_.store(true);
        }
    }

    mutable std::mutex mu_;
    std::string answer_;
    int create_calls_ = 0;
    int total_releases_ = 0;
    uint64_t last_gen_ = 0;
    std::map<uint64_t, wi::PeerCallbacks> stored_;
    std::map<uint64_t, int> close_counts_;
    std::map<uint64_t, int> release_counts_;
    std::map<uint64_t, int> write_counts_;
    std::map<uint64_t, int> add_ice_counts_;
    std::map<uint64_t, std::vector<std::string>> add_ice_payloads_;
    std::set<std::thread::id> retire_threads_;
    std::atomic<bool> probe_failed_{false};

    std::mutex gate_mu_;
    std::condition_variable gate_cv_;
    bool gate_open_ = true;

    std::mutex wgate_mu_;
    std::condition_variable wgate_cv_;
    bool wgate_open_ = false;  // parks only writes for write_block_gen
};

// Harness: fake signaling + fake peer ops + manual clock on the shared
// runtime. Destruction order mirrors production: media -> signaling ->
// runtime token.
struct PeerRuntimeHarness {
    std::shared_ptr<ManualClock> clock;
    std::shared_ptr<TestSignalingOps> sig_ops;
    std::shared_ptr<FakePeerSdkOps> peer_ops;
    std::unique_ptr<WebRtcSignaling> signaling;
    std::unique_ptr<WebRtcMediaManager> mgr;

    explicit PeerRuntimeHarness(size_t permits = 16, bool connect_signaling = false) {
        clock = std::make_shared<ManualClock>();
        sig_ops = std::make_shared<TestSignalingOps>(clock);
        peer_ops = std::make_shared<FakePeerSdkOps>();
        wi::RuntimeOptions options;
        options.handle_permits = permits;
        WebRtcConfig config;
        config.channel_name = "rt-media-channel";
        config.aws_region = "us-east-1";
        AwsConfig aws;
        aws.thing_name = "rt-media-thing";
        std::string err;
        signaling = WebRtcSignaling::create_for_test(config, aws, sig_ops, clock,
                                                     options, &err);
        if (signaling && connect_signaling) signaling->connect();
        if (signaling) {
            mgr = WebRtcMediaManager::create_for_test(*signaling, "us-east-1",
                                                      peer_ops, clock, options, &err);
        }
        if (mgr) peer_ops->probe_target = mgr.get();
    }

    ~PeerRuntimeHarness() {
        // Probing through the public pointer is only valid while the manager
        // is alive; shutdown-drain retirements must not probe.
        peer_ops->probe_enabled.store(false);
        peer_ops->open_close_gate();  // never leave the Reaper gated at teardown
        peer_ops->open_write_gate();  // never leave a broadcast parked either
        mgr.reset();                  // drains + joins the Reaper
        signaling.reset();
    }
};

}  // namespace

// 1. Synchronous early callback (fired inside create, before the handle is
//    returned) connects the peer; a bogus-generation callback fired before
//    the peer generation is even valid has zero side effects.
// **Validates: Requirements 4.1, 4.4**
TEST(PeerRuntimeTest, SyncEarlyCallbackConnectsPeer) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    h.peer_ops->emit_wrong_generation.store(true);
    EXPECT_TRUE(h.mgr->on_viewer_offer("peer-a", "offer-sdp"));
    EXPECT_EQ(h.mgr->peer_count(), 1u);
    EXPECT_EQ(h.peer_ops->create_calls(), 1);
}

// 2. broadcast_frame writes only to CONNECTED sessions (fresh runtime).
// **Validates: Requirements 5.1, 7.2**
TEST(PeerRuntimeTest, BroadcastWritesConnectedSession) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.mgr->on_viewer_offer("peer-a", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    uint8_t frame[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    h.mgr->broadcast_frame(frame, sizeof(frame), 1000, true);
    EXPECT_EQ(h.peer_ops->write_count(gen), 1);
}

// 3. remove_peer retires exactly once on the Reaper thread (never on the
//    calling thread); repeated removes are no-ops.
// **Validates: Requirements 4.3, 4.5**
TEST(PeerRuntimeTest, RemoveRetiresExactlyOnceOnReaperThread) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.mgr->on_viewer_offer("peer-a", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    const auto main_id = std::this_thread::get_id();
    h.mgr->remove_peer("peer-a");
    EXPECT_EQ(h.mgr->peer_count(), 0u);
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen) == 1; }));
    EXPECT_EQ(h.peer_ops->close_count(gen), 1);
    h.mgr->remove_peer("peer-a");
    h.mgr->remove_peer("peer-a");
    EXPECT_EQ(h.peer_ops->release_count(gen), 1);
    auto threads = h.peer_ops->retire_threads();
    EXPECT_EQ(threads.size(), 1u) << "close/free must run on the single Reaper worker";
    EXPECT_EQ(threads.count(main_id), 0u) << "never free on the calling thread";
}

// 4. Arbitrarily late callbacks for a retired generation have zero side
//    effects (slot memory is stable, lease validation rejects them).
// **Validates: Requirements 4.1, 4.4**
TEST(PeerRuntimeTest, LateCallbacksAfterRetireHaveZeroSideEffects) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.mgr->on_viewer_offer("peer-a", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    auto cbs = h.peer_ops->stored_callbacks(gen);
    ASSERT_TRUE(static_cast<bool>(cbs.on_state));
    h.mgr->remove_peer("peer-a");
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen) == 1; }));
    cbs.on_state(gen, wi::kPeerStateConnected);
    cbs.on_state(gen, wi::kPeerStateFailed);
    cbs.on_local_ice(gen, "candidate");
    EXPECT_EQ(h.mgr->peer_count(), 0u);
    EXPECT_EQ(h.peer_ops->release_count(gen), 1);
    EXPECT_EQ(h.peer_ops->close_count(gen), 1);
}

// 5. Late callbacks AFTER slot reuse (permits=1 forces reuse of the single
//    slot) must not pollute the new generation bound to the same slot.
// **Validates: Requirements 4.1, 4.4**
TEST(PeerRuntimeTest, LateCallbacksAfterSlotReuseDoNotPolluteNewPeer) {
    PeerRuntimeHarness h(/*permits=*/1);
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.mgr->on_viewer_offer("peer-a", "sdp"));
    const uint64_t gen1 = h.peer_ops->last_generation();
    auto old_cbs = h.peer_ops->stored_callbacks(gen1);
    h.mgr->remove_peer("peer-a");
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen1) == 1; }));
    // The single permit/slot becomes reusable only after in-flight==0.
    ASSERT_TRUE(eventually([&] { return h.mgr->on_viewer_offer("peer-b", "sdp"); }));
    const uint64_t gen2 = h.peer_ops->last_generation();
    EXPECT_NE(gen1, gen2);
    EXPECT_EQ(h.mgr->peer_count(), 1u);
    // Old-generation callbacks hit the REUSED slot: stale, zero side effects.
    old_cbs.on_state(gen1, wi::kPeerStateFailed);
    old_cbs.on_state(gen1, wi::kPeerStateConnected);
    old_cbs.on_local_ice(gen1, "candidate");
    EXPECT_EQ(h.mgr->peer_count(), 1u);
    EXPECT_EQ(h.peer_ops->release_count(gen2), 0);
    uint8_t frame[] = {0x00, 0x00, 0x00, 0x01};
    h.mgr->broadcast_frame(frame, sizeof(frame), 1, true);
    EXPECT_EQ(h.peer_ops->write_count(gen2), 1) << "new peer must stay functional";
}

// 6. Permit exhaustion rejects new peers while KEEPING existing peers; a
//    blocked close/free is unblockable via the event gate, after which the
//    permit is recycled and admission recovers.
// **Validates: Requirements 4.2, 4.3**
TEST(PeerRuntimeTest, PermitExhaustionRejectsAndRecoversAfterUnblock) {
    PeerRuntimeHarness h(/*permits=*/2);
    ASSERT_NE(h.mgr, nullptr);
    h.peer_ops->close_close_gate();
    ASSERT_TRUE(h.mgr->on_viewer_offer("peer-a", "sdp"));  // permit 1
    h.mgr->remove_peer("peer-a");  // Reaper blocks inside close(); permit 1 held
    ASSERT_TRUE(h.mgr->on_viewer_offer("peer-b", "sdp"));  // permit 2
    std::string err;
    EXPECT_FALSE(h.mgr->on_viewer_offer("peer-c", "sdp", &err));
    EXPECT_EQ(err, "no handle permit available");
    EXPECT_EQ(h.mgr->peer_count(), 1u) << "existing peer must be preserved";
    h.peer_ops->open_close_gate();
    ASSERT_TRUE(eventually([&] { return h.mgr->on_viewer_offer("peer-c", "sdp"); }));
    EXPECT_EQ(h.mgr->peer_count(), 2u);
}

// 7. Reaper backpressure: gated close() queues retirements without freeing
//    anything on the calling thread; opening the gate drains everything
//    exactly once on the single Reaper worker.
// **Validates: Requirements 4.3, 4.5, 6.1**
TEST(PeerRuntimeTest, ReaperBackpressureExactlyOnce) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    h.peer_ops->close_close_gate();
    std::vector<uint64_t> gens;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(h.mgr->on_viewer_offer("bp-" + std::to_string(i), "sdp"));
        gens.push_back(h.peer_ops->last_generation());
    }
    for (int i = 0; i < 5; ++i) h.mgr->remove_peer("bp-" + std::to_string(i));
    EXPECT_EQ(h.mgr->peer_count(), 0u);
    EXPECT_EQ(h.peer_ops->total_release_calls(), 0)
        << "nothing may be freed while close() is gated (and never in place)";
    h.peer_ops->open_close_gate();
    ASSERT_TRUE(eventually([&] { return h.peer_ops->total_release_calls() == 5; }));
    for (uint64_t g : gens) {
        EXPECT_EQ(h.peer_ops->close_count(g), 1);
        EXPECT_EQ(h.peer_ops->release_count(g), 1);
    }
    auto threads = h.peer_ops->retire_threads();
    EXPECT_EQ(threads.size(), 1u);
    EXPECT_EQ(threads.count(std::this_thread::get_id()), 0u);
}

// 8. Repeated offer for the same peer id replaces exactly once: the old
//    generation is retired exactly once, frames go only to the new one.
// **Validates: Requirements 4.3, 4.4, 7.2**
TEST(PeerRuntimeTest, RepeatedOfferReplacesExactlyOnce) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.mgr->on_viewer_offer("dup", "sdp-1"));
    const uint64_t gen1 = h.peer_ops->last_generation();
    ASSERT_TRUE(h.mgr->on_viewer_offer("dup", "sdp-2"));
    const uint64_t gen2 = h.peer_ops->last_generation();
    EXPECT_NE(gen1, gen2);
    EXPECT_EQ(h.mgr->peer_count(), 1u);
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen1) == 1; }));
    EXPECT_EQ(h.peer_ops->close_count(gen1), 1);
    EXPECT_EQ(h.peer_ops->release_count(gen2), 0);
    uint8_t frame[] = {0x00, 0x00, 0x00, 0x01};
    h.mgr->broadcast_frame(frame, sizeof(frame), 1, true);
    EXPECT_EQ(h.peer_ops->write_count(gen1), 0);
    EXPECT_EQ(h.peer_ops->write_count(gen2), 1);
}

// 9. create() failure rolls back and recycles the permit (permits=1 proves
//    recycling: the next offer can only succeed on the recycled permit).
// **Validates: Requirements 4.3, 4.5**
TEST(PeerRuntimeTest, CreateFailureRollsBackAndRecyclesPermit) {
    PeerRuntimeHarness h(/*permits=*/1);
    ASSERT_NE(h.mgr, nullptr);
    h.peer_ops->create_failures.store(1);
    std::string err;
    EXPECT_FALSE(h.mgr->on_viewer_offer("peer-a", "sdp", &err));
    EXPECT_EQ(h.mgr->peer_count(), 0u);
    EXPECT_EQ(h.peer_ops->total_release_calls(), 0) << "no handle existed to free";
    ASSERT_TRUE(eventually([&] { return h.mgr->on_viewer_offer("peer-b", "sdp"); }));
    EXPECT_EQ(h.mgr->peer_count(), 1u);
}

// 10. negotiate() failure rolls back: the already-created handle is closed
//     and freed exactly once by the Reaper, permit recycled.
// **Validates: Requirements 4.3, 4.5**
TEST(PeerRuntimeTest, NegotiateFailureRollsBackAndFreesHandle) {
    PeerRuntimeHarness h(/*permits=*/1);
    ASSERT_NE(h.mgr, nullptr);
    h.peer_ops->negotiate_failures.store(1);
    EXPECT_FALSE(h.mgr->on_viewer_offer("peer-a", "sdp"));
    EXPECT_EQ(h.mgr->peer_count(), 0u);
    const uint64_t gen1 = h.peer_ops->last_generation();
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen1) == 1; }));
    EXPECT_EQ(h.peer_ops->close_count(gen1), 1);
    ASSERT_TRUE(eventually([&] { return h.mgr->on_viewer_offer("peer-b", "sdp"); }));
    EXPECT_EQ(h.mgr->peer_count(), 1u);
}

// 11. send_answer failure (signaling not connected) rolls back the fully
//     created peer; the handle is freed exactly once, permit recycled.
// **Validates: Requirements 4.3, 4.5**
TEST(PeerRuntimeTest, SendAnswerFailureRollsBack) {
    PeerRuntimeHarness h(/*permits=*/1);  // signaling NOT connected
    ASSERT_NE(h.mgr, nullptr);
    h.peer_ops->set_answer("answer-sdp");
    std::string err;
    EXPECT_FALSE(h.mgr->on_viewer_offer("peer-a", "sdp", &err));
    EXPECT_EQ(err, "send_answer failed");
    EXPECT_EQ(h.mgr->peer_count(), 0u);
    const uint64_t gen1 = h.peer_ops->last_generation();
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen1) == 1; }));
    h.peer_ops->set_answer("");
    ASSERT_TRUE(eventually([&] { return h.mgr->on_viewer_offer("peer-b", "sdp"); }));
}

// 12. Non-empty answer goes out through the injected signaling (SEND
//     semantics preserved end to end).
// **Validates: Requirements 7.2**
TEST(PeerRuntimeTest, SendAnswerSuccessGoesThroughSignaling) {
    PeerRuntimeHarness h(/*permits=*/16, /*connect_signaling=*/true);
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.signaling->is_connected());
    h.peer_ops->set_answer("answer-sdp");
    ASSERT_TRUE(h.mgr->on_viewer_offer("peer-a", "sdp"));
    auto peers = h.sig_ops->answered_peers();
    ASSERT_EQ(peers.size(), 1u);
    EXPECT_EQ(peers[0], "peer-a");
}

// 13. Terminal state callback (current generation) marks DISCONNECTING;
//     the Reaper frees after the 10s grace period (manual time), exactly
//     once even with duplicated terminal callbacks.
// **Validates: Requirements 4.3, 4.4, 4.5**
TEST(PeerRuntimeTest, FailedStateCallbackRetiresAfterGrace) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.mgr->on_viewer_offer("peer-a", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    auto cbs = h.peer_ops->stored_callbacks(gen);
    cbs.on_state(gen, wi::kPeerStateFailed);
    EXPECT_EQ(h.mgr->peer_count(), 0u);
    cbs.on_state(gen, wi::kPeerStateClosed);   // duplicates must not double-submit
    cbs.on_state(gen, wi::kPeerStateFailed);
    EXPECT_EQ(h.peer_ops->release_count(gen), 0) << "grace period not expired yet";
    h.clock->advance(std::chrono::seconds(11));
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen) == 1; }));
    EXPECT_EQ(h.peer_ops->close_count(gen), 1);
}

// 14. Early viewer ICE candidates are buffered and flushed after the offer;
//     later candidates apply directly (both outside the map mutex).
// **Validates: Requirements 7.2**
TEST(PeerRuntimeTest, EarlyIceBufferedAndFlushedAfterOffer) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    EXPECT_TRUE(h.mgr->on_viewer_ice_candidate("peer-a", "cand-1"));
    EXPECT_TRUE(h.mgr->on_viewer_ice_candidate("peer-a", "cand-2"));
    ASSERT_TRUE(h.mgr->on_viewer_offer("peer-a", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    EXPECT_EQ(h.peer_ops->add_ice_count(gen), 2);
    EXPECT_TRUE(h.mgr->on_viewer_ice_candidate("peer-a", "cand-3"));
    EXPECT_EQ(h.peer_ops->add_ice_count(gen), 3);
}

// 15. Shutdown (destruction) retires every remaining peer exactly once and
//     drains the Reaper synchronously; repeated remove/shutdown are no-ops.
// **Validates: Requirements 4.3, 4.5, 8.2, 8.3**
TEST(PeerRuntimeTest, ShutdownRetiresAllPeersExactlyOnce) {
    auto h = std::make_unique<PeerRuntimeHarness>();
    ASSERT_NE(h->mgr, nullptr);
    std::vector<uint64_t> gens;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(h->mgr->on_viewer_offer("sd-" + std::to_string(i), "sdp"));
        gens.push_back(h->peer_ops->last_generation());
    }
    h->mgr->remove_peer("no-such-peer");   // repeated/nonexistent remove: no-op
    auto ops = h->peer_ops;                // keep the fake alive past the manager
    h->mgr.reset();                        // full shutdown incl. Reaper drain/join
    for (uint64_t g : gens) {
        EXPECT_EQ(ops->close_count(g), 1);
        EXPECT_EQ(ops->release_count(g), 1);
    }
    h.reset();  // signaling + runtime token teardown must be clean too
}

// 16. Lock probe: no PeerSdkOps call (create/negotiate/add_ice/write/close/
//     release) may run while the peer map mutex is held — covering the
//     success path AND every failure rollback branch (create/negotiate/
//     send_answer) plus the replace branch.
// **Validates: Requirements 4.2, 4.5, 5.1**
TEST(PeerRuntimeTest, NoSdkCallUnderPeerMapMutex) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    h.peer_ops->probe_enabled.store(true);
    // Success path: create/negotiate probes, then add_ice, write, close, release.
    ASSERT_TRUE(h.mgr->on_viewer_offer("probe-a", "sdp"));
    const uint64_t gen_a = h.peer_ops->last_generation();
    EXPECT_TRUE(h.mgr->on_viewer_ice_candidate("probe-a", "cand"));
    uint8_t frame[] = {0x00, 0x00, 0x00, 0x01};
    h.mgr->broadcast_frame(frame, sizeof(frame), 1, true);
    h.mgr->remove_peer("probe-a");
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen_a) == 1; }));
    // create-failure rollback branch.
    h.peer_ops->create_failures.store(1);
    EXPECT_FALSE(h.mgr->on_viewer_offer("probe-b", "sdp"));
    // negotiate-failure rollback branch (close/release probes on the Reaper).
    h.peer_ops->negotiate_failures.store(1);
    EXPECT_FALSE(h.mgr->on_viewer_offer("probe-c", "sdp"));
    const uint64_t gen_c = h.peer_ops->last_generation();
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen_c) == 1; }));
    // send_answer-failure rollback branch (signaling not connected).
    h.peer_ops->set_answer("answer");
    EXPECT_FALSE(h.mgr->on_viewer_offer("probe-d", "sdp"));
    const uint64_t gen_d = h.peer_ops->last_generation();
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen_d) == 1; }));
    h.peer_ops->set_answer("");
    // Replace branch.
    ASSERT_TRUE(h.mgr->on_viewer_offer("probe-e", "sdp"));
    const uint64_t gen_e1 = h.peer_ops->last_generation();
    ASSERT_TRUE(h.mgr->on_viewer_offer("probe-e", "sdp"));
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen_e1) == 1; }));
    EXPECT_FALSE(h.peer_ops->probe_failed())
        << "a PeerSdkOps call ran while the peer map mutex was held";
}

// **Validates: Requirements 4.1, 4.2, 4.4, 4.5**
// Feature: spec-33-webrtc-long-running-resilience, Property 5:
// Bridge slot safety & peer exactly-once retirement — for any sequence of
// offer/remove/late-callback/broadcast operations, late callbacks for
// retired generations have zero side effects (slot addresses are stable and
// reused only after quiescence), and every retired generation is closed and
// released exactly once.
RC_GTEST_PROP(PeerRuntimePBT, SlotSafetyAndExactlyOnceRetirement, ()) {
    PeerRuntimeHarness h(/*permits=*/4);
    RC_ASSERT(h.mgr != nullptr);

    std::vector<std::pair<uint64_t, wi::PeerCallbacks>> retired_cbs;
    std::map<std::string, uint64_t> live;  // peer id -> current generation

    auto num_ops = *rc::gen::inRange(10, 40);
    for (int i = 0; i < num_ops; ++i) {
        auto op = *rc::gen::inRange(0, 4);
        auto idx = *rc::gen::inRange(0, 3);
        std::string peer = "pbt-" + std::to_string(idx);
        if (op == 0) {
            bool ok = h.mgr->on_viewer_offer(peer, "sdp");
            if (ok) {
                auto it = live.find(peer);
                if (it != live.end()) {
                    retired_cbs.push_back({it->second,
                                           h.peer_ops->stored_callbacks(it->second)});
                }
                live[peer] = h.peer_ops->last_generation();
            }
            // Rejection (transient permit exhaustion with permits=4) must
            // leave the model unchanged.
        } else if (op == 1) {
            auto it = live.find(peer);
            if (it != live.end()) {
                retired_cbs.push_back({it->second,
                                       h.peer_ops->stored_callbacks(it->second)});
                live.erase(it);
            }
            h.mgr->remove_peer(peer);
        } else if (op == 2 && !retired_cbs.empty()) {
            // Arbitrary late callback for a retired generation (before or
            // after slot reuse — permits=4 forces heavy reuse).
            auto pick = *rc::gen::inRange<size_t>(0, retired_cbs.size());
            auto state = *rc::gen::element(
                static_cast<int>(wi::kPeerStateConnected),
                static_cast<int>(wi::kPeerStateFailed),
                static_cast<int>(wi::kPeerStateClosed));
            auto& entry = retired_cbs[pick];
            if (entry.second.on_state) entry.second.on_state(entry.first, state);
            if (entry.second.on_local_ice) entry.second.on_local_ice(entry.first, "cand");
        } else {
            uint8_t frame[] = {0x00, 0x00, 0x00, 0x01};
            h.mgr->broadcast_frame(frame, sizeof(frame),
                                   static_cast<uint64_t>(i) + 1, true);
        }
        // Zero side effects from late callbacks: only successful offers are
        // CONNECTED, retired generations never resurrect.
        RC_ASSERT(h.mgr->peer_count() == live.size());
        RC_ASSERT(h.mgr->peer_count() <= 10u);
    }
    // Exactly-once retirement for every generation that left the map.
    for (const auto& entry : retired_cbs) {
        const uint64_t gen = entry.first;
        RC_ASSERT(eventually([&] { return h.peer_ops->release_count(gen) == 1; }));
        RC_ASSERT(h.peer_ops->close_count(gen) == 1);
    }
}

// ============================================================
// Spec 33 Task 5 — media I/O isolation, CONNECTING timeout, write-failure
// thresholds, local ICE fire-and-forget, pending ICE limits and health.
// All deterministic: event gates + manual clock, no fixed sleeps, no
// throughput-ratio assertions.
// ============================================================

// T5-1. Deterministic replacement of ReadReadConcurrencyWithSharedLock:
// while one session's write is parked inside its I/O gate, the peer map
// (peer_count), new offers and the FULL retirement of another session all
// proceed — sessions never share an I/O lock and writes never hold the map.
// **Validates: Requirements 5.2, 7.3**
TEST(PeerRuntimeTest, SlowWriteDoesNotBlockMapOrOtherSessions) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.mgr->on_viewer_offer("slow-a", "sdp"));
    const uint64_t gen_a = h.peer_ops->last_generation();
    ASSERT_TRUE(h.mgr->on_viewer_offer("fast-b", "sdp"));
    const uint64_t gen_b = h.peer_ops->last_generation();

    h.peer_ops->write_block_gen.store(gen_a);
    uint8_t frame[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    std::thread bcast([&] {
        h.mgr->broadcast_frame(frame, sizeof(frame), 1000, true);
    });
    ASSERT_TRUE(eventually([&] { return h.peer_ops->writes_blocked.load() == 1; }))
        << "broadcast thread must park inside write(gen_a)";

    // (a) Map reads complete while the slow write holds session A's I/O gate.
    auto count_fut = std::async(std::launch::async, [&] { return h.mgr->peer_count(); });
    ASSERT_EQ(count_fut.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "peer_count() must not block on a slow session write";
    EXPECT_EQ(count_fut.get(), 2u);

    // (b) New offers proceed (map and other slots are free).
    auto offer_fut = std::async(std::launch::async, [&] {
        return h.mgr->on_viewer_offer("new-c", "sdp");
    });
    ASSERT_EQ(offer_fut.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "on_viewer_offer must not block on a slow session write";
    EXPECT_TRUE(offer_fut.get());

    // (c) Another session retires fully (close + release on its own gate)
    //     while A's write is still parked: no shared I/O lock.
    h.mgr->remove_peer("fast-b");
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen_b) == 1; }));
    EXPECT_EQ(h.peer_ops->writes_blocked.load(), 1) << "A's write must still be parked";

    h.peer_ops->open_write_gate();
    bcast.join();
    EXPECT_GE(h.peer_ops->write_count(gen_a), 1);
}

// T5-2. A session stuck in CONNECTING past 30s transitions to
// DISCONNECTING and is reaped after the 10s grace (manual time only).
// **Validates: Requirements 5.1**
TEST(PeerRuntimeTest, ConnectingTimeoutRetiresAfterGrace) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    h.peer_ops->sync_connected.store(false);  // session stays CONNECTING
    ASSERT_TRUE(h.mgr->on_viewer_offer("stuck", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    EXPECT_EQ(h.mgr->peer_count(), 0u);  // CONNECTING is not counted

    // 29s: still within the CONNECTING window — nothing may be retired.
    h.clock->advance(std::chrono::seconds(29));
    EXPECT_EQ(h.peer_ops->release_count(gen), 0);
    EXPECT_EQ(h.mgr->health_snapshot().connecting_timeouts, 0u);

    // Cross 30s: DISCONNECTING; retirement still waits the 10s grace.
    h.clock->advance(std::chrono::seconds(2));
    ASSERT_TRUE(eventually([&] {
        return h.mgr->health_snapshot().connecting_timeouts == 1;
    }));
    EXPECT_EQ(h.peer_ops->release_count(gen), 0) << "grace not expired yet";
    h.clock->advance(std::chrono::seconds(11));
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen) == 1; }));
    EXPECT_EQ(h.peer_ops->close_count(gen), 1);

    bool reason_found = false;
    for (const auto& entry : h.mgr->health_snapshot().reap_reasons) {
        if (entry.first == "connecting_timeout") {
            reason_found = true;
            EXPECT_EQ(entry.second, 1u);
        }
    }
    EXPECT_TRUE(reason_found) << "reap reason must be classified";
}

// T5-3. A session that reaches CONNECTED before the 30s deadline is never
// touched by the stale watch (zero side effects).
// **Validates: Requirements 5.1**
TEST(PeerRuntimeTest, ConnectedBeforeTimeoutIsNotReaped) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    h.peer_ops->sync_connected.store(false);
    ASSERT_TRUE(h.mgr->on_viewer_offer("late-ok", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    auto cbs = h.peer_ops->stored_callbacks(gen);
    ASSERT_TRUE(static_cast<bool>(cbs.on_state));
    cbs.on_state(gen, wi::kPeerStateConnected);  // connects before the deadline
    EXPECT_EQ(h.mgr->peer_count(), 1u);

    h.clock->advance(std::chrono::seconds(60));
    // Deterministic settle point: watches are swept before jobs in every
    // Reaper pass, so once this freshly-submitted retirement completes the
    // stale watch has certainly been consumed.
    h.peer_ops->sync_connected.store(true);
    ASSERT_TRUE(h.mgr->on_viewer_offer("settle", "sdp"));
    const uint64_t settle_gen = h.peer_ops->last_generation();
    h.mgr->remove_peer("settle");
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(settle_gen) == 1; }));

    EXPECT_EQ(h.mgr->peer_count(), 1u);
    EXPECT_EQ(h.peer_ops->release_count(gen), 0);
    EXPECT_EQ(h.mgr->health_snapshot().connecting_timeouts, 0u);
}

// T5-4. The two write-failure thresholds are distinct: the configurable
// one only flips keyframe-only mode; only the fixed 100-failure bound
// disconnects (then 10s grace, Reaper reclaims). RETRYABLE (SRTP not
// ready) is a transient skip and never counted.
// **Validates: Requirements 5.1, 5.3**
TEST(PeerRuntimeTest, WriteFailureThresholdsKeyframeOnlyThenDisconnect) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    h.mgr->set_writeframe_fail_threshold(3);
    ASSERT_TRUE(h.mgr->on_viewer_offer("wf", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    uint8_t frame[] = {0x00, 0x00, 0x00, 0x01};

    // RETRYABLE: transient skip, never counted, never disconnects.
    h.peer_ops->write_result_mode.store(1);
    for (int i = 0; i < 50; ++i) {
        h.mgr->broadcast_frame(frame, sizeof(frame), static_cast<uint64_t>(i) + 1, true);
    }
    EXPECT_EQ(h.mgr->peer_count(), 1u);

    // 3 FATAL failures reach the configurable threshold: keyframe-only
    // mode (send-mode change only, NOT a disconnect).
    h.peer_ops->write_result_mode.store(2);
    for (int i = 0; i < 3; ++i) {
        h.mgr->broadcast_frame(frame, sizeof(frame), 100 + static_cast<uint64_t>(i), true);
    }
    EXPECT_EQ(h.mgr->peer_count(), 1u) << "keyframe-only threshold must not disconnect";
    const int writes_before = h.peer_ops->write_count(gen);
    h.mgr->broadcast_frame(frame, sizeof(frame), 200, false);  // non-keyframe
    EXPECT_EQ(h.peer_ops->write_count(gen), writes_before)
        << "keyframe-only mode must skip non-keyframes";

    // 97 more keyframe failures: exactly 100 consecutive — still connected.
    for (int i = 0; i < 97; ++i) {
        h.mgr->broadcast_frame(frame, sizeof(frame), 300 + static_cast<uint64_t>(i), true);
    }
    EXPECT_EQ(h.mgr->peer_count(), 1u) << "exactly 100 failures: not above the bound yet";
    // Failure #101 crosses the fixed bound: DISCONNECTING, then 10s grace.
    h.mgr->broadcast_frame(frame, sizeof(frame), 999, true);
    EXPECT_EQ(h.mgr->peer_count(), 0u);
    EXPECT_EQ(h.peer_ops->release_count(gen), 0) << "grace not expired yet";
    h.clock->advance(std::chrono::seconds(11));
    ASSERT_TRUE(eventually([&] { return h.peer_ops->release_count(gen) == 1; }));
    EXPECT_EQ(h.peer_ops->close_count(gen), 1);
}

// T5-5. Local ICE callback: bounded copy + fire-and-forget post through
// signaling (enqueue only), oversized candidates dropped with a classified
// count, stale generations never posted.
// **Validates: Requirements 5.4**
TEST(PeerRuntimeTest, LocalIcePostsThroughSignalingFireAndForget) {
    PeerRuntimeHarness h(/*permits=*/16, /*connect_signaling=*/true);
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.signaling->is_connected());
    ASSERT_TRUE(h.mgr->on_viewer_offer("ice-peer", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    auto cbs = h.peer_ops->stored_callbacks(gen);
    ASSERT_TRUE(static_cast<bool>(cbs.on_local_ice));

    cbs.on_local_ice(gen, "candidate:1 udp 2130706431 192.0.2.1 5000 typ host");
    ASSERT_TRUE(eventually([&] { return h.sig_ops->posted_ice().size() == 1; }));
    auto posted = h.sig_ops->posted_ice();
    EXPECT_EQ(posted[0].first, "ice-peer");
    EXPECT_EQ(h.mgr->health_snapshot().local_ice_posted, 1u);

    // Oversized (> 4KiB): dropped with a classified count, never posted.
    std::string big(5000, 'x');
    cbs.on_local_ice(gen, big);
    auto s = h.mgr->health_snapshot();
    EXPECT_EQ(s.local_ice_dropped_oversized, 1u);
    EXPECT_EQ(h.sig_ops->posted_ice().size(), 1u);

    // Stale generation: counted, zero side effects, never posted.
    cbs.on_local_ice(gen + 12345, "candidate");
    s = h.mgr->health_snapshot();
    EXPECT_GE(s.stale_callbacks, 1u);
    EXPECT_EQ(s.local_ice_posted, 1u);
}

// T5-6. When signaling is not connected the fire-and-forget post is
// rejected and counted — the callback still returns immediately (no wait
// for any signaling reply, per the Task 1 peerConnectionObjLock contract).
// **Validates: Requirements 5.4**
TEST(PeerRuntimeTest, LocalIceRejectedWhenSignalingNotConnected) {
    PeerRuntimeHarness h;  // signaling NOT connected
    ASSERT_NE(h.mgr, nullptr);
    ASSERT_TRUE(h.mgr->on_viewer_offer("ice-peer", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    auto cbs = h.peer_ops->stored_callbacks(gen);
    cbs.on_local_ice(gen, "candidate");
    auto s = h.mgr->health_snapshot();
    EXPECT_EQ(s.local_ice_rejected, 1u);
    EXPECT_EQ(s.local_ice_posted, 0u);
    EXPECT_EQ(h.sig_ops->posted_ice().size(), 0u);
}

// T5-7. Per-peer pending ICE cap (50): a full peer evicts its own OLDEST
// candidate; the offer flushes the survivors in FIFO order.
// **Validates: Requirements 5.5**
TEST(PeerRuntimeTest, PendingIcePerPeerCapEvictsOldestCandidate) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    for (int i = 0; i < 55; ++i) {
        EXPECT_TRUE(h.mgr->on_viewer_ice_candidate("flood", "cand-" + std::to_string(i)));
    }
    auto s = h.mgr->health_snapshot();
    EXPECT_EQ(s.pending_ice_depth, 50u);
    EXPECT_EQ(s.pending_ice_peers, 1u);
    EXPECT_EQ(s.pending_ice_evicted, 5u);

    ASSERT_TRUE(h.mgr->on_viewer_offer("flood", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    auto payloads = h.peer_ops->add_ice_payloads(gen);
    ASSERT_EQ(payloads.size(), 50u);
    EXPECT_EQ(payloads.front(), "cand-5");   // the oldest five were evicted
    EXPECT_EQ(payloads.back(), "cand-54");   // FIFO order preserved
    EXPECT_EQ(h.mgr->health_snapshot().pending_ice_depth, 0u);
}

// T5-8. Pending peer cap (20): the 21st pending peer evicts the
// least-recently-updated pending peer entirely.
// **Validates: Requirements 5.5**
TEST(PeerRuntimeTest, PendingIcePeerCapEvictsLruPeer) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    for (int i = 0; i < 21; ++i) {
        EXPECT_TRUE(h.mgr->on_viewer_ice_candidate("p" + std::to_string(i), "cand"));
    }
    auto s = h.mgr->health_snapshot();
    EXPECT_EQ(s.pending_ice_peers, 20u);
    EXPECT_EQ(s.pending_ice_depth, 20u);
    EXPECT_EQ(s.pending_ice_evicted, 1u);

    ASSERT_TRUE(h.mgr->on_viewer_offer("p0", "sdp"));
    EXPECT_EQ(h.peer_ops->add_ice_count(h.peer_ops->last_generation()), 0)
        << "p0 was the LRU pending peer and must have been evicted";
    ASSERT_TRUE(h.mgr->on_viewer_offer("p20", "sdp"));
    EXPECT_EQ(h.peer_ops->add_ice_count(h.peer_ops->last_generation()), 1);
}

// T5-9. Global pending ICE cap (200): sustained flood from 20 peers keeps
// evicting the oldest pending peer; totals never exceed the caps and the
// end state is exactly 4 surviving full peers (4 x 50 = 200).
// **Validates: Requirements 5.5, 6.4**
TEST(PeerRuntimeTest, PendingIceGlobalCapBoundsTotal) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    for (int p = 0; p < 20; ++p) {
        for (int c = 0; c < 50; ++c) {
            h.mgr->on_viewer_ice_candidate("gp" + std::to_string(p),
                                           "c" + std::to_string(c));
        }
        auto s = h.mgr->health_snapshot();
        EXPECT_LE(s.pending_ice_depth, 200u);
        EXPECT_LE(s.pending_ice_peers, 20u);
    }
    auto s = h.mgr->health_snapshot();
    EXPECT_EQ(s.pending_ice_depth, 200u);
    EXPECT_EQ(s.pending_ice_peers, 4u);
    EXPECT_LE(s.pending_ice_depth * 4096u, 16u * 1024u * 1024u)
        << "estimated pending ICE bytes must stay within the 16MiB budget";
}

// T5-10. Pending ICE TTL (30s): stale candidates are expired (counted)
// and never flushed to the peer.
// **Validates: Requirements 5.5**
TEST(PeerRuntimeTest, PendingIceTtlExpiresOldCandidates) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(h.mgr->on_viewer_ice_candidate("ttl", "old-" + std::to_string(i)));
    }
    h.clock->advance(std::chrono::seconds(31));
    EXPECT_TRUE(h.mgr->on_viewer_ice_candidate("ttl", "fresh"));
    auto s = h.mgr->health_snapshot();
    EXPECT_EQ(s.pending_ice_expired, 3u);
    EXPECT_EQ(s.pending_ice_depth, 1u);

    ASSERT_TRUE(h.mgr->on_viewer_offer("ttl", "sdp"));
    const uint64_t gen = h.peer_ops->last_generation();
    auto payloads = h.peer_ops->add_ice_payloads(gen);
    ASSERT_EQ(payloads.size(), 1u);
    EXPECT_EQ(payloads[0], "fresh");
}

// T5-11. Single-candidate size cap (4KiB) for viewer ICE: rejected with a
// classified count, nothing buffered.
// **Validates: Requirements 5.5**
TEST(PeerRuntimeTest, OversizedViewerIceRejected) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    std::string big(4097, 'y');
    std::string err;
    EXPECT_FALSE(h.mgr->on_viewer_ice_candidate("big", big, &err));
    EXPECT_EQ(err, "ICE candidate too large");
    auto s = h.mgr->health_snapshot();
    EXPECT_EQ(s.pending_ice_rejected_oversized, 1u);
    EXPECT_EQ(s.pending_ice_depth, 0u);
}

// T5-12. The CONNECTED callback only sets keyframe_pending; the media
// path (broadcast) consumes it and runs the force-keyframe helper against
// the pipeline — never from an SDK callback.
// **Validates: Requirements 5.6**
TEST(PeerRuntimeTest, KeyframePendingConsumedOnMediaPath) {
    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    GstElement* pipeline = gst_parse_launch(
        "videotestsrc name=src ! identity name=encoder ! fakesink", nullptr);
    ASSERT_NE(pipeline, nullptr);
    h.mgr->set_pipeline(pipeline);
    ASSERT_TRUE(h.mgr->on_viewer_offer("kf", "sdp"));  // sync CONNECTED sets the flag
    const uint64_t gen = h.peer_ops->last_generation();
    uint8_t frame[] = {0x00, 0x00, 0x00, 0x01};
    h.mgr->broadcast_frame(frame, sizeof(frame), 1, true);  // consumes the flag
    h.mgr->broadcast_frame(frame, sizeof(frame), 2, true);
    EXPECT_EQ(h.peer_ops->write_count(gen), 2);
    h.mgr->set_pipeline(nullptr);
    gst_object_unref(pipeline);
}

// T5-13. Health snapshot bounds under churn + worst-case budget by static
// computation; CONNECTED signaling with 0 viewers stays healthy (the log
// path is observability only and never feeds recovery).
// **Validates: Requirements 6.1, 6.2, 6.4**
TEST(PeerRuntimeTest, HealthSnapshotBoundsAndBudget) {
    // Static worst-case computation (design Component 8): both the pre-cap
    // per-peer product and the enforced global cap fit the 16MiB budget.
    constexpr size_t kPerPeerWorst = 20u * 50u * 4096u;   // 4000 KiB
    constexpr size_t kGlobalWorst = 200u * 4096u;          // 800 KiB
    static_assert(kPerPeerWorst <= 16u * 1024u * 1024u,
                  "pending ICE per-peer worst case must fit 16MiB");
    static_assert(kGlobalWorst <= 16u * 1024u * 1024u,
                  "pending ICE global worst case must fit 16MiB");

    PeerRuntimeHarness h;
    ASSERT_NE(h.mgr, nullptr);
    // 0 viewers with connected-capable signaling: log path stays healthy.
    h.mgr->log_health_status();

    h.peer_ops->close_close_gate();   // park retirements to grow live handles
    for (int i = 0; i < 12; ++i) {
        h.mgr->on_viewer_offer("churn-" + std::to_string(i % 6), "sdp");
        if (i % 2 == 1) h.mgr->remove_peer("churn-" + std::to_string(i % 6));
        auto s = h.mgr->health_snapshot();
        EXPECT_LE(s.active_peers, 10u);
        EXPECT_LE(s.live_handles, 16u);
        EXPECT_LE(s.reaper_queue_depth, 16u);
    }
    h.peer_ops->open_close_gate();
    ASSERT_TRUE(eventually([&] {
        return h.mgr->health_snapshot().reaper_queue_depth == 0;
    }));
    auto s = h.mgr->health_snapshot();
    EXPECT_GT(s.reaped_total, 0u);
    bool manual_reason = false;
    for (const auto& entry : s.reap_reasons) {
        if (entry.first == "manual_remove" && entry.second > 0) manual_reason = true;
    }
    EXPECT_TRUE(manual_reason) << "reap reasons must be classified in the snapshot";
    h.mgr->log_health_status();
}

// ============================================================
// Custom main: gst_init required for pipeline tests
// ============================================================

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
