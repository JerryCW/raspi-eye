# Spec 路线图

基于 `product.md` 产品定义，按依赖顺序规划的完整 Spec 列表。
每个 Spec 遵循 meta-harness 方法论：3-7 个 Task、2-5 个文件、100-500 行新增代码。

## 依赖关系图

```
spec-0 → spec-1 → spec-2 → spec-3（摄像头抽象）→ spec-4（交叉编译）
                         │
                         ├→ spec-5（管道健康监控，仅依赖 spec-2）
                         │
                         ├→ spec-6（AWS infra）→ spec-7（IoT 认证）
                         │                        ├→ spec-8（KVS，也依赖 spec-5）
                         │                        ├→ spec-12（WebRTC 信令，也依赖 spec-5）→ spec-13（WebRTC 媒体）
                         │                        └→ spec-11（截图上传，也依赖 spec-10）
                         │
                         └→ spec-9（YOLO 检测）→ spec-10（AI 管道，也依赖 spec-5）→ spec-11（截图上传）

spec-8 + spec-13 → spec-14（自适应码率 + 流模式切换）
spec-14 → spec-15（零拷贝缓冲区）
spec-11 → spec-16（SageMaker endpoint）→ spec-17（Lambda 触发）
spec-15 → spec-18（systemd 看门狗）
spec-17 + spec-13 → spec-19（前端 MVP，可选）
```

---

## 阶段一：设备端骨架 + 双平台验证

从零搭建到 tee 分流管道 + 摄像头抽象 + Pi 5 交叉编译。阶段结束时 macOS 和 Pi 5 都能跑通测试。

| Spec | 名称 | 目标 | 依赖 | 模块 |
|------|------|------|------|------|
| 0 | gstreamer-capture | CMake 骨架 + PipelineManager（gst_parse_launch）+ 冒烟测试 | 无 | device |
| 1 | spdlog-logging | 结构化诊断基准：spdlog 替换 g_print，支持 JSON 单行非阻塞输出 | spec-0 | device |
| 2 | h264-tee-pipeline | H.264 编码 + tee 分流（3 路 fakesink 占位），手动构建管道 | spec-1 | device |
| 3 | camera-abstraction | 摄像头接口抽象层（videotestsrc / libcamera / V4L2 统一接口） | spec-2 | device |
| 4 | cross-compile | aarch64 交叉编译工具链 + Pi 5 部署脚本 + 双平台 CI 验证 | spec-3 | device |

为什么交叉编译放这里：摄像头抽象层提供了 libcamera/V4L2 的 Pi 5 实现，交叉编译可以立即验证。后续所有 Spec 双平台验证，尽早暴露平台差异。

## 阶段二：管道容错 + AWS 基础设施 + 认证

管道健康监控不依赖 AWS，可以和 infra 并行。认证体系为后续三路 sink 接入做准备。

| Spec | 名称 | 目标 | 依赖 | 模块 |
|------|------|------|------|------|
| 5 | pipeline-health | 管道健康监控 + 自动恢复（Element 错误检测、重建管道）+ 故障注入验证 | spec-2 | device |
| 6 | infra-core | AWS 基础设施 IaC：KVS 流、S3 桶、DynamoDB 表、IAM 角色 | spec-2 | infra |
| 7 | iot-credentials | AWS IoT Thing 注册 + X.509 证书 + STS 临时凭证获取 | spec-6 | device + infra |

spec-5 和 spec-6 可并行开发（一个 device，一个 infra）。管道健康监控是纯设备端逻辑，和 AWS 凭证无关。

## 阶段三：三路分支实际集成（可并行推进）

tee 的三条分支从 fakesink 替换为实际 sink。三条路线互相独立。

### 路线 A：KVS 录制

| Spec | 名称 | 目标 | 依赖 | 模块 |
|------|------|------|------|------|
| 8 | kvs-producer | KVS Producer SDK 集成，替换 kvs 分支的 fakesink | spec-7, spec-5 | device |

### 路线 B：AI 检测 + 截图上传

| Spec | 名称 | 目标 | 依赖 | 模块 |
|------|------|------|------|------|
| 9 | yolo-detector | ONNX Runtime + YOLOv11s 设备端目标检测（独立模块）+ 推理耗时与峰值内存基线采集 | spec-2 | device |
| 10 | ai-pipeline | AI 推理管道集成，替换 ai 分支的 fakesink，buffer probe 抽帧 + 检测 | spec-9, spec-5 | device |
| 11 | screenshot-uploader | 检测到目标后截图 + 上传 S3 | spec-10, spec-7 | device |

### 路线 C：WebRTC 实时观看

| Spec | 名称 | 目标 | 依赖 | 模块 |
|------|------|------|------|------|
| 12 | webrtc-signaling | KVS WebRTC 信令通道（macOS stub + Linux 实现） | spec-7, spec-5 | device |
| 13 | webrtc-media | WebRTC 媒体流，替换 webrtc 分支的 fakesink | spec-12 | device |

YOLO 检测器（spec-9）只依赖 spec-2：纯本地推理，不需要 AWS 凭证。AI 管道（spec-10）依赖 spec-5 因为需要健康监控保护 ai 分支。

## 阶段四：管道智能化与性能优化

| Spec | 名称 | 目标 | 依赖 | 模块 |
|------|------|------|------|------|
| 14 | adaptive-streaming | 自适应码率控制 + 流模式切换（FULL/KVS_ONLY/WEBRTC_ONLY/DEGRADED） | spec-8, spec-13 | device |
| 15 | zero-copy-buffers | 缓冲区零拷贝重构与预分配池化，目标 CPU 负载 ~26% @ 720p15 | spec-14 | device |

零拷贝在自适应码率之后：自适应码率会动态启停分支改变数据流路径，零拷贝需要在最终路径上优化。

## 阶段五：云端 AI 推理

| Spec | 名称 | 目标 | 依赖 | 模块 |
|------|------|------|------|------|
| 16 | sagemaker-endpoint | SageMaker Serverless 推理 endpoint（DINOv2 种类识别） | spec-11 | model + infra |
| 17 | lambda-trigger | S3 事件触发 Lambda → 调用 SageMaker → 结果写 DynamoDB | spec-16 | model + infra |

## 阶段六：部署运维

| Spec | 名称 | 目标 | 依赖 | 模块 |
|------|------|------|------|------|
| 18 | systemd-watchdog | systemd 服务集成 + 进程级看门狗（7×24 无人值守） | spec-15 | device |

## 阶段七：前端（可选，优先级低）

| Spec | 名称 | 目标 | 依赖 | 模块 |
|------|------|------|------|------|
| 19 | viewer-mvp | 前端 MVP：WebRTC 实时观看 + 事件列表浏览 | spec-17, spec-13 | viewer |

---

## 推荐执行顺序（单人串行）

```
0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 → 11 → 12 → 13 → 14 → 15 → 16 → 17 → 18 → 19
```

其中 spec-5 和 spec-6 可并行，spec-9 可在等待 spec-7 时提前开发。

理由：
- 0-4：从零到双平台可验证的 tee 分流管道
- 5-7：管道容错 + AWS 认证
- 8：KVS 录制（最核心数据通路）
- 9-11：YOLO + AI 管道 + 截图上传（核心功能）
- 12-13：WebRTC 实时观看
- 14-15：自适应码率 + 零拷贝性能优化
- 16-17：云端推理
- 18：systemd 看门狗
- 19：前端（可选）

---

## 延迟待办项

_从 Spec 执行过程中推迟的事项，创建新 Spec 前检查此列表。_

暂无。

---

## 状态说明

- ⬜ 未开始
- 🔄 进行中
- ✅ 已完成
- ⏸️ 暂停

当前所有 Spec 状态：⬜ 未开始
