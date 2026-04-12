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
| 10 | ai-pipeline | AI 推理管道集成，替换 ai 分支的 fakesink，buffer probe 抽帧 + 检测 | spec-9(.5), spec-5 | device | ⬜ |
| 11 | screenshot-uploader | 检测到目标后截图 + 上传 S3（libcurl + S3 REST API + SigV4 签名） | spec-10, spec-7 | device | 🔄 |

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
| 15 | adaptive-streaming | 自适应码率控制 + 流模式切换（FULL/KVS_ONLY/WEBRTC_ONLY/DEGRADED） | spec-8, spec-13 | device | ✅ |
| 16 | zero-copy-buffers | 缓冲区零拷贝重构与预分配池化，目标 CPU 负载 ~26% @ 720p15 | spec-15 | device | ⬜ |

零拷贝在自适应码率之后：自适应码率会动态启停分支改变数据流路径，零拷贝需要在最终路径上优化。

## 阶段五：云端 AI 推理

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 17 | sagemaker-endpoint | SageMaker Serverless 推理 endpoint（DINOv2 种类识别） | spec-11 | model + infra | ⬜ |
| 18 | lambda-trigger | S3 事件触发 Lambda → 调用 SageMaker → 结果写 DynamoDB | spec-17 | model + infra | ⬜ |

## 阶段六：部署运维

| Spec | 名称 | 目标 | 依赖 | 模块 | 状态 |
|------|------|------|------|------|------|
| 19 | config-file | ConfigManager 统一配置加载：TOML 解析 camera/streaming/logging section，命令行覆盖，三层优先级 | spec-7 | device | ✅ |
| 20 | systemd-watchdog | systemd 服务集成 + 进程级看门狗（7×24 无人值守），配置文件路径通过 systemd unit 指定 | spec-19 | device + scripts | ⬜ |
| 22 | pi-deploy | Pi 5 部署自动化：build + install + systemctl restart 一键脚本，config.toml 初始化部署 | spec-20 | scripts | ⬜ |

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

- **broadcast_frame 异步帧分发**：当前 broadcast_frame 在 GStreamer streaming 线程中同步调用 writeFrame，持锁遍历所有 peer。如果 Pi 5 端到端验证发现 writeFrame 有阻塞行为（网络拥塞时），需要改为异步队列分发。等 spec-14 或 spec-15 根据实际性能数据决定。（来源：spec-13 review）

- **main.cpp 三路集成**：KVS（spec-8）、WebRTC（spec-12+13）、AI（spec-10）三路分支的模块都已就绪，但 main.cpp 仍使用 fakesink 占位。需要一个独立 Spec 统一修改 main.cpp：读取 config.toml → 构建各模块配置 → 创建 WebRtcSignaling/WebRtcMediaManager → 注册回调 → 传入 build_tee_pipeline。依赖 spec-8 + spec-13 + spec-10 全部完成。（来源：spec-13 review）→ **已纳入 Spec 13.5（main-integration），先集成 KVS + WebRTC 两路，AI 分支后续 spec-10 完成后再接入**

---

## 状态说明

- ⬜ 未开始
- 🔄 进行中
- ✅ 已完成
- ⏸️ 暂停

当前进度：spec-0 ~ spec-9.5 ✅, spec-12 ~ spec-15 ✅, spec-16 ✅（shutdown fix）, spec-19 ✅（config-file）已完成，下一个 spec-20（systemd-watchdog）→ spec-22（pi-deploy）
