// main.cpp
// Application entry point - thin wrapper around AppContext lifecycle.
#include "app_context.h"
#include "camera_source.h"
#include "log_init.h"
#include <spdlog/spdlog.h>
#include <gst/gst.h>
#include <csignal>
#include <string>

static GMainLoop* loop = nullptr;

static void sigint_handler(int /*sig*/) {
    if (loop) g_main_loop_quit(loop);
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

    if (logger) {
        if (cam_config.type == CameraSource::CameraType::V4L2) {
            const char* dev = cam_config.device.empty() ? "/dev/video0" : cam_config.device.c_str();
            logger->info("Starting with camera: v4l2src (device={})", dev);
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

    // Create main loop and register signal handlers
    loop = g_main_loop_new(nullptr, FALSE);
    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);

    // Phase 5: AppContext start
    if (!ctx.start(&err)) {
        if (logger) logger->error("AppContext start failed: {}", err);
        g_main_loop_unref(loop);
        loop = nullptr;
        log_init::shutdown();
        return 1;
    }

    // Phase 6: Run main loop
    g_main_loop_run(loop);

    // Phase 7: Cleanup
    ctx.stop();
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
