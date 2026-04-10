// yolo_detector.cpp
// YOLO object detector implementation using ONNX Runtime C API.
#include "yolo_detector.h"

#include <onnxruntime_c_api.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <unordered_map>

// ---------------------------------------------------------------------------
// ONNX Runtime API accessor (thread-safe lazy init via static local)
// ---------------------------------------------------------------------------
static const OrtApi* get_ort_api() {
    static const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    return api;
}

// ---------------------------------------------------------------------------
// Standalone functions
// ---------------------------------------------------------------------------

float compute_iou(const Detection& a, const Detection& b) {
    // Center (x,y,w,h) -> corner (x1,y1,x2,y2)
    float a_x1 = a.x - a.w / 2.0f, a_y1 = a.y - a.h / 2.0f;
    float a_x2 = a.x + a.w / 2.0f, a_y2 = a.y + a.h / 2.0f;
    float b_x1 = b.x - b.w / 2.0f, b_y1 = b.y - b.h / 2.0f;
    float b_x2 = b.x + b.w / 2.0f, b_y2 = b.y + b.h / 2.0f;

    float inter_x1 = std::max(a_x1, b_x1);
    float inter_y1 = std::max(a_y1, b_y1);
    float inter_x2 = std::min(a_x2, b_x2);
    float inter_y2 = std::min(a_y2, b_y2);

    float inter_area = std::max(0.0f, inter_x2 - inter_x1) *
                       std::max(0.0f, inter_y2 - inter_y1);
    float union_area = a.w * a.h + b.w * b.h - inter_area;

    return (union_area > 0.0f) ? inter_area / union_area : 0.0f;
}

std::vector<Detection> nms(std::vector<Detection> dets, float iou_threshold) {
    if (dets.empty()) return {};

    // Group by class_id
    std::unordered_map<int, std::vector<size_t>> class_groups;
    for (size_t i = 0; i < dets.size(); ++i) {
        class_groups[dets[i].class_id].push_back(i);
    }

    std::vector<Detection> result;

    for (auto& [cls, indices] : class_groups) {
        // Stable sort by confidence descending (deterministic)
        std::stable_sort(indices.begin(), indices.end(),
                         [&dets](size_t a, size_t b) {
                             return dets[a].confidence > dets[b].confidence;
                         });

        std::vector<bool> suppressed(indices.size(), false);

        for (size_t i = 0; i < indices.size(); ++i) {
            if (suppressed[i]) continue;
            result.push_back(dets[indices[i]]);

            for (size_t j = i + 1; j < indices.size(); ++j) {
                if (suppressed[j]) continue;
                if (compute_iou(dets[indices[i]], dets[indices[j]]) > iou_threshold) {
                    suppressed[j] = true;
                }
            }
        }
    }

    // Final sort by confidence descending for deterministic output
    std::stable_sort(result.begin(), result.end(),
                     [](const Detection& a, const Detection& b) {
                         return a.confidence > b.confidence;
                     });

    return result;
}

LetterboxInfo letterbox_resize(const uint8_t* src, int src_w, int src_h,
                               std::vector<float>& dst_nchw) {
    constexpr int TARGET = 640;
    dst_nchw.resize(1 * 3 * TARGET * TARGET);

    float scale = std::min(static_cast<float>(TARGET) / src_w,
                           static_cast<float>(TARGET) / src_h);
    int new_w = static_cast<int>(src_w * scale);
    int new_h = static_cast<int>(src_h * scale);
    int pad_x = (TARGET - new_w) / 2;
    int pad_y = (TARGET - new_h) / 2;

    // Fill with gray background (128/255 ~ 0.502)
    constexpr float GRAY = 128.0f / 255.0f;
    std::fill(dst_nchw.begin(), dst_nchw.end(), GRAY);

    // Bilinear interpolation + normalize + HWC->NCHW
    for (int dy = 0; dy < new_h; ++dy) {
        float sy = dy / scale;
        int sy0 = static_cast<int>(sy);
        int sy1 = std::min(sy0 + 1, src_h - 1);
        float fy = sy - sy0;

        for (int dx = 0; dx < new_w; ++dx) {
            float sx = dx / scale;
            int sx0 = static_cast<int>(sx);
            int sx1 = std::min(sx0 + 1, src_w - 1);
            float fx = sx - sx0;

            int dst_y = dy + pad_y;
            int dst_x = dx + pad_x;

            for (int c = 0; c < 3; ++c) {
                float p00 = src[(sy0 * src_w + sx0) * 3 + c];
                float p10 = src[(sy0 * src_w + sx1) * 3 + c];
                float p01 = src[(sy1 * src_w + sx0) * 3 + c];
                float p11 = src[(sy1 * src_w + sx1) * 3 + c];
                float val = (1.0f - fx) * (1.0f - fy) * p00 +
                            fx * (1.0f - fy) * p10 +
                            (1.0f - fx) * fy * p01 +
                            fx * fy * p11;
                dst_nchw[c * TARGET * TARGET + dst_y * TARGET + dst_x] =
                    std::clamp(val / 255.0f, 0.0f, 1.0f);
            }
        }
    }

    return {scale, pad_x, pad_y, new_w, new_h};
}

void restore_coordinates(std::vector<Detection>& dets,
                         const LetterboxInfo& info,
                         int orig_w, int orig_h) {
    constexpr int TARGET = 640;
    for (auto& d : dets) {
        // Letterbox normalized [0,1] -> pixel coordinates
        float cx_px = d.x * TARGET;
        float cy_px = d.y * TARGET;
        float w_px = d.w * TARGET;
        float h_px = d.h * TARGET;

        // Remove padding offset
        cx_px -= info.pad_x;
        cy_px -= info.pad_y;

        // Undo scaling
        cx_px /= info.scale;
        cy_px /= info.scale;
        w_px /= info.scale;
        h_px /= info.scale;

        // Normalize to original image [0,1] and clamp
        d.x = std::clamp(cx_px / orig_w, 0.0f, 1.0f);
        d.y = std::clamp(cy_px / orig_h, 0.0f, 1.0f);
        d.w = std::clamp(w_px / orig_w, 0.0f, 1.0f);
        d.h = std::clamp(h_px / orig_h, 0.0f, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// RAII custom deleters
// ---------------------------------------------------------------------------

void YoloDetector::OrtEnvDeleter::operator()(OrtEnv* p) const {
    if (p) get_ort_api()->ReleaseEnv(p);
}

void YoloDetector::OrtSessionDeleter::operator()(OrtSession* p) const {
    if (p) get_ort_api()->ReleaseSession(p);
}

void YoloDetector::OrtMemoryInfoDeleter::operator()(OrtMemoryInfo* p) const {
    if (p) get_ort_api()->ReleaseMemoryInfo(p);
}

// Helper: check OrtStatus and extract error message, then release status
static bool check_ort_status(const OrtApi* ort, OrtStatus* status,
                             std::string* error_msg) {
    if (status != nullptr) {
        const char* msg = ort->GetErrorMessage(status);
        if (error_msg) *error_msg = msg;
        ort->ReleaseStatus(status);
        return false;
    }
    return true;
}

// OrtValue deleter for unique_ptr
struct OrtValueDeleter {
    void operator()(OrtValue* p) const {
        if (p) get_ort_api()->ReleaseValue(p);
    }
};
using OrtValuePtr = std::unique_ptr<OrtValue, OrtValueDeleter>;

// ---------------------------------------------------------------------------
// Private constructor
// ---------------------------------------------------------------------------

YoloDetector::YoloDetector(const DetectorConfig& config)
    : config_(config) {}

// ---------------------------------------------------------------------------
// Destructor (unique_ptr deleters handle cleanup)
// ---------------------------------------------------------------------------

YoloDetector::~YoloDetector() = default;

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

YoloDetector::YoloDetector(YoloDetector&& other) noexcept
    : env_(std::move(other.env_)),
      session_(std::move(other.session_)),
      memory_info_(std::move(other.memory_info_)),
      config_(other.config_),
      input_name_(std::move(other.input_name_)),
      output_name_(std::move(other.output_name_)),
      input_h_(other.input_h_),
      input_w_(other.input_w_),
      num_classes_(other.num_classes_),
      num_proposals_(other.num_proposals_),
      input_buffer_(std::move(other.input_buffer_)) {}

YoloDetector& YoloDetector::operator=(YoloDetector&& other) noexcept {
    if (this != &other) {
        env_ = std::move(other.env_);
        session_ = std::move(other.session_);
        memory_info_ = std::move(other.memory_info_);
        config_ = other.config_;
        input_name_ = std::move(other.input_name_);
        output_name_ = std::move(other.output_name_);
        input_h_ = other.input_h_;
        input_w_ = other.input_w_;
        num_classes_ = other.num_classes_;
        num_proposals_ = other.num_proposals_;
        input_buffer_ = std::move(other.input_buffer_);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Factory method: create()
// ---------------------------------------------------------------------------

std::unique_ptr<YoloDetector> YoloDetector::create(
    const std::string& model_path,
    const DetectorConfig& config,
    std::string* error_msg) {

    const OrtApi* ort = get_ort_api();

    // 1. Check file existence
    if (!std::filesystem::exists(model_path)) {
        if (error_msg) *error_msg = "Model file not found: " + model_path;
        spdlog::error("Model file not found: {}", model_path);
        return nullptr;
    }

    // 2. Create OrtEnv
    OrtEnv* env_raw = nullptr;
    OrtStatus* status = ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "yolo", &env_raw);
    if (!check_ort_status(ort, status, error_msg)) {
        spdlog::error("Failed to create ORT environment");
        return nullptr;
    }

    // 3. Create SessionOptions and set thread count
    OrtSessionOptions* opts = nullptr;
    status = ort->CreateSessionOptions(&opts);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseEnv(env_raw);
        spdlog::error("Failed to create session options");
        return nullptr;
    }

    status = ort->SetIntraOpNumThreads(opts, config.num_threads);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseSessionOptions(opts);
        ort->ReleaseEnv(env_raw);
        spdlog::error("Failed to set intra-op thread count");
        return nullptr;
    }

    // 3b. Set inter-op thread count (non-fatal on failure)
    status = ort->SetInterOpNumThreads(opts, config.inter_op_num_threads);
    if (!check_ort_status(ort, status, error_msg)) {
        spdlog::warn("Failed to set inter-op threads: {}", config.inter_op_num_threads);
    }
    spdlog::info("Threads: intra-op={}, inter-op={}", config.num_threads, config.inter_op_num_threads);

    // 3c. Set graph optimization level (fatal on failure)
    auto opt_level = static_cast<GraphOptimizationLevel>(config.graph_optimization_level);
    status = ort->SetSessionGraphOptimizationLevel(opts, opt_level);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseSessionOptions(opts);
        ort->ReleaseEnv(env_raw);
        spdlog::error("Failed to set graph optimization level: {}", config.graph_optimization_level);
        return nullptr;
    }
    spdlog::info("Graph optimization level: {}", config.graph_optimization_level);

    // 3d. XNNPACK EP registration (non-fatal on failure)
    if (config.use_xnnpack) {
        status = ort->SessionOptionsAppendExecutionProvider(opts, "XNNPACK", nullptr, nullptr, 0);
        if (status != nullptr) {
            const char* msg = ort->GetErrorMessage(status);
            spdlog::warn("XNNPACK EP not available: {}, falling back to CPU EP", msg);
            ort->ReleaseStatus(status);
        } else {
            spdlog::info("Execution Provider: XNNPACK");
        }
    } else {
        spdlog::info("Execution Provider: CPU");
    }

    // 4. Create Session
    OrtSession* session_raw = nullptr;
    status = ort->CreateSession(env_raw, model_path.c_str(), opts, &session_raw);
    ort->ReleaseSessionOptions(opts);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseEnv(env_raw);
        spdlog::error("Failed to create session from: {}", model_path);
        return nullptr;
    }

    // 5. Create MemoryInfo
    OrtMemoryInfo* mem_raw = nullptr;
    status = ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_raw);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        spdlog::error("Failed to create CPU memory info");
        return nullptr;
    }

    // 6. Query input/output names via allocator
    OrtAllocator* allocator = nullptr;
    status = ort->GetAllocatorWithDefaultOptions(&allocator);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }

    // Get input name
    char* input_name_raw = nullptr;
    status = ort->SessionGetInputName(session_raw, 0, allocator, &input_name_raw);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }
    std::string input_name(input_name_raw);
    status = ort->AllocatorFree(allocator, input_name_raw);
    if (status) ort->ReleaseStatus(status);

    // Get output name
    char* output_name_raw = nullptr;
    status = ort->SessionGetOutputName(session_raw, 0, allocator, &output_name_raw);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }
    std::string output_name(output_name_raw);
    status = ort->AllocatorFree(allocator, output_name_raw);
    if (status) ort->ReleaseStatus(status);

    // 7. Query input shape via TypeInfo
    OrtTypeInfo* input_type_info = nullptr;
    status = ort->SessionGetInputTypeInfo(session_raw, 0, &input_type_info);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }

    const OrtTensorTypeAndShapeInfo* input_tensor_info = nullptr;
    status = ort->CastTypeInfoToTensorInfo(input_type_info, &input_tensor_info);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseTypeInfo(input_type_info);
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }

    size_t input_dims_count = 0;
    status = ort->GetDimensionsCount(input_tensor_info, &input_dims_count);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseTypeInfo(input_type_info);
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }
    std::vector<int64_t> input_dims(input_dims_count);
    status = ort->GetDimensions(input_tensor_info, input_dims.data(), input_dims_count);
    ort->ReleaseTypeInfo(input_type_info);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }

    // Query output shape
    OrtTypeInfo* output_type_info = nullptr;
    status = ort->SessionGetOutputTypeInfo(session_raw, 0, &output_type_info);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }

    const OrtTensorTypeAndShapeInfo* output_tensor_info = nullptr;
    status = ort->CastTypeInfoToTensorInfo(output_type_info, &output_tensor_info);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseTypeInfo(output_type_info);
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }

    size_t output_dims_count = 0;
    status = ort->GetDimensionsCount(output_tensor_info, &output_dims_count);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseTypeInfo(output_type_info);
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }
    std::vector<int64_t> output_dims(output_dims_count);
    status = ort->GetDimensions(output_tensor_info, output_dims.data(), output_dims_count);
    ort->ReleaseTypeInfo(output_type_info);
    if (!check_ort_status(ort, status, error_msg)) {
        ort->ReleaseMemoryInfo(mem_raw);
        ort->ReleaseSession(session_raw);
        ort->ReleaseEnv(env_raw);
        return nullptr;
    }

    // 8. Construct YoloDetector and transfer ownership
    auto detector = std::unique_ptr<YoloDetector>(new YoloDetector(config));
    detector->env_.reset(env_raw);
    detector->session_.reset(session_raw);
    detector->memory_info_.reset(mem_raw);
    detector->input_name_ = std::move(input_name);
    detector->output_name_ = std::move(output_name);

    // Parse input shape [1, 3, H, W]
    if (input_dims.size() == 4) {
        detector->input_h_ = input_dims[2];
        detector->input_w_ = input_dims[3];
    }

    // Parse output shape [1, 84, 8400]
    if (output_dims.size() == 3) {
        detector->num_classes_ = output_dims[1] - 4; // 84 - 4 = 80
        detector->num_proposals_ = output_dims[2];
    }

    spdlog::info("YOLO model loaded: {} | input: [1,3,{},{}] | output: [1,{},{}]",
                 model_path, detector->input_h_, detector->input_w_,
                 detector->num_classes_ + 4, detector->num_proposals_);

    return detector;
}

// ---------------------------------------------------------------------------
// detect_with_stats()
// ---------------------------------------------------------------------------

std::pair<std::vector<Detection>, InferenceStats>
YoloDetector::detect_with_stats(const uint8_t* data, int width, int height) {
    InferenceStats stats;
    const OrtApi* ort = get_ort_api();

    // Validate input
    if (!data) {
        spdlog::warn("Null input data, skipping inference");
        return {{}, stats};
    }
    if (width <= 0 || height <= 0) {
        spdlog::warn("Invalid input dimensions: {}x{}", width, height);
        return {{}, stats};
    }

    auto t0 = std::chrono::steady_clock::now();

    // --- Preprocess ---
    auto info = letterbox_resize(data, width, height, input_buffer_);
    auto t1 = std::chrono::steady_clock::now();
    stats.preprocess_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // --- Inference ---
    // Create input tensor (data owned by input_buffer_, not copied)
    int64_t input_shape[] = {1, 3, input_h_, input_w_};
    size_t input_data_len = input_buffer_.size() * sizeof(float);

    OrtValue* input_tensor_raw = nullptr;
    OrtStatus* status = ort->CreateTensorWithDataAsOrtValue(
        memory_info_.get(),
        input_buffer_.data(), input_data_len,
        input_shape, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &input_tensor_raw);
    if (status) {
        const char* msg = ort->GetErrorMessage(status);
        spdlog::error("Failed to create input tensor: {}", msg);
        ort->ReleaseStatus(status);
        return {{}, stats};
    }
    OrtValuePtr input_tensor(input_tensor_raw);

    // Run inference
    const char* input_names[] = {input_name_.c_str()};
    const char* output_names[] = {output_name_.c_str()};
    OrtValue* output_tensor_raw = nullptr;
    const OrtValue* input_values[] = {input_tensor.get()};

    status = ort->Run(session_.get(), nullptr,
                      input_names, input_values, 1,
                      output_names, 1, &output_tensor_raw);
    if (status) {
        const char* msg = ort->GetErrorMessage(status);
        spdlog::error("Inference failed: {}", msg);
        ort->ReleaseStatus(status);
        return {{}, stats};
    }
    OrtValuePtr output_tensor(output_tensor_raw);

    auto t2 = std::chrono::steady_clock::now();
    stats.inference_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

    // --- Postprocess ---
    // Get output data pointer: shape [1, 84, 8400]
    float* output_data = nullptr;
    status = ort->GetTensorMutableData(output_tensor.get(),
                                       reinterpret_cast<void**>(&output_data));
    if (status) {
        const char* msg = ort->GetErrorMessage(status);
        spdlog::error("Failed to get output tensor data: {}", msg);
        ort->ReleaseStatus(status);
        return {{}, stats};
    }

    // Parse output: [1, 84, 8400] where rows 0-3 are bbox (cx, cy, w, h)
    // and rows 4-83 are class scores
    const int64_t total_attrs = num_classes_ + 4;
    std::vector<Detection> detections;

    for (int64_t i = 0; i < num_proposals_; ++i) {
        // Find max class score for this proposal
        float max_score = 0.0f;
        int max_class = 0;
        for (int64_t c = 0; c < num_classes_; ++c) {
            float score = output_data[(4 + c) * num_proposals_ + i];
            if (score > max_score) {
                max_score = score;
                max_class = static_cast<int>(c);
            }
        }

        // Filter by confidence threshold
        if (max_score < config_.confidence_threshold) continue;

        // Extract bbox in letterbox normalized space
        float cx = output_data[0 * num_proposals_ + i] / static_cast<float>(input_w_);
        float cy = output_data[1 * num_proposals_ + i] / static_cast<float>(input_h_);
        float bw = output_data[2 * num_proposals_ + i] / static_cast<float>(input_w_);
        float bh = output_data[3 * num_proposals_ + i] / static_cast<float>(input_h_);

        detections.push_back({cx, cy, bw, bh, max_class, max_score});
    }

    // NMS
    detections = nms(std::move(detections), config_.iou_threshold);

    // Restore coordinates to original image space
    restore_coordinates(detections, info, width, height);

    auto t3 = std::chrono::steady_clock::now();
    stats.postprocess_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    stats.total_ms = std::chrono::duration<double, std::milli>(t3 - t0).count();

    spdlog::debug("Inference: pre={:.1f}ms infer={:.1f}ms post={:.1f}ms total={:.1f}ms detections={}",
                  stats.preprocess_ms, stats.inference_ms,
                  stats.postprocess_ms, stats.total_ms, detections.size());

    return {std::move(detections), stats};
}

// ---------------------------------------------------------------------------
// detect() — delegates to detect_with_stats()
// ---------------------------------------------------------------------------

std::vector<Detection> YoloDetector::detect(
    const uint8_t* data, int width, int height) {
    return detect_with_stats(data, width, height).first;
}
