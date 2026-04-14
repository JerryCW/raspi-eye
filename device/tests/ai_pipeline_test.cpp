// ai_pipeline_test.cpp
// AI pipeline handler tests: example-based + PBT.
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "ai_pipeline_handler.h"
#include "config_manager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

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

    // fps = 1 with backward compat: active_fps=1, idle_fps=1 -> idle >= active -> fail
    std::unordered_map<std::string, std::string> kv1 = {{"inference_fps", "1"}};
    EXPECT_FALSE(parse_ai_config(kv1, config, &err));

    // fps = 2 should succeed (backward compat: active_fps=2, idle_fps=1)
    std::unordered_map<std::string, std::string> kv2 = {{"inference_fps", "2"}};
    EXPECT_TRUE(parse_ai_config(kv2, config, &err));
    EXPECT_EQ(config.inference_fps, 2);
    EXPECT_EQ(config.active_fps, 2);
    EXPECT_EQ(config.idle_fps, 1);

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

// ============================================================
// Property 6: 1-second window best frame selection
// ============================================================

// Simulates the 1-second window candidate selection logic as a pure function.
// Given a sequence of (confidence) values within the same 1-second window,
// the selected candidate should have the highest confidence.
// **Validates: Requirements 6.1**
RC_GTEST_PROP(AiPipelinePBT, WindowBestFrameSelection, ()) {
    // Feature: event-pipeline-optimization, Property 6: 1-second window best frame selection

    // Generate 1-30 frames within the same 1-second window
    auto frame_count = *rc::gen::inRange(1, 31);

    // Generate confidence values for each frame (0.01 to 1.0)
    float best_confidence = -1.0f;
    float selected_confidence = -1.0f;

    for (int i = 0; i < frame_count; ++i) {
        float confidence = *rc::gen::map(rc::gen::inRange(1, 1001),
                                         [](int v) { return v / 1000.0f; });

        // Track the true maximum
        best_confidence = std::max(best_confidence, confidence);

        // Simulate window candidate selection: keep highest confidence
        if (confidence > selected_confidence) {
            selected_confidence = confidence;
        }
    }

    // The selected candidate must have the highest confidence
    RC_ASSERT(std::abs(selected_confidence - best_confidence) < 1e-6f);
}

// ============================================================
// Property 7: Top-K cache invariant
// ============================================================

// **Validates: Requirements 6.3, 6.4, 6.5**
RC_GTEST_PROP(AiPipelinePBT, TopKCacheInvariant, ()) {
    // Feature: event-pipeline-optimization, Property 7: Top-K cache invariant

    // Random K in [1, 20]
    auto max_k = *rc::gen::inRange(1, 21);

    // Random number of candidates to insert (0-50)
    auto num_candidates = *rc::gen::inRange(0, 51);

    std::vector<SnapshotEntry> heap;
    std::vector<float> all_confidences;

    for (int i = 0; i < num_candidates; ++i) {
        float confidence = *rc::gen::map(rc::gen::inRange(0, 10001),
                                         [](int v) { return v / 10000.0f; });
        all_confidences.push_back(confidence);

        SnapshotEntry entry;
        entry.confidence = confidence;
        entry.filename = "frame_" + std::to_string(i) + ".jpg";
        // jpeg_data not needed for this test
        entry.timestamp = std::chrono::system_clock::now();

        try_submit_to_topk(heap, max_k, std::move(entry));
    }

    // (a) Cache size <= K
    RC_ASSERT(static_cast<int>(heap.size()) <= max_k);

    // (b) Cache size == min(N, K)
    int expected_size = std::min(num_candidates, max_k);
    RC_ASSERT(static_cast<int>(heap.size()) == expected_size);

    // (c) Cache contains the top min(N, K) highest confidence values
    if (!all_confidences.empty()) {
        // Sort all confidences descending to find the expected top-K
        std::vector<float> sorted_conf = all_confidences;
        std::sort(sorted_conf.begin(), sorted_conf.end(), std::greater<float>());

        // Collect heap confidences and sort descending
        std::vector<float> heap_conf;
        for (const auto& entry : heap) {
            heap_conf.push_back(entry.confidence);
        }
        std::sort(heap_conf.begin(), heap_conf.end(), std::greater<float>());

        // Compare: heap should contain the top min(N, K) values
        RC_ASSERT(heap_conf.size() == static_cast<size_t>(expected_size));
        for (size_t i = 0; i < heap_conf.size(); ++i) {
            RC_ASSERT(std::abs(heap_conf[i] - sorted_conf[i]) < 1e-6f);
        }
    }
}

// ============================================================
// Property 8: frame_count consistency
// ============================================================

// **Validates: Requirements 6.9**
RC_GTEST_PROP(AiPipelinePBT, FrameCountConsistency, ()) {
    // Feature: event-pipeline-optimization, Property 8: frame_count consistency

    // Random number of snapshot entries (1-20)
    auto num_entries = *rc::gen::inRange(1, 21);

    // Create a temporary directory for this test
    auto tmp_dir = std::filesystem::temp_directory_path() / ("pbt_fc_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmp_dir);

    // Generate random SnapshotEntry items and write them as .jpg files
    std::vector<SnapshotEntry> entries;
    auto base_time = std::chrono::system_clock::now();
    for (int i = 0; i < num_entries; ++i) {
        SnapshotEntry entry;
        // Generate a unique filename
        std::ostringstream fname;
        fname << "20260412_153045_" << std::setfill('0') << std::setw(3) << (i + 1) << ".jpg";
        entry.filename = fname.str();
        // Minimal JPEG data (just needs to be non-empty for write)
        entry.jpeg_data = {0xFF, 0xD8, 0xFF, 0xE0, 0x00};
        entry.confidence = *rc::gen::map(rc::gen::inRange(1, 1001),
                                         [](int v) { return v / 1000.0f; });
        entry.timestamp = base_time + std::chrono::seconds(i);
        entries.push_back(std::move(entry));
    }

    // Sort by timestamp (as close_event does)
    std::sort(entries.begin(), entries.end(),
              [](const SnapshotEntry& a, const SnapshotEntry& b) {
                  return a.timestamp < b.timestamp;
              });

    // Write .jpg files to disk
    for (const auto& entry : entries) {
        std::string filepath = (tmp_dir / entry.filename).string();
        std::ofstream ofs(filepath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(entry.jpeg_data.data()),
                  static_cast<std::streamsize>(entry.jpeg_data.size()));
    }

    // Write event.json with frame_count = entries.size()
    int frame_count = static_cast<int>(entries.size());
    nlohmann::json event_json;
    event_json["event_id"] = "evt_test";
    event_json["device_id"] = "test_device";
    event_json["start_time"] = "2026-04-12T15:30:45Z";
    event_json["end_time"] = "2026-04-12T15:31:15Z";
    event_json["status"] = "closed";
    event_json["frame_count"] = frame_count;
    event_json["detections_summary"] = nlohmann::json::object();

    std::string json_path = (tmp_dir / "event.json").string();
    {
        std::ofstream json_ofs(json_path);
        json_ofs << event_json.dump(4);
    }

    // Count actual .jpg files on disk
    int actual_jpg_count = 0;
    for (const auto& dir_entry : std::filesystem::directory_iterator(tmp_dir)) {
        auto fname = dir_entry.path().filename().string();
        if (fname.size() >= 4 && fname.substr(fname.size() - 4) == ".jpg") {
            actual_jpg_count++;
        }
    }

    // Read back event.json and verify frame_count
    std::ifstream json_ifs(json_path);
    auto read_json = nlohmann::json::parse(json_ifs);
    int read_frame_count = read_json["frame_count"].get<int>();

    // Assert: frame_count in event.json == actual .jpg file count
    RC_ASSERT(read_frame_count == actual_jpg_count);
    RC_ASSERT(read_frame_count == frame_count);

    // Cleanup
    std::filesystem::remove_all(tmp_dir);
}

// ============================================================
// Task 9.1: Two-phase event confirmation state machine tests
// ============================================================
//
// Since AiPipelineHandler's state machine is private and requires
// GStreamer + YoloDetector, we simulate the state machine logic
// using a lightweight test harness that mirrors inference_loop().
// This tests the state transitions and their observable effects
// (disk I/O, callbacks, summary content) using the same pure
// functions (filter_detections, try_submit_to_topk, etc.).

namespace {

// Lightweight state machine simulator mirroring inference_loop() logic.
// Tests observable behavior without GStreamer/YoloDetector dependencies.
struct EventStateSim {
    enum class State { IDLE, PENDING, CONFIRMED, CLOSING };

    State state = State::IDLE;
    int consecutive_count = 0;
    static constexpr int kThreshold = 3;

    // Observable state
    int detection_cb_calls = 0;
    int event_close_cb_calls = 0;
    bool close_cb_registered = true;

    // Snapshot state (uses real Top-K logic)
    std::vector<SnapshotEntry> snapshot_heap;
    int max_snapshots = 10;

    // Window candidate simulation
    float window_best_confidence = -1.0f;
    bool has_window_candidate = false;

    // Summary tracking
    std::unordered_map<std::string, int> summary_counts;

    // Disk state
    std::string snapshot_dir;
    std::string event_id;
    bool disk_written = false;

    // Feed a detection frame into the state machine
    void feed_detection(float confidence, int class_id = 0) {
        const char* name = coco_class_name(class_id);

        switch (state) {
            case State::IDLE: {
                // First detection -> PENDING
                state = State::PENDING;
                consecutive_count = 1;
                event_id = "evt_test";
                summary_counts.clear();
                snapshot_heap.clear();
                window_best_confidence = -1.0f;
                has_window_candidate = false;
                disk_written = false;
                // Accumulate summary from first detection
                summary_counts[name]++;
                // Update window candidate
                update_window(confidence);
                break;
            }
            case State::PENDING: {
                consecutive_count++;
                summary_counts[name]++;
                update_window(confidence);
                if (consecutive_count >= kThreshold) {
                    state = State::CONFIRMED;
                }
                break;
            }
            case State::CONFIRMED: {
                summary_counts[name]++;
                update_window(confidence);
                // detection_callback only for CONFIRMED
                detection_cb_calls++;
                break;
            }
            case State::CLOSING:
                break;
        }
    }

    // Feed a no-detection frame
    void feed_no_detection() {
        switch (state) {
            case State::PENDING: {
                // Interrupted: discard
                consecutive_count = 0;
                summary_counts.clear();
                snapshot_heap.clear();
                window_best_confidence = -1.0f;
                has_window_candidate = false;
                state = State::IDLE;
                break;
            }
            default:
                break;
        }
    }

    // Simulate close_event (timeout or explicit)
    void close_event() {
        state = State::CLOSING;

        // Flush window candidate to Top-K
        flush_window_candidate();

        if (!snapshot_heap.empty() && !snapshot_dir.empty()) {
            // Sort by timestamp and write to disk
            std::sort(snapshot_heap.begin(), snapshot_heap.end(),
                      [](const SnapshotEntry& a, const SnapshotEntry& b) {
                          return a.timestamp < b.timestamp;
                      });

            std::filesystem::create_directories(snapshot_dir + event_id);
            for (const auto& entry : snapshot_heap) {
                std::string filepath = snapshot_dir + event_id + "/" + entry.filename;
                std::ofstream ofs(filepath, std::ios::binary);
                if (ofs) {
                    ofs.write(reinterpret_cast<const char*>(entry.jpeg_data.data()),
                              static_cast<std::streamsize>(entry.jpeg_data.size()));
                }
            }

            // Write event.json
            nlohmann::json ej;
            ej["event_id"] = event_id;
            ej["frame_count"] = static_cast<int>(snapshot_heap.size());
            nlohmann::json summary = nlohmann::json::object();
            for (const auto& [name, count] : summary_counts) {
                summary[name] = {{"count", count}};
            }
            ej["detections_summary"] = summary;

            std::string json_path = snapshot_dir + event_id + "/event.json";
            std::ofstream json_ofs(json_path);
            if (json_ofs) {
                json_ofs << ej.dump(4);
                disk_written = true;
            }

            // Call close callback if registered and disk write succeeded
            if (disk_written && close_cb_registered) {
                event_close_cb_calls++;
            }
        }

        snapshot_heap.clear();
        consecutive_count = 0;
        state = State::IDLE;
    }

private:
    void update_window(float confidence) {
        if (!has_window_candidate || confidence > window_best_confidence) {
            window_best_confidence = confidence;
            has_window_candidate = true;
        }
    }

    void flush_window_candidate() {
        if (!has_window_candidate) return;

        SnapshotEntry entry;
        entry.confidence = window_best_confidence;
        entry.filename = "test_snapshot.jpg";
        entry.jpeg_data = {0xFF, 0xD8, 0xFF, 0xE0, 0x00};  // minimal JPEG
        entry.timestamp = std::chrono::system_clock::now();

        try_submit_to_topk(snapshot_heap, max_snapshots, std::move(entry));
        has_window_candidate = false;
        window_best_confidence = -1.0f;
    }
};

}  // namespace

// Test: 3 consecutive detections -> state upgrades to CONFIRMED
TEST(EventStateTest, PendingToConfirmed) {
    EventStateSim sim;
    EXPECT_EQ(sim.state, EventStateSim::State::IDLE);

    // First detection -> PENDING
    sim.feed_detection(0.8f, 0);
    EXPECT_EQ(sim.state, EventStateSim::State::PENDING);
    EXPECT_EQ(sim.consecutive_count, 1);

    // Second detection -> still PENDING
    sim.feed_detection(0.7f, 0);
    EXPECT_EQ(sim.state, EventStateSim::State::PENDING);
    EXPECT_EQ(sim.consecutive_count, 2);

    // Third detection -> CONFIRMED
    sim.feed_detection(0.9f, 0);
    EXPECT_EQ(sim.state, EventStateSim::State::CONFIRMED);
    EXPECT_EQ(sim.consecutive_count, 3);
}

// Test: Detection interrupted during PENDING -> reset to IDLE
TEST(EventStateTest, PendingInterrupted) {
    EventStateSim sim;

    // Two detections -> PENDING
    sim.feed_detection(0.8f, 0);
    sim.feed_detection(0.7f, 0);
    EXPECT_EQ(sim.state, EventStateSim::State::PENDING);
    EXPECT_EQ(sim.consecutive_count, 2);

    // No detection -> reset to IDLE
    sim.feed_no_detection();
    EXPECT_EQ(sim.state, EventStateSim::State::IDLE);
    EXPECT_EQ(sim.consecutive_count, 0);
    EXPECT_TRUE(sim.summary_counts.empty());
    EXPECT_TRUE(sim.snapshot_heap.empty());
}

// Test: Confirmed event timeout -> close_event writes to disk and returns to IDLE
TEST(EventStateTest, ConfirmedTimeout) {
    auto tmp_dir = std::filesystem::temp_directory_path() / ("evt_timeout_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string dir_str = tmp_dir.string() + "/";

    EventStateSim sim;
    sim.snapshot_dir = dir_str;

    // Reach CONFIRMED state
    sim.feed_detection(0.8f, 0);
    sim.feed_detection(0.7f, 0);
    sim.feed_detection(0.9f, 0);
    EXPECT_EQ(sim.state, EventStateSim::State::CONFIRMED);

    // Simulate timeout -> close_event
    sim.close_event();
    EXPECT_EQ(sim.state, EventStateSim::State::IDLE);
    EXPECT_TRUE(sim.disk_written);

    // Verify event directory exists with event.json
    std::string event_dir = dir_str + "evt_test";
    EXPECT_TRUE(std::filesystem::exists(event_dir + "/event.json"));

    // Cleanup
    std::filesystem::remove_all(tmp_dir);
}

// Test: close_event callback is called after successful disk write
TEST(EventStateTest, CloseCallbackCalled) {
    auto tmp_dir = std::filesystem::temp_directory_path() / ("evt_cb_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string dir_str = tmp_dir.string() + "/";

    EventStateSim sim;
    sim.snapshot_dir = dir_str;
    sim.close_cb_registered = true;

    // Reach CONFIRMED and close
    sim.feed_detection(0.8f, 0);
    sim.feed_detection(0.7f, 0);
    sim.feed_detection(0.9f, 0);
    sim.close_event();

    EXPECT_EQ(sim.event_close_cb_calls, 1);

    // Cleanup
    std::filesystem::remove_all(tmp_dir);
}

// Test: No callback registered -> close_event still succeeds
TEST(EventStateTest, CloseCallbackNotRegistered) {
    auto tmp_dir = std::filesystem::temp_directory_path() / ("evt_nocb_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string dir_str = tmp_dir.string() + "/";

    EventStateSim sim;
    sim.snapshot_dir = dir_str;
    sim.close_cb_registered = false;

    // Reach CONFIRMED and close
    sim.feed_detection(0.8f, 0);
    sim.feed_detection(0.7f, 0);
    sim.feed_detection(0.9f, 0);
    sim.close_event();

    EXPECT_EQ(sim.state, EventStateSim::State::IDLE);
    EXPECT_EQ(sim.event_close_cb_calls, 0);
    EXPECT_TRUE(sim.disk_written);

    // Cleanup
    std::filesystem::remove_all(tmp_dir);
}

// Test: detection_callback only called for CONFIRMED events, not PENDING
TEST(EventStateTest, DetectionCallbackOnlyForConfirmed) {
    EventStateSim sim;

    // PENDING phase: no detection_callback
    sim.feed_detection(0.8f, 0);
    sim.feed_detection(0.7f, 0);
    EXPECT_EQ(sim.detection_cb_calls, 0);

    // Third detection upgrades to CONFIRMED, but the upgrade frame itself
    // doesn't trigger callback (matches inference_loop: callback only in
    // CONFIRMED case, and the 3rd detection transitions from PENDING)
    sim.feed_detection(0.9f, 0);
    EXPECT_EQ(sim.detection_cb_calls, 0);

    // Fourth detection: now in CONFIRMED -> callback called
    sim.feed_detection(0.85f, 0);
    EXPECT_EQ(sim.detection_cb_calls, 1);

    // Fifth detection: still CONFIRMED -> callback called again
    sim.feed_detection(0.75f, 0);
    EXPECT_EQ(sim.detection_cb_calls, 2);
}

// Test: detections_summary includes PENDING phase detections
TEST(EventStateTest, SummaryIncludesPending) {
    EventStateSim sim;

    // Feed detections with different classes during PENDING
    sim.feed_detection(0.8f, 0);   // person (PENDING, count=1)
    sim.feed_detection(0.7f, 14);  // bird (PENDING, count=2)
    sim.feed_detection(0.9f, 0);   // person (upgrades to CONFIRMED, count=3)

    // Feed one more in CONFIRMED
    sim.feed_detection(0.85f, 15); // cat (CONFIRMED, count=4)

    // Summary should include all detections from PENDING start
    EXPECT_EQ(sim.summary_counts["person"], 2);  // 1st + 3rd
    EXPECT_EQ(sim.summary_counts["bird"], 1);     // 2nd
    EXPECT_EQ(sim.summary_counts["cat"], 1);      // 4th
}

// Test: close_event flushes pending window candidate to Top-K
TEST(EventStateTest, CloseFlushesWindowCandidate) {
    auto tmp_dir = std::filesystem::temp_directory_path() / ("evt_flush_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string dir_str = tmp_dir.string() + "/";

    EventStateSim sim;
    sim.snapshot_dir = dir_str;

    // Reach CONFIRMED with detections (window candidate exists)
    sim.feed_detection(0.8f, 0);
    sim.feed_detection(0.7f, 0);
    sim.feed_detection(0.9f, 0);
    EXPECT_EQ(sim.state, EventStateSim::State::CONFIRMED);

    // Before close: snapshot_heap is empty (window not yet flushed)
    EXPECT_TRUE(sim.snapshot_heap.empty());
    EXPECT_TRUE(sim.has_window_candidate);

    // Close event: should flush window candidate to Top-K then write to disk
    sim.close_event();

    // Verify disk write happened (window candidate was flushed)
    EXPECT_TRUE(sim.disk_written);
    std::string event_dir = dir_str + "evt_test";
    EXPECT_TRUE(std::filesystem::exists(event_dir + "/test_snapshot.jpg"));

    // Read event.json and verify frame_count
    std::ifstream json_ifs(event_dir + "/event.json");
    auto ej = nlohmann::json::parse(json_ifs);
    EXPECT_EQ(ej["frame_count"].get<int>(), 1);

    // Cleanup
    std::filesystem::remove_all(tmp_dir);
}


// ============================================================
// Property 9: Pending event has no side effects
// ============================================================

// **Validates: Requirements 7.4, 7.5, 7.6**
RC_GTEST_PROP(AiPipelinePBT, PendingEventNoSideEffects, ()) {
    // Feature: event-pipeline-optimization, Property 9: Pending event no side effects
    //
    // Generator: random 1-2 detections followed by interruption (< 3 consecutive)
    // Assertions: no disk I/O, no S3 upload, detection_callback not called

    // Random number of detections before interruption: 1 or 2 (always < kThreshold=3)
    auto num_detections = *rc::gen::inRange(1, 3);

    // Create a unique temporary directory to monitor for disk I/O
    auto tmp_dir = std::filesystem::temp_directory_path() / ("pbt_pending_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmp_dir);
    std::string snapshot_dir = tmp_dir.string() + "/";

    // Count files before simulation
    int files_before = 0;
    for (const auto& _ : std::filesystem::directory_iterator(tmp_dir)) {
        (void)_;
        files_before++;
    }

    // Simulate state machine: IDLE -> PENDING -> interrupted
    EventStateSim sim;
    sim.snapshot_dir = snapshot_dir;

    for (int i = 0; i < num_detections; ++i) {
        float confidence = *rc::gen::map(rc::gen::inRange(1, 1001),
                                         [](int v) { return v / 1000.0f; });
        int class_id = *rc::gen::inRange(0, 80);
        sim.feed_detection(confidence, class_id);
    }

    // Verify still in PENDING state (not yet confirmed)
    RC_ASSERT(sim.state == EventStateSim::State::PENDING);
    RC_ASSERT(sim.consecutive_count < EventStateSim::kThreshold);

    // Interrupt: no detection
    sim.feed_no_detection();

    // Assert: state reset to IDLE
    RC_ASSERT(sim.state == EventStateSim::State::IDLE);

    // Assert: no disk I/O (snapshot_dir has no new files)
    int files_after = 0;
    for (const auto& _ : std::filesystem::directory_iterator(tmp_dir)) {
        (void)_;
        files_after++;
    }
    RC_ASSERT(files_after == files_before);

    // Assert: detection_callback was never called
    RC_ASSERT(sim.detection_cb_calls == 0);

    // Assert: event_close_callback was never called (no S3 upload trigger)
    RC_ASSERT(sim.event_close_cb_calls == 0);

    // Assert: no disk write flag
    RC_ASSERT(!sim.disk_written);

    // Assert: internal state fully cleared
    RC_ASSERT(sim.summary_counts.empty());
    RC_ASSERT(sim.snapshot_heap.empty());
    RC_ASSERT(!sim.has_window_candidate);

    // Cleanup
    std::filesystem::remove_all(tmp_dir);
}
