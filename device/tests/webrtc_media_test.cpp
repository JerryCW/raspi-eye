// webrtc_media_test.cpp
// WebRTC media manager tests: 6 example-based + 1 PBT property.
// Custom main() required for gst_init (pipeline tests need GStreamer).
#include "webrtc_media.h"
#include "webrtc_signaling.h"
#include "pipeline_builder.h"

#include <string>
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

// ============================================================
// Example-based tests
// ============================================================

// 1. StubCreateSuccess: create() returns non-null, peer_count() == 0
TEST(WebRtcMediaTest, StubCreateSuccess) {
    auto sig = create_stub_signaling();
    ASSERT_NE(sig, nullptr);
    std::string err;
    auto mgr = WebRtcMediaManager::create(*sig, &err);
    ASSERT_NE(mgr, nullptr) << "create() failed: " << err;
    EXPECT_EQ(mgr->peer_count(), 0u);
}

// 2. BroadcastFrameNoPeers: broadcast_frame() with no peers does not crash
TEST(WebRtcMediaTest, BroadcastFrameNoPeers) {
    auto sig = create_stub_signaling();
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);
    uint8_t data[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    mgr->broadcast_frame(data, sizeof(data), 1000, true);
    EXPECT_EQ(mgr->peer_count(), 0u);
}

// 3. AppsinkReplacesFakesink: webrtc-sink is appsink when WebRtcMediaManager provided
TEST(WebRtcMediaTest, AppsinkReplacesFakesink) {
    auto sig = create_stub_signaling();
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
// Note: new-sample callback consumes buffers, so try_pull_sample would block.
// Instead verify pipeline reaches a running state (PAUSED or PLAYING).
TEST(WebRtcMediaTest, PipelineSmokeWithAppsink) {
    auto sig = create_stub_signaling();
    auto mgr = WebRtcMediaManager::create(*sig);
    ASSERT_NE(mgr, nullptr);
    std::string err;
    GstElement* pipeline = PipelineBuilder::build_tee_pipeline(
        &err, {}, nullptr, nullptr, mgr.get());
    ASSERT_NE(pipeline, nullptr) << err;

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    ASSERT_NE(ret, GST_STATE_CHANGE_FAILURE);

    // Wait up to 3s for pipeline to reach at least PAUSED
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
// Custom main: gst_init required for pipeline tests
// ============================================================

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
