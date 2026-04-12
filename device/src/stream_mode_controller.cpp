// stream_mode_controller.cpp
// StreamModeController implementation - mode state machine + branch flow control.
#include "stream_mode_controller.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <mutex>

// ---------------------------------------------------------------------------
// Pure functions (no side effects, exposed for PBT)
// ---------------------------------------------------------------------------

StreamMode compute_target_mode(BranchStatus kvs, BranchStatus webrtc) {
    if (kvs == BranchStatus::HEALTHY && webrtc == BranchStatus::HEALTHY) {
        return StreamMode::FULL;
    }
    if (kvs == BranchStatus::HEALTHY && webrtc == BranchStatus::UNHEALTHY) {
        return StreamMode::KVS_ONLY;
    }
    if (kvs == BranchStatus::UNHEALTHY && webrtc == BranchStatus::HEALTHY) {
        return StreamMode::WEBRTC_ONLY;
    }
    // Both UNHEALTHY
    return StreamMode::DEGRADED;
}

BranchQueueParams compute_queue_params(StreamMode mode) {
    switch (mode) {
        case StreamMode::FULL:
            return {{1, 0}, {1, 2}};
        case StreamMode::KVS_ONLY:
            return {{1, 0}, {0, 2}};
        case StreamMode::WEBRTC_ONLY:
            return {{0, 2}, {1, 2}};
        case StreamMode::DEGRADED:
            return {{1, 0}, {0, 2}};
    }
    // Fallback (should not reach)
    return {{1, 0}, {1, 2}};
}

const char* stream_mode_name(StreamMode mode) {
    switch (mode) {
        case StreamMode::FULL:         return "FULL";
        case StreamMode::KVS_ONLY:     return "KVS_ONLY";
        case StreamMode::WEBRTC_ONLY:  return "WEBRTC_ONLY";
        case StreamMode::DEGRADED:     return "DEGRADED";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Impl structure
// ---------------------------------------------------------------------------

struct StreamModeController::Impl {
    int debounce_ms_ = 3000;
    StreamMode current_mode_ = StreamMode::FULL;
    BranchStatus kvs_confirmed_ = BranchStatus::HEALTHY;
    BranchStatus webrtc_confirmed_ = BranchStatus::HEALTHY;

    // Pending status + timestamp for debounce
    BranchStatus pending_kvs_ = BranchStatus::HEALTHY;
    BranchStatus pending_webrtc_ = BranchStatus::HEALTHY;
    std::chrono::steady_clock::time_point pending_kvs_time_{};
    std::chrono::steady_clock::time_point pending_webrtc_time_{};

    GstElement* pipeline_ = nullptr;  // Not owned
    mutable std::mutex mutex_;
    ModeChangeCallback mode_change_cb_;
    guint debounce_timer_id_ = 0;

    // Apply queue parameters to pipeline elements for the given mode.
    // Must NOT hold mutex_ when calling (acquires no lock internally).
    void apply_mode(StreamMode new_mode, GstElement* pipeline);

    // Debounce evaluation: check if pending statuses have been stable >= 3s.
    // Called from GLib timer callback.
    // Returns true if timer should continue, false to remove.
    static gboolean evaluate_cb(gpointer user_data);
};

// ---------------------------------------------------------------------------
// apply_mode — set queue properties via gst_bin_get_by_name
// ---------------------------------------------------------------------------

void StreamModeController::Impl::apply_mode(StreamMode new_mode, GstElement* pipeline) {
    if (!pipeline) return;

    auto logger = spdlog::get("stream");
    auto params = compute_queue_params(new_mode);

    // Apply q-kvs parameters
    GstElement* q_kvs = gst_bin_get_by_name(GST_BIN(pipeline), "q-kvs");
    if (q_kvs) {
        g_object_set(q_kvs,
                     "max-size-buffers", static_cast<guint>(params.kvs.max_size_buffers),
                     "leaky", params.kvs.leaky,
                     nullptr);
        gst_object_unref(q_kvs);
    } else {
        if (logger) {
            logger->warn("q-kvs element not found in pipeline, skipping queue config");
        }
    }

    // Apply q-web parameters
    GstElement* q_web = gst_bin_get_by_name(GST_BIN(pipeline), "q-web");
    if (q_web) {
        g_object_set(q_web,
                     "max-size-buffers", static_cast<guint>(params.web.max_size_buffers),
                     "leaky", params.web.leaky,
                     nullptr);
        gst_object_unref(q_web);
    } else {
        if (logger) {
            logger->warn("q-web element not found in pipeline, skipping queue config");
        }
    }
}

// ---------------------------------------------------------------------------
// evaluate_cb — debounce timer callback
// ---------------------------------------------------------------------------

gboolean StreamModeController::Impl::evaluate_cb(gpointer user_data) {
    auto* impl = static_cast<Impl*>(user_data);
    auto now = std::chrono::steady_clock::now();

    StreamMode old_mode{};
    StreamMode new_mode{};
    bool mode_changed = false;
    ModeChangeCallback cb_copy;
    std::string reason;
    GstElement* pipeline = nullptr;

    {
        std::lock_guard<std::mutex> lock(impl->mutex_);

        bool kvs_changed = (impl->pending_kvs_ != impl->kvs_confirmed_);
        bool webrtc_changed = (impl->pending_webrtc_ != impl->webrtc_confirmed_);

        // Check if pending KVS status has been stable >= debounce period
        if (kvs_changed) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - impl->pending_kvs_time_).count();
            if (elapsed >= impl->debounce_ms_) {
                auto logger = spdlog::get("stream");
                if (logger) {
                    logger->warn("KVS branch status confirmed: {}",
                                 impl->pending_kvs_ == BranchStatus::HEALTHY
                                     ? "HEALTHY" : "UNHEALTHY");
                }
                impl->kvs_confirmed_ = impl->pending_kvs_;
            }
        }

        // Check if pending WebRTC status has been stable >= debounce period
        if (webrtc_changed) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - impl->pending_webrtc_time_).count();
            if (elapsed >= impl->debounce_ms_) {
                auto logger = spdlog::get("stream");
                if (logger) {
                    logger->warn("WebRTC branch status confirmed: {}",
                                 impl->pending_webrtc_ == BranchStatus::HEALTHY
                                     ? "HEALTHY" : "UNHEALTHY");
                }
                impl->webrtc_confirmed_ = impl->pending_webrtc_;
            }
        }

        // Compute target mode from confirmed statuses
        StreamMode target = compute_target_mode(impl->kvs_confirmed_,
                                                 impl->webrtc_confirmed_);
        if (target != impl->current_mode_) {
            old_mode = impl->current_mode_;
            new_mode = target;
            mode_changed = true;
            impl->current_mode_ = target;
            pipeline = impl->pipeline_;
            cb_copy = impl->mode_change_cb_;

            reason = std::string("KVS=") +
                     (impl->kvs_confirmed_ == BranchStatus::HEALTHY
                          ? "HEALTHY" : "UNHEALTHY") +
                     ", WebRTC=" +
                     (impl->webrtc_confirmed_ == BranchStatus::HEALTHY
                          ? "HEALTHY" : "UNHEALTHY");
        }
    }

    // Apply mode and invoke callback outside mutex
    if (mode_changed) {
        impl->apply_mode(new_mode, pipeline);

        auto logger = spdlog::get("stream");
        if (logger) {
            logger->info("Mode switched: {} -> {} ({})",
                         stream_mode_name(old_mode),
                         stream_mode_name(new_mode),
                         reason);
        }

        if (cb_copy) {
            cb_copy(old_mode, new_mode, reason);
        }
    }

    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

StreamModeController::StreamModeController(GstElement* pipeline, int debounce_ms)
    : impl_(std::make_unique<Impl>()) {
    impl_->pipeline_ = pipeline;
    impl_->debounce_ms_ = debounce_ms;
}

StreamModeController::~StreamModeController() {
    stop();
}

// ---------------------------------------------------------------------------
// report_kvs_status / report_webrtc_status
// ---------------------------------------------------------------------------

void StreamModeController::report_kvs_status(BranchStatus status) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (status != impl_->pending_kvs_) {
        impl_->pending_kvs_ = status;
        impl_->pending_kvs_time_ = std::chrono::steady_clock::now();
    }
    // Ensure debounce timer is running
    if (impl_->debounce_timer_id_ == 0) {
        impl_->debounce_timer_id_ = g_timeout_add(
            1000, Impl::evaluate_cb, impl_.get());
    }
}

void StreamModeController::report_webrtc_status(BranchStatus status) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (status != impl_->pending_webrtc_) {
        impl_->pending_webrtc_ = status;
        impl_->pending_webrtc_time_ = std::chrono::steady_clock::now();
    }
    // Ensure debounce timer is running
    if (impl_->debounce_timer_id_ == 0) {
        impl_->debounce_timer_id_ = g_timeout_add(
            1000, Impl::evaluate_cb, impl_.get());
    }
}

// ---------------------------------------------------------------------------
// set_mode_change_callback / current_mode
// ---------------------------------------------------------------------------

void StreamModeController::set_mode_change_callback(ModeChangeCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->mode_change_cb_ = std::move(cb);
}

StreamMode StreamModeController::current_mode() const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->current_mode_;
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

void StreamModeController::start() {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    // Start periodic debounce evaluation timer (1s interval)
    if (impl_->debounce_timer_id_ == 0) {
        impl_->debounce_timer_id_ = g_timeout_add(
            1000, Impl::evaluate_cb, impl_.get());
    }

    auto logger = spdlog::get("stream");
    if (logger) {
        logger->info("StreamModeController started (debounce={}ms)",
                     impl_->debounce_ms_);
    }
}

void StreamModeController::stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (impl_->debounce_timer_id_ != 0) {
        g_source_remove(impl_->debounce_timer_id_);
        impl_->debounce_timer_id_ = 0;
    }
}

// ---------------------------------------------------------------------------
// set_pipeline — update pipeline pointer after rebuild
// ---------------------------------------------------------------------------

void StreamModeController::set_pipeline(GstElement* new_pipeline) {
    StreamMode mode;
    GstElement* pipeline;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->pipeline_ = new_pipeline;
        mode = impl_->current_mode_;
        pipeline = impl_->pipeline_;
    }
    // Re-apply current mode queue params to new pipeline
    impl_->apply_mode(mode, pipeline);
}
