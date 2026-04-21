---
inclusion: always
---

# SHALL NOT — 禁止项池

从开发 trace 中提炼的已知失败模式，按 Spec 三层分类。
编写 Spec 时必须检查此列表，将相关项复制到对应层级的文件中。

> 维护规则：同类问题在 `docs/development-trace.md` 中出现 ≥ 3 次时，提炼到此文件对应分类下。

---

## Requirements 层

_功能边界、用户交互、场景覆盖相关的禁止项。→ 补充到 requirements.md_

暂无。

---

## Design 层

_架构模式、API 选择、依赖兼容、接口契约相关的禁止项。→ 补充到 design.md_

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret（来源：安全基线）
  - 原因：泄露风险，一旦提交到 git 无法撤回
  - 建议：通过环境变量、配置文件（已加入 .gitignore）或 AWS IoT 证书机制获取

- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）

- SHALL NOT 使用 `ContainerEntrypoint` 覆盖 SageMaker 预构建容器的默认入口脚本（来源：spec-29 Training Job 无 CloudWatch Logs）
  - 原因：覆盖默认入口会导致 SageMaker 的 CloudWatch 日志管道不初始化，训练过程无法在 CloudWatch 控制台监控
  - 建议：使用 SageMaker Python SDK 的 script mode（`sagemaker_program` + `sagemaker_submit_directory`）或 `source_dir` 方式指定训练脚本

- SHALL NOT 在非 pipeline 模块中使用 `spdlog::get("pipeline")` logger（来源：spec-12/13 Pi 5 生产日志审查）
  - 原因：所有模块共用 pipeline logger 导致日志无法按模块过滤，生产环境排查困难
  - 建议：每个模块使用与其功能对应的 logger 名称（webrtc、ai、s3、kvs 等），在 Spec tasks.md 中明确指定
  - 原因：日志可能被收集到云端或共享给他人
  - 建议：日志中只输出资源标识（如 thing-name），不输出凭证内容

- SHALL NOT 在不确定 ONNX Runtime 默认行为的情况下显式设置 SetInterOpNumThreads 为非零值（来源：spec-9.5）
  - 原因：ORT 的默认 inter-op 线程策略比显式设为 1 快 30%
  - 建议：inter_op_num_threads 默认 0（不调用），仅在明确需要时设置非零值

- SHALL NOT 在 Pi 5 多线程推理场景中保持 ORT 默认的 intra-op spinning（来源：spec-9.5）
  - 原因：spinning=1 时 4 线程空转互相抢 CPU 核心，导致性能退化 45%（636ms vs 352ms）
  - 建议：显式设置 `AddSessionConfigEntry("session.intra_op.allow_spinning", "0")`
  - 原因：日志可能被收集到云端或共享给他人
  - 建议：日志中只输出资源标识（如 thing-name），不输出凭证内容

- SHALL NOT 在 macOS 上直接在 main() 中运行含 autovideosink 的 GStreamer 管道（来源：spec-0 手动验证）
  - 原因：macOS 要求 NSApplication 在主线程运行，否则 GStreamer-GL 报警告且可能崩溃
  - 建议：用 `gst_macos_main()` 包装管道运行逻辑

- SHALL NOT 在 pImpl 模式中将需要访问 private Impl 的回调函数或辅助类型定义在 Impl 外部（来源：WebRTC SDK v1.18 适配）
  - 原因：GCC 严格检查 private 访问，macOS Clang 走 stub 路径不报错，Pi 5 上才暴露
  - 建议：将 SDK 回调函数和 CallbackContext 等辅助类型作为 Impl 的 static 成员

- SHALL NOT 在 KVS WebRTC SDK 的 `SignalingMessage.payloadLen` 中使用 `std::string::size()`（来源：spec-14 Pi 5 端到端调试）
  - 原因：`std::string::size()` 可能包含 null terminator，导致 viewer 端解析 SDP/ICE 异常，ICE 协商失败 `0x5a00000d`
  - 建议：必须用 `(UINT32) STRLEN(msg.payload)` 确保不含 null terminator，同时设置 `msg.correlationId[0] = '\0'`

- SHALL NOT 将 AVC 格式（length-prefixed）的 H.264 数据传给 KVS WebRTC SDK 的 `writeFrame`（来源：spec-14 Pi 5 端到端调试）
  - 原因：SDK 期望 Annex B（byte-stream）格式，AVC 格式报 `STATUS_RTP_INVALID_NALU (0x5c000003)`
  - 建议：在 `h264parse` 后加 capsfilter 强制 `stream-format=byte-stream,alignment=au`；kvssink 分支单独做 AVC 转换

- SHALL NOT 对含 `std::atomic` 成员的结构体使用 `unordered_map::emplace` 或 `insert`（来源：spec-13.6 Pi 5 GCC 12 编译失败）
  - 原因：`std::atomic` 不可拷贝/移动，GCC 12 严格检查类型约束，Apple Clang 宽松放过，导致 macOS 编译通过但 Pi 5 失败
  - 建议：改用 `try_emplace` + 就地构造后逐字段赋值

- SHALL NOT 在 GStreamer tee 后的单个分支上设置与其他分支冲突的 H.264 stream-format caps（来源：spec-14 Pi 5 端到端调试）
  - 原因：tee 无法同时输出两种格式，caps 协商冲突导致 pipeline 崩溃重建
  - 建议：tee 前统一为一种格式（byte-stream），各分支按需转换

---

## Tasks 层

_具体实现写法、性能预算、安全校验、边界处理相关的禁止项。→ 补充到 tasks.md 对应任务_

- SHALL NOT 直接使用系统 `python` 或 `python3` 执行项目 Python 代码或测试（来源：历史经验）
  - 原因：未激活 venv 导致依赖找不到，反复出现
  - 建议：执行任何 Python 命令前必须先 `source .venv-raspi-eye/bin/activate`

- SHALL NOT 直接运行测试可执行文件（如 `./build/log_test`、`device/build/smoke_test --gtest_print_time=1`），必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行（来源：spec-0, spec-1, spec-1 共 3 次违规）
  - 原因：直接运行单个测试可执行文件容易遗漏其他测试、运行方式不统一、与 CI 集成不一致
  - 建议：所有测试验证统一使用 `ctest --test-dir device/build --output-on-failure`，禁止在子代理 prompt 中使用任何 `./build/<test_name>` 形式的命令

- SHALL NOT 将 .pem、.key、.env、证书文件提交到 git（来源：安全基线）
  - 原因：密钥泄露不可逆
  - 建议：已在 .gitignore 中排除，提交前用 `git status` 确认

- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符（来源：历史经验）
  - 原因：GLib 输出函数在部分终端环境下不正确处理 UTF-8，显示为乱码
  - 建议：日志和用户提示信息统一使用英文

- SHALL NOT 在子代理（subagent）的最终检查点任务中执行 git commit（来源：历史经验）
  - 原因：commit 后编排层还需更新 tasks.md 状态和写入 trace 记录，导致变更遗漏
  - 建议：git commit 统一由编排层在所有 post-task hook 完成后执行

- SHALL NOT 将新建文件直接放到项目根目录（来源：历史经验）
  - 原因：根目录应保持干净，文件散落在根目录会导致项目结构混乱
  - 建议：根据文件功能放到对应模块目录下（如 device/、docs/、.kiro/ 等），参考 `.kiro/steering/structure.md` 中的项目结构

- SHALL NOT 在不确定外部 SDK/库 API 用法时凭猜测编写代码（来源：历史经验）
  - 原因：猜测 API 参数、头文件路径、返回值语义等容易导致编译失败或运行时行为不符预期，反复修复浪费时间
  - 建议：先查阅官方文档或头文件确认 API 签名、参数含义、返回值约定，必要时写最小验证代码确认行为，再正式集成

- SHALL NOT 省略 kvssink iot-certificate 属性字符串中的 `iot-thing-name` 字段（来源：spec-8 Pi 5 端到端验证）
  - 原因：AWS 官方文档未提及此字段，但 KVS SDK 内部依赖它设置 `x-amzn-iot-thingname` HTTP header。缺失时 SDK 用 stream-name 替代，当 thing name 和 stream name 不同时 IoT Credentials Provider 返回 403
  - 建议：iot-certificate 格式必须为 `iot-certificate,iot-thing-name=...,endpoint=...,cert-path=...,key-path=...,ca-path=...,role-aliases=...`

- SHALL NOT 编译 KVS Producer SDK 时使用 `-DBUILD_DEPENDENCIES=ON` 或保留 `open-source/` 目录中的自编译依赖（来源：spec-8 Pi 5 端到端验证）
  - 原因：自编译的 libcurl TLS 后端可能与系统 OpenSSL 不兼容，导致 mTLS 握手超时或失败
  - 建议：始终用 `-DBUILD_DEPENDENCIES=OFF`，编译后用 `ldd libgstkvssink.so | grep curl` 确认链接的是系统 libcurl

- SHALL NOT 使用 `cat <<` heredoc 方式写入文件（来源：用户明确要求）
  - 原因：heredoc 在 shell 中容易出现转义问题、缩进混乱，且不适合写入测试代码等复杂内容
  - 建议：使用 fsWrite / fsAppend 工具写入文件，不通过 bash heredoc

- SHALL NOT 在无法本地复现的远程平台问题上凭猜测修复（来源：spec-5 Pi 5 验证，连续 4 次猜测失败）
  - 原因：没有诊断日志时盲目猜测 GStreamer API 返回值，浪费 4 轮 push+验证
  - 建议：先在 Pi 5 上用工具收集信息（`GST_DEBUG`、`fprintf(stderr)`、`gst-inspect`、`gst-launch`、`free -m` 等），综合分析后再做一次性修改。诊断用 fprintf 不用 spdlog（测试环境可能未初始化日志）。严禁"改一行推一次看结果"的试错循环

- SHALL NOT 在 Spec 文档（requirements.md、design.md、tasks.md）未全部确定前单独 commit（来源：spec-9.5, spec-6 经验）
  - 原因：中间阶段的 commit 会产生不完整的 Spec 快照，review 修改后又要追加 commit，污染 git 历史
  - 建议：等 requirements + design + tasks 三个文档全部确定后，统一 commit 一次

- SHALL NOT 在测试中用假凭证数据调用可能创建真实 kvssink 的函数（来源：spec-8 Pi 5 验证）
  - 原因：Pi 5 上有 kvssink 插件时，假凭证数据会导致 SDK 内部解析异常（尝试分配 50GB 内存后 abort）
  - 建议：涉及 create_kvs_sink 的测试必须先检测 `kvssink_available()`，有真实 kvssink 时 GTEST_SKIP
