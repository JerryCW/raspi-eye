// pipeline_manager.h
// GStreamer pipeline lifecycle manager with RAII semantics.
#pragma once
#include <gst/gst.h>
#include <memory>
#include <string>

class PipelineManager {
public:
    // Factory function: create a pipeline from a description string.
    // Returns unique_ptr on success, nullptr on failure.
    // error_msg receives the error detail if non-null.
    static std::unique_ptr<PipelineManager> create(
        const std::string& pipeline_desc,
        std::string* error_msg = nullptr);

    ~PipelineManager();

    // No copy
    PipelineManager(const PipelineManager&) = delete;
    PipelineManager& operator=(const PipelineManager&) = delete;

    // Move
    PipelineManager(PipelineManager&& other) noexcept;
    PipelineManager& operator=(PipelineManager&& other) noexcept;

    // Start the pipeline (set to PLAYING).
    // Returns true on success, false on failure with optional error_msg.
    bool start(std::string* error_msg = nullptr);

    // Stop the pipeline and release resources (idempotent).
    void stop();

    // Query the current pipeline state.
    GstState current_state() const;

    // Access the underlying GstElement* pipeline (non-owning).
    // Returns nullptr if the pipeline has been stopped.
    GstElement* pipeline() const { return pipeline_; }

private:
    explicit PipelineManager(GstElement* pipeline);
    GstElement* pipeline_ = nullptr;
};
