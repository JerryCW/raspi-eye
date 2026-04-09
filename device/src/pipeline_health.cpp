// pipeline_health.cpp
// PipelineHealthMonitor implementation - three-layer detection, two-level recovery.
#include "pipeline_health.h"
#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// health_state_name
// ---------------------------------------------------------------------------

const char* health_state_name(HealthState s) {
    switch (s) {
        case HealthState::HEALTHY:    return "HEALTHY";
        case HealthState::DEGRADED:   return "DEGRADED";
        case HealthState::ERROR:      return "ERROR";
        case HealthState::RECOVERING: return "RECOVERING";
        case HealthState::FATAL:      return "FATAL";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

PipelineHealthMonitor::PipelineHealthMonitor(GstElement* pipeline,
                                             const HealthConfig& config)
    : config_(config)
    , current_backoff_ms_(config.initial_backoff_ms)
    , pipeline_(pipeline)
{
    auto now = std::chrono::steady_clock::now();
    last_buffer_time_ = now;
    stats_.healthy_since = now;
}

PipelineHealthMonitor::~PipelineHealthMonitor() {
    stop();
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

void PipelineHealthMonitor::start(const std::string& source_element_name) {
    source_element_name_ = source_element_name;

    // Install buffer probe on source element
    install_probe(source_element_name);

    // Register bus watch
    GstBus* bus = gst_element_get_bus(pipeline_);
    if (bus) {
        bus_watch_id_ = gst_bus_add_watch(bus, bus_watch_cb, this);
        gst_object_unref(bus);
    }

    // Register watchdog timer
    watchdog_timer_id_ = g_timeout_add(
        static_cast<guint>(config_.watchdog_timeout_ms), watchdog_timer_cb, this);

    // Register heartbeat timer
    heartbeat_timer_id_ = g_timeout_add(
        static_cast<guint>(config_.heartbeat_interval_ms), heartbeat_timer_cb, this);

    auto logger = spdlog::get("pipeline");
    if (logger) {
        logger->info("Health monitor started (watchdog={}ms, heartbeat={}ms)",
                     config_.watchdog_timeout_ms, config_.heartbeat_interval_ms);
    }
}

void PipelineHealthMonitor::stop() {
    // Remove watchdog timer
    if (watchdog_timer_id_ != 0) {
        g_source_remove(watchdog_timer_id_);
        watchdog_timer_id_ = 0;
    }

    // Remove heartbeat timer
    if (heartbeat_timer_id_ != 0) {
        g_source_remove(heartbeat_timer_id_);
        heartbeat_timer_id_ = 0;
    }

    // Remove retry timer
    if (retry_timer_id_ != 0) {
        g_source_remove(retry_timer_id_);
        retry_timer_id_ = 0;
    }

    // Remove bus watch
    if (bus_watch_id_ != 0) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }

    // Remove buffer probe
    remove_probe();
}

// ---------------------------------------------------------------------------
// state / stats / set callbacks
// ---------------------------------------------------------------------------

HealthState PipelineHealthMonitor::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

HealthStats PipelineHealthMonitor::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void PipelineHealthMonitor::set_health_callback(HealthCallback cb) {
    health_cb_ = std::move(cb);
}

void PipelineHealthMonitor::set_rebuild_callback(RebuildCallback cb) {
    rebuild_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// transition_to — validates legal transitions per state machine table
// ---------------------------------------------------------------------------

bool PipelineHealthMonitor::transition_to(HealthState new_state) {
    // Must be called with mutex_ held.
    // Validate legal transitions:
    //   HEALTHY   -> DEGRADED, ERROR
    //   DEGRADED  -> HEALTHY, ERROR
    //   ERROR     -> RECOVERING
    //   RECOVERING -> HEALTHY, ERROR, FATAL
    //   FATAL     -> (none, terminal state)
    bool legal = false;
    switch (state_) {
        case HealthState::HEALTHY:
            legal = (new_state == HealthState::DEGRADED ||
                     new_state == HealthState::ERROR);
            break;
        case HealthState::DEGRADED:
            legal = (new_state == HealthState::HEALTHY ||
                     new_state == HealthState::ERROR);
            break;
        case HealthState::ERROR:
            legal = (new_state == HealthState::RECOVERING);
            break;
        case HealthState::RECOVERING:
            legal = (new_state == HealthState::HEALTHY ||
                     new_state == HealthState::ERROR ||
                     new_state == HealthState::FATAL);
            break;
        case HealthState::FATAL:
            legal = false;  // Terminal state
            break;
    }

    if (!legal) {
        auto logger = spdlog::get("pipeline");
        if (logger) {
            logger->warn("Invalid transition from {} to {}",
                         health_state_name(state_),
                         health_state_name(new_state));
        }
        return false;
    }

    state_ = new_state;
    return true;
}

// ---------------------------------------------------------------------------
// Buffer probe callback (minimal work, high-performance path)
// ---------------------------------------------------------------------------

GstPadProbeReturn PipelineHealthMonitor::buffer_probe_cb(
    GstPad* /*pad*/, GstPadProbeInfo* /*info*/, gpointer user_data) {
    auto* self = static_cast<PipelineHealthMonitor*>(user_data);
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->last_buffer_time_ = std::chrono::steady_clock::now();
    }
    return GST_PAD_PROBE_OK;
}

// ---------------------------------------------------------------------------
// Bus watch callback
// ---------------------------------------------------------------------------

gboolean PipelineHealthMonitor::bus_watch_cb(
    GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
    auto* self = static_cast<PipelineHealthMonitor*>(user_data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);

            auto logger = spdlog::get("pipeline");
            if (logger) {
                logger->error("Bus ERROR from {}: {}",
                              GST_OBJECT_NAME(msg->src),
                              err ? err->message : "unknown");
                if (dbg) logger->debug("Debug info: {}", dbg);
            }

            if (err) g_error_free(err);
            if (dbg) g_free(dbg);

            self->attempt_recovery();
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_warning(msg, &err, &dbg);

            auto logger = spdlog::get("pipeline");
            if (logger) {
                logger->warn("Bus WARNING from {}: {}",
                             GST_OBJECT_NAME(msg->src),
                             err ? err->message : "unknown");
            }

            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
            break;
        }
        case GST_MESSAGE_EOS: {
            auto logger = spdlog::get("pipeline");
            if (logger) logger->info("End of stream");
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Watchdog timer callback
// ---------------------------------------------------------------------------

gboolean PipelineHealthMonitor::watchdog_timer_cb(gpointer user_data) {
    auto* self = static_cast<PipelineHealthMonitor*>(user_data);
    HealthCallback cb_copy;
    HealthState old_state{};
    HealthState new_state{};
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - self->last_buffer_time_).count();

        if (elapsed_ms >= self->config_.watchdog_timeout_ms &&
            self->state_ == HealthState::HEALTHY) {
            old_state = self->state_;
            changed = self->transition_to(HealthState::DEGRADED);
            new_state = self->state_;
            cb_copy = self->health_cb_;

            auto logger = spdlog::get("pipeline");
            if (logger) {
                logger->warn("Watchdog timeout: no buffer for {}ms (threshold={}ms)",
                             elapsed_ms, self->config_.watchdog_timeout_ms);
            }
        }
    }

    // Callback outside mutex
    if (changed && cb_copy) {
        cb_copy(old_state, new_state);
    }
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Heartbeat timer callback
// ---------------------------------------------------------------------------

gboolean PipelineHealthMonitor::heartbeat_timer_cb(gpointer user_data) {
    auto* self = static_cast<PipelineHealthMonitor*>(user_data);
    HealthCallback cb_copy;
    HealthState old_state{};
    HealthState new_state{};
    bool changed = false;

    GstState gst_state = GST_STATE_NULL;
    gst_element_get_state(self->pipeline_, &gst_state, nullptr, 0);

    if (gst_state != GST_STATE_PLAYING) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->state_ == HealthState::HEALTHY ||
            self->state_ == HealthState::DEGRADED) {
            old_state = self->state_;
            changed = self->transition_to(HealthState::ERROR);
            new_state = self->state_;
            cb_copy = self->health_cb_;

            auto logger = spdlog::get("pipeline");
            if (logger) {
                logger->warn("Heartbeat: pipeline state is {} (expected PLAYING)",
                             gst_element_state_get_name(gst_state));
            }
        }
    }

    // Callback outside mutex
    if (changed && cb_copy) {
        cb_copy(old_state, new_state);
    }
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Recovery engine
// ---------------------------------------------------------------------------

void PipelineHealthMonitor::attempt_recovery() {
    HealthCallback cb_copy;
    HealthState old_state{};
    HealthState new_state{};
    bool changed = false;

    // Check re-entrancy and transition to RECOVERING
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recovery_in_progress_) {
            auto logger = spdlog::get("pipeline");
            if (logger) logger->debug("Recovery already in progress, skipping");
            return;
        }
        if (state_ == HealthState::FATAL) return;

        recovery_in_progress_ = true;

        // Transition to ERROR first if not already there
        if (state_ == HealthState::HEALTHY || state_ == HealthState::DEGRADED) {
            old_state = state_;
            changed = transition_to(HealthState::ERROR);
            new_state = state_;
            cb_copy = health_cb_;
        }
    }

    // Notify ERROR transition outside mutex
    if (changed && cb_copy) {
        cb_copy(old_state, new_state);
    }

    // Transition to RECOVERING
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old_state = state_;
        changed = transition_to(HealthState::RECOVERING);
        new_state = state_;
        cb_copy = health_cb_;
    }
    if (changed && cb_copy) {
        cb_copy(old_state, new_state);
    }

    auto logger = spdlog::get("pipeline");

    // Try state reset first
    bool recovered = try_state_reset();
    if (!recovered) {
        if (logger) logger->warn("State reset failed, attempting full rebuild");
        recovered = try_full_rebuild();
    }

    if (recovered) {
        // Success: reset counters, update stats
        if (logger) logger->info("Recovery successful");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            consecutive_failures_ = 0;
            current_backoff_ms_ = config_.initial_backoff_ms;
            stats_.total_recoveries++;
            auto now = std::chrono::steady_clock::now();
            stats_.last_recovery_time = now;
            stats_.healthy_since = now;
            last_buffer_time_ = now;
            old_state = state_;
            changed = transition_to(HealthState::HEALTHY);
            new_state = state_;
            cb_copy = health_cb_;
            recovery_in_progress_ = false;
        }
        if (changed && cb_copy) {
            cb_copy(old_state, new_state);
        }
    } else {
        // Failure: increment counter, check max retries
        int backoff = 0;
        bool is_fatal = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            consecutive_failures_++;

            if (consecutive_failures_ >= config_.max_retries) {
                if (logger) {
                    logger->error("Max retries ({}) exceeded, entering FATAL state",
                                  config_.max_retries);
                }
                old_state = state_;
                changed = transition_to(HealthState::FATAL);
                new_state = state_;
                cb_copy = health_cb_;
                is_fatal = true;
            } else {
                if (logger) {
                    logger->warn("Recovery failed ({}/{}), backoff {}ms",
                                 consecutive_failures_, config_.max_retries,
                                 current_backoff_ms_);
                }
                old_state = state_;
                changed = transition_to(HealthState::ERROR);
                new_state = state_;
                cb_copy = health_cb_;

                backoff = current_backoff_ms_;
                current_backoff_ms_ *= 2;
            }
            recovery_in_progress_ = false;
        }

        // Callback outside mutex
        if (changed && cb_copy) {
            cb_copy(old_state, new_state);
        }

        // Schedule delayed retry if not fatal
        if (!is_fatal && backoff > 0) {
            retry_timer_id_ = g_timeout_add(static_cast<guint>(backoff),
                          [](gpointer data) -> gboolean {
                              auto* monitor = static_cast<PipelineHealthMonitor*>(data);
                              monitor->retry_timer_id_ = 0;
                              monitor->attempt_recovery();
                              return G_SOURCE_REMOVE;
                          }, this);
        }
    }
}

// ---------------------------------------------------------------------------
// try_state_reset — NULL -> wait -> PLAYING
// ---------------------------------------------------------------------------

bool PipelineHealthMonitor::try_state_reset() {
    auto logger = spdlog::get("pipeline");
    if (logger) logger->info("Attempting state reset recovery");

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        if (logger) logger->warn("set_state(NULL) failed");
        return false;
    }

    // Wait for NULL state (up to 1 second)
    GstState actual = GST_STATE_VOID_PENDING;
    gst_element_get_state(pipeline_, &actual, nullptr, GST_SECOND);
    if (actual != GST_STATE_NULL) {
        if (logger) logger->warn("Pipeline did not reach NULL state");
        return false;
    }

    ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        if (logger) logger->warn("set_state(PLAYING) failed after reset");
        return false;
    }

    // Wait for PLAYING state confirmation (up to 1 second)
    actual = GST_STATE_VOID_PENDING;
    ret = gst_element_get_state(pipeline_, &actual, nullptr, GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE || actual != GST_STATE_PLAYING) {
        if (logger) logger->warn("Pipeline did not reach PLAYING state after reset");
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        return false;
    }

    if (logger) logger->info("State reset recovery succeeded");
    return true;
}

// ---------------------------------------------------------------------------
// try_full_rebuild — call rebuild callback
// ---------------------------------------------------------------------------

bool PipelineHealthMonitor::try_full_rebuild() {
    auto logger = spdlog::get("pipeline");

    if (!rebuild_cb_) {
        if (logger) logger->warn("No rebuild callback registered, cannot attempt full rebuild");
        return false;
    }

    if (logger) logger->info("Attempting full rebuild recovery");

    GstElement* new_pipeline = rebuild_cb_();
    if (!new_pipeline) {
        if (logger) logger->error("Full rebuild callback returned nullptr");
        return false;
    }

    set_pipeline(new_pipeline, source_element_name_);
    if (logger) logger->info("Full rebuild recovery succeeded");
    return true;
}

// ---------------------------------------------------------------------------
// install_probe / remove_probe
// ---------------------------------------------------------------------------

void PipelineHealthMonitor::install_probe(const std::string& source_element_name) {
    if (source_element_name.empty()) return;

    auto logger = spdlog::get("pipeline");

    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline_), source_element_name.c_str());
    if (!source) {
        if (logger) {
            logger->warn("Source element '{}' not found, probe not installed",
                         source_element_name);
        }
        return;
    }

    GstPad* pad = gst_element_get_static_pad(source, "src");
    if (!pad) {
        if (logger) {
            logger->warn("Source pad not found on element '{}'", source_element_name);
        }
        gst_object_unref(source);
        return;
    }

    probe_id_ = gst_pad_add_probe(
        pad,
        GST_PAD_PROBE_TYPE_BUFFER,
        buffer_probe_cb,
        this,
        nullptr);
    probe_pad_ = pad;  // Keep reference for later removal

    gst_object_unref(source);

    if (logger) {
        logger->debug("Buffer probe installed on {}/src", source_element_name);
    }
}

void PipelineHealthMonitor::remove_probe() {
    if (probe_pad_ && probe_id_ != 0) {
        gst_pad_remove_probe(probe_pad_, probe_id_);
        probe_id_ = 0;
    }
    if (probe_pad_) {
        gst_object_unref(probe_pad_);
        probe_pad_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// set_pipeline — update pipeline pointer after rebuild
// ---------------------------------------------------------------------------

void PipelineHealthMonitor::set_pipeline(GstElement* new_pipeline,
                                          const std::string& source_element_name) {
    // Remove probe from old pipeline
    remove_probe();

    // Update pipeline pointer
    pipeline_ = new_pipeline;
    source_element_name_ = source_element_name;

    // Install probe on new pipeline
    install_probe(source_element_name);

    // Re-register bus watch on new pipeline
    if (bus_watch_id_ != 0) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }
    GstBus* bus = gst_element_get_bus(pipeline_);
    if (bus) {
        bus_watch_id_ = gst_bus_add_watch(bus, bus_watch_cb, this);
        gst_object_unref(bus);
    }
}
