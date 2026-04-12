// main.cpp
// Application entry point - thin wrapper around AppContext lifecycle.
#include "app_context.h"
#include "camera_source.h"
#include "log_init.h"
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

// idle callback 轮询 shutdown flag
static gboolean check_shutdown(gpointer data) {
    auto* loop = static_cast<GMainLoop*>(data);
    if (g_shutdown_requested.load(std::memory_order_relaxed)) {
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static int run_pipeline(int argc, char* argv[]) {
    // GStreamer init (on macOS, gst_macos_main handles this)
    gst_init(&argc, &argv);

    // Phase 1: Parse all command-line arguments before log init
    bool use_json = false;
    CameraSource::CameraConfig cam_config;
    bool has_device = false;
    bool camera_parse_ok = true;
    std::string camera_raw_value;
    std::string config_path = "device/config/config.toml";

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--log-json") {
            use_json = true;
        } else if (arg == "--camera" && i + 1 < argc) {
            camera_raw_value = argv[++i];
            CameraSource::CameraType type;
            if (!CameraSource::parse_camera_type(camera_raw_value, type)) {
                camera_parse_ok = false;
            } else {
                cam_config.type = type;
            }
        } else if (arg == "--device" && i + 1 < argc) {
            cam_config.device = argv[++i];
            has_device = true;
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    // Phase 2: Initialize logging
    log_init::init(use_json);
    auto logger = spdlog::get("main");

    // Phase 3: Validate parsed values
    if (!camera_parse_ok) {
        if (logger) logger->error("Invalid camera type: {}", camera_raw_value);
        log_init::shutdown();
        return 1;
    }

    if (has_device && cam_config.type != CameraSource::CameraType::V4L2) {
        if (logger) logger->warn("--device ignored (only used with v4l2)");
    }

    if (cam_config.type == CameraSource::CameraType::V4L2 && !has_device) {
        if (logger) logger->error("V4L2 camera requires --device (e.g. --device /dev/IMX678)");
        log_init::shutdown();
        return 1;
    }

    if (logger) {
        if (cam_config.type == CameraSource::CameraType::V4L2) {
            logger->info("Starting with camera: v4l2src (device={})", cam_config.device.c_str());
        } else {
            logger->info("Starting with camera: {}",
                         CameraSource::camera_type_name(cam_config.type));
        }
    }

    // Phase 4: AppContext init
    AppContext ctx;
    std::string err;
    if (!ctx.init(config_path, cam_config, &err)) {
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

    // Register idle callback to poll shutdown flag
    g_idle_add(check_shutdown, loop);

    // Phase 5: AppContext start
    if (!ctx.start(&err)) {
        if (logger) logger->error("AppContext start failed: {}", err);
        g_main_loop_unref(loop);
        log_init::shutdown();
        return 1;
    }

    // Phase 6: Run main loop
    g_main_loop_run(loop);

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
