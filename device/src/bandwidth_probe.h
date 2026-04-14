// bandwidth_probe.h
// 启动时轻量级带宽估算：测量 kvssink 上传吞吐量，设置合理初始码率。
// 纯函数 compute_initial_bitrate() 便于 PBT 测试。
// 使用 "network" logger，不使用 "pipeline" logger。
#pragma once

#include "bitrate_adapter.h"  // BitrateConfig

#include <gst/gst.h>
#include <memory>

// --- 纯函数（PBT 友好，无副作用）---

// 根据估算带宽计算初始码率：
// 1. target = estimated_bandwidth_kbps * 0.8
// 2. 如果 target < min → 返回 min
// 3. 如果 target > max → 返回 max
// 4. aligned = min + ((target - min) / step) * step（向下取整）
// 5. clamp(aligned, min, max)
// 保证：(result - min) % step == 0
int compute_initial_bitrate(int estimated_bandwidth_kbps,
                            const BitrateConfig& config);

// --- BandwidthProbe 类（pImpl 模式）---

class BandwidthProbe {
public:
    struct ProbeConfig {
        bool enabled = true;
        int duration_sec = 10;
    };

    BandwidthProbe();
    explicit BandwidthProbe(const ProbeConfig& config);
    ~BandwidthProbe();

    // 禁止拷贝
    BandwidthProbe(const BandwidthProbe&) = delete;
    BandwidthProbe& operator=(const BandwidthProbe&) = delete;

    // 启动探测（异步，在 pipeline 启动后调用）
    // pipeline: 用于查找 kvs-sink 元素获取字节计数
    // adapter: 探测完成后可用于设置初始码率（当前仅记录结果）
    // bitrate_config: 用于 compute_initial_bitrate 计算
    void start_probe(GstElement* pipeline,
                     BitrateAdapter* adapter,
                     const BitrateConfig& bitrate_config);

    // 查询探测结果（探测完成后有效）
    int estimated_bandwidth_kbps() const;
    bool probe_completed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
