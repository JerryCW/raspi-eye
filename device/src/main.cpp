// main.cpp
// Application entry point - creates a test pipeline and runs GMainLoop.
#include "pipeline_manager.h"
#include "pipeline_builder.h"
#include "pipeline_health.h"
#include "camera_source.h"
#include "log_init.h"
#include <spdlog/spdlog.h>
#include <gst/gst.h>
#include <csignal>
#include <string>

static GMainLoop* loop = nullptr;

// SIGINT handler: quit the main loop on Ctrl+C.
static void sigint_handler(int /*sig*/) {
    if (loop) g_main_loop_quit(loop);
}

// Pipeline run logic - called by gst_macos_main on macOS or directly on Linux.
static int run_pipeline(int argc, char* argv[]) {
    // Phase 1: Parse all command-line arguments before log init
    bool use_json = false;
    CameraSource::CameraConfig cam_config;
    bool has_device = false;
    bool camera_parse_ok = true;
    std::string camera_raw_value;

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
        }
    }

    // Phase 2: Initialize logging (--log-json must be parsed first)
    log_init::init(use_json);
    auto logger = spdlog::get("main");

    // Phase 3: Validate parsed values (error logging requires initialized logger)
    if (!camera_parse_ok) {
        if (logger) logger->error("Invalid camera type: {}", camera_raw_value);
        log_init::shutdown();
        return 1;
    }

    // Warn if --device is provided for non-v4l2 camera types
    if (has_device && cam_config.type != CameraSource::CameraType::V4L2) {
        if (logger) logger->warn("--device ignored (only used with v4l2)");
    }

    // Log the camera type being used at startup
    if (logger) {
        if (cam_config.type == CameraSource::CameraType::V4L2) {
            const char* dev = cam_config.device.empty() ? "/dev/video0" : cam_config.device.c_str();
            logger->info("Starting with camera: v4l2src (device={})", dev);
        } else {
            logger->info("Starting with camera: {}",
                         CameraSource::camera_type_name(cam_config.type));
        }
    }

    // Build pipeline with camera config
    std::string err_msg;
    GstElement* raw_pipeline = PipelineBuilder::build_tee_pipeline(&err_msg, cam_config);
    if (!raw_pipeline) {
        if (logger) logger->error("Failed to build tee pipeline: {}", err_msg);
        log_init::shutdown();
        return 1;
    }

    auto pm = PipelineManager::create(raw_pipeline, &err_msg);
    if (!pm) {
        if (logger) logger->error("Failed to adopt pipeline: {}", err_msg);
        log_init::shutdown();
        return 1;
    }

    // Create main loop
    loop = g_main_loop_new(nullptr, FALSE);

    // Register SIGINT handler
    std::signal(SIGINT, sigint_handler);

    // Start pipeline
    if (!pm->start(&err_msg)) {
        if (logger) logger->error("Failed to start pipeline: {}", err_msg);
        g_main_loop_unref(loop);
        loop = nullptr;
        log_init::shutdown();
        return 1;
    }

    // Create health monitor (bus watch is managed by monitor, not main)
    PipelineHealthMonitor health_monitor(pm->pipeline());

    // Register rebuild callback: rebuild pipeline and update PipelineManager
    health_monitor.set_rebuild_callback([&pm, &cam_config]() -> GstElement* {
        std::string err;
        GstElement* p = PipelineBuilder::build_tee_pipeline(&err, cam_config);
        if (!p) return nullptr;
        auto new_pm = PipelineManager::create(p, &err);
        if (!new_pm) {
            gst_object_unref(p);
            return nullptr;
        }
        if (!new_pm->start(&err)) {
            return nullptr;
        }
        pm = std::move(new_pm);
        return pm->pipeline();
    });

    // Register health callback: log state changes, quit on FATAL
    health_monitor.set_health_callback(
        [](HealthState old_s, HealthState new_s) {
            auto lg = spdlog::get("main");
            if (lg) {
                lg->info("Health state: {} -> {}",
                         health_state_name(old_s),
                         health_state_name(new_s));
            }
            if (new_s == HealthState::FATAL && loop) {
                g_main_loop_quit(loop);
            }
        });

    // Start health monitoring (installs bus watch, probe, timers)
    health_monitor.start("src");

    // Run event loop (blocks until quit)
    g_main_loop_run(loop);

    // Cleanup
    health_monitor.stop();
    pm->stop();
    g_main_loop_unref(loop);
    loop = nullptr;

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
