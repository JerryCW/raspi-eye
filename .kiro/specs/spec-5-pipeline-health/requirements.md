# 需求文档：Spec 5 — 管道健康监控 + 自动恢复

## 简介

本 Spec 为 GStreamer 管道引入健康监控与自动恢复机制。当前管道在遇到错误时直接退出（main.cpp 中 bus ERROR → quit loop），缺乏容错能力。对于 7×24 无人值守的智能摄像头场景，管道必须能自动检测故障并尝试恢复。

三层检测机制：
- **Buffer Probe Watchdog**（最主动）：在视频源 src pad 上挂 buffer probe，每个 buffer 重置 watchdog 计时器，超时（如 5 秒无新 buffer）判定数据流中断
- **Bus ERROR/WARNING 监听**（事件驱动）：作为辅助检测，覆盖 probe 无法捕获的场景（如元素内部错误）
- **定时心跳轮询**（秒级兜底）：周期性查询 pipeline state 和 queue level，兜底检测静默故障

健康状态机：`HEALTHY → DEGRADED → ERROR → RECOVERING`，状态转换防重入。

自动恢复策略：先尝试 NULL→PLAYING 状态重置，失败则销毁重建整个管道。指数退避（1s → 2s → 4s），达到上限后进入 FATAL 状态停止重试。

本 Spec 引入独立的 `PipelineHealthMonitor` 类，通过组合方式与 `PipelineManager` 协作，不修改 `PipelineManager` 的核心接口。

## 前置条件

- Spec 0（gstreamer-capture）✅ 已完成
- Spec 1（spdlog-logging）✅ 已完成
- Spec 2（cross-compile）✅ 已完成
- Spec 3（h264-tee-pipeline）✅ 已完成
- Spec 4（camera-abstraction）✅ 已完成

## 术语表

- **PipelineHealthMonitor**：管道健康监控器，本 Spec 新增的核心类，负责检测管道故障并触发自动恢复
- **HealthState**：健康状态枚举，包含 HEALTHY、DEGRADED、ERROR、RECOVERING、FATAL 五个状态
- **Buffer Probe**：GStreamer pad probe 机制，在数据流经 pad 时触发回调，用于监控数据流是否正常
- **Watchdog Timer**：看门狗计时器，在指定时间内未收到 buffer 则判定数据流中断
- **Bus Message**：GStreamer 消息总线上的消息，包括 ERROR、WARNING、EOS 等类型
- **Heartbeat Poll**：心跳轮询，周期性查询管道状态和 queue level 的兜底检测机制
- **State Reset Recovery**：状态重置恢复，将管道从当前状态切换到 NULL 再切换到 PLAYING 的恢复策略
- **Full Rebuild Recovery**：完全重建恢复，销毁当前管道并重新构建的恢复策略
- **Exponential Backoff**：指数退避，连续恢复失败时逐步增加重试间隔（1s → 2s → 4s）
- **PipelineManager**：管道生命周期管理器（Spec 0/3 交付），RAII 语义管理 GstElement*
- **PipelineBuilder**：双 tee 管道构建器（Spec 3 交付），构建 H.264 + tee 三路分流管道
- **CameraSource**：摄像头抽象层（Spec 4 交付），根据配置创建 GStreamer 视频源元素
- **HealthCallback**：健康状态变化回调函数类型，上层模块通过注册回调感知状态变化
- **Recovery Statistics**：恢复统计信息，包含恢复次数、最后恢复时间、连续运行时长等指标

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言标准 | C++17 |
| CMake 最低版本 | 3.16 |
| 目标平台 | macOS（Debug + ASan）/ Linux aarch64（Pi 5 Release） |
| Debug 构建 | 开启 ASan（`-fsanitize=address -fno-omit-frame-pointer`） |
| 单个测试耗时 | ≤ 5 秒 |
| 代码组织 | .h + .cpp 分离模式 |
| GStreamer 资源管理 | RAII，所有引用计数必须配对 |
| 向后兼容 | 不修改 PipelineManager 核心接口（create/start/stop/current_state/pipeline） |
| 向后兼容 | 不修改现有测试文件（smoke_test、log_test、tee_test、camera_test） |
| 日志语言 | 英文，不使用非 ASCII 字符 |
| 线程安全 | 状态机转换必须防重入，多线程访问状态需同步 |
| 新增代码量 | 100-500 行 |
| 涉及文件 | 2-5 个 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现多摄像头同时运行
- SHALL NOT 在本 Spec 中实现运行时热切换摄像头
- SHALL NOT 在本 Spec 中实现自适应码率控制（属于 Spec 14: adaptive-streaming）
- SHALL NOT 在本 Spec 中实现 systemd 看门狗集成（属于 Spec 19: systemd-watchdog）
- SHALL NOT 在本 Spec 中实现远程告警/通知（后续扩展）

### Design 层

- SHALL NOT 修改 PipelineManager 的任何公开接口（create、start、stop、current_state、pipeline）
- SHALL NOT 修改现有测试文件（smoke_test.cpp、log_test.cpp、tee_test.cpp、camera_test.cpp）
- SHALL NOT 在手动构建管道时遗漏 GStreamer 引用计数释放
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息

### Tasks 层

- SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行
- SHALL NOT 在日志中使用非 ASCII 字符
- SHALL NOT 将新建文件直接放到项目根目录
- SHALL NOT 使用 `new`/`malloc` 管理 C++ 对象（除对接 GStreamer C API 外）
- SHALL NOT 在高性能路径（Buffer Probe 回调）中执行同步磁盘 I/O


## 需求

### 需求 1：健康状态机

**用户故事：** 作为开发者，我需要一个明确的健康状态模型来表示管道的当前健康状况，以便上层模块根据状态做出决策。

#### 验收标准

1. THE PipelineHealthMonitor SHALL 定义 `HealthState` 枚举，包含五个状态：`HEALTHY`、`DEGRADED`、`ERROR`、`RECOVERING`、`FATAL`
2. WHEN PipelineHealthMonitor 创建后且管道正常运行时，THE PipelineHealthMonitor SHALL 处于 `HEALTHY` 状态
3. WHEN buffer probe watchdog 超时但 bus 未报告 ERROR 时，THE PipelineHealthMonitor SHALL 转换到 `DEGRADED` 状态
4. WHEN bus 报告 ERROR 消息或心跳轮询检测到管道状态异常时，THE PipelineHealthMonitor SHALL 转换到 `ERROR` 状态
5. WHEN 开始执行恢复操作时，THE PipelineHealthMonitor SHALL 转换到 `RECOVERING` 状态
6. WHEN 恢复成功且数据流恢复正常时，THE PipelineHealthMonitor SHALL 转换回 `HEALTHY` 状态
7. WHEN 连续恢复失败次数达到上限（默认 3 次）时，THE PipelineHealthMonitor SHALL 转换到 `FATAL` 状态并停止重试
8. THE PipelineHealthMonitor SHALL 使用 `std::mutex` 保护状态转换，防止多线程重入

### 需求 2：Buffer Probe Watchdog 检测

**用户故事：** 作为开发者，我需要通过 buffer probe 实时监控数据流，以便在数据流中断时第一时间检测到故障。

#### 验收标准

1. THE PipelineHealthMonitor SHALL 在视频源元素的 src pad 上安装 buffer probe
2. WHEN buffer probe 收到新 buffer 时，THE PipelineHealthMonitor SHALL 重置 watchdog 计时器
3. WHEN watchdog 计时器超时（默认 5 秒无新 buffer）时，THE PipelineHealthMonitor SHALL 将状态从 `HEALTHY` 转换到 `DEGRADED`
4. THE PipelineHealthMonitor SHALL 通过 spdlog 记录 watchdog 超时事件，包含超时时长和最后一次收到 buffer 的时间戳
5. THE buffer probe 回调 SHALL 仅更新时间戳，不执行同步磁盘 I/O 或阻塞操作

### 需求 3：Bus ERROR/WARNING 监听

**用户故事：** 作为开发者，我需要监听 GStreamer bus 上的错误和警告消息，以便捕获 buffer probe 无法检测到的元素内部错误。

#### 验收标准

1. THE PipelineHealthMonitor SHALL 监听管道 bus 上的 `GST_MESSAGE_ERROR` 消息
2. WHEN 收到 `GST_MESSAGE_ERROR` 消息时，THE PipelineHealthMonitor SHALL 将状态转换到 `ERROR` 并触发恢复流程
3. THE PipelineHealthMonitor SHALL 通过 spdlog 记录错误消息的来源元素名称和错误描述
4. THE PipelineHealthMonitor SHALL 监听 `GST_MESSAGE_WARNING` 消息并通过 spdlog 记录，但不触发状态转换

### 需求 4：定时心跳轮询

**用户故事：** 作为开发者，我需要周期性轮询管道状态作为兜底检测，以便发现 probe 和 bus 都无法捕获的静默故障。

#### 验收标准

1. THE PipelineHealthMonitor SHALL 以可配置的间隔（默认 2 秒）周期性查询管道状态
2. WHEN 心跳轮询检测到管道状态不是 `GST_STATE_PLAYING`（且当前 HealthState 为 HEALTHY 或 DEGRADED）时，THE PipelineHealthMonitor SHALL 将状态转换到 `ERROR`
3. THE PipelineHealthMonitor SHALL 通过 spdlog 记录每次心跳检测到的异常状态

### 需求 5：自动恢复策略

**用户故事：** 作为开发者，我需要管道在检测到故障后自动尝试恢复，以便实现 7×24 无人值守运行。

#### 验收标准

1. WHEN HealthState 转换到 `ERROR` 时，THE PipelineHealthMonitor SHALL 首先尝试状态重置恢复：将管道设置为 `GST_STATE_NULL`，等待状态变更完成，再设置为 `GST_STATE_PLAYING`
2. WHEN 状态重置恢复成功（管道达到 PLAYING 状态且 buffer probe 在 watchdog 超时时间内收到新 buffer）时，THE PipelineHealthMonitor SHALL 将状态转换回 `HEALTHY`
3. WHEN 状态重置恢复失败时，THE PipelineHealthMonitor SHALL 尝试完全重建恢复：调用用户提供的 rebuild 回调函数重新构建管道
4. WHEN 完全重建恢复成功时，THE PipelineHealthMonitor SHALL 将状态转换回 `HEALTHY` 并重新安装 buffer probe
5. THE PipelineHealthMonitor SHALL 通过 spdlog 记录每次恢复尝试的类型（state-reset / full-rebuild）和结果（success / failure）

### 需求 6：指数退避与重试上限

**用户故事：** 作为开发者，我需要恢复操作使用指数退避策略，以避免在持续故障时频繁重试消耗资源。

#### 验收标准

1. THE PipelineHealthMonitor SHALL 在连续恢复失败时使用指数退避：初始间隔可配置（默认 1 秒），每次翻倍（默认 1s → 2s → 4s）
2. WHEN 连续恢复失败次数达到上限（默认 3 次）时，THE PipelineHealthMonitor SHALL 转换到 `FATAL` 状态并停止所有重试
3. WHEN 恢复成功时，THE PipelineHealthMonitor SHALL 重置退避间隔和连续失败计数
4. THE PipelineHealthMonitor SHALL 通过 spdlog 记录当前退避间隔和剩余重试次数
5. THE 退避间隔、重试上限、watchdog 超时等参数 SHALL 可通过构造函数或配置方法设置，以便测试中使用毫秒级间隔

### 需求 7：恢复统计指标

**用户故事：** 作为开发者，我需要查询管道的恢复统计信息，以便监控系统的长期稳定性。

#### 验收标准

1. THE PipelineHealthMonitor SHALL 提供 `stats()` 方法，返回包含以下字段的统计结构体：总恢复次数、最后恢复时间（`std::chrono::steady_clock::time_point`）、连续运行时长（自最后一次恢复成功以来的时长）
2. WHEN 恢复成功时，THE PipelineHealthMonitor SHALL 递增总恢复次数并更新最后恢复时间
3. THE stats() 方法 SHALL 线程安全，可在任意线程调用

### 需求 8：健康状态变化回调

**用户故事：** 作为开发者，我需要注册回调函数来感知健康状态变化，以便上层模块（如 main.cpp）在状态变化时执行相应操作（如日志记录、通知）。

#### 验收标准

1. THE PipelineHealthMonitor SHALL 提供 `set_health_callback(std::function<void(HealthState old_state, HealthState new_state)>)` 方法
2. WHEN HealthState 发生变化时，THE PipelineHealthMonitor SHALL 调用已注册的回调函数，传入旧状态和新状态
3. IF 未注册回调函数，THEN THE PipelineHealthMonitor SHALL 正常运行，不调用任何回调
4. THE PipelineHealthMonitor SHALL 在状态转换的 mutex 保护范围外调用回调，避免回调中操作 monitor 导致死锁

### 需求 9：故障注入测试

**用户故事：** 作为开发者，我需要通过故障注入验证健康监控和自动恢复机制的正确性，以确保在真实故障场景下系统行为符合预期。

#### 验收标准

1. THE Test_Suite SHALL 验证：通过向管道 bus 手动 post `GST_MESSAGE_ERROR` 消息，PipelineHealthMonitor 检测到错误并触发恢复流程
2. THE Test_Suite SHALL 验证：恢复成功后 HealthState 回到 `HEALTHY`
3. THE Test_Suite SHALL 验证：连续恢复失败达到上限后 HealthState 转换到 `FATAL`
4. THE Test_Suite SHALL 验证：健康状态变化回调在每次状态转换时被正确调用，传入正确的旧状态和新状态
5. THE Test_Suite SHALL 验证：恢复统计指标（总恢复次数）在恢复成功后正确递增
6. THE Test_Suite SHALL 验证：指数退避间隔按配置的初始间隔逐次翻倍递增（测试中使用毫秒级间隔以满足 5 秒约束）
7. THE Test_Suite SHALL 仅使用 `fakesink` 作为 sink 元素，不使用需要显示设备的 sink
8. WHEN 单个测试执行时，THE Test_Suite SHALL 在 5 秒内完成
9. THE Test_Suite SHALL 在 macOS Debug（ASan）构建下通过，无内存错误报告

### 需求 10：双平台构建与测试验证

**用户故事：** 作为开发者，我需要确保管道健康监控在 macOS 和 Pi 5 上均能成功编译和测试。

#### 验收标准

1. WHEN 在 macOS 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试（包括现有 smoke_test、log_test、tee_test、camera_test 和新增的 health_test），ASan 不报告任何内存错误
2. WHEN 在 Pi 5 上执行 `ctest --test-dir device/build --output-on-failure` 时，THE Test_Suite SHALL 通过所有测试
3. WHEN 在 macOS 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误
4. WHEN 在 Pi 5 上执行 `cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build` 时，THE Build_System SHALL 成功编译所有源文件，无编译错误

## 参考代码

### Buffer Probe 安装示例

```cpp
// 在视频源 src pad 上安装 buffer probe
GstPad* src_pad = gst_element_get_static_pad(source_element, "src");
gulong probe_id = gst_pad_add_probe(
    src_pad,
    GST_PAD_PROBE_TYPE_BUFFER,
    buffer_probe_callback,
    user_data,
    nullptr);
gst_object_unref(src_pad);
```

### 手动 Post ERROR 消息（故障注入）

```cpp
// 向管道 bus 手动 post ERROR 消息用于测试
GstBus* bus = gst_element_get_bus(pipeline);
GError* error = g_error_new(GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "Injected test error");
GstMessage* msg = gst_message_new_error(GST_OBJECT(pipeline), error, "fault injection for testing");
gst_bus_post(bus, msg);
g_error_free(error);
gst_object_unref(bus);
```

## 验证命令

```bash
# macOS Debug 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure

# Pi 5 Release 构建 + 测试
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release && cmake --build device/build && ctest --test-dir device/build --output-on-failure
```

预期结果：两个平台均配置成功、编译无错误、所有测试通过（macOS 下 ASan 无报告）。

## 明确不包含

- 多摄像头同时运行（后期扩展）
- 运行时热切换摄像头（需管道重建，但切换决策不在本 Spec 范围）
- 自适应码率控制（Spec 14: adaptive-streaming）
- systemd 看门狗集成（Spec 19: systemd-watchdog，进程级看门狗）
- 远程告警/通知（后续扩展）
- 替换 fakesink 为实际 KVS/WebRTC/AI sink（Spec 8/12/9）
