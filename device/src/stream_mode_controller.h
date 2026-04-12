// stream_mode_controller.h
// Stream mode state machine + branch data flow control.
// Switches between FULL / KVS_ONLY / WEBRTC_ONLY / DEGRADED based on
// branch health signals, with 3-second debounce to avoid flapping.
#pragma once

#include <gst/gst.h>
#include <functional>
#include <memory>
#include <string>

// Stream mode enumeration
enum class StreamMode {
    FULL,           // KVS + WebRTC both healthy
    KVS_ONLY,       // Only KVS, WebRTC branch drops data
    WEBRTC_ONLY,    // Only WebRTC, KVS branch drops data
    DEGRADED        // Both unhealthy, lowest bitrate KVS only
};

// Branch health status
enum class BranchStatus { HEALTHY, UNHEALTHY };

// Mode change callback: old mode, new mode, reason string
using ModeChangeCallback = std::function<void(StreamMode old_mode,
                                               StreamMode new_mode,
                                               const std::string& reason)>;

// Queue parameters for a single queue element (POD, for PBT)
struct QueueParams {
    int max_size_buffers;
    int leaky;
};

// Queue parameters for both branches (POD, for PBT)
struct BranchQueueParams {
    QueueParams kvs;
    QueueParams web;
};

// Pure function: compute target mode from branch statuses.
// Deterministic mapping of 4 (BranchStatus, BranchStatus) combinations.
StreamMode compute_target_mode(BranchStatus kvs, BranchStatus webrtc);

// Pure function: compute queue parameters for a given stream mode.
// Returns (q-kvs, q-web) parameter pair per the predefined mapping table.
BranchQueueParams compute_queue_params(StreamMode mode);

// Return human-readable English name for StreamMode.
const char* stream_mode_name(StreamMode mode);

class StreamModeController {
public:
    // pipeline: not owned, must outlive the controller (or be replaced via set_pipeline).
    explicit StreamModeController(GstElement* pipeline);
    ~StreamModeController();

    // No copy
    StreamModeController(const StreamModeController&) = delete;
    StreamModeController& operator=(const StreamModeController&) = delete;

    // Branch health reporting (called from external timers or callbacks)
    void report_kvs_status(BranchStatus status);
    void report_webrtc_status(BranchStatus status);

    // Register mode change callback
    void set_mode_change_callback(ModeChangeCallback cb);

    // Query current mode (thread-safe)
    StreamMode current_mode() const;

    // Start/stop (manage internal debounce timers)
    void start();
    void stop();

    // Update pipeline pointer after rebuild, re-apply current mode queue params
    void set_pipeline(GstElement* new_pipeline);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
