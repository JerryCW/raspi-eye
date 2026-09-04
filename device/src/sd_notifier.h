// sd_notifier.h
// 跨平台 systemd 通知封装：Linux 调用 libsystemd，macOS 空实现。
#pragma once

#include <cstdint>
#include <functional>

class SdNotifier {
public:
    // 禁止实例化
    SdNotifier() = delete;

    // 通知 systemd 服务启动完成（READY=1）
    // Linux + HAVE_SYSTEMD: 调用 sd_notify(0, "READY=1")
    // 其他平台: no-op
    static void notify_ready();

    // 发送看门狗心跳（WATCHDOG=1）
    // Linux + HAVE_SYSTEMD: 调用 sd_notify(0, "WATCHDOG=1")
    // 其他平台: no-op
    static void notify_watchdog();

    // 通知 systemd 服务正在关闭（STOPPING=1）
    // Linux + HAVE_SYSTEMD: 调用 sd_notify(0, "STOPPING=1")
    // 其他平台: no-op
    static void notify_stopping();

    // 启动后台心跳线程，每 interval_sec 秒发送一次 WATCHDOG=1
    // 如果线程已在运行，则忽略调用
    // interval_sec: 心跳间隔（秒），默认 15（WatchdogSec=30 的一半）
    static void start_watchdog_thread(int interval_sec = 15);

    // 停止后台心跳线程，通过 condition_variable 快速唤醒
    // 如果线程未运行，则忽略调用
    static void stop_watchdog_thread();

    // 查询心跳线程是否正在运行（线程安全）
    static bool watchdog_running();

    // 注册健康查询回调（线程安全）。看门狗心跳发送前查询：
    // 返回 false（非健康，如 FATAL）时跳过 WATCHDOG=1，使 systemd WatchdogSec 兜底重启。
    // 未注册时默认 healthy=true（保持现有行为）。(Spec 32 需求 4)
    static void set_health_check(std::function<bool()> is_healthy);

    // 返回当前是否应发送看门狗心跳（组合门控判定，可单测）：
    // health 维度（既有语义不变：未注册 health_check 时视为健康）AND
    // liveness 维度（时间戳新鲜度，见 should_feed_watchdog）。(Spec 32.5 缺陷 1)
    // 判定为跳过喂狗时输出 warn 日志并区分原因（health gate / liveness stale）。
    static bool watchdog_gate_open();

    // --- Spec 32.5 缺陷 1：喂狗与主循环活性绑定（liveness 门控） ---
    //
    // 阈值链约束（改任何参数必须重推，见 spec-32.5 design 决策点 1）：
    //   相邻检查点最大间隔 5s+ε < T_stale=10s < 喂狗间隔 15s < WatchdogSec=30s
    // - 5s+ε：恢复关键路径上单次有界等待预算（每个有界原语返回点即检查点）
    // - T_stale=10s：给出 2 倍安全余量；且 < 喂狗间隔 15s，保证死锁后第一个
    //   喂狗时机必然判定 stale 并跳过（陈旧度 >= 15-2 = 13s > 10s）
    // - WatchdogSec=30s：systemd 自最后一次成功喂狗起 <=30s 触发重启，
    //   总检测窗口 <=35s（含 RestartSec=5）

    // 刷新 liveness 时间戳（写入 steady_clock 当前毫秒，单次原子写）。
    // 由 GLib 主循环 2s timer（CP0）与恢复流程检查点（CP1-CP6）调用。
    static void refresh_liveness();

    // 纯函数：喂狗判定（时钟全部参数注入，不依赖 systemd 与真实时钟，可单测/PBT）。
    // - !healthy 短路返回 false（保留 spec-32 FATAL 门控语义）
    // - last_liveness_ms == 0 为未刷新哨兵，liveness 门控视为开（向后兼容：
    //   只由 health 决定；生产上 main.cpp 在启动看门狗前先显式刷新一次，
    //   哨兵态不会出现在生产运行期）
    // - 否则 (now_ms - last_liveness_ms) <= stale_threshold_ms 时喂狗
    static bool should_feed_watchdog(int64_t now_ms, int64_t last_liveness_ms,
                                     int64_t stale_threshold_ms, bool healthy);

    // 设置 liveness stale 阈值（毫秒，测试用；生产用默认 10000）。
    static void set_liveness_stale_threshold_ms(int64_t ms);

    // 测试专用：复位 liveness 状态到初始值（时间戳回哨兵 0、阈值回默认 10000），
    // 保证触碰过 liveness 的测试不影响其他测试（顺序无关）。生产代码不得调用。
    static void reset_liveness_for_test();
};
