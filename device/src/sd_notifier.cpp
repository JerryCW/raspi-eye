// sd_notifier.cpp
// 跨平台 systemd 通知封装实现。
// HAVE_SYSTEMD 由 CMake 在 Linux 上检测到 libsystemd 时定义。
#include "sd_notifier.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

// --- 静态变量 ---
static std::mutex s_mtx;
static std::condition_variable s_cv;
static std::atomic<bool> s_stop{false};
static std::atomic<bool> s_running{false};
static std::thread s_thread;

// 健康门控回调（s_health_mtx 保护，看门狗线程读 / 主循环写）
static std::mutex s_health_mtx;
static std::function<bool()> s_health_check;

// --- Spec 32.5 缺陷 1：liveness 门控状态 ---
// liveness 时间戳（steady_clock 毫秒；0 = 未刷新哨兵）。
// 主循环 timer / 恢复检查点写，看门狗线程读——必须 lock-free 原子。
// 阈值链约束（改任何参数必须重推，见 spec-32.5 design 决策点 1）：
//   相邻检查点最大间隔 5s+ε < T_stale=10s < 喂狗间隔 15s < WatchdogSec=30s
static std::atomic<int64_t> s_liveness_ms{0};
static std::atomic<int64_t> s_stale_threshold_ms{10000};  // T_stale 默认 10s
static_assert(std::atomic<int64_t>::is_always_lock_free,
              "atomic<int64_t> must be lock-free on target platforms "
              "(aarch64/x86_64) for watchdog-thread reads");

// steady_clock 当前毫秒
static int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// --- notify 方法 ---

void SdNotifier::notify_ready() {
#ifdef HAVE_SYSTEMD
    int r = sd_notify(0, "READY=1");
    auto logger = spdlog::get("app");
    if (r < 0) {
        if (logger) logger->warn("sd_notify READY=1 failed: {}", r);
    } else {
        if (logger) logger->info("sd_notify: READY=1 sent");
    }
#endif
}

void SdNotifier::notify_watchdog() {
#ifdef HAVE_SYSTEMD
    int r = sd_notify(0, "WATCHDOG=1");
    // 成功时不记录日志（避免每 15 秒刷屏）
    if (r < 0) {
        auto logger = spdlog::get("app");
        if (logger) logger->warn("sd_notify WATCHDOG=1 failed: {}", r);
    }
#endif
}

void SdNotifier::notify_stopping() {
#ifdef HAVE_SYSTEMD
    int r = sd_notify(0, "STOPPING=1");
    auto logger = spdlog::get("app");
    if (r < 0) {
        if (logger) logger->warn("sd_notify STOPPING=1 failed: {}", r);
    } else {
        if (logger) logger->info("sd_notify: STOPPING=1 sent");
    }
#endif
}

// --- 心跳线程 ---

void SdNotifier::start_watchdog_thread(int interval_sec) {
    if (s_running.load()) return;  // 已在运行，幂等忽略

    s_stop.store(false);
    s_thread = std::thread([interval_sec]() {
        auto logger = spdlog::get("app");
        if (logger) logger->info("watchdog thread started, interval={}s", interval_sec);
        try {
            std::unique_lock<std::mutex> lock(s_mtx);
            while (!s_stop.load()) {
                s_cv.wait_for(lock, std::chrono::seconds(interval_sec),
                              [] { return s_stop.load(); });
                if (!s_stop.load()) {
                    // 组合门控：health（FATAL）AND liveness（主循环活性）。
                    // 跳过原因的 warn 日志由 watchdog_gate_open() 内部区分输出。
                    if (watchdog_gate_open()) {
                        notify_watchdog();
                    }
                }
            }
        } catch (const std::exception& e) {
            auto lg = spdlog::get("app");
            if (lg) lg->error("watchdog thread exception: {}", e.what());
        } catch (...) {
            auto lg = spdlog::get("app");
            if (lg) lg->error("watchdog thread unknown exception");
        }
        if (logger) logger->info("watchdog thread stopped");
    });
    s_running.store(true);
}

void SdNotifier::stop_watchdog_thread() {
    if (!s_running.load()) return;  // 未运行，幂等忽略

    s_stop.store(true);
    s_cv.notify_one();  // 快速唤醒 wait_for
    if (s_thread.joinable()) {
        s_thread.join();
    }
    s_running.store(false);
}

bool SdNotifier::watchdog_running() {
    return s_running.load();
}

void SdNotifier::set_health_check(std::function<bool()> is_healthy) {
    std::lock_guard<std::mutex> lk(s_health_mtx);
    s_health_check = std::move(is_healthy);
}

bool SdNotifier::watchdog_gate_open() {
    // health 维度：既有逻辑不动（未注册视为健康）
    std::function<bool()> check;
    {
        std::lock_guard<std::mutex> lk(s_health_mtx);
        check = s_health_check;
    }
    const bool healthy = check ? check() : true;

    // liveness 维度：读原子时间戳 + 取 now，交给纯函数判定（Spec 32.5 缺陷 1）
    const int64_t last = s_liveness_ms.load(std::memory_order_relaxed);
    const int64_t threshold = s_stale_threshold_ms.load(std::memory_order_relaxed);
    const int64_t now = steady_now_ms();

    const bool feed = should_feed_watchdog(now, last, threshold, healthy);
    if (!feed) {
        auto logger = spdlog::get("app");
        if (logger) {
            if (!healthy) {
                logger->warn("watchdog: health gate closed, skipping WATCHDOG=1");
            } else {
                logger->warn(
                    "watchdog: liveness stale, skipping WATCHDOG=1 "
                    "(age={}ms threshold={}ms)",
                    now - last, threshold);
            }
        }
    }
    return feed;
}

// --- Spec 32.5 缺陷 1：liveness 原语 ---

void SdNotifier::refresh_liveness() {
    s_liveness_ms.store(steady_now_ms(), std::memory_order_relaxed);
}

bool SdNotifier::should_feed_watchdog(int64_t now_ms, int64_t last_liveness_ms,
                                      int64_t stale_threshold_ms, bool healthy) {
    if (!healthy) return false;          // FATAL 门控短路（spec-32 语义保留）
    if (last_liveness_ms == 0) return true;  // 未刷新哨兵：liveness 门控视为开
    return (now_ms - last_liveness_ms) <= stale_threshold_ms;
}

void SdNotifier::set_liveness_stale_threshold_ms(int64_t ms) {
    s_stale_threshold_ms.store(ms, std::memory_order_relaxed);
}

void SdNotifier::reset_liveness_for_test() {
    s_liveness_ms.store(0, std::memory_order_relaxed);
    s_stale_threshold_ms.store(10000, std::memory_order_relaxed);
}
