# 需求文档：Spec 17 — 云端推理链路（SageMaker Endpoint + Lambda + DynamoDB）

## 简介

本 Spec 实现完整的云端推理链路：设备端 YOLO 检测到鸟类后截图上传 S3（Spec 11）→ S3 事件通知触发 Lambda → Lambda 读取图片调用 SageMaker Serverless endpoint 推理 → 推理结果写入 DynamoDB。

具体包含五个部分：

1. **SageMaker Endpoint（Serverless + Real-time 双模式）**：部署 Spec 29 训练产出的鸟类分类模型（DINOv3-ViT-L/16 backbone + linear head，46 种鸟类），编写自包含推理脚本（`inference.py`，不依赖 training 模块）、打包 `model.tar.gz`、通过 boto3 三步部署。支持 Serverless（6GB 内存，冷启动约 56 秒）和 Real-time（ml.m5.large）双模式
2. **Lambda 函数**：接收 S3 ObjectCreated 事件，从 S3 读取 `event.json`，解析事件元数据，读取截图图片，调用 SageMaker endpoint 推理，将结果通过 `update_item` 写回已有的 `raspi-eye-events` DynamoDB 表
3. **DynamoDB 写回**：不创建新表，改为用 `update_item` 写回 `raspi-eye-events` 表（PK: device_id + start_time），推理结果字段加 `inference_` 前缀（inference_species、inference_confidence 等）
4. **S3 事件通知**：配置 S3 bucket 的事件通知规则，当 `event.json` 上传时触发 Lambda
5. **一键部署脚本 + 端到端验证**：部署整条链路并验证完整流程

Serverless Inference 适合本项目的事件驱动场景：流量稀疏且不规律，按调用计费、自动缩放到 0，无需维护常驻实例。Real-time endpoint（ml.m5.large）适合调试和低延迟场景。Lambda 同样按调用计费，与 S3 事件通知天然集成。

### 实际部署经验总结

- **inference.py 完全自包含**：不依赖 training 模块（backbone_registry.py、classifier.py、augmentation.py），因为 SageMaker 容器内 model.tar.gz 的 code/ 目录里没有这些文件。内联了 `_get_val_transform`、`_BirdClassifier`、`_create_backbone_offline`
- **模型加载方式**：不用 `BirdClassifier(backbone_config=...)` 构造（会触发 `from_pretrained` 下载 1.2GB），改用 `AutoConfig.from_pretrained`（只下载几KB config.json）+ `AutoModel.from_config`（创建空结构）+ `load_state_dict`（加载权重）
- **DINOv3 模型类**：DINOv3 用的是 `DINOv3ViTConfig` + `DINOv3ViTModel`，不是 `Dinov2Config`
- **S3 跨 region 限制**：model.tar.gz 必须和 endpoint 在同一 region。模型数据 bucket (raspi-eye-model-data) 在 us-east-1，endpoint 在 ap-southeast-1，需要把 model.tar.gz 复制到新加坡的截图 bucket
- **HF_TOKEN 获取**：deploy_endpoint.py 从 Secrets Manager (us-east-1) 获取 HF_TOKEN 注入 SageMaker Model 环境变量，inference.py 通过环境变量读取

本 Spec 合并了原 backlog 中 spec-17（SageMaker endpoint）和 spec-18（Lambda 触发 + DynamoDB 写入）的内容，形成完整的云端推理链路。

## 前置条件

- Spec 29（bird-classifier-training）已完成，产出模型文件：
  - `s3://raspi-eye-model-data/training/models/dinov3-vitl16/bird_classifier.pt`
  - `s3://raspi-eye-model-data/training/models/dinov3-vitl16/class_names.json`
- Spec 11（s3-uploader）已完成，设备端截图上传到 S3 bucket
- 模型 .pt 文件格式：`torch.save({"state_dict": ..., "metadata": {"backbone_name", "num_classes", "class_names", "input_size", "feature_dim"}})`
- Python >= 3.11 环境已就绪（`.venv-raspi-eye/`）
- HuggingFace token 存储在 Secrets Manager: `raspi-eye/huggingface-token`（DINOv3 gated model 访问）
- SageMaker Role: `arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role`
- AWS Region: `ap-southeast-1`
- S3 bucket（设备截图）: `raspi-eye-captures-014498626607-ap-southeast-1-an`
- S3 截图路径格式（Spec 11 定义）: `{device_id}/{date}/{event_id}/{filename}`（如 `RaspiEyeAlpha/2026-04-12/evt_20260412_153045/event.json`）

## 术语表

- **Inference_Script**：推理脚本（`inference.py`），实现 SageMaker PyTorch Inference Toolkit 要求的四个钩子函数（`model_fn`、`input_fn`、`predict_fn`、`output_fn`），完全自包含（不依赖 training 模块），在 SageMaker 推理容器内执行
- **Model_Packager**：模型打包模块，将 .pt 模型文件和推理代码打包为 `model.tar.gz` 格式，上传到 S3
- **Endpoint_Deployer**：endpoint 部署脚本（`model/deploy_endpoint.py`），使用 boto3 创建 SageMaker Model -> EndpointConfig -> Endpoint 三步部署
- **Serverless_Config**：SageMaker Serverless Inference 的配置参数，包括 `MemorySizeInMB`（内存大小）和 `MaxConcurrency`（最大并发数）
- **model.tar.gz**：SageMaker 推理容器期望的模型打包格式，解压后根目录包含模型文件，`code/` 子目录包含推理脚本和依赖声明
- **PyTorch_Inference_Container**：SageMaker 预构建的 PyTorch 推理容器（`pytorch-inference:2.6.0-cpu-py312-ubuntu22.04-sagemaker`），内置 TorchServe + SageMaker PyTorch Inference Toolkit
- **Cold_Start**：Serverless endpoint 在无流量时缩放到 0，首次请求需要启动容器并加载模型的延迟
- **Inference_Lambda**：Lambda 函数，接收 S3 事件通知，读取截图，调用 SageMaker endpoint 推理，将结果写入 DynamoDB
- **Results_Table**：DynamoDB 表（`raspi-eye-events`），Lambda 通过 `update_item` 写回推理结果（字段加 `inference_` 前缀）
- **S3_Event_Notification**：S3 bucket 的事件通知配置，当指定前缀和后缀的对象创建时触发 Lambda
- **Deploy_Script**：一键部署脚本（`scripts/deploy-inference-pipeline.sh`），部署整条云端推理链路
- **event.json**：Spec 10/11 定义的事件元数据文件，包含 event_id、device_id、start_time、detections_summary 等字段，上传到 S3 后作为 Lambda 触发的信号文件

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言 | Python >= 3.11 |
| 环境管理 | venv（`.venv-raspi-eye/`），所有命令必须先 `source .venv-raspi-eye/bin/activate` |
| 测试框架 | pytest + Hypothesis（PBT） |
| 代码目录 | `model/endpoint/`（推理脚本和打包逻辑）、`model/lambda/`（Lambda 函数代码） |
| 部署脚本入口 | `model/deploy_endpoint.py`（endpoint 部署）、`scripts/deploy-inference-pipeline.sh`（全链路部署） |
| 推理容器 | PyTorch 2.6 CPU 预构建推理容器（`pytorch-inference:2.6.0-cpu-py312-ubuntu22.04-sagemaker`） |
| Serverless 内存 | 6144 MB（DINOv3 ViT-L/16 模型约 1.2GB，加载后内存占用约 3-4GB，6GB 是 Serverless 最大可选值，冷启动约 56 秒） |
| Serverless 最大并发 | 5（鸟类检测事件频率低，5 并发足够） |
| Real-time 实例 | ml.m5.large（调试和低延迟场景，无冷启动超时问题） |
| Lambda 运行时 | Python 3.12 |
| Lambda 内存 | 256 MB（仅做 S3 读取 + SageMaker invoke + DynamoDB 写入，不做图片处理） |
| Lambda 超时 | 120 秒（含 SageMaker 冷启动等待） |
| DynamoDB 写回方式 | update_item 写回已有的 `raspi-eye-events` 表（PK: device_id + start_time），不创建新表 |
| 模型来源 | `s3://raspi-eye-model-data/training/models/{backbone_name}/bird_classifier.pt` |
| S3 bucket（模型数据） | `raspi-eye-model-data`（us-east-1，model.tar.gz 需复制到 ap-southeast-1 的截图 bucket） |
| S3 bucket（设备截图） | `raspi-eye-captures-014498626607-ap-southeast-1-an` |
| AWS Region | `ap-southeast-1` |
| 推理输入 | JPEG 图片二进制数据（Content-Type: `image/jpeg` 或 `application/x-image`） |
| 推理输出 | JSON（Content-Type: `application/json`），包含 top-k 预测结果 |
| 涉及文件 | 10-15 个 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中修改 Spec 29 的训练代码或模型格式
- SHALL NOT 在本 Spec 中修改 Spec 11 的设备端 S3 上传逻辑
- SHALL NOT 使用 GPU 推理容器（Serverless Inference 仅支持 CPU，且 DINOv3 推理延迟在 CPU 上可接受）
- SHALL NOT 在 Lambda 函数中做图片预处理或 AI 推理（图片处理和推理全部由 SageMaker endpoint 完成，Lambda 仅做编排）
- SHALL NOT 在 DynamoDB 表中存储图片二进制数据（仅存储 S3 key 引用）

### Design 层

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、Role ARN 或 HF_TOKEN（通过环境变量或 Secrets Manager 获取）
- SHALL NOT 在日志中打印 HF_TOKEN、AWS 凭证等敏感信息
- SHALL NOT 使用 `ContainerEntrypoint` 覆盖 SageMaker 预构建容器的默认入口脚本（来源：spec-29 经验，覆盖会导致 Inference Toolkit 不初始化）
- SHALL NOT 在推理脚本中使用 OpenCV 做图片处理（使用 Pillow + torchvision transforms，保持与 Spec 29 一致）
- SHALL NOT 在 Lambda 中使用 SageMaker Python SDK（仅用 boto3 的 `sagemaker-runtime` 调用 `invoke_endpoint`，Lambda 部署包保持轻量）

### Tasks 层

- SHALL NOT 直接使用系统 `python` 或 `python3` 执行项目 Python 代码或测试，必须先 `source .venv-raspi-eye/bin/activate`
- SHALL NOT 将模型权重文件（.pt、model.tar.gz）提交到 git（通过 S3 管理）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 将新建文件直接放到项目根目录（所有文件放 `model/` 或 `scripts/` 目录下）
- SHALL NOT 在子代理最终检查点执行 git commit
- SHALL NOT 在不确定 SageMaker PyTorch Inference Toolkit API 用法时凭猜测编写代码（先查阅官方文档确认 `model_fn`/`input_fn`/`predict_fn`/`output_fn` 的签名和行为）
- SHALL NOT 在不确定 AWS Lambda + S3 事件通知 API 用法时凭猜测编写代码（先查阅官方文档确认事件格式和 API）

## 需求

### 需求 1：推理脚本（inference.py）

**用户故事：** 作为开发者，我需要编写符合 SageMaker PyTorch Inference Toolkit 规范的推理脚本，使 endpoint 能够接收 JPEG 图片并返回鸟类分类结果。

#### 验收标准

1. THE Inference_Script SHALL 实现 `model_fn(model_dir)` 函数，从 `model_dir` 加载 `bird_classifier.pt`，读取元数据（`backbone_name`、`input_size`、`class_names`），使用内联的 `_create_backbone_offline` 函数通过 `AutoConfig.from_pretrained`（下载 config.json）+ `AutoModel.from_config`（创建空模型结构）加载 backbone，重建内联的 `_BirdClassifier` 模型，加载 state_dict，设置为 eval 模式，返回包含模型和元数据的字典
2. WHEN `model_fn` 加载模型时，THE Inference_Script SHALL 通过环境变量 `HF_TOKEN` 获取 HuggingFace token（由 deploy_endpoint.py 从 Secrets Manager 注入到 SageMaker Model 环境变量）
3. THE Inference_Script SHALL 实现 `input_fn(request_body, content_type)` 函数，接受 `image/jpeg` 或 `application/x-image` 类型的二进制图片数据，使用 Pillow 解码为 RGB 图片，应用与 Spec 29 验证集相同的预处理（Resize + CenterCrop + Normalize），返回预处理后的张量
4. IF `input_fn` 收到不支持的 content_type，THEN THE Inference_Script SHALL 抛出 ValueError 并返回描述性错误信息
5. IF `input_fn` 收到无法解码的图片数据，THEN THE Inference_Script SHALL 抛出 ValueError 并返回描述性错误信息
6. THE Inference_Script SHALL 实现 `predict_fn(input_data, model_dict)` 函数，使用模型对预处理后的张量执行推理（`torch.no_grad`），计算 softmax 概率，返回 top-5 预测结果（物种名 + 置信度）
7. THE Inference_Script SHALL 实现 `output_fn(prediction, accept)` 函数，将预测结果序列化为 JSON 格式，包含 `predictions` 列表（每项含 `species` 和 `confidence`）和 `model_metadata`（backbone 名称、类别数）
8. THE Inference_Script SHALL 位于 `model/endpoint/inference.py`，完全自包含（不依赖 training 模块），可独立于 SageMaker 容器在本地测试

### 需求 2：模型打包（model.tar.gz）

**用户故事：** 作为开发者，我需要将模型文件和推理代码打包为 SageMaker 期望的 `model.tar.gz` 格式，以便部署到 Serverless endpoint。

#### 验收标准

1. THE Model_Packager SHALL 将以下文件打包为 `model.tar.gz`：
   - 根目录：`bird_classifier.pt`（模型文件）、`class_names.json`（类别映射）
   - `code/` 子目录：`inference.py`（推理脚本）、`requirements.txt`（推理依赖）
2. THE Model_Packager SHALL 在 `code/requirements.txt` 中声明推理所需的额外依赖（`transformers>=4.56`、`Pillow`），SageMaker 推理容器启动时自动安装
3. THE Model_Packager SHALL 支持从本地 .pt 文件或 S3 路径获取模型文件
4. THE Model_Packager SHALL 将打包好的 `model.tar.gz` 上传到 `s3://raspi-eye-model-data/endpoint/{backbone_name}/model.tar.gz`
5. WHEN 打包完成后，THE Model_Packager SHALL 打印 tar.gz 文件大小和 S3 上传路径

### 需求 3：Endpoint 部署脚本

**用户故事：** 作为开发者，我需要通过脚本一键完成 SageMaker Serverless endpoint 的创建和部署，包括 Model、EndpointConfig 和 Endpoint 三个资源。

#### 验收标准

1. THE Endpoint_Deployer SHALL 提供 Python 脚本 `model/deploy_endpoint.py`，使用 boto3 完成三步部署：`create_model` -> `create_endpoint_config` -> `create_endpoint`
2. THE Endpoint_Deployer SHALL 使用 PyTorch 2.6 CPU 预构建推理容器（`pytorch-inference:2.6.0-cpu-py312-ubuntu22.04-sagemaker`）
3. THE Endpoint_Deployer SHALL 支持 Serverless 和 Real-time 双模式：通过 `--serverless` 开关选择 Serverless（`MemorySizeInMB=6144`、`MaxConcurrency=5`），默认为 Real-time（`--instance-type ml.m5.large`、`--instance-count 1`）
4. THE Endpoint_Deployer SHALL 支持通过 CLI 参数配置：`--backbone`（默认 `dinov3-vitl16`，决定模型路径）、`--s3-bucket`、`--role`、`--region`、`--memory-size`（默认 6144）、`--max-concurrency`（默认 5）、`--endpoint-name`（默认 `raspi-eye-bird-classifier`）、`--instance-type`（默认 `ml.m5.large`）、`--instance-count`（默认 1）、`--serverless`
5. THE Endpoint_Deployer SHALL 在部署前自动执行模型打包和上传（需求 2），除非指定 `--skip-package` 跳过打包步骤
6. THE Endpoint_Deployer SHALL 支持 `--wait` 参数，等待 endpoint 变为 InService 状态，打印最终状态和 endpoint 名称
7. WHEN endpoint 已存在时，THE Endpoint_Deployer SHALL 支持 `--update` 参数，更新现有 endpoint 的模型（创建新 EndpointConfig -> 调用 `update_endpoint`），而非报错退出
8. THE Endpoint_Deployer SHALL 在部署完成后打印 endpoint 名称和调用示例命令
9. THE Endpoint_Deployer SHALL 支持 `--delete` 参数，删除指定 endpoint 及其关联的 EndpointConfig 和 Model 资源
10. IF `create_endpoint` 失败，THEN THE Endpoint_Deployer SHALL 打印完整错误信息和 CloudWatch Logs 链接

### 需求 4：Endpoint 调用验证

**用户故事：** 作为开发者，我需要验证部署的 endpoint 能够正确接收图片并返回分类结果，确保端到端推理链路正常。

#### 验收标准

1. THE Endpoint_Deployer SHALL 支持 `--test` 参数，部署完成后使用 `model/samples/` 目录下的样本图片调用 endpoint 验证
2. WHEN `--test` 执行时，THE Endpoint_Deployer SHALL 使用 `sagemaker-runtime` 的 `invoke_endpoint` API 发送 JPEG 图片，打印返回的 top-5 预测结果（物种名 + 置信度）
3. WHEN `--test` 执行时，THE Endpoint_Deployer SHALL 记录并打印推理延迟（包含冷启动时间）

### 需求 5：Endpoint 单元测试

**用户故事：** 作为开发者，我需要通过本地单元测试验证推理脚本和打包逻辑的正确性，确保代码在部署到 SageMaker 之前没有逻辑 bug。

#### 验收标准

1. THE Test_Suite SHALL 包含 `model_fn` 测试：使用 MockBackbone 验证模型加载、state_dict 恢复、eval 模式设置
2. THE Test_Suite SHALL 包含 `input_fn` 测试：验证 JPEG 解码、预处理输出尺寸、不支持的 content_type 抛出 ValueError、损坏图片抛出 ValueError
3. THE Test_Suite SHALL 包含 `predict_fn` 测试：验证输出包含 top-5 预测、每项含 species 和 confidence、confidence 之和 <= 1.0（softmax 属性）
4. THE Test_Suite SHALL 包含 `output_fn` 测试：验证 JSON 序列化格式正确、包含 predictions 和 model_metadata 字段
5. THE Test_Suite SHALL 包含 `model.tar.gz` 打包测试：验证 tar.gz 内部结构正确（根目录含 bird_classifier.pt 和 class_names.json，code/ 含 inference.py 和 requirements.txt）
6. THE Test_Suite SHALL 包含推理 round-trip 属性测试（PBT）：对任意合法 JPEG 图片（随机尺寸 [32, 2048]），经过 `input_fn` -> `predict_fn` -> `output_fn` 完整链路后，输出的 JSON 恒包含 `predictions` 列表且长度 <= 5，每项 `confidence` 在 (0.0, 1.0]
7. THE Test_Suite SHALL 不依赖 GPU、网络或真实模型权重（使用 MockBackbone），确保本地 CPU 环境可运行
8. THE Test_Suite SHALL 使用 pytest 运行，命令为 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_endpoint.py -v`

### 需求 6：Lambda 函数

**用户故事：** 作为开发者，我需要一个 Lambda 函数，当设备截图上传到 S3 后自动触发，读取截图调用 SageMaker endpoint 推理，并将结果写入 DynamoDB，实现全自动的云端推理链路。

#### 验收标准

1. THE Inference_Lambda SHALL 接收 S3 ObjectCreated 事件，从事件记录中提取 bucket 名称和 object key
2. WHEN 触发的 object key 以 `event.json` 结尾时，THE Inference_Lambda SHALL 从 S3 读取该 `event.json` 文件，解析出 `event_id`、`device_id`、`start_time`、`detections_summary` 等元数据
3. THE Inference_Lambda SHALL 从 `event.json` 的 `snapshots` 数组中获取截图文件名列表，拼接 event.json 所在目录前缀构造完整 S3 key，逐张从 S3 读取 JPEG 图片
4. THE Inference_Lambda SHALL 对每张 JPEG 图片调用 SageMaker endpoint（`invoke_endpoint`，ContentType `image/jpeg`），获取 top-5 预测结果
5. THE Inference_Lambda SHALL 从所有图片的推理结果中选取置信度最高的预测作为该事件的最终分类结果
6. THE Inference_Lambda SHALL 将推理结果通过 `update_item` 写回已有的 `raspi-eye-events` DynamoDB 表（PK: device_id + start_time），推理结果字段加 `inference_` 前缀：`inference_species`（最高置信度物种名）、`inference_confidence`（最高置信度值）、`inference_image_key`（最高置信度对应的 S3 图片 key）、`inference_top5`（top-5 列表）、`inference_latency_ms`（推理耗时）
7. IF SageMaker endpoint 调用失败（超时、endpoint 不存在等），THEN THE Inference_Lambda SHALL 记录错误日志并将错误信息写入 DynamoDB 记录的 `inference_error` 字段，不抛出异常（避免 S3 事件重试风暴）
8. IF event.json 解析失败或格式不符合预期，THEN THE Inference_Lambda SHALL 记录警告日志并跳过该事件
9. THE Inference_Lambda SHALL 通过环境变量获取配置：`ENDPOINT_NAME`（SageMaker endpoint 名称）、`TABLE_NAME`（DynamoDB 表名，默认 `raspi-eye-events`）
10. THE Inference_Lambda SHALL 位于 `model/lambda/handler.py`，可独立于 AWS Lambda 环境在本地测试

### 需求 7：Lambda 执行角色

**用户故事：** 作为开发者，我需要为 Lambda 函数创建具有最小权限的 IAM 执行角色，确保 Lambda 仅能访问所需的 AWS 资源。

#### 验收标准

1. THE Deploy_Script SHALL 创建 Lambda 执行角色 `raspi-eye-inference-lambda-role`，信任策略允许 `lambda.amazonaws.com` 服务承担该角色
2. THE Deploy_Script SHALL 为该角色附加以下权限策略：S3 只读（`s3:GetObject`，资源限定为截图 bucket）、SageMaker 调用（`sagemaker:InvokeEndpoint`，资源限定为 endpoint）、DynamoDB 更新（`dynamodb:UpdateItem`，资源限定为 `raspi-eye-events` 表）、CloudWatch Logs（`logs:CreateLogGroup`、`logs:CreateLogStream`、`logs:PutLogEvents`）
3. THE Deploy_Script SHALL 使用内联策略（inline policy）而非托管策略，确保权限与角色生命周期一致

### 需求 8：DynamoDB 写回（使用已有表）

**用户故事：** 作为开发者，推理结果写回已有的 `raspi-eye-events` 表，不创建新表。

#### 验收标准

1. THE Inference_Lambda SHALL 使用 `update_item` 写回 `raspi-eye-events` 表（PK: device_id HASH + start_time RANGE），推理结果字段加 `inference_` 前缀
2. THE Deploy_Script SHALL 不创建或删除 DynamoDB 表（表由设备端事件流程管理）
3. THE Lambda IAM 策略 SHALL 仅包含 `dynamodb:UpdateItem` 权限（不需要 CreateTable、PutItem 等）

### 需求 9：S3 事件通知配置

**用户故事：** 作为开发者，我需要配置 S3 bucket 的事件通知，当设备截图的 event.json 上传完成时自动触发 Lambda 函数。

#### 验收标准

1. THE Deploy_Script SHALL 在截图 bucket（`raspi-eye-captures-014498626607-ap-southeast-1-an`）上配置 S3 事件通知，事件类型为 `s3:ObjectCreated:*`
2. THE S3_Event_Notification SHALL 配置后缀过滤器为 `event.json`，仅当 event.json 文件上传时触发 Lambda（避免每张 JPEG 图片都触发）
3. THE Deploy_Script SHALL 为 Lambda 函数添加 resource-based policy，允许 S3 服务（`s3.amazonaws.com`）调用该 Lambda，source ARN 限定为截图 bucket
4. IF S3 事件通知已存在，THEN THE Deploy_Script SHALL 更新现有配置而非追加，避免重复触发

### 需求 10：一键部署脚本

**用户故事：** 作为开发者，我需要一个脚本一键部署整条云端推理链路（SageMaker endpoint + Lambda + DynamoDB + S3 事件通知），简化部署流程。

#### 验收标准

1. THE Deploy_Script SHALL 提供 `scripts/deploy-inference-pipeline.sh`，按以下顺序部署：(1) SageMaker endpoint（调用 `model/deploy_endpoint.py`）(2) Lambda 执行角色 (3) Lambda 函数 (4) S3 事件通知
2. THE Deploy_Script SHALL 支持 `--skip-endpoint` 参数，跳过 SageMaker endpoint 部署（endpoint 已存在时使用）
3. THE Deploy_Script SHALL 支持 `--delete` 参数，按逆序删除所有资源：S3 事件通知 -> Lambda -> IAM 角色 -> SageMaker endpoint（不删除 DynamoDB 表）
4. THE Deploy_Script SHALL 在每个步骤完成后打印状态信息，部署完成后打印所有资源的 ARN 和名称汇总
5. IF 任何步骤失败，THEN THE Deploy_Script SHALL 打印错误信息并停止后续步骤（不自动回滚，由用户决定）

### 需求 11：端到端验证

**用户故事：** 作为开发者，我需要验证完整的云端推理链路：上传测试图片到 S3 -> Lambda 自动触发 -> SageMaker 推理 -> DynamoDB 写入结果。

#### 验收标准

1. THE Deploy_Script SHALL 支持 `--e2e-test` 参数，执行端到端验证流程
2. WHEN `--e2e-test` 执行时，THE Deploy_Script SHALL 构造一个模拟事件目录（包含 event.json + 测试 JPEG 图片），上传到截图 bucket 的测试路径（`e2e-test/{timestamp}/`）
3. WHEN `--e2e-test` 执行时，THE Deploy_Script SHALL 等待最多 180 秒（含 SageMaker 冷启动），轮询 DynamoDB 表检查对应 event_id 的记录是否已写入
4. WHEN DynamoDB 记录写入成功时，THE Deploy_Script SHALL 打印推理结果（species、confidence、inference_latency_ms）并报告验证通过
5. IF 180 秒内未检测到 DynamoDB 记录，THEN THE Deploy_Script SHALL 打印 Lambda CloudWatch Logs 链接并报告验证失败
6. WHEN `--e2e-test` 完成后，THE Deploy_Script SHALL 清理测试数据（删除 S3 测试对象和 DynamoDB 测试记录）

### 需求 12：Lambda 单元测试

**用户故事：** 作为开发者，我需要通过本地单元测试验证 Lambda 函数的逻辑正确性，确保事件解析、SageMaker 调用、DynamoDB 写入的逻辑在部署前没有 bug。

#### 验收标准

1. THE Test_Suite SHALL 包含 S3 事件解析测试：验证从 S3 事件记录中正确提取 bucket 和 key
2. THE Test_Suite SHALL 包含 event.json 解析测试：验证正确提取 event_id、device_id、start_time、detections_summary、snapshots 列表，以及 snapshots 文件名拼接为完整 S3 key 的逻辑
3. THE Test_Suite SHALL 包含 SageMaker 调用 mock 测试：使用 moto 或手动 mock `invoke_endpoint`，验证请求参数正确、响应解析正确
4. THE Test_Suite SHALL 包含 DynamoDB 写入 mock 测试：使用 moto 或手动 mock `update_item`，验证写入的记录包含所有必需字段（inference_ 前缀）
5. THE Test_Suite SHALL 包含错误处理测试：SageMaker 调用超时时记录错误但不抛异常、event.json 格式错误时跳过
6. THE Test_Suite SHALL 包含最佳预测选择属性测试（PBT）：对任意数量的图片推理结果（每个含 top-5 预测），选出的最终结果的 confidence 恒为所有图片所有预测中的最大值
7. THE Test_Suite SHALL 不依赖真实 AWS 服务（使用 mock），确保本地环境可运行
8. THE Test_Suite SHALL 使用 pytest 运行，命令为 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_lambda.py -v`

## 参考代码

### model.tar.gz 目录结构

```
model.tar.gz
+-- bird_classifier.pt          # 模型文件（state_dict + 元数据）
+-- class_names.json            # 类别映射 {index: species_name}
+-- code/                       # 推理代码目录
    +-- inference.py            # 推理脚本（model_fn/input_fn/predict_fn/output_fn）
    +-- requirements.txt        # 推理依赖（transformers>=4.56, Pillow）
```

### SageMaker PyTorch Inference Toolkit 钩子函数签名

```python
def model_fn(model_dir: str) -> dict:
    """加载模型。model_dir 是 model.tar.gz 解压后的根目录路径。"""

def input_fn(request_body: bytes, content_type: str) -> torch.Tensor:
    """反序列化输入数据。request_body 是原始请求体。"""

def predict_fn(input_data: torch.Tensor, model_dict: dict) -> dict:
    """执行推理。input_data 来自 input_fn，model_dict 来自 model_fn。"""

def output_fn(prediction: dict, accept: str) -> tuple[str, str]:
    """序列化输出。返回 (response_body, content_type)。"""
```

### S3 事件通知 Lambda 事件格式

```json
{
    "Records": [{
        "s3": {
            "bucket": {"name": "raspi-eye-captures-014498626607-ap-southeast-1-an"},
            "object": {"key": "RaspiEyeAlpha/2026-04-12/evt_20260412_153045/event.json"}
        }
    }]
}
```

### event.json 结构（Spec 10/11 定义，Lambda 只读）

```json
{
    "event_id": "evt_20260412_153045",
    "device_id": "RaspiEyeAlpha",
    "start_time": "2026-04-12T15:30:45Z",
    "end_time": "2026-04-12T15:31:15Z",
    "frame_count": 8,
    "kvs_stream_name": "RaspiEyeAlphaStream",
    "kvs_region": "ap-southeast-1",
    "detections_summary": {
        "bird": {"count": 8, "max_confidence": 0.92},
        "cat": {"count": 3, "max_confidence": 0.78}
    },
    "snapshots": [
        "20260412_153046_001.jpg",
        "20260412_153047_002.jpg"
    ]
}
```

### DynamoDB 记录结构（update_item 写回 raspi-eye-events 表）

```json
{
    "device_id": "RaspiEyeAlpha",
    "start_time": "2026-04-12T15:30:45Z",
    "inference_species": "Passer montanus",
    "inference_confidence": 0.92,
    "inference_image_key": "RaspiEyeAlpha/2026-04-12/evt_20260412_153045/20260412_153046_001.jpg",
    "inference_top5": [
        {"species": "Passer montanus", "confidence": 0.92},
        {"species": "Pycnonotus sinensis", "confidence": 0.05},
        {"species": "Zosterops japonicus", "confidence": 0.02},
        {"species": "Passer cinnamomeus", "confidence": 0.005},
        {"species": "Lonchura striata", "confidence": 0.003}
    ],
    "inference_latency_ms": 3200,
    "inference_error": null
}
```

### Lambda 函数核心逻辑示例

```python
import json
import time
import boto3

s3_client = boto3.client("s3")
sm_runtime = boto3.client("sagemaker-runtime")
dynamodb = boto3.resource("dynamodb")

def handler(event, context):
    for record in event["Records"]:
        bucket = record["s3"]["bucket"]["name"]
        key = record["s3"]["object"]["key"]
        if not key.endswith("event.json"):
            continue
        # 读取 event.json
        event_obj = s3_client.get_object(Bucket=bucket, Key=key)
        event_data = json.loads(event_obj["Body"].read())
        # 列出同目录下的 JPEG 图片（从 snapshots 数组获取）
        prefix = key.rsplit("/", 1)[0] + "/"
        jpg_keys = [prefix + fname for fname in event_data.get("snapshots", [])]
        # 对每张图片调用 SageMaker endpoint
        best = None
        for jpg_key in jpg_keys:
            img = s3_client.get_object(Bucket=bucket, Key=jpg_key)["Body"].read()
            start = time.time()
            resp = sm_runtime.invoke_endpoint(
                EndpointName=ENDPOINT_NAME,
                ContentType="image/jpeg", Body=img)
            latency = (time.time() - start) * 1000
            result = json.loads(resp["Body"].read())
            top1 = result["predictions"][0]
            if best is None or top1["confidence"] > best["confidence"]:
                best = {**top1, "image_key": jpg_key, "latency": latency,
                        "top5": result["predictions"]}
        # 写入 DynamoDB（update_item 写回 raspi-eye-events 表）
        table.update_item(
            Key={"device_id": event_data["device_id"], "start_time": event_data["start_time"]},
            UpdateExpression="SET inference_species = :species, inference_confidence = :conf, ...",
            ExpressionAttributeValues={":species": best["species"], ":conf": best["confidence"], ...},
        )
```

### boto3 三步部署示例

```python
import boto3

sm_client = boto3.client("sagemaker", region_name="ap-southeast-1")

# Step 1: Create Model
sm_client.create_model(
    ModelName="raspi-eye-bird-classifier",
    ExecutionRoleArn=role,
    PrimaryContainer={
        "Image": "763104351884.dkr.ecr.ap-southeast-1.amazonaws.com/"
                 "pytorch-inference:2.6.0-cpu-py312-ubuntu22.04-sagemaker",
        "ModelDataUrl": "s3://raspi-eye-model-data/endpoint/dinov3-vitl16/model.tar.gz",
        "Environment": {
            "SAGEMAKER_PROGRAM": "inference.py",
            "SAGEMAKER_SUBMIT_DIRECTORY": "/opt/ml/model/code",
        },
    },
)

# Step 2: Create Endpoint Config (Serverless)
sm_client.create_endpoint_config(
    EndpointConfigName="raspi-eye-bird-classifier-config",
    ProductionVariants=[{
        "ModelName": "raspi-eye-bird-classifier",
        "VariantName": "AllTraffic",
        "ServerlessConfig": {
            "MemorySizeInMB": 6144,
            "MaxConcurrency": 5,
        },
    }],
)

# Step 3: Create Endpoint
sm_client.create_endpoint(
    EndpointName="raspi-eye-bird-classifier",
    EndpointConfigName="raspi-eye-bird-classifier-config",
)
```

## 验证命令

```bash
# 激活 venv
source .venv-raspi-eye/bin/activate

# 运行 endpoint 单元测试（离线，不依赖 GPU 或网络）
pytest model/tests/test_endpoint.py -v

# 运行 Lambda 单元测试（离线，使用 mock）
pytest model/tests/test_lambda.py -v

# 打包模型并部署 Real-time endpoint（一键）
python model/deploy_endpoint.py \
    --s3-bucket raspi-eye-model-data \
    --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
    --backbone dinov3-vitl16 \
    --wait --test

# 部署 Serverless endpoint
python model/deploy_endpoint.py \
    --s3-bucket raspi-eye-model-data \
    --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
    --backbone dinov3-vitl16 \
    --serverless --wait --test

# 部署整条推理链路（一键）
scripts/deploy-inference-pipeline.sh \
    --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
    --s3-bucket raspi-eye-model-data

# 端到端验证
scripts/deploy-inference-pipeline.sh --e2e-test

# 删除所有资源（不删除 DynamoDB 表）
scripts/deploy-inference-pipeline.sh --delete

# 仅部署 Lambda + S3 事件通知（endpoint 已存在）
scripts/deploy-inference-pipeline.sh --skip-endpoint \
    --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role
```

预期结果：单元测试全部通过（离线）；`deploy_endpoint.py --wait` 完成后 endpoint 状态为 InService；`deploy-inference-pipeline.sh` 完成后所有资源就绪；`--e2e-test` 上传测试图片后 180 秒内 DynamoDB 出现推理结果记录。

## 明确不包含

- 前端 UI 展示分类结果（Spec 21）
- GPU 推理容器（Serverless Inference 使用 CPU）
- Provisioned Concurrency 配置（流量稀疏，按需冷启动即可，后续根据实际延迟需求决定）
- 多模型 endpoint（当前仅部署一个 backbone 的模型）
- 模型 A/B 测试（后续可通过 endpoint 更新切换模型）
- 自定义推理容器（使用 SageMaker 预构建 PyTorch 推理容器）
- Lambda Dead Letter Queue（DLQ）配置（流量低，初期不需要）
- DynamoDB TTL 自动过期（后续根据数据量决定）
- CloudFormation / CDK 基础设施即代码（本 Spec 使用 boto3 + AWS CLI 脚本部署，后续可迁移到 IaC）
- 设备端 S3 上传逻辑修改（Spec 11 已完成）
- 训练代码或模型格式修改（Spec 29 已完成）
