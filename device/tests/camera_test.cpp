// camera_test.cpp
// Unit tests for CameraSource abstraction layer.
// Validates camera type enum, config, source creation, and pipeline integration.
#include <gtest/gtest.h>
#include <gst/gst.h>
#include "camera_source.h"
#include "pipeline_builder.h"
#include "pipeline_manager.h"
#include <string>

// RapidCheck headers — camera_test 链接 rapidcheck 后生效（任务 8）
#if __has_include(<rapidcheck.h>)
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#define HAS_RAPIDCHECK 1
#else
#define HAS_RAPIDCHECK 0
#endif

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

// ============================================================
// Task 2.3: select_best_format 单元测试
// Requirements: 1.1, 1.2, 1.3, 1.4, 1.5
// ============================================================

TEST(SelectBestFormatTest, MjpgPlusYuyvReturnsMjpg) {
    std::vector<CameraSource::V4L2Format> fmts = {
        CameraSource::V4L2Format::MJPG,
        CameraSource::V4L2Format::YUYV
    };
    EXPECT_EQ(CameraSource::select_best_format(fmts), CameraSource::V4L2Format::MJPG);
}

TEST(SelectBestFormatTest, MjpgPlusI420ReturnsMjpg) {
    std::vector<CameraSource::V4L2Format> fmts = {
        CameraSource::V4L2Format::MJPG,
        CameraSource::V4L2Format::I420
    };
    EXPECT_EQ(CameraSource::select_best_format(fmts), CameraSource::V4L2Format::MJPG);
}

TEST(SelectBestFormatTest, OnlyYuyvReturnsYuyv) {
    std::vector<CameraSource::V4L2Format> fmts = {
        CameraSource::V4L2Format::YUYV
    };
    EXPECT_EQ(CameraSource::select_best_format(fmts), CameraSource::V4L2Format::YUYV);
}

TEST(SelectBestFormatTest, OnlyI420ReturnsI420) {
    std::vector<CameraSource::V4L2Format> fmts = {
        CameraSource::V4L2Format::I420
    };
    EXPECT_EQ(CameraSource::select_best_format(fmts), CameraSource::V4L2Format::I420);
}

TEST(SelectBestFormatTest, EmptyReturnsUnknown) {
    std::vector<CameraSource::V4L2Format> fmts;
    EXPECT_EQ(CameraSource::select_best_format(fmts), CameraSource::V4L2Format::UNKNOWN);
}

// ============================================================
// Task 2.2: Property 1 — 格式选择优先级不变式（PBT）
// Feature: pipeline-cpu-optimization, Property 1: 格式选择优先级不变式
// **Validates: Requirements 1.1, 1.2, 1.3, 1.4, 1.5**
// ============================================================

#if HAS_RAPIDCHECK

RC_GTEST_PROP(FormatPriority, InvariantMjpgOverI420OverYuyv, ()) {
    // 生成器：随机生成 1-10 个 V4L2Format（从 MJPG, I420, YUYV 中选取，允许重复）
    const auto count = *rc::gen::inRange(1, 11);
    std::vector<CameraSource::V4L2Format> formats;
    formats.reserve(count);
    for (int i = 0; i < count; ++i) {
        formats.push_back(*rc::gen::element(
            CameraSource::V4L2Format::MJPG,
            CameraSource::V4L2Format::I420,
            CameraSource::V4L2Format::YUYV
        ));
    }

    auto result = CameraSource::select_best_format(formats);

    // 检查是否包含各格式
    bool has_mjpg = false, has_i420 = false, has_yuyv = false;
    for (auto fmt : formats) {
        if (fmt == CameraSource::V4L2Format::MJPG) has_mjpg = true;
        if (fmt == CameraSource::V4L2Format::I420) has_i420 = true;
        if (fmt == CameraSource::V4L2Format::YUYV) has_yuyv = true;
    }

    // 断言：MJPG > I420 > YUYV 优先级
    if (has_mjpg) {
        RC_ASSERT(result == CameraSource::V4L2Format::MJPG);
    } else if (has_i420) {
        RC_ASSERT(result == CameraSource::V4L2Format::I420);
    } else if (has_yuyv) {
        RC_ASSERT(result == CameraSource::V4L2Format::YUYV);
    }
}

#endif // HAS_RAPIDCHECK
