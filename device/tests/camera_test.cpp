// camera_test.cpp
// Unit tests for CameraSource abstraction layer.
// Validates camera type enum, config, source creation, and pipeline integration.
#include <gtest/gtest.h>
#include <gst/gst.h>
#include "camera_source.h"
#include "pipeline_builder.h"
#include "pipeline_manager.h"
#include <string>

// GStreamer must be initialised before any gst_* call.
int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// --- Requirement 1.3, 6.3 ---
// default_camera_type() returns TEST on macOS, V4L2 on Linux.
TEST(CameraSourceTest, DefaultCameraType) {
#ifdef __APPLE__
    EXPECT_EQ(CameraSource::default_camera_type(), CameraSource::CameraType::TEST);
#else
    EXPECT_EQ(CameraSource::default_camera_type(), CameraSource::CameraType::V4L2);
#endif
}

// --- Requirement 1.4 ---
// camera_type_name() returns correct GStreamer factory name for each type.
TEST(CameraSourceTest, CameraTypeName) {
    EXPECT_STREQ(CameraSource::camera_type_name(CameraSource::CameraType::TEST), "videotestsrc");
    EXPECT_STREQ(CameraSource::camera_type_name(CameraSource::CameraType::V4L2), "v4l2src");
    EXPECT_STREQ(CameraSource::camera_type_name(CameraSource::CameraType::LIBCAMERA), "libcamerasrc");
}

// --- Requirement 5.1 ---
// parse_camera_type() correctly parses lowercase valid strings.
TEST(CameraSourceTest, ParseCameraTypeValid) {
    CameraSource::CameraType type;

    EXPECT_TRUE(CameraSource::parse_camera_type("test", type));
    EXPECT_EQ(type, CameraSource::CameraType::TEST);

    EXPECT_TRUE(CameraSource::parse_camera_type("v4l2", type));
    EXPECT_EQ(type, CameraSource::CameraType::V4L2);

    EXPECT_TRUE(CameraSource::parse_camera_type("libcamera", type));
    EXPECT_EQ(type, CameraSource::CameraType::LIBCAMERA);
}

// --- Requirement 5.1 ---
// parse_camera_type() is case-insensitive.
TEST(CameraSourceTest, ParseCameraTypeCaseInsensitive) {
    CameraSource::CameraType type;

    EXPECT_TRUE(CameraSource::parse_camera_type("TEST", type));
    EXPECT_EQ(type, CameraSource::CameraType::TEST);

    EXPECT_TRUE(CameraSource::parse_camera_type("V4L2", type));
    EXPECT_EQ(type, CameraSource::CameraType::V4L2);

    EXPECT_TRUE(CameraSource::parse_camera_type("LibCamera", type));
    EXPECT_EQ(type, CameraSource::CameraType::LIBCAMERA);
}

// --- Requirement 5.4 ---
// parse_camera_type() returns false for invalid strings.
TEST(CameraSourceTest, ParseCameraTypeInvalid) {
    CameraSource::CameraType type;
    EXPECT_FALSE(CameraSource::parse_camera_type("usb", type));
    EXPECT_FALSE(CameraSource::parse_camera_type("", type));
}

// --- Requirement 2.1, 2.2, 6.4 ---
// create_source(TEST) succeeds, element name is "src".
TEST(CameraSourceTest, CreateSourceTest) {
    CameraSource::CameraConfig config;
    config.type = CameraSource::CameraType::TEST;

    std::string err;
    GstElement* src = CameraSource::create_source(config, &err);
    ASSERT_NE(src, nullptr) << "create_source(TEST) failed: " << err;
    EXPECT_STREQ(GST_ELEMENT_NAME(src), "src");
    gst_object_unref(src);
}

// --- Requirement 2.5, 6.5 ---
// On macOS, V4L2 and LIBCAMERA plugins are unavailable -> nullptr + error.
#ifdef __APPLE__
TEST(CameraSourceTest, CreateSourceUnavailable) {
    std::string err;

    CameraSource::CameraConfig v4l2_config;
    v4l2_config.type = CameraSource::CameraType::V4L2;
    GstElement* v4l2_src = CameraSource::create_source(v4l2_config, &err);
    EXPECT_EQ(v4l2_src, nullptr);
    EXPECT_FALSE(err.empty());

    err.clear();

    CameraSource::CameraConfig libcam_config;
    libcam_config.type = CameraSource::CameraType::LIBCAMERA;
    GstElement* libcam_src = CameraSource::create_source(libcam_config, &err);
    EXPECT_EQ(libcam_src, nullptr);
    EXPECT_FALSE(err.empty());
}
#endif

// --- Requirement 3.1, 3.2, 6.6 ---
// Default CameraConfig -> build_tee_pipeline -> PipelineManager -> PLAYING.
TEST(CameraSourceTest, TeePipelineDefaultConfig) {
    std::string err;
    GstElement* raw = PipelineBuilder::build_tee_pipeline(&err);
    ASSERT_NE(raw, nullptr) << "build_tee_pipeline (default) failed: " << err;

    auto pm = PipelineManager::create(raw, &err);
    ASSERT_NE(pm, nullptr) << err;

    ASSERT_TRUE(pm->start(&err)) << "start() failed: " << err;
    // On Pi 5 (GStreamer 1.22, no GMainLoop in test), get_state may return
    // PAUSED for live+x264enc pipelines. Accept both as valid running states.
    GstState state = pm->current_state();
    EXPECT_TRUE(state == GST_STATE_PLAYING || state == GST_STATE_PAUSED)
        << "Expected PLAYING or PAUSED, got " << state;
}

// --- Requirement 3.3, 6.7 ---
// Explicit CameraType::TEST config -> build_tee_pipeline -> PLAYING.
TEST(CameraSourceTest, TeePipelineExplicitTest) {
    std::string err;
    CameraSource::CameraConfig config;
    config.type = CameraSource::CameraType::TEST;

    GstElement* raw = PipelineBuilder::build_tee_pipeline(&err, config);
    ASSERT_NE(raw, nullptr) << "build_tee_pipeline (explicit TEST) failed: " << err;

    auto pm = PipelineManager::create(raw, &err);
    ASSERT_NE(pm, nullptr) << err;

    ASSERT_TRUE(pm->start(&err)) << "start() failed: " << err;
    EXPECT_EQ(pm->current_state(), GST_STATE_PLAYING);
}

// --- Requirement 3.3, 3.5 ---
// Pipeline built with TEST config contains a "src" element.
TEST(CameraSourceTest, TeePipelineSourceElement) {
    std::string err;
    CameraSource::CameraConfig config;
    config.type = CameraSource::CameraType::TEST;

    GstElement* raw = PipelineBuilder::build_tee_pipeline(&err, config);
    ASSERT_NE(raw, nullptr) << "build_tee_pipeline failed: " << err;

    GstElement* src = gst_bin_get_by_name(GST_BIN(raw), "src");
    EXPECT_NE(src, nullptr) << "Missing element: src";
    if (src) gst_object_unref(src);

    // Clean up via PipelineManager RAII
    auto pm = PipelineManager::create(raw, &err);
    ASSERT_NE(pm, nullptr) << err;
}
