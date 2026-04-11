// camera_source.cpp
// Camera abstraction layer implementation.
// Spec 4.5: create_source returns GstBin with ghost pad instead of raw element.
#include "camera_source.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <vector>

namespace {

// ============================================================
// 1.1 V4L2Format enum and helper functions
// ============================================================

enum class V4L2Format { I420, YUYV, MJPG, UNKNOWN };

const char* v4l2_format_name(V4L2Format fmt) {
    switch (fmt) {
        case V4L2Format::I420:    return "I420";
        case V4L2Format::YUYV:    return "YUYV";
        case V4L2Format::MJPG:    return "MJPG";
        case V4L2Format::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

// Parse a single GstStructure from caps query into V4L2Format.
V4L2Format parse_caps_structure(const GstStructure* s) {
    const gchar* media_type = gst_structure_get_name(s);
    if (!media_type) return V4L2Format::UNKNOWN;

    // image/jpeg -> MJPG
    if (g_strcmp0(media_type, "image/jpeg") == 0) {
        return V4L2Format::MJPG;
    }

    // video/x-raw -> check format field
    if (g_strcmp0(media_type, "video/x-raw") == 0) {
        const gchar* format = gst_structure_get_string(s, "format");
        if (!format) return V4L2Format::UNKNOWN;
        if (g_strcmp0(format, "I420") == 0) return V4L2Format::I420;
        if (g_strcmp0(format, "YUY2") == 0 || g_strcmp0(format, "YUYV") == 0) return V4L2Format::YUYV;
    }

    return V4L2Format::UNKNOWN;
}

// Probe V4L2 device supported formats via GStreamer caps query.
// Creates a temporary v4l2src, sets it to READY, queries caps, then cleans up.
// Returns empty vector on failure.
std::vector<V4L2Format> probe_v4l2_formats(const std::string& device_path,
                                            std::string* error_msg) {
    std::vector<V4L2Format> formats;

    GstElement* tmp_src = gst_element_factory_make("v4l2src", nullptr);
    if (!tmp_src) {
        if (error_msg) *error_msg = "Failed to create v4l2src (plugin not available)";
        return formats;
    }

    g_object_set(G_OBJECT(tmp_src), "device", device_path.c_str(), nullptr);

    // Set to READY to trigger device open
    GstStateChangeReturn ret = gst_element_set_state(tmp_src, GST_STATE_READY);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        if (error_msg) {
            *error_msg = "V4L2 format probe failed for ";
            *error_msg += device_path;
            *error_msg += ": unable to open device";
        }
        gst_element_set_state(tmp_src, GST_STATE_NULL);
        gst_object_unref(tmp_src);
        return formats;
    }

    // Query caps from src pad
    GstPad* src_pad = gst_element_get_static_pad(tmp_src, "src");
    if (!src_pad) {
        if (error_msg) {
            *error_msg = "V4L2 format probe failed for ";
            *error_msg += device_path;
            *error_msg += ": no src pad";
        }
        gst_element_set_state(tmp_src, GST_STATE_NULL);
        gst_object_unref(tmp_src);
        return formats;
    }

    GstCaps* caps = gst_pad_query_caps(src_pad, nullptr);
    gst_object_unref(src_pad);

    if (caps) {
        guint n = gst_caps_get_size(caps);
        for (guint i = 0; i < n; ++i) {
            const GstStructure* s = gst_caps_get_structure(caps, i);
            V4L2Format fmt = parse_caps_structure(s);
            if (fmt != V4L2Format::UNKNOWN) {
                // Deduplicate
                bool found = false;
                for (auto& existing : formats) {
                    if (existing == fmt) { found = true; break; }
                }
                if (!found) formats.push_back(fmt);
            }
        }
        gst_caps_unref(caps);
    }

    // Cleanup temporary element
    gst_element_set_state(tmp_src, GST_STATE_NULL);
    gst_object_unref(tmp_src);

    if (formats.empty() && error_msg) {
        *error_msg = "V4L2 device ";
        *error_msg += device_path;
        *error_msg += ": no supported formats detected";
    }

    return formats;
}

// Select best format by priority: I420 > YUYV > MJPG
V4L2Format select_best_format(const std::vector<V4L2Format>& formats) {
    for (auto fmt : formats) {
        if (fmt == V4L2Format::I420) return V4L2Format::I420;
    }
    for (auto fmt : formats) {
        if (fmt == V4L2Format::YUYV) return V4L2Format::YUYV;
    }
    for (auto fmt : formats) {
        if (fmt == V4L2Format::MJPG) return V4L2Format::MJPG;
    }
    return V4L2Format::UNKNOWN;
}

// ============================================================
// 1.2 create_single_element_bin helper
// ============================================================

// Create a GstBin("src") containing a single element with a ghost pad("src").
// Used for TEST (videotestsrc), LIBCAMERA (libcamerasrc), and V4L2 raw formats (v4l2src).
GstElement* create_single_element_bin(const char* factory_name,
                                       const char* element_name,
                                       std::string* error_msg) {
    GstElement* element = gst_element_factory_make(factory_name, element_name);
    if (!element) {
        if (error_msg) {
            *error_msg = "Failed to create ";
            *error_msg += factory_name;
            *error_msg += " (plugin not available)";
        }
        return nullptr;
    }

    GstElement* bin = gst_bin_new("src");
    if (!bin) {
        gst_object_unref(element);
        if (error_msg) *error_msg = "Failed to create source bin";
        return nullptr;
    }

    gst_bin_add(GST_BIN(bin), element);

    // Create ghost pad from element's src pad
    GstPad* src_pad = gst_element_get_static_pad(element, "src");
    if (!src_pad) {
        if (error_msg) {
            *error_msg = "Failed to get src pad from ";
            *error_msg += factory_name;
        }
        gst_object_unref(bin);
        return nullptr;
    }

    GstPad* ghost = gst_ghost_pad_new("src", src_pad);
    gst_object_unref(src_pad);

    if (!ghost) {
        if (error_msg) *error_msg = "Failed to create ghost pad for source bin";
        gst_object_unref(bin);
        return nullptr;
    }

    gst_element_add_pad(bin, ghost);
    return bin;
}

// ============================================================
// 1.3 create_mjpg_bin for MJPG devices
// ============================================================

// Build GstBin("src") containing: v4l2src -> capsfilter(image/jpeg) -> jpegdec
// Ghost pad connects to jpegdec's src pad.
GstElement* create_mjpg_bin(const std::string& device_path,
                             std::string* error_msg) {
    GstElement* bin = gst_bin_new("src");
    if (!bin) {
        if (error_msg) *error_msg = "Failed to create source bin";
        return nullptr;
    }

    GstElement* v4l2 = gst_element_factory_make("v4l2src", "v4l2-source");
    GstElement* capsf = gst_element_factory_make("capsfilter", "mjpg-caps");
    GstElement* jdec = gst_element_factory_make("jpegdec", "jpeg-decoder");

    if (!v4l2 || !capsf || !jdec) {
        if (error_msg) {
            *error_msg = "Failed to create ";
            if (!v4l2) *error_msg += "v4l2src";
            else if (!capsf) *error_msg += "capsfilter";
            else *error_msg += "jpegdec";
            *error_msg += " for source bin";
        }
        if (v4l2) gst_object_unref(v4l2);
        if (capsf) gst_object_unref(capsf);
        if (jdec) gst_object_unref(jdec);
        gst_object_unref(bin);
        return nullptr;
    }

    // Set v4l2src device property
    g_object_set(G_OBJECT(v4l2), "device", device_path.c_str(), nullptr);

    // Set capsfilter: image/jpeg, width=1920, height=1080, framerate=15/1
    GstCaps* caps = gst_caps_new_simple("image/jpeg",
        "width", G_TYPE_INT, 1920,
        "height", G_TYPE_INT, 1080,
        "framerate", GST_TYPE_FRACTION, 15, 1,
        nullptr);
    g_object_set(G_OBJECT(capsf), "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Add elements to bin and link
    gst_bin_add_many(GST_BIN(bin), v4l2, capsf, jdec, nullptr);

    if (!gst_element_link_many(v4l2, capsf, jdec, nullptr)) {
        if (error_msg) *error_msg = "Failed to link source bin elements";
        gst_object_unref(bin);
        return nullptr;
    }

    // Create ghost pad from jpegdec's src pad
    GstPad* jdec_src = gst_element_get_static_pad(jdec, "src");
    if (!jdec_src) {
        if (error_msg) *error_msg = "Failed to get jpegdec src pad";
        gst_object_unref(bin);
        return nullptr;
    }

    GstPad* ghost = gst_ghost_pad_new("src", jdec_src);
    gst_object_unref(jdec_src);

    if (!ghost) {
        if (error_msg) *error_msg = "Failed to create ghost pad for source bin";
        gst_object_unref(bin);
        return nullptr;
    }

    gst_element_add_pad(bin, ghost);
    return bin;
}

} // anonymous namespace

// ============================================================
// CameraSource namespace implementation
// ============================================================

namespace CameraSource {

CameraType default_camera_type() {
#ifdef __APPLE__
    return CameraType::TEST;
#else
    return CameraType::V4L2;
#endif
}

const char* camera_type_name(CameraType type) {
    switch (type) {
        case CameraType::TEST:      return "videotestsrc";
        case CameraType::V4L2:      return "v4l2src";
        case CameraType::LIBCAMERA: return "libcamerasrc";
    }
    return "unknown";
}

// ============================================================
// 1.4 Rewrite create_source to dispatch by CameraType and format
// ============================================================

GstElement* create_source(const CameraConfig& config,
                          std::string* error_msg) {
    auto pl = spdlog::get("pipeline");

    switch (config.type) {
        case CameraType::TEST: {
            GstElement* bin = create_single_element_bin("videotestsrc", "test-source", error_msg);
            if (bin && pl) pl->info("Camera source created: videotestsrc (Source Bin)");
            return bin;
        }

        case CameraType::LIBCAMERA: {
            GstElement* bin = create_single_element_bin("libcamerasrc", "libcam-source", error_msg);
            if (bin && pl) pl->info("Camera source created: libcamerasrc (Source Bin)");
            return bin;
        }

        case CameraType::V4L2: {
#ifdef __linux__
            // If device path is empty, skip format probe and create a simple
            // v4l2src bin (backward compatible with tests using default config).
            // Format probe only runs when --device is explicitly specified.
            if (config.device.empty()) {
                GstElement* bin = create_single_element_bin("v4l2src", "v4l2-source", error_msg);
                if (bin && pl) pl->info("Camera source created: v4l2src (Source Bin, no device specified)");
                return bin;
            }

            // Probe device formats
            auto formats = probe_v4l2_formats(config.device, error_msg);
            if (formats.empty()) {
                return nullptr;
            }

            // Log detected formats
            if (pl) {
                std::string fmt_list;
                for (size_t i = 0; i < formats.size(); ++i) {
                    if (i > 0) fmt_list += ", ";
                    fmt_list += v4l2_format_name(formats[i]);
                }
                pl->info("V4L2 device {}: detected formats [{}]", config.device, fmt_list);
            }

            V4L2Format best = select_best_format(formats);

            if (best == V4L2Format::MJPG) {
                GstElement* bin = create_mjpg_bin(config.device, error_msg);
                if (bin && pl) {
                    pl->info("V4L2 device {}: selected {} -> jpegdec pipeline (Source Bin)",
                             config.device, v4l2_format_name(best));
                }
                return bin;
            } else {
                // Raw format (I420 or YUYV): single v4l2src bin
                GstElement* bin = create_single_element_bin("v4l2src", "v4l2-source", error_msg);
                if (bin) {
                    // Set device property on the inner v4l2src element
                    GstElement* inner = gst_bin_get_by_name(GST_BIN(bin), "v4l2-source");
                    if (inner) {
                        g_object_set(G_OBJECT(inner), "device", config.device.c_str(), nullptr);
                        gst_object_unref(inner);
                    }
                    if (pl) {
                        pl->info("V4L2 device {}: selected {} -> raw pipeline (Source Bin)",
                                 config.device, v4l2_format_name(best));
                    }
                }
                return bin;
            }
#else
            // On non-Linux (macOS), v4l2src plugin is not available
            if (error_msg) *error_msg = "Failed to create v4l2src (plugin not available)";
            return nullptr;
#endif
        }
    }

    if (error_msg) *error_msg = "Unknown camera type";
    return nullptr;
}

bool parse_camera_type(const std::string& str, CameraType& out_type) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "test")      { out_type = CameraType::TEST;      return true; }
    if (lower == "v4l2")      { out_type = CameraType::V4L2;      return true; }
    if (lower == "libcamera") { out_type = CameraType::LIBCAMERA; return true; }
    return false;
}

} // namespace CameraSource
