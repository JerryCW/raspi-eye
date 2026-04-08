// smoke_test.cpp
// Smoke tests for PipelineManager — validates core lifecycle with fakesink.
#include "pipeline_manager.h"
#include <gtest/gtest.h>
#include <string>
#include <type_traits>

// --- Requirement 2.1, 6.2 ---
// Valid pipeline description returns a non-null PipelineManager.
TEST(PipelineManagerTest, CreateValidPipeline) {
    std::string err;
    auto pm = PipelineManager::create("videotestsrc ! fakesink", &err);
    ASSERT_NE(pm, nullptr) << "create() failed: " << err;
}

// --- Requirement 2.2, 6.2 ---
// Empty pipeline description returns nullptr with a non-empty error message.
TEST(PipelineManagerTest, CreateInvalidPipeline) {
    std::string err;
    auto pm = PipelineManager::create("", &err);
    EXPECT_EQ(pm, nullptr);
    EXPECT_FALSE(err.empty());
}

// --- Requirement 2.2 ---
// Unknown element in pipeline description returns nullptr or parse error.
TEST(PipelineManagerTest, CreateUnknownElement) {
    std::string err;
    auto pm = PipelineManager::create("nonexistent_element ! fakesink", &err);
    // gst_parse_launch may return a pipeline with a missing-plugin error,
    // or nullptr depending on GStreamer version. Either way the pipeline
    // should not be usable: nullptr OR start() fails.
    if (pm != nullptr) {
        bool started = pm->start(&err);
        EXPECT_FALSE(started) << "Pipeline with unknown element should not reach PLAYING";
    }
}

// --- Requirement 3.1, 6.3 ---
// After start(), current_state() reports GST_STATE_PLAYING.
TEST(PipelineManagerTest, StartPipeline) {
    std::string err;
    auto pm = PipelineManager::create("videotestsrc ! fakesink", &err);
    ASSERT_NE(pm, nullptr) << err;

    ASSERT_TRUE(pm->start(&err)) << "start() failed: " << err;
    EXPECT_EQ(pm->current_state(), GST_STATE_PLAYING);
}

// --- Requirement 3.2, 6.4 ---
// After stop(), resources are released (pipeline() returns nullptr).
TEST(PipelineManagerTest, StopPipeline) {
    std::string err;
    auto pm = PipelineManager::create("videotestsrc ! fakesink", &err);
    ASSERT_NE(pm, nullptr) << err;
    ASSERT_TRUE(pm->start(&err)) << err;

    pm->stop();
    EXPECT_EQ(pm->pipeline(), nullptr);
    EXPECT_EQ(pm->current_state(), GST_STATE_NULL);
}

// --- Requirement 3.4 ---
// Calling stop() twice is safe — no crash, no ASan report.
TEST(PipelineManagerTest, StopIdempotent) {
    std::string err;
    auto pm = PipelineManager::create("videotestsrc ! fakesink", &err);
    ASSERT_NE(pm, nullptr) << err;
    ASSERT_TRUE(pm->start(&err)) << err;

    pm->stop();
    pm->stop();  // second call — must be safe
    EXPECT_EQ(pm->pipeline(), nullptr);
}

// --- Requirement 4.1, 4.3, 6.5 ---
// RAII: pipeline created and started inside a scope; leaving scope triggers
// automatic cleanup. ASan will flag any leak or use-after-free.
TEST(PipelineManagerTest, RAIICleanup) {
    {
        std::string err;
        auto pm = PipelineManager::create("videotestsrc ! fakesink", &err);
        ASSERT_NE(pm, nullptr) << err;
        ASSERT_TRUE(pm->start(&err)) << err;
        // pm goes out of scope here — destructor calls stop()
    }
    // If ASan is enabled and there is a leak or double-free, the test
    // process will abort with a non-zero exit code, failing the test.
    SUCCEED();
}

// --- Requirement 4.2 ---
// PipelineManager must not be copy-constructible or copy-assignable.
TEST(PipelineManagerTest, NoCopy) {
    static_assert(!std::is_copy_constructible_v<PipelineManager>,
                  "PipelineManager must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<PipelineManager>,
                  "PipelineManager must not be copy-assignable");
    SUCCEED();
}
