// bitrate_adapter.h
// Adaptive bitrate controller for H.264 encoder and kvssink.
// Adjusts encoder bitrate based on KVS health signals and stream mode,
// with step-based ramp-up/ramp-down and periodic evaluation.
#pragma once

#include "stream_mode_controller.h"

#include <gst/gst.h>
#include <memory>

// Bitrate configuration (POD, all values in kbps unless noted)
struct BitrateConfig {
    int min_kbps = 1000;
    int max_kbps = 4000;
    int step_kbps = 500;
    int default_kbps = 2500;
    int eval_interval_sec = 5;
    int rampup_interval_sec = 30;
};

// Pure function: compute next bitrate based on current state.
// - kvs_status UNHEALTHY: decrease by step (clamped to min)
// - kvs_status HEALTHY + rampup_eligible: increase by step (clamped to max)
// - otherwise: no change
int compute_next_bitrate(int current_kbps, BranchStatus kvs_status,
                         bool rampup_eligible, const BitrateConfig& config);

class BitrateAdapter {
public:
    // pipeline: not owned, must outlive the adapter (or be replaced via set_pipeline).
    explicit BitrateAdapter(GstElement* pipeline,
                            const BitrateConfig& config = BitrateConfig{});
    ~BitrateAdapter();

    // No copy
    BitrateAdapter(const BitrateAdapter&) = delete;
    BitrateAdapter& operator=(const BitrateAdapter&) = delete;

    // Stream mode change notification (called from StreamModeController callback)
    void on_mode_changed(StreamMode old_mode, StreamMode new_mode);

    // KVS branch health reporting (for bitrate up/down decisions)
    void report_kvs_health(BranchStatus status);

    // Query current target bitrate (thread-safe)
    int current_bitrate_kbps() const;

    // Start/stop (manage internal evaluation timer)
    void start();
    void stop();

    // Update pipeline pointer after rebuild, re-apply current bitrate
    void set_pipeline(GstElement* new_pipeline);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
