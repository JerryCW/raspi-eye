// sd_notifier_test.cpp
// SdNotifier 单元测试 + 属性测试（PBT）。
// macOS 上所有 notify 方法为 no-op，心跳线程正常运行。
#include "sd_notifier.h"
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>
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

// Spec 32 需求 4：看门狗健康门控
// 未注册 health_check 时默认门开（healthy）
TEST(SdNotifierTest, GateOpenWhenNoHealthCheck) {
    SdNotifier::set_health_check({});  // 清空
    EXPECT_TRUE(SdNotifier::watchdog_gate_open());
}

// 注册 health_check 返回 true（健康）-> 门开（发送心跳）
TEST(SdNotifierTest, GateOpenWhenHealthy) {
    SdNotifier::set_health_check([]() { return true; });
    EXPECT_TRUE(SdNotifier::watchdog_gate_open());
    SdNotifier::set_health_check({});  // 复位避免影响其他测试
}

// 注册 health_check 返回 false（FATAL）-> 门关（跳过心跳）
TEST(SdNotifierTest, GateClosedWhenUnhealthy) {
    SdNotifier::set_health_check([]() { return false; });
    EXPECT_FALSE(SdNotifier::watchdog_gate_open());
    SdNotifier::set_health_check({});  // 复位
}

// 门控查询会调用注册的回调（计数验证）
TEST(SdNotifierTest, GateQueriesHealthCheck) {
    int calls = 0;
    SdNotifier::set_health_check([&calls]() { ++calls; return true; });
    SdNotifier::watchdog_gate_open();
    SdNotifier::watchdog_gate_open();
    EXPECT_EQ(calls, 2);
    SdNotifier::set_health_check({});  // 复位
}

// ============================================================
// Spec 32.5 Task 4.3: 缺陷 1 单测 — should_feed_watchdog 边界 + 门控四象限
// ============================================================
// **Validates: Requirements 2.2, 2.3, 2.4, 2.5**（design Property 2/3）

// 边界：陈旧度恰好等于阈值 -> fresh（喂狗）
TEST(ShouldFeedWatchdogTest, ExactlyAtThresholdIsFresh) {
    // now - last == threshold（10000）：<= 判定为 fresh
    EXPECT_TRUE(SdNotifier::should_feed_watchdog(11000, 1000, 10000, true));
}

// 边界：超出阈值 1ms -> stale（跳过喂狗）
TEST(ShouldFeedWatchdogTest, OneMsPastThresholdIsStale) {
    EXPECT_FALSE(SdNotifier::should_feed_watchdog(11001, 1000, 10000, true));
}

// 哨兵：last == 0（从未刷新）-> liveness 门控视为开（向后兼容，design 决策点 2）
TEST(ShouldFeedWatchdogTest, SentinelZeroTreatedAsOpen) {
    EXPECT_TRUE(SdNotifier::should_feed_watchdog(999999999, 0, 10000, true));
}

// healthy=false 短路：无论 liveness fresh / stale / 哨兵，一律不喂
TEST(ShouldFeedWatchdogTest, UnhealthyShortCircuits) {
    EXPECT_FALSE(SdNotifier::should_feed_watchdog(1000, 1000, 10000, false));   // fresh
    EXPECT_FALSE(SdNotifier::should_feed_watchdog(99999, 1000, 10000, false));  // stale
    EXPECT_FALSE(SdNotifier::should_feed_watchdog(1000, 0, 10000, false));      // 哨兵
}

// 门控四象限枚举（design Property 3）：(healthy, fresh) ∈ {true,false}²，
// 喂狗 ⇔ healthy AND fresh。fresh/stale 用真实全局状态制造：
// refresh_liveness 后立即查询 = fresh；缩小阈值 + 等待越过 = stale。
TEST(SdNotifierTest, GateFourQuadrants) {
    // 象限 (healthy=true, fresh=true) -> 开
    SdNotifier::set_health_check([]() { return true; });
    SdNotifier::refresh_liveness();
    EXPECT_TRUE(SdNotifier::watchdog_gate_open());

    // 象限 (healthy=false, fresh=true) -> 关（FATAL 门控保留，spec-32 语义）
    SdNotifier::set_health_check([]() { return false; });
    SdNotifier::refresh_liveness();
    EXPECT_FALSE(SdNotifier::watchdog_gate_open());

    // 制造 stale：刷新后缩小阈值（50ms）并等待越过（120ms）
    SdNotifier::set_liveness_stale_threshold_ms(50);
    SdNotifier::refresh_liveness();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    // 象限 (healthy=true, fresh=false) -> 关（liveness stale，缺陷 1 修复点）
    SdNotifier::set_health_check([]() { return true; });
    EXPECT_FALSE(SdNotifier::watchdog_gate_open());

    // 象限 (healthy=false, fresh=false) -> 关
    SdNotifier::set_health_check([]() { return false; });
    EXPECT_FALSE(SdNotifier::watchdog_gate_open());

    // 复位全局状态（health + liveness 回哨兵/默认阈值），保证测试顺序无关
    SdNotifier::set_health_check({});
    SdNotifier::reset_liveness_for_test();
}

// ============================================================
// Spec 32.5 Task 2: Bug condition exploration test（缺陷 1 看门狗失明）
// ============================================================
// **Property 1: Bug Condition** — 主循环停摆时喂狗必停
// **Validates: Requirements 2.1, 2.2, 2.6**（spec-32.5 design Property 1/2/3）
//
// 场景建模（对应 2026-09-03 生产事故）：
// - health_check 恒返回 true：主线程死锁时状态机永远停在 RECOVERING
//   （非 FATAL），门控的 health 维度恒"健康"。
// - 主循环死锁 = liveness 时间戳停止刷新。未修复代码上根本不存在
//   liveness 刷新机制——这正是缺陷本身，故本测试不做任何刷新动作。
// - 断言编码 Expected Behavior：门控必须反映主循环活性，停摆时关闭
//   （喂狗必停），使 systemd WatchdogSec=30 在有限窗口内兜底重启。
//
// EXPECTED ON UNFIXED CODE: FAIL —— watchdog_gate_open() 仅查询
// health_check，恒返回 true。失败即确认缺陷 1 存在：门控只反映状态机
// 标记，不反映主循环活性（事故中 WATCHDOG=1 照发 2 天 8 小时）。
// [2026-09-04 已在未修复代码上运行并确认 FAIL，反例已归档]
//
// 修复后适配（task 4.4，断言本身不变）：design 决策点 2 规定"从未刷新
// （哨兵 0）视为门控开"，故死锁建模补齐 arrange 步骤——refresh_liveness()
// 一次（模拟主循环曾经存活）→ set_liveness_stale_threshold_ms 缩小阈值
// （100ms，避免测试真实等待 10s）→ 等待越过阈值（模拟刷新停止 = 主循环
// 死锁）。EXPECTED ON FIXED CODE: PASS（stale 判定生效、门控关闭）。
TEST(SdNotifierTest, ExplorationMainLoopStalledGateMustClose) {
    // Arrange: 状态机停在 RECOVERING（非 FATAL）——health 维度恒健康
    SdNotifier::set_health_check([]() { return true; });

    // Arrange: 主循环曾经存活（刷新一次，消除哨兵态），随后刷新停止 =
    // 主循环死锁。缩小阈值到 100ms 后等待 150ms 越过阈值。
    SdNotifier::refresh_liveness();
    SdNotifier::set_liveness_stale_threshold_ms(100);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Assert: 主循环停摆时喂狗门控必须关闭
    EXPECT_FALSE(SdNotifier::watchdog_gate_open())
        << "Bug condition confirmed: gate stays open while main loop is "
           "stalled (liveness never refreshed) and health state is "
           "non-FATAL (RECOVERING) -- WATCHDOG=1 keeps being sent, "
           "matching the 2d8h production incident.";

    // 复位全局状态：health 回调、阈值回默认 10000、liveness 回哨兵 0，
    // 保证 s_liveness_ms 全局状态不影响其他测试（顺序无关）
    SdNotifier::set_health_check({});
    SdNotifier::reset_liveness_for_test();
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

// ============================================================
// Spec 32.5 Task 3: Preservation property test（修复前基线快照）
// ============================================================
// **Property 2: Preservation** — 正常喂狗节律 / FATAL 门控不变
// **Validates: Requirements 3.1, 3.2, 3.6**
// （对应 spec-32.5 design Property 7 正常喂狗节律 + FATAL 门控快照）
//
// Observation-first（未修复代码上的既有行为快照，由既有 example tests 覆盖）：
// - health_check 未注册 → watchdog_gate_open() == true（GateOpenWhenNoHealthCheck）
// - 注册返回 true → true（GateOpenWhenHealthy）
// - 注册返回 false → false（GateClosedWhenUnhealthy）
//
// 本 PBT 泛化上述快照：随机 healthy 布尔序列逐个注册 health_check，
// 断言 watchdog_gate_open() == healthy 恒成立。刻意不触碰任何 liveness
// 接口——修复后依靠"未刷新哨兵 0 视为 liveness 门控开"的向后兼容语义
// （design 决策点 2），本测试不改断言必须继续全绿。
//
// EXPECTED ON UNFIXED CODE: PASS（确认待保留的基线行为）
RC_GTEST_PROP(SdNotifierPBT, PreservationGateEqualsHealthy, ()) {
    const auto seq = *rc::gen::arbitrary<std::vector<bool>>();

    for (const bool healthy : seq) {
        SdNotifier::set_health_check([healthy]() { return healthy; });
        const bool gate = SdNotifier::watchdog_gate_open();
        SdNotifier::set_health_check({});  // 先复位再断言，失败也不污染后续测试
        RC_ASSERT(gate == healthy);
    }
}

// ============================================================
// Spec 32.5 Task 4.3: 缺陷 1 RapidCheck PBT
// ============================================================

// ------------------------------------------------------------
// PBT-1（design Property 2）: liveness stale 判定纯函数性质
// 生成器：随机阈值 + 随机事件序列（刷新 / 时钟单调前进 delta 毫秒）
// 验证：(a) 阈值边界精确（feed ⇔ now-last <= threshold）；
//       (b) 无刷新时 fresh→stale 单向不可逆（时钟单调前进不会变回 fresh）；
//       (c) 任意 refresh 之后立即判定必为 fresh；
//       (d) 时钟全部参数注入，不依赖真实时钟与 systemd。
// **Validates: Requirements 2.1, 2.2, 2.4, 2.5**
// Feature: recovery-deadlock-watchdog-fix, Property 2: stale 判定纯函数性质
// ------------------------------------------------------------
RC_GTEST_PROP(SdNotifierPBT, LivenessFreshToStaleOneWay, ()) {
    const int threshold = *rc::gen::inRange(1, 5000);
    // 事件：(is_refresh, delta_ms)。is_refresh=true 刷新；否则时钟前进 delta
    const auto events = *rc::gen::container<std::vector<std::pair<bool, int>>>(
        rc::gen::pair(rc::gen::arbitrary<bool>(), rc::gen::inRange(1, 12000)));

    int64_t now = 1;   // 从 1 起步，避免刷新产生 last==0 与哨兵语义冲突
    int64_t last = now;  // 起始视为刚刷新过
    bool was_stale = false;

    for (const auto& ev : events) {
        if (ev.first) {
            last = now;  // refresh
            // (c) refresh 后立即判定必为 fresh
            RC_ASSERT(SdNotifier::should_feed_watchdog(now, last, threshold, true));
            was_stale = false;
        } else {
            now += ev.second;  // 时钟单调前进
            const bool feed =
                SdNotifier::should_feed_watchdog(now, last, threshold, true);
            // (a) 阈值边界精确
            RC_ASSERT(feed == ((now - last) <= threshold));
            // (b) fresh→stale 单向：一旦 stale 且无刷新，不会变回 fresh
            if (was_stale) {
                RC_ASSERT(!feed);
            }
            if (!feed) {
                was_stale = true;
            }
        }
    }
}

// ------------------------------------------------------------
// PBT-2（design Property 3/7）: 门控组合判定完整等价式
// 生成器：随机 (healthy, last[含哨兵 0], delta, threshold) 向量
// 验证：feed ⇔ healthy ∧ (now−last ≤ threshold ∨ last==0)
// **Validates: Requirements 2.2, 2.3**
// Feature: recovery-deadlock-watchdog-fix, Property 3: 门控组合四象限泛化
// ------------------------------------------------------------
RC_GTEST_PROP(SdNotifierPBT, FeedIffHealthyAndFreshOrSentinel, ()) {
    const bool healthy = *rc::gen::arbitrary<bool>();
    const bool sentinel = *rc::gen::arbitrary<bool>();
    const int64_t last =
        sentinel ? 0 : static_cast<int64_t>(*rc::gen::inRange(1, 1000000));
    const int64_t threshold = static_cast<int64_t>(*rc::gen::inRange(0, 20000));
    const int64_t delta = static_cast<int64_t>(*rc::gen::inRange(0, 40000));
    // 哨兵时 now 独立取值（保持 > 0）；否则 now = last + delta（时钟不回退）
    const int64_t now = (last == 0) ? (delta + 1) : (last + delta);

    const bool feed =
        SdNotifier::should_feed_watchdog(now, last, threshold, healthy);
    const bool expected =
        healthy && ((last == 0) || ((now - last) <= threshold));
    RC_ASSERT(feed == expected);
}
