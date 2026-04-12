// pipeline_builder.h
// Dual-tee pipeline builder for H.264 encoding with three-way split.
//
// Pipeline topology:
//   videotestsrc -> videoconvert -> capsfilter(I420) -> raw-tee
//     -> queue(leaky) -> fakesink("ai-sink")           [AI branch, raw frames]
//     -> queue -> x264enc -> h264parse -> encoded-tee
//       -> queue -> fakesink("kvs-sink")                [KVS branch, H.264]
//       -> queue(leaky) -> appsink/fakesink("webrtc-sink") [WebRTC branch, H.264]
//
// The ai-sink is a placeholder; subsequent Specs will replace it with
// real AI inference sink. kvs-sink uses KvsSinkFactory when configured.
// webrtc-sink uses appsink when WebRtcMediaManager is provided, otherwise fakesink.
#pragma once
#include <gst/gst.h>
#include <string>
#include "camera_source.h"
#include "kvs_sink_factory.h"

class WebRtcMediaManager;  // Forward declaration
class AiPipelineHandler;   // Forward declaration

namespace PipelineBuilder {

// Build the dual-tee pipeline and return the top-level GstPipeline.
// Caller takes ownership via PipelineManager::create(GstElement*).
// Returns nullptr on failure; error_msg receives the detail.
// error_msg in front to keep compatibility with existing build_tee_pipeline(&err) calls.
// When kvs_config and aws_config are non-null, KvsSinkFactory creates the KVS sink.
// When nullptr (default), a fakesink is used (backward compatible).
// When webrtc_media is non-null, an appsink replaces the webrtc fakesink.
GstElement* build_tee_pipeline(
    std::string* error_msg = nullptr,
    CameraSource::CameraConfig config = CameraSource::CameraConfig{},
    const KvsSinkFactory::KvsConfig* kvs_config = nullptr,
    const AwsConfig* aws_config = nullptr,
    WebRtcMediaManager* webrtc_media = nullptr,
    AiPipelineHandler* ai_handler = nullptr);

} // namespace PipelineBuilder
