// main.cpp
// Application entry point - thin wrapper around AppContext lifecycle.
#include "app_context.h"
#include "config_manager.h"
#include "log_init.h"
#include "sd_notifier.h"
#include "shutdown_handler.h"
#include <spdlog/spdlog.h>
#include <gst/gst.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <string>

// 全局 atomic（async-signal-safe）
static std::atomic<int> g_signal_count{0};
static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int /*sig*/) {
    if (g_signal_count.fetch_add(1, std::memory_order_relaxed) >= 1) {
        _exit(EXIT_FAILURE);  // 第二次信号，强制退出
    }
    g_shutdown_requested.store(true, std::memory_order_relaxed);
}

// timeout callback 轮询 shutdown flag（每 200ms）
static gboolean check_shutdown(gpointer data) {
    auto* loop = static_cast<GMainLoop*>(data);
    if (g_shutdown_requested.load(std::memory_order_relaxed)) {
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static int run_pipeline(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    // Phase 1: Parse command-line arguments into ConfigOverrides
    ConfigOverrides overrides;
    std::string config_path = "device/config/config.toml";

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--log-json") {
            overrides.log_json = true;
        } else if (arg == "--camera" && i + 1 < argc) {
            overrides.camera_type = argv[++i];
        } else if (arg == "--device" && i + 1 < argc) {
            overrides.device = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    // Phase 2: Load config to get logging settings
    ConfigManager config;
    std::string err;
    if (!config.load(config_path, &err)) {
        // Fallback: init logging with basic settings, then report error
        log_init::init(overrides.log_json);
        auto logger = spdlog::get("main");
        if (logger) logger->error("Config load failed: {}", err);
        log_init::shutdown();
        return 1;
    }
    if (!config.apply_overrides(overrides, &err)) {
        log_init::init(overrides.log_json);
        auto logger = spdlog::get("main");
        if (logger) logger->error("Config override failed: {}", err);
        log_init::shutdown();
        return 1;
    }

    // Phase 3: Initialize logging with full config (level + format)
    // init() creates all 9 named loggers: main, pipeline, app, config, stream, ai, kvs, webrtc, s3
    log_init::init(config.logging_config());
    log_init::setup_kvs_log_redirect();
    auto logger = spdlog::get("main");

    // Phase 4: AppContext init (will re-load config internally)
    AppContext ctx;
    if (!ctx.init(config_path, overrides, &err)) {
        if (logger) logger->error("AppContext init failed: {}", err);
        log_init::shutdown();
        return 1;
    }

    // Create main loop (local variable, not global)
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

    // Register signal handlers via sigaction (async-signal-safe)
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Register timeout callback to poll shutdown flag (every 200ms)
    g_timeout_add(200, check_shutdown, loop);

    // Phase 5: AppContext start
    if (!ctx.start(&err)) {
        if (logger) logger->error("AppContext start failed: {}", err);
        g_main_loop_unref(loop);
        log_init::shutdown();
        return 1;
    }

    // Phase 5.5: 通知 systemd 启动完成 + 启动看门狗
    SdNotifier::notify_ready();
    SdNotifier::start_watchdog_thread();

    // Phase 6: Run main loop
    g_main_loop_run(loop);

    // Phase 6.5: 通知 systemd 正在关闭 + 停止看门狗
    SdNotifier::notify_stopping();
    SdNotifier::stop_watchdog_thread();

    // Phase 7: Cleanup
    auto summary = ctx.stop();
    if (logger) {
        logger->info("shutdown complete: {} step(s), {}ms total",
                     summary.steps.size(), summary.total_duration_ms);
        for (const auto& step : summary.steps) {
            logger->info("  [{}]: {} ({}ms)", step.name,
                         status_str(step.status), step.duration_ms);
        }
    }
    g_main_loop_unref(loop);
    log_init::shutdown();
    return 0;
}

int main(int argc, char* argv[]) {
#ifdef __APPLE__
    return gst_macos_main((GstMainFunc)run_pipeline, argc, argv, nullptr);
#else
    return run_pipeline(argc, argv);
#endif
}
