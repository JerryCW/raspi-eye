// network_monitor.h
// 网络状态监控器：聚合 KVS latency pressure 和 WebRTC writeFrame 失败率，
// 向 BitrateAdapter / StreamModeController 报告网络健康状态。
// 纯函数优先设计，便于 PBT 测试。
#pragma once

#include "stream_mode_controller.h"  // BranchStatus

#include <cstdint>
#include <memory>

class BitrateAdapter;
class StreamModeController;

// 网络自适应配置（POD，从 StreamingConfig 转换）
struct NetworkConfig {
    int latency_pressure_threshold = 5;      // 10 秒内触发次数阈值
    int latency_pressure_cooldown_sec = 30;  // pressure 停止后恢复等待秒数
    int writeframe_fail_threshold = 10;      // writeFrame 连续失败阈值
    int writeframe_recovery_count = 50;      // writeFrame 连续成功恢复阈值
};

// --- 纯函数（PBT 友好，无副作用）---

// Latency pressure 状态判断：
// - count >= threshold → UNHEALTHY
// - count < threshold 且 cooldown 过期 → HEALTHY
// - count < threshold 且 cooldown 未过期 → UNHEALTHY（保持）
BranchStatus compute_kvs_network_status(
    int pressure_count_in_window,
    int threshold,
    bool cooldown_expired);

// WriteFrame 健康状态判断：
// - consecutive_failures >= fail_threshold → UNHEALTHY
// - consecutive_successes >= recovery_count → HEALTHY
// - 其他情况保持当前状态（返回 UNHEALTHY 作为保守默认）
BranchStatus compute_webrtc_network_status(
    int consecutive_failures,
    int consecutive_successes,
    int fail_threshold,
    int recovery_count);

// 仅关键帧模式状态转换（per-peer 纯函数）：
// - 连续失败 >= fail_threshold 且当前非仅关键帧 → true（进入仅关键帧模式）
// - 仅关键帧模式下连续成功 >= recovery_in_keyframe_mode → false（恢复正常）
// - 其他情况保持 current_keyframe_only
bool compute_keyframe_only_transition(
    bool current_keyframe_only,
    int consecutive_failures,
    int consecutive_successes,
    int fail_threshold,
    int recovery_in_keyframe_mode);

// --- NetworkMonitor 类（pImpl 模式）---

class NetworkMonitor {
public:
    explicit NetworkMonitor(const NetworkConfig& config = NetworkConfig{});
    ~NetworkMonitor();

    // 禁止拷贝
    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;

    // KVS latency pressure 回调入口（从 KVS SDK 线程调用，线程安全，不阻塞）
    void on_latency_pressure(uint64_t buffer_duration_ms);

    // WebRTC writeFrame 结果回报（从 GStreamer 线程调用）
    void on_writeframe_result(bool success);

    // 连接下游模块（启动前调用）
    void set_bitrate_adapter(BitrateAdapter* adapter);
    void set_stream_mode_controller(StreamModeController* controller);

    // 启动/停止内部 cooldown 定时器
    void start();
    void stop();

    // 查询当前状态（线程安全）
    BranchStatus kvs_network_status() const;
    BranchStatus webrtc_network_status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
