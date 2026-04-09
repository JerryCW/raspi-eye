// pipeline_builder.h
// Dual-tee pipeline builder for H.264 encoding with three-way split.
//
// Pipeline topology:
//   videotestsrc -> videoconvert -> capsfilter(I420) -> raw-tee
//     -> queue(leaky) -> fakesink("ai-sink")           [AI branch, raw frames]
//     -> queue -> x264enc -> h264parse -> encoded-tee
//       -> queue -> fakesink("kvs-sink")                [KVS branch, H.264]
//       -> queue(leaky) -> fakesink("webrtc-sink")      [WebRTC branch, H.264]
//
// The three fakesink elements are placeholders; subsequent Specs will
// replace them with real sinks (KVS Producer, WebRTC, AI inference).
#pragma once
#include <gst/gst.h>
#include <string>

namespace PipelineBuilder {

// Build the dual-tee pipeline and return the top-level GstPipeline.
// Caller takes ownership via PipelineManager::create(GstElement*).
// Returns nullptr on failure; error_msg receives the detail.
GstElement* build_tee_pipeline(std::string* error_msg = nullptr);

} // namespace PipelineBuilder
