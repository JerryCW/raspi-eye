# 需求文档

## 简介

当前系统在 Pi 5 上通过 SSH 手动启动 `raspi-eye` 进程运行，SSH 断开后进程终止，无法实现 7×24 无人值守。进程崩溃后无自动恢复机制，需要人工介入重启。

本 Spec 创建 systemd service unit 文件实现开机自启和崩溃自动重启，并在应用代码中集成 sd_notify 协议实现进程级看门狗：应用定期发送心跳信号，超时未发送则 systemd 杀进程并重启。同时集成 READY=1（启动完成通知）和 STOPPING=1（优雅关闭通知），使 systemd 准确感知应用生命周期。macOS 开发环境通过条件编译提供空实现，不链接 libsystemd。

## 术语表

- **systemd**: Linux 系统和服务管理器，负责启动、停止、监控系统服务
- **sd_notify**: systemd 提供的通知接口，服务进程通过 Unix socket 向 systemd 发送状态消息（如 READY=1、WATCHDOG=1、STOPPING=1）
- **libsystemd**: systemd 客户端库，提供 sd_notify() 等 C API，Pi 5 Debian Bookworm 系统自带
- **WatchdogSec**: systemd unit 文件中的看门狗超时配置，服务进程必须在此间隔内发送 WATCHDOG=1 心跳，否则 systemd 判定进程卡死并执行重启
- **READY=1**: sd_notify 状态消息，表示服务启动完成并准备好接受请求
- **WATCHDOG=1**: sd_notify 心跳消息，表示服务进程仍在正常运行
- **STOPPING=1**: sd_notify 状态消息，表示服务正在执行优雅关闭
- **AppContext**: 应用上下文，定义在 `app_context.h` 中，负责三阶段生命周期管理（init/start/stop）
- **ShutdownHandler**: 关闭管理器，定义在 `shutdown_handler.h` 中，按注册逆序执行清理步骤，每步 5 秒超时，全局 30 秒超时
- **PipelineHealthMonitor**: 管道健康监控器，定义在 `pipeline_health.h` 中，检测管道异常并自动恢复
- **SdNotifier**: 本 Spec 新增的 sd_notify 封装模块，提供跨平台的 systemd 通知接口

## 需求

### 需求 1：systemd service unit 文件

**用户故事：** 作为运维人员，我希望 raspi-eye 作为 systemd 服务运行，以实现开机自启和崩溃自动重启。

#### 验收标准

1. THE service unit 文件 SHALL 位于 `scripts/raspi-eye.service`，包含完整的 `[Unit]`、`[Service]`、`[Install]` 三个 section
2. THE service unit 文件 SHALL 配置 `Type=notify`，使 systemd 等待应用发送 READY=1 后才认为服务启动成功
3. THE service unit 文件 SHALL 配置 `Restart=on-failure`，使进程非零退出码或被信号杀死时自动重启
4. THE service unit 文件 SHALL 配置 `RestartSec=5`，使重启前等待 5 秒避免快速循环重启
5. THE service unit 文件 SHALL 配置 `WatchdogSec=30`，使 systemd 在 30 秒内未收到 WATCHDOG=1 心跳时杀进程并重启
6. THE service unit 文件 SHALL 配置 `ExecStart` 使用绝对路径启动 raspi-eye 二进制文件，并通过 `--config` 参数指定配置文件路径
7. THE service unit 文件 SHALL 配置 `StandardOutput=journal` 和 `StandardError=journal`，使 stdout/stderr 输出自动进入 journalctl 日志系统
8. WHEN `systemctl enable raspi-eye.service` 被执行, THE service unit 文件 SHALL 通过 `[Install]` section 的 `WantedBy=multi-user.target` 实现开机自启
9. THE service unit 文件 SHALL 配置 `StartLimitBurst=5` 和 `StartLimitIntervalSec=60`，防止配置错误导致的无限快速重启循环（60 秒内最多重启 5 次，超过后停止尝试）
10. THE service unit 文件 SHALL 配置 `TimeoutStartSec=60`，如果应用在 60 秒内未发送 READY=1 则判定启动失败
11. THE service unit 文件 SHALL 配置安全加固选项：`NoNewPrivileges=true`（防止进程提权）、`ProtectSystem=strict`（只读挂载系统目录）、`ProtectHome=read-only`（只读挂载 home 目录）、`PrivateTmp=true`（隔离 /tmp）、`ReadWritePaths=/home/pi/raspi-eye/device/config`（允许写入配置目录）

### 需求 2：SdNotifier 封装模块

**用户故事：** 作为开发者，我希望有一个跨平台的 sd_notify 封装模块，以在 Linux 上调用 libsystemd API，在 macOS 上提供空实现。

#### 验收标准

1. THE SdNotifier SHALL 提供 `notify_ready()` 静态方法，在 Linux 上调用 `sd_notify(0, "READY=1")` 通知 systemd 服务启动完成
2. THE SdNotifier SHALL 提供 `notify_watchdog()` 静态方法，在 Linux 上调用 `sd_notify(0, "WATCHDOG=1")` 发送心跳信号
3. THE SdNotifier SHALL 提供 `notify_stopping()` 静态方法，在 Linux 上调用 `sd_notify(0, "STOPPING=1")` 通知 systemd 服务正在关闭
4. THE SdNotifier SHALL 提供 `start_watchdog_thread()` 静态方法，启动后台线程以固定间隔发送 WATCHDOG=1 心跳
5. THE SdNotifier SHALL 提供 `stop_watchdog_thread()` 静态方法，停止后台心跳线程
6. WHEN 编译目标为 macOS（`__APPLE__` 宏定义存在）, THE SdNotifier 的所有方法 SHALL 为空实现（no-op），不链接 libsystemd
7. WHEN 编译目标为 Linux（`__linux__` 宏定义存在）且 libsystemd 可用（CMake 检测到）, THE SdNotifier SHALL 链接 libsystemd 并调用真实的 sd_notify() API
8. WHEN 编译目标为 Linux 但 libsystemd 不可用, THE SdNotifier SHALL 回退到空实现并在编译时输出警告
9. WHEN sd_notify() 调用返回负值（错误）, THE SdNotifier SHALL 记录 warn 级别日志但不中断应用运行

### 需求 3：看门狗心跳集成

**用户故事：** 作为运维人员，我希望应用在运行期间定期发送心跳信号，以便 systemd 在应用卡死时自动重启。

#### 验收标准

1. WHEN AppContext::start() 成功完成, THE 应用 SHALL 调用 `SdNotifier::start_watchdog_thread()` 启动心跳线程
2. WHILE 心跳线程运行中, THE SdNotifier SHALL 每 15 秒调用一次 `sd_notify(0, "WATCHDOG=1")`（WatchdogSec=30 的一半，确保在超时前发送）
3. WHEN 应用进入关闭流程, THE 应用 SHALL 调用 `SdNotifier::stop_watchdog_thread()` 停止心跳线程
4. IF 心跳线程因异常终止, THEN THE SdNotifier SHALL 记录错误日志，心跳停止后 systemd 将在 WatchdogSec 超时后自动重启进程
5. THE 心跳线程 SHALL 使用 condition_variable::wait_for 实现定时等待（而非 sleep_for），以便 stop_watchdog_thread() 能快速唤醒线程退出

### 需求 4：启动完成通知

**用户故事：** 作为运维人员，我希望 systemd 准确知道应用何时启动完成，以避免在初始化阶段误判超时。

#### 验收标准

1. WHEN AppContext::start() 成功返回（管道已启动、健康监控已运行）, THE 应用 SHALL 调用 `SdNotifier::notify_ready()` 发送 READY=1
2. WHEN SdNotifier::notify_ready() 被调用, THE SdNotifier SHALL 在 Linux 上调用 `sd_notify(0, "READY=1")` 并记录 info 级别日志
3. IF AppContext::init() 或 AppContext::start() 失败, THEN THE 应用 SHALL 以非零退出码退出，不发送 READY=1，systemd 将根据 Restart=on-failure 策略自动重启

### 需求 5：优雅关闭通知

**用户故事：** 作为运维人员，我希望 systemd 在应用关闭期间不误判为卡死，以确保优雅关闭流程有足够时间完成。

#### 验收标准

1. WHEN 应用收到 SIGTERM/SIGINT 信号进入关闭流程, THE 应用 SHALL 在调用 AppContext::stop() 之前调用 `SdNotifier::notify_stopping()` 发送 STOPPING=1
2. WHEN SdNotifier::notify_stopping() 被调用, THE SdNotifier SHALL 在 Linux 上调用 `sd_notify(0, "STOPPING=1")` 并记录 info 级别日志
3. THE service unit 文件 SHALL 配置 `TimeoutStopSec=35`，给予 ShutdownHandler 的 30 秒全局超时加 5 秒缓冲时间

### 需求 6：CMake 构建集成

**用户故事：** 作为开发者，我希望 CMake 在 Linux 上自动查找并链接 libsystemd，在 macOS 上使用空实现，以实现无缝跨平台编译。

#### 验收标准

1. WHEN 编译目标为 Linux, THE CMakeLists.txt SHALL 使用 `pkg_check_modules` 查找 libsystemd，找到则链接并定义 `HAVE_SYSTEMD` 编译宏
2. WHEN 编译目标为 macOS, THE CMakeLists.txt SHALL 编译 SdNotifier 的空实现版本，不查找也不链接 libsystemd
3. WHEN libsystemd 在 Linux 上未安装, THE CMake 配置阶段 SHALL 输出警告信息并继续编译，SdNotifier 回退到空实现（不定义 `HAVE_SYSTEMD`）
4. THE SdNotifier 源文件 SHALL 同时编译到主程序和测试目标中

### 需求 7：journalctl 日志集成

**用户故事：** 作为运维人员，我希望通过 journalctl 查看 raspi-eye 的运行日志，以便远程诊断问题。

#### 验收标准

1. WHEN raspi-eye 作为 systemd 服务运行, THE 应用的 stdout 和 stderr 输出 SHALL 自动进入 systemd journal
2. WHEN 运维人员执行 `journalctl -u raspi-eye.service`, THE journal SHALL 显示 raspi-eye 的完整运行日志（包括 spdlog 输出和 GStreamer 诊断信息）
3. WHEN 运维人员执行 `journalctl -u raspi-eye.service -f`, THE journal SHALL 实时显示 raspi-eye 的最新日志输出

## 配置文件示例

```ini
# scripts/raspi-eye.service
# systemd service unit for raspi-eye smart camera application

[Unit]
Description=RaspiEye Smart Camera Service
After=network-online.target
Wants=network-online.target
StartLimitBurst=5
StartLimitIntervalSec=60

[Service]
Type=notify
ExecStart=/home/pi/raspi-eye/device/build/raspi-eye --config /home/pi/raspi-eye/device/config/config.toml
Restart=on-failure
RestartSec=5
WatchdogSec=30
TimeoutStartSec=60
TimeoutStopSec=35
StandardOutput=journal
StandardError=journal
SyslogIdentifier=raspi-eye
WorkingDirectory=/home/pi/raspi-eye
User=pi
Group=pi
SupplementaryGroups=video

# Security hardening
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=read-only
PrivateTmp=true
ReadWritePaths=/home/pi/raspi-eye/device/config

[Install]
WantedBy=multi-user.target
```

## 约束

- C++17，Linux only（systemd API），macOS 条件编译空实现
- systemd unit 文件放在 `scripts/` 目录
- sd_notify 通过 libsystemd 调用（Pi 5 Debian Bookworm 自带 `libsystemd-dev`）
- macOS 上不链接 libsystemd，使用 `#ifdef __linux__` 条件编译
- 心跳间隔为 WatchdogSec 的一半（15 秒），确保在超时前至少发送一次
- 不修改现有测试文件（除非需要适配 SdNotifier 编译）
- 不包含配置文件热重载、systemd socket activation、systemd-resolved 集成、容器化部署

## 禁止项

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret（来源：安全基线）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）
- SHALL NOT 在 macOS 上链接 libsystemd 或调用任何 systemd API（来源：跨平台隔离规则）
- SHALL NOT 在心跳线程中执行任何阻塞 I/O 或耗时操作（来源：看门狗可靠性要求）
