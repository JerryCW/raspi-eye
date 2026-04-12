// sd_notifier.h
// 跨平台 systemd 通知封装：Linux 调用 libsystemd，macOS 空实现。
#pragma once

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
};
