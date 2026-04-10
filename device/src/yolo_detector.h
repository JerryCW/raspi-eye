// yolo_detector.h
// YOLO object detector using ONNX Runtime C API with RAII resource management.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Forward declare ONNX Runtime C types to avoid exposing onnxruntime_c_api.h in header
struct OrtEnv;
struct OrtSession;
struct OrtMemoryInfo;

// Single detection result (POD)
struct Detection {
    float x;          // Center x, normalized to original image [0.0, 1.0]
    float y;          // Center y, normalized to original image [0.0, 1.0]
    float w;          // Width, normalized to original image [0.0, 1.0]
    float h;          // Height, normalized to original image [0.0, 1.0]
    int class_id;     // COCO 80-class ID
    float confidence; // Detection confidence [0.0, 1.0]
};

// Detector configuration (POD)
struct DetectorConfig {
    float confidence_threshold = 0.25f; // Min confidence to keep
    float iou_threshold = 0.45f;        // NMS IoU threshold
    int num_threads = 2;                // ONNX Runtime intra-op threads
    int inter_op_num_threads = 1;       // ONNX Runtime inter-op threads (YOLO is sequential)
    bool use_xnnpack = false;           // Enable XNNPACK EP (ARM NEON)
    int graph_optimization_level = 99;  // 0=DISABLE_ALL, 1=BASIC, 2=EXTENDED, 99=ALL
};

// Inference timing statistics (POD)
struct InferenceStats {
    double preprocess_ms = 0.0;  // Letterbox + normalize + NCHW
    double inference_ms = 0.0;   // OrtSession::Run
    double postprocess_ms = 0.0; // Confidence filter + NMS + coord restore
    double total_ms = 0.0;       // End-to-end
};

// Letterbox transform info (POD, used internally and for coordinate restoration)
struct LetterboxInfo {
    float scale;   // min(640.0/w, 640.0/h)
    int pad_x;     // Horizontal padding offset (pixels)
    int pad_y;     // Vertical padding offset (pixels)
    int new_w;     // Scaled width before padding
    int new_h;     // Scaled height before padding
};

// --- Standalone functions (testable independently) ---

// Non-Maximum Suppression: per-class greedy selection.
// Deterministic: same input produces same output order.
std::vector<Detection> nms(std::vector<Detection> dets, float iou_threshold);

// Compute IoU (Intersection over Union) between two detections.
float compute_iou(const Detection& a, const Detection& b);

// Letterbox resize: bilinear interpolation, gray (128) padding.
// Output is always 640x640, NCHW float32 [0.0, 1.0].
// Returns LetterboxInfo for coordinate restoration.
LetterboxInfo letterbox_resize(const uint8_t* src, int src_w, int src_h,
                               std::vector<float>& dst_nchw);

// Restore detection coordinates from letterbox space to original image
// normalized coordinates [0.0, 1.0], clamped.
void restore_coordinates(std::vector<Detection>& dets,
                         const LetterboxInfo& info,
                         int orig_w, int orig_h);

// --- YoloDetector class ---

class YoloDetector {
public:
    // Factory method: create detector from ONNX model file.
    // Returns unique_ptr on success, nullptr on failure.
    // error_msg receives the error detail if non-null.
    static std::unique_ptr<YoloDetector> create(
        const std::string& model_path,
        const DetectorConfig& config = DetectorConfig{},
        std::string* error_msg = nullptr);

    ~YoloDetector();

    // No copy
    YoloDetector(const YoloDetector&) = delete;
    YoloDetector& operator=(const YoloDetector&) = delete;

    // Move
    YoloDetector(YoloDetector&& other) noexcept;
    YoloDetector& operator=(YoloDetector&& other) noexcept;

    // Run detection on RGB image data.
    // Returns detected objects with coordinates normalized to original image.
    std::vector<Detection> detect(const uint8_t* data, int width, int height);

    // Run detection with timing statistics.
    std::pair<std::vector<Detection>, InferenceStats>
    detect_with_stats(const uint8_t* data, int width, int height);

private:
    // Private constructor (use create() factory)
    YoloDetector(const DetectorConfig& config);

    // ONNX Runtime resource management (custom deleters)
    struct OrtEnvDeleter { void operator()(OrtEnv* p) const; };
    struct OrtSessionDeleter { void operator()(OrtSession* p) const; };
    struct OrtMemoryInfoDeleter { void operator()(OrtMemoryInfo* p) const; };

    std::unique_ptr<OrtEnv, OrtEnvDeleter> env_;
    std::unique_ptr<OrtSession, OrtSessionDeleter> session_;
    std::unique_ptr<OrtMemoryInfo, OrtMemoryInfoDeleter> memory_info_;

    DetectorConfig config_;

    // Cached input/output names and shapes (queried once at creation)
    std::string input_name_;
    std::string output_name_;
    int64_t input_h_ = 640;
    int64_t input_w_ = 640;
    int64_t num_classes_ = 80;
    int64_t num_proposals_ = 8400;

    // Reusable buffer for NCHW input tensor
    std::vector<float> input_buffer_;
};
