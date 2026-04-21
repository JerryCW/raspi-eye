# 实现计划：Spec 29 — 鸟类分类模型训练

## 概述

将设计文档中的鸟类分类模型训练框架转化为增量实现步骤。核心路径：Backbone Registry → BirdClassifier → 数据增强 → 训练循环 → 评估 → 导出 → SageMaker 启动 → 离线推理。所有测试使用 Mock Backbone，不依赖 GPU/网络/真实模型。

## 禁止项（Tasks 层）

- SHALL NOT 直接使用系统 `python`，必须先 `source .venv-raspi-eye/bin/activate`
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在子代理最终检查点执行 git commit
- SHALL NOT 在不确定 HuggingFace Transformers DINOv3 API 用法时凭猜测编写代码
- SHALL NOT 将模型权重文件（.pt、.onnx）提交到 git
- SHALL NOT 将新建文件直接放到项目根目录（所有文件放 `model/` 目录下）

## Tasks

- [x] 1. 实现 Backbone Registry 和 BirdClassifier 核心模块
  - [x] 1.1 创建 `model/src/backbone_registry.py`
    - 定义 `BackboneConfig` dataclass（name、load_fn、extract_fn、input_size、feature_dim、needs_hf_token）
    - 实现 `BACKBONE_REGISTRY` 字典，内置 `dinov3-vitl16`、`dinov2-vitl14` 两个配置
    - 实现 `get_backbone(name)` 函数，未注册名称抛出 ValueError 并列出所有可用 backbone
    - DINOv3 使用 HuggingFace Transformers `AutoModel.from_pretrained`（gated model，需 HF_TOKEN），提取 `last_hidden_state[:, 0]`（CLS token）
    - DINOv2 使用 HuggingFace Transformers `AutoModel.from_pretrained`（公开模型），提取 `last_hidden_state[:, 0]`（CLS token）
    - _Requirements: 1.1, 1.2, 1.8_

  - [x] 1.2 创建 `model/src/classifier.py`
    - 实现 `BirdClassifier(nn.Module)`：接收 `num_classes`、`BackboneConfig`、可选 `lora=False`、`lora_rank=8`
    - `__init__`：加载 backbone → 冻结所有参数（`requires_grad=False`）→ 设置 `backbone.eval()` → 可选注入 LoRA adapter（使用 peft 库）→ 创建 `nn.Linear(feature_dim, num_classes)` head
    - `forward`：LoRA 模式下 backbone 推理有梯度；非 LoRA 模式下 `torch.no_grad()` 调用 backbone extract_fn → head 前向
    - `trainable_parameters()`：非 LoRA 仅返回 head 参数；LoRA 返回 head + LoRA adapter 参数
    - `merge_lora()`：导出前调用，将 LoRA 权重合并回 backbone（`merge_and_unload()`）
    - 构建完成后打印模型摘要（backbone 名称、frozen 参数量、trainable 参数量、input_size、feature_dim；LoRA 模式额外打印 rank 和 adapter 参数量）
    - _Requirements: 1.3, 1.4, 1.5, 1.7, 1.9, 1.10, 1.11, 1.12_

  - [x] 1.3 编写属性测试：模型构建不变量
    - **Property 1: 模型构建不变量（Frozen backbone + Trainable head + 正确维度）**
    - 使用 MockBackbone（小型 nn.Linear，不依赖网络）替代真实 backbone
    - 对任意 num_classes ∈ [2, 100]，验证：backbone 参数全部 `requires_grad=False`、head 参数全部 `requires_grad=True`、head.in_features == feature_dim、head.out_features == num_classes、trainable_parameters() 恰好等于 head 参数
    - 测试文件：`model/tests/test_training.py`
    - **Validates: Requirements 1.3, 1.4, 1.5, 7.1**

- [x] 2. 实现数据增强模块
  - [x] 2.1 创建 `model/src/augmentation.py`
    - 实现 `get_train_transform(input_size)` → `v2.Compose([RandomResizedCrop, HFlip, ColorJitter, ToImage, ToDtype, Normalize])`
    - 实现 `get_val_transform(input_size)` → `v2.Compose([Resize, CenterCrop, ToImage, ToDtype, Normalize])`
    - 使用 `torchvision.transforms.v2`，Normalize 使用 ImageNet 标准值
    - input_size 参数化，由 BackboneConfig 决定
    - _Requirements: 1.6, 2.2, 2.3_

  - [x] 2.2 编写属性测试：数据增强输出尺寸不变量
    - **Property 2: 数据增强输出尺寸不变量**
    - 对任意 input_size ∈ {224, 518} 和任意合法 RGB 图片（宽高 ∈ [32, 2048]），验证训练增强和验证预处理输出 shape 恒为 (3, input_size, input_size)
    - 使用 Hypothesis 生成随机尺寸的 PIL Image
    - 测试文件：`model/tests/test_training.py`
    - **Validates: Requirements 1.6, 2.2, 2.3, 7.2**

- [x] 3. 实现评估模块
  - [x] 3.1 创建 `model/src/evaluator.py`
    - 定义 `EvaluationReport` dataclass（top1_accuracy、top5_accuracy、per_class_accuracy、per_class_count、confusion_matrix、top5_confused_pairs、class_names）
    - 实现 `evaluate(model, dataloader, class_names, device)` → EvaluationReport
    - 实现 `compute_confusion_matrix(all_preds, all_labels, num_classes)` → np.ndarray
    - 实现 `find_top_confused_pairs(cm, class_names, k=5)` → list[tuple[str, str, int]]
    - 实现 `save_evaluation_report(report, output_dir)` → 保存 evaluation_report.json + confusion_matrix.json
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5_

  - [x] 3.2 编写属性测试：评估指标数学不变量
    - **Property 3: 评估指标数学不变量**
    - 对任意随机 logits 矩阵 [N, C] 和标签 [N]，验证：top5_accuracy >= top1_accuracy、per_class_accuracy ∈ [0.0, 1.0]、per_class_count 之和 == N
    - 测试文件：`model/tests/test_training.py`
    - **Validates: Requirements 4.2**

  - [x] 3.3 编写属性测试：混淆矩阵正确性
    - **Property 4: 混淆矩阵正确性与 top-k 易混淆对提取**
    - 对任意随机预测标签和真实标签（N 样本，C 类别），验证：shape == (C, C)、元素 >= 0、每行之和 == 该类样本数、总和 == N、top-k 对确实是非对角线最大值降序
    - 测试文件：`model/tests/test_training.py`
    - **Validates: Requirements 4.3, 4.4**

- [x] 4. 检查点 — 确保核心模块测试通过
  - 运行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_training.py -v`
  - 确保所有已实现的测试通过，如有问题请询问用户

- [x] 5. 实现模型导出模块
  - [x] 5.1 创建 `model/src/exporter.py`
    - 实现 `export_pytorch(model, backbone_config, class_names, output_dir, lora=False, lora_rank=0)` → Path
    - 使用 `torch.save` 保存 state_dict + 元数据（backbone_name、num_classes、class_names、input_size、feature_dim、lora、lora_rank）
    - LoRA 模式下导出前自动调用 `model.merge_lora()` 合并权重
    - 实现 `export_class_names(class_names, output_dir)` → 保存 class_names.json（{index: name}）
    - 实现 `export_onnx(model, backbone_config, output_dir)` → Path（可选，opset_version >= 17）
    - ONNX 导出后验证：onnx.checker.check_model + ORT 推理数值对比 < 1e-4
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 1.13_

  - [~] 5.2 编写属性测试：模型导出 round-trip
    - **Property 5: 模型导出 round-trip**
    - 对任意 num_classes ∈ [2, 50] 的随机初始化 BirdClassifier（MockBackbone），导出 .pt → 重新加载 → 验证 state_dict 数值一致、元数据完全恢复、推理输出一致
    - 测试文件：`model/tests/test_training.py`
    - **Validates: Requirements 5.1, 7.4**

- [x] 6. 实现训练入口脚本
  - [x] 6.1 创建 `model/training/train.py`
    - 解析超参数：支持 CLI 参数和 SageMaker 环境变量（`SM_CHANNEL_TRAIN`、`SM_CHANNEL_VAL`、`SM_MODEL_DIR`）
    - 超参数：`--backbone`（默认 dinov3-vitl16）、`--epochs`（10）、`--batch-size`（64）、`--lr`（1e-3）、`--weight-decay`（1e-4）、`--num-workers`（4）、`--export-onnx`（可选标志）、`--lora`（可选，启用 LoRA 微调）、`--lora-rank`（默认 8）
    - 数据加载：`ImageFolder` + `DataLoader`，训练集用 `get_train_transform`，验证集用 `get_val_transform`
    - 打印数据集统计：训练/验证图片数、类别数、每类样本数范围
    - 训练循环：CrossEntropyLoss(label_smoothing=0.1) + AdamW（head 参数 + 可选 LoRA 参数）+ CosineAnnealingLR + AMP（autocast + GradScaler）
    - 每 epoch 评估：记录 train loss、val loss、val top-1/top-5 accuracy
    - 保存 best checkpoint（val top-1 最高）
    - 训练完成后：加载 best checkpoint → 完整评估 → LoRA 模式下先 merge_lora() → 导出 .pt + class_names.json + evaluation_report.json + confusion_matrix.json
    - 可选 ONNX 导出（`--export-onnx`）
    - 打印训练摘要：总 epoch、最佳 epoch、最佳 accuracy、总耗时
    - _Requirements: 2.1, 2.4, 2.5, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8_

  - [x] 6.2 编写训练循环单元测试
    - 合成小数据集（2 类 × 10 张 × 32×32 随机图片），MockBackbone，3 epoch
    - 验证：loss 下降、best checkpoint 文件存在、导出文件（.pt、class_names.json、evaluation_report.json）存在
    - 测试文件：`model/tests/test_training.py`
    - _Requirements: 7.3, 7.4_

- [x] 7. 实现 SageMaker Training Job 启动脚本
  - [x] 7.1 创建 `model/launch_training.py`
    - 复用 `launch_processing.py` 的 `PYTORCH_IMAGE_URIS` 和 `get_image_uri()` 逻辑
    - CLI 参数：`--s3-bucket`、`--role`、`--backbone`（默认 dinov3-vitl16）、`--epochs`、`--batch-size`、`--lr`、`--weight-decay`、`--instance-type`（默认 ml.g4dn.xlarge）、`--region`、`--wait`、`--export-onnx`、`--lora`（启用 LoRA）、`--lora-rank`（默认 8）
    - 从 Secrets Manager（`raspi-eye/huggingface-token`）获取 HF_TOKEN，通过 Environment 传入容器
    - 使用 SageMaker script mode 部署代码：
      - 将 `model/` 目录打包为 `sourcedir.tar.gz`（排除 `data/`、`tests/`、`__pycache__/`、`.pytest_cache/`、`*.pyc`）
      - 在 tar.gz 中包含 `training/requirements.txt`（位于 `model/training/requirements.txt`，列出 `peft`、`transformers>=4.56` 等训练依赖）
      - 上传 tar.gz 到 `s3://{bucket}/pipeline/sourcedir-{timestamp}.tar.gz`
      - 通过 HyperParameters 传入 `sagemaker_program: "training/train.py"` 和 `sagemaker_submit_directory: "s3://...tar.gz"`
      - SHALL NOT 设置 `ContainerEntrypoint`，保留容器默认入口（SageMaker Training Toolkit）
    - 配置 S3 输入通道：train → `/opt/ml/input/data/train/`、val → `/opt/ml/input/data/val/`（不再需要 code 通道）
    - 训练超参数通过 HyperParameters 传入容器（与 sagemaker_program/sagemaker_submit_directory 合并在同一个 HyperParameters 字典中）
    - 配置 S3 输出路径：`s3://raspi-eye-model-data/training/jobs/`
    - 提交后打印 Job 名称和 CloudWatch Logs 链接
    - `--wait` 模式：等待完成 → 打印状态/指标/耗时 → 复制模型产物到 `training/models/{backbone_name}/`
    - 删除 `model/run_training.sh`（不再需要 wrapper 脚本）
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8, 6.9, 6.10, 6.11, 6.12, 6.13_

- [x] 8. 实现离线推理验证脚本
  - [x] 8.1 创建 `model/predict.py`
    - CLI 参数：`--model`（.pt 文件路径或 S3 路径）、`--images`（图片目录或单张图片）、`--top-k`（默认 5）、`--output`（可选 JSON 输出路径）
    - 从 .pt 元数据读取 backbone_name 和 input_size，应用 `get_val_transform`
    - 自动检测 GPU/CPU 设备
    - 对每张图片输出 top-k 预测（species + confidence）
    - 图片目录含物种子目录时自动计算 per-class accuracy
    - 支持 `--output` 保存 JSON 结果
    - 损坏图片跳过并打印警告
    - _Requirements: 8.1, 8.2, 8.3, 8.4, 8.5, 8.6, 8.7_

- [x] 9. 最终检查点 — 确保所有测试通过
  - 运行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_training.py -v`
  - 确保所有测试通过，如有问题请询问用户
  - SHALL NOT 在此步骤执行 git commit

## Notes

- 标记 `*` 的子任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求编号，确保可追溯
- 所有测试使用 MockBackbone（小型 nn.Linear），不依赖 GPU/网络/真实模型权重
- SageMaker 容器镜像复用 Spec 28 的 `PYTORCH_IMAGE_URIS`（PyTorch 2.6 GPU）
- 属性测试使用 Hypothesis，`@settings(max_examples=100)`
- 模型导出为纯 PyTorch .pt（state_dict + 元数据），不依赖 HuggingFace 序列化
