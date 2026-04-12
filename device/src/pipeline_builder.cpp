// pipeline_builder.cpp
// Dual-tee pipeline construction using GStreamer C API.
#include "pipeline_builder.h"
#include "camera_source.h"
#include "kvs_sink_factory.h"
#include "webrtc_media.h"
#ifdef ENABLE_YOLO
#include "ai_pipeline_handler.h"
#endif
#include <gst/app/gstappsink.h>
#include <spdlog/spdlog.h>
#include <array>
#include <vector>

namespace {

// --- Encoder candidate list (priority order) ---------------------------

struct EncoderCandidate {
    const char* factory_name;   // GStreamer element factory name
    const char* display_name;   // Human-readable name for logging
};

// Current: software-only. Future: prepend hardware encoder entries.
constexpr std::array<EncoderCandidate, 1> kEncoderCandidates = {{
    {"x264enc", "x264enc (software)"},
}};

// --- create_encoder ----------------------------------------------------
// Iterate candidates, probe availability via gst_element_factory_find,
// create the first available encoder and configure low-latency params.

GstElement* create_encoder(std::string* error_msg) {
    auto pl = spdlog::get("pipeline");

    for (const auto& c : kEncoderCandidates) {
        GstElementFactory* factory = gst_element_factory_find(c.factory_name);
        if (!factory) {
            if (pl) pl->info("Encoder {} not available, trying next", c.factory_name);
            continue;
        }
        gst_object_unref(factory);  // factory_find ref must be released

        GstElement* encoder = gst_element_factory_make(c.factory_name, "encoder");
        if (!encoder) {
            if (pl) pl->warn("Failed to create {}", c.factory_name);
            continue;
        }

        // x264enc low-latency parameters (integer enum/flag values)
        g_object_set(G_OBJECT(encoder),
            "tune",         0x00000004,  // zerolatency (GstX264EncTune flags)
            "speed-preset", 1,           // ultrafast   (GstX264EncPreset enum)
            "threads",      2,           // 2 threads for Pi 5
            nullptr);

        if (pl) pl->info("Selected encoder: {} (ultrafast, zerolatency, threads=2)",
                         c.display_name);
        return encoder;
    }

    if (error_msg) *error_msg = "No H.264 encoder available (tried: x264enc)";
    return nullptr;
}

// --- link_tee_to_element -----------------------------------------------
// Request a src pad from tee, get the downstream sink pad, link them,
// and release both pad references.

bool link_tee_to_element(GstElement* tee, GstElement* element,
                         std::string* error_msg) {
    GstPad* tee_pad = gst_element_request_pad_simple(tee, "src_%u");
    if (!tee_pad) {
        if (error_msg) {
            *error_msg = "Failed to request pad from tee '";
            *error_msg += GST_ELEMENT_NAME(tee);
            *error_msg += "'";
        }
        return false;
    }

    GstPad* sink_pad = gst_element_get_static_pad(element, "sink");
    if (!sink_pad) {
        gst_object_unref(tee_pad);
        if (error_msg) {
            *error_msg = "Failed to get sink pad from '";
            *error_msg += GST_ELEMENT_NAME(element);
            *error_msg += "'";
        }
        return false;
    }

    GstPadLinkReturn ret = gst_pad_link(tee_pad, sink_pad);
    gst_object_unref(sink_pad);
    gst_object_unref(tee_pad);

    if (ret != GST_PAD_LINK_OK) {
        if (error_msg) {
            *error_msg = "Failed to link tee pad to '";
            *error_msg += GST_ELEMENT_NAME(element);
            *error_msg += "'";
        }
        return false;
    }
    return true;
}

// --- on_new_sample ---------------------------------------------------------
// appsink callback: extract H.264 frame from GstBuffer and broadcast to
// all connected PeerConnections via WebRtcMediaManager.

static GstFlowReturn on_new_sample(GstElement* sink, gpointer user_data) {
    auto* manager = static_cast<WebRtcMediaManager*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        uint64_t pts = GST_BUFFER_PTS(buffer);
        bool is_keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        // Convert PTS: GStreamer ns -> KVS 100ns
        uint64_t timestamp_100ns = pts / 100;
        manager->broadcast_frame(map.data, map.size, timestamp_100ns, is_keyframe);
        gst_buffer_unmap(buffer, &map);
    }
    // gst_buffer_map failure: skip this frame, unref sample, return OK
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

} // anonymous namespace

// --- build_tee_pipeline ------------------------------------------------

GstElement* PipelineBuilder::build_tee_pipeline(std::string* error_msg,
                                                CameraSource::CameraConfig config,
                                                const KvsSinkFactory::KvsConfig* kvs_config,
                                                const AwsConfig* aws_config,
                                                WebRtcMediaManager* webrtc_media,
                                                AiPipelineHandler* ai_handler) {
    // 1. Pipeline container
    GstElement* pipeline = gst_pipeline_new("tee-pipeline");
    if (!pipeline) {
        if (error_msg) *error_msg = "Failed to create pipeline container";
        return nullptr;
    }

    // 2. Create elements — conditionally skip videoconvert when source already outputs I420
    CameraSource::SourceOutputFormat src_format = CameraSource::SourceOutputFormat::UNKNOWN;
    GstElement* src       = CameraSource::create_source(config, error_msg, &src_format);
    bool need_convert = (src_format != CameraSource::SourceOutputFormat::I420);
    GstElement* convert   = need_convert
                            ? gst_element_factory_make("videoconvert", "convert")
                            : nullptr;
    GstElement* capsfilter= gst_element_factory_make("capsfilter",    "capsfilter");
    GstElement* raw_tee   = gst_element_factory_make("tee",           "raw-tee");

    GstElement* q_ai      = gst_element_factory_make("queue",         "q-ai");
    GstElement* ai_sink   = gst_element_factory_make("fakesink",      "ai-sink");

    GstElement* q_enc     = gst_element_factory_make("queue",         "q-enc");
    GstElement* encoder   = create_encoder(error_msg);
    GstElement* parser    = gst_element_factory_make("h264parse",     "parser");
    if (parser) {
        // Insert SPS/PPS before every IDR frame — required for WebRTC viewers
        // that may join mid-stream and need codec config to decode.
        g_object_set(G_OBJECT(parser), "config-interval", -1, nullptr);
    }
    // Force byte-stream output from parser so WebRTC appsink gets Annex B NALUs.
    GstElement* bs_caps   = gst_element_factory_make("capsfilter",    "bs-caps");
    GstElement* enc_tee   = gst_element_factory_make("tee",           "encoded-tee");

    GstElement* q_kvs     = gst_element_factory_make("queue",         "q-kvs");
    // kvssink needs AVC format, so add h264parse to convert byte-stream → avc
    GstElement* kvs_parser= gst_element_factory_make("h264parse",     "kvs-parser");
    GstElement* avc_caps  = gst_element_factory_make("capsfilter",    "avc-caps");
    GstElement* kvs_sink  = nullptr;
    if (kvs_config && aws_config) {
        kvs_sink = KvsSinkFactory::create_kvs_sink(*kvs_config, *aws_config, error_msg);
    } else {
        kvs_sink = gst_element_factory_make("fakesink", "kvs-sink");
    }

    GstElement* q_web     = gst_element_factory_make("queue",         "q-web");
    GstElement* web_sink  = nullptr;
    if (webrtc_media) {
        web_sink = gst_element_factory_make("appsink", "webrtc-sink");
    } else {
        web_sink = gst_element_factory_make("fakesink", "webrtc-sink");
    }

    // 3. Null-check: if any element failed, clean up and bail out.
    //    Elements are not yet in the bin, so each must be individually unref'd.
    //    When need_convert is false, convert is intentionally nullptr — skip it in checks.
    //    Build dynamic arrays excluding convert when skipped.
    std::vector<GstElement*> all;
    std::vector<const char*> names;

    all.push_back(src);        names.push_back("src");
    if (need_convert) {
        all.push_back(convert);    names.push_back("videoconvert");
    }
    all.push_back(capsfilter); names.push_back("capsfilter");
    all.push_back(raw_tee);    names.push_back("raw-tee");
    all.push_back(q_ai);       names.push_back("q-ai");
    all.push_back(ai_sink);    names.push_back("ai-sink");
    all.push_back(q_enc);      names.push_back("q-enc");
    all.push_back(encoder);    names.push_back("encoder");
    all.push_back(parser);     names.push_back("h264parse");
    all.push_back(bs_caps);    names.push_back("bs-caps");
    all.push_back(enc_tee);    names.push_back("encoded-tee");
    all.push_back(q_kvs);      names.push_back("q-kvs");
    all.push_back(kvs_parser); names.push_back("kvs-parser");
    all.push_back(avc_caps);   names.push_back("avc-caps");
    all.push_back(kvs_sink);   names.push_back("kvs-sink");
    all.push_back(q_web);      names.push_back("q-web");
    all.push_back(web_sink);   names.push_back("webrtc-sink");

    for (size_t i = 0; i < all.size(); ++i) {
        if (!all[i]) {
            // error_msg may already be set by create_encoder; only overwrite if empty
            if (error_msg && error_msg->empty()) {
                *error_msg = "Failed to create element: ";
                *error_msg += names[i];
            }
            // Clean up all non-null elements created so far (not yet in bin)
            for (size_t j = 0; j < all.size(); ++j) {
                if (all[j]) gst_object_unref(all[j]);
            }
            gst_object_unref(pipeline);
            return nullptr;
        }
    }

    // 4. Set capsfilter caps: video/x-raw,format=I420 with optional resolution/framerate
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "I420", nullptr);
    if (config.width > 0 && config.height > 0) {
        gst_caps_set_simple(caps,
            "width", G_TYPE_INT, config.width,
            "height", G_TYPE_INT, config.height,
            nullptr);
    }
    if (config.framerate > 0) {
        gst_caps_set_simple(caps,
            "framerate", GST_TYPE_FRACTION, config.framerate, 1,
            nullptr);
    }
    g_object_set(G_OBJECT(capsfilter), "caps", caps, nullptr);
    gst_caps_unref(caps);

    // 4b. Set byte-stream caps after h264parse (for WebRTC Annex B)
    GstCaps* bs = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au", nullptr);
    g_object_set(G_OBJECT(bs_caps), "caps", bs, nullptr);
    gst_caps_unref(bs);

    // 4c. Set AVC caps for kvssink branch
    GstCaps* avc = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "avc",
        "alignment", G_TYPE_STRING, "au", nullptr);
    g_object_set(G_OBJECT(avc_caps), "caps", avc, nullptr);
    gst_caps_unref(avc);

    // 5. Configure queue parameters
    //    AI and WebRTC: leaky=downstream(2), max-size-buffers=1
    //    Encoder and KVS: max-size-buffers=1 (no leaky)
    g_object_set(G_OBJECT(q_ai),  "max-size-buffers", 1, "leaky", 2, nullptr);
    g_object_set(G_OBJECT(q_web), "max-size-buffers", 1, "leaky", 2, nullptr);
    g_object_set(G_OBJECT(q_enc), "max-size-buffers", 1, nullptr);
    g_object_set(G_OBJECT(q_kvs), "max-size-buffers", 1, nullptr);

    // 6. Configure appsink when WebRtcMediaManager is provided
    if (webrtc_media) {
        g_object_set(G_OBJECT(web_sink),
            "emit-signals", TRUE,
            "drop", TRUE,
            "max-buffers", 1,
            "sync", FALSE,
            nullptr);
        g_signal_connect(web_sink, "new-sample",
                         G_CALLBACK(on_new_sample), webrtc_media);
    }

    // 7. Add all elements to the pipeline bin
    if (need_convert) {
        gst_bin_add_many(GST_BIN(pipeline),
            src, convert, capsfilter, raw_tee,
            q_ai, ai_sink,
            q_enc, encoder, parser, bs_caps, enc_tee,
            q_kvs, kvs_parser, avc_caps, kvs_sink,
            q_web, web_sink,
            nullptr);
    } else {
        gst_bin_add_many(GST_BIN(pipeline),
            src, capsfilter, raw_tee,
            q_ai, ai_sink,
            q_enc, encoder, parser, bs_caps, enc_tee,
            q_kvs, kvs_parser, avc_caps, kvs_sink,
            q_web, web_sink,
            nullptr);
    }

    // --- From this point, pipeline owns all elements.
    // --- On failure, only gst_object_unref(pipeline) is needed.

    // 8. Link trunk: conditionally skip videoconvert
    if (need_convert) {
        if (!gst_element_link_many(src, convert, capsfilter, raw_tee, nullptr)) {
            if (error_msg) *error_msg = "Failed to link trunk (src -> convert -> capsfilter -> raw-tee)";
            gst_object_unref(pipeline);
            return nullptr;
        }
    } else {
        if (!gst_element_link_many(src, capsfilter, raw_tee, nullptr)) {
            if (error_msg) *error_msg = "Failed to link trunk (src -> capsfilter -> raw-tee)";
            gst_object_unref(pipeline);
            return nullptr;
        }
    }

    // 9. Link encoding chain: q-enc -> encoder -> parser -> bs-caps -> enc-tee
    if (!gst_element_link_many(q_enc, encoder, parser, bs_caps, enc_tee, nullptr)) {
        if (error_msg) *error_msg = "Failed to link encoding chain";
        gst_object_unref(pipeline);
        return nullptr;
    }

    // 10. Link tee request pads (4 connections)
    if (!link_tee_to_element(raw_tee, q_ai, error_msg) ||
        !link_tee_to_element(raw_tee, q_enc, error_msg) ||
        !link_tee_to_element(enc_tee, q_kvs, error_msg) ||
        !link_tee_to_element(enc_tee, q_web, error_msg)) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    // 11. Link branch tails
    if (!gst_element_link(q_ai, ai_sink)) {
        if (error_msg) *error_msg = "Failed to link q-ai -> ai-sink";
        gst_object_unref(pipeline);
        return nullptr;
    }
    if (!gst_element_link(q_kvs, kvs_parser)) {
        if (error_msg) *error_msg = "Failed to link q-kvs -> kvs-parser";
        gst_object_unref(pipeline);
        return nullptr;
    }
    if (!gst_element_link_many(kvs_parser, avc_caps, kvs_sink, nullptr)) {
        if (error_msg) *error_msg = "Failed to link kvs-parser -> avc-caps -> kvs-sink";
        gst_object_unref(pipeline);
        return nullptr;
    }
    if (!gst_element_link(q_web, web_sink)) {
        if (error_msg) *error_msg = "Failed to link q-web -> webrtc-sink";
        gst_object_unref(pipeline);
        return nullptr;
    }

    auto pl = spdlog::get("pipeline");
    if (pl) {
        if (!need_convert) {
            pl->info("Skipping videoconvert: source already outputs I420");
        } else {
            const char* fmt_name = "UNKNOWN";
            if (src_format == CameraSource::SourceOutputFormat::YUYV) fmt_name = "YUYV";
            else if (src_format == CameraSource::SourceOutputFormat::UNKNOWN) fmt_name = "UNKNOWN";
            pl->info("Using videoconvert: source format={}", fmt_name);
        }
        int elem_count = need_convert ? 17 : 16;
        pl->info("Dual-tee pipeline built successfully ({} elements)", elem_count);
    }

    // Install AI pipeline probe if handler provided
#ifdef ENABLE_YOLO
    if (ai_handler) {
        std::string probe_err;
        if (!ai_handler->install_probe(pipeline, &probe_err)) {
            spdlog::warn("Failed to install AI probe: {}", probe_err);
            // Non-fatal: pipeline works without AI probe
        }
    }
#else
    (void)ai_handler;
#endif

    return pipeline;
}
