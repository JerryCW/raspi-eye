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
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
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

// Inject a bus ERROR whose source is the named element inside the pipeline.
// Used to test ErrorScope-based routing (Spec 32 需求 1).
static void inject_bus_error_from(GstElement* pipeline, const char* element_name) {
    GstElement* el = gst_bin_get_by_name(GST_BIN(pipeline), element_name);
    GstBus* bus = gst_element_get_bus(pipeline);
    GError* error = g_error_new(GST_CORE_ERROR, GST_CORE_ERROR_FAILED,
                                "Injected branch error");
    GstMessage* msg = gst_message_new_error(
        GST_OBJECT(el ? el : pipeline), error, "branch fault injection");
    gst_bus_post(bus, msg);
    g_error_free(error);
    gst_object_unref(bus);
    if (el) gst_object_unref(el);
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

// ---------------------------------------------------------------------------
// ThreadProbe: minimal GstElement subclass that records the thread id of every
// state transition it undergoes (Spec 32.5 S1/S6 thread assertions).
// A GstBin propagates set_state to its children synchronously on the calling
// thread, so the recorded ids expose which thread executed set_state on the
// pipeline -- exactly the S6/S1 hazard surface (kvssink change_state, incl.
// the NULL_TO_READY producer re-init, runs on the set_state caller thread).
// ---------------------------------------------------------------------------

typedef struct { GstElement parent; } ThreadProbe;
typedef struct { GstElementClass parent_class; } ThreadProbeClass;

static std::mutex g_tp_mutex;
static std::map<GstStateChange, std::thread::id> g_tp_threads;

G_DEFINE_TYPE(ThreadProbe, thread_probe, GST_TYPE_ELEMENT)

static GstStateChangeReturn thread_probe_change_state(GstElement* element,
                                                      GstStateChange transition) {
    {
        std::lock_guard<std::mutex> lock(g_tp_mutex);
        g_tp_threads[transition] = std::this_thread::get_id();
    }
    return GST_ELEMENT_CLASS(thread_probe_parent_class)
        ->change_state(element, transition);
}

static void thread_probe_class_init(ThreadProbeClass* klass) {
    GST_ELEMENT_CLASS(klass)->change_state = thread_probe_change_state;
}

static void thread_probe_init(ThreadProbe* /*self*/) {}

static void thread_probe_reset() {
    std::lock_guard<std::mutex> lock(g_tp_mutex);
    g_tp_threads.clear();
}

static bool thread_probe_lookup(GstStateChange transition, std::thread::id* out) {
    std::lock_guard<std::mutex> lock(g_tp_mutex);
    auto it = g_tp_threads.find(transition);
    if (it == g_tp_threads.end()) return false;
    *out = it->second;
    return true;
}

// Working pipeline (videotestsrc name=src ! fakesink) + a ThreadProbe child.
static GstElement* create_probe_pipeline() {
    GstElement* pipeline = gst_pipeline_new("probe-pipe");
    GstElement* src = gst_element_factory_make("videotestsrc", "src");
    GstElement* sink = gst_element_factory_make("fakesink", "sink");
    GstElement* probe = GST_ELEMENT(
        g_object_new(thread_probe_get_type(), "name", "thread-probe", nullptr));
    g_object_set(src, "is-live", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), src, sink, probe, nullptr);
    gst_element_link(src, sink);
    return pipeline;
}

// Broken pipeline (unlinked audiotestsrc + fakesink, PLAYING never reached)
// + a ThreadProbe child. Mirrors create_broken_pipeline().
static GstElement* create_broken_probe_pipeline() {
    GstElement* pipeline = gst_pipeline_new("broken-probe");
    GstElement* asrc = gst_element_factory_make("audiotestsrc", "asrc");
    GstElement* fsink = gst_element_factory_make("fakesink", "fsink");
    GstElement* probe = GST_ELEMENT(
        g_object_new(thread_probe_get_type(), "name", "thread-probe", nullptr));
    gst_bin_add_many(GST_BIN(pipeline), asrc, fsink, probe, nullptr);
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

// 2b. KvsBranchErrorNoRecovery — Spec 32 需求 1
//     Bus ERROR from an element named "kvs-sink" is classified KVS_BRANCH and
//     must NOT trigger whole-pipeline recovery (state stays HEALTHY).
TEST(HealthMonitor, KvsBranchErrorNoRecovery) {
    GstElement* pipeline = gst_parse_launch(
        "videotestsrc name=src is-live=true ! fakesink name=kvs-sink", nullptr);
    ASSERT_NE(pipeline, nullptr);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;  // disable heartbeat interference
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.start("src");

    inject_bus_error_from(pipeline, "kvs-sink");

    // Pump the loop for a while; branch error must NOT cause recovery.
    run_until([&]() { return monitor.stats().total_recoveries > 0; }, 800);

    EXPECT_EQ(monitor.state(), HealthState::HEALTHY);
    EXPECT_EQ(monitor.stats().total_recoveries, 0u);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 2c. WebRtcBranchErrorNoRecovery — Spec 32 需求 1.3
TEST(HealthMonitor, WebRtcBranchErrorNoRecovery) {
    GstElement* pipeline = gst_parse_launch(
        "videotestsrc name=src is-live=true ! fakesink name=webrtc-sink", nullptr);
    ASSERT_NE(pipeline, nullptr);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.start("src");

    inject_bus_error_from(pipeline, "webrtc-sink");
    run_until([&]() { return monitor.stats().total_recoveries > 0; }, 800);

    EXPECT_EQ(monitor.state(), HealthState::HEALTHY);
    EXPECT_EQ(monitor.stats().total_recoveries, 0u);

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
// Spec 32.5 缺陷 2: bounded helper thread assertions (design Property 4 轨 b)
// ---------------------------------------------------------------------------

// 10. Injected fns run on a worker thread, never on the caller thread.
//     Covers the mechanics behind S1 (set_null_bounded), S6 (set_playing_bounded)
//     and S2/S3 (teardown_pipeline_bounded).
TEST(BoundedHelpers, InjectedFnRunsOffCallerThread) {
    GstElement* p = gst_pipeline_new("bounded-thread-pipe");
    const auto caller_tid = std::this_thread::get_id();

    std::thread::id null_tid, playing_tid, teardown_tid;
    EXPECT_TRUE(set_null_bounded(p, 1000, [&](GstElement*) {
        null_tid = std::this_thread::get_id();
    }));
    EXPECT_TRUE(set_playing_bounded(p, 1000, [&](GstElement*) {
        playing_tid = std::this_thread::get_id();
    }));
    EXPECT_TRUE(teardown_pipeline_bounded(p, 1000, [&](GstElement*) {
        teardown_tid = std::this_thread::get_id();
    }));

    EXPECT_NE(null_tid, caller_tid);      // S1 hazard surface
    EXPECT_NE(playing_tid, caller_tid);   // S6 hazard surface
    EXPECT_NE(teardown_tid, caller_tid);  // S2/S3 hazard surface

    // Custom teardown_fn: caller owns the unref decision.
    gst_object_unref(p);
}

// 11. Timeout branch is observable: returns false within budget while the
//     detached worker completes in the background (S4 ref keeps the pipeline
//     alive for the whole detached run).
TEST(BoundedHelpers, TimeoutDetachesAndCompletesInBackground) {
    GstElement* p = gst_pipeline_new("bounded-timeout-pipe");
    auto worker_done = std::make_shared<std::atomic<bool>>(false);

    auto start = std::chrono::steady_clock::now();
    bool ok = set_playing_bounded(p, 50, [worker_done](GstElement*) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        worker_done->store(true);
    });
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_FALSE(ok);          // timeout -> false
    EXPECT_LT(elapsed, 250);   // caller not blocked for the fn's full 300ms

    // Background completion flag must eventually be set.
    bool completed = false;
    for (int i = 0; i < 100; ++i) {
        if (worker_done->load()) { completed = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(completed);
    // Give the detached worker a moment for its final S4 unref + done log.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gst_object_unref(p);
}

// 12. S6 封堵: driving try_state_reset (TRUNK bus error on a working pipeline)
//     must run the gst state changes on worker threads, never on the main loop
//     thread. NULL_TO_READY is the S6 取证定案 transition (kvssink producer
//     re-init destroys the old producer synchronously in that leg).
TEST(HealthMonitor, StateResetRunsStateChangesOffMainThread) {
    GstElement* pipeline = create_probe_pipeline();
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.start("src");

    // Drop setup-time recordings (they happened on the main thread).
    thread_probe_reset();
    const auto main_tid = std::this_thread::get_id();

    inject_bus_error(pipeline);

    bool recovered = run_until([&]() {
        return monitor.state() == HealthState::HEALTHY &&
               monitor.stats().total_recoveries > 0;
    }, 10000);
    ASSERT_TRUE(recovered) << "State: " << health_state_name(monitor.state());

    std::thread::id tid;
    // set_null_bounded worker executed the downward transitions.
    ASSERT_TRUE(thread_probe_lookup(GST_STATE_CHANGE_READY_TO_NULL, &tid));
    EXPECT_NE(tid, main_tid);
    // set_playing_bounded worker executed the uplift (S6 定案调用点).
    ASSERT_TRUE(thread_probe_lookup(GST_STATE_CHANGE_NULL_TO_READY, &tid));
    EXPECT_NE(tid, main_tid);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 13. S1 封堵: "set_null_bounded 成功 + PLAYING 拉起失败" branch -- the
//     failure-branch NULL push must also run on a worker thread (previously a
//     bare synchronous set_state(NULL) on the main loop).
TEST(HealthMonitor, StateResetFailureBranchNullRunsOffMainThread) {
    // Broken pipeline: initial set_null is a no-op (already NULL, records no
    // transitions), PLAYING uplift fails -> any READY_TO_NULL recorded can only
    // come from the failure-branch set_null_bounded worker.
    GstElement* pipeline = create_broken_probe_pipeline();

    auto cfg = test_config();
    cfg.max_retries = 1;  // one attempt then FATAL, keeps the test fast
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.set_rebuild_callback([]() -> GstElement* { return nullptr; });
    monitor.start("");

    thread_probe_reset();
    const auto main_tid = std::this_thread::get_id();

    inject_bus_error(pipeline);

    bool fatal = run_until([&]() {
        return monitor.state() == HealthState::FATAL;
    }, 10000);
    ASSERT_TRUE(fatal) << "State: " << health_state_name(monitor.state());

    std::thread::id tid;
    // S6: the uplift ran on the set_playing_bounded worker.
    ASSERT_TRUE(thread_probe_lookup(GST_STATE_CHANGE_NULL_TO_READY, &tid));
    EXPECT_NE(tid, main_tid);
    // S1: the failure-branch NULL push ran on the set_null_bounded worker.
    ASSERT_TRUE(thread_probe_lookup(GST_STATE_CHANGE_READY_TO_NULL, &tid));
    EXPECT_NE(tid, main_tid);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// ---------------------------------------------------------------------------
// Spec 32.5 缺陷 1: recovery liveness checkpoints (CP1-CP3.5/CP6)
// ---------------------------------------------------------------------------

// 14. Successful state reset fires CP1/CP2/CP2.5/CP3/CP6 exactly once each.
TEST(HealthMonitor, CheckpointsFireOnSuccessfulStateReset) {
    GstElement* pipeline = create_test_pipeline();
    auto cfg = test_config();
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);

    std::atomic<int> checkpoints{0};
    monitor.set_liveness_callback([&]() { checkpoints++; });
    monitor.start("src");

    inject_bus_error(pipeline);

    bool recovered = run_until([&]() {
        return monitor.state() == HealthState::HEALTHY &&
               monitor.stats().total_recoveries > 0;
    }, 5000);
    ASSERT_TRUE(recovered) << "State: " << health_state_name(monitor.state());

    // CP1 + CP2 + CP2.5 + CP3 + CP6
    EXPECT_EQ(checkpoints.load(), 5);

    monitor.stop();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

// 15. PLAYING 拉起失败 path fires the additional CP3.5 (6 checkpoints total).
TEST(HealthMonitor, CheckpointsFireOnPlayingFailure) {
    GstElement* pipeline = create_broken_pipeline();

    auto cfg = test_config();
    cfg.max_retries = 1;  // one attempt then FATAL
    cfg.watchdog_timeout_ms = 60000;
    cfg.heartbeat_interval_ms = 60000;
    PipelineHealthMonitor monitor(pipeline, cfg);
    monitor.set_rebuild_callback([]() -> GstElement* { return nullptr; });

    std::atomic<int> checkpoints{0};
    monitor.set_liveness_callback([&]() { checkpoints++; });
    monitor.start("");

    inject_bus_error(pipeline);

    bool fatal = run_until([&]() {
        return monitor.state() == HealthState::FATAL;
    }, 10000);
    ASSERT_TRUE(fatal) << "State: " << health_state_name(monitor.state());

    // CP1 + CP2 + CP2.5 + CP3 + CP3.5 + CP6
    EXPECT_EQ(checkpoints.load(), 6);

    monitor.stop();
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

// Property 4 (Spec 32.5 PBT-3): checkpoint injection does not alter the state
// machine trajectory -- with a liveness callback registered, N successful
// recoveries produce exactly the per-recovery track HEALTHY->ERROR ->
// ERROR->RECOVERING -> RECOVERING->HEALTHY, identical to the un-instrumented
// baseline (health_test Property 3), while checkpoints fire 5x per recovery.
// **Validates: Requirements 3.4**
RC_GTEST_PROP(CheckpointInjection, DoesNotAlterStateTrack, ()) {
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
    std::atomic<int> checkpoints{0};

    monitor.set_liveness_callback([&]() { checkpoints++; });
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
    RC_ASSERT(transitions.size() == static_cast<size_t>(3 * N));
    for (int i = 0; i < N; ++i) {
        RC_ASSERT(transitions[3 * i] ==
                  std::make_pair(HealthState::HEALTHY, HealthState::ERROR));
        RC_ASSERT(transitions[3 * i + 1] ==
                  std::make_pair(HealthState::ERROR, HealthState::RECOVERING));
        RC_ASSERT(transitions[3 * i + 2] ==
                  std::make_pair(HealthState::RECOVERING, HealthState::HEALTHY));
    }
    // Checkpoints fired (CP1/CP2/CP2.5/CP3/CP6 per successful recovery)
    // without changing the trajectory.
    RC_ASSERT(checkpoints.load() == 5 * N);

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
