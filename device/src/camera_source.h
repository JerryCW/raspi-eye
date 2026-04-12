// camera_source.h
// 摄像头源抽象层：根据配置创建 GStreamer 视频源元素
#pragma once
#include <gst/gst.h>
#include <string>
#include <vector>

namespace CameraSource {

// V4L2 设备支持的像素格式枚举
enum class V4L2Format { I420, YUYV, MJPG, UNKNOWN };

// 返回 V4L2Format 对应的可读名称
const char* v4l2_format_name(V4L2Format fmt);

// 按优先级从格式列表中选择最佳格式
// 当前优先级：MJPG > I420 > YUYV
V4L2Format select_best_format(const std::vector<V4L2Format>& formats);

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

// Source Bin 输出格式标记，用于 PipelineBuilder 决定是否跳过 videoconvert
enum class SourceOutputFormat {
    UNKNOWN,  // 未知输出格式（保守路径，保留 videoconvert）
    I420,     // 已是 I420（MJPG+jpegdec、原生 I420）
    YUYV      // 需要 videoconvert 转换
};

// Return GStreamer factory name for the given CameraType
// TEST -> "videotestsrc", V4L2 -> "v4l2src", LIBCAMERA -> "libcamerasrc"
const char* camera_type_name(CameraType type);

// Create GStreamer video source element based on config.
// Returns GstElement* named "src" on success, nullptr on failure.
// error_msg receives error detail (optional).
// out_format receives the source output format (optional, for videoconvert skip logic).
GstElement* create_source(const CameraConfig& config,
                          std::string* error_msg = nullptr,
                          SourceOutputFormat* out_format = nullptr);

// Parse CameraType from string (for CLI argument parsing).
// Accepts "test", "v4l2", "libcamera" (case-insensitive).
// Returns true and sets out_type on success, false on failure.
bool parse_camera_type(const std::string& str, CameraType& out_type);

} // namespace CameraSource
