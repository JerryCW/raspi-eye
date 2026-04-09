---
inclusion: always
---

# C++ 性能与安全约束

目标平台 Raspberry Pi 5（4GB RAM），从第一行代码开始关注性能和内存安全。

## 内存安全

- 除对接 GStreamer / C 库 API 外，禁止使用 `new`/`malloc`，优先 `std::unique_ptr` 或 `std::shared_ptr`
- 所有资源（文件句柄、GStreamer 对象、socket）必须遵循 RAII，在析构函数中释放
- GStreamer 引用计数必须配对：每个 `gst_element_get_bus` / `gst_*_ref` 必须有对应的 `gst_object_unref`
- 禁止 `goto`、禁止未初始化的指针
- 拷贝语义：持有 GStreamer 资源指针的类必须 `= delete` 拷贝构造和拷贝赋值

## 性能约束

- 管道冷启动（初始化到第一个 Buffer 到达 Sink）≤ 2 秒
- 高性能路径（Buffer Probe、编码回调）中禁止同步磁盘 I/O
- videoconvert 与后续模块对接时优先探索零拷贝（内存映射），避免不必要的数据拷贝
- 单个测试超时分级：

| 测试类型 | 超时 | 说明 |
|---------|------|------|
| 纯逻辑/状态机 | ≤ 1 秒 | 不涉及 GStreamer 管道启停 |
| 管道启停 | ≤ 5 秒 | 单次 pipeline create/start/stop |
| 多轮恢复/PBT | ≤ 15 秒 | 涉及 get_state 等待 × N 次重试 |

## 构建安全

- Debug 构建默认开启 AddressSanitizer：`-fsanitize=address -fno-omit-frame-pointer`
- 测试运行时如果出现 ASan 报告（heap-use-after-free、buffer-overflow 等），必须修复后才能标记任务完成
- 后续代码量增长后（Spec 3+）引入 clang-tidy 静态分析

## 日志规范

- 错误路径必须输出上下文信息：Element 名称、错误代码、当前状态
- 禁止在 `g_print`/`g_printerr` 中使用非 ASCII 字符
- 禁止在日志中输出密钥、证书内容、token 等敏感信息

## 代码组织

- 统一使用 .h + .cpp 分离模式。严禁使用 Header-only 模式处理复杂逻辑，以提升 Raspberry Pi 5 的增量编译效率
- 例外：纯 POD 结构体（如 PipelineConfig）可以保持 header-only

## 跨平台隔离

- 所有平台相关代码必须通过条件编译隔离（`#ifdef __APPLE__` / `#ifdef __linux__`），不允许裸写平台特定 API
- macOS 开发环境使用 stub 实现（如 WebRTC stub、videotestsrc 替代真实摄像头）
- Pi 5 特有功能（libcamera、V4L2、systemd、硬解码）通过接口抽象层隔离，macOS 提供空实现或 mock
