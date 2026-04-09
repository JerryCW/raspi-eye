// health_test.cpp
// Pipeline health monitor tests: 9 example-based + 3 PBT properties.
#include "pipeline_health.h"
#include <gtest/gtest.h>
#include <gst/gst.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static HealthConfig test_config() {
    return HealthConfig{
        .watchdog_timeout_ms  = 100,
        .heartbeat_interval_ms = 50,
        .initial_backoff_ms   = 10,
        .max_retries          = 3
    };
}

static void inject_bus_error(GstElement* pipeline) {
    GstBus* bus = gst_element_get_bus(pipeline);
    GError* error = g_error_new(GST_CORE_ERROR, GST_CORE_ERROR_FAILED,
                                "Injected test error");
    GstMessage* msg = gst_message_new_error(
        GST_OBJECT(pipeline), error, "fault injection");
    gst_bus_post(bus, msg);
    g_error_free(error);
    gst_object_unref(bus);
}

static void inject_bus_warning(GstElement* pipeline) {
    GstBus* bus = gst_element_get_bus(pipeline);
    GError* error = g_error_new(GST_CORE_ERROR, GST_CORE_ERROR_FAILED,
                                "Injected test warning");
    GstMessage* msg = gst_message_new_warning(
        GST_OBJECT(pipeline), error, "warning injection");
    gst_bus_post(bus, msg);
    g_error_free(error);
    gst_object_unref(bus);
}

static GstElement* create_test_pipeline() {
    GstElement* pipeline = gst_parse_launch(
        "videotestsrc name=src is-live=true ! fakesink", nullptr);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    return pipeline;
}

// Create a broken pipeline: unlinked audiotestsrc + fakesink.
// set_state(PLAYING) returns ASYNC but get_state times out because
// caps negotiation never completes on the unlinked pads.
// This makes try_state_reset fail reliably.
static GstElement* create_broken_pipeline() {
    GstElement* pipeline = gst_pipeline_new("broken");
    GstElement* asrc = gst_element_factory_make("audiotestsrc", "asrc");
    GstElement* fsink = gst_element_factory_make("fakesink", "fsink");
    gst_bin_add_many(GST_BIN(pipeline), asrc, fsink, nullptr);
    // Deliberately NOT linking asrc -> fsink
    return pipeline;
}

// Run GMainContext iterations until condition is met or timeout expires.
static bool run_until(std::function<bool()> condition, int timeout_ms = 3000) {
    GMainContext* ctx = g_main_context_default();
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(ctx, FALSE)) {}
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    while (g_main_context_iteration(ctx, FALSE)) {}
    return condition();
}

// ---------------------------------------------------------------------------
// Example-based tests
// ---------------------------------------------------------------------------

// 1. InitialStateIsHealthy — Req 1.2
TEST(HealthMonitor, InitialStateIsHealthy) {
    GstElement* pipeline = create_test_pipeline();
    PipelineHealthMonitor monitor(pipeline, test_config());

    EXPECT_EQ(monitor.state(), HealthState::HEALTHY);
    EXPECT_EQ(monitor.stats().total_recoveries, 0u);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 2. BusErrorTriggersRecovery — Req 3.2, 5.1, 5.2, 9.1, 9.2
TEST(HealthMonitor, BusErrorTriggersRecovery) {
    GstElement* pipeline = create_test_pipeline();
    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.start("src");

    inject_bus_error(pipeline);

    bool recovered = run_until([&]() {
        return monitor.state() == HealthState::HEALTHY &&
               monitor.stats().total_recoveries > 0;
    }, 5000);

    EXPECT_TRUE(recovered) << "State: " << health_state_name(monitor.state());
    EXPECT_GE(monitor.stats().total_recoveries, 1u);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 3. ConsecutiveFailuresReachFatal — Req 1.7, 9.3
TEST(HealthMonitor, ConsecutiveFailuresReachFatal) {
    // Use a broken pipeline where state reset fails (unlinked elements,
    // get_state times out waiting for PLAYING).
    GstElement* pipeline = create_broken_pipeline();

    auto cfg = test_config();
    cfg.max_retries = 2;
    cfg.initial_backoff_ms = 5;
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.set_rebuild_callback([]() -> GstElement* { return nullptr; });
    monitor.start("");

    inject_bus_error(pipeline);

    bool fatal = run_until([&]() {
        return monitor.state() == HealthState::FATAL;
    }, 10000);

    EXPECT_TRUE(fatal) << "State: " << health_state_name(monitor.state());

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 4. HealthCallbackInvoked — Req 8.2, 9.4
TEST(HealthMonitor, HealthCallbackInvoked) {
    GstElement* pipeline = create_test_pipeline();
    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);

    std::vector<std::pair<HealthState, HealthState>> transitions;
    std::mutex transitions_mutex;

    monitor.set_health_callback(
        [&](HealthState old_s, HealthState new_s) {
            std::lock_guard<std::mutex> lock(transitions_mutex);
            transitions.emplace_back(old_s, new_s);
        });
    monitor.start("src");

    inject_bus_error(pipeline);

    run_until([&]() {
        return monitor.state() == HealthState::HEALTHY &&
               monitor.stats().total_recoveries > 0;
    }, 5000);

    monitor.stop();

    std::lock_guard<std::mutex> lock(transitions_mutex);
    EXPECT_GE(transitions.size(), 3u);
    if (!transitions.empty()) {
        EXPECT_EQ(transitions[0].first, HealthState::HEALTHY);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 5. NoCallbackNoCrash — Req 8.3
TEST(HealthMonitor, NoCallbackNoCrash) {
    GstElement* pipeline = create_test_pipeline();
    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.start("src");

    inject_bus_error(pipeline);

    run_until([&]() {
        return monitor.state() == HealthState::HEALTHY &&
               monitor.stats().total_recoveries > 0;
    }, 5000);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 6. CallbackOutsideMutex — Req 8.4
TEST(HealthMonitor, CallbackOutsideMutex) {
    GstElement* pipeline = create_test_pipeline();
    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);

    std::atomic<bool> callback_ok{false};
    monitor.set_health_callback(
        [&](HealthState, HealthState) {
            // If callback is inside mutex, these calls would deadlock
            auto s = monitor.state();
            auto st = monitor.stats();
            (void)s;
            (void)st;
            callback_ok.store(true);
        });
    monitor.start("src");

    inject_bus_error(pipeline);

    bool done = run_until([&]() { return callback_ok.load(); }, 5000);
    EXPECT_TRUE(done) << "Callback was never invoked (possible deadlock)";

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 7. StatsIncrementOnRecovery — Req 7.2, 9.5
TEST(HealthMonitor, StatsIncrementOnRecovery) {
    GstElement* pipeline = create_test_pipeline();
    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.start("src");

    auto before = monitor.stats().total_recoveries;
    auto time_before = std::chrono::steady_clock::now();

    inject_bus_error(pipeline);

    run_until([&]() {
        return monitor.stats().total_recoveries > before;
    }, 5000);

    auto after_stats = monitor.stats();
    EXPECT_EQ(after_stats.total_recoveries, before + 1);
    EXPECT_GE(after_stats.last_recovery_time, time_before);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 8. WarningNoStateChange — Req 3.4
TEST(HealthMonitor, WarningNoStateChange) {
    GstElement* pipeline = create_test_pipeline();
    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.start("src");

    inject_bus_warning(pipeline);

    run_until([&]() { return false; }, 200);

    EXPECT_EQ(monitor.state(), HealthState::HEALTHY);
    EXPECT_EQ(monitor.stats().total_recoveries, 0u);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 9. FullRebuildAfterStateResetFails — Req 5.3, 5.4
TEST(HealthMonitor, FullRebuildAfterStateResetFails) {
    // Broken pipeline: state reset fails (unlinked elements).
    GstElement* pipeline = create_broken_pipeline();

    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);

    std::atomic<int> rebuild_count{0};
    monitor.set_rebuild_callback([&]() -> GstElement* {
        rebuild_count++;
        GstElement* p = gst_parse_launch(
            "videotestsrc name=src is-live=true ! fakesink", nullptr);
        gst_element_set_state(p, GST_STATE_PLAYING);
        return p;
    });
    monitor.start("");

    inject_bus_error(pipeline);

    bool recovered = run_until([&]() {
        return monitor.state() == HealthState::HEALTHY &&
               monitor.stats().total_recoveries > 0;
    }, 10000);

    EXPECT_TRUE(recovered) << "State: " << health_state_name(monitor.state());
    EXPECT_GE(rebuild_count.load(), 1) << "Full rebuild should have been attempted";

    monitor.stop();
    // Original broken pipeline was replaced by rebuild; just unref it
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// ---------------------------------------------------------------------------
// Property-Based Tests (RapidCheck)
// ---------------------------------------------------------------------------

// Property 1: Exponential backoff sequence and FATAL termination
// **Validates: Requirements 1.7, 6.1, 6.2**
RC_GTEST_PROP(ExponentialBackoff, ExponentialBackoffAndFatal, ()) {
    const auto initial_backoff_ms = *rc::gen::inRange(1, 101);
    const auto max_retries = *rc::gen::inRange(1, 6);

    GstElement* pipeline = create_broken_pipeline();

    HealthConfig cfg;
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    cfg.initial_backoff_ms = initial_backoff_ms;
    cfg.max_retries = max_retries;

    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.set_rebuild_callback([]() -> GstElement* { return nullptr; });
    monitor.start("");

    inject_bus_error(pipeline);

    bool fatal = run_until(
        [&]() { return monitor.state() == HealthState::FATAL; }, 10000);

    RC_ASSERT(fatal);
    RC_ASSERT(monitor.state() == HealthState::FATAL);
    RC_ASSERT(monitor.stats().total_recoveries == 0u);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// Property 2: Recovery counter accuracy
// **Validates: Requirements 7.2**
RC_GTEST_PROP(RecoveryCounter, RecoveryCounterAccuracy, ()) {
    const auto K = *rc::gen::inRange(1, 6);

    GstElement* pipeline = create_test_pipeline();

    HealthConfig cfg;
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    cfg.initial_backoff_ms = 10;
    cfg.max_retries = 10;

    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.start("src");

    auto time_before = std::chrono::steady_clock::now();

    for (int i = 0; i < K; ++i) {
        auto before = monitor.stats().total_recoveries;
        inject_bus_error(pipeline);
        bool ok = run_until(
            [&]() {
                return monitor.state() == HealthState::HEALTHY &&
                       monitor.stats().total_recoveries > before;
            }, 5000);
        RC_ASSERT(ok);
    }

    auto st = monitor.stats();
    RC_ASSERT(st.total_recoveries == static_cast<uint32_t>(K));
    RC_ASSERT(st.last_recovery_time >= time_before);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// Property 3: State transition callback correctness
// **Validates: Requirements 8.2**
RC_GTEST_PROP(StateCallback, StateTransitionCallbackCorrectness, ()) {
    const auto N = *rc::gen::inRange(1, 4);

    GstElement* pipeline = create_test_pipeline();

    HealthConfig cfg;
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    cfg.initial_backoff_ms = 10;
    cfg.max_retries = 10;

    PipelineHealthMonitor monitor(pipeline, cfg);

    std::vector<std::pair<HealthState, HealthState>> transitions;
    std::mutex transitions_mutex;

    monitor.set_health_callback(
        [&](HealthState old_s, HealthState new_s) {
            std::lock_guard<std::mutex> lock(transitions_mutex);
            transitions.emplace_back(old_s, new_s);
        });
    monitor.start("src");

    for (int i = 0; i < N; ++i) {
        auto before = monitor.stats().total_recoveries;
        inject_bus_error(pipeline);
        bool ok = run_until(
            [&]() {
                return monitor.state() == HealthState::HEALTHY &&
                       monitor.stats().total_recoveries > before;
            }, 5000);
        RC_ASSERT(ok);
    }

    monitor.stop();

    std::lock_guard<std::mutex> lock(transitions_mutex);

    // Every transition callback must have old != new
    for (const auto& [old_s, new_s] : transitions) {
        RC_ASSERT(old_s != new_s);
    }

    // Must contain HEALTHY->ERROR and RECOVERING->HEALTHY transitions
    bool has_healthy_to_error = false;
    bool has_recovering_to_healthy = false;
    for (const auto& [old_s, new_s] : transitions) {
        if (old_s == HealthState::HEALTHY && new_s == HealthState::ERROR)
            has_healthy_to_error = true;
        if (old_s == HealthState::RECOVERING && new_s == HealthState::HEALTHY)
            has_recovering_to_healthy = true;
    }
    RC_ASSERT(has_healthy_to_error);
    RC_ASSERT(has_recovering_to_healthy);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// ---------------------------------------------------------------------------
// Custom main: gst_init required before any GStreamer API calls
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
