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
                    // 健康门控：非健康（FATAL）时跳过心跳，让 systemd WatchdogSec 兜底
                    if (watchdog_gate_open()) {
                        notify_watchdog();
                    } else {
                        if (logger) logger->warn("watchdog: health gate closed, skipping WATCHDOG=1");
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
    std::function<bool()> check;
    {
        std::lock_guard<std::mutex> lk(s_health_mtx);
        check = s_health_check;
    }
    if (!check) return true;  // 未注册：默认健康
    return check();
}
