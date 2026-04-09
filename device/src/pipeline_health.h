// pipeline_health.h
// Pipeline health monitor with three-layer detection and two-level recovery.
#pragma once
#include <gst/gst.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

// Health state of the pipeline
enum class HealthState {
    HEALTHY,     // Pipeline running normally, buffers flowing
    DEGRADED,    // Watchdog timeout, no buffers but no bus error
    ERROR,       // Bus ERROR or heartbeat detected abnormal state
    RECOVERING,  // Recovery in progress (state-reset or full-rebuild)
    FATAL        // Max retries exceeded, giving up
};

// Return human-readable name for HealthState
const char* health_state_name(HealthState s);

// Configuration for PipelineHealthMonitor (POD, all durations in milliseconds)
struct HealthConfig {
    int watchdog_timeout_ms   = 5000;  // Buffer probe watchdog timeout
    int heartbeat_interval_ms = 2000;  // Heartbeat poll interval
    int initial_backoff_ms    = 1000;  // Initial retry backoff
    int max_retries           = 3;     // Max consecutive recovery failures before FATAL
};

// Recovery statistics
struct HealthStats {
    uint32_t total_recoveries = 0;
    std::chrono::steady_clock::time_point last_recovery_time{};
    std::chrono::steady_clock::time_point healthy_since{};
};

// Callback types
using HealthCallback = std::function<void(HealthState old_state, HealthState new_state)>;
using RebuildCallback = std::function<GstElement*()>;

class PipelineHealthMonitor {
public:
    // Construct monitor for the given pipeline.
    // Does NOT take ownership of the pipeline pointer.
    // The pipeline must outlive the monitor (or be replaced via rebuild).
    explicit PipelineHealthMonitor(GstElement* pipeline,
                                   const HealthConfig& config = HealthConfig{});

    ~PipelineHealthMonitor();

    // No copy
    PipelineHealthMonitor(const PipelineHealthMonitor&) = delete;
    PipelineHealthMonitor& operator=(const PipelineHealthMonitor&) = delete;

    // Start monitoring: install buffer probe, bus watch, timers.
    // Must be called after pipeline is in PLAYING state.
    // source_element_name: name of the video source element for probe installation
    //                      (e.g. "src"). If empty, probe is not installed.
    void start(const std::string& source_element_name = "src");

    // Stop monitoring: remove probe, timers, bus watch.
    void stop();

    // Current health state (thread-safe)
    HealthState state() const;

    // Recovery statistics (thread-safe)
    HealthStats stats() const;

    // Register health state change callback.
    // Called outside mutex to avoid deadlock.
    void set_health_callback(HealthCallback cb);

    // Register rebuild callback for full-rebuild recovery.
    // Must return a new GstElement* pipeline in PLAYING state, or nullptr on failure.
    void set_rebuild_callback(RebuildCallback cb);

    // Update the monitored pipeline pointer (after rebuild).
    // Also re-installs buffer probe on the new pipeline.
    void set_pipeline(GstElement* new_pipeline,
                      const std::string& source_element_name = "src");

private:
    // State transition (must hold mutex_)
    // Returns true if transition occurred
    bool transition_to(HealthState new_state);

    // Buffer probe callback (static, minimal work)
    static GstPadProbeReturn buffer_probe_cb(GstPad* pad,
                                              GstPadProbeInfo* info,
                                              gpointer user_data);

    // Bus message handler (static)
    static gboolean bus_watch_cb(GstBus* bus, GstMessage* msg, gpointer user_data);

    // Watchdog timer callback (static, via g_timeout_add)
    static gboolean watchdog_timer_cb(gpointer user_data);

    // Heartbeat timer callback (static, via g_timeout_add)
    static gboolean heartbeat_timer_cb(gpointer user_data);

    // Recovery logic
    void attempt_recovery();
    bool try_state_reset();
    bool try_full_rebuild();

    // Install buffer probe on source element's src pad
    void install_probe(const std::string& source_element_name);

    // Remove buffer probe if installed
    void remove_probe();

    // Configuration
    HealthConfig config_;

    // Mutable state (protected by mutex_)
    mutable std::mutex mutex_;
    HealthState state_ = HealthState::HEALTHY;
    HealthStats stats_;
    int consecutive_failures_ = 0;
    int current_backoff_ms_ = 0;
    std::chrono::steady_clock::time_point last_buffer_time_;

    // Pipeline pointer (not owned)
    GstElement* pipeline_ = nullptr;

    // Buffer probe tracking
    GstPad* probe_pad_ = nullptr;
    gulong probe_id_ = 0;

    // Bus watch
    guint bus_watch_id_ = 0;

    // Timer IDs (g_timeout_add)
    guint watchdog_timer_id_ = 0;
    guint heartbeat_timer_id_ = 0;
    guint retry_timer_id_ = 0;

    // Callbacks (set once, read from timer/bus callbacks)
    HealthCallback health_cb_;
    RebuildCallback rebuild_cb_;

    // Source element name for re-installing probe after rebuild
    std::string source_element_name_;

    // Flag to prevent re-entrant recovery
    bool recovery_in_progress_ = false;
};
