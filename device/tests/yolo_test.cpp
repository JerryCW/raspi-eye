// yolo_test.cpp
// Example-based tests + Property-based tests for YoloDetector module.
#include "yolo_detector.h"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

// Model paths passed via CMake compile definitions
#ifndef YOLO_MODEL_PATH_SMALL
#define YOLO_MODEL_PATH_SMALL ""
#endif
#ifndef YOLO_MODEL_PATH_NANO
#define YOLO_MODEL_PATH_NANO ""
#endif
#ifndef YOLO_MODEL_PATH_SMALL_INT8
#define YOLO_MODEL_PATH_SMALL_INT8 ""
#endif
#ifndef YOLO_MODEL_PATH_NANO_INT8
#define YOLO_MODEL_PATH_NANO_INT8 ""
#endif

static bool model_available() {
    return std::filesystem::exists(YOLO_MODEL_PATH_SMALL);
}

static bool model_nano_available() {
    return std::filesystem::exists(YOLO_MODEL_PATH_NANO);
}

static std::vector<uint8_t> random_rgb_image(int w, int h) {
    std::vector<uint8_t> data(w * h * 3);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& v : data) v = static_cast<uint8_t>(dist(rng));
    return data;
}

// ===== NMS Example-Based Tests =====

TEST(NmsTest, EmptyInput) {
    std::vector<Detection> empty;
    auto result = nms(empty, 0.45f);
    EXPECT_TRUE(result.empty());
}

TEST(NmsTest, SingleBox) {
    std::vector<Detection> dets = {{0.5f, 0.5f, 0.2f, 0.2f, 0, 0.9f}};
    auto result = nms(dets, 0.45f);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].class_id, 0);
    EXPECT_FLOAT_EQ(result[0].confidence, 0.9f);
}

TEST(NmsTest, SameClassOverlap) {
    // Two nearly identical boxes, same class — keep higher confidence
    std::vector<Detection> dets = {
        {0.5f, 0.5f, 0.3f, 0.3f, 1, 0.9f},
        {0.51f, 0.51f, 0.3f, 0.3f, 1, 0.7f},
    };
    auto result = nms(dets, 0.45f);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_FLOAT_EQ(result[0].confidence, 0.9f);
}

TEST(NmsTest, DifferentClassOverlap) {
    // Two overlapping boxes, different classes — both kept
    std::vector<Detection> dets = {
        {0.5f, 0.5f, 0.3f, 0.3f, 0, 0.9f},
        {0.51f, 0.51f, 0.3f, 0.3f, 1, 0.8f},
    };
    auto result = nms(dets, 0.45f);
    EXPECT_EQ(result.size(), 2u);
}

TEST(NmsTest, Deterministic) {
    std::vector<Detection> dets = {
        {0.3f, 0.3f, 0.2f, 0.2f, 0, 0.8f},
        {0.31f, 0.31f, 0.2f, 0.2f, 0, 0.6f},
        {0.7f, 0.7f, 0.1f, 0.1f, 1, 0.95f},
    };
    auto r1 = nms(dets, 0.45f);
    auto r2 = nms(dets, 0.45f);
    ASSERT_EQ(r1.size(), r2.size());
    for (size_t i = 0; i < r1.size(); ++i) {
        EXPECT_EQ(r1[i].class_id, r2[i].class_id);
        EXPECT_FLOAT_EQ(r1[i].confidence, r2[i].confidence);
    }
}

// ===== Letterbox Example-Based Tests =====

TEST(LetterboxTest, SquareInput) {
    auto img = random_rgb_image(640, 640);
    std::vector<float> buf;
    auto info = letterbox_resize(img.data(), 640, 640, buf);
    EXPECT_EQ(buf.size(), 3u * 640 * 640);
    EXPECT_FLOAT_EQ(info.scale, 1.0f);
    EXPECT_EQ(info.pad_x, 0);
    EXPECT_EQ(info.pad_y, 0);
}

TEST(LetterboxTest, WideInput) {
    auto img = random_rgb_image(1280, 640);
    std::vector<float> buf;
    auto info = letterbox_resize(img.data(), 1280, 640, buf);
    EXPECT_EQ(buf.size(), 3u * 640 * 640);
    EXPECT_FLOAT_EQ(info.scale, 0.5f);  // min(640/1280, 640/640) = 0.5
    EXPECT_EQ(info.pad_x, 0);
    EXPECT_GT(info.pad_y, 0);  // top/bottom padding
}

TEST(LetterboxTest, TallInput) {
    auto img = random_rgb_image(640, 1280);
    std::vector<float> buf;
    auto info = letterbox_resize(img.data(), 640, 1280, buf);
    EXPECT_EQ(buf.size(), 3u * 640 * 640);
    EXPECT_FLOAT_EQ(info.scale, 0.5f);  // min(640/640, 640/1280) = 0.5
    EXPECT_GT(info.pad_x, 0);  // left/right padding
    EXPECT_EQ(info.pad_y, 0);
}

// ===== Model Loading Tests =====

TEST(YoloDetectorTest, CreateWithInvalidPath) {
    std::string err;
    auto det = YoloDetector::create("/nonexistent/model.onnx", {}, &err);
    EXPECT_EQ(det, nullptr);
    EXPECT_FALSE(err.empty());
}

// Shared fixture: load model once for all tests that need it
class YoloModelFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        if (!model_available()) return;
        std::string err;
        shared_detector_ = YoloDetector::create(YOLO_MODEL_PATH_SMALL, {}, &err);
    }
    static std::unique_ptr<YoloDetector> shared_detector_;
};
std::unique_ptr<YoloDetector> YoloModelFixture::shared_detector_;

TEST_F(YoloModelFixture, CreateWithValidModel) {
    if (!model_available()) GTEST_SKIP() << "Model not available";
    EXPECT_NE(shared_detector_, nullptr);
}

TEST_F(YoloModelFixture, DetectEndToEnd) {
    if (!model_available() || !shared_detector_) GTEST_SKIP() << "Model not available";
    auto img = random_rgb_image(640, 480);
    auto results = shared_detector_->detect(img.data(), 640, 480);
    for (const auto& d : results) {
        EXPECT_GE(d.x, 0.0f);
        EXPECT_LE(d.x, 1.0f);
        EXPECT_GE(d.y, 0.0f);
        EXPECT_LE(d.y, 1.0f);
        EXPECT_GE(d.w, 0.0f);
        EXPECT_LE(d.w, 1.0f);
        EXPECT_GE(d.h, 0.0f);
        EXPECT_LE(d.h, 1.0f);
        EXPECT_GE(d.class_id, 0);
        EXPECT_LT(d.class_id, 80);
        EXPECT_GT(d.confidence, 0.0f);
        EXPECT_LE(d.confidence, 1.0f);
    }
}

TEST_F(YoloModelFixture, DetectWithStatsTimings) {
    if (!model_available() || !shared_detector_) GTEST_SKIP() << "Model not available";
    auto img = random_rgb_image(640, 480);
    auto [results, stats] = shared_detector_->detect_with_stats(img.data(), 640, 480);
    EXPECT_GE(stats.preprocess_ms, 0.0);
    EXPECT_GE(stats.inference_ms, 0.0);
    EXPECT_GE(stats.postprocess_ms, 0.0);
    EXPECT_GE(stats.total_ms, 0.0);
}


// ===== PBT — Property 1: NMS Monotonic Decrease =====

RC_GTEST_PROP(NmsPBT, MonotonicDecrease, ()) {
    // Feature: yolo-detector, Property 1: NMS output count <= input count
    auto count = *rc::gen::inRange(0, 30);
    std::vector<Detection> dets;
    for (int i = 0; i < count; ++i) {
        Detection d;
        d.x = *rc::gen::map(rc::gen::inRange(1, 1000), [](int v) { return v / 1000.0f; });
        d.y = *rc::gen::map(rc::gen::inRange(1, 1000), [](int v) { return v / 1000.0f; });
        d.w = *rc::gen::map(rc::gen::inRange(1, 500), [](int v) { return v / 1000.0f; });
        d.h = *rc::gen::map(rc::gen::inRange(1, 500), [](int v) { return v / 1000.0f; });
        d.class_id = *rc::gen::inRange(0, 80);
        d.confidence = *rc::gen::map(rc::gen::inRange(1, 1000), [](int v) { return v / 1000.0f; });
        dets.push_back(d);
    }
    auto iou_thresh = *rc::gen::map(rc::gen::inRange(1, 1000), [](int v) { return v / 1000.0f; });

    auto result = nms(dets, iou_thresh);
    RC_ASSERT(result.size() <= dets.size());
}

// ===== PBT — Property 2: NMS IoU Invariant =====

RC_GTEST_PROP(NmsPBT, IouInvariant, ()) {
    // Feature: yolo-detector, Property 2: same-class output boxes IoU <= threshold
    auto count = *rc::gen::inRange(0, 30);
    std::vector<Detection> dets;
    for (int i = 0; i < count; ++i) {
        Detection d;
        d.x = *rc::gen::map(rc::gen::inRange(1, 1000), [](int v) { return v / 1000.0f; });
        d.y = *rc::gen::map(rc::gen::inRange(1, 1000), [](int v) { return v / 1000.0f; });
        d.w = *rc::gen::map(rc::gen::inRange(1, 500), [](int v) { return v / 1000.0f; });
        d.h = *rc::gen::map(rc::gen::inRange(1, 500), [](int v) { return v / 1000.0f; });
        d.class_id = *rc::gen::inRange(0, 80);
        d.confidence = *rc::gen::map(rc::gen::inRange(1, 1000), [](int v) { return v / 1000.0f; });
        dets.push_back(d);
    }
    auto iou_thresh = *rc::gen::map(rc::gen::inRange(1, 1000), [](int v) { return v / 1000.0f; });

    auto result = nms(dets, iou_thresh);

    // Group by class_id, check pairwise IoU
    std::unordered_map<int, std::vector<const Detection*>> groups;
    for (const auto& d : result) groups[d.class_id].push_back(&d);

    for (const auto& [cls, boxes] : groups) {
        for (size_t i = 0; i < boxes.size(); ++i) {
            for (size_t j = i + 1; j < boxes.size(); ++j) {
                float iou = compute_iou(*boxes[i], *boxes[j]);
                RC_ASSERT(iou <= iou_thresh + 1e-5f);
            }
        }
    }
}

// ===== PBT — Property 3: Letterbox Output Size Constant =====

RC_GTEST_PROP(LetterboxPBT, OutputSizeConstant, ()) {
    // Feature: yolo-detector, Property 3: output always 3*640*640, values in [0,1]
    auto w = *rc::gen::inRange(1, 64);
    auto h = *rc::gen::inRange(1, 64);

    // Generate random image using mt19937 (much faster than per-pixel rc::gen)
    std::vector<uint8_t> img(w * h * 3);
    std::mt19937 rng(static_cast<uint32_t>(w * 1000 + h));
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& v : img) v = static_cast<uint8_t>(dist(rng));

    std::vector<float> buf;
    auto info = letterbox_resize(img.data(), w, h, buf);

    // Buffer size must always be 3*640*640
    RC_ASSERT(buf.size() == 3u * 640 * 640);

    // Sample check: verify min/max of entire buffer in [0.0, 1.0]
    auto [min_it, max_it] = std::minmax_element(buf.begin(), buf.end());
    RC_ASSERT(*min_it >= 0.0f);
    RC_ASSERT(*max_it <= 1.0f);

    // Check scale
    float expected_scale = std::min(640.0f / w, 640.0f / h);
    RC_ASSERT(std::abs(info.scale - expected_scale) < 1e-5f);
}

// ===== PBT — Property 4: Coordinate Restore Clamp [0, 1] =====

RC_GTEST_PROP(CoordRestorePBT, ClampZeroOne, ()) {
    // Feature: yolo-detector, Property 4: restored coords clamped to [0,1]
    auto orig_w = *rc::gen::inRange(1, 4096);
    auto orig_h = *rc::gen::inRange(1, 4096);

    float scale = std::min(640.0f / orig_w, 640.0f / orig_h);
    int new_w = static_cast<int>(orig_w * scale);
    int new_h = static_cast<int>(orig_h * scale);
    LetterboxInfo info{scale, (640 - new_w) / 2, (640 - new_h) / 2, new_w, new_h};

    auto count = *rc::gen::inRange(1, 20);
    std::vector<Detection> dets;
    for (int i = 0; i < count; ++i) {
        Detection d;
        d.x = *rc::gen::map(rc::gen::inRange(0, 1001), [](int v) { return v / 1000.0f; });
        d.y = *rc::gen::map(rc::gen::inRange(0, 1001), [](int v) { return v / 1000.0f; });
        d.w = *rc::gen::map(rc::gen::inRange(0, 1001), [](int v) { return v / 1000.0f; });
        d.h = *rc::gen::map(rc::gen::inRange(0, 1001), [](int v) { return v / 1000.0f; });
        d.class_id = 0;
        d.confidence = 0.9f;
        dets.push_back(d);
    }

    restore_coordinates(dets, info, orig_w, orig_h);

    for (const auto& d : dets) {
        RC_ASSERT(d.x >= 0.0f && d.x <= 1.0f);
        RC_ASSERT(d.y >= 0.0f && d.y <= 1.0f);
        RC_ASSERT(d.w >= 0.0f && d.w <= 1.0f);
        RC_ASSERT(d.h >= 0.0f && d.h <= 1.0f);
    }
}

// ===== Performance Baseline Tests =====

#ifdef __linux__
#include <fstream>
static long get_rss_kb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::stol(line.substr(6));
        }
    }
    return -1;
}
#elif defined(__APPLE__)
#include <mach/mach.h>
static long get_rss_kb() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<long>(info.resident_size / 1024);
    }
    return -1;
}
#else
static long get_rss_kb() { return -1; }
#endif

#include <spdlog/spdlog.h>

static void run_perf_baseline(const std::string& model_path, const std::string& label) {
    constexpr int RUNS = 3;
    long rss_before = get_rss_kb();
    auto det = YoloDetector::create(model_path);
    ASSERT_NE(det, nullptr);
    long rss_after_load = get_rss_kb();

    auto img = random_rgb_image(640, 480);

    std::vector<double> pre_ms, inf_ms, post_ms, tot_ms;
    for (int i = 0; i < RUNS; ++i) {
        auto [results, stats] = det->detect_with_stats(img.data(), 640, 480);
        pre_ms.push_back(stats.preprocess_ms);
        inf_ms.push_back(stats.inference_ms);
        post_ms.push_back(stats.postprocess_ms);
        tot_ms.push_back(stats.total_ms);
    }

    auto avg = [](const std::vector<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    };
    auto mn = [](const std::vector<double>& v) { return *std::min_element(v.begin(), v.end()); };
    auto mx = [](const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); };

    long rss_delta = (rss_before > 0 && rss_after_load > 0)
                         ? (rss_after_load - rss_before) : -1;

    spdlog::info("[{}] {} runs | pre: avg={:.1f} min={:.1f} max={:.1f}ms | "
                 "infer: avg={:.1f} min={:.1f} max={:.1f}ms | "
                 "post: avg={:.1f} min={:.1f} max={:.1f}ms | "
                 "total: avg={:.1f} min={:.1f} max={:.1f}ms | "
                 "RSS delta: {}kB",
                 label, RUNS,
                 avg(pre_ms), mn(pre_ms), mx(pre_ms),
                 avg(inf_ms), mn(inf_ms), mx(inf_ms),
                 avg(post_ms), mn(post_ms), mx(post_ms),
                 avg(tot_ms), mn(tot_ms), mx(tot_ms),
                 rss_delta);
}

TEST(YoloPerformance, PerformanceBaseline) {
    if (!model_available()) GTEST_SKIP() << "Model (yolov11s) not available";
    run_perf_baseline(YOLO_MODEL_PATH_SMALL, "yolov11s");
}

TEST(YoloPerformance, PerformanceBaselineNano) {
    if (!model_nano_available()) GTEST_SKIP() << "Model (yolov11n) not available";
    run_perf_baseline(YOLO_MODEL_PATH_NANO, "yolov11n");
}

// ===== Config Extension Tests (Spec 9.5, Task 3.1) =====

TEST(YoloConfigTest, ConfigDefaultValues) {
    DetectorConfig cfg;
    EXPECT_EQ(cfg.use_xnnpack, false);
    EXPECT_EQ(cfg.graph_optimization_level, 99);
    EXPECT_EQ(cfg.inter_op_num_threads, 1);
    // Verify original fields retain defaults
    EXPECT_FLOAT_EQ(cfg.confidence_threshold, 0.25f);
    EXPECT_FLOAT_EQ(cfg.iou_threshold, 0.45f);
    EXPECT_EQ(cfg.num_threads, 2);
}

TEST(YoloConfigTest, ConfigBackwardCompatible) {
    DetectorConfig cfg{0.25f, 0.45f, 2};
    EXPECT_FLOAT_EQ(cfg.confidence_threshold, 0.25f);
    EXPECT_FLOAT_EQ(cfg.iou_threshold, 0.45f);
    EXPECT_EQ(cfg.num_threads, 2);
    // New fields use defaults
    EXPECT_EQ(cfg.use_xnnpack, false);
    EXPECT_EQ(cfg.graph_optimization_level, 99);
}

// ===== Graph Optimization Level Tests (Spec 9.5, Task 3.2) =====

TEST(YoloGraphOptTest, GraphOptLevelAll) {
    if (!model_available()) GTEST_SKIP() << "Model not available";
    DetectorConfig cfg;
    cfg.graph_optimization_level = 99;
    auto det = YoloDetector::create(YOLO_MODEL_PATH_SMALL, cfg);
    EXPECT_NE(det, nullptr);
}

TEST(YoloGraphOptTest, GraphOptLevelDisable) {
    if (!model_available()) GTEST_SKIP() << "Model not available";
    DetectorConfig cfg;
    cfg.graph_optimization_level = 0;
    auto det = YoloDetector::create(YOLO_MODEL_PATH_SMALL, cfg);
    EXPECT_NE(det, nullptr);
}

TEST(YoloGraphOptTest, GraphOptLevelBasic) {
    if (!model_available()) GTEST_SKIP() << "Model not available";
    DetectorConfig cfg;
    cfg.graph_optimization_level = 1;
    auto det = YoloDetector::create(YOLO_MODEL_PATH_SMALL, cfg);
    EXPECT_NE(det, nullptr);
}

TEST(YoloGraphOptTest, GraphOptLevelExtended) {
    if (!model_available()) GTEST_SKIP() << "Model not available";
    DetectorConfig cfg;
    cfg.graph_optimization_level = 2;
    auto det = YoloDetector::create(YOLO_MODEL_PATH_SMALL, cfg);
    EXPECT_NE(det, nullptr);
}

// ===== XNNPACK Fallback Test (Spec 9.5, Task 3.3) =====

TEST(YoloXnnpackTest, XnnpackFallbackOnUnsupported) {
    if (!model_available()) GTEST_SKIP() << "Model not available";
    DetectorConfig cfg;
    cfg.use_xnnpack = true;
    // When XNNPACK EP is not available (e.g. macOS prebuilt without XNNPACK),
    // create() should fall back to CPU EP and return a valid detector
    auto det = YoloDetector::create(YOLO_MODEL_PATH_SMALL, cfg);
    EXPECT_NE(det, nullptr);
}

// ===== A/B Comparison Benchmark Framework (Spec 9.5, Task 6) =====

struct BenchConfig {
    std::string label;
    DetectorConfig detector_config;
    std::string model_path;
};

static int get_cpu_temp_celsius() {
#ifdef __linux__
    std::ifstream temp_file("/sys/class/thermal/thermal_zone0/temp");
    if (temp_file.is_open()) {
        int millideg = 0;
        temp_file >> millideg;
        return millideg / 1000;
    }
#endif
    return -1;
}

static void wait_for_cool_cpu() {
#ifdef __linux__
    for (int i = 0; i < 60; ++i) {
        int temp = get_cpu_temp_celsius();
        if (temp < 0 || temp < 70) return;
        if (temp >= 80) {
            spdlog::info("CPU temp {}C >= 80C, waiting 5s to cool down...", temp);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        } else {
            return;
        }
    }
#endif
}

static void run_benchmark(const BenchConfig& bench, int runs = 10) {
    if (!std::filesystem::exists(bench.model_path)) {
        spdlog::info("[{}] SKIPPED: model not available", bench.label);
        return;
    }

    wait_for_cool_cpu();
    int cpu_temp_before = get_cpu_temp_celsius();

    long rss_before = get_rss_kb();
    auto det = YoloDetector::create(bench.model_path, bench.detector_config);
    if (!det) {
        spdlog::info("[{}] SKIPPED: detector creation failed", bench.label);
        return;
    }
    long rss_after_load = get_rss_kb();

    auto img = random_rgb_image(640, 480);
    std::vector<double> pre_ms, inf_ms, post_ms, tot_ms;

    for (int i = 0; i < runs; ++i) {
        auto [results, stats] = det->detect_with_stats(img.data(), 640, 480);
        pre_ms.push_back(stats.preprocess_ms);
        inf_ms.push_back(stats.inference_ms);
        post_ms.push_back(stats.postprocess_ms);
        tot_ms.push_back(stats.total_ms);
    }

    auto avg = [](const std::vector<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    };
    auto mn = [](const std::vector<double>& v) { return *std::min_element(v.begin(), v.end()); };
    auto mx = [](const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); };

    long rss_delta = (rss_before > 0 && rss_after_load > 0)
                         ? (rss_after_load - rss_before) : -1;
    int cpu_temp_after = get_cpu_temp_celsius();

    spdlog::info("[{}] {} runs | infer: avg={:.1f} min={:.1f} max={:.1f}ms | "
                 "total: avg={:.1f} min={:.1f} max={:.1f}ms | "
                 "RSS delta: {}kB | CPU temp: {}->{}C",
                 bench.label, runs,
                 avg(inf_ms), mn(inf_ms), mx(inf_ms),
                 avg(tot_ms), mn(tot_ms), mx(tot_ms),
                 rss_delta, cpu_temp_before, cpu_temp_after);
}

TEST(YoloBenchmark, OptimizationComparison) {
#ifdef __APPLE__
    GTEST_SKIP() << "ARM optimization benchmark skipped on macOS";
#endif
    if (!model_available()) GTEST_SKIP() << "Model not available";

    std::vector<BenchConfig> configs = {
        {"baseline-cpu-2t-all",  {0.25f, 0.45f, 2, 1, false, 99}, YOLO_MODEL_PATH_SMALL},
        {"cpu-1t-all",           {0.25f, 0.45f, 1, 1, false, 99}, YOLO_MODEL_PATH_SMALL},
        {"cpu-3t-all",           {0.25f, 0.45f, 3, 1, false, 99}, YOLO_MODEL_PATH_SMALL},
        {"cpu-4t-all",           {0.25f, 0.45f, 4, 1, false, 99}, YOLO_MODEL_PATH_SMALL},
        {"cpu-4t-inter2-all",    {0.25f, 0.45f, 4, 2, false, 99}, YOLO_MODEL_PATH_SMALL},
        {"xnnpack-2t-all",       {0.25f, 0.45f, 2, 1, true,  99}, YOLO_MODEL_PATH_SMALL},
        {"cpu-2t-disable",       {0.25f, 0.45f, 2, 1, false,  0}, YOLO_MODEL_PATH_SMALL},
        {"cpu-2t-basic",         {0.25f, 0.45f, 2, 1, false,  1}, YOLO_MODEL_PATH_SMALL},
        {"cpu-2t-extended",      {0.25f, 0.45f, 2, 1, false,  2}, YOLO_MODEL_PATH_SMALL},
    };

    for (const auto& cfg : configs) {
        run_benchmark(cfg);
    }
}

TEST(YoloBenchmark, Int8ModelBenchmark) {
#ifdef __APPLE__
    GTEST_SKIP() << "ARM optimization benchmark skipped on macOS";
#endif
    bool fp32_available = std::filesystem::exists(YOLO_MODEL_PATH_SMALL);
    bool int8_available = std::filesystem::exists(YOLO_MODEL_PATH_SMALL_INT8);
    if (!fp32_available && !int8_available) {
        GTEST_SKIP() << "Neither FP32 nor INT8 model available";
    }

    if (fp32_available) {
        run_benchmark({"fp32-cpu-2t-all", {0.25f, 0.45f, 2, 1, false, 99}, YOLO_MODEL_PATH_SMALL});
    }
    if (int8_available) {
        run_benchmark({"int8-cpu-2t-all", {0.25f, 0.45f, 2, 1, false, 99}, YOLO_MODEL_PATH_SMALL_INT8});
    }
}
