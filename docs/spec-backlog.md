# Spec 路线图

基于 `product.md` 产品定义，按依赖顺序规划的完整 Spec 列表。
每个 Spec 遵循 meta-harness 方法论：3-7 个 Task、2-5 个文件、100-500 行新增代码。

## 依赖关系图

```
spec-0 → spec-1 → spec-2（Pi 5 原生编译）→ spec-3（H.264 + tee）→ spec-4（摄像头抽象）
                                              │
                                              ├→ spec-5（管道健康监控，依赖 spec-3 + spec-4）
                                              │
                                              ├→ spec-6（IoT provisioning）→ spec-7（credential-provider）
                                              │                             └→ spec-8（KVS，也依赖 spec-5）
                                              │                                                            ├→ spec-12（WebRTC 信令，也依赖 spec-5）→ spec-13（WebRTC 媒体）
                                              │                                                            └→ spec-11（截图上传，也依赖 spec-10）
                                              │
                                              └→ spec-9（YOLO 检测）→ spec-9.5（ONNX ARM 优化）→ spec-10（AI 管道，也依赖 spec-5）→ spec-11（截图上传）

spec-8 + spec-13 → spec-13.5（main.cpp KVS+WebRTC 集成）→ spec-14（WebRTC SDP bugfix）→ spec-15（自适应码率 + 流模式切换）
spec-15 → spec-16（零拷贝缓冲区）
spec-11 → spec-17（SageMaker endpoint）→ spec-18（Lambda 触发）
spec-6 → spec-19（配置文件加载）
spec-19 → spec-20（systemd 看门狗，不再硬依赖 spec-16）
spec-20 → spec-22（部署自动化）
spec-19 → spec-23（统一日志管理）
spec-13 → spec-13.6（WebRTC peer 生命周期死锁修复）
spec-18 + spec-13 → spec-21（前端 MVP，可选）
```

---

## 阶段一：设备端骨架 + 双平台验证

从零搭建到 tee 分流管道 + 摄像头抽象。交叉编译在 spdlog 之后立即搭建，从 spec-3 开始所有 Spec 双平台验证。

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 0 | gstreamer-capture | CMake 骨架 + PipelineManager（gst_parse_launch）+ 冒烟测试 | 无 | device | ✅ |
| 1 | spdlog-logging | 结构化诊断基准：spdlog 替换 g_print，支持 JSON 单行非阻塞输出 | spec-0 | device | ✅ |
| 2 | cross-compile | Pi 5 原生编译流程 + SSH 远程构建脚本 + 双平台验证 | spec-1 | device | ✅ |
| 3 | h264-tee-pipeline | H.264 编码 + tee 分流（3 路 fakesink 占位），手动构建管道 | spec-2 | device | ✅ |
| 4 | camera-abstraction | 摄像头接口抽象层（videotestsrc / libcamera / V4L2 统一接口） | spec-3 | device | ✅ |

为什么 Pi 5 原生编译放 spec-2：spec-0 + spec-1 完成后有完整的 CMake + GStreamer + spdlog + GTest 项目，复杂度刚好够验证双平台编译。Pi 5 原生编译维护成本低，项目规模小时编译速度可接受。从 spec-3 开始每个 Spec 都双平台验证，H.264 编码在 Pi 5 上的 CPU 表现第一时间可见。后续编译时间超过 5 分钟时再考虑交叉编译。

## 阶段二：管道容错 + AWS 基础设施 + 认证

管道健康监控不依赖 AWS，可以和 infra 并行。认证体系为后续三路 sink 接入做准备。

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 5 | pipeline-health | 管道健康监控 + 自动恢复（Element 错误检测、重建管道）+ 故障注入验证 | spec-3, spec-4 | device | ✅ |
| 6 | iot-provisioning | AWS IoT Thing 注册 + X.509 证书生成 + IoT Policy + IAM Role + Role Alias（Bash 脚本 + AWS CLI） | spec-3 | scripts + infra | ✅ |
| 7 | credential-provider | 设备端 C++ 凭证模块：libcurl mTLS 请求 IoT Credentials Provider 获取 STS 临时凭证 | spec-6 | device | ✅ |

spec-5 和 spec-6 可并行开发（一个 device，一个 infra + device）。管道健康监控是纯设备端逻辑，和 AWS 凭证无关。IaC 资源（KVS 流、S3 桶等）推迟到各自的 Spec 里按需创建。

## 阶段三：三路分支实际集成（可并行推进）

tee 的三条分支从 fakesink 替换为实际 sink。三条路线互相独立。

### 路线 A：KVS 录制

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 8 | kvs-producer | KVS Producer SDK 集成，替换 kvs 分支的 fakesink | spec-6, spec-5 | device + scripts | ✅ |

### 路线 B：AI 检测 + 截图上传

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 9 | yolo-detector | ONNX Runtime + YOLOv11s 设备端目标检测（独立模块）+ 推理耗时与峰值内存基线采集 | spec-3 | device | ✅ |
| 9.5 | onnx-arm-optimization | ONNX Runtime ARM 推理优化（XNNPACK EP、线程调优、图优化、INT8 量化）+ A/B 基准测试 | spec-9 | device | ✅ |
| 10 | ai-pipeline | AI 推理管道集成，替换 ai 分支的 fakesink，buffer probe 抽帧 + 检测 | spec-9(.5), spec-5 | device | ✅ |
| 11 | screenshot-uploader | 检测到目标后截图 + 上传 S3（libcurl + S3 REST API + SigV4 签名） | spec-10, spec-7 | device | ✅ |

### 路线 C：WebRTC 实时观看

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 12 | webrtc-signaling | KVS WebRTC 信令通道（macOS stub + Linux 实现） | spec-7, spec-5 | device | ✅ |
| 13 | webrtc-media | WebRTC 媒体流，替换 webrtc 分支的 fakesink | spec-12 | device | ✅ |

### 路线 D：main.cpp 端到端集成

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 13.5 | main-integration | main.cpp 集成 KVS + WebRTC 两路：读取 config.toml → 创建凭证/信令/媒体模块 → 传入 build_tee_pipeline，Pi 5 端到端验证 | spec-8, spec-13 | device | ✅ |
| 4.5 | camera-source-v2 | 摄像头源管道增强：V4L2 格式自动检测（MJPG→jpegdec）、多摄像头支持（udev symlink）、libcamerasrc CSI 支持 | spec-4 | device | ⬜ |

YOLO 检测器（spec-9）只依赖 spec-3：纯本地推理，不需要 AWS 凭证。AI 管道（spec-10）依赖 spec-5 因为需要健康监控保护 ai 分支。

## 阶段四：管道智能化与性能优化

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 14 | webrtc-sdp-fix | Bugfix: WebRTC SDP 协商 + ICE 连接 + NALU 格式修复（addSupportedCodec、ICE 缓存、byte-stream 格式） | spec-13.5 | device | ✅ |
| 13.6 | webrtc-peer-lifecycle-fix | Bugfix: WebRTC peer connection 生命周期死锁修复 — deferred cleanup + shared_mutex + cleanup thread | spec-13 | device | ✅ |
| 13.7 | signaling-reconnect-fix | Bugfix: signaling WebSocket 断连自动重连与健康日志 | spec-12, spec-13.6 | device | ✅ |
| 15 | adaptive-streaming | 自适应码率控制 + 流模式切换（FULL/KVS_ONLY/WEBRTC_ONLY/DEGRADED） | spec-8, spec-13 | device | ✅ |
| 15.5 | shutdown-fix | Bugfix: Shutdown 卡死修复 — std::thread + condition_variable 替换 std::async | spec-15 | device | ✅ |
| 16 | zero-copy-buffers | 缓冲区零拷贝重构与预分配池化，目标 CPU 负载 ~26% @ 720p15 | spec-15 | device | ✅ |

零拷贝在自适应码率之后：自适应码率会动态启停分支改变数据流路径，零拷贝需要在最终路径上优化。

## 阶段五：云端 AI 推理

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 27 | inat-data-collection | iNaturalist 鸟类图片采集 + 数据清洗（去重、质量过滤、统一 resize）→ 按物种分目录的训练数据集 | 无 | model | ✅ |
| 28 | feature-space-cleaning | DINOv3 特征空间深度清洗：YOLO 鸟体裁切 + DINOv3 特征提取（frozen backbone）+ Mahalanobis 离群点检测 + 余弦相似度语义去重 + train/val 分层划分 → ImageFolder 格式数据集 | spec-27 | model | ✅ |
| 29 | bird-classifier-training | DINOv3-ViT-L/16 backbone（frozen）+ linear head fine-tuning + 数据增强 → 鸟类分类模型训练 + 评估 + 导出 | spec-28 | model | ✅ |
| 17 | sagemaker-endpoint | SageMaker Endpoint 部署（Serverless + Real-time 双模式）+ Lambda 触发 + DynamoDB 写回 raspi-eye-events 表 | spec-29 | model + infra | ✅ |
| 18 | lambda-trigger | （已合并到 spec-17） | spec-17 | model + infra | ✅ |

## 阶段六：部署运维

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 19 | config-file | ConfigManager 统一配置加载：TOML 解析 camera/streaming/logging section，命令行覆盖，三层优先级 | spec-7 | device | ✅ |
| 20 | systemd-watchdog | systemd 服务集成 + 进程级看门狗（7×24 无人值守），配置文件路径通过 systemd unit 指定 | spec-19 | device + scripts | ✅ |
| 22 | pi-deploy | Pi 5 部署自动化：build + install + systemctl restart 一键脚本，config.toml 初始化部署 | spec-20 | scripts | ✅ |
| 23 | log-management | 统一日志管理：per-component level 配置 + KVS SDK 日志重定向到 spdlog + 统一格式 | spec-19 | device | ✅ |

## 阶段五续：推理质量优化

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 30 | inference-yolo-crop | 推理链路 YOLO Crop 对齐：SageMaker endpoint 加入 YOLO crop 步骤消除 train-serving skew，Lambda 提取 crop 图片上传 S3 | spec-17 | model | ✅ |
| 31 | inference-voting-threshold | Lambda 端多图投票与置信度门槛：多数投票选择最终 species + confidence < 0.5 标记 uncertain + DynamoDB 新增 vote_count/reliable 字段 | spec-30 | model | 🔄 |

## 阶段八：设备端稳定性加固

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 32 | pipeline-resilience | 管道韧性加固：bus 错误按域分类（KVS/WebRTC 分支错误不再拖垮整管道）+ kvssink restart-on-error 自愈 + heartbeat 去抖主动恢复 + 有界异步 teardown（set_state(NULL) 移出主循环，130s→≤5s）+ FATAL 优雅退出交 systemd 重启 + 看门狗健康门控 + 默认码率降至 1200kbps + CPU 基线诊断脚本 | spec-5, spec-8, spec-20 | device + scripts | 🔄 |
| 33 | webrtc-long-running-resilience | Bugfix: Task 0 失活取证（pi-diagnose.sh，归因基线）+ Pi SDK 语义门禁 + signaling 单 owner/两级 command deadline（QUERY 2s / SEND 10s 迟到无害）+ 有界 message dispatcher + 固定池化 CallbackBridge（type-stable，免 SDK quiescence 证明）+ PeerSession/HandlePermit/Reaper + 僵尸 peer 回收，修复运行 1～2 天后 WebRTC 永久停止；分两个 Stage 部署归因（Stage 1 signaling 层 / Stage 2 peer 层）；4 个生产文件 + 2 个既有测试文件（6 文件显式例外） | spec-13.6, spec-13.7；整机 72h 依赖 spec-34 | device | ⬜ |
| 34 | device-lifecycle-safety | 修复 pipeline 双 teardown owner、ShutdownHandler detached worker、borrower 解绑顺序、health timer 与 AppContext 生命周期 P0；完成后解锁 spec-32 故障注入和 spec-33 整机 72h | spec-32 Task 1～6 | device | ⬜ |

## 阶段七：前端（可选，优先级低）

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 21 | viewer-mvp | 前端 MVP：WebRTC 实时观看 + 事件列表浏览 | spec-18, spec-13 | viewer | ⬜ |

---

## 推荐执行顺序（单人串行）

```
0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 9.5 → 12 → 13 → 13.5 → 14 → 15 → 16 → 19 → 20 → 22 → 10 → 11 → 17 → 18 → 21
```

其中 spec-5 和 spec-6 可并行，spec-9 可在等待 spec-7 时提前开发，spec-16（零拷贝）为性能优化可后置。

理由：
- 0-2：从零到双平台可编译的基础设施（Pi 5 原生编译 + SSH 远程构建）
- 3-4：H.264 + tee 分流 + 摄像头抽象，Pi 5 上第一时间验证 CPU 表现
- 5-7：管道容错 + IoT provisioning + 设备端凭证模块
- 8：KVS 录制（最核心数据通路）
- 9-9.5：YOLO + ONNX 优化（独立模块，不阻塞主线）
- 12-15：WebRTC + 端到端集成 + SDP bugfix + 自适应码率
- 16：零拷贝性能优化（可后置，不影响功能完整性）
- 19：配置文件统一加载
- 20：systemd 看门狗（7×24 无人值守）
- 22：Pi 5 部署自动化（一键部署脚本）
- 10-11：AI 管道 + 截图上传（AI 工程阶段）
- 17-18：云端推理
- 21：前端（可选）

---

## 延迟待办项

_从 Spec 执行过程中推迟的事项，创建新 Spec 前检查此列表。_

- **XNNPACK execution provider 优化**：ONNX Runtime 针对 ARM CPU 的加速后端，比默认 CPU provider 快 2-3x。需要从源码编译 ONNX Runtime 启用 `--use_xnnpack`。等 Spec 9 基线数据出来后决定是否需要。（来源：spec-9 需求讨论）→ **已纳入 Spec 9.5（onnx-arm-optimization）**

- **kvssink framerate/avg-bandwidth-bps 调优**：kvssink 默认 framerate=25、avg-bandwidth-bps=4194304（4Mbps），Pi 5 上 720p15 的实际码率远低于 4Mbps。不匹配不影响功能，但 KVS SDK 内部缓冲区分配可能不够优化。等 spec-14（adaptive-streaming）时根据实际码率数据统一调优。（来源：spec-8 review）

- **WebRtcMediaManager 与 PipelineHealthMonitor 集成**：WebRTC 分支的连接状态（所有 peer 断开、连续 writeFrame 失败等）需要反馈给 PipelineHealthMonitor，用于流模式切换（FULL/KVS_ONLY/WEBRTC_ONLY/DEGRADED）。等 spec-14（adaptive-streaming）实现。（来源：spec-13 review）

- **broadcast_frame 异步帧分发**：当前 broadcast_frame 在 GStreamer streaming 线程中同步调用 writeFrame，持锁遍历所有 peer。→ **已纳入 Spec 33 的 SDK 语义门禁与媒体 I/O 隔离**：先确认 Pi 当前 SDK 的 `writeFrame` 阻塞上界和 frameData 所有权，再采用 session 级 I/O gate；不得用 detached frame worker 猜测规避生命周期问题。（来源：spec-13 review、spec-33 design）

- **WebRTC 日志可观测性优化**：当前 ICE candidate 收发每条都打 info 级别（一次连接 20+ 条），缺少 ICE 状态转换、DTLS 握手、SDP 摘要、media flow 统计等关键诊断信息。需要：(1) ICE candidate 单条降为 debug，连接建立后打汇总 info；(2) 增加 ICE/DTLS 状态转换里程碑日志；(3) SDP offer/answer 打印 codec/分辨率摘要；(4) peer 连接期间增加 media flow 状态日志；(5) cleanup 日志加上 peer 存活时长。（来源：spec-24 Pi 5 生产日志审查）→ **已纳入 Spec 25（webrtc-log-observability）**

- **WebRTC 永久失活升级为进程退出（兜底自愈）**：spec-33 的目标是无需重启即可恢复，但作为 7×24 无人值守产品，恢复手段穷尽后仍需最后一道保险：signaling 连续 recreate 失败超过阈值（如 30 分钟）或 WebRTC 子系统进入不可恢复状态时，主动让 sd_notify 停止喂狗或直接优雅退出，交 systemd 重启（Restart=on-failure + WatchdogSec=30 已就绪）。与 spec-33 验收不冲突（验收针对断网/churn 场景，兜底针对未知 bug）。等 spec-33 Stage 2 部署观察后，纳入 spec-20 的后续迭代或独立小 Spec。（来源：spec-33 第三次 review）

- **KVS streamLatencyPressure 接入 BitrateAdapter**：当前 adaptive-streaming（Spec 15）仅监听 GStreamer kvssink 的 `stream-status` 信号（HEALTHY/UNHEALTHY），但 KVS Producer SDK 内部的 `streamLatencyPressure` 回调不经过 GStreamer 信号。Pi 5 生产日志显示 SDK 层面 buffer 积压 67 秒但 BitrateAdapter 未触发降码率。需要将 `streamLatencyPressure` 回调接入 BitrateAdapter 作为额外的降码率触发源。（来源：spec-25 Pi 5 生产日志审查）

- **KVS 弱网优化**：Pi 5 到 KVS ap-southeast-1 的实际上传速度仅 211 KB/s（1.7 Mbps），远低于到 Cloudflare 的 1.4 MB/s（11 Mbps），导致 2.5 Mbps 编码码率持续积压、putMedia 连接反复断开重建（30 分钟内 10000+ 条 latency pressure）。需要：(1) kvssink 属性调优（avg-bandwidth-bps 匹配实际码率）→ **已纳入 Spec 32（avg-bandwidth-bps 跟随 default 码率）**；(2) 降低默认码率到上传速度以下（~1200 kbps）→ **已纳入 Spec 32（需求 6，default 1200kbps / max 1500kbps）**；(3) 评估换 region（东京/香港）是否改善路由（仍待办，Spec 32 明确不含换 region）；(4) 考虑 KVS_ONLY 模式下进一步降码率（仍待办）。（来源：spec-25 Pi 5 生产日志排查）

- **model/ 目录重组**：当前 `model/src/` 把采集（Spec 27）、清洗（Spec 28）、训练（Spec 29）的代码全混在一起。需要按数据流水线阶段重组为 `model/collection/`、`model/cleaning/`、`model/training/` 三个子目录，更新所有 import 路径、测试、S3 同步命令和 SageMaker 容器内路径。等 Spec 29 训练跑通后开独立 Spec 执行。（来源：spec-29 代码审查）

- **main.cpp 三路集成**：KVS（spec-8）、WebRTC（spec-12+13）、AI（spec-10）三路分支的模块都已就绪，但 main.cpp 仍使用 fakesink 占位。需要一个独立 Spec 统一修改 main.cpp：读取 config.toml → 构建各模块配置 → 创建 WebRtcSignaling/WebRtcMediaManager → 注册回调 → 传入 build_tee_pipeline。依赖 spec-8 + spec-13 + spec-10 全部完成。（来源：spec-13 review）→ **已纳入 Spec 13.5（main-integration），先集成 KVS + WebRTC 两路，AI 分支后续 spec-10 完成后再接入**

---

## 状态说明

- ⬜ 未开始
- 🔄 进行中
- ✅ 已完成
- ⏸️ 暂停

当前进度：spec-0 ~ spec-16 ✅（含 spec-4.5 ⬜、spec-13.6/13.7 ✅、spec-15.5 ✅）, spec-17 ~ spec-30 ✅（含 spec-21 ⬜）, spec-31 🔄, spec-32 🔄（Task 1-6 已完成并通过宿主机验证，Task 7 待生命周期 P0 修复后做 Pi 实测）, spec-33 ⬜（三件套已完成第三次 review 修订：消解 send deadline 自锁矛盾（决策 D 两级 deadline）、CallbackBridge 改固定池化免 SDK quiescence 证明（决策 B）、reconnect 默认 A2、新增 Task 0 取证与 Stage 1/2 分阶段部署归因；Task 0 取证 + Task 1 SDK 门禁可立即并行开始，后续编码受门禁结果约束，整机 72h 依赖 spec-34）, spec-34 ⬜（待创建：设备生命周期安全 P0）。下一步：Pi 可达后先跑 spec-33 Task 0 取证（scripts/pi-diagnose.sh）；创建并完成 spec-34；期间可并行收集 spec-33 Task 1 的 Pi SDK 证据；生命周期 P0 修复后再执行 spec-32 Task 7 与 spec-33 整机长稳验收。
