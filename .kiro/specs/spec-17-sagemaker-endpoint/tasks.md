# 实现计划：Spec 17 — 云端推理链路（SageMaker Endpoint + Lambda + DynamoDB）

## 概述

将设计文档中的云端推理链路转化为增量实现步骤。核心路径：推理脚本（inference.py，自包含）→ 模型打包（packager.py）→ Endpoint 部署（deploy_endpoint.py，Serverless + Real-time 双模式）→ Lambda 函数（handler.py，update_item 写回 raspi-eye-events）→ 一键部署脚本（deploy-inference-pipeline.sh，4 步）。所有单元测试使用 MockBackbone + mock AWS 服务，不依赖 GPU/网络/真实模型权重。

## 禁止项（Tasks 层）

- SHALL NOT 直接使用系统 `python`，必须先 `source .venv-raspi-eye/bin/activate`
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在子代理最终检查点执行 git commit
- SHALL NOT 在不确定 SageMaker PyTorch Inference Toolkit API 用法时凭猜测编写代码（先查阅官方文档确认 `model_fn`/`input_fn`/`predict_fn`/`output_fn` 的签名和行为）
- SHALL NOT 在不确定 AWS Lambda + S3 事件通知 API 用法时凭猜测编写代码
- SHALL NOT 将模型权重文件（.pt、model.tar.gz）提交到 git
- SHALL NOT 将新建文件直接放到项目根目录（推理脚本放 `model/endpoint/`，Lambda 放 `model/lambda/`，部署脚本放 `model/` 或 `scripts/`）
- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、Role ARN 或 HF_TOKEN
- SHALL NOT 在日志中打印 HF_TOKEN、AWS 凭证等敏感信息

## Tasks

- [x] 1. 实现推理脚本（inference.py）
  - [x] 1.1 创建 `model/endpoint/inference.py`
    - 实现 `model_fn(model_dir)` 函数：
      - 从 `model_dir/bird_classifier.pt` 加载 checkpoint，读取 metadata（backbone_name、input_size、class_names、num_classes、feature_dim）
      - 通过环境变量 `HF_TOKEN` 获取 HuggingFace token（由 deploy_endpoint.py 从 Secrets Manager 注入）
      - 使用内联的 `_create_backbone_offline`：`AutoConfig.from_pretrained`（下载 config.json）+ `AutoModel.from_config`（创建空模型结构）
      - 重建内联的 `_BirdClassifier`（backbone + nn.Linear head），加载 state_dict，设为 eval 模式
      - 返回 `{"model": model, "transform": val_transform, "class_names": [...], "metadata": {...}}`
    - 实现 `input_fn(request_body, content_type)` 函数：
      - 接受 `image/jpeg` 或 `application/x-image`，不支持的 content_type 抛出 ValueError
      - Pillow 解码 → RGB → 内联的 `_get_val_transform(input_size)` → `unsqueeze(0)`
      - 损坏图片抛出 ValueError
    - 实现 `predict_fn(input_data, model_dict)` 函数：
      - `torch.no_grad()` → model(input_data) → softmax → top-5
      - 返回 `{"predictions": [{"species": str, "confidence": float}, ...], "model_metadata": {"backbone": str, "num_classes": int}}`
    - 实现 `output_fn(prediction, accept)` 函数：
      - JSON 序列化 prediction dict，返回 `(json_string, "application/json")`
    - 完全自包含：内联 `_get_val_transform`、`_BirdClassifier`、`_create_backbone_offline`，不依赖 training 模块
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8_

  - [x] 1.2 创建 `model/endpoint/requirements.txt`
    - 声明推理依赖：`transformers>=4.56`、`Pillow`
    - 此文件将被打包进 model.tar.gz 的 `code/` 目录
    - _Requirements: 2.2_

  - [x] 1.3 编写属性测试：input_fn 输出 shape 不变量
    - **Property 1: input_fn 输出 shape 不变量**
    - 对任意合法 JPEG 图片（Hypothesis 生成随机尺寸，宽高 ∈ [32, 2048]），经过 `input_fn` 预处理后，输出张量 shape 恒为 `(1, 3, input_size, input_size)`
    - 使用 MockBackbone 替代真实 backbone，monkey-patch `BACKBONE_REGISTRY`
    - 测试文件：`model/tests/test_endpoint.py`
    - **Validates: Requirements 1.3, 5.2**

  - [x] 1.4 编写属性测试：input_fn 拒绝非法 content_type
    - **Property 2: input_fn 拒绝非法 content_type**
    - 对任意不属于 `{"image/jpeg", "application/x-image"}` 的 content_type 字符串，`input_fn` 应抛出 ValueError
    - 测试文件：`model/tests/test_endpoint.py`
    - **Validates: Requirements 1.4**

  - [x] 1.5 编写属性测试：推理 round-trip
    - **Property 3: 推理 round-trip（input_fn → predict_fn → output_fn）**
    - 对任意合法 JPEG 图片（随机尺寸，宽高 ∈ [32, 2048]），经过完整链路后：
      - 输出为合法 JSON 字符串
      - JSON 包含 `predictions` 列表，长度 ∈ [1, 5]
      - 每项含 `species`（非空字符串）和 `confidence`（∈ (0.0, 1.0]）
      - 所有 `confidence` 之和 ≤ 1.0
      - JSON 包含 `model_metadata`，含 `backbone`（非空字符串）和 `num_classes`（正整数）
    - 使用 MockBackbone，`@settings(max_examples=100)`
    - 测试文件：`model/tests/test_endpoint.py`
    - **Validates: Requirements 1.3, 1.6, 1.7, 5.3, 5.4, 5.6**

  - [x] 1.6 编写单元测试：model_fn 加载 + HF_TOKEN 回退 + 损坏图片
    - model_fn 测试：MockBackbone 创建 .pt → model_fn 加载 → 验证返回字典结构（model、transform、class_names、metadata）
    - HF_TOKEN 回退测试：Mock Secrets Manager 失败 → 验证从环境变量获取
    - input_fn 损坏图片测试：随机字节流 → 验证 ValueError
    - 测试文件：`model/tests/test_endpoint.py`
    - _Requirements: 1.1, 1.2, 1.5, 5.1_

- [x] 2. 实现模型打包（packager.py）
  - [x] 2.1 创建 `model/endpoint/packager.py`
    - 实现 `package_model(model_path, class_names_path, output_dir, s3_bucket, backbone_name)` 函数
    - 支持从本地 .pt 文件或 S3 URI 获取模型文件（S3 URI 时先下载到临时目录）
    - 打包 model.tar.gz 内部结构：
      - 根目录：`bird_classifier.pt`、`class_names.json`
      - `code/`：`inference.py`（从 `model/endpoint/inference.py` 复制）、`requirements.txt`（从 `model/endpoint/requirements.txt` 复制）
    - 可选上传到 `s3://{s3_bucket}/endpoint/{backbone_name}/model.tar.gz`
    - 打印 tar.gz 文件大小和 S3 上传路径
    - 返回 S3 URI 或本地路径
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5_

  - [x] 2.2 编写单元测试：model.tar.gz 打包结构
    - 创建临时 .pt 文件和 class_names.json → 调用 package_model → 解压 tar.gz → 验证内部结构
    - 验证根目录含 `bird_classifier.pt`、`class_names.json`
    - 验证 `code/` 含 `inference.py`、`requirements.txt`
    - 测试文件：`model/tests/test_endpoint.py`
    - _Requirements: 2.1, 5.5_

- [x] 3. 检查点 — 确保推理脚本和打包测试通过
  - 运行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_endpoint.py -v`
  - 确保所有已实现的测试通过，如有问题请询问用户
  - SHALL NOT 在此步骤执行 git commit

- [x] 4. 实现 Lambda 函数（handler.py）
  - [x] 4.1 创建 `model/lambda/handler.py`
    - 实现 `handler(event, context)` 入口函数：
      - 遍历 `event["Records"]`，提取 bucket + key
      - 过滤非 `event.json` 结尾的 key
      - 从 S3 读取 event.json，调用 `parse_event_json` 解析元数据
      - 调用 `build_snapshot_keys` 构造完整 S3 key 列表
      - 逐张从 S3 读取 JPEG → `invoke_endpoint`（ContentType `image/jpeg`）
      - 调用 `select_best_prediction` 选取最高置信度预测
      - 通过 `update_item` 写回 `raspi-eye-events` 表（PK: device_id + start_time），推理结果字段加 `inference_` 前缀（inference_species、inference_confidence、inference_image_key、inference_top5、inference_latency_ms、inference_error）
    - 实现 `parse_event_json(event_data)` 函数：提取 event_id、device_id、start_time、detections_summary、snapshots
    - 实现 `build_snapshot_keys(event_key, snapshots)` 函数：从 event.json 所在目录前缀 + snapshot 文件名构造完整 S3 key
    - 实现 `select_best_prediction(results)` 函数：从多张图片的推理结果中选取最高置信度预测
    - 错误处理：SageMaker 调用失败记录到 inference_error 字段，不抛异常；event.json 格式错误跳过；单张图片失败跳过继续
    - 通过环境变量获取 `ENDPOINT_NAME`、`TABLE_NAME`（默认 `raspi-eye-events`）
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8, 6.9, 6.10_

  - [x] 4.2 编写属性测试：事件解析与 snapshot key 构造不变量
    - **Property 4: 事件解析与 snapshot key 构造不变量**
    - 对任意合法的 event.json S3 key（格式 `{device_id}/{date}/{event_id}/event.json`）和任意 snapshots 文件名列表，`build_snapshot_keys` 构造的完整 S3 key 满足：
      - 每个 key 以 event.json 所在目录为前缀
      - 每个 key 以对应的 snapshot 文件名为后缀
      - key 数量等于 snapshots 列表长度
    - 测试文件：`model/tests/test_lambda.py`
    - **Validates: Requirements 6.2, 6.3, 12.2**

  - [x] 4.3 编写属性测试：最佳预测选择不变量
    - **Property 5: 最佳预测选择不变量**
    - 对任意非空的推理结果列表（每个结果含 top-5 预测，每项含 species 和 confidence ∈ (0.0, 1.0]），`select_best_prediction` 选出的最终结果的 confidence 恒等于所有图片所有预测中的最大值
    - 测试文件：`model/tests/test_lambda.py`
    - **Validates: Requirements 6.5, 12.6**

  - [x] 4.4 编写单元测试：Lambda 事件处理 + mock AWS 服务
    - S3 事件解析测试：构造 S3 事件 → 验证提取 bucket 和 key
    - event.json 解析测试：构造 event.json → 验证提取所有字段
    - SageMaker 调用 mock 测试：Mock `invoke_endpoint` → 验证请求参数和响应解析
    - DynamoDB 写入 mock 测试：Mock `put_item` → 验证记录包含所有必需字段
    - SageMaker 调用失败测试：Mock 抛出异常 → 验证不抛异常且 error 字段有值
    - event.json 格式错误测试：传入缺少字段的 JSON → 验证跳过且不抛异常
    - 测试文件：`model/tests/test_lambda.py`
    - _Requirements: 12.1, 12.2, 12.3, 12.4, 12.5, 12.7_

- [x] 5. 检查点 — 确保 Lambda 测试通过
  - 运行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_lambda.py -v`
  - 确保所有已实现的测试通过，如有问题请询问用户
  - SHALL NOT 在此步骤执行 git commit

- [x] 6. 实现 Endpoint 部署脚本（deploy_endpoint.py）
  - [x] 6.1 创建 `model/deploy_endpoint.py`
    - CLI 参数：`--backbone`（默认 dinov3-vitl16）、`--s3-bucket`（默认 raspi-eye-model-data）、`--role`（required）、`--region`（默认 ap-southeast-1）、`--instance-type`（默认 ml.m5.large）、`--instance-count`（默认 1）、`--serverless`（开关，使用 Serverless 模式）、`--memory-size`（默认 6144）、`--max-concurrency`（默认 5）、`--endpoint-name`（默认 raspi-eye-bird-classifier）、`--skip-package`、`--wait`、`--test`、`--update`、`--delete`
    - 部署流程（非 --delete 模式）：
      - 若未指定 `--skip-package`，先调用 `packager.package_model` 打包并上传 model.tar.gz
      - `_fetch_hf_token()` 从 Secrets Manager (us-east-1) 获取 HF_TOKEN，失败时从环境变量回退
      - boto3 三步部署：`create_model` → `create_endpoint_config` → `create_endpoint`
      - 支持 Serverless（`--serverless`）和 Real-time（默认）双模式
      - 使用 PyTorch 2.6 CPU 预构建推理容器镜像
      - 设置环境变量 `SAGEMAKER_PROGRAM=inference.py`、`SAGEMAKER_SUBMIT_DIRECTORY=/opt/ml/model/code`、`SAGEMAKER_MODEL_SERVER_TIMEOUT=300`、`HF_TOKEN=<从 Secrets Manager 获取>`
    - `--update` 模式：创建新 EndpointConfig → `update_endpoint`（同样支持 Serverless + Real-time 双模式）
    - `--delete` 模式：删除 Endpoint → EndpointConfig → Model
    - `--wait` 模式：轮询等待 endpoint 变为 InService
    - `--test` 模式：使用 `model/samples/` 下的样本图片调用 endpoint 验证，打印 top-5 结果和推理延迟（read_timeout=300s）
    - endpoint 已存在且未指定 --update 时打印提示信息
    - 部署完成后打印 endpoint 名称和调用示例命令
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8, 3.9, 3.10, 4.1, 4.2, 4.3_

- [x] 7. 实现一键部署脚本（deploy-inference-pipeline.sh）
  - [x] 7.1 创建 `scripts/deploy-inference-pipeline.sh`
    - 部署顺序（4 步，不含 DynamoDB 表创建）：
      1. SageMaker Endpoint（调用 `python model/deploy_endpoint.py --wait`）
      2. Lambda IAM 角色（`aws iam create-role` + `put-role-policy`，内联策略含 S3 GetObject、SageMaker InvokeEndpoint、DynamoDB UpdateItem、CloudWatch Logs）
      3. Lambda 函数（zip handler.py → `aws lambda create-function`，Python 3.12，256MB，120s 超时，环境变量 TABLE_NAME=raspi-eye-events）
      4. S3 事件通知（`aws lambda add-permission` + `s3api put-bucket-notification-configuration`，suffix filter: event.json）
    - 支持参数：
      - `--skip-endpoint`：跳过 SageMaker endpoint 部署
      - `--delete`：按逆序删除所有资源（不删除 DynamoDB 表）
      - `--e2e-test`：端到端验证（上传测试 event.json + JPEG → 等待 DynamoDB update_item 写入推理结果 → 打印结果 → 清理测试数据）
    - 每步完成后打印状态信息，部署完成后打印所有资源 ARN 汇总
    - 资源已存在时跳过创建并打印提示
    - 任何步骤失败时打印错误信息并停止后续步骤
    - `--e2e-test`：180 秒超时轮询 DynamoDB，超时打印 Lambda CloudWatch Logs 链接
    - _Requirements: 7.1, 7.2, 7.3, 8.1, 8.2, 8.3, 9.1, 9.2, 9.3, 9.4, 10.1, 10.2, 10.3, 10.4, 10.5, 11.1, 11.2, 11.3, 11.4, 11.5, 11.6_

- [x] 8. 最终检查点 — 确保所有测试通过
  - 运行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_endpoint.py model/tests/test_lambda.py -v`
  - 确保所有测试通过，如有问题请询问用户
  - SHALL NOT 在此步骤执行 git commit

## Notes

- 标记 `*` 的子任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求编号，确保可追溯
- 所有测试使用 MockBackbone（小型 nn.Linear），不依赖 GPU/网络/真实模型权重
- Lambda 测试使用 mock AWS 服务（moto 或手动 mock），不依赖真实 AWS 环境
- 属性测试使用 Hypothesis，`@settings(max_examples=100)`
- 推理脚本完全自包含，内联了 backbone 创建、分类器和预处理逻辑，不依赖 training 模块
- 模型加载使用 AutoConfig + AutoModel.from_config 方式，避免下载完整权重
- Lambda 通过 update_item 写回 raspi-eye-events 表，不创建新表
- 部署脚本支持 Serverless + Real-time 双模式
- 所有 Python 命令必须先 `source .venv-raspi-eye/bin/activate`
