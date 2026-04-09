// pipeline_manager.cpp
// PipelineManager implementation - wraps GStreamer C API with RAII.
#include "pipeline_manager.h"
#include <spdlog/spdlog.h>

// --- GStreamer init (shared by both create() overloads) ----------------

bool PipelineManager::ensure_gst_init(std::string* error_msg) {
    static bool gst_initialised = false;
    if (gst_initialised) return true;

    GError* init_err = nullptr;
    if (!gst_init_check(nullptr, nullptr, &init_err)) {
        if (error_msg) {
            *error_msg = "Failed to initialize GStreamer";
            if (init_err) {
                *error_msg += ": ";
                *error_msg += init_err->message;
            }
        }
        if (init_err) g_error_free(init_err);
        return false;
    }
    gst_initialised = true;
    return true;
}

// --- Factory -----------------------------------------------------------

std::unique_ptr<PipelineManager> PipelineManager::create(
    const std::string& pipeline_desc,
    std::string* error_msg)
{
    // Empty description guard
    if (pipeline_desc.empty()) {
        if (error_msg) *error_msg = "Pipeline description is empty";
        return nullptr;
    }

    // One-time GStreamer init
    if (!ensure_gst_init(error_msg)) return nullptr;

    // Parse the pipeline description
    GError* parse_err = nullptr;
    GstElement* pipeline = gst_parse_launch(pipeline_desc.c_str(), &parse_err);

    if (parse_err) {
        if (error_msg) {
            *error_msg = "Failed to parse pipeline: ";
            *error_msg += parse_err->message;
        }
        g_error_free(parse_err);
        if (pipeline) {
            gst_object_unref(pipeline);
        }
        return nullptr;
    }

    if (!pipeline) {
        if (error_msg) *error_msg = "gst_parse_launch returned NULL";
        return nullptr;
    }

    auto pl = spdlog::get("pipeline");
    if (pl) pl->info("Pipeline created: {}", pipeline_desc);

    return std::unique_ptr<PipelineManager>(new PipelineManager(pipeline));
}

std::unique_ptr<PipelineManager> PipelineManager::create(
    GstElement* pipeline,
    std::string* error_msg)
{
    if (!pipeline) {
        if (error_msg) *error_msg = "Pipeline pointer is null";
        return nullptr;
    }

    if (!ensure_gst_init(error_msg)) return nullptr;

    auto pl = spdlog::get("pipeline");
    if (pl) pl->info("Pipeline adopted from pre-built GstElement*");

    return std::unique_ptr<PipelineManager>(new PipelineManager(pipeline));
}

// --- Constructor / Destructor ------------------------------------------

PipelineManager::PipelineManager(GstElement* pipeline)
    : pipeline_(pipeline) {}

PipelineManager::~PipelineManager() {
    stop();
}

// --- Move semantics ----------------------------------------------------

PipelineManager::PipelineManager(PipelineManager&& other) noexcept
    : pipeline_(other.pipeline_)
{
    other.pipeline_ = nullptr;
}

PipelineManager& PipelineManager::operator=(PipelineManager&& other) noexcept {
    if (this != &other) {
        stop();                        // release current resource
        pipeline_ = other.pipeline_;
        other.pipeline_ = nullptr;
    }
    return *this;
}

// --- Start / Stop / State ----------------------------------------------

bool PipelineManager::start(std::string* error_msg) {
    if (!pipeline_) {
        if (error_msg) *error_msg = "Pipeline is not initialized";
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        if (error_msg) *error_msg = "Failed to set pipeline to PLAYING";
        auto pl = spdlog::get("pipeline");
        if (pl && error_msg) pl->error("Failed to start pipeline: {}", *error_msg);
        return false;
    }

    auto pl = spdlog::get("pipeline");
    if (pl) pl->info("Pipeline started");
    return true;
}

void PipelineManager::stop() {
    if (!pipeline_) return;

    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;

    auto pl = spdlog::get("pipeline");
    if (pl) pl->info("Pipeline stopped");
}

GstState PipelineManager::current_state() const {
    if (!pipeline_) return GST_STATE_NULL;

    GstState state = GST_STATE_NULL;
    // 5-second timeout for state query (Pi 5 x264enc init may take >3s)
    gst_element_get_state(pipeline_, &state, nullptr, 5 * GST_SECOND);
    return state;
}
