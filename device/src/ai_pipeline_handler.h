// ai_pipeline_handler.h
// AI inference pipeline handler: buffer probe + async inference + event management.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gst/gst.h>
#include "yolo_detector.h"

// AI pipeline configuration (POD)
struct AiConfig {
    std::string model_path;                      // ONNX model path
    int inference_fps = 2;                       // Frame sampling rate (frames per second)
    float confidence_threshold = 0.25f;          // Global confidence threshold
    std::string snapshot_dir = "device/events/"; // Event snapshot root directory
    int event_timeout_sec = 15;                  // Event timeout in seconds
    int max_cache_mb = 16;                       // JPEG memory cache limit (MB)
    std::string device_id;                       // Device identifier (from aws.thing_name)

    // Per-class confidence override
    struct TargetClass {
        std::string name;                        // COCO class name
        float confidence = -1.0f;                // -1 means use global threshold
    };
    std::vector<TargetClass> target_classes;
};

// Detection result callback type
using DetectionCallback = std::function<void(
    const std::vector<Detection>& detections,
    const InferenceStats& stats,
    const uint8_t* rgb_data,
    int frame_width,
    int frame_height)>;

// --- Standalone functions (testable independently) ---

// I420 to RGB conversion using integer fixed-point BT.601 coefficients.
// Output values clamped to [0, 255]. Output buffer must be width * height * 3 bytes.
void i420_to_rgb(const uint8_t* y_plane, const uint8_t* u_plane,
                 const uint8_t* v_plane,
                 int width, int height, int y_stride, int uv_stride,
                 uint8_t* rgb_out);

// Filter detections by target_classes and confidence thresholds.
// Pure function: returns subset of input detections matching criteria.
// If target_classes is empty, all detections with confidence >= global_threshold are kept.
std::vector<Detection> filter_detections(
    const std::vector<Detection>& detections,
    const std::vector<AiConfig::TargetClass>& target_classes,
    float global_threshold);

// Map COCO class_id (0-79) to class name. Returns "unknown" for out-of-range.
const char* coco_class_name(int class_id);

// Frame sampling throttle decision.
// Returns true if elapsed_ms >= 1000 / fps.
bool should_sample(int64_t elapsed_ms, int fps);

// --- AiPipelineHandler class ---

class AiPipelineHandler {
public:
    // Factory method
    static std::unique_ptr<AiPipelineHandler> create(
        std::unique_ptr<YoloDetector> detector,
        const AiConfig& config,
        std::string* error_msg = nullptr);

    ~AiPipelineHandler();

    // No copy
    AiPipelineHandler(const AiPipelineHandler&) = delete;
    AiPipelineHandler& operator=(const AiPipelineHandler&) = delete;

    // Install buffer probe on q-ai src pad.
    // Automatically removes old probe if one exists.
    bool install_probe(GstElement* pipeline, std::string* error_msg = nullptr);

    // Remove installed probe
    void remove_probe();

    // Start inference thread
    bool start(std::string* error_msg = nullptr);

    // Stop inference thread (join and wait)
    void stop();

    // Register detection callback.
    // Must be called before start(). Not thread-safe during inference.
    void set_detection_callback(DetectionCallback cb);

private:
    AiPipelineHandler(std::unique_ptr<YoloDetector> detector,
                      const AiConfig& config);

    // Probe callback (static, accesses this via user_data)
    static GstPadProbeReturn buffer_probe_cb(
        GstPad* pad, GstPadProbeInfo* info, gpointer user_data);

    // Inference thread main loop
    void inference_loop();

    // Event management
    void open_event(const std::vector<Detection>& detections);
    void close_event();
    void encode_snapshot(const uint8_t* rgb_data, int width, int height);
    void update_detections_summary(const std::vector<Detection>& detections);
    void check_event_timeout();
    void flush_cache();  // Intermediate flush when cache exceeds max_cache_mb

    // Members
    std::unique_ptr<YoloDetector> detector_;
    AiConfig config_;
    DetectionCallback detection_cb_;

    // Probe state
    GstPad* probe_pad_ = nullptr;
    gulong probe_id_ = 0;

    // Frame sampling throttle
    int64_t frame_interval_ms_ = 500;  // 1000 / inference_fps
    std::chrono::steady_clock::time_point last_sample_time_;

    // Inference thread synchronization
    std::thread inference_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_flag_{false};
    bool frame_ready_ = false;
    std::atomic<bool> busy_{false};

    // Frame buffer (pre-allocated)
    std::vector<uint8_t> rgb_buffer_;
    int frame_width_ = 0;
    int frame_height_ = 0;

    // Event state
    bool event_active_ = false;
    std::string event_id_;
    std::chrono::system_clock::time_point event_start_time_;
    std::chrono::steady_clock::time_point last_detection_time_;
    int frame_count_ = 0;

    // JPEG memory cache
    struct CachedFrame {
        std::string filename;
        std::vector<uint8_t> jpeg_data;
    };
    std::vector<CachedFrame> cached_frames_;
    size_t cached_bytes_ = 0;
    bool event_dir_created_ = false;  // Track if directory was created during intermediate flush

    // Detection summary (accumulated in memory)
    struct ClassSummary {
        int count = 0;
        float max_confidence = 0.0f;
    };
    std::unordered_map<std::string, ClassSummary> detections_summary_;
};
