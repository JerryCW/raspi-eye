// bandwidth_probe.cpp
// BandwidthProbe 实现：启动时测量 kvssink 上传吞吐量，计算初始码率。
// 使用 "network" logger，不使用 "pipeline" logger。
#include "bandwidth_probe.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// 纯函数实现（无副作用）
// ---------------------------------------------------------------------------

int compute_initial_bitrate(int estimated_bandwidth_kbps,
                            const BitrateConfig& config) {
    // target = estimated * 0.8（整数运算：乘 4 除 5 避免浮点）
    int target = estimated_bandwidth_kbps * 4 / 5;

    // clamp 边界
    if (target <= config.min_kbps) {
        return config.min_kbps;
    }
    if (target >= config.max_kbps) {
        // max 本身必须对齐到 step（假设配置合法）
        // 向下取整到 step 档位
        int aligned = config.min_kbps +
            ((config.max_kbps - config.min_kbps) / config.step_kbps) * config.step_kbps;
        return std::min(aligned, config.max_kbps);
    }

    // 向下取整到 step 档位：aligned = min + ((target - min) / step) * step
    int aligned = config.min_kbps +
        ((target - config.min_kbps) / config.step_kbps) * config.step_kbps;

    // 最终 clamp（防御性）
    return std::clamp(aligned, config.min_kbps, config.max_kbps);
}

// ---------------------------------------------------------------------------
// Impl 结构体
// ---------------------------------------------------------------------------

struct BandwidthProbe::Impl {
    ProbeConfig config;
    int estimated_kbps = 0;
    bool probe_done = false;
    mutable std::mutex mutex;
    std::thread probe_thread;

    explicit Impl(const ProbeConfig& cfg) : config(cfg) {}

    ~Impl() {
        if (probe_thread.joinable()) {
            probe_thread.join();
        }
    }
};

// ---------------------------------------------------------------------------
// BandwidthProbe 公共 API
// ---------------------------------------------------------------------------

BandwidthProbe::BandwidthProbe()
    : impl_(std::make_unique<Impl>(ProbeConfig{})) {}

BandwidthProbe::BandwidthProbe(const ProbeConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

BandwidthProbe::~BandwidthProbe() = default;

void BandwidthProbe::start_probe(GstElement* pipeline,
                                 BitrateAdapter* /*adapter*/,
                                 const BitrateConfig& bitrate_config) {
    auto logger = spdlog::get("network");

    // 探测禁用 → 使用 default_kbps
    if (!impl_->config.enabled) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->estimated_kbps = bitrate_config.default_kbps;
        impl_->probe_done = true;
        if (logger) {
            logger->info("BandwidthProbe disabled, using default_kbps={}",
                         bitrate_config.default_kbps);
        }
        return;
    }

    // pipeline 为空 → 使用 default_kbps
    if (!pipeline) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->estimated_kbps = bitrate_config.default_kbps;
        impl_->probe_done = true;
        if (logger) {
            logger->info("BandwidthProbe: pipeline is null, using default_kbps={}",
                         bitrate_config.default_kbps);
        }
        return;
    }

    // 查找 kvs-sink 元素
    GstElement* kvs_sink = gst_bin_get_by_name(GST_BIN(pipeline), "kvs-sink");
    if (!kvs_sink) {
        // kvssink 不可用（fakesink 等）→ 使用 default_kbps
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->estimated_kbps = bitrate_config.default_kbps;
        impl_->probe_done = true;
        if (logger) {
            logger->info("BandwidthProbe: kvs-sink not found, using default_kbps={}",
                         bitrate_config.default_kbps);
        }
        return;
    }

    // kvs-sink 可用 → 启动异步探测线程
    int duration_sec = impl_->config.duration_sec;
    // 增加 kvs_sink 引用计数，线程中使用后释放
    gst_object_ref(kvs_sink);

    impl_->probe_thread = std::thread(
        [this, kvs_sink, duration_sec, bitrate_config]() {
            auto thr_logger = spdlog::get("network");

            // 读取初始 bytes-sent
            guint64 bytes_start = 0;
            GObjectClass* klass = G_OBJECT_GET_CLASS(kvs_sink);
            bool has_bytes_sent = (g_object_class_find_property(klass, "bytes-sent") != nullptr);

            if (has_bytes_sent) {
                g_object_get(kvs_sink, "bytes-sent", &bytes_start, nullptr);
            }

            // 等待探测时长
            std::this_thread::sleep_for(std::chrono::seconds(duration_sec));

            // 读取结束 bytes-sent
            guint64 bytes_end = 0;
            if (has_bytes_sent) {
                g_object_get(kvs_sink, "bytes-sent", &bytes_end, nullptr);
            }

            gst_object_unref(kvs_sink);

            // 计算估算带宽
            int estimated_bw = 0;
            if (has_bytes_sent && bytes_end > bytes_start && duration_sec > 0) {
                guint64 bytes_diff = bytes_end - bytes_start;
                // kbps = bytes_diff * 8 / duration_sec / 1000
                estimated_bw = static_cast<int>(bytes_diff * 8 / duration_sec / 1000);
            }

            // 如果估算为 0（无数据传输），使用 default_kbps
            if (estimated_bw <= 0) {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->estimated_kbps = bitrate_config.default_kbps;
                impl_->probe_done = true;
                if (thr_logger) {
                    thr_logger->info("BandwidthProbe: no data transferred, "
                                     "using default_kbps={}",
                                     bitrate_config.default_kbps);
                }
                return;
            }

            int initial_bitrate = compute_initial_bitrate(estimated_bw, bitrate_config);

            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->estimated_kbps = estimated_bw;
                impl_->probe_done = true;
            }

            if (thr_logger) {
                thr_logger->info("BandwidthProbe completed: estimated_bw={}kbps, "
                                 "initial_bitrate={}kbps (80% of estimated, "
                                 "step-aligned, clamped to [{}, {}])",
                                 estimated_bw, initial_bitrate,
                                 bitrate_config.min_kbps, bitrate_config.max_kbps);
            }
        });
}

int BandwidthProbe::estimated_bandwidth_kbps() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->estimated_kbps;
}

bool BandwidthProbe::probe_completed() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->probe_done;
}
