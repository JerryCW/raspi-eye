---
inclusion: always
---

# 产品概述

Smart Camera 是一个运行在 Raspberry Pi 5 上的智能摄像头应用，核心功能是野生鸟类种类识别，未来扩展到其他动物。个人学习项目，目标可分享给同事复用。

## 核心流程

1. 摄像头采集视频流（单摄像头，通过 GStreamer tee 分出多路）
2. 一路 → KVS（Kinesis Video Streams）：云端录制与回放
3. 一路 → KVS WebRTC：低延迟实时观看（最多 10 个并发）
4. 一路 → AI 推理管道：设备端 YOLO 抽帧检测目标物（鸟、人、猫、狗等）
5. 检测到目标 → 截图 → 上传云端 → SageMaker Serverless 做种类识别（事件驱动，非实时，模型可灵活切换如 DINOv2）

## 关键特性

- 自适应码率控制（基于网络状况）
- 自动流模式切换（FULL / KVS_ONLY / WEBRTC_ONLY / DEGRADED）
- 管道健康监控与自动恢复
- 进程级看门狗（7×24 无人值守）
- 可扩展的 AI 模型（更换模型即可识别不同动物种类）
- AWS IoT X.509 证书认证（mTLS），STS 临时凭证，无硬编码密钥

## 硬件环境

- Raspberry Pi 5
- 摄像头：IMX678（USB，主力，V4L2 接入）+ IMX216（CSI，备用，libcamera 接入），需要灵活切换
- macOS 开发环境使用 videotestsrc 模拟

## 开发与部署

- 开发环境：macOS，使用 `videotestsrc` 模拟摄像头 + WebRTC stub
- 部署环境：Raspberry Pi 5（Linux aarch64）

## 当前阶段不包含

- 多摄像头支持（后期扩展）
- 实时云端推理（当前为事件驱动截图上传）
- 前端 UI / 移动端 App
