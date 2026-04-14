// network_monitor.cpp
// NetworkMonitor 实现：聚合 KVS latency pressure 和 WebRTC writeFrame 信号，
// 向 BitrateAdapter / StreamModeController 报告网络健康状态。
// 使用 "network" logger，不使用 "pipeline" logger。
#include "network_monitor.h"
#include "bitrate_adapter.h"
#include "stream_mode_controller.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// 纯函数实现（无副作用）
// ---------------------------------------------------------------------------

BranchStatus compute_kvs_network_status(
    int pressure_count_in_window,
    int threshold,
    bool cooldown_expired) {
    if (pressure_count_in_window >= threshold) {
        return BranchStatus::UNHEALTHY;
    }
    // count < threshold：cooldown 过期才恢复 HEALTHY
    if (cooldown_expired) {
        return BranchStatus::HEALTHY;
    }
    // cooldown 未过期，保持 UNHEALTHY
    return BranchStatus::UNHEALTHY;
}

BranchStatus compute_webrtc_network_status(
    int consecutive_failures,
    int consecutive_successes,
    int fail_threshold,
    int recovery_count) {
    if (consecutive_failures >= fail_threshold) {
        return BranchStatus::UNHEALTHY;
    }
    if (consecutive_successes >= recovery_count) {
        return BranchStatus::HEALTHY;
    }
    // 未达到任何阈值，保守返回 UNHEALTHY
    return BranchStatus::UNHEALTHY;
}

bool compute_keyframe_only_transition(
    bool current_keyframe_only,
    int consecutive_failures,
    int consecutive_successes,
    int fail_threshold,
    int recovery_in_keyframe_mode) {
    if (!current_keyframe_only) {
        // 正常模式：连续失败达到阈值 → 进入仅关键帧模式
        if (consecutive_failures >= fail_threshold) {
            return true;
        }
        return false;
    }
    // 仅关键帧模式：连续成功达到恢复阈值 → 恢复正常
    if (consecutive_successes >= recovery_in_keyframe_mode) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Impl 结构体
// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct NetworkMonitor::Impl {
    // KVS latency pressure 状态
    std::deque<TimePoint> pressure_timestamps;  // 10 秒滑动窗口
    TimePoint last_pressure_time{};             // 最后一次 pressure 时间
    BranchStatus kvs_status = BranchStatus::HEALTHY;

    // WebRTC writeFrame 状态
    int consecutive_failures = 0;
    int consecutive_successes = 0;
    BranchStatus webrtc_status = BranchStatus::HEALTHY;

    // 下游模块指针（不拥有）
    BitrateAdapter* bitrate_adapter = nullptr;
    StreamModeController* stream_controller = nullptr;

    // 配置
    NetworkConfig config;
    mutable std::mutex mutex;

    // cooldown 定时器线程
    std::thread timer_thread;
    std::condition_variable timer_cv;
    bool running = false;

    explicit Impl(const NetworkConfig& cfg) : config(cfg) {}

    // 清理 10 秒窗口外的过期时间戳
    void prune_expired_timestamps(TimePoint now) {
        auto cutoff = now - std::chrono::seconds(10);
        while (!pressure_timestamps.empty() && pressure_timestamps.front() < cutoff) {
            pressure_timestamps.pop_front();
        }
    }

    // cooldown 定时器循环
    void timer_loop() {
        auto logger = spdlog::get("network");
        std::unique_lock<std::mutex> lock(mutex);

        while (running) {
            // 每秒检查一次 cooldown 是否过期
            timer_cv.wait_for(lock, std::chrono::seconds(1));

            if (!running) break;

            auto now = Clock::now();

            // 检查 KVS cooldown 过期
            if (kvs_status == BranchStatus::UNHEALTHY &&
                last_pressure_time != TimePoint{}) {
                prune_expired_timestamps(now);
                int count = static_cast<int>(pressure_timestamps.size());

                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_pressure_time).count();
                bool cooldown_expired = (elapsed >= config.latency_pressure_cooldown_sec);

                auto new_status = compute_kvs_network_status(
                    count, config.latency_pressure_threshold, cooldown_expired);

                if (new_status != kvs_status) {
                    kvs_status = new_status;
                    if (logger) {
                        logger->info("KVS network status recovered to HEALTHY "
                                     "(cooldown expired after {}s)", elapsed);
                    }
                    // 在锁外调用下游
                    auto* adapter = bitrate_adapter;
                    lock.unlock();
                    if (adapter) {
                        adapter->report_kvs_health(BranchStatus::HEALTHY);
                    }
                    lock.lock();
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// NetworkMonitor 公共 API
// ---------------------------------------------------------------------------

NetworkMonitor::NetworkMonitor(const NetworkConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

NetworkMonitor::~NetworkMonitor() {
    stop();
}

void NetworkMonitor::on_latency_pressure(uint64_t buffer_duration_ms) {
    auto logger = spdlog::get("network");
    BitrateAdapter* adapter = nullptr;
    BranchStatus new_status;
    bool status_changed = false;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto now = Clock::now();

        // 记录时间戳
        impl_->pressure_timestamps.push_back(now);
        impl_->last_pressure_time = now;

        // 清理过期条目
        impl_->prune_expired_timestamps(now);

        int count = static_cast<int>(impl_->pressure_timestamps.size());

        // cooldown 未过期（刚收到 pressure）
        new_status = compute_kvs_network_status(
            count, impl_->config.latency_pressure_threshold, false);

        if (new_status != impl_->kvs_status) {
            impl_->kvs_status = new_status;
            status_changed = true;
            adapter = impl_->bitrate_adapter;
        }

        if (logger) {
            logger->warn("latency pressure: buffer_duration={}ms, "
                         "pressure_count={}, threshold={}",
                         buffer_duration_ms, count,
                         impl_->config.latency_pressure_threshold);
        }
    }

    // 状态变化时在锁外调用下游
    if (status_changed && adapter) {
        adapter->report_kvs_health(new_status);
    }
}

void NetworkMonitor::on_writeframe_result(bool success) {
    auto logger = spdlog::get("network");
    StreamModeController* controller = nullptr;
    BranchStatus new_status;
    bool status_changed = false;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);

        if (success) {
            impl_->consecutive_successes++;
            impl_->consecutive_failures = 0;
        } else {
            impl_->consecutive_failures++;
            impl_->consecutive_successes = 0;
        }

        new_status = compute_webrtc_network_status(
            impl_->consecutive_failures,
            impl_->consecutive_successes,
            impl_->config.writeframe_fail_threshold,
            impl_->config.writeframe_recovery_count);

        if (new_status != impl_->webrtc_status) {
            auto old_status = impl_->webrtc_status;
            impl_->webrtc_status = new_status;
            status_changed = true;
            controller = impl_->stream_controller;

            if (logger) {
                if (new_status == BranchStatus::UNHEALTHY) {
                    logger->warn("WebRTC UNHEALTHY: consecutive_failures={}",
                                 impl_->consecutive_failures);
                } else {
                    logger->info("WebRTC recovered to HEALTHY: consecutive_successes={}",
                                 impl_->consecutive_successes);
                }
            }
        }
    }

    // 状态变化时在锁外调用下游
    if (status_changed && controller) {
        controller->report_webrtc_status(new_status);
    }
}

void NetworkMonitor::set_bitrate_adapter(BitrateAdapter* adapter) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->bitrate_adapter = adapter;
}

void NetworkMonitor::set_stream_mode_controller(StreamModeController* controller) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stream_controller = controller;
}

void NetworkMonitor::start() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->running) return;
    impl_->running = true;
    impl_->timer_thread = std::thread([this]() { impl_->timer_loop(); });

    auto logger = spdlog::get("network");
    if (logger) {
        logger->info("NetworkMonitor started: pressure_threshold={}, "
                     "cooldown={}s, writeframe_fail_threshold={}",
                     impl_->config.latency_pressure_threshold,
                     impl_->config.latency_pressure_cooldown_sec,
                     impl_->config.writeframe_fail_threshold);
    }
}

void NetworkMonitor::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running) return;
        impl_->running = false;
    }
    impl_->timer_cv.notify_all();
    if (impl_->timer_thread.joinable()) {
        impl_->timer_thread.join();
    }

    auto logger = spdlog::get("network");
    if (logger) {
        logger->info("NetworkMonitor stopped");
    }
}

BranchStatus NetworkMonitor::kvs_network_status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->kvs_status;
}

BranchStatus NetworkMonitor::webrtc_network_status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->webrtc_status;
}
