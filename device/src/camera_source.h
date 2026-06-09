// camera_source.h
// 摄像头源抽象层：根据配置创建 GStreamer 视频源元素
#pragma once
#include <gst/gst.h>
#include <functional>
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
    int rotation = 0;       // Video rotation in degrees: 0 (default), 90, 180, 270
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

// 摄像头打开重试配置（Spec 32 需求 2）。
// 用于恢复/重建时覆盖旧 v4l2 fd 尚未释放的窗口。
struct OpenRetryConfig {
    int max_attempts = 6;    // 最多尝试次数
    int interval_ms  = 500;  // 每次尝试间隔（毫秒）
};

// 反复调用 try_open（返回 true 即成功），直到成功或耗尽 max_attempts。
// 不直接依赖 GStreamer，便于单测：sleep_ms 可注入（默认真实 sleep）。
// out_attempts（可选）返回实际调用 try_open 的次数。
// 返回是否成功。约定：成功时 try_open 调用次数 ∈ [1, max_attempts]；
// 全失败时恰好调用 max_attempts 次并返回 false。
bool open_with_retry(const std::function<bool()>& try_open,
                     const OpenRetryConfig& cfg,
                     const std::function<void(int)>& sleep_ms = {},
                     int* out_attempts = nullptr);

} // namespace CameraSource
