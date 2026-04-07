---
inclusion: always
---

# 项目结构

```
raspi-eye/
├── device/              # 设备端 C++（Raspberry Pi 5）
│   ├── src/             # 源代码
│   ├── tests/           # GTest 测试
│   └── CMakeLists.txt
├── viewer/              # 前端（部署到 AWS）
├── model/               # ML 全链路
│   ├── training/        # 训练脚本
│   ├── inference/       # SageMaker endpoint 推理代码
│   ├── lambda/          # Lambda 函数（调 SageMaker 的胶水逻辑）
│   └── deploy/          # 模型部署脚本、标签文件
├── infra/               # 基础设施（IaC + IAM）
├── docs/                # 文档
│   ├── development-trace.md  # Trace 归档（由 post-task hook 维护）
│   └── spec-backlog.md       # Spec 待办池（推迟到后续 Spec 的事项）
├── .kiro/               # AI 辅助开发
│   ├── hooks/           # Agent hooks
│   ├── specs/           # Spec 文件
│   └── steering/        # Steering 规则
└── README.md
```

## 模块职责

| 模块 | 语言 | 部署目标 | 职责 |
|------|------|---------|------|
| device | C++17 | Raspberry Pi 5 | 视频采集、编码、流分发、设备端 YOLO 检测 |
| viewer | 待定 | AWS（ECS/S3） | 实时视频观看、事件浏览 |
| model | Python | SageMaker | 模型训练、推理 endpoint、Lambda 触发器 |
| infra | 待定 | AWS | IAM、S3、DynamoDB、SageMaker 等资源定义 |
