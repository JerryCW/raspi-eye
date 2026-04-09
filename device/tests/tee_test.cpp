// tee_test.cpp
// Smoke tests for dual-tee pipeline — validates PipelineBuilder + adopt factory.
#include "pipeline_manager.h"
#include "pipeline_builder.h"
#include <gst/gst.h>
#include <gtest/gtest.h>
#include <string>

// GStreamer must be initialised before any gst_* call.
// PipelineBuilder::build_tee_pipeline and direct gst_pipeline_new calls
// require GStreamer to be ready, so we init once in main().
int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// --- Requirement 1.1, 1.2 ---
// Adopt a valid GstPipeline via create(GstElement*), returns non-null.
TEST(TeeTest, AdoptValidPipeline) {
    std::string err;
    GstElement* pipe = gst_pipeline_new("test-pipe");
    ASSERT_NE(pipe, nullptr);

    auto pm = PipelineManager::create(pipe, &err);
    ASSERT_NE(pm, nullptr) << "create(GstElement*) failed: " << err;
}

// --- Requirement 1.3 ---
// Adopt nullptr returns nullptr with non-empty error message.
TEST(TeeTest, AdoptNullPipeline) {
    std::string err;
    auto pm = PipelineManager::create(static_cast<GstElement*>(nullptr), &err);
    EXPECT_EQ(pm, nullptr);
    EXPECT_FALSE(err.empty());
}

// --- Requirement 1.5 ---
// Adopted simple pipeline can start and reach PLAYING.
TEST(TeeTest, AdoptedPipelineStart) {
    std::string err;
    GstElement* pipe = gst_pipeline_new("simple-pipe");
    ASSERT_NE(pipe, nullptr);

    GstElement* src  = gst_element_factory_make("videotestsrc", "src");
    GstElement* sink = gst_element_factory_make("fakesink", "sink");
    ASSERT_NE(src, nullptr);
    ASSERT_NE(sink, nullptr);
    gst_bin_add_many(GST_BIN(pipe), src, sink, nullptr);
    ASSERT_TRUE(gst_element_link(src, sink));

    auto pm = PipelineManager::create(pipe, &err);
    ASSERT_NE(pm, nullptr) << err;

    ASSERT_TRUE(pm->start(&err)) << "start() failed: " << err;
    EXPECT_EQ(pm->current_state(), GST_STATE_PLAYING);
}

// --- Requirement 3.1, 3.8, 4.4, 7.2 ---
// Full tee pipeline reaches PLAYING.
TEST(TeeTest, TeePipelinePlaying) {
    std::string err;
    GstElement* raw = PipelineBuilder::build_tee_pipeline(&err);
    ASSERT_NE(raw, nullptr) << "build_tee_pipeline failed: " << err;

    auto pm = PipelineManager::create(raw, &err);
    ASSERT_NE(pm, nullptr) << err;

    ASSERT_TRUE(pm->start(&err)) << "start() failed: " << err;
    EXPECT_EQ(pm->current_state(), GST_STATE_PLAYING);
}

// --- Requirement 3.4, 7.3 ---
// Named sinks exist in the running tee pipeline.
TEST(TeeTest, TeePipelineNamedSinks) {
    std::string err;
    GstElement* raw = PipelineBuilder::build_tee_pipeline(&err);
    ASSERT_NE(raw, nullptr) << err;

    auto pm = PipelineManager::create(raw, &err);
    ASSERT_NE(pm, nullptr) << err;
    ASSERT_TRUE(pm->start(&err)) << err;

    const char* names[] = {"kvs-sink", "webrtc-sink", "ai-sink"};
    for (const char* name : names) {
        GstElement* elem = gst_bin_get_by_name(GST_BIN(pm->pipeline()), name);
        EXPECT_NE(elem, nullptr) << "Missing element: " << name;
        if (elem) gst_object_unref(elem);
    }
}

// --- Requirement 1.6, 7.4 ---
// After stop(), pipeline() returns nullptr. ASan validates no leaks.
TEST(TeeTest, TeePipelineStop) {
    std::string err;
    GstElement* raw = PipelineBuilder::build_tee_pipeline(&err);
    ASSERT_NE(raw, nullptr) << err;

    auto pm = PipelineManager::create(raw, &err);
    ASSERT_NE(pm, nullptr) << err;
    ASSERT_TRUE(pm->start(&err)) << err;

    pm->stop();
    EXPECT_EQ(pm->pipeline(), nullptr);
}

// --- Requirement 7.5 ---
// RAII: tee pipeline created and started in a scope; leaving scope triggers
// automatic cleanup. ASan will flag any leak or use-after-free.
TEST(TeeTest, TeePipelineRAII) {
    {
        std::string err;
        GstElement* raw = PipelineBuilder::build_tee_pipeline(&err);
        ASSERT_NE(raw, nullptr) << err;

        auto pm = PipelineManager::create(raw, &err);
        ASSERT_NE(pm, nullptr) << err;
        ASSERT_TRUE(pm->start(&err)) << err;
        // pm goes out of scope — destructor calls stop()
    }
    SUCCEED();
}

// --- Requirement 2.1, 2.2, 7.8 ---
// Encoder detection: build_tee_pipeline succeeds (encoder available),
// query encoder properties to verify tune/speed-preset/threads.
TEST(TeeTest, EncoderDetection) {
    std::string err;
    GstElement* raw = PipelineBuilder::build_tee_pipeline(&err);
    ASSERT_NE(raw, nullptr) << "build_tee_pipeline failed: " << err;

    // Retrieve the encoder element by name
    GstElement* encoder = gst_bin_get_by_name(GST_BIN(raw), "encoder");
    ASSERT_NE(encoder, nullptr) << "encoder element not found in pipeline";

    // Query tune (GstX264EncTune flags) — expect 0x00000004 (zerolatency)
    guint tune_val = 0;
    g_object_get(G_OBJECT(encoder), "tune", &tune_val, nullptr);
    EXPECT_EQ(tune_val, 0x00000004u) << "tune should be zerolatency (0x4)";

    // Query speed-preset (GstX264EncPreset enum) — expect 1 (ultrafast)
    guint preset_val = 0;
    g_object_get(G_OBJECT(encoder), "speed-preset", &preset_val, nullptr);
    EXPECT_EQ(preset_val, 1u) << "speed-preset should be ultrafast (1)";

    // Query threads (guint) — expect 2
    guint threads_val = 0;
    g_object_get(G_OBJECT(encoder), "threads", &threads_val, nullptr);
    EXPECT_EQ(threads_val, 2u) << "threads should be 2";

    gst_object_unref(encoder);

    // Clean up: adopt into PipelineManager for proper RAII teardown
    auto pm = PipelineManager::create(raw, &err);
    ASSERT_NE(pm, nullptr) << err;
}
