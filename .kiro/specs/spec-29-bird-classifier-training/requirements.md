# 需求文档：Spec 29 — 鸟类分类模型训练

## 简介

本 Spec 使用可切换的 frozen backbone（默认 DINOv3 ViT-L/16）+ linear classification head 对 46 种鸟类进行分类。训练框架支持通过 `--backbone` 参数灵活切换不同的预训练模型（如 DINOv3、DINOv2、ResNet、EfficientNet 等），便于模型对比实验。训练在 SageMaker Training Job（GPU 实例）上运行，训练数据来自 Spec 28 产出的 ImageFolder 格式数据集（S3）。训练完成后导出 PyTorch (.pt) 格式的模型，用于后续 SageMaker Serverless endpoint 部署（Spec 17）。种类识别在云端进行，不在 Pi 5 设备端推理。

本 Spec 只做模型训练、评估和导出，不做 endpoint 部署。

## 前置条件

- Spec 28（feature-space-cleaning）已完成，产出 ImageFolder 格式数据集：
  - `s3://raspi-eye-model-data/dataset/train/{species}/*.jpg`
  - `s3://raspi-eye-model-data/dataset/val/{species}/*.jpg`
- 46 个鸟类物种，每个物种约 500-1500 张清洗后图片
- Python ≥ 3.11 环境已就绪（`.venv-raspi-eye/`）
- HuggingFace token 存储在 Secrets Manager: `raspi-eye/huggingface-token`（DINOv3 gated model 访问）
- SageMaker Role: `arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role`

## 术语表

- **DINOv3**：Facebook Research 自监督视觉基础模型（`facebookresearch/dinov3`），本 Spec 使用 `dinov3_vitl16`（ViT-L/16）变体作为默认 frozen backbone
- **Frozen_Backbone**：冻结预训练权重，不做任何参数更新，仅用于特征提取
- **Backbone_Registry**：backbone 注册表，通过 `--backbone` 参数名称查找对应的模型加载逻辑、输入尺寸和特征维度，支持灵活切换不同预训练模型
- **Linear_Head**：线性分类头（`nn.Linear(feature_dim, num_classes)`），接在 frozen backbone 的特征输出之后，是唯一需要训练的参数
- **Training_Script**：训练入口脚本，在 SageMaker Training Job 容器内执行，负责数据加载、模型构建、训练循环、评估和模型导出
- **Training_Launcher**：训练 Job 启动脚本，使用 boto3 创建 SageMaker Training Job，配置输入/输出通道和超参数
- **Model_Exporter**：模型导出模块，将训练好的 linear head 连同 backbone 导出为 PyTorch (.pt) 格式（ONNX 可选）
- **Data_Augmentation**：训练时的数据增强变换（RandomResizedCrop、RandomHorizontalFlip、ColorJitter 等），仅应用于训练集
- **ImageFolder**：PyTorch torchvision 标准数据集格式，目录结构为 `{split}/{class_name}/*.jpg`
- **Confusion_Matrix**：混淆矩阵，N×N 矩阵展示每个物种的预测分布，用于分析易混淆物种对
- **ONNX**：Open Neural Network Exchange，跨平台模型格式，本 Spec 中为可选导出格式
- **LoRA**：Low-Rank Adaptation，参数高效微调方法，通过在 Transformer 的 attention 层注入低秩矩阵实现微调，可训练参数量远少于全量微调。使用 HuggingFace `peft` 库实现。仅适用于 Transformer backbone（DINOv3、DINOv2），不适用于 CNN backbone（ResNet、EfficientNet）

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言 | Python ≥ 3.11 |
| 环境管理 | venv（`.venv-raspi-eye/`），所有命令必须先 `source .venv-raspi-eye/bin/activate` |
| 测试框架 | pytest + Hypothesis（PBT） |
| 代码目录 | `model/training/`（训练模块） |
| 训练脚本入口 | `model/training/train.py`（SageMaker 容器内执行，用户不直接运行） |
| 启动脚本入口 | `model/launch_training.py` |
| DINOv3 模型 | HuggingFace Transformers `facebook/dinov3-vitl16-pretrain-lvd1689m`（gated model，需 HF_TOKEN），作为默认 backbone |
| DINOv3 使用方式 | frozen backbone，仅提取 class token，不做任何参数更新 |
| Backbone 可切换 | 通过 `--backbone` 超参数切换，内置支持 `dinov3-vitl16`（默认）、`dinov2-vitl14`，均为 Transformer 架构，新增 backbone 只需在注册表中添加一条配置 |
| 训练参数 | 默认仅训练 Linear_Head（`nn.Linear`），backbone 参数全部 `requires_grad=False`；启用 `--lora` 时额外训练 LoRA adapter 参数 |
| LoRA 微调 | 可选（`--lora` 标志），支持所有已注册的 Transformer backbone，默认 rank=8（`--lora-rank` 可配置），target modules: q_proj/v_proj |
| LoRA 依赖 | HuggingFace `peft` 库，导出前通过 `merge_and_unload()` 合并 LoRA 权重回 backbone |
| 训练数据 | S3: `s3://raspi-eye-model-data/dataset/train/{species}/*.jpg`（ImageFolder 格式） |
| 验证数据 | S3: `s3://raspi-eye-model-data/dataset/val/{species}/*.jpg`（ImageFolder 格式） |
| 训练输出 | S3: `s3://raspi-eye-model-data/training/jobs/{job_name}/` |
| 模型导出 | S3: `s3://raspi-eye-model-data/training/models/{backbone_name}/`（按 backbone 分目录，便于对比） |
| 导出格式 | PyTorch (.pt)，用于 SageMaker Serverless endpoint 部署 |
| ONNX 导出 | 可选（`--export-onnx` 标志），非默认行为，仅在需要时导出 |
| 图片输入尺寸 | 由 backbone 决定（DINOv3: 518×518，DINOv2: 518×518），从 Backbone_Registry 自动获取 |
| 物种数量 | 46 类（从 `model/config/species.yaml` 读取） |
| 运行环境 | SageMaker Training Job（GPU 实例，默认 `ml.g4dn.xlarge`） |
| SageMaker 容器 | PyTorch 预构建 GPU 容器 |
| HF_TOKEN | 从 Secrets Manager `raspi-eye/huggingface-token` 获取，通过环境变量传入容器 |
| 代码部署方式 | 打包 `model/` 为 `sourcedir.tar.gz`（`tar.add("model/", arcname=".")`）上传到 S3 → 通过 HyperParameters `sagemaker_program`（`training/train.py`）+ `sagemaker_submit_directory` 指定训练脚本，SageMaker Training Toolkit 自动解压并执行 |
| 涉及文件 | 5-8 个 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现 SageMaker endpoint 部署（属于 Spec 17）
- SHALL NOT 在本 Spec 中实现 Lambda 触发器或 DynamoDB 写入（属于 Spec 18）
- SHALL NOT 在本 Spec 中修改 Spec 28 产出的 dataset/train/ 和 dataset/val/ 数据（只读输入）
- SHALL NOT 在本 Spec 中实现数据清洗或 train/val 重新划分（已由 Spec 28 完成）
- SHALL NOT 在本 Spec 中实现设备端推理集成（种类识别在云端 SageMaker Serverless endpoint 进行）

### Design 层

- SHALL NOT 对任何 backbone 做全量参数更新（full fine-tuning），仅允许通过 LoRA adapter 进行参数高效微调（`--lora` 启用时）；未启用 LoRA 时 backbone 参数全部 `requires_grad=False`
- SHALL NOT 在训练循环、评估或导出代码中硬编码特定 backbone 的逻辑（所有 backbone 差异通过 Backbone_Registry 抽象）
- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、Role ARN 或 HF_TOKEN（通过环境变量或 Secrets Manager 获取）
- SHALL NOT 在日志中打印 HF_TOKEN、AWS 凭证等敏感信息
- SHALL NOT 使用 OpenCV 做图片处理（使用 Pillow + torchvision transforms，保持与 Spec 27/28 一致）

### Tasks 层

- SHALL NOT 直接使用系统 `python` 或 `python3` 执行项目 Python 代码或测试，必须先 `source .venv-raspi-eye/bin/activate`
- SHALL NOT 将模型权重文件（.pt、.onnx）提交到 git（通过 S3 管理）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 将新建文件直接放到项目根目录（所有文件放 `model/` 目录下）
- SHALL NOT 在 Spec 文档未全部确定前单独 commit
- SHALL NOT 在不确定 HuggingFace Transformers DINOv3 API 用法时凭猜测编写代码（先查阅官方文档确认 API）

## 需求

### 需求 1：可切换 Backbone + Linear Head 模型构建

**用户故事：** 作为开发者，我需要一个支持灵活切换 backbone 的训练框架，以便使用不同的预训练模型（DINOv3、DINOv2、ResNet 等）进行对比实验，找到最适合鸟类分类的模型。

#### 验收标准

1. THE Training_Script SHALL 提供 Backbone_Registry，通过 `--backbone` 超参数名称查找对应的模型加载函数、输入尺寸（`input_size`）和特征维度（`feature_dim`）
2. THE Backbone_Registry SHALL 内置以下 backbone 配置：
   - `dinov3-vitl16`（默认）：HuggingFace `facebook/dinov3-vitl16-pretrain-lvd1689m`，input_size=518，feature_dim=1024，需要 HF_TOKEN
   - `dinov2-vitl14`：HuggingFace `facebook/dinov2-large`，input_size=518，feature_dim=1024，无需 HF_TOKEN
3. THE Training_Script SHALL 将选定 backbone 的所有参数设置为 `requires_grad=False`，确保训练过程中不更新 backbone 参数
4. THE Training_Script SHALL 在 backbone 的特征输出之后添加一个 `nn.Linear(feature_dim, num_classes)` 作为分类头，其中 `feature_dim` 从 Backbone_Registry 获取，`num_classes` 从训练数据集的类别数自动推断
5. THE Training_Script SHALL 仅将 Linear_Head 的参数传给优化器，backbone 参数不参与梯度计算
6. THE Training_Script SHALL 根据选定 backbone 的 `input_size` 自动调整数据增强中的 resize/crop 尺寸（需求 2 中的增强参数随 backbone 变化）
7. WHEN 模型构建完成后，THE Training_Script SHALL 打印模型结构摘要：backbone 名称、backbone 参数量（frozen）、linear head 参数量（trainable）、输入尺寸、特征维度；LoRA 模式下额外打印 LoRA rank 和 LoRA adapter 参数量
8. WHEN 新增 backbone 时，开发者只需在 Backbone_Registry 中添加一条配置（名称、加载函数、input_size、feature_dim），无需修改训练循环、评估或导出代码
9. THE Training_Script SHALL 支持可选的 `--lora` 标志，启用时对 Transformer backbone（dinov3-vitl16、dinov2-vitl14）注入 LoRA adapter，微调 attention 层的 q_proj 和 v_proj 权重
10. WHEN `--lora` 启用时，THE Training_Script SHALL 支持通过 `--lora-rank` 超参数配置 LoRA rank（默认 8）
11. WHEN `--lora` 启用时，THE Training_Script SHALL 将 LoRA adapter 参数和 Linear_Head 参数一起传给优化器，backbone 的非 LoRA 参数仍保持 `requires_grad=False`
12. WHEN 模型导出时，如果使用了 LoRA，THE Model_Exporter SHALL 先调用 `merge_and_unload()` 将 LoRA 权重合并回 backbone，确保导出的 .pt 文件格式与非 LoRA 模式完全一致

### 需求 2：训练数据加载与数据增强

**用户故事：** 作为开发者，我需要从 ImageFolder 格式的数据集加载训练和验证数据，并对训练集应用数据增强以提高模型泛化能力。

#### 验收标准

1. THE Training_Script SHALL 使用 `torchvision.datasets.ImageFolder` 加载训练集和验证集
2. THE Training_Script SHALL 对训练集应用以下数据增强变换（按顺序，尺寸参数使用 Backbone_Registry 的 `input_size`）：
   - `RandomResizedCrop(input_size, scale=(0.6, 1.0))`：随机裁切并 resize
   - `RandomHorizontalFlip(p=0.5)`：随机水平翻转
   - `ColorJitter(brightness=0.3, contrast=0.3, saturation=0.2, hue=0.1)`：颜色抖动
   - `ToTensor()` + `Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225])`：ImageNet 标准化
3. THE Training_Script SHALL 对验证集仅应用：`Resize(input_size)` + `CenterCrop(input_size)` + `ToTensor()` + `Normalize`（无随机增强）
4. THE Training_Script SHALL 使用 `DataLoader` 加载数据，支持通过超参数配置 `batch_size`（默认 64）和 `num_workers`（默认 4）
5. WHEN 数据加载完成后，THE Training_Script SHALL 打印数据集统计：训练集总图片数、验证集总图片数、类别数、每类训练样本数范围（最少/最多）

### 需求 3：训练循环

**用户故事：** 作为开发者，我需要一个标准的训练循环，使用 cross-entropy loss 和 AdamW 优化器训练 linear head。

#### 验收标准

1. THE Training_Script SHALL 使用 `nn.CrossEntropyLoss(label_smoothing=0.1)` 作为损失函数，label smoothing 有助于细粒度物种（如噪鹛/画眉）的泛化
2. THE Training_Script SHALL 使用 `AdamW` 优化器，默认学习率 `lr=1e-3`、权重衰减 `weight_decay=1e-4`（通过超参数可配置）；LoRA 模式下优化器同时包含 LoRA adapter 参数和 Linear_Head 参数
3. THE Training_Script SHALL 使用 `CosineAnnealingLR` 学习率调度器，`T_max` 等于总 epoch 数
4. THE Training_Script SHALL 支持通过超参数配置训练 epoch 数（默认 10）
5. THE Training_Script SHALL 在每个 epoch 结束后在验证集上评估，记录 train loss、val loss、val top-1 accuracy、val top-5 accuracy
6. THE Training_Script SHALL 使用混合精度训练（`torch.amp.GradScaler` + `torch.amp.autocast('cuda')`）以加速训练和降低显存占用
7. THE Training_Script SHALL 在训练过程中保存验证集 top-1 accuracy 最高的模型权重（best checkpoint）
8. WHEN 训练完成后，THE Training_Script SHALL 打印训练摘要：总 epoch 数、最佳 epoch、最佳 val top-1 accuracy、最佳 val top-5 accuracy、总训练耗时

### 需求 4：模型评估

**用户故事：** 作为开发者，我需要在训练完成后对最佳模型进行详细评估，生成 per-class accuracy 和 confusion matrix，以便分析模型在各物种上的表现和易混淆物种对。

#### 验收标准

1. THE Training_Script SHALL 在训练完成后加载 best checkpoint，在验证集上执行完整评估
2. THE Training_Script SHALL 计算并输出以下指标：
   - overall top-1 accuracy
   - overall top-5 accuracy
   - per-class top-1 accuracy（每个物种的准确率）
   - per-class sample count（每个物种的验证样本数）
3. THE Training_Script SHALL 生成 confusion matrix（N×N，N=物种数），保存为 JSON 文件（`confusion_matrix.json`）
4. THE Training_Script SHALL 在评估报告中标注 top-5 最易混淆物种对（confusion matrix 中非对角线最大值的 5 对）
5. THE Training_Script SHALL 将评估报告保存为 JSON 文件（`evaluation_report.json`），包含所有上述指标

### 需求 5：模型导出

**用户故事：** 作为开发者，我需要将训练好的模型导出为 PyTorch (.pt) 格式，用于 SageMaker Serverless endpoint 部署。ONNX 导出作为可选功能保留。

#### 验收标准

1. THE Model_Exporter SHALL 导出完整模型（backbone + linear head）为 PyTorch 格式（`bird_classifier.pt`），使用 `torch.save` 保存 state_dict 和模型元数据（backbone 名称、类别数、类别名列表、输入尺寸、特征维度）
2. THE Model_Exporter SHALL 同时导出类别映射文件（`class_names.json`），包含 `{class_index: scientific_name}` 映射
3. THE Model_Exporter SHALL 支持可选的 `--export-onnx` 标志，启用时额外导出 ONNX 格式（`bird_classifier.onnx`），使用 `torch.onnx.export`，opset_version ≥ 17
4. WHEN `--export-onnx` 启用时，THE Model_Exporter SHALL 验证 ONNX 模型：使用 `onnx.checker.check_model` 检查有效性，使用 ONNX Runtime 执行一次推理并与 PyTorch 输出对比，确保数值差异 < 1e-4
5. WHEN 导出完成后，THE Model_Exporter SHALL 打印导出摘要：PyTorch 模型文件大小、backbone 名称、类别数；如果导出了 ONNX 则额外打印 ONNX 文件大小和验证结果

### 需求 6：SageMaker Training Job 部署

**用户故事：** 作为开发者，我需要通过脚本一键将训练任务提交到 SageMaker Training Job 在 GPU 实例上运行，以便利用云端 GPU 加速训练。

#### 验收标准

1. THE Training_Launcher SHALL 提供 Python 脚本 `model/launch_training.py`，使用 boto3 创建并启动 SageMaker Training Job
2. THE Training_Launcher SHALL 使用 PyTorch 预构建 GPU 容器镜像（与 Spec 28 的 `launch_processing.py` 保持一致的镜像选择逻辑）
3. THE Training_Launcher SHALL 配置 S3 输入通道：
   - `train`：`s3://raspi-eye-model-data/dataset/train/` → `/opt/ml/input/data/train/`
   - `val`：`s3://raspi-eye-model-data/dataset/val/` → `/opt/ml/input/data/val/`
4. THE Training_Launcher SHALL 配置 S3 输出路径：`s3://raspi-eye-model-data/training/jobs/`
5. THE Training_Launcher SHALL 支持通过 CLI 参数配置超参数：`--backbone`（默认 `dinov3-vitl16`）、`--epochs`、`--batch-size`、`--lr`、`--weight-decay`、`--instance-type`、`--lora`（启用 LoRA 微调）、`--lora-rank`（默认 8）
6. THE Training_Launcher SHALL 从 Secrets Manager（`raspi-eye/huggingface-token`）获取 HF_TOKEN，通过环境变量传入训练容器
7. THE Training_Launcher SHALL 在提交 Job 后打印 Job 名称和 CloudWatch Logs 链接，Job 命名格式为 `bird-v1-{backbone}-{timestamp}`（含版本号和 backbone 名称，便于在 S3 控制台识别）
8. THE Training_Launcher SHALL 支持 `--wait` 参数，等待 Job 完成并打印最终状态、训练指标和耗时
9. WHEN Job 完成后，THE Training_Launcher SHALL 将模型产物（.pt、class_names.json、evaluation_report.json、confusion_matrix.json，以及可选的 .onnx）从 Job 输出路径复制到 `s3://raspi-eye-model-data/training/models/{backbone_name}/`（按 backbone 分目录，便于对比）
10. THE Training_Launcher SHALL 确保训练过程中的 stdout/stderr 日志实时输出到 CloudWatch Logs（log group: `/aws/sagemaker/TrainingJobs`），以便在 CloudWatch 控制台实时监控训练进度。SHALL NOT 使用 `ContainerEntrypoint` 覆盖 SageMaker 预构建容器的默认入口脚本（覆盖会导致 CloudWatch 日志管道断裂）
11. THE Training_Launcher SHALL 配置 Job 超时时间为 12 小时（43200 秒）
12. THE Training_Launcher SHALL 使用 SageMaker 预构建容器的 script mode 部署训练代码：将 `model/` 目录打包为 `sourcedir.tar.gz`（`tar.add("model/", arcname=".")`）上传到 S3，通过 HyperParameters 中的 `sagemaker_program`（指定 `training/train.py`，相对于 tar.gz 解压后的根目录）和 `sagemaker_submit_directory`（指定 tar.gz 的 S3 路径）让容器默认入口脚本自动解压代码并执行训练脚本
13. THE Training_Launcher SHALL 在 `sourcedir.tar.gz` 中包含 `training/requirements.txt`（位于 `model/training/requirements.txt`，打包后路径为 `training/requirements.txt`），列出训练所需的额外依赖（如 `peft`、`transformers>=4.56`），SageMaker Training Toolkit 会在执行训练脚本前自动安装这些依赖

### 需求 7：单元测试与属性测试

**用户故事：** 作为开发者，我需要通过本地单元测试验证模型构建、数据增强、训练循环和模型导出的纯逻辑正确性，确保代码在提交到 SageMaker Training Job 之前没有语法错误和逻辑 bug。

#### 验收标准

1. THE Test_Suite SHALL 包含模型构建测试：backbone 参数全部 frozen（`requires_grad=False`）、linear head 参数 trainable（`requires_grad=True`）、输出维度等于类别数
2. THE Test_Suite SHALL 包含数据增强测试：训练增强后输出尺寸恒为 (3, input_size, input_size)（PBT 属性，input_size 由 backbone 决定）、验证增强后输出尺寸恒为 (3, input_size, input_size)（PBT 属性）、ImageNet 标准化值域检查
3. THE Test_Suite SHALL 包含训练循环测试（合成小数据集，2 个类别 × 10 张图片 × 3 epoch）：loss 下降、accuracy 提升、best checkpoint 保存
4. THE Test_Suite SHALL 包含 PyTorch 模型导出测试：导出成功、state_dict 可加载、元数据（backbone 名称、类别数、输入尺寸）正确保存和恢复
5. THE Test_Suite SHALL 包含可选的 ONNX 导出测试（`--export-onnx` 路径）：导出成功、`onnx.checker.check_model` 通过、ONNX Runtime 推理输出与 PyTorch 输出数值差异 < 1e-4
6. THE Test_Suite SHALL 包含 per-class accuracy 和 confusion matrix 计算测试：合成预测结果 → 验证指标计算正确性
7. THE Test_Suite SHALL 使用 pytest 运行，命令为 `source .venv-raspi-eye/bin/activate && pytest model/tests/ -v`
8. THE Test_Suite SHALL 不依赖 GPU、网络或真实模型权重（使用小型随机初始化模型替代 DINOv3，特征向量使用 torch 合成），确保本地 CPU 环境可运行

### 需求 8：离线推理验证脚本（真实场景验证）

**用户故事：** 作为开发者，我需要用 Pi 5 实际拍摄的鸟类照片验证模型在真实部署场景下的分类效果，因为 iNaturalist val 集的同分布评估（可能 99%+）无法反映真实场景的泛化能力。

#### 验收标准

1. THE Predict_Script SHALL 提供 Python 脚本 `model/predict.py`，支持对任意图片目录或单张图片执行离线推理
2. THE Predict_Script SHALL 通过 `--model` 参数指定模型路径（本地 .pt 文件或 S3 路径），通过 `--images` 参数指定图片目录或单张图片路径
3. THE Predict_Script SHALL 自动从模型元数据中读取 backbone 名称和输入尺寸，应用对应的验证集预处理（Resize + CenterCrop + Normalize）
4. THE Predict_Script SHALL 对每张图片输出 top-k 预测结果（默认 k=5，通过 `--top-k` 可配置），包含：物种名（scientific_name）、置信度（softmax 概率）
5. THE Predict_Script SHALL 支持 `--output` 参数将结果保存为 JSON 文件，格式为 `{filename: [{species: str, confidence: float}, ...]}`
6. WHEN 图片目录中包含以物种名命名的子目录时（如 `test/{species}/*.jpg`），THE Predict_Script SHALL 自动计算 per-class accuracy 并输出汇总报告（与需求 4 的评估报告格式一致）
7. THE Predict_Script SHALL 支持 GPU 和 CPU 两种模式，自动检测可用设备

## 参考代码

### Backbone Registry + BirdClassifier 构建示例

```python
import torch
import torch.nn as nn
from dataclasses import dataclass
from typing import Callable

@dataclass
class BackboneConfig:
    """Backbone 配置。"""
    name: str
    load_fn: Callable[[], nn.Module]  # 返回 backbone 模块
    extract_fn: Callable[[nn.Module, torch.Tensor], torch.Tensor]  # 提取特征
    input_size: int
    feature_dim: int
    needs_hf_token: bool = False

def _load_dinov3() -> nn.Module:
    from transformers import AutoModel
    return AutoModel.from_pretrained("facebook/dinov3-vitl16-pretrain-lvd1689m")

def _extract_cls_token(model, x):
    return model(x).last_hidden_state[:, 0]

BACKBONE_REGISTRY: dict[str, BackboneConfig] = {
    "dinov3-vitl16": BackboneConfig(
        name="dinov3-vitl16", load_fn=_load_dinov3,
        extract_fn=_extract_cls_token,
        input_size=518, feature_dim=1024, needs_hf_token=True,
    ),
    "dinov2-vitl14": BackboneConfig(
        name="dinov2-vitl14",
        load_fn=lambda: AutoModel.from_pretrained("facebook/dinov2-large"),
        extract_fn=_extract_cls_token,
        input_size=518, feature_dim=1024,
    ),
    # 扩展：resnet50, efficientnet-b0 等
}

class BirdClassifier(nn.Module):
    """可切换 backbone + linear classification head。"""

    def __init__(self, num_classes: int, backbone_config: BackboneConfig):
        super().__init__()
        self.config = backbone_config
        self.backbone = backbone_config.load_fn()
        for param in self.backbone.parameters():
            param.requires_grad = False
        self.backbone.eval()
        self.head = nn.Linear(backbone_config.feature_dim, num_classes)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        with torch.no_grad():
            features = self.config.extract_fn(self.backbone, x)
        return self.head(features)
```

### 数据增强示例

```python
from torchvision.transforms import v2

train_transform = v2.Compose([
    v2.RandomResizedCrop(518, scale=(0.6, 1.0)),
    v2.RandomHorizontalFlip(p=0.5),
    v2.ColorJitter(brightness=0.3, contrast=0.3, saturation=0.2, hue=0.1),
    v2.ToImage(),
    v2.ToDtype(torch.float32, scale=True),
    v2.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
])

val_transform = v2.Compose([
    v2.Resize(518),
    v2.CenterCrop(518),
    v2.ToImage(),
    v2.ToDtype(torch.float32, scale=True),
    v2.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
])
```

### ONNX 导出与验证示例

```python
import torch
import onnx
import onnxruntime as ort
import numpy as np

def export_onnx(model: nn.Module, output_path: str, input_size: tuple = (1, 3, 518, 518)):
    """导出 ONNX 模型并验证。"""
    model.eval()
    dummy_input = torch.randn(*input_size)

    torch.onnx.export(
        model, dummy_input, output_path,
        opset_version=17,
        input_names=["input"],
        output_names=["logits"],
        dynamic_axes=None,  # 固定 batch_size=1
    )

    # 验证 ONNX 模型
    onnx_model = onnx.load(output_path)
    onnx.checker.check_model(onnx_model)

    # 数值一致性验证
    session = ort.InferenceSession(output_path)
    with torch.no_grad():
        pt_output = model(dummy_input).numpy()
    ort_output = session.run(None, {"input": dummy_input.numpy()})[0]
    assert np.allclose(pt_output, ort_output, atol=1e-4), "ONNX 输出与 PyTorch 不一致"
```

### SageMaker Training Job 启动示例

```python
import boto3
import tarfile
import os
from datetime import datetime, timezone

sm_client = boto3.client("sagemaker", region_name="us-east-1")
s3_client = boto3.client("s3", region_name="us-east-1")
timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
job_name = f"bird-v1-{backbone}-{timestamp}"

# 打包代码为 sourcedir.tar.gz 并上传到 S3
tar_path = "/tmp/sourcedir.tar.gz"
with tarfile.open(tar_path, "w:gz") as tar:
    tar.add("model/", arcname=".")  # 排除 data/、__pycache__、tests/ 等
s3_client.upload_file(tar_path, bucket, f"pipeline/sourcedir-{timestamp}.tar.gz")
submit_dir = f"s3://{bucket}/pipeline/sourcedir-{timestamp}.tar.gz"

sm_client.create_training_job(
    TrainingJobName=job_name,
    AlgorithmSpecification={
        "TrainingImage": image_uri,  # PyTorch GPU 预构建容器
        "TrainingInputMode": "File",
        # 不设置 ContainerEntrypoint，使用容器默认入口（SageMaker Training Toolkit）
    },
    RoleArn=role,
    InputDataConfig=[
        {
            "ChannelName": "train",
            "DataSource": {
                "S3DataSource": {
                    "S3DataType": "S3Prefix",
                    "S3Uri": "s3://raspi-eye-model-data/dataset/train/",
                    "S3DataDistributionType": "FullyReplicated",
                }
            },
        },
        {
            "ChannelName": "val",
            "DataSource": {
                "S3DataSource": {
                    "S3DataType": "S3Prefix",
                    "S3Uri": "s3://raspi-eye-model-data/dataset/val/",
                    "S3DataDistributionType": "FullyReplicated",
                }
            },
        },
    ],
    OutputDataConfig={
        "S3OutputPath": f"s3://raspi-eye-model-data/training/jobs/",
    },
    ResourceConfig={
        "InstanceType": "ml.g4dn.xlarge",
        "InstanceCount": 1,
        "VolumeSizeInGB": 50,
    },
    HyperParameters={
        "sagemaker_program": "training/train.py",
        "sagemaker_submit_directory": submit_dir,
        "epochs": "10",
        "batch_size": "64",
        "lr": "1e-3",
        "weight_decay": "1e-4",
    },
    StoppingCondition={"MaxRuntimeInSeconds": 43200},  # 12 小时
    Environment={"HF_TOKEN": hf_token},
)
```

## 验证命令

```bash
# 激活 venv
source .venv-raspi-eye/bin/activate

# 运行单元测试（离线，不依赖 GPU 或网络）
pytest model/tests/ -v

# 启动 SageMaker Training Job（默认 DINOv3，linear probe）
# launch_training.py 会自动打包 model/ 为 sourcedir.tar.gz 并上传到 S3
python model/launch_training.py \
    --s3-bucket raspi-eye-model-data \
    --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
    --wait

# 切换 backbone 对比实验（DINOv2，linear probe）
python model/launch_training.py \
    --s3-bucket raspi-eye-model-data \
    --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
    --backbone dinov2-vitl14 --wait

# LoRA 微调对比实验（DINOv3 + LoRA）
python model/launch_training.py \
    --s3-bucket raspi-eye-model-data \
    --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
    --backbone dinov3-vitl16 --lora --wait

# LoRA 微调对比实验（DINOv2 + LoRA）
python model/launch_training.py \
    --s3-bucket raspi-eye-model-data \
    --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
    --backbone dinov2-vitl14 --lora --wait

# 自定义 LoRA rank
python model/launch_training.py \
    --s3-bucket raspi-eye-model-data \
    --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
    --backbone dinov3-vitl16 --lora --lora-rank 16 --wait

# 用 Pi 5 实拍照片验证模型（真实场景）
python model/predict.py \
    --model training/models/dinov3-vitl16/bird_classifier.pt \
    --images /path/to/pi5-photos/ \
    --top-k 5

# 如果照片按物种分目录，自动计算 accuracy
python model/predict.py \
    --model training/models/dinov3-vitl16/bird_classifier.pt \
    --images /path/to/pi5-photos/test/ \
    --output results.json
```

预期结果：单元测试全部通过（离线）；SageMaker Training Job 完成后，`s3://raspi-eye-model-data/training/models/{backbone_name}/` 下生成 `bird_classifier.pt`、`class_names.json`、`evaluation_report.json`、`confusion_matrix.json`。不同 backbone 的结果存储在各自子目录下，便于对比。

## 明确不包含

- SageMaker Serverless endpoint 部署（Spec 17）
- Lambda 触发器和 DynamoDB 写入（Spec 18）
- 设备端推理集成（种类识别在云端 SageMaker Serverless endpoint 进行，不在 Pi 5 上推理）
- 数据清洗或 train/val 重新划分（已由 Spec 28 完成）
- DINOv3 backbone 全量微调（本 Spec 仅支持 frozen backbone + linear head 和 LoRA 参数高效微调两种模式，不做 full fine-tuning）
- 超参数自动搜索（手动配置，后续可扩展）
- 分布式训练（单 GPU 实例足够）
- Web UI 训练监控（通过 CloudWatch Logs 监控）
