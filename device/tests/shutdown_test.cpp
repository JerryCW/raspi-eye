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
// Bug Condition 探索性测试
// 验证 std::async + std::future 析构阻塞的 bug
// 在未修复代码上预期 FAIL（execute() 耗时约 8 秒）
// **Validates: Requirements 1.1, 1.5**
// ============================================================

TEST(ShutdownHandlerTest, BugCondition_TimeoutStepBlocks) {
    ShutdownHandler handler;
    std::atomic<bool> normal_executed{false};

    // sleep step 注册在 index 0（超过 kStepTimeout=5s）
    handler.register_step("slow_8s", []() {
        std::this_thread::sleep_for(std::chrono::seconds(8));
    });

    // normal step 注册在 index 1
    handler.register_step("normal", [&normal_executed]() {
        normal_executed = true;
    });

    // 逆序执行：normal (index 1) 先执行，slow_8s (index 0) 后执行
    const auto start = std::chrono::steady_clock::now();
    handler.execute();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_s =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        / 1000.0;

    // 断言 1：正常 step 应被执行（逆序执行，normal 先于 slow_8s）
    EXPECT_TRUE(normal_executed.load())
        << "normal step should have executed (runs before slow step in reverse order)";

    // 断言 2：execute() 总耗时应 ≤ kStepTimeout + 1s tolerance（约 6 秒）
    // 未修复代码上：std::future 析构阻塞等待 8 秒，此断言 FAIL
    // 修复后：超时 step 被 detach 跳过，此断言 PASS
    constexpr double kMaxAllowedSeconds = 6.0;  // kStepTimeout(5s) + 1s tolerance
    EXPECT_LE(elapsed_s, kMaxAllowedSeconds)
        << "execute() took " << elapsed_s << "s, expected <= " << kMaxAllowedSeconds
        << "s. If this fails, std::future destructor is blocking (bug confirmed).";
}

// ============================================================
// Property 1: Reverse execution order invariant
// **Validates: Requirements 4.2**
// ============================================================

RC_GTEST_PROP(ShutdownPBT, ReverseOrderInvariant, ()) {
    const auto count = *rc::gen::inRange(1, 8);

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
    const auto count = *rc::gen::inRange(2, 6);
    // 为每个 step 随机决定是否抛异常，确保至少一个非异常 step
    std::set<int> exception_set;
    for (int i = 0; i < count; ++i) {
        if (*rc::gen::inRange(0, 2) == 1) {
            exception_set.insert(i);
        }
    }
    if (static_cast<int>(exception_set.size()) >= count) {
        exception_set.erase(exception_set.begin());
    }

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

// ============================================================
// Preservation Property: 基于 ShutdownSummary 返回值的增强测试
// 在未修复代码上运行——预期 PASS（非超时场景下旧实现行为正确）
// ============================================================

// ============================================================
// Preservation PBT 1: 逆序执行 + ShutdownSummary 验证
// **Validates: Requirements 3.1, 3.4**
// ============================================================

RC_GTEST_PROP(ShutdownPBT, Preservation_ReverseOrderWithSummary, ()) {
    const auto count = *rc::gen::inRange(1, 8);

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

    auto summary = handler.execute();

    // 验证 steps 数量与注册数量一致
    RC_ASSERT(static_cast<int>(summary.steps.size()) == count);

    // 验证 ShutdownSummary 中 step 按逆序排列
    for (int i = 0; i < count; ++i) {
        RC_ASSERT(summary.steps[i].name == "step_" + std::to_string(count - 1 - i));
    }

    // 验证所有 step 状态为 OK
    for (const auto& step : summary.steps) {
        RC_ASSERT(step.status == StepStatus::OK);
        RC_ASSERT(step.duration_ms >= 0);
    }

    // 验证实际执行顺序也是逆序
    RC_ASSERT(static_cast<int>(execution_order.size()) == count);
    for (int i = 0; i < count; ++i) {
        RC_ASSERT(execution_order[i] == count - 1 - i);
    }

    // 验证 total_duration_ms 合理
    RC_ASSERT(summary.total_duration_ms >= 0);
}

// ============================================================
// Preservation PBT 2: 异常隔离 + ShutdownSummary 验证
// **Validates: Requirements 3.2, 3.4**
// ============================================================

RC_GTEST_PROP(ShutdownPBT, Preservation_ExceptionIsolationWithSummary, ()) {
    const auto count = *rc::gen::inRange(2, 6);
    // 为每个 step 随机决定是否抛异常，确保至少一个非异常 step
    std::set<int> exception_set;
    for (int i = 0; i < count; ++i) {
        if (*rc::gen::inRange(0, 2) == 1) {
            exception_set.insert(i);
        }
    }
    if (static_cast<int>(exception_set.size()) >= count) {
        exception_set.erase(exception_set.begin());
    }

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

    auto summary = handler.execute();

    // 验证 steps 数量与注册数量一致
    RC_ASSERT(static_cast<int>(summary.steps.size()) == count);

    // 验证 ShutdownSummary 中 step 按逆序排列
    for (int i = 0; i < count; ++i) {
        int reg_index = count - 1 - i;
        if (exception_set.count(reg_index)) {
            RC_ASSERT(summary.steps[i].name == "throw_" + std::to_string(reg_index));
            // 异常 step 状态为 EXCEPTION
            RC_ASSERT(summary.steps[i].status == StepStatus::EXCEPTION);
        } else {
            RC_ASSERT(summary.steps[i].name == "ok_" + std::to_string(reg_index));
            // 非异常 step 状态为 OK
            RC_ASSERT(summary.steps[i].status == StepStatus::OK);
        }
    }

    // 验证实际执行顺序：非异常 step 按逆序执行
    std::vector<int> expected_non_exception;
    for (int i = count - 1; i >= 0; --i) {
        if (!exception_set.count(i)) {
            expected_non_exception.push_back(i);
        }
    }
    RC_ASSERT(execution_order == expected_non_exception);

    // 验证 total_duration_ms 合理
    RC_ASSERT(summary.total_duration_ms >= 0);
}

// ============================================================
// Preservation 示例测试: 空 step 列表返回空 ShutdownSummary
// **Validates: Requirements 3.3**
// ============================================================

TEST(ShutdownHandlerTest, Preservation_EmptyStepsReturnsSummary) {
    ShutdownHandler handler;
    auto summary = handler.execute();

    // 空 step 列表：steps 为空
    EXPECT_TRUE(summary.steps.empty());
    // total_duration_ms >= 0
    EXPECT_GE(summary.total_duration_ms, 0);
}
