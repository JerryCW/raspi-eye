// camera_source.h
// Camera abstraction layer: create GStreamer video source element based on config.
#pragma once
#include <gst/gst.h>
#include <string>

namespace CameraSource {

// Camera type enum
enum class CameraType {
    TEST,       // videotestsrc (macOS dev environment)
    V4L2,       // v4l2src (IMX678 USB camera, Pi 5 primary)
    LIBCAMERA   // libcamerasrc (IMX216 CSI camera, Pi 5 secondary)
};

// Platform default camera type
// macOS -> TEST, Linux -> V4L2
CameraType default_camera_type();

// Camera configuration (POD struct)
struct CameraConfig {
    CameraType type = default_camera_type();
    std::string device;  // v4l2src device path, empty defaults to /dev/video0
    int width = 1280;       // Resolution width (parse/store only, pipeline use in future spec)
    int height = 720;       // Resolution height
    int framerate = 15;     // Capture framerate
};

// Return GStreamer factory name for the given CameraType
// TEST -> "videotestsrc", V4L2 -> "v4l2src", LIBCAMERA -> "libcamerasrc"
const char* camera_type_name(CameraType type);

// Create GStreamer video source element based on config.
// Returns GstElement* named "src" on success, nullptr on failure.
// error_msg receives error detail (optional).
GstElement* create_source(const CameraConfig& config,
                          std::string* error_msg = nullptr);

// Parse CameraType from string (for CLI argument parsing).
// Accepts "test", "v4l2", "libcamera" (case-insensitive).
// Returns true and sets out_type on success, false on failure.
bool parse_camera_type(const std::string& str, CameraType& out_type);

} // namespace CameraSource
