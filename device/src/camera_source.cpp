// camera_source.cpp
// Camera abstraction layer implementation.
#include "camera_source.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>

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

GstElement* create_source(const CameraConfig& config,
                          std::string* error_msg) {
    const char* factory_name = camera_type_name(config.type);
    GstElement* src = gst_element_factory_make(factory_name, "src");

    if (!src) {
        if (error_msg) {
            *error_msg = "Failed to create camera source: ";
            *error_msg += factory_name;
            *error_msg += " (plugin not available)";
        }
        return nullptr;
    }

    // V4L2: set device property (fallback to /dev/video0 if empty)
    if (config.type == CameraType::V4L2) {
        const char* dev = config.device.empty() ? "/dev/video0" : config.device.c_str();
        g_object_set(G_OBJECT(src), "device", dev, nullptr);

        auto pl = spdlog::get("pipeline");
        if (pl) pl->info("Camera source created: v4l2src (device={})", dev);
    } else {
        auto pl = spdlog::get("pipeline");
        if (pl) pl->info("Camera source created: {}", factory_name);
    }

    return src;
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
