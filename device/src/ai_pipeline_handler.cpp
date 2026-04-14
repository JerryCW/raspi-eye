// ai_pipeline_handler.cpp
// AI inference pipeline handler implementation.
#include "ai_pipeline_handler.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <spdlog/spdlog.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <nlohmann/json.hpp>

// COCO 80-class names (indices 0-79)
static const char* kCocoNames[] = {
    "person",        "bicycle",      "car",           "motorcycle",
    "airplane",      "bus",          "train",         "truck",
    "boat",          "traffic light","fire hydrant",  "stop sign",
    "parking meter", "bench",        "bird",          "cat",
    "dog",           "horse",        "sheep",         "cow",
    "elephant",      "bear",         "zebra",         "giraffe",
    "backpack",      "umbrella",     "handbag",       "tie",
    "suitcase",      "frisbee",      "skis",          "snowboard",
    "sports ball",   "kite",         "baseball bat",  "baseball glove",
    "skateboard",    "surfboard",    "tennis racket", "bottle",
    "wine glass",    "cup",          "fork",          "knife",
    "spoon",         "bowl",         "banana",        "apple",
    "sandwich",      "orange",       "broccoli",      "carrot",
    "hot dog",       "pizza",        "donut",         "cake",
    "chair",         "couch",        "potted plant",  "bed",
    "dining table",  "toilet",       "tv",            "laptop",
    "mouse",         "remote",       "keyboard",      "cell phone",
    "microwave",     "oven",         "toaster",       "sink",
    "refrigerator",  "book",         "clock",         "vase",
    "scissors",      "teddy bear",   "hair drier",    "toothbrush",
};

const char* coco_class_name(int class_id) {
    if (class_id < 0 || class_id >= 80) {
        return "unknown";
    }
    return kCocoNames[class_id];
}

void i420_to_rgb(const uint8_t* y_plane, const uint8_t* u_plane,
                 const uint8_t* v_plane,
                 int width, int height, int y_stride, int uv_stride,
                 uint8_t* rgb_out) {
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            int y_val = y_plane[row * y_stride + col];
            int u_val = u_plane[(row / 2) * uv_stride + (col / 2)] - 128;
            int v_val = v_plane[(row / 2) * uv_stride + (col / 2)] - 128;
            int r = y_val + ((359 * v_val) >> 8);
            int g = y_val - ((88 * u_val + 183 * v_val) >> 8);
            int b = y_val + ((454 * u_val) >> 8);
            int idx = (row * width + col) * 3;
            rgb_out[idx + 0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            rgb_out[idx + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            rgb_out[idx + 2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
        }
    }
}

std::vector<Detection> filter_detections(
    const std::vector<Detection>& detections,
    const std::vector<AiConfig::TargetClass>& target_classes,
    float global_threshold) {
    std::vector<Detection> result;

    for (const auto& det : detections) {
        const char* name = coco_class_name(det.class_id);

        if (target_classes.empty()) {
            // No target classes configured: keep all detections above global threshold
            if (det.confidence >= global_threshold) {
                result.push_back(det);
            }
        } else {
            // Check if this detection's class is in target_classes
            for (const auto& tc : target_classes) {
                if (tc.name == name) {
                    // Use per-class threshold if set (>= 0), otherwise global
                    float threshold = (tc.confidence >= 0.0f) ? tc.confidence
                                                              : global_threshold;
                    if (det.confidence >= threshold) {
                        result.push_back(det);
                    }
                    break;
                }
            }
        }
    }

    return result;
}

bool should_sample(int64_t elapsed_ms, int fps) {
    return elapsed_ms >= 1000 / fps;
}

// --- Top-K min-heap pure function ---

bool try_submit_to_topk(std::vector<SnapshotEntry>& heap, int max_k,
                        SnapshotEntry candidate) {
    if (max_k <= 0) return false;

    if (static_cast<int>(heap.size()) < max_k) {
        // Cache not full: add candidate
        heap.push_back(std::move(candidate));
        std::push_heap(heap.begin(), heap.end(), SnapshotMinHeapCmp{});
        return true;
    }

    // Cache full: compare with lowest confidence (heap front)
    if (candidate.confidence > heap.front().confidence) {
        // Replace lowest
        std::pop_heap(heap.begin(), heap.end(), SnapshotMinHeapCmp{});
        heap.back() = std::move(candidate);
        std::push_heap(heap.begin(), heap.end(), SnapshotMinHeapCmp{});
        return true;
    }

    // Candidate confidence <= lowest: discard
    return false;
}

// --- AiPipelineHandler class implementation ---

AiPipelineHandler::AiPipelineHandler(std::unique_ptr<YoloDetector> detector,
                                     const AiConfig& config)
    : detector_(std::move(detector)), config_(config) {
    current_fps_ = config_.idle_fps;
}

AiPipelineHandler::~AiPipelineHandler() {
    stop();
    remove_probe();
}

std::unique_ptr<AiPipelineHandler> AiPipelineHandler::create(
    std::unique_ptr<YoloDetector> detector,
    const AiConfig& config,
    std::string* error_msg) {
    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    if (!detector) {
        if (error_msg) {
            *error_msg = "YoloDetector is nullptr";
        }
        ai_log->error("AiPipelineHandler::create() failed: YoloDetector is nullptr");
        return nullptr;
    }

    // Use raw new since constructor is private (can't use make_unique)
    std::unique_ptr<AiPipelineHandler> handler(
        new AiPipelineHandler(std::move(detector), config));

    ai_log->info("AiPipelineHandler created: model={}, idle_fps={}, active_fps={}, "
                 "confidence_threshold={:.2f}",
                 config.model_path, config.idle_fps, config.active_fps,
                 config.confidence_threshold);

    return handler;
}

bool AiPipelineHandler::install_probe(GstElement* pipeline, std::string* error_msg) {
    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    // Remove old probe if one exists
    if (probe_pad_ != nullptr) {
        remove_probe();
    }

    GstElement* q_ai = gst_bin_get_by_name(GST_BIN(pipeline), "q-ai");
    if (!q_ai) {
        if (error_msg) {
            *error_msg = "Element 'q-ai' not found in pipeline";
        }
        ai_log->error("install_probe failed: element 'q-ai' not found in pipeline");
        return false;
    }

    GstPad* src_pad = gst_element_get_static_pad(q_ai, "src");
    if (!src_pad) {
        gst_object_unref(q_ai);
        if (error_msg) {
            *error_msg = "Failed to get src pad from 'q-ai'";
        }
        ai_log->error("install_probe failed: cannot get src pad from 'q-ai'");
        return false;
    }

    probe_id_ = gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                   buffer_probe_cb, this, nullptr);
    probe_pad_ = src_pad;  // Take ownership of the ref

    gst_object_unref(q_ai);

    ai_log->info("Buffer probe installed on q-ai src pad, probe_id={}", probe_id_);
    return true;
}

void AiPipelineHandler::remove_probe() {
    if (probe_pad_ != nullptr && probe_id_ != 0) {
        // Check if the pad's parent pipeline is still alive
        if (GST_OBJECT_PARENT(probe_pad_) != nullptr) {
            gst_pad_remove_probe(probe_pad_, probe_id_);
        }
        gst_object_unref(probe_pad_);
    }
    probe_pad_ = nullptr;
    probe_id_ = 0;
}

GstPadProbeReturn AiPipelineHandler::buffer_probe_cb(
    GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    auto* self = static_cast<AiPipelineHandler*>(user_data);

    // Check sampling interval using adaptive fps
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - self->last_sample_time_).count();
    if (!should_sample(elapsed, self->current_fps_.load())) {
        return GST_PAD_PROBE_OK;
    }

    // Check busy flag — skip if inference thread is still processing
    if (self->busy_.load()) {
        return GST_PAD_PROBE_OK;
    }

    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    // Get frame dimensions from pad caps (not GstVideoMeta)
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        ai_log->warn("buffer_probe_cb: failed to get current caps, skipping frame");
        return GST_PAD_PROBE_OK;
    }
    GstStructure* s = gst_caps_get_structure(caps, 0);
    int w = 0, h = 0;
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);
    gst_caps_unref(caps);

    if (w <= 0 || h <= 0) {
        ai_log->warn("buffer_probe_cb: invalid frame dimensions {}x{}", w, h);
        return GST_PAD_PROBE_OK;
    }

    // Map I420 buffer
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        ai_log->warn("buffer_probe_cb: failed to map buffer");
        return GST_PAD_PROBE_OK;
    }

    // I420 layout: Y plane (w*h) + U plane (w/2 * h/2) + V plane (w/2 * h/2)
    const uint8_t* y_plane = map.data;
    int y_stride = w;
    int uv_stride = w / 2;
    const uint8_t* u_plane = y_plane + y_stride * h;
    const uint8_t* v_plane = u_plane + uv_stride * (h / 2);

    // Ensure RGB buffer is large enough
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        size_t needed = static_cast<size_t>(w) * h * 3;
        if (self->rgb_buffer_.size() < needed) {
            self->rgb_buffer_.resize(needed);
        }
        self->frame_width_ = w;
        self->frame_height_ = h;

        // Convert I420 to RGB
        i420_to_rgb(y_plane, u_plane, v_plane, w, h, y_stride, uv_stride,
                    self->rgb_buffer_.data());

        self->frame_ready_ = true;
    }

    gst_buffer_unmap(buffer, &map);

    self->last_sample_time_ = now;
    self->cv_.notify_one();

    return GST_PAD_PROBE_OK;
}

bool AiPipelineHandler::start(std::string* error_msg) {
    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    stop_flag_ = false;
    inference_thread_ = std::thread(&AiPipelineHandler::inference_loop, this);
    ai_log->info("Inference thread started");
    return true;
}

void AiPipelineHandler::stop() {
    stop_flag_ = true;
    cv_.notify_one();

    if (inference_thread_.joinable()) {
        inference_thread_.join();
    }

    if (event_state_ == EventState::CONFIRMED || event_state_ == EventState::CLOSING) {
        close_event();
    } else if (event_state_ == EventState::PENDING) {
        // Discard pending event without disk write
        event_state_ = EventState::IDLE;
        consecutive_detection_count_ = 0;
        cached_frames_.clear();
        cached_bytes_ = 0;
        detections_summary_.clear();
        window_candidate_.reset();
        snapshot_heap_.clear();
        current_fps_ = config_.idle_fps;
    }
}

void AiPipelineHandler::inference_loop() {
    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    while (!stop_flag_) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return frame_ready_ || stop_flag_.load(); });

        if (stop_flag_) break;

        // Swap frame buffer under lock
        std::vector<uint8_t> frame;
        frame.swap(rgb_buffer_);
        int width = frame_width_;
        int height = frame_height_;
        frame_ready_ = false;
        busy_ = true;
        lock.unlock();

        // Run inference (no lock held, no GStreamer objects referenced)
        auto start_time = std::chrono::steady_clock::now();

        try {
            auto [detections, stats] = detector_->detect_with_stats(
                frame.data(), width, height);

            // Filter by target_classes
            auto filtered = filter_detections(
                detections, config_.target_classes, config_.confidence_threshold);

            bool has_detections = !filtered.empty();

            // Compute max confidence from filtered detections for window candidate
            float max_confidence = 0.0f;
            if (has_detections) {
                for (const auto& det : filtered) {
                    max_confidence = std::max(max_confidence, det.confidence);
                }
            }

            // --- Two-phase event confirmation state machine ---
            if (has_detections) {
                switch (event_state_) {
                    case EventState::IDLE: {
                        // First detection: enter PENDING state
                        open_event(filtered);
                        event_state_ = EventState::PENDING;
                        consecutive_detection_count_ = 1;
                        // Switch to active fps
                        int old_fps = current_fps_.load();
                        current_fps_ = config_.active_fps;
                        ai_log->info("Inference fps: {} -> {}, reason=detection",
                                     old_fps, config_.active_fps);
                        // Accumulate detections summary from first detection
                        update_detections_summary(filtered);
                        // Update window candidate (replaces encode_snapshot)
                        update_window_candidate(frame, width, height, max_confidence);
                        last_detection_time_ = std::chrono::steady_clock::now();
                        break;
                    }
                    case EventState::PENDING: {
                        // Subsequent detection in pending state
                        consecutive_detection_count_++;
                        update_detections_summary(filtered);
                        update_window_candidate(frame, width, height, max_confidence);
                        last_detection_time_ = std::chrono::steady_clock::now();

                        if (consecutive_detection_count_ >= kConfirmationThreshold) {
                            // Upgrade to CONFIRMED
                            event_state_ = EventState::CONFIRMED;
                            ai_log->info("Event confirmed: event_id={}, after {} consecutive detections",
                                         event_id_, consecutive_detection_count_);
                        }
                        break;
                    }
                    case EventState::CONFIRMED: {
                        // Continued detection in confirmed state
                        update_detections_summary(filtered);
                        update_window_candidate(frame, width, height, max_confidence);
                        last_detection_time_ = std::chrono::steady_clock::now();

                        // detection_callback only for CONFIRMED events
                        if (detection_cb_) {
                            detection_cb_(filtered, stats, frame.data(), width, height);
                        }
                        break;
                    }
                    case EventState::CLOSING:
                        // Should not normally receive detections during CLOSING
                        break;
                }
            } else {
                // No detections
                switch (event_state_) {
                    case EventState::PENDING: {
                        // Pending event interrupted: discard
                        ai_log->debug("Pending event discarded: detection interrupted");
                        consecutive_detection_count_ = 0;
                        cached_frames_.clear();
                        cached_bytes_ = 0;
                        detections_summary_.clear();
                        frame_count_ = 0;
                        window_candidate_.reset();
                        snapshot_heap_.clear();
                        event_state_ = EventState::IDLE;
                        // Switch back to idle fps
                        current_fps_ = config_.idle_fps;
                        break;
                    }
                    case EventState::CONFIRMED: {
                        // Check timeout for confirmed events
                        // (timeout check is done below for both detection/no-detection cases)
                        break;
                    }
                    default:
                        break;
                }
            }

            // Check event timeout for CONFIRMED state
            if (event_state_ == EventState::CONFIRMED) {
                check_event_timeout();
            }

            auto end_time = std::chrono::steady_clock::now();
            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_time - start_time).count();

            if (has_detections) {
                // Detected targets -> info level summary
                std::ostringstream summary;
                for (size_t i = 0; i < filtered.size(); ++i) {
                    if (i > 0) summary << ", ";
                    summary << coco_class_name(filtered[i].class_id)
                            << "(" << std::fixed << std::setprecision(2)
                            << filtered[i].confidence << ")";
                }
                ai_log->info("Inference: {}ms, detected: {}", duration_ms, summary.str());
            } else {
                // No targets -> debug level
                ai_log->debug("Inference: {}ms, {} raw detections, 0 after filter",
                              duration_ms, detections.size());
            }

        } catch (const std::exception& e) {
            ai_log->error("Inference exception: {}", e.what());
        }

        // Restore rgb_buffer_ for next frame
        {
            std::lock_guard<std::mutex> lg(mutex_);
            rgb_buffer_ = std::move(frame);
        }
        busy_ = false;
    }
}

void AiPipelineHandler::set_detection_callback(DetectionCallback cb) {
    detection_cb_ = std::move(cb);
}

void AiPipelineHandler::set_event_close_callback(EventCloseCallback cb) {
    event_close_cb_ = std::move(cb);
}

// --- Event management helpers ---

static std::string format_utc_time(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val{};
    gmtime_r(&time_t_val, &tm_val);
    std::ostringstream oss;
    oss << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

static std::string format_timestamp(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val{};
    gmtime_r(&time_t_val, &tm_val);
    std::ostringstream oss;
    oss << std::put_time(&tm_val, "%Y%m%d_%H%M%S");
    return oss.str();
}

static void stbi_write_callback(void* context, void* data, int size) {
    auto* vec = static_cast<std::vector<uint8_t>*>(context);
    auto* bytes = static_cast<const uint8_t*>(data);
    vec->insert(vec->end(), bytes, bytes + size);
}

// --- Event management implementation ---

void AiPipelineHandler::open_event(const std::vector<Detection>& detections) {
    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    auto now_sys = std::chrono::system_clock::now();
    event_id_ = "evt_" + format_timestamp(now_sys);
    event_start_time_ = now_sys;
    last_detection_time_ = std::chrono::steady_clock::now();
    // event_state_ is set by the caller (inference_loop state machine)
    frame_count_ = 0;
    cached_frames_.clear();
    cached_bytes_ = 0;
    event_dir_created_ = false;
    detections_summary_.clear();

    // Reset smart snapshot selection state
    window_candidate_.reset();
    window_start_ = std::chrono::steady_clock::now();
    snapshot_heap_.clear();

    // Log trigger info: first detection's class + confidence
    if (!detections.empty()) {
        const char* trigger_class = coco_class_name(detections[0].class_id);
        ai_log->info("Event opened: event_id={}, trigger_class={}, confidence={:.2f}",
                     event_id_, trigger_class, detections[0].confidence);
    } else {
        ai_log->info("Event opened: event_id={}", event_id_);
    }
}

void AiPipelineHandler::close_event() {
    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    event_state_ = EventState::CLOSING;
    auto end_time = std::chrono::system_clock::now();

    // Flush any pending window candidate to Top-K before writing
    submit_window_candidate();
    window_candidate_.reset();

    // Skip disk write for empty events
    if (snapshot_heap_.empty()) {
        event_state_ = EventState::IDLE;
        consecutive_detection_count_ = 0;
        ai_log->info("Event closed (empty, no disk write): event_id={}", event_id_);
        // Switch back to idle fps
        int old_fps = current_fps_.load();
        current_fps_ = config_.idle_fps;
        if (old_fps != config_.idle_fps) {
            ai_log->info("Inference fps: {} -> {}, reason=timeout", old_fps, config_.idle_fps);
        }
        return;
    }

    // Sort snapshot_heap_ by timestamp for chronological disk write
    std::sort(snapshot_heap_.begin(), snapshot_heap_.end(),
              [](const SnapshotEntry& a, const SnapshotEntry& b) {
                  return a.timestamp < b.timestamp;
              });

    // frame_count = actual number of snapshots written to disk
    frame_count_ = static_cast<int>(snapshot_heap_.size());

    // Batch write to disk
    bool disk_write_ok = true;
    try {
        std::string event_dir = config_.snapshot_dir + event_id_;

        // Create event directory if not yet created
        if (!event_dir_created_) {
            std::filesystem::create_directories(event_dir);
            event_dir_created_ = true;
        }

        // Write all snapshot JPEG files
        for (const auto& entry : snapshot_heap_) {
            std::string filepath = event_dir + "/" + entry.filename;
            std::ofstream ofs(filepath, std::ios::binary);
            if (ofs) {
                ofs.write(reinterpret_cast<const char*>(entry.jpeg_data.data()),
                          static_cast<std::streamsize>(entry.jpeg_data.size()));
            } else {
                ai_log->warn("Failed to write snapshot: {}", filepath);
            }
        }

        // Build event.json using nlohmann/json
        nlohmann::json event_json;
        event_json["event_id"] = event_id_;
        event_json["device_id"] = config_.device_id;
        event_json["start_time"] = format_utc_time(event_start_time_);
        event_json["end_time"] = format_utc_time(end_time);
        event_json["status"] = "closed";
        event_json["frame_count"] = frame_count_;

        nlohmann::json summary_json = nlohmann::json::object();
        for (const auto& [name, stats] : detections_summary_) {
            summary_json[name] = {
                {"count", stats.count},
                {"max_confidence", stats.max_confidence}
            };
        }
        event_json["detections_summary"] = summary_json;

        // Write event.json
        std::string json_path = event_dir + "/event.json";
        std::ofstream json_ofs(json_path);
        if (json_ofs) {
            json_ofs << event_json.dump(4);
        } else {
            ai_log->warn("Failed to write event.json: {}", json_path);
            disk_write_ok = false;
        }

    } catch (const std::exception& e) {
        ai_log->warn("Error during event close disk write: {}", e.what());
        disk_write_ok = false;
    }

    // Calculate duration
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        end_time - event_start_time_).count();

    std::string event_dir = config_.snapshot_dir + event_id_;
    ai_log->info("Event closed: event_id={}, duration={}s, snapshots={}, dir={}",
                 event_id_, duration, frame_count_, event_dir);

    // Call event close callback if registered and disk write succeeded
    if (disk_write_ok && event_close_cb_) {
        event_close_cb_();
    } else if (!disk_write_ok) {
        ai_log->warn("Skipping event close callback due to disk write failure: event_id={}", event_id_);
    }

    // Clear cache and reset state
    cached_frames_.clear();
    cached_bytes_ = 0;
    snapshot_heap_.clear();
    consecutive_detection_count_ = 0;
    event_state_ = EventState::IDLE;

    // Switch back to idle fps
    int old_fps = current_fps_.load();
    current_fps_ = config_.idle_fps;
    if (old_fps != config_.idle_fps) {
        ai_log->info("Inference fps: {} -> {}, reason=timeout", old_fps, config_.idle_fps);
    }
}

void AiPipelineHandler::encode_snapshot(const uint8_t* rgb_data, int width, int height) {
    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    // Generate filename: YYYYMMDD_HHMMSS_NNN.jpg
    auto now_sys = std::chrono::system_clock::now();
    std::string ts = format_timestamp(now_sys);
    int seq = frame_count_ + 1;

    std::ostringstream fname;
    fname << ts << "_" << std::setfill('0') << std::setw(3) << seq << ".jpg";

    // Encode JPEG to memory using stbi_write_jpg_to_func
    std::vector<uint8_t> jpeg_data;
    int result = stbi_write_jpg_to_func(stbi_write_callback, &jpeg_data,
                                         width, height, 3, rgb_data, 85);
    if (result == 0 || jpeg_data.empty()) {
        ai_log->warn("JPEG encoding failed for frame {}", seq);
        return;
    }

    // Cache the frame
    CachedFrame cached;
    cached.filename = fname.str();
    cached.jpeg_data = std::move(jpeg_data);
    cached_bytes_ += cached.jpeg_data.size();
    cached_frames_.push_back(std::move(cached));

    frame_count_++;

    // Check if cache exceeds limit
    size_t max_bytes = static_cast<size_t>(config_.max_cache_mb) * 1024 * 1024;
    if (cached_bytes_ >= max_bytes) {
        flush_cache();
    }
}

void AiPipelineHandler::update_window_candidate(const std::vector<uint8_t>& rgb_frame,
                                                 int width, int height, float confidence) {
    auto now_steady = std::chrono::steady_clock::now();

    // Check if 1-second window has elapsed
    auto window_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now_steady - window_start_).count();
    if (window_elapsed >= 1000 && window_candidate_.has_value()) {
        // Window ended: submit current candidate to Top-K
        submit_window_candidate();
        window_candidate_.reset();
        window_start_ = now_steady;
    }

    // Update candidate within current window: higher confidence replaces
    if (!window_candidate_.has_value() || confidence > window_candidate_->confidence) {
        WindowCandidate wc;
        wc.rgb_data = rgb_frame;  // Copy RGB data
        wc.width = width;
        wc.height = height;
        wc.confidence = confidence;
        wc.timestamp = std::chrono::system_clock::now();
        window_candidate_ = std::move(wc);
    }
}

void AiPipelineHandler::submit_window_candidate() {
    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    if (!window_candidate_.has_value()) return;

    auto& wc = window_candidate_.value();

    // JPEG encode the candidate
    std::vector<uint8_t> jpeg_data;
    int result = stbi_write_jpg_to_func(stbi_write_callback, &jpeg_data,
                                         wc.width, wc.height, 3, wc.rgb_data.data(), 85);
    if (result == 0 || jpeg_data.empty()) {
        ai_log->warn("JPEG encoding failed for window candidate");
        return;
    }

    // Generate filename from timestamp
    std::string ts = format_timestamp(wc.timestamp);
    int seq = static_cast<int>(snapshot_heap_.size()) + 1;
    std::ostringstream fname;
    fname << ts << "_" << std::setfill('0') << std::setw(3) << seq << ".jpg";

    SnapshotEntry entry;
    entry.filename = fname.str();
    entry.jpeg_data = std::move(jpeg_data);
    entry.confidence = wc.confidence;
    entry.timestamp = wc.timestamp;

    try_submit_to_topk(snapshot_heap_, config_.max_snapshots_per_event, std::move(entry));
}

void AiPipelineHandler::update_detections_summary(const std::vector<Detection>& detections) {
    for (const auto& det : detections) {
        const char* name = coco_class_name(det.class_id);
        auto& summary = detections_summary_[name];
        summary.count++;
        summary.max_confidence = std::max(summary.max_confidence, det.confidence);
    }
}

void AiPipelineHandler::check_event_timeout() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_detection_time_).count();
    if (elapsed >= config_.event_timeout_sec) {
        close_event();
    }
}

void AiPipelineHandler::flush_cache() {
    auto ai_log = spdlog::get("ai");
    if (!ai_log) ai_log = spdlog::default_logger();

    try {
        std::string event_dir = config_.snapshot_dir + event_id_;

        // Create event directory if first flush
        if (!event_dir_created_) {
            std::filesystem::create_directories(event_dir);
            event_dir_created_ = true;
        }

        // Write all cached JPEG files to disk
        for (const auto& frame : cached_frames_) {
            std::string filepath = event_dir + "/" + frame.filename;
            std::ofstream ofs(filepath, std::ios::binary);
            if (ofs) {
                ofs.write(reinterpret_cast<const char*>(frame.jpeg_data.data()),
                          static_cast<std::streamsize>(frame.jpeg_data.size()));
            } else {
                ai_log->warn("Flush: failed to write snapshot: {}", filepath);
            }
        }
    } catch (const std::exception& e) {
        ai_log->warn("Error during intermediate flush: {}", e.what());
    }

    // Clear cache, event continues
    cached_frames_.clear();
    cached_bytes_ = 0;
}
