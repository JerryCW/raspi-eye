// main.cpp
// Application entry point - creates a test pipeline and runs GMainLoop.
#include "pipeline_manager.h"
#include "pipeline_builder.h"
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

// Bus callback: handle ERROR and EOS messages from the pipeline.
static gboolean bus_callback(GstBus* /*bus*/, GstMessage* msg, gpointer /*data*/) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);

            auto logger = spdlog::get("main");
            if (logger) {
                logger->error("Error from {}: {}",
                              GST_OBJECT_NAME(msg->src),
                              err ? err->message : "unknown error");
                if (dbg) {
                    logger->debug("Debug info: {}", dbg);
                }
            }

            if (dbg) g_free(dbg);
            if (err) g_error_free(err);
            if (loop) g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_EOS: {
            auto logger = spdlog::get("main");
            if (logger) {
                logger->info("End of stream");
            }
            if (loop) g_main_loop_quit(loop);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// Pipeline run logic - called by gst_macos_main on macOS or directly on Linux.
static int run_pipeline(int argc, char* argv[]) {
    // Parse --log-json argument
    bool use_json = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--log-json") use_json = true;
    }
    log_init::init(use_json);
    auto logger = spdlog::get("main");

    std::string err_msg;
    GstElement* raw_pipeline = PipelineBuilder::build_tee_pipeline(&err_msg);
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

    // Get bus and register watch (bus_callback dispatched via GMainLoop)
    GstBus* bus = gst_element_get_bus(pm->pipeline());
    gst_bus_add_watch(bus, bus_callback, nullptr);
    gst_object_unref(bus);

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

    // Run event loop (blocks until quit)
    g_main_loop_run(loop);

    // Cleanup
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
