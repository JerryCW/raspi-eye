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

// Error scope: classifies a GStreamer bus error by fault domain.
// KVS_BRANCH / WEBRTC_BRANCH errors are branch-local and self-heal; only TRUNK
// errors (shared upstream) should trigger whole-pipeline recovery. (Spec 32 需求 1)
enum class ErrorScope { KVS_BRANCH, WEBRTC_BRANCH, TRUNK };

// Pure function: classify a bus error by its source element name.
// KVS branch elements (q-kvs/kvs-parser/avc-caps/kvs-sink) -> KVS_BRANCH;
// WebRTC branch elements (q-web/webrtc-sink) -> WEBRTC_BRANCH;
// everything else (incl. unknown / empty) -> TRUNK (conservative: rather recover than miss).
ErrorScope classify_bus_error(const std::string& src_name);

// Pure function: returns true iff consecutive_non_playing >= threshold && threshold > 0.
// Used by heartbeat debounce to drive active recovery without premature triggering.
bool should_trigger_recovery(int consecutive_non_playing, int threshold);

// Bounded asynchronous teardown helpers (Spec 32 决策 B / 方案 X).
// The blocking set_state(NULL) (kvssink stopStreamSync may block ~130s) is moved
// off the GLib main loop: a worker thread runs it while the caller waits at most
// budget_ms. On timeout the worker is detached and finishes in the background.
//
// (a) Transfers ownership: the worker sets the pipeline to NULL then unrefs it.
//     Used by full_rebuild to destroy the old pipeline. Returns true if the
//     teardown completed within budget_ms, false on timeout (worker detached).
//     teardown_fn is injectable for tests (default: gst set NULL + get_state + unref);
//     when a custom teardown_fn is supplied, the caller owns the unref decision.
bool teardown_pipeline_bounded(GstElement* pipeline, int budget_ms,
                               std::function<void(GstElement*)> teardown_fn = {});

// (b) Does NOT transfer ownership: the worker only sets the pipeline to NULL
//     (no unref). Used by try_state_reset to reuse the same pipeline. Returns
//     true if NULL was reached within budget_ms, false on timeout.
//     set_null_fn is injectable for tests (default: gst set NULL + get_state).
bool set_null_bounded(GstElement* pipeline, int budget_ms,
                      std::function<void(GstElement*)> set_null_fn = {});

// Configuration for PipelineHealthMonitor (POD, all durations in milliseconds)
struct HealthConfig {
    int watchdog_timeout_ms   = 5000;  // Buffer probe watchdog timeout
    int heartbeat_interval_ms = 2000;  // Heartbeat poll interval
    int initial_backoff_ms    = 1000;  // Initial retry backoff
    int max_retries           = 3;     // Max consecutive recovery failures before FATAL
    int heartbeat_fail_threshold = 3;  // Consecutive non-PLAYING count to trigger recovery (Spec 32 需求 3)
    int state_reset_timeout_ms   = 5000; // Bounded wait for NULL/teardown on main loop (Spec 32 需求 5)
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

    // Detach the monitor from the current pipeline BEFORE it is destroyed:
    // removes the buffer probe and the bus watch, and sets pipeline_ to nullptr.
    // Idempotent. Must be called on the GLib main loop thread (Spec 32 决策 C).
    void detach();

private:
    // State transition (must hold mutex_)
    // Returns true if transition occurred
    bool transition_to(HealthState new_state);

    // Buffer probe callback (static, minimal work)
    static GstPadProbeReturn buffer_probe_cb(GstPad* pad,
                                              GstPadProbeInfo* info,
                                              gpointer user_data);

    // Bus sync handler — filters messages on streaming thread (static)
    static GstBusSyncReply bus_sync_handler(GstBus* bus, GstMessage* msg, gpointer user_data);

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
    int consecutive_non_playing_ = 0;      // Heartbeat debounce counter (Spec 32 需求 3)
    uint64_t branch_error_count_ = 0;      // Branch-level error count (observation, Spec 32 需求 1)
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
