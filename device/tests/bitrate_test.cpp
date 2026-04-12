// bitrate_test.cpp
// BitrateAdapter example-based tests + PBT properties.
// Custom main(): gst_init required before any GStreamer API calls.
#include "bitrate_adapter.h"
#include "stream_mode_controller.h"

#include <gst/gst.h>
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

// ---------------------------------------------------------------------------
// Example-based tests
// ---------------------------------------------------------------------------

// 1. ComputeNextBitrate_UnhealthyDecreases
//    Validates: Requirement 3.2
TEST(BitrateTest, ComputeNextBitrate_UnhealthyDecreases) {
    BitrateConfig config;
    // From default 2500, UNHEALTHY should decrease by step (500) to 2000
    int result = compute_next_bitrate(config.default_kbps,
                                      BranchStatus::UNHEALTHY,
                                      false, config);
    EXPECT_EQ(result, config.default_kbps - config.step_kbps);
}

// 2. ComputeNextBitrate_HealthyRampup
//    Validates: Requirement 3.3
TEST(BitrateTest, ComputeNextBitrate_HealthyRampup) {
    BitrateConfig config;
    // From default 2500, HEALTHY + rampup_eligible should increase by step to 3000
    int result = compute_next_bitrate(config.default_kbps,
                                      BranchStatus::HEALTHY,
                                      true, config);
    EXPECT_EQ(result, config.default_kbps + config.step_kbps);
}

// 3. ComputeNextBitrate_DegradedForcesMin
//    Validates: Requirement 3.4
TEST(BitrateTest, ComputeNextBitrate_DegradedForcesMin) {
    // Use BitrateAdapter with nullptr pipeline (no GStreamer elements needed
    // for this test -- apply_bitrate will log a warning and skip).
    BitrateConfig config;
    BitrateAdapter adapter(nullptr, config);

    // Initial bitrate should be default (2500)
    EXPECT_EQ(adapter.current_bitrate_kbps(), config.default_kbps);

    // Trigger DEGRADED mode via on_mode_changed
    adapter.on_mode_changed(StreamMode::FULL, StreamMode::DEGRADED);

    // After DEGRADED, bitrate must be forced to min_kbps
    EXPECT_EQ(adapter.current_bitrate_kbps(), config.min_kbps);
}

// 4. ComputeNextBitrate_ClampToRange
//    Validates: Requirement 3.1
TEST(BitrateTest, ComputeNextBitrate_ClampToRange) {
    BitrateConfig config;

    // At min, UNHEALTHY should not go below min
    int at_min = compute_next_bitrate(config.min_kbps,
                                      BranchStatus::UNHEALTHY,
                                      false, config);
    EXPECT_EQ(at_min, config.min_kbps);

    // At max, HEALTHY + rampup should not exceed max
    int at_max = compute_next_bitrate(config.max_kbps,
                                      BranchStatus::HEALTHY,
                                      true, config);
    EXPECT_EQ(at_max, config.max_kbps);
}

// 5. InitialBitrate_IsDefault
//    Validates: Requirement 7.5
TEST(BitrateTest, InitialBitrate_IsDefault) {
    BitrateConfig config;
    BitrateAdapter adapter(nullptr, config);
    EXPECT_EQ(adapter.current_bitrate_kbps(), config.default_kbps);
}

// ---------------------------------------------------------------------------
// Property-Based Tests (RapidCheck)
// ---------------------------------------------------------------------------

// Feature: spec-15-adaptive-streaming, Property 4: Bitrate range invariant
// **Validates: Requirements 3.1, 3.2, 3.3, 3.4**
RC_GTEST_PROP(BitratePBT, BitrateRangeInvariant, ()) {
    BitrateConfig config;
    int bitrate = config.default_kbps;

    // Generate a random sequence of health events (length 1..50)
    auto events = *rc::gen::container<std::vector<std::pair<BranchStatus, bool>>>(
        rc::gen::pair(
            rc::gen::element(BranchStatus::HEALTHY, BranchStatus::UNHEALTHY),
            rc::gen::arbitrary<bool>()));

    RC_PRE(!events.empty());

    for (const auto& [status, rampup] : events) {
        int prev = bitrate;
        bitrate = compute_next_bitrate(bitrate, status, rampup, config);

        // Invariant 1: min <= bitrate <= max
        RC_ASSERT(bitrate >= config.min_kbps);
        RC_ASSERT(bitrate <= config.max_kbps);

        // Invariant 2: (bitrate - min) % step == 0
        RC_ASSERT((bitrate - config.min_kbps) % config.step_kbps == 0);

        // Invariant 3: UNHEALTHY -> bitrate <= previous
        if (status == BranchStatus::UNHEALTHY) {
            RC_ASSERT(bitrate <= prev);
        }

        // Invariant 4: HEALTHY + rampup -> bitrate >= previous
        if (status == BranchStatus::HEALTHY && rampup) {
            RC_ASSERT(bitrate >= prev);
        }
    }
}

// ---------------------------------------------------------------------------
// Custom main: gst_init required before any GStreamer API calls
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
