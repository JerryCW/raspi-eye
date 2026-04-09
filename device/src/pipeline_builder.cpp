// pipeline_builder.cpp
// Dual-tee pipeline construction using GStreamer C API.
#include "pipeline_builder.h"
#include "camera_source.h"
#include <spdlog/spdlog.h>
#include <array>

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

} // anonymous namespace

// --- build_tee_pipeline ------------------------------------------------

GstElement* PipelineBuilder::build_tee_pipeline(std::string* error_msg,
                                                CameraSource::CameraConfig config) {
    // 1. Pipeline container
    GstElement* pipeline = gst_pipeline_new("tee-pipeline");
    if (!pipeline) {
        if (error_msg) *error_msg = "Failed to create pipeline container";
        return nullptr;
    }

    // 2. Create all 14 elements
    GstElement* src       = CameraSource::create_source(config, error_msg);
    GstElement* convert   = gst_element_factory_make("videoconvert",  "convert");
    GstElement* capsfilter= gst_element_factory_make("capsfilter",    "capsfilter");
    GstElement* raw_tee   = gst_element_factory_make("tee",           "raw-tee");

    GstElement* q_ai      = gst_element_factory_make("queue",         "q-ai");
    GstElement* ai_sink   = gst_element_factory_make("fakesink",      "ai-sink");

    GstElement* q_enc     = gst_element_factory_make("queue",         "q-enc");
    GstElement* encoder   = create_encoder(error_msg);
    GstElement* parser    = gst_element_factory_make("h264parse",     "parser");
    GstElement* enc_tee   = gst_element_factory_make("tee",           "encoded-tee");

    GstElement* q_kvs     = gst_element_factory_make("queue",         "q-kvs");
    GstElement* kvs_sink  = gst_element_factory_make("fakesink",      "kvs-sink");

    GstElement* q_web     = gst_element_factory_make("queue",         "q-web");
    GstElement* web_sink  = gst_element_factory_make("fakesink",      "webrtc-sink");

    // 3. Null-check: if any element failed, clean up and bail out.
    //    Elements are not yet in the bin, so each must be individually unref'd.
    GstElement* all[] = {
        src, convert, capsfilter, raw_tee,
        q_ai, ai_sink,
        q_enc, encoder, parser, enc_tee,
        q_kvs, kvs_sink,
        q_web, web_sink
    };
    const char* names[] = {
        "src", "videoconvert", "capsfilter", "raw-tee",
        "q-ai", "ai-sink",
        "q-enc", "encoder", "h264parse", "encoded-tee",
        "q-kvs", "kvs-sink",
        "q-web", "webrtc-sink"
    };
    static_assert(sizeof(all) / sizeof(all[0]) == sizeof(names) / sizeof(names[0]),
                  "element and name arrays must match");

    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        if (!all[i]) {
            // error_msg may already be set by create_encoder; only overwrite if empty
            if (error_msg && error_msg->empty()) {
                *error_msg = "Failed to create element: ";
                *error_msg += names[i];
            }
            // Clean up all non-null elements created so far (not yet in bin)
            for (size_t j = 0; j < sizeof(all) / sizeof(all[0]); ++j) {
                if (all[j]) gst_object_unref(all[j]);
            }
            gst_object_unref(pipeline);
            return nullptr;
        }
    }

    // 4. Set capsfilter caps: video/x-raw,format=I420
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "I420", nullptr);
    g_object_set(G_OBJECT(capsfilter), "caps", caps, nullptr);
    gst_caps_unref(caps);  // Must unref immediately after setting

    // 5. Configure queue parameters
    //    AI and WebRTC: leaky=downstream(2), max-size-buffers=1
    //    Encoder and KVS: max-size-buffers=1 (no leaky)
    g_object_set(G_OBJECT(q_ai),  "max-size-buffers", 1, "leaky", 2, nullptr);
    g_object_set(G_OBJECT(q_web), "max-size-buffers", 1, "leaky", 2, nullptr);
    g_object_set(G_OBJECT(q_enc), "max-size-buffers", 1, nullptr);
    g_object_set(G_OBJECT(q_kvs), "max-size-buffers", 1, nullptr);

    // 6. fakesink names are already set via gst_element_factory_make above

    // 7. Add all elements to the pipeline bin
    gst_bin_add_many(GST_BIN(pipeline),
        src, convert, capsfilter, raw_tee,
        q_ai, ai_sink,
        q_enc, encoder, parser, enc_tee,
        q_kvs, kvs_sink,
        q_web, web_sink,
        nullptr);

    // --- From this point, pipeline owns all elements.
    // --- On failure, only gst_object_unref(pipeline) is needed.

    // 8. Link trunk: src -> convert -> capsfilter -> raw-tee
    if (!gst_element_link_many(src, convert, capsfilter, raw_tee, nullptr)) {
        if (error_msg) *error_msg = "Failed to link trunk (src -> convert -> capsfilter -> raw-tee)";
        gst_object_unref(pipeline);
        return nullptr;
    }

    // 9. Link encoding chain: q-enc -> encoder -> parser -> enc-tee
    if (!gst_element_link_many(q_enc, encoder, parser, enc_tee, nullptr)) {
        if (error_msg) *error_msg = "Failed to link encoding chain (q-enc -> encoder -> parser -> enc-tee)";
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
    if (!gst_element_link(q_kvs, kvs_sink)) {
        if (error_msg) *error_msg = "Failed to link q-kvs -> kvs-sink";
        gst_object_unref(pipeline);
        return nullptr;
    }
    if (!gst_element_link(q_web, web_sink)) {
        if (error_msg) *error_msg = "Failed to link q-web -> webrtc-sink";
        gst_object_unref(pipeline);
        return nullptr;
    }

    auto pl = spdlog::get("pipeline");
    if (pl) pl->info("Dual-tee pipeline built successfully (14 elements)");

    return pipeline;
}
