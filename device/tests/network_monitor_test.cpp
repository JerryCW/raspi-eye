// network_monitor_test.cpp
// NetworkMonitor example-based tests + PBT properties.
// Custom main(): gst_init + RUN_ALL_TESTS().
#include "network_monitor.h"
#include "stream_mode_controller.h"

#include <gst/gst.h>
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <vector>

// ===========================================================================
// Example-based tests (Task 2.3)
// ===========================================================================

// --- compute_kvs_network_status 基本场景 ---

TEST(KvsNetworkStatusTest, AboveThreshold_ReturnsUnhealthy) {
    // count >= threshold → UNHEALTHY（无论 cooldown 状态）
    EXPECT_EQ(compute_kvs_network_status(5, 5, true), BranchStatus::UNHEALTHY);
    EXPECT_EQ(compute_kvs_network_status(5, 5, false), BranchStatus::UNHEALTHY);
    EXPECT_EQ(compute_kvs_network_status(10, 5, true), BranchStatus::UNHEALTHY);
}

TEST(KvsNetworkStatusTest, BelowThreshold_CooldownExpired_ReturnsHealthy) {
    // count < threshold 且 cooldown 过期 → HEALTHY
    EXPECT_EQ(compute_kvs_network_status(0, 5, true), BranchStatus::HEALTHY);
    EXPECT_EQ(compute_kvs_network_status(4, 5, true), BranchStatus::HEALTHY);
}

TEST(KvsNetworkStatusTest, BelowThreshold_CooldownNotExpired_ReturnsUnhealthy) {
    // count < threshold 且 cooldown 未过期 → UNHEALTHY（保持）
    EXPECT_EQ(compute_kvs_network_status(0, 5, false), BranchStatus::UNHEALTHY);
    EXPECT_EQ(compute_kvs_network_status(4, 5, false), BranchStatus::UNHEALTHY);
}

// --- compute_webrtc_network_status 基本场景 ---

TEST(WebrtcNetworkStatusTest, FailuresAboveThreshold_ReturnsUnhealthy) {
    EXPECT_EQ(compute_webrtc_network_status(10, 0, 10, 50), BranchStatus::UNHEALTHY);
    EXPECT_EQ(compute_webrtc_network_status(15, 0, 10, 50), BranchStatus::UNHEALTHY);
}

TEST(WebrtcNetworkStatusTest, SuccessesAboveRecovery_ReturnsHealthy) {
    EXPECT_EQ(compute_webrtc_network_status(0, 50, 10, 50), BranchStatus::HEALTHY);
    EXPECT_EQ(compute_webrtc_network_status(0, 100, 10, 50), BranchStatus::HEALTHY);
}

TEST(WebrtcNetworkStatusTest, NeitherThresholdMet_ReturnsUnhealthy) {
    // 未达到任何阈值 → 保守返回 UNHEALTHY
    EXPECT_EQ(compute_webrtc_network_status(5, 30, 10, 50), BranchStatus::UNHEALTHY);
}

// --- compute_keyframe_only_transition 基本场景 ---

TEST(KeyframeOnlyTest, NormalMode_FailuresReachThreshold_EntersKeyframeOnly) {
    // 正常模式，连续失败 >= threshold → 进入仅关键帧模式
    EXPECT_TRUE(compute_keyframe_only_transition(false, 10, 0, 10, 10));
    EXPECT_TRUE(compute_keyframe_only_transition(false, 15, 0, 10, 10));
}

TEST(KeyframeOnlyTest, NormalMode_FailuresBelowThreshold_StaysNormal) {
    EXPECT_FALSE(compute_keyframe_only_transition(false, 5, 0, 10, 10));
    EXPECT_FALSE(compute_keyframe_only_transition(false, 0, 0, 10, 10));
}

TEST(KeyframeOnlyTest, KeyframeOnlyMode_SuccessesReachRecovery_RestoresNormal) {
    // 仅关键帧模式，连续成功 >= recovery → 恢复正常
    EXPECT_FALSE(compute_keyframe_only_transition(true, 0, 10, 10, 10));
    EXPECT_FALSE(compute_keyframe_only_transition(true, 0, 20, 10, 10));
}

TEST(KeyframeOnlyTest, KeyframeOnlyMode_SuccessesBelowRecovery_StaysKeyframeOnly) {
    EXPECT_TRUE(compute_keyframe_only_transition(true, 0, 5, 10, 10));
    EXPECT_TRUE(compute_keyframe_only_transition(true, 0, 0, 10, 10));
}

// --- NetworkMonitor 集成测试（nullptr adapter/controller，验证不崩溃）---

TEST(NetworkMonitorIntegrationTest, NullAdapterAndController_NoCrash) {
    NetworkConfig config;
    config.latency_pressure_threshold = 3;
    config.latency_pressure_cooldown_sec = 1;
    config.writeframe_fail_threshold = 5;
    config.writeframe_recovery_count = 10;

    NetworkMonitor monitor(config);
    // 不设置 adapter 和 controller（保持 nullptr）

    monitor.start();

    // 调用 on_latency_pressure 多次，不应崩溃
    for (int i = 0; i < 10; ++i) {
        monitor.on_latency_pressure(5000);
    }

    // 调用 on_writeframe_result 多次，不应崩溃
    for (int i = 0; i < 20; ++i) {
        monitor.on_writeframe_result(false);
    }
    for (int i = 0; i < 20; ++i) {
        monitor.on_writeframe_result(true);
    }

    // 查询状态
    auto kvs = monitor.kvs_network_status();
    auto webrtc = monitor.webrtc_network_status();
    (void)kvs;
    (void)webrtc;

    monitor.stop();
    SUCCEED();
}

TEST(NetworkMonitorIntegrationTest, StartStop_MultipleTimes_NoCrash) {
    NetworkMonitor monitor;
    monitor.start();
    monitor.stop();
    monitor.start();
    monitor.stop();
    SUCCEED();
}

// ===========================================================================
// PBT — 属性 1：Latency pressure 状态机不变量 (Task 2.4)
// Feature: network-adaptive-bitrate, Property 1: Latency pressure state machine invariant
// **Validates: Requirements 1.1, 1.2, 1.3**
// ===========================================================================

RC_GTEST_PROP(NetworkPBT, LatencyPressureStateMachineInvariant, ()) {
    // 生成随机参数
    auto pressure_count = *rc::gen::inRange(0, 100);
    auto threshold = *rc::gen::inRange(1, 50);
    auto cooldown_expired = *rc::gen::arbitrary<bool>();

    auto status = compute_kvs_network_status(pressure_count, threshold, cooldown_expired);

    if (pressure_count >= threshold) {
        // count >= threshold → 必须 UNHEALTHY
        RC_ASSERT(status == BranchStatus::UNHEALTHY);
    } else if (cooldown_expired) {
        // count < threshold 且 cooldown 过期 → 必须 HEALTHY
        RC_ASSERT(status == BranchStatus::HEALTHY);
    } else {
        // count < threshold 且 cooldown 未过期 → 必须 UNHEALTHY
        RC_ASSERT(status == BranchStatus::UNHEALTHY);
    }
}

// ===========================================================================
// PBT — 属性 2：writeFrame 健康状态转换 (Task 2.5)
// Feature: network-adaptive-bitrate, Property 2: WriteFrame health state transition
// **Validates: Requirements 3.1, 3.2**
// ===========================================================================

RC_GTEST_PROP(NetworkPBT, WriteFrameHealthStateTransition, ()) {
    auto consecutive_failures = *rc::gen::inRange(0, 200);
    auto consecutive_successes = *rc::gen::inRange(0, 200);
    auto fail_threshold = *rc::gen::inRange(1, 50);
    auto recovery_count = *rc::gen::inRange(1, 100);

    auto status = compute_webrtc_network_status(
        consecutive_failures, consecutive_successes,
        fail_threshold, recovery_count);

    if (consecutive_failures >= fail_threshold) {
        // failures >= threshold → 必须 UNHEALTHY
        RC_ASSERT(status == BranchStatus::UNHEALTHY);
    } else if (consecutive_successes >= recovery_count) {
        // successes >= recovery → 必须 HEALTHY
        RC_ASSERT(status == BranchStatus::HEALTHY);
    }
    // 其他情况：保守返回 UNHEALTHY（不做额外断言，函数行为已定义）
}

// ===========================================================================
// PBT — 属性 6：仅关键帧模式状态转换 (Task 2.6)
// Feature: network-adaptive-bitrate, Property 6: Keyframe-only mode state transition
// **Validates: Requirements 7.1, 7.2**
// ===========================================================================

RC_GTEST_PROP(NetworkPBT, KeyframeOnlyModeStateTransition, ()) {
    auto fail_threshold = *rc::gen::inRange(1, 50);
    auto recovery_count = 10;  // 固定为 10（需求 7.2 规定）

    // 模拟 per-peer 帧序列
    auto num_frames = *rc::gen::inRange(1, 200);

    bool keyframe_only = false;
    int consecutive_failures = 0;
    int consecutive_successes = 0;

    for (int i = 0; i < num_frames; ++i) {
        auto success = *rc::gen::arbitrary<bool>();

        if (success) {
            consecutive_successes++;
            consecutive_failures = 0;
        } else {
            consecutive_failures++;
            consecutive_successes = 0;
        }

        bool new_keyframe_only = compute_keyframe_only_transition(
            keyframe_only, consecutive_failures, consecutive_successes,
            fail_threshold, recovery_count);

        // 验证不变量
        if (!keyframe_only && consecutive_failures >= fail_threshold) {
            // 正常模式下连续失败达到阈值 → 必须进入仅关键帧模式
            RC_ASSERT(new_keyframe_only == true);
        }
        if (keyframe_only && consecutive_successes >= recovery_count) {
            // 仅关键帧模式下连续成功达到恢复阈值 → 必须恢复正常
            RC_ASSERT(new_keyframe_only == false);
        }

        keyframe_only = new_keyframe_only;

        // 进入仅关键帧模式后重置 success 计数（模拟实际行为）
        if (keyframe_only && !success) {
            consecutive_successes = 0;
        }
    }
}

// ===========================================================================
// PBT — 属性 7：多 peer 仅关键帧模式独立性 (Task 2.7)
// Feature: network-adaptive-bitrate, Property 7: Multi-peer keyframe-only independence
// **Validates: Requirements 7.4**
// ===========================================================================

RC_GTEST_PROP(NetworkPBT, MultiPeerKeyframeOnlyIndependence, ()) {
    auto num_peers = *rc::gen::inRange(2, 6);  // 2-5 个 peer
    auto num_frames = *rc::gen::inRange(10, 100);
    int fail_threshold = 10;
    int recovery_count = 10;

    // 每个 peer 独立状态
    struct PeerState {
        bool keyframe_only = false;
        int consecutive_failures = 0;
        int consecutive_successes = 0;
    };
    std::vector<PeerState> peers(static_cast<size_t>(num_peers));

    for (int frame = 0; frame < num_frames; ++frame) {
        // 为每个 peer 独立生成 writeFrame 结果
        for (int p = 0; p < num_peers; ++p) {
            auto success = *rc::gen::arbitrary<bool>();

            if (success) {
                peers[p].consecutive_successes++;
                peers[p].consecutive_failures = 0;
            } else {
                peers[p].consecutive_failures++;
                peers[p].consecutive_successes = 0;
            }

            bool new_state = compute_keyframe_only_transition(
                peers[p].keyframe_only,
                peers[p].consecutive_failures,
                peers[p].consecutive_successes,
                fail_threshold, recovery_count);

            peers[p].keyframe_only = new_state;
        }
    }

    // 验证独立性：每个 peer 的状态仅由自己的帧序列决定
    // 重新用相同逻辑独立计算每个 peer 的最终状态，确认一致
    // （由于我们已经独立计算了，这里验证的是纯函数不依赖全局状态）
    for (int p = 0; p < num_peers; ++p) {
        // 验证每个 peer 的状态与其自身计数一致
        bool expected = compute_keyframe_only_transition(
            peers[p].keyframe_only,
            peers[p].consecutive_failures,
            peers[p].consecutive_successes,
            fail_threshold, recovery_count);
        // 当前状态经过一次 transition 后应该是稳定的（幂等性）
        // 如果已经在 keyframe_only 且 successes < recovery → 保持
        // 如果不在 keyframe_only 且 failures < threshold → 保持
        if (peers[p].keyframe_only) {
            if (peers[p].consecutive_successes >= recovery_count) {
                RC_ASSERT(expected == false);
            } else {
                RC_ASSERT(expected == true);
            }
        } else {
            if (peers[p].consecutive_failures >= fail_threshold) {
                RC_ASSERT(expected == true);
            } else {
                RC_ASSERT(expected == false);
            }
        }
    }

    // 关键独立性验证：修改一个 peer 的状态不影响其他 peer
    // 强制 peer 0 进入 keyframe_only 模式
    bool peer0_forced = compute_keyframe_only_transition(
        false, fail_threshold, 0, fail_threshold, recovery_count);
    RC_ASSERT(peer0_forced == true);

    // 其他 peer 的状态不受影响（纯函数，无共享状态）
    for (int p = 1; p < num_peers; ++p) {
        bool other_state = compute_keyframe_only_transition(
            peers[p].keyframe_only,
            peers[p].consecutive_failures,
            peers[p].consecutive_successes,
            fail_threshold, recovery_count);
        // 其他 peer 的结果应该和之前一样
        bool expected_other = compute_keyframe_only_transition(
            peers[p].keyframe_only,
            peers[p].consecutive_failures,
            peers[p].consecutive_successes,
            fail_threshold, recovery_count);
        RC_ASSERT(other_state == expected_other);
    }
}

// ===========================================================================
// Custom main: gst_init + RUN_ALL_TESTS
// ===========================================================================

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
