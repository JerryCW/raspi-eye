# 需求文档：Spec 28 — DINOv3 特征空间深度清洗

## 简介

本 Spec 对 Spec 27 产出的清洗后图片池（`model/data/cleaned/`）进行特征空间级别的深度清洗。使用 DINOv3（ViT-L/16，frozen backbone）提取图片的语义特征向量，在特征空间中执行离群点检测和语义去重，结合 YOLO 鸟体裁切聚焦鸟体区域，最终产出按 train/val 分层划分的 ImageFolder 格式数据集，供后续 Spec 29（模型训练）直接使用。

本 Spec 只做数据深度清洗，不做模型训练。整个清洗流程通过 SageMaker Processing Job 在 GPU 实例上运行。

## 前置条件

- Spec 27（inat-data-collection）已完成，产出 `model/data/cleaned/{species}/*.jpg`（518×518 letterbox resize 后的图片）和 `model/data/taxonomy.json`
- Python ≥ 3.11 环境已就绪（`.venv-raspi-eye/`）
- GPU 环境可用（SageMaker Processing Job 使用 `ml.g4dn.xlarge` 或更高 GPU 实例）

## 术语表

- **DINOv3**：Facebook Research 于 2025 年 9 月发布的自监督视觉基础模型，GitHub repo: `facebookresearch/dinov3`，本 Spec 使用 `dinov3_vitl16`（ViT-L/16，patch size 16）变体
- **Frozen_Backbone**：冻结 DINOv3 预训练权重，仅用于特征提取，不做任何参数更新
- **Feature_Vector**：DINOv3 输出的 class token 向量，作为图片的语义特征表示
- **Mahalanobis_Distance**：考虑特征分布协方差的距离度量，用于 per-class 离群点检测，比欧氏距离更适合高维特征空间
- **Cosine_Similarity**：两个特征向量的余弦相似度，用于语义去重，值域 [-1, 1]，≥ 0.95 视为语义重复
- **YOLO_Cropper**：使用 YOLOv11 检测图片中的鸟体 bounding box，裁切鸟体区域后 resize 到 518×518，聚焦鸟体特征
- **ImageFolder**：PyTorch torchvision 标准数据集格式，目录结构为 `{split}/{class_name}/*.jpg`
- **Feature_Extractor**：特征提取模块，加载 DINOv3 frozen backbone，批量提取图片的 class token 特征向量
- **Outlier_Detector**：离群点检测模块，基于 per-class Mahalanobis distance 识别特征空间中的异常样本
- **Semantic_Deduplicator**：语义去重模块，基于余弦相似度识别特征空间中的语义重复样本
- **Dataset_Splitter**：数据集划分模块，执行 train/val 分层随机划分

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言 | Python ≥ 3.11 |
| 环境管理 | venv（`.venv-raspi-eye/`），所有命令必须先 `source .venv-raspi-eye/bin/activate` |
| 测试框架 | pytest + Hypothesis（PBT） |
| 代码目录 | `model/cleaning/`（清洗模块） |
| 脚本入口 | `model/clean_features.py` |
| DINOv3 模型 | `torch.hub.load('facebookresearch/dinov3', 'dinov3_vitl16')`，ViT-L/16，patch size 16 |
| DINOv3 使用方式 | frozen backbone，仅提取 class token 特征向量，不做任何参数更新 |
| YOLO 模型 | ultralytics YOLOv11（`yolo11x.pt`），COCO 预训练，class 14 = bird |
| 输入数据 | `model/data/cleaned/{species}/*.jpg`（Spec 27 产出，518×518） |
| 输出数据 | `model/data/train/{species}/*.jpg` + `model/data/val/{species}/*.jpg`（ImageFolder 格式） |
| 输出图片尺寸 | 518×518（YOLO 裁切后 letterbox resize；DINOv3 ViT-L/16 patch size=16 会自动裁切到 512×512 即 32×32 patches，518 的额外 6 像素被丢弃，不影响特征质量） |
| 离群点检测方法 | per-class Mahalanobis distance，阈值 χ²(d, 0.975)（d 为 PCA 降维后维度）；当 N < 1.5D 时自动 PCA 降维到 128 维，当 N < 10 时回退余弦距离 |
| 语义去重阈值 | cosine similarity ≥ 0.95 视为语义重复（可配置），重复组保留距类中心最近的样本 |
| train/val 划分 | 80/20 分层随机划分，固定 seed=42 |
| 每物种最少图片数 | 清洗后 ≥ 20 张（不足时打印警告，不中断） |
| 数据集不提交 git | `model/data/` 已加入 `.gitignore` |
| 运行环境 | SageMaker Processing Job（`ml.g4dn.xlarge` GPU 实例），输入/输出走 S3 |
| 涉及文件 | 5-8 个 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现模型训练或微调（属于 Spec 29）
- SHALL NOT 在本 Spec 中实现 SageMaker endpoint 部署（属于 Spec 17）
- SHALL NOT 在本 Spec 中实现 ONNX 模型导出
- SHALL NOT 在本 Spec 中修改 Spec 27 产出的 `model/data/cleaned/` 原始数据（只读输入）
- SHALL NOT 在本 Spec 中修改 `model/data/taxonomy.json`（只读输入）

### Design 层

- SHALL NOT 对 DINOv3 backbone 做任何参数更新（frozen，仅用于特征提取）
- SHALL NOT 使用 DINOv2 替代 DINOv3（用户明确要求 DINOv3，`facebookresearch/dinov3`，`dinov3_vitl16`）
- SHALL NOT 使用 OpenCV 做图片处理（使用 Pillow + torchvision transforms，保持与 Spec 27 一致）
- SHALL NOT 在特征提取时使用 patch tokens 或 register tokens（仅使用 class token 作为图片级特征向量）
- SHALL NOT 使用全局 Mahalanobis distance（必须 per-class 计算，不同物种的特征分布差异大）

### Tasks 层

- SHALL NOT 直接使用系统 `python` 或 `python3` 执行项目 Python 代码或测试，必须先 `source .venv-raspi-eye/bin/activate`
- SHALL NOT 将 `model/data/` 目录下的图片文件提交到 git
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 将新建文件直接放到项目根目录（所有文件放 `model/` 目录下）
- SHALL NOT 在 Spec 文档未全部确定前单独 commit
- SHALL NOT 在不确定 DINOv3 API 用法时凭猜测编写代码（先查阅 `facebookresearch/dinov3` 官方文档确认 API）

## 需求

### 需求 1：YOLO 鸟体裁切

**用户故事：** 作为开发者，我需要使用 YOLO 检测图片中的鸟体区域并裁切，以便后续特征提取聚焦鸟体而非背景。

#### 验收标准

1. THE YOLO_Cropper SHALL 使用 ultralytics YOLOv11（`yolo11x.pt`）检测图片中 class=14（bird）的 bounding box
2. WHEN 检测到鸟体 bounding box 时，THE YOLO_Cropper SHALL 裁切置信度最高的 bounding box 区域，并在四周扩展 20% padding（保留足够的羽毛边缘和上下文信息供 DINOv3 利用）
3. THE YOLO_Cropper SHALL 将裁切后的图片 resize 到 518×518（使用 letterbox resize，保持宽高比）
4. WHEN 未检测到鸟体（无 class=14 的 bounding box 或置信度 < 0.3）时，THE YOLO_Cropper SHALL 丢弃该图片并记录到丢弃列表（宁缺毋滥，避免背景主导的特征向量污染后续离群点检测）
5. THE YOLO_Cropper SHALL 在裁切完成后打印统计：总图片数、成功裁切数、丢弃数（未检测到鸟体）
6. FOR ALL 裁切后的图片，输出尺寸 SHALL 恒为 518×518（不变量属性）

### 需求 2：DINOv3 特征提取

**用户故事：** 作为开发者，我需要使用 DINOv3 frozen backbone 提取每张图片的语义特征向量，以便在特征空间中进行离群点检测和语义去重。

#### 验收标准

1. THE Feature_Extractor SHALL 通过 `torch.hub.load('facebookresearch/dinov3', 'dinov3_vitl16')` 加载 DINOv3 ViT-L/16 模型
2. THE Feature_Extractor SHALL 将模型设置为 eval 模式（`model.eval()`）并冻结所有参数（`torch.no_grad()`），不做任何参数更新
3. THE Feature_Extractor SHALL 对输入图片应用 DINOv3 标准预处理：resize 到 518×518、ImageNet 标准化（mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]）
4. THE Feature_Extractor SHALL 提取模型输出的 class token 作为图片的特征向量
5. THE Feature_Extractor SHALL 支持 batch 推理（默认 batch_size=32，通过配置可调），利用 GPU 加速，并开启混合精度推理（`torch.autocast('cuda')`）以提升吞吐量和降低显存占用
6. THE Feature_Extractor SHALL 将每个物种的特征向量保存为 `.npy` 文件（`model/data/features/{species}.npy`），便于后续步骤复用而无需重新提取
7. WHEN GPU 不可用时，THE Feature_Extractor SHALL 回退到 CPU 推理并打印警告（CPU 推理速度慢但功能正确）

### 需求 3：特征空间离群点检测

**用户故事：** 作为开发者，我需要在特征空间中检测并移除离群样本（如错误标注、非鸟类图片、极端角度/光照），以提高数据集纯度。

#### 验收标准

1. THE Outlier_Detector SHALL 对每个物种独立计算特征向量的均值和协方差矩阵
2. WHEN 某物种的样本数 N < 1.5 × 特征维度 D 时，THE Outlier_Detector SHALL 先对该物种的特征向量执行 PCA 降维到 128 维，再计算 Mahalanobis distance（防止协方差矩阵秩亏导致计算不稳定）
3. THE Outlier_Detector SHALL 使用 Mahalanobis distance 计算每张图片到其所属物种特征中心的距离
4. WHEN 某张图片的 Mahalanobis distance 超过 χ²(d, 0.975) 阈值（d 为实际使用的特征维度，PCA 后为 128）时，THE Outlier_Detector SHALL 将其标记为离群点并移除
5. THE Outlier_Detector SHALL 在离群点检测完成后打印每个物种的统计：输入数量、移除数量、保留数量、是否使用了 PCA 降维、平均 Mahalanobis distance、移除比例
6. IF 某物种的样本数 < 10 时，THEN THE Outlier_Detector SHALL 回退到余弦距离（cosine distance to centroid）进行离群点检测，阈值为 per-class 距离的 Q3 + 1.5×IQR
7. THE Outlier_Detector SHALL 支持在 species.yaml 中为单个物种配置自定义 `outlier_alpha` 值（覆盖全局默认 0.975），用于姿态多样性高的物种（如家燕）放宽阈值

### 需求 4：特征空间语义去重

**用户故事：** 作为开发者，我需要在特征空间中检测并移除语义重复的图片（pHash 无法捕获的高级语义重复，如同一只鸟不同帧的截图），以提高数据集多样性。

#### 验收标准

1. THE Semantic_Deduplicator SHALL 对每个物种内的所有图片两两计算特征向量的余弦相似度
2. WHEN 两张图片的余弦相似度 ≥ `cosine_threshold`（默认 0.95，通过配置可调）时，THE Semantic_Deduplicator SHALL 将其标记为语义重复
3. WHEN 检测到语义重复组时，THE Semantic_Deduplicator SHALL 保留距离类中心最近（余弦距离最小）的一张，移除其余（距离中心近 = 更具代表性，比文件大小更合理）
4. THE Semantic_Deduplicator SHALL 在语义去重完成后打印每个物种的统计：输入数量、移除数量、保留数量
5. FOR ALL 语义去重后的图片集合，同一物种内任意两张图片的余弦相似度 SHALL 小于 `cosine_threshold`（不变量属性）

### 需求 5：Train/Val 分层随机划分

**用户故事：** 作为开发者，我需要将清洗后的数据集按 80/20 比例划分为训练集和验证集，以便后续 Spec 29 直接使用。

#### 验收标准

1. THE Dataset_Splitter SHALL 对每个物种独立执行分层随机划分，比例为 80% train / 20% val
2. THE Dataset_Splitter SHALL 使用固定随机种子 seed=42，确保划分结果可复现
3. THE Dataset_Splitter SHALL 将划分后的图片复制到 ImageFolder 格式目录：`model/data/train/{species}/*.jpg` 和 `model/data/val/{species}/*.jpg`
4. WHEN 某物种的图片数 < 5 时，THE Dataset_Splitter SHALL 将所有图片放入 train，val 为空，并打印警告
5. THE Dataset_Splitter SHALL 在划分完成后打印统计：每物种的 train/val 数量、总 train/val 数量
6. FOR ALL 物种，train 集和 val 集 SHALL 无交集（不变量属性）
7. FOR ALL 图片数 ≥ 5 的物种，val 集比例 SHALL 在 15%-25% 范围内（容差来自整数取整）

### 需求 6：端到端管道与统计报告

**用户故事：** 作为开发者，我需要一个端到端的清洗管道，依次执行 YOLO 裁切 → DINOv3 特征提取 → 离群点检测 → 语义去重 → train/val 划分，并生成完整的统计报告。

#### 验收标准

1. WHEN 执行 `python model/clean_features.py --config model/config/species.yaml` 时，THE Pipeline SHALL 依次执行：YOLO 裁切（筛选 + 裁切）→ DINOv3 特征提取 → 离群点检测 → 语义去重 → train/val 划分
2. THE Pipeline SHALL 支持通过 SageMaker Processing Job 运行，输入数据从 S3 下载到 `/opt/ml/processing/input/`，输出数据上传到 S3 的 `/opt/ml/processing/output/`
3. THE Pipeline SHALL 自动检测运行环境（`/opt/ml/processing/` 是否存在），无需手动指定本地或 SageMaker 模式
2. THE Pipeline SHALL 支持 `--skip-crop` 参数，跳过 YOLO 裁切步骤（直接使用 cleaned/ 中的原图）
3. THE Pipeline SHALL 支持 `--skip-extract` 参数，跳过特征提取步骤（复用已有的 `.npy` 特征文件）
4. THE Pipeline SHALL 支持 `--species` 参数指定单个物种（scientific_name），用于调试单物种流程
5. THE Pipeline SHALL 在执行完成后打印完整的统计报告：每物种的输入/裁切丢弃/离群点移除/语义去重/最终数量、train/val 数量、平均 Mahalanobis distance、离群点移除比例、总耗时
6. IF 任何物种的最终图片数为 0，THEN THE Pipeline SHALL 打印错误信息但继续处理其他物种
7. WHEN 某物种清洗后图片数 < 20 时，THE Pipeline SHALL 打印警告但继续处理

### 需求 7：SageMaker Processing Job 部署

**用户故事：** 作为开发者，我需要通过脚本一键将清洗管道提交到 SageMaker Processing Job 在 GPU 实例上运行，以便利用云端 GPU 加速特征提取而无需手动管理 EC2 实例。

#### 验收标准

1. THE Deployment_Script SHALL 提供 Python 脚本 `model/launch_processing.py`，使用 SageMaker Python SDK 创建并启动 Processing Job
2. THE Deployment_Script SHALL 使用 `PyTorchProcessor`（或 `ScriptProcessor` + 自定义容器），指定 GPU 实例类型（默认 `ml.g4dn.xlarge`，通过 CLI 参数可覆盖）
3. THE Deployment_Script SHALL 配置 S3 输入通道：`/opt/ml/processing/input/cleaned/` ← S3 上的 cleaned 数据集，`/opt/ml/processing/input/config/` ← species.yaml 配置文件
4. THE Deployment_Script SHALL 配置 S3 输出通道：`/opt/ml/processing/output/train/` → S3 上的 train 数据集，`/opt/ml/processing/output/val/` → S3 上的 val 数据集，`/opt/ml/processing/output/features/` → 特征向量 .npy 文件，`/opt/ml/processing/output/report/` → 清洗统计报告
5. THE Deployment_Script SHALL 支持 `--s3-bucket` 参数指定 S3 桶名，`--s3-prefix` 参数指定数据路径前缀
6. THE Deployment_Script SHALL 在提交 Job 后打印 Job 名称和 CloudWatch Logs 链接，便于监控
7. THE Deployment_Script SHALL 支持 `--wait` 参数，等待 Job 完成并打印最终状态和耗时
8. THE clean_features.py 入口脚本 SHALL 自动检测运行环境：当 `/opt/ml/processing/` 目录存在时使用 SageMaker 路径，否则使用本地路径（无需 `--local` 参数手动切换）
9. THE Deployment_Script SHALL 支持 `--role` 参数指定 SageMaker Execution Role ARN；未指定时从环境变量 `SAGEMAKER_ROLE_ARN` 读取，都没有则报错退出
10. THE Spec SHALL 提供 IAM Role 创建脚本（`scripts/create-sagemaker-role.sh`），创建 SageMaker Execution Role，附加以下最小权限策略：
    - S3 读写：仅限指定桶的 `s3:GetObject`、`s3:PutObject`、`s3:ListBucket`
    - CloudWatch Logs：`logs:CreateLogGroup`、`logs:CreateLogStream`、`logs:PutLogEvents`（Processing Job 日志输出）
    - ECR 拉取镜像：`ecr:GetAuthorizationToken`、`ecr:BatchGetImage`、`ecr:GetDownloadUrlForLayer`（PyTorch 预构建容器）
    - SageMaker 信任关系：`sagemaker.amazonaws.com` 作为 AssumeRole principal
11. THE IAM Role 创建脚本 SHALL 输出 Role ARN，便于传给 `launch_processing.py --role`

### 需求 8：单元测试与属性测试

**用户故事：** 作为开发者，我需要通过本地单元测试验证各清洗步骤的纯逻辑正确性，确保代码在提交到 SageMaker Processing Job 之前没有语法错误和逻辑 bug。

#### 验收标准

1. THE Test_Suite SHALL 包含 YOLO 裁切逻辑测试（mock YOLO 模型）：有检测结果 → 裁切、无检测结果 → 丢弃、输出尺寸恒为 518×518（PBT 属性）
2. THE Test_Suite SHALL 包含 Mahalanobis distance 离群点检测测试（合成特征向量）：正常样本保留、人工注入的离群样本移除、N < 1.5D 时 PCA 降维路径、N < 10 时余弦距离回退路径
3. THE Test_Suite SHALL 包含余弦相似度语义去重测试（合成特征向量）：相同向量 → 去重、不同向量 → 保留、阈值边界、保留距中心最近的样本
4. THE Test_Suite SHALL 包含 train/val 划分测试：比例正确、无交集（PBT 属性）、seed 可复现
5. THE Test_Suite SHALL 包含 SageMaker 路径检测测试：mock `/opt/ml/processing/` 存在时使用 SageMaker 路径、不存在时使用本地路径
6. THE Test_Suite SHALL 使用 pytest 运行，命令为 `source .venv-raspi-eye/bin/activate && pytest model/tests/ -v`
7. THE Test_Suite SHALL 不依赖 GPU、网络或真实模型权重（DINOv3 和 YOLO 均使用 mock，特征向量使用 numpy 合成），确保本地 CPU 环境可运行
8. THE Test_Suite 的目标是验证代码逻辑正确性和可运行性，不验证清洗效果（清洗效果由 SageMaker 端到端运行后的统计报告评估）

## 参考代码

### DINOv3 特征提取示例

```python
import torch
from torchvision import transforms
from PIL import Image

# 加载 DINOv3 ViT-L/16（frozen backbone）
model = torch.hub.load('facebookresearch/dinov3', 'dinov3_vitl16')
model.eval()
model.cuda()  # GPU 加速

# DINOv3 标准预处理（patch size 16，输入 518×518）
transform = transforms.Compose([
    transforms.Resize((518, 518)),
    transforms.ToTensor(),
    transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
])

# 提取 class token 特征向量
with torch.no_grad():
    img = Image.open("bird.jpg").convert("RGB")
    tensor = transform(img).unsqueeze(0).cuda()
    features = model(tensor)  # class token 输出
    # features.shape: (1, feature_dim)
```

### Mahalanobis Distance 离群点检测示例

```python
import numpy as np
from scipy.spatial.distance import mahalanobis
from scipy.stats import chi2

def detect_outliers(features: np.ndarray, alpha: float = 0.975) -> np.ndarray:
    """Per-class Mahalanobis distance 离群点检测（含 PCA 降维保护）。
    
    Args:
        features: (N, D) 特征矩阵
        alpha: χ² 分位数（默认 0.975，即 97.5% 置信度）
    
    Returns:
        布尔数组，True = 正常样本，False = 离群点
    """
    N, D = features.shape
    
    # N < 10：回退到余弦距离 + IQR
    if N < 10:
        centroid = np.mean(features, axis=0)
        norms = np.linalg.norm(features, axis=1, keepdims=True)
        norms[norms == 0] = 1
        cos_dist = 1 - (features @ centroid) / (norms.squeeze() * np.linalg.norm(centroid))
        q1, q3 = np.percentile(cos_dist, [25, 75])
        iqr = q3 - q1
        return cos_dist <= q3 + 1.5 * iqr
    
    # N < 1.5D：PCA 降维到 128 维
    if N < 1.5 * D:
        from sklearn.decomposition import PCA
        pca = PCA(n_components=min(128, N - 1))
        features = pca.fit_transform(features)
    
    mean = np.mean(features, axis=0)
    cov = np.cov(features, rowvar=False)
    cov += np.eye(cov.shape[0]) * 1e-6
    cov_inv = np.linalg.inv(cov)
    
    d = features.shape[1]
    threshold = chi2.ppf(alpha, df=d)
    
    distances = np.array([
        mahalanobis(f, mean, cov_inv) ** 2
        for f in features
    ])
    
    return distances <= threshold
```

### 余弦相似度语义去重示例

```python
import numpy as np
from sklearn.metrics.pairwise import cosine_similarity

def semantic_deduplicate(features: np.ndarray, paths: list[str],
                         threshold: float = 0.95) -> list[str]:
    """基于余弦相似度的语义去重。
    
    Args:
        features: (N, D) 特征矩阵
        paths: 对应的图片路径列表
        threshold: 余弦相似度阈值（≥ threshold 视为重复）
    
    Returns:
        去重后的图片路径列表
    """
    sim_matrix = cosine_similarity(features)
    removed = set()
    
    for i in range(len(paths)):
        if i in removed:
            continue
        for j in range(i + 1, len(paths)):
            if j in removed:
                continue
            if sim_matrix[i, j] >= threshold:
                # 保留距离类中心最近的（更具代表性）
                centroid = np.mean(features, axis=0)
                dist_i = 1 - np.dot(features[i], centroid) / (np.linalg.norm(features[i]) * np.linalg.norm(centroid))
                dist_j = 1 - np.dot(features[j], centroid) / (np.linalg.norm(features[j]) * np.linalg.norm(centroid))
                removed.add(j if dist_i <= dist_j else i)
    
    return [p for idx, p in enumerate(paths) if idx not in removed]
```

### YOLO 鸟体裁切示例

```python
from ultralytics import YOLO
from PIL import Image

BIRD_CLASS_ID = 14  # COCO class 14 = bird

def crop_bird(image_path: str, model: YOLO, conf_threshold: float = 0.3,
              padding: float = 0.2) -> Image.Image:
    """YOLO 鸟体裁切。
    
    Args:
        image_path: 输入图片路径
        model: YOLOv11 模型实例
        conf_threshold: 最低置信度阈值
        padding: bounding box 四周扩展比例（默认 20%）
    
    Returns:
        裁切后的 PIL Image（518×518 letterbox resize），未检测到鸟体时返回 None（丢弃）
    """
    img = Image.open(image_path).convert("RGB")
    results = model(image_path, verbose=False)
    
    # 筛选 bird class，按置信度排序
    bird_boxes = [
        box for box in results[0].boxes
        if int(box.cls) == BIRD_CLASS_ID and float(box.conf) >= conf_threshold
    ]
    
    if not bird_boxes:
        return None  # 未检测到鸟体，丢弃
    
    # 取置信度最高的 box
    best = max(bird_boxes, key=lambda b: float(b.conf))
    x1, y1, x2, y2 = best.xyxy[0].tolist()
    
    # 扩展 padding
    w, h = x2 - x1, y2 - y1
    x1 = max(0, x1 - w * padding)
    y1 = max(0, y1 - h * padding)
    x2 = min(img.width, x2 + w * padding)
    y2 = min(img.height, y2 + h * padding)
    
    cropped = img.crop((int(x1), int(y1), int(x2), int(y2)))
    return letterbox_resize(cropped, 518)  # 复用 spec-27 的 letterbox_resize
```

### SageMaker Processing Job 启动示例

```python
from sagemaker.pytorch import PyTorchProcessor
from sagemaker.processing import ProcessingInput, ProcessingOutput

processor = PyTorchProcessor(
    framework_version="2.1",
    py_version="py310",
    role="arn:aws:iam::xxx:role/SageMakerRole",
    instance_type="ml.g4dn.xlarge",
    instance_count=1,
    base_job_name="bird-feature-cleaning",
)

processor.run(
    code="model/clean_features.py",
    source_dir="model/",
    inputs=[
        ProcessingInput(
            source="s3://bucket/data/cleaned/",
            destination="/opt/ml/processing/input/cleaned/",
        ),
        ProcessingInput(
            source="s3://bucket/config/species.yaml",
            destination="/opt/ml/processing/input/config/",
        ),
    ],
    outputs=[
        ProcessingOutput(
            source="/opt/ml/processing/output/train/",
            destination="s3://bucket/data/train/",
        ),
        ProcessingOutput(
            source="/opt/ml/processing/output/val/",
            destination="s3://bucket/data/val/",
        ),
    ],
    arguments=["--config", "/opt/ml/processing/input/config/species.yaml"],
)
```

### Train/Val 划分示例

```python
from sklearn.model_selection import train_test_split

def split_dataset(image_paths: list[str], test_size: float = 0.2,
                  random_state: int = 42) -> tuple[list[str], list[str]]:
    """分层随机划分 train/val。"""
    if len(image_paths) < 5:
        return image_paths, []  # 样本太少，全部放 train
    
    train, val = train_test_split(
        image_paths, test_size=test_size, random_state=random_state
    )
    return train, val
```

## 验证命令

```bash
# 激活 venv
source .venv-raspi-eye/bin/activate

# 运行单元测试（离线，不依赖 GPU 或网络）
pytest model/tests/ -v

# 端到端清洗（需要 GPU，在 EC2 上运行）
python model/clean_features.py --config model/config/species.yaml

# 跳过 YOLO 裁切（直接使用 cleaned/ 原图）
python model/clean_features.py --config model/config/species.yaml --skip-crop

# 跳过特征提取（复用已有 .npy 文件）
python model/clean_features.py --config model/config/species.yaml --skip-extract

# 单物种调试
python model/clean_features.py --config model/config/species.yaml --species "Passer montanus"
```

预期结果：单元测试全部通过（离线）；端到端执行后 `model/data/train/{species}/*.jpg` 和 `model/data/val/{species}/*.jpg` 按 ImageFolder 格式生成，每物种 train:val ≈ 80:20。

## 明确不包含

- 模型训练或微调（Spec 29）
- SageMaker endpoint 部署（Spec 17）
- ONNX 模型导出
- 数据增强（data augmentation，属于 Spec 29 训练阶段）
- DINOv3 模型微调（本 Spec 仅用 frozen backbone 做特征提取）
- 修改 Spec 27 产出的 cleaned/ 数据或 taxonomy.json
- Web UI 浏览数据集
- 多模型对比实验（仅使用 DINOv3 ViT-L/16）
