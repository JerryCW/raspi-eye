// main.cpp
// Application entry point — creates a test pipeline and runs GMainLoop.
#include "pipeline_manager.h"
#include <gst/gst.h>
#include <csignal>
#include <cstdlib>

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
            g_printerr("Error from %s: %s\n",
                        GST_OBJECT_NAME(msg->src),
                        err ? err->message : "unknown error");
            if (dbg) {
                g_printerr("Debug info: %s\n", dbg);
                g_free(dbg);
            }
            if (err) g_error_free(err);
            if (loop) g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_printerr("End of stream\n");
            if (loop) g_main_loop_quit(loop);
            break;
        default:
            break;
    }
    return TRUE;
}

// Pipeline run logic — called by gst_macos_main on macOS or directly on Linux.
static int run_pipeline(int /*argc*/, char* /*argv*/[]) {
    std::string err_msg;
    auto pm = PipelineManager::create(
        "videotestsrc ! videoconvert ! autovideosink", &err_msg);
    if (!pm) {
        g_printerr("Failed to create pipeline: %s\n", err_msg.c_str());
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
        g_printerr("Failed to start pipeline: %s\n", err_msg.c_str());
        g_main_loop_unref(loop);
        loop = nullptr;
        return 1;
    }

    // Run event loop (blocks until quit)
    g_main_loop_run(loop);

    // Cleanup
    pm->stop();
    g_main_loop_unref(loop);
    loop = nullptr;

    return 0;
}

int main(int argc, char* argv[]) {
#ifdef __APPLE__
    return gst_macos_main((GstMainFunc)run_pipeline, argc, argv, nullptr);
#else
    return run_pipeline(argc, argv);
#endif
}
