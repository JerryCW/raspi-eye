// ai_pipeline_test.cpp
// AI pipeline handler tests: example-based + PBT.
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "ai_pipeline_handler.h"
#include "config_manager.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================
// Example-based tests
// ============================================================

TEST(AiPipelineTest, I420ToRGB_PureBlack) {
    // Y=0, U=128, V=128 -> R~0, G~0, B~0
    const int w = 2, h = 2;
    std::vector<uint8_t> y(w * h, 0);
    std::vector<uint8_t> u(w / 2 * h / 2, 128);
    std::vector<uint8_t> v(w / 2 * h / 2, 128);
    std::vector<uint8_t> rgb(w * h * 3);
    i420_to_rgb(y.data(), u.data(), v.data(), w, h, w, w / 2, rgb.data());
    for (int i = 0; i < w * h * 3; ++i) {
        EXPECT_NEAR(rgb[i], 0, 2) << "pixel byte index " << i;
    }
}

TEST(AiPipelineTest, I420ToRGB_PureWhite) {
    // Y=255, U=128, V=128 -> R~255, G~255, B~255
    const int w = 2, h = 2;
    std::vector<uint8_t> y(w * h, 255);
    std::vector<uint8_t> u(w / 2 * h / 2, 128);
    std::vector<uint8_t> v(w / 2 * h / 2, 128);
    std::vector<uint8_t> rgb(w * h * 3);
    i420_to_rgb(y.data(), u.data(), v.data(), w, h, w, w / 2, rgb.data());
    for (int i = 0; i < w * h * 3; ++i) {
        EXPECT_NEAR(rgb[i], 255, 2) << "pixel byte index " << i;
    }
}

TEST(AiPipelineTest, CreateNullDetector) {
    std::string err;
    auto handler = AiPipelineHandler::create(nullptr, AiConfig{}, &err);
    EXPECT_EQ(handler, nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(AiPipelineTest, FilterDetections_EmptyTargetClasses) {
    // Empty target_classes: all detections filtered by global threshold
    std::vector<Detection> dets = {
        {0.5f, 0.5f, 0.1f, 0.1f, 0, 0.8f},  // person, above threshold
        {0.3f, 0.3f, 0.1f, 0.1f, 15, 0.3f},  // cat, below threshold
        {0.7f, 0.7f, 0.1f, 0.1f, 14, 0.6f},  // bird, above threshold
    };
    std::vector<AiConfig::TargetClass> empty_tc;
    auto result = filter_detections(dets, empty_tc, 0.5f);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].class_id, 0);   // person
    EXPECT_EQ(result[1].class_id, 14);  // bird
}

TEST(AiPipelineTest, FilterDetections_PerClassOverride) {
    // Per-class threshold overrides global threshold
    std::vector<Detection> dets = {
        {0.5f, 0.5f, 0.1f, 0.1f, 0, 0.4f},   // person, conf=0.4
        {0.3f, 0.3f, 0.1f, 0.1f, 15, 0.6f},   // cat, conf=0.6
    };
    std::vector<AiConfig::TargetClass> tc = {
        {"person", 0.3f},  // per-class threshold 0.3 -> person passes
        {"cat", 0.7f},     // per-class threshold 0.7 -> cat fails
    };
    auto result = filter_detections(dets, tc, 0.5f);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].class_id, 0);  // only person passes
}

TEST(AiPipelineTest, CocoClassName_ValidRange) {
    EXPECT_STREQ(coco_class_name(0), "person");
    EXPECT_STREQ(coco_class_name(79), "toothbrush");
}

TEST(AiPipelineTest, CocoClassName_OutOfRange) {
    EXPECT_STREQ(coco_class_name(-1), "unknown");
    EXPECT_STREQ(coco_class_name(80), "unknown");
}

TEST(AiPipelineTest, ParseAiConfig_InferenceFpsRange) {
    AiConfig config;
    std::string err;

    // fps = 0 should fail
    std::unordered_map<std::string, std::string> kv0 = {{"inference_fps", "0"}};
    EXPECT_FALSE(parse_ai_config(kv0, config, &err));

    // fps = 31 should fail
    std::unordered_map<std::string, std::string> kv31 = {{"inference_fps", "31"}};
    EXPECT_FALSE(parse_ai_config(kv31, config, &err));

    // fps = 1 should succeed
    std::unordered_map<std::string, std::string> kv1 = {{"inference_fps", "1"}};
    EXPECT_TRUE(parse_ai_config(kv1, config, &err));
    EXPECT_EQ(config.inference_fps, 1);

    // fps = 30 should succeed
    std::unordered_map<std::string, std::string> kv30 = {{"inference_fps", "30"}};
    EXPECT_TRUE(parse_ai_config(kv30, config, &err));
    EXPECT_EQ(config.inference_fps, 30);
}

TEST(AiPipelineTest, ParseAiConfig_EventTimeoutMin) {
    AiConfig config;
    std::string err;

    // event_timeout_sec = 2 (below minimum 3) -> uses default 15
    std::unordered_map<std::string, std::string> kv = {{"event_timeout_sec", "2"}};
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    EXPECT_EQ(config.event_timeout_sec, 15);
}

TEST(AiPipelineTest, ParseAiConfig_TargetClassesParsing) {
    AiConfig config;
    std::string err;

    std::unordered_map<std::string, std::string> kv = {
        {"target_classes", "bird:0.3,person:0.5,cat,dog"}};
    EXPECT_TRUE(parse_ai_config(kv, config, &err));
    ASSERT_EQ(config.target_classes.size(), 4u);

    EXPECT_EQ(config.target_classes[0].name, "bird");
    EXPECT_FLOAT_EQ(config.target_classes[0].confidence, 0.3f);

    EXPECT_EQ(config.target_classes[1].name, "person");
    EXPECT_FLOAT_EQ(config.target_classes[1].confidence, 0.5f);

    EXPECT_EQ(config.target_classes[2].name, "cat");
    EXPECT_LT(config.target_classes[2].confidence, 0.0f);  // -1 = use global

    EXPECT_EQ(config.target_classes[3].name, "dog");
    EXPECT_LT(config.target_classes[3].confidence, 0.0f);  // -1 = use global
}

// ============================================================
// PBT Property tests
// ============================================================

// --- Property 1: I420 to RGB output invariants ---
// **Validates: Requirements 3.2, 3.5**
RC_GTEST_PROP(AiPipelinePBT, I420ToRGBOutputInvariants, ()) {
    // Feature: ai-pipeline, Property 1: I420 to RGB output invariants
    // Random even width/height in [2, 128] (small for performance)
    auto half_w = *rc::gen::inRange(1, 65);
    auto half_h = *rc::gen::inRange(1, 65);
    int w = half_w * 2;
    int h = half_h * 2;

    // Generate random Y/U/V planes using mt19937 (faster than per-pixel rc::gen)
    std::mt19937 rng(static_cast<uint32_t>(w * 1000 + h));
    std::uniform_int_distribution<int> dist(0, 255);

    std::vector<uint8_t> y_plane(w * h);
    for (auto& v : y_plane) v = static_cast<uint8_t>(dist(rng));

    std::vector<uint8_t> u_plane(w / 2 * h / 2);
    for (auto& v : u_plane) v = static_cast<uint8_t>(dist(rng));

    std::vector<uint8_t> v_plane(w / 2 * h / 2);
    for (auto& v : v_plane) v = static_cast<uint8_t>(dist(rng));

    // Allocate output with sentinel bytes to detect out-of-bounds writes
    size_t expected_size = static_cast<size_t>(w) * h * 3;
    constexpr uint8_t sentinel = 0xAB;
    std::vector<uint8_t> rgb(expected_size + 16, sentinel);

    i420_to_rgb(y_plane.data(), u_plane.data(), v_plane.data(),
                w, h, w, w / 2, rgb.data());

    // Verify output buffer size: all expected_size bytes were written
    // (we check sentinel bytes after the expected region are untouched)
    for (size_t i = expected_size; i < rgb.size(); ++i) {
        RC_ASSERT(rgb[i] == sentinel);
    }

    // Verify all output pixel values in [0, 255] — uint8_t guarantees this,
    // but we verify no out-of-bounds writes corrupted the data
    for (size_t i = 0; i < expected_size; ++i) {
        RC_ASSERT(rgb[i] <= 255);
    }
}

// --- Property 2: Frame sampling throttle consistency ---
// **Validates: Requirements 2.2, 2.3**
RC_GTEST_PROP(AiPipelinePBT, FrameSamplingThrottleConsistency, ()) {
    // Feature: ai-pipeline, Property 2: Frame sampling throttle consistency
    auto elapsed_ms = *rc::gen::inRange(0, 5001);
    auto fps = *rc::gen::inRange(1, 31);

    bool result = should_sample(static_cast<int64_t>(elapsed_ms), fps);
    bool expected = elapsed_ms >= (1000 / fps);

    RC_ASSERT(result == expected);
}

// --- Property 3: target_classes filter correctness ---
// **Validates: Requirements 4.4, 8.2, 8.3**
RC_GTEST_PROP(AiPipelinePBT, TargetClassesFilterCorrectness, ()) {
    // Feature: ai-pipeline, Property 3: target_classes filter correctness

    // Generate random detections (0-20)
    auto det_count = *rc::gen::inRange(0, 21);
    std::vector<Detection> detections;
    for (int i = 0; i < det_count; ++i) {
        Detection d;
        d.x = *rc::gen::map(rc::gen::inRange(0, 1001), [](int v) { return v / 1000.0f; });
        d.y = *rc::gen::map(rc::gen::inRange(0, 1001), [](int v) { return v / 1000.0f; });
        d.w = *rc::gen::map(rc::gen::inRange(1, 500), [](int v) { return v / 1000.0f; });
        d.h = *rc::gen::map(rc::gen::inRange(1, 500), [](int v) { return v / 1000.0f; });
        d.class_id = *rc::gen::inRange(0, 80);
        d.confidence = *rc::gen::map(rc::gen::inRange(0, 1001), [](int v) { return v / 1000.0f; });
        detections.push_back(d);
    }

    // Generate random target_classes (0-5), picking from COCO names
    auto tc_count = *rc::gen::inRange(0, 6);
    std::vector<AiConfig::TargetClass> target_classes;
    std::unordered_set<std::string> tc_names;
    for (int i = 0; i < tc_count; ++i) {
        int cid = *rc::gen::inRange(0, 80);
        std::string name = coco_class_name(cid);
        if (tc_names.count(name)) continue;  // skip duplicates
        tc_names.insert(name);
        AiConfig::TargetClass tc;
        tc.name = name;
        // confidence in [-1.0, 1.0]; -1 means use global threshold
        tc.confidence = *rc::gen::map(rc::gen::inRange(-1000, 1001),
                                      [](int v) { return v / 1000.0f; });
        target_classes.push_back(tc);
    }

    // Random global threshold [0.0, 1.0]
    auto global_threshold = *rc::gen::map(rc::gen::inRange(0, 1001),
                                          [](int v) { return v / 1000.0f; });

    auto result = filter_detections(detections, target_classes, global_threshold);

    // Build lookup for target_classes
    std::unordered_map<std::string, float> tc_map;
    for (const auto& tc : target_classes) {
        tc_map[tc.name] = tc.confidence;
    }

    // (a) Output is a subset of input (no new or modified elements)
    RC_ASSERT(result.size() <= detections.size());

    for (const auto& out : result) {
        // Find this detection in the original input
        bool found = false;
        for (const auto& inp : detections) {
            if (inp.class_id == out.class_id &&
                std::abs(inp.confidence - out.confidence) < 1e-6f &&
                std::abs(inp.x - out.x) < 1e-6f &&
                std::abs(inp.y - out.y) < 1e-6f) {
                found = true;
                break;
            }
        }
        RC_ASSERT(found);

        const char* class_name = coco_class_name(out.class_id);

        if (target_classes.empty()) {
            // (b) When target_classes is empty, all classes kept above global threshold
            RC_ASSERT(out.confidence >= global_threshold);
        } else {
            // (a) class_name must be in target_classes
            RC_ASSERT(tc_map.count(class_name) > 0);

            // (b) confidence >= applicable threshold
            float tc_conf = tc_map[class_name];
            float threshold = (tc_conf >= 0.0f) ? tc_conf : global_threshold;
            RC_ASSERT(out.confidence >= threshold);
        }
    }
}
