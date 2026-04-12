// sd_notifier_test.cpp
// SdNotifier 单元测试 + 属性测试（PBT）。
// macOS 上所有 notify 方法为 no-op，心跳线程正常运行。
#include "sd_notifier.h"
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <chrono>
#include <functional>
#include <vector>

// ============================================================
// Example-based tests（任务 5.1）
// ============================================================

// notify_ready 不崩溃（macOS no-op 正常返回）
// 验证需求: 2.1, 2.6
TEST(SdNotifierTest, NotifyReadyNoCrash) {
    EXPECT_NO_THROW(SdNotifier::notify_ready());
}

// notify_watchdog 不崩溃
// 验证需求: 2.2, 2.6
TEST(SdNotifierTest, NotifyWatchdogNoCrash) {
    EXPECT_NO_THROW(SdNotifier::notify_watchdog());
}

// notify_stopping 不崩溃
// 验证需求: 2.3, 2.6
TEST(SdNotifierTest, NotifyStoppingNoCrash) {
    EXPECT_NO_THROW(SdNotifier::notify_stopping());
}

// 初始状态 watchdog_running == false
// 验证需求: 2.4, 2.5
TEST(SdNotifierTest, InitialWatchdogNotRunning) {
    EXPECT_FALSE(SdNotifier::watchdog_running());
}

// stop 未运行的线程不崩溃（幂等性）
// 验证需求: 2.5
TEST(SdNotifierTest, StopIdleNoCrash) {
    EXPECT_NO_THROW(SdNotifier::stop_watchdog_thread());
    EXPECT_FALSE(SdNotifier::watchdog_running());
}

// 短间隔启动心跳线程后验证 running 状态
// 验证需求: 2.4, 2.5
TEST(SdNotifierTest, WatchdogThreadRuns) {
    SdNotifier::start_watchdog_thread(1);
    EXPECT_TRUE(SdNotifier::watchdog_running());
    SdNotifier::stop_watchdog_thread();
    EXPECT_FALSE(SdNotifier::watchdog_running());
}


// ============================================================
// Property-based tests（任务 5.3 - 5.6）
// ============================================================

// ------------------------------------------------------------
// Property 1: 心跳线程 start/stop 往返一致性（任务 5.3）
// 生成器：随机正整数 interval_sec ∈ [1, 60]
// 验证：start 后 running==true，stop 后 running==false
// **Validates: Requirements 2.4, 2.5, 3.1, 3.3**
// Feature: systemd-watchdog, Property 1: start/stop round-trip consistency
// ------------------------------------------------------------
RC_GTEST_PROP(SdNotifierPBT, StartStopRoundTrip, ()) {
    const auto interval = *rc::gen::inRange(1, 61);

    SdNotifier::start_watchdog_thread(interval);
    RC_ASSERT(SdNotifier::watchdog_running() == true);

    SdNotifier::stop_watchdog_thread();
    RC_ASSERT(SdNotifier::watchdog_running() == false);
}

// ------------------------------------------------------------
// Property 2: stop_watchdog_thread 快速响应（任务 5.4）
// 生成器：随机正整数 interval_sec ∈ [1, 60]
// 验证：start 后立即 stop，stop 在 1 秒内返回
// **Validates: Requirements 3.5**
// Feature: systemd-watchdog, Property 2: stop_watchdog_thread fast response
// ------------------------------------------------------------
RC_GTEST_PROP(SdNotifierPBT, StopFastResponse, ()) {
    const auto interval = *rc::gen::inRange(1, 61);

    SdNotifier::start_watchdog_thread(interval);

    const auto start = std::chrono::steady_clock::now();
    SdNotifier::stop_watchdog_thread();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // stop 应在 1 秒内返回（证明使用了 condition_variable::wait_for 而非 sleep_for）
    RC_ASSERT(elapsed_ms < 1000);
    RC_ASSERT(SdNotifier::watchdog_running() == false);
}

// ------------------------------------------------------------
// Property 3: start_watchdog_thread 幂等性（任务 5.5）
// 生成器：随机正整数 interval_sec ∈ [1, 60]
// 验证：连续两次 start 不崩溃，running==true，单次 stop 即可停止
// **Validates: Requirements 2.4**
// Feature: systemd-watchdog, Property 3: start_watchdog_thread idempotency
// ------------------------------------------------------------
RC_GTEST_PROP(SdNotifierPBT, StartIdempotent, ()) {
    const auto interval = *rc::gen::inRange(1, 61);

    // 连续两次 start 不崩溃
    SdNotifier::start_watchdog_thread(interval);
    SdNotifier::start_watchdog_thread(interval);

    RC_ASSERT(SdNotifier::watchdog_running() == true);

    // 单次 stop 即可停止
    SdNotifier::stop_watchdog_thread();
    RC_ASSERT(SdNotifier::watchdog_running() == false);
}

// ------------------------------------------------------------
// Property 4: notify 方法不崩溃（任务 5.6）
// 生成器：随机调用序列（1-10 次，从 notify_ready/notify_watchdog/notify_stopping 中随机选择）
// 验证：所有调用正常返回，不抛异常
// **Validates: Requirements 2.1, 2.2, 2.3, 2.6, 2.9**
// Feature: systemd-watchdog, Property 4: notify methods no crash
// ------------------------------------------------------------
RC_GTEST_PROP(SdNotifierPBT, NotifyNoCrash, ()) {
    const auto count = *rc::gen::inRange(1, 11);

    const std::vector<std::function<void()>> methods = {
        SdNotifier::notify_ready,
        SdNotifier::notify_watchdog,
        SdNotifier::notify_stopping,
    };

    for (int i = 0; i < count; ++i) {
        const auto idx = *rc::gen::inRange(0, static_cast<int>(methods.size()));
        methods[idx]();  // 不抛异常即通过
    }
}
