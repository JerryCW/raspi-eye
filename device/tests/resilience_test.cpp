// resilience_test.cpp
// Spec 32 管道韧性加固 — 纯函数 + 有界 teardown helper 单测。
// 覆盖 Correctness Properties 1-5（classify_bus_error / should_trigger_recovery /
// open_with_retry / validate_streaming_config）+ Task 3（release / teardown helper）。
#include "pipeline_health.h"
#include "camera_source.h"
#include "config_manager.h"
#include "pipeline_manager.h"
#include <gtest/gtest.h>
#include <gst/gst.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// ===========================================================================
// Property 1 + 2: classify_bus_error 全覆盖、确定、TRUNK 保守
// ===========================================================================

TEST(ResilienceClassify, KvsBranchElements) {
    EXPECT_EQ(classify_bus_error("q-kvs"), ErrorScope::KVS_BRANCH);
    EXPECT_EQ(classify_bus_error("kvs-parser"), ErrorScope::KVS_BRANCH);
    EXPECT_EQ(classify_bus_error("avc-caps"), ErrorScope::KVS_BRANCH);
    EXPECT_EQ(classify_bus_error("kvs-sink"), ErrorScope::KVS_BRANCH);
}

TEST(ResilienceClassify, WebRtcBranchElements) {
    EXPECT_EQ(classify_bus_error("q-web"), ErrorScope::WEBRTC_BRANCH);
    EXPECT_EQ(classify_bus_error("webrtc-sink"), ErrorScope::WEBRTC_BRANCH);
}

TEST(ResilienceClassify, TrunkElementsAndUnknown) {
    // Trunk 元素
    EXPECT_EQ(classify_bus_error("src"), ErrorScope::TRUNK);
    EXPECT_EQ(classify_bus_error("v4l2-source"), ErrorScope::TRUNK);
    EXPECT_EQ(classify_bus_error("encoder"), ErrorScope::TRUNK);
    EXPECT_EQ(classify_bus_error("encoded-tee"), ErrorScope::TRUNK);
    // 空串 / 未知名 -> TRUNK（保守）
    EXPECT_EQ(classify_bus_error(""), ErrorScope::TRUNK);
    EXPECT_EQ(classify_bus_error("totally-unknown-xyz"), ErrorScope::TRUNK);
}

// Property 1: 同输入恒同输出（确定性）；已知集合恒返回对应 scope
RC_GTEST_PROP(ResilienceClassifyPBT, Deterministic, (const std::string& name)) {
    ErrorScope a = classify_bus_error(name);
    ErrorScope b = classify_bus_error(name);
    RC_ASSERT(a == b);
}

// Property 2: TRUNK 是保守默认 —— 任意不在白名单内的串 -> TRUNK
RC_GTEST_PROP(ResilienceClassifyPBT, UnknownIsTrunk, (const std::string& name)) {
    static const std::vector<std::string> kvs = {
        "q-kvs", "kvs-parser", "avc-caps", "kvs-sink"};
    static const std::vector<std::string> web = {"q-web", "webrtc-sink"};

    bool is_kvs = false, is_web = false;
    for (const auto& s : kvs) if (s == name) is_kvs = true;
    for (const auto& s : web) if (s == name) is_web = true;

    ErrorScope scope = classify_bus_error(name);
    if (is_kvs) {
        RC_ASSERT(scope == ErrorScope::KVS_BRANCH);
    } else if (is_web) {
        RC_ASSERT(scope == ErrorScope::WEBRTC_BRANCH);
    } else {
        RC_ASSERT(scope == ErrorScope::TRUNK);
    }
}

// ===========================================================================
// Property 3: should_trigger_recovery 单调阈值
// ===========================================================================

TEST(ResilienceShouldTrigger, ThresholdBoundary) {
    EXPECT_FALSE(should_trigger_recovery(0, 3));
    EXPECT_FALSE(should_trigger_recovery(2, 3));
    EXPECT_TRUE(should_trigger_recovery(3, 3));
    EXPECT_TRUE(should_trigger_recovery(4, 3));
    // threshold <= 0 永远不触发
    EXPECT_FALSE(should_trigger_recovery(5, 0));
    EXPECT_FALSE(should_trigger_recovery(5, -1));
}

RC_GTEST_PROP(ResilienceShouldTriggerPBT, MonotoneThreshold, ()) {
    int c = *rc::gen::inRange(-10, 100);
    int t = *rc::gen::inRange(-5, 20);
    bool expected = (t > 0 && c >= t);
    RC_ASSERT(should_trigger_recovery(c, t) == expected);
}

// ===========================================================================
// Property 4: open_with_retry 次数有界且正确
// ===========================================================================

TEST(ResilienceOpenRetry, SucceedsFirstTry) {
    CameraSource::OpenRetryConfig cfg{6, 500};
    int attempts = 0;
    bool ok = CameraSource::open_with_retry(
        [] { return true; }, cfg, [](int) {}, &attempts);
    EXPECT_TRUE(ok);
    EXPECT_EQ(attempts, 1);
}

TEST(ResilienceOpenRetry, SucceedsOnThirdTry) {
    CameraSource::OpenRetryConfig cfg{6, 500};
    int calls = 0;
    int attempts = 0;
    bool ok = CameraSource::open_with_retry(
        [&] { return ++calls >= 3; }, cfg, [](int) {}, &attempts);
    EXPECT_TRUE(ok);
    EXPECT_EQ(attempts, 3);
}

TEST(ResilienceOpenRetry, AllFailExhaustsAttempts) {
    CameraSource::OpenRetryConfig cfg{6, 500};
    int attempts = 0;
    bool ok = CameraSource::open_with_retry(
        [] { return false; }, cfg, [](int) {}, &attempts);
    EXPECT_FALSE(ok);
    EXPECT_EQ(attempts, 6);
}

TEST(ResilienceOpenRetry, InjectedSleepNotCalledAfterLastFailure) {
    CameraSource::OpenRetryConfig cfg{3, 500};
    int sleeps = 0;
    int attempts = 0;
    // 全失败：3 次尝试，但只在前 2 次失败后 sleep（最后一次不 sleep）
    CameraSource::open_with_retry(
        [] { return false; }, cfg, [&](int) { ++sleeps; }, &attempts);
    EXPECT_EQ(attempts, 3);
    EXPECT_EQ(sleeps, 2);
}

// Property 4: 任意 max_attempts >= 1，调用次数 ∈ [1, max_attempts]；
// 前 k 次失败、第 k+1 次成功则恰好 k+1 次；全失败恰好 max_attempts 次。
RC_GTEST_PROP(ResilienceOpenRetryPBT, BoundedAndCorrect, ()) {
    int max_attempts = *rc::gen::inRange(1, 20);
    // succeed_at: 1..max_attempts => 第几次成功；> max_attempts => 全失败
    int succeed_at = *rc::gen::inRange(1, max_attempts + 2);

    CameraSource::OpenRetryConfig cfg{max_attempts, 0};
    int calls = 0;
    int attempts = 0;
    bool ok = CameraSource::open_with_retry(
        [&] { return ++calls >= succeed_at; }, cfg, [](int) {}, &attempts);

    RC_ASSERT(attempts >= 1);
    RC_ASSERT(attempts <= max_attempts);
    if (succeed_at <= max_attempts) {
        RC_ASSERT(ok);
        RC_ASSERT(attempts == succeed_at);
    } else {
        RC_ASSERT(!ok);
        RC_ASSERT(attempts == max_attempts);
    }
}

// ===========================================================================
// Property 5: validate_streaming_config 区间一致性
// ===========================================================================

TEST(ResilienceValidate, NewDefaultsConsistent) {
    StreamingConfig sc;  // 使用新默认值 800/1200/1500
    EXPECT_EQ(sc.bitrate_min_kbps, 800);
    EXPECT_EQ(sc.bitrate_default_kbps, 1200);
    EXPECT_EQ(sc.bitrate_max_kbps, 1500);
    std::string err;
    EXPECT_TRUE(validate_streaming_config(sc, &err));
}

RC_GTEST_PROP(ResilienceValidatePBT, IntervalConsistency, ()) {
    StreamingConfig sc;
    sc.bitrate_min_kbps     = *rc::gen::inRange(0, 10001);
    sc.bitrate_default_kbps = *rc::gen::inRange(0, 10001);
    sc.bitrate_max_kbps     = *rc::gen::inRange(0, 10001);
    std::string err;
    bool expected = (sc.bitrate_min_kbps <= sc.bitrate_default_kbps &&
                     sc.bitrate_default_kbps <= sc.bitrate_max_kbps);
    RC_ASSERT(validate_streaming_config(sc, &err) == expected);
}

// ===========================================================================
// Task 3: PipelineManager::release() —— 释放后壳为空，原指针仍有效
// ===========================================================================

TEST(ResilienceRelease, ShellEmptyOriginalValid) {
    GstElement* p = gst_parse_launch(
        "videotestsrc name=src is-live=true ! fakesink", nullptr);
    ASSERT_NE(p, nullptr);
    gst_element_set_state(p, GST_STATE_PLAYING);

    auto pm = PipelineManager::create(p, nullptr);
    ASSERT_NE(pm, nullptr);
    EXPECT_EQ(pm->pipeline(), p);

    GstElement* raw = pm->release();
    EXPECT_EQ(raw, p);            // 返回原裸指针
    EXPECT_EQ(pm->pipeline(), nullptr);  // 壳已空

    // 原指针仍有效（release 不 stop/unref），可继续操作
    EXPECT_TRUE(GST_IS_ELEMENT(raw));
    gst_element_set_state(raw, GST_STATE_NULL);
    gst_object_unref(raw);

    // pm 析构对空壳安全（不会 double-free）
    pm.reset();
}

// ===========================================================================
// Task 3: 有界 teardown helper —— 快完成返回 true，慢操作超时返回 false 且主线程不被阻塞
// ===========================================================================

TEST(ResilienceTeardown, FastCompletesWithinBudget) {
    // 注入快 set_null fn（立即返回），预算 1000ms
    auto start = std::chrono::steady_clock::now();
    bool ok = set_null_bounded(nullptr, 1000, [](GstElement*) {
        // 立即完成
    });
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(ok);
    EXPECT_LT(elapsed, 500);  // 远小于预算
}

TEST(ResilienceTeardown, SlowTimesOutMainThreadNotBlocked) {
    // 注入慢 set_null fn（2000ms），预算仅 300ms
    std::atomic<bool> worker_done{false};
    auto start = std::chrono::steady_clock::now();
    bool ok = set_null_bounded(nullptr, 300, [&](GstElement*) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        worker_done.store(true);
    });
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_FALSE(ok);                 // 超时返回 false
    EXPECT_GE(elapsed, 250);          // 至少等了预算
    EXPECT_LT(elapsed, 1500);         // 但远小于慢操作的 2000ms（主线程未被阻塞）
    // 等后台 worker 完成，避免 detach 线程在进程退出时访问已销毁对象
    while (!worker_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

TEST(ResilienceTeardown, TeardownOwnershipFastCompletes) {
    std::atomic<int> teardown_calls{0};
    bool ok = teardown_pipeline_bounded(nullptr, 1000, [&](GstElement*) {
        ++teardown_calls;  // 立即完成（不真 unref，因为传 nullptr）
    });
    EXPECT_TRUE(ok);
    EXPECT_EQ(teardown_calls.load(), 1);
}

// ---------------------------------------------------------------------------
// Custom main: gst_init required before any GStreamer API calls
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
