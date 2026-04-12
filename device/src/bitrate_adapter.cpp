// bitrate_adapter.cpp
// Adaptive bitrate controller implementation.
// Uses g_timeout_add for periodic evaluation, adjusts encoder bitrate
// and kvssink avg-bandwidth-bps via g_object_set.
#include "bitrate_adapter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <mutex>

// ---------------------------------------------------------------------------
// Pure function
// ---------------------------------------------------------------------------

int compute_next_bitrate(int current_kbps, BranchStatus kvs_status,
                         bool rampup_eligible, const BitrateConfig& config) {
    if (kvs_status == BranchStatus::UNHEALTHY) {
        return std::max(current_kbps - config.step_kbps, config.min_kbps);
    }
    if (kvs_status == BranchStatus::HEALTHY && rampup_eligible) {
        return std::min(current_kbps + config.step_kbps, config.max_kbps);
    }
    return current_kbps;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct BitrateAdapter::Impl {
    int current_bitrate_kbps_;
    StreamMode current_mode_ = StreamMode::FULL;
    BranchStatus kvs_status_ = BranchStatus::HEALTHY;
    std::chrono::steady_clock::time_point last_kvs_healthy_time_;
    GstElement* pipeline_ = nullptr;
    BitrateConfig config_;
    guint eval_timer_id_ = 0;
    mutable std::mutex mutex_;

    explicit Impl(GstElement* pipeline, const BitrateConfig& config)
        : current_bitrate_kbps_(config.default_kbps),
          last_kvs_healthy_time_(std::chrono::steady_clock::now()),
          pipeline_(pipeline),
          config_(config) {}

    // Apply bitrate to encoder and optionally to kvssink
    void apply_bitrate(int new_kbps) {
        auto logger = spdlog::get("bitrate");

        if (!pipeline_) {
            if (logger) logger->warn("apply_bitrate: pipeline is null, skipping");
            return;
        }

        // Set encoder bitrate (x264enc, unit: kbps)
        GstElement* encoder = gst_bin_get_by_name(GST_BIN(pipeline_), "encoder");
        if (encoder) {
            g_object_set(encoder, "bitrate", new_kbps, nullptr);
            gst_object_unref(encoder);
        } else {
            if (logger) logger->warn("apply_bitrate: encoder element not found, skipping");
        }

        // Try to set kvssink avg-bandwidth-bps (unit: bps = kbps * 1000)
        GstElement* kvs_sink = gst_bin_get_by_name(GST_BIN(pipeline_), "kvs-sink");
        if (kvs_sink) {
            GObjectClass* klass = G_OBJECT_GET_CLASS(kvs_sink);
            if (g_object_class_find_property(klass, "avg-bandwidth-bps")) {
                g_object_set(kvs_sink, "avg-bandwidth-bps",
                             static_cast<guint>(new_kbps * 1000), nullptr);
            } else {
                if (logger)
                    logger->info("kvs-sink does not have avg-bandwidth-bps property, skipping");
            }
            gst_object_unref(kvs_sink);
        } else {
            if (logger) logger->info("kvs-sink element not found, skipping bandwidth sync");
        }
    }

    // Periodic evaluation callback (called from GLib main loop)
    static gboolean eval_timer_cb(gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        self->evaluate();
        return G_SOURCE_CONTINUE;
    }

    void evaluate() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto logger = spdlog::get("bitrate");

        // In DEGRADED mode, bitrate is forced to min by on_mode_changed;
        // periodic evaluation does not override that.
        if (current_mode_ == StreamMode::DEGRADED) {
            return;
        }

        // Check rampup eligibility: KVS HEALTHY for >= rampup_interval_sec
        bool rampup_eligible = false;
        if (kvs_status_ == BranchStatus::HEALTHY) {
            auto elapsed = std::chrono::steady_clock::now() - last_kvs_healthy_time_;
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            rampup_eligible = (secs >= config_.rampup_interval_sec);
        }

        int new_bitrate = compute_next_bitrate(
            current_bitrate_kbps_, kvs_status_, rampup_eligible, config_);

        if (new_bitrate != current_bitrate_kbps_) {
            int old_bitrate = current_bitrate_kbps_;
            current_bitrate_kbps_ = new_bitrate;
            apply_bitrate(new_bitrate);
            if (logger)
                logger->info("bitrate adjusted: {} -> {} kbps", old_bitrate, new_bitrate);
        }
    }
};

// ---------------------------------------------------------------------------
// BitrateAdapter public API
// ---------------------------------------------------------------------------

BitrateAdapter::BitrateAdapter(GstElement* pipeline, const BitrateConfig& config)
    : impl_(std::make_unique<Impl>(pipeline, config)) {}

BitrateAdapter::~BitrateAdapter() {
    stop();
}

void BitrateAdapter::on_mode_changed(StreamMode /*old_mode*/, StreamMode new_mode) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    auto logger = spdlog::get("bitrate");

    impl_->current_mode_ = new_mode;

    if (new_mode == StreamMode::DEGRADED) {
        int old_bitrate = impl_->current_bitrate_kbps_;
        impl_->current_bitrate_kbps_ = impl_->config_.min_kbps;
        impl_->apply_bitrate(impl_->config_.min_kbps);
        if (logger)
            logger->info("DEGRADED mode: bitrate forced {} -> {} kbps",
                         old_bitrate, impl_->config_.min_kbps);
    }
}

void BitrateAdapter::report_kvs_health(BranchStatus status) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    auto logger = spdlog::get("bitrate");

    impl_->kvs_status_ = status;

    if (status == BranchStatus::HEALTHY) {
        // Record healthy timestamp for rampup eligibility tracking
        impl_->last_kvs_healthy_time_ = std::chrono::steady_clock::now();
    } else {
        // UNHEALTHY: trigger immediate step-down
        if (impl_->current_mode_ != StreamMode::DEGRADED) {
            int old_bitrate = impl_->current_bitrate_kbps_;
            int new_bitrate = std::max(
                impl_->current_bitrate_kbps_ - impl_->config_.step_kbps,
                impl_->config_.min_kbps);
            if (new_bitrate != impl_->current_bitrate_kbps_) {
                impl_->current_bitrate_kbps_ = new_bitrate;
                impl_->apply_bitrate(new_bitrate);
                if (logger)
                    logger->info("KVS UNHEALTHY: bitrate {} -> {} kbps",
                                 old_bitrate, new_bitrate);
            }
        }
    }
}

int BitrateAdapter::current_bitrate_kbps() const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->current_bitrate_kbps_;
}

void BitrateAdapter::start() {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (impl_->eval_timer_id_ != 0) {
        return; // already running
    }
    impl_->eval_timer_id_ = g_timeout_add(
        static_cast<guint>(impl_->config_.eval_interval_sec * 1000),
        Impl::eval_timer_cb, impl_.get());

    auto logger = spdlog::get("bitrate");
    if (logger)
        logger->info("BitrateAdapter started, eval interval={}s, default={}kbps",
                     impl_->config_.eval_interval_sec, impl_->current_bitrate_kbps_);
}

void BitrateAdapter::stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (impl_->eval_timer_id_ != 0) {
        g_source_remove(impl_->eval_timer_id_);
        impl_->eval_timer_id_ = 0;
    }
    auto logger = spdlog::get("bitrate");
    if (logger) logger->info("BitrateAdapter stopped");
}

void BitrateAdapter::set_pipeline(GstElement* new_pipeline) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->pipeline_ = new_pipeline;
    impl_->apply_bitrate(impl_->current_bitrate_kbps_);

    auto logger = spdlog::get("bitrate");
    if (logger)
        logger->info("pipeline updated, re-applied bitrate={}kbps",
                     impl_->current_bitrate_kbps_);
}
