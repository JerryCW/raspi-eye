// stream_mode_test.cpp
// StreamModeController example-based tests + PBT properties.
// Custom main(): gst_init required before any GStreamer API calls.
#include "stream_mode_controller.h"

#include <gst/gst.h>
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <chrono>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Expected mapping tables (ground truth for verification)
// ---------------------------------------------------------------------------

static StreamMode expected_target_mode(BranchStatus kvs, BranchStatus web) {
    if (kvs == BranchStatus::HEALTHY && web == BranchStatus::HEALTHY)
        return StreamMode::FULL;
    if (kvs == BranchStatus::HEALTHY && web == BranchStatus::UNHEALTHY)
        return StreamMode::KVS_ONLY;
    if (kvs == BranchStatus::UNHEALTHY && web == BranchStatus::HEALTHY)
        return StreamMode::WEBRTC_ONLY;
    return StreamMode::DEGRADED;
}

static BranchQueueParams expected_queue_params(StreamMode mode) {
    switch (mode) {
        case StreamMode::FULL:         return {{1, 0}, {1, 2}};
        case StreamMode::KVS_ONLY:     return {{1, 0}, {0, 2}};
        case StreamMode::WEBRTC_ONLY:  return {{0, 2}, {1, 2}};
        case StreamMode::DEGRADED:     return {{1, 0}, {0, 2}};
    }
    return {{1, 0}, {1, 2}};
}

// ---------------------------------------------------------------------------
// Example-based tests
// ---------------------------------------------------------------------------

// 1. ComputeTargetMode_AllCombinations
//    Validates: Requirements 1.3, 1.4, 1.5, 1.6
TEST(StreamModeTest, ComputeTargetMode_AllCombinations) {
    EXPECT_EQ(compute_target_mode(BranchStatus::HEALTHY, BranchStatus::HEALTHY),
              StreamMode::FULL);
    EXPECT_EQ(compute_target_mode(BranchStatus::HEALTHY, BranchStatus::UNHEALTHY),
              StreamMode::KVS_ONLY);
    EXPECT_EQ(compute_target_mode(BranchStatus::UNHEALTHY, BranchStatus::HEALTHY),
              StreamMode::WEBRTC_ONLY);
    EXPECT_EQ(compute_target_mode(BranchStatus::UNHEALTHY, BranchStatus::UNHEALTHY),
              StreamMode::DEGRADED);
}

// 2. ComputeQueueParams_AllModes
//    Validates: Requirements 2.1, 2.2, 2.3, 2.4
TEST(StreamModeTest, ComputeQueueParams_AllModes) {
    // FULL: q-kvs(1,0), q-web(1,2)
    auto full = compute_queue_params(StreamMode::FULL);
    EXPECT_EQ(full.kvs.max_size_buffers, 1);
    EXPECT_EQ(full.kvs.leaky, 0);
    EXPECT_EQ(full.web.max_size_buffers, 1);
    EXPECT_EQ(full.web.leaky, 2);

    // KVS_ONLY: q-kvs(1,0), q-web(0,2)
    auto kvs_only = compute_queue_params(StreamMode::KVS_ONLY);
    EXPECT_EQ(kvs_only.kvs.max_size_buffers, 1);
    EXPECT_EQ(kvs_only.kvs.leaky, 0);
    EXPECT_EQ(kvs_only.web.max_size_buffers, 0);
    EXPECT_EQ(kvs_only.web.leaky, 2);

    // WEBRTC_ONLY: q-kvs(0,2), q-web(1,2)
    auto webrtc_only = compute_queue_params(StreamMode::WEBRTC_ONLY);
    EXPECT_EQ(webrtc_only.kvs.max_size_buffers, 0);
    EXPECT_EQ(webrtc_only.kvs.leaky, 2);
    EXPECT_EQ(webrtc_only.web.max_size_buffers, 1);
    EXPECT_EQ(webrtc_only.web.leaky, 2);

    // DEGRADED: q-kvs(1,0), q-web(0,2)
    auto degraded = compute_queue_params(StreamMode::DEGRADED);
    EXPECT_EQ(degraded.kvs.max_size_buffers, 1);
    EXPECT_EQ(degraded.kvs.leaky, 0);
    EXPECT_EQ(degraded.web.max_size_buffers, 0);
    EXPECT_EQ(degraded.web.leaky, 2);
}

// 3. InitialMode_IsFull
//    Validates: Requirement 1.2
TEST(StreamModeTest, InitialMode_IsFull) {
    StreamModeController ctrl(nullptr);
    EXPECT_EQ(ctrl.current_mode(), StreamMode::FULL);
}

// 4. ModeChangedCallback_Invoked
//    Validates: Requirement 1.8
TEST(StreamModeTest, ModeChangedCallback_Invoked) {
    // Create a minimal pipeline (videotestsrc ! fakesink)
    GstElement* pipeline = gst_parse_launch(
        "videotestsrc is-live=true ! fakesink", nullptr);
    ASSERT_NE(pipeline, nullptr);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    StreamModeController ctrl(pipeline);

    StreamMode cb_old = StreamMode::FULL;
    StreamMode cb_new = StreamMode::FULL;
    std::string cb_reason;
    bool cb_invoked = false;

    ctrl.set_mode_change_callback(
        [&](StreamMode old_m, StreamMode new_m, const std::string& reason) {
            cb_old = old_m;
            cb_new = new_m;
            cb_reason = reason;
            cb_invoked = true;
        });

    // Directly set confirmed statuses by reporting and waiting for debounce.
    // To avoid waiting 3s debounce, we manipulate via report + run main loop.
    // Report KVS UNHEALTHY to trigger mode change after debounce.
    ctrl.report_kvs_status(BranchStatus::UNHEALTHY);

    // Run GLib main loop iterations for debounce to fire (>3s)
    GMainContext* ctx = g_main_context_default();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(ctx, FALSE)) {}
        if (cb_invoked) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(cb_invoked) << "Mode change callback was not invoked within 5s";
    if (cb_invoked) {
        EXPECT_EQ(cb_old, StreamMode::FULL);
        EXPECT_EQ(cb_new, StreamMode::WEBRTC_ONLY);
        EXPECT_FALSE(cb_reason.empty());
    }

    ctrl.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 5. NullPipeline_NoQueueCrash
//    Validates: error handling (gst_bin_get_by_name returns nullptr)
TEST(StreamModeTest, NullPipeline_NoQueueCrash) {
    StreamModeController ctrl(nullptr);
    // Should not crash when pipeline is nullptr
    ctrl.report_kvs_status(BranchStatus::UNHEALTHY);
    ctrl.report_webrtc_status(BranchStatus::UNHEALTHY);

    // Run main loop briefly to let any timers fire
    GMainContext* ctx = g_main_context_default();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(ctx, FALSE)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // If we reach here without crash, the test passes
    ctrl.stop();
    SUCCEED();
}


// ---------------------------------------------------------------------------
// Property-Based Tests (RapidCheck)
// ---------------------------------------------------------------------------

// Feature: spec-15-adaptive-streaming, Property 1: Mode decision correctness
// **Validates: Requirements 1.3, 1.4, 1.5, 1.6**
RC_GTEST_PROP(StreamModePBT, ModeDecisionCorrectness, ()) {
    auto kvs = *rc::gen::element(BranchStatus::HEALTHY, BranchStatus::UNHEALTHY);
    auto web = *rc::gen::element(BranchStatus::HEALTHY, BranchStatus::UNHEALTHY);

    auto result = compute_target_mode(kvs, web);
    auto expected = expected_target_mode(kvs, web);

    RC_ASSERT(result == expected);
}

// Feature: spec-15-adaptive-streaming, Property 3: Queue params mapping correctness
// **Validates: Requirements 2.1, 2.2, 2.3, 2.4**
RC_GTEST_PROP(StreamModePBT, QueueParamsMappingCorrectness, ()) {
    auto mode = *rc::gen::element(
        StreamMode::FULL, StreamMode::KVS_ONLY,
        StreamMode::WEBRTC_ONLY, StreamMode::DEGRADED);

    auto result = compute_queue_params(mode);
    auto expected = expected_queue_params(mode);

    RC_ASSERT(result.kvs.max_size_buffers == expected.kvs.max_size_buffers);
    RC_ASSERT(result.kvs.leaky == expected.kvs.leaky);
    RC_ASSERT(result.web.max_size_buffers == expected.web.max_size_buffers);
    RC_ASSERT(result.web.leaky == expected.web.leaky);
}

// ---------------------------------------------------------------------------
// Custom main: gst_init required before any GStreamer API calls
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
