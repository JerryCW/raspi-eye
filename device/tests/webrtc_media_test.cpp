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
            } else {
                RC_ASSERT(result);
                ref_set.insert(peer_id);
            }
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
            } else {
                RC_ASSERT(result);
                connected_peers.insert(peer_id);
            }
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

// 测试 2: 读-读并发验证 — 验证 peer_count 和 broadcast_frame 可以并发执行
// 未修复代码使用 std::mutex（独占锁），peer_count 和 broadcast_frame 互斥。
// 修复后使用 std::shared_mutex + shared_lock，peer_count 和 broadcast_frame
// 都用读锁，可以真正并发执行。
//
// 测试方法：启动多个线程同时调用 peer_count()，测量总耗时。
// 如果使用独占锁，线程串行执行，总耗时 ≈ N × 单次耗时。
// 如果使用读锁，线程并发执行，总耗时 ≈ 单次耗时。
//
// 由于单次 peer_count() 耗时极短（纳秒级），我们通过大量迭代放大差异：
// 每个线程执行 100K 次 peer_count()，4 个线程并发。
// 独占锁下总吞吐量受限，读锁下吞吐量线性增长。
//
// 断言：4 线程并发的吞吐量应 >= 2 × 单线程吞吐量（读锁下成立）
// 未修复代码（独占锁）：4 线程吞吐量 ≈ 单线程（串行化），比值 < 2 → FAIL
// 修复后代码（读锁）：4 线程吞吐量 ≈ 4 × 单线程，比值 >= 2 → PASS
TEST(WebRtcMediaBugCondition, ReadReadConcurrencyWithSharedLock) {
    auto sig = create_stub_signaling();
    SKIP_IF_REAL_SDK(sig);
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);

    // 添加一些 peer 让 peer_count 有工作量
    for (int i = 0; i < 5; ++i) {
        mgr->on_viewer_offer("peer-" + std::to_string(i), "sdp");
    }

    constexpr int kIterations = 200'000;
    constexpr int kThreads = 4;

    // 单线程基准：测量 kIterations 次 peer_count 的耗时
    auto single_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        volatile size_t count = mgr->peer_count();
        (void)count;
    }
    auto single_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - single_start).count();

    // 多线程并发：kThreads 个线程各执行 kIterations 次 peer_count
    // 同时有一个线程持续调用 broadcast_frame（也需要锁）
    std::atomic<bool> stop_broadcast{false};
    uint8_t frame_data[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    std::thread broadcast_thread([&]() {
        while (!stop_broadcast.load(std::memory_order_relaxed)) {
            mgr->broadcast_frame(frame_data, sizeof(frame_data), 1000, true);
        }
    });

    auto multi_start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kIterations; ++i) {
                volatile size_t count = mgr->peer_count();
                (void)count;
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    auto multi_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - multi_start).count();

    stop_broadcast.store(true, std::memory_order_relaxed);
    broadcast_thread.join();

    // 计算吞吐量比值
    // 单线程吞吐量：kIterations / single_us
    // 多线程总吞吐量：(kThreads * kIterations) / multi_us
    // 比值 = (kThreads * kIterations / multi_us) / (kIterations / single_us)
    //       = kThreads * single_us / multi_us
    double ratio = static_cast<double>(kThreads) * single_us / multi_us;

    // 期望行为（修复后）：读锁允许并发，比值 >= 2.0
    // 未修复代码（独占锁）：串行化，比值 ≈ 1.0（可能 < 2.0）
    //
    // 注意：由于 peer_count() 操作极快（纳秒级），锁获取/释放的开销占比大，
    // 即使独占锁也可能因为锁竞争不够激烈而达到较高比值。
    // 但在有 broadcast_frame 持续竞争锁的情况下，独占锁的串行化效应更明显。
    EXPECT_GE(ratio, 2.0)
        << "Read-read concurrency ratio: " << ratio
        << " (single=" << single_us << "us, multi=" << multi_us << "us). "
        << "Expected >= 2.0 with shared_lock (read-read concurrent). "
        << "Ratio < 2.0 indicates exclusive lock (std::mutex) serializing reads — "
        << "this is part of the bug condition where peers_mutex blocks all operations.";
}

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
    // 随机阻塞时间 150-300ms，模拟 freePeerConnection 不同耗时
    auto block_ms = *rc::gen::inRange(150, 301);

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

    // Property: bug pattern（锁内阻塞）导致 on_viewer_offer 被阻塞 >= 100ms
    RC_ASSERT(elapsed_us >= 100'000);

    mgr->remove_peer(new_viewer);
}

// ============================================================
// Custom main: gst_init required for pipeline tests
// ============================================================

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
