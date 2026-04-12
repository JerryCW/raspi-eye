// shutdown_test.cpp
// ShutdownHandler tests: example-based + PBT.
#include "shutdown_handler.h"
#include "app_context.h"
#include "log_init.h"
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

// ============================================================
// Global test environment: init/shutdown logging once
// ============================================================

class LogEnvironment : public ::testing::Environment {
public:
    void SetUp() override { log_init::init(); }
    void TearDown() override { log_init::shutdown(); }
};

static ::testing::Environment* const log_env =
    ::testing::AddGlobalTestEnvironment(new LogEnvironment);

// ============================================================
// Example-based tests
// ============================================================

TEST(ShutdownHandlerTest, TimeoutProtection) {
    ShutdownHandler handler;
    std::atomic<bool> normal_executed{false};

    // sleep step registered first (index 0), normal step second (index 1)
    // Reverse execution: normal (index 1) runs first, sleep (index 0) runs second
    handler.register_step("slow", []() {
        std::this_thread::sleep_for(std::chrono::seconds(6));
    });
    handler.register_step("normal", [&normal_executed]() {
        normal_executed = true;
    });

    handler.execute();
    EXPECT_TRUE(normal_executed.load());
}

TEST(ShutdownHandlerTest, EmptySteps) {
    ShutdownHandler handler;
    // Should not crash
    handler.execute();
}

TEST(ShutdownHandlerTest, NoCopy) {
    static_assert(!std::is_copy_constructible_v<ShutdownHandler>,
                  "ShutdownHandler must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<ShutdownHandler>,
                  "ShutdownHandler must not be copy-assignable");
    static_assert(!std::is_copy_constructible_v<AppContext>,
                  "AppContext must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<AppContext>,
                  "AppContext must not be copy-assignable");
}

// ============================================================
// Property 1: Reverse execution order invariant
// **Validates: Requirements 4.2**
// ============================================================

RC_GTEST_PROP(ShutdownPBT, ReverseOrderInvariant, ()) {
    const auto count = *rc::gen::inRange(1, 21);

    ShutdownHandler handler;
    std::vector<int> execution_order;
    std::mutex mtx;

    for (int i = 0; i < count; ++i) {
        handler.register_step("step_" + std::to_string(i),
                              [i, &execution_order, &mtx]() {
            std::lock_guard<std::mutex> lock(mtx);
            execution_order.push_back(i);
        });
    }

    handler.execute();

    RC_ASSERT(static_cast<int>(execution_order.size()) == count);
    for (int i = 0; i < count; ++i) {
        RC_ASSERT(execution_order[i] == count - 1 - i);
    }
}

// ============================================================
// Property 2: Exception isolation invariant
// **Validates: Requirements 4.4**
// ============================================================

RC_GTEST_PROP(ShutdownPBT, ExceptionIsolationInvariant, ()) {
    const auto count = *rc::gen::inRange(2, 11);
    auto exception_indices = *rc::gen::unique<std::vector<int>>(
        rc::gen::inRange(0, count));
    // Ensure at least one non-exception step
    if (static_cast<int>(exception_indices.size()) >= count) {
        exception_indices.pop_back();
    }
    std::set<int> exception_set(exception_indices.begin(),
                                exception_indices.end());

    ShutdownHandler handler;
    std::vector<int> execution_order;
    std::mutex mtx;

    for (int i = 0; i < count; ++i) {
        if (exception_set.count(i)) {
            handler.register_step("throw_" + std::to_string(i), []() {
                throw std::runtime_error("test exception");
            });
        } else {
            handler.register_step("ok_" + std::to_string(i),
                                  [i, &execution_order, &mtx]() {
                std::lock_guard<std::mutex> lock(mtx);
                execution_order.push_back(i);
            });
        }
    }

    handler.execute();

    // All non-exception steps executed in reverse registration order
    std::vector<int> expected_non_exception;
    for (int i = count - 1; i >= 0; --i) {
        if (!exception_set.count(i)) {
            expected_non_exception.push_back(i);
        }
    }
    RC_ASSERT(execution_order == expected_non_exception);
}
