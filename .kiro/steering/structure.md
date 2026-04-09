---
inclusion: always
---

# 项目结构

```
raspi-eye/
├── device/              # 设备端 C++（Raspberry Pi 5）
│   ├── src/             # 源代码
│   │   ├── pipeline_manager.{h,cpp}   # 管道生命周期管理器（RAII）
│   │   ├── pipeline_builder.{h,cpp}   # 双 tee 管道构建器
│   │   ├── log_init.{h,cpp}           # 日志系统初始化/关闭
│   │   ├── json_formatter.{h,cpp}     # JSON 单行日志格式化器
│   │   └── main.cpp                   # 应用入口
│   ├── tests/           # GTest 测试
│   │   ├── smoke_test.cpp             # PipelineManager 冒烟测试
│   │   ├── log_test.cpp               # 日志系统测试（含 PBT）
│   │   └── tee_test.cpp               # 双 tee 管道冒烟测试
│   └── CMakeLists.txt
├── scripts/             # 构建与部署脚本
│   ├── pi-build.sh      # Pi 5 构建脚本（本地/SSH 远程双模式）
│   └── build-all.sh     # 双平台验证脚本（macOS Debug + Pi 5 Release）
├── docs/                # 文档
│   ├── development-trace.md  # Trace 归档（由 post-task hook 维护）
│   ├── spec-backlog.md       # Spec 路线图与延迟待办项
│   └── pi-setup.md           # Pi 5 环境配置文档
├── .kiro/               # AI 辅助开发
│   ├── hooks/           # Agent hooks
│   ├── specs/           # Spec 文件（spec-0 ~ spec-3）
│   └── steering/        # Steering 规则
├── .gitignore
├── viewer/              # 前端（待创建，部署到 AWS）
├── model/               # ML 全链路（待创建）
└── infra/               # 基础设施 IaC（待创建）
```

## 模块职责

| 模块 | 语言 | 部署目标 | 职责 | 状态 |
|------|------|---------|------|------|
| device | C++17 | Raspberry Pi 5 | 视频采集、编码、流分发、设备端 YOLO 检测 | 开发中 |
| scripts | Bash | macOS / Pi 5 | 构建脚本、双平台验证 | 已有 |
| docs | Markdown | — | 开发文档、环境配置、Spec 路线图 | 已有 |
| viewer | 待定 | AWS（ECS/S3） | 实时视频观看、事件浏览 | 待创建 |
| model | Python | SageMaker | 模型训练、推理 endpoint、Lambda 触发器 | 待创建 |
| infra | 待定 | AWS | IAM、S3、DynamoDB、SageMaker 等资源定义 | 待创建 |
