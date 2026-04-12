# 实施计划：systemd 看门狗集成

## 概述

将 raspi-eye 集成为 systemd 服务，实现开机自启、崩溃自动重启和进程级看门狗。新增 SdNotifier 跨平台封装模块（Linux 调用 libsystemd，macOS 空实现），在 main.cpp 中集成 READY=1、WATCHDOG=1、STOPPING=1 生命周期通知。CMake 通过 pkg_check_modules 可选链接 libsystemd。

## 任务

- [x] 1. 创建 SdNotifier 模块
  - [x] 1.1 创建 `device/src/sd_notifier.h`，声明 SdNotifier 静态方法类（notify_ready/notify_watchdog/notify_stopping/start_watchdog_thread/stop_watchdog_thread/watchdog_running），禁止实例化（构造函数 = delete）
    - _需求: 2.1, 2.2, 2.3, 2.4, 2.5_
  - [x] 1.2 创建 `device/src/sd_notifier.cpp`，实现条件编译逻辑：`#ifdef HAVE_SYSTEMD` 调用 sd_notify() 真实 API，否则空实现；心跳线程使用 condition_variable::wait_for 定时等待，stop 通过 notify_one 快速唤醒；sd_notify 错误记录 warn 日志但不中断运行
    - 静态变量：s_mtx、s_cv、s_stop（atomic<bool>）、s_running（atomic<bool>）、s_thread
    - notify_watchdog 成功时不记录日志（避免每 15 秒刷屏）
    - 心跳线程异常用 try/catch 捕获并记录 error 日志
    - _需求: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.2, 3.4, 3.5_

- [x] 2. CMake 构建集成
  - [x] 2.1 修改 `device/CMakeLists.txt`：Linux 上用 `pkg_check_modules(SYSTEMD libsystemd)` 查找 libsystemd，找到则定义 `HAVE_SYSTEMD=1` 编译宏并链接；macOS 跳过查找；libsystemd 未找到时输出 WARNING 并继续
    - 新增 `sd_notifier_module` 静态库目标，链接 spdlog
    - 将 sd_notifier_module 链接到 raspi-eye 主程序
    - _需求: 6.1, 6.2, 6.3, 6.4_

- [x] 3. 集成 SdNotifier 到 main.cpp
  - [x] 3.1 修改 `device/src/main.cpp`：在 AppContext::start() 成功后调用 `SdNotifier::notify_ready()` 和 `SdNotifier::start_watchdog_thread()`；在 g_main_loop_run() 退出后、AppContext::stop() 之前调用 `SdNotifier::notify_stopping()` 和 `SdNotifier::stop_watchdog_thread()`
    - 添加 `#include "sd_notifier.h"`
    - 启动失败时不发送 READY=1，以非零退出码退出
    - _需求: 3.1, 3.3, 4.1, 4.2, 4.3, 5.1, 5.2_

- [x] 4. 检查点 - 编译验证
  - 确保 macOS Debug 构建通过（cmake configure + build），所有现有测试仍然通过。如有问题请询问用户。

- [x] 5. 单元测试与属性测试
  - [x] 5.1 创建 `device/tests/sd_notifier_test.cpp`，编写 example-based 单元测试：notify_ready/notify_watchdog/notify_stopping 不崩溃、初始状态 watchdog_running==false、stop 未运行的线程不崩溃、短间隔启动心跳线程后验证 running 状态
    - _需求: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_
  - [x] 5.2 在 `device/CMakeLists.txt` 中添加 sd_notifier_test 测试目标，链接 sd_notifier_module、GTest、RapidCheck
    - _需求: 6.4_
  - [x] 5.3 编写属性测试：心跳线程 start/stop 往返一致性
    - **Property 1: 心跳线程 start/stop 往返一致性**
    - 生成器：随机正整数 interval_sec ∈ [1, 60]
    - 验证：start 后 running==true，stop 后 running==false
    - **验证需求: 2.4, 2.5, 3.1, 3.3**
  - [x] 5.4 编写属性测试：stop_watchdog_thread 快速响应
    - **Property 2: stop_watchdog_thread 快速响应**
    - 生成器：随机正整数 interval_sec ∈ [1, 60]
    - 验证：start 后立即 stop，stop 在 1 秒内返回
    - **验证需求: 3.5**
  - [x] 5.5 编写属性测试：start_watchdog_thread 幂等性
    - **Property 3: start_watchdog_thread 幂等性**
    - 生成器：随机正整数 interval_sec ∈ [1, 60]
    - 验证：连续两次 start 不崩溃，running==true，单次 stop 即可停止
    - **验证需求: 2.4**
  - [x] 5.6 编写属性测试：notify 方法不崩溃
    - **Property 4: notify 方法不崩溃**
    - 生成器：随机调用序列（1-10 次，从 notify_ready/notify_watchdog/notify_stopping 中随机选择）
    - 验证：所有调用正常返回，不抛异常
    - **验证需求: 2.1, 2.2, 2.3, 2.6, 2.9**

- [x] 6. 检查点 - 全量测试验证
  - 确保所有测试通过（`ctest --test-dir device/build --output-on-failure`），包括新增的 sd_notifier_test。如有问题请询问用户。

- [x] 7. 创建 systemd service unit 文件
  - [x] 7.1 创建 `scripts/raspi-eye.service`，包含完整的 [Unit]、[Service]、[Install] 三个 section：Type=notify、Restart=on-failure、RestartSec=5、WatchdogSec=30、TimeoutStartSec=60、TimeoutStopSec=35、StartLimitBurst=5、StartLimitIntervalSec=60、安全加固选项（NoNewPrivileges、ProtectSystem、ProtectHome、PrivateTmp、ReadWritePaths）、StandardOutput/StandardError=journal、WantedBy=multi-user.target
    - _需求: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 1.10, 1.11, 5.3, 7.1, 7.2, 7.3_

- [x] 8. 最终检查点
  - 确保所有测试通过，确认 sd_notifier.h/cpp、sd_notifier_test.cpp、raspi-eye.service、CMakeLists.txt 改动、main.cpp 改动均已完成。如有问题请询问用户。

## 注意事项

- 标记 `*` 的任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求编号以确保可追溯
- 检查点确保增量验证
- 属性测试验证设计文档中定义的正确性属性
- 单元测试验证具体示例和边界情况
- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在心跳线程中使用 sleep_for，必须使用 condition_variable::wait_for
- SHALL NOT 在 macOS 上链接 libsystemd 或调用任何 systemd API
