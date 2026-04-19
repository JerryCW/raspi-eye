# 实施计划：Spec 28 — DINOv3 特征空间深度清洗

## 概述

在 `model/` 目录下扩展 Python 数据管道，实现四层特征空间深度清洗：YOLO 鸟体裁切 → DINOv3 特征提取 → Mahalanobis 离群点检测 → 余弦相似度语义去重 → Train/Val 分层划分。代码运行在 SageMaker Processing Job（GPU 实例）上，本地测试使用 mock。实现语言为 Python ≥ 3.11，测试框架 pytest + Hypothesis PBT。

## 禁止项（Tasks 层）

- SHALL NOT 直接使用系统 `python` 或 `python3` 执行项目 Python 代码或测试，必须先 `source .venv-raspi-eye/bin/activate`
- SHALL NOT 将 `model/data/` 目录下的文件提交到 git
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 将新建文件直接放到项目根目录（所有文件放 `model/` 或 `scripts/` 目录下）
- SHALL NOT 在 Spec 文档未全部确定前单独 commit
- SHALL NOT 在不确定 DINOv3 API 用法时凭猜测编写代码（先查阅 `facebookresearch/dinov3` 官方文档确认 API）
- SHALL NOT 在子代理的最终检查点任务中执行 git commit

## 任务

- [x] 1. 依赖更新与 .gitignore 扩展
  - [x] 1.1 更新 `requirements.txt` 添加新依赖
    - 在现有依赖基础上追加：`torch>=2.1`、`torchvision>=0.16`、`ultralytics>=8.0`、`scikit-learn>=1.3`、`scipy>=1.11`、`sagemaker>=2.200`
    - 执行 `source .venv-raspi-eye/bin/activate && pip install -r requirements.txt`
    - _需求：约束条件（Python 依赖）_

  - [x] 1.2 更新 `.gitignore` 添加中间产物排除规则
    - 确认 `model/data/` 已排除（Spec 27 已添加）
    - 追加注释说明 Spec 28 新增的中间产物目录：`model/data/cropped/`、`model/data/features/`、`model/data/train/`、`model/data/val/`、`model/data/report/`（均已被 `model/data/` 规则覆盖）
    - 追加 `model/output/` 排除规则
    - _需求：约束条件（数据集不提交 git）_

- [x] 2. config.py 扩展与 YOLO 裁切模块
  - [x] 2.1 扩展 `model/src/config.py` — SpeciesEntry 增加 outlier_alpha 字段
    - 在 `SpeciesEntry` dataclass 中增加 `outlier_alpha: float | None = None`
    - `load_config` 函数无需修改（已有的 `**{k: v ...}` 模式自动处理新字段）
    - _需求：3.7_

  - [x] 2.2 创建 `model/src/cropper.py` — YOLO 鸟体裁切模块
    - 实现 `CropStats` dataclass（species、total、cropped、discarded）
    - 实现 `crop_bird(image_path, model, conf_threshold=0.3, padding=0.2) -> Image | None`：
      - 加载图片，YOLO 推理，筛选 class=14（bird）且 conf ≥ 0.3 的 box
      - 取置信度最高的 box，扩展 20% padding（clamp 到图片边界）
      - 裁切后调用 `letterbox_resize(cropped, 518)` 复用 Spec 27
      - 未检测到鸟体时返回 None
    - 实现 `crop_species(species_name, input_dir, output_dir, model, ...) -> CropStats`：裁切单个物种所有图片
    - 从 `model.src.cleaner` 导入 `letterbox_resize`
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5, 1.6_

  - [x] 2.3 创建 `model/tests/test_cropper.py` — 裁切逻辑单元测试
    - 测试有检测结果时正确裁切 + padding + letterbox resize 到 518×518
    - 测试无检测结果（无 bird class 或 conf < 0.3）时返回 None
    - 测试 padding 不超出图片边界（边缘 bbox 场景）
    - 测试 crop_species 统计计数正确
    - YOLO 模型使用 mock（返回预设 bounding box 结果）
    - 图片使用 `Image.new()` 内存生成，文件系统使用 `tmp_path`
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 8.1, 8.7_

  - [x] 2.4 PBT — 属性 1：YOLO 裁切 padding 边界安全 + 输出尺寸不变量
    - **Property 1：YOLO 裁切 padding 边界安全 + 输出尺寸不变量**
    - **验证：需求 1.2, 1.6**
    - 在 `test_cropper.py` 中实现
    - 生成随机图片尺寸（50~4000 × 50~4000）和随机 bbox 比例，验证 padding 扩展后不超出图片边界，且 letterbox resize 输出恒为 518×518
    - `@settings(max_examples=100)`
    - 标签：`# Feature: feature-space-cleaning, Property 1: YOLO 裁切 padding 边界安全 + 输出尺寸不变量`

- [x] 3. 检查点 — 裁切模块验证
  - 执行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_cropper.py -v`
  - 确认所有测试通过（离线，不依赖 GPU 或网络）
  - 如有问题，询问用户

- [x] 4. 特征提取模块
  - [x] 4.1 创建 `model/src/feature_extractor.py` — DINOv3 特征提取
    - 实现 `ExtractStats` dataclass（species、num_images、feature_dim、device）
    - 实现 `FeatureExtractor` 类：
      - `__init__(repo_dir, weights_path, batch_size=32, num_workers=4)`
      - `_load_model() -> torch.nn.Module`：`torch.hub.load(repo_dir, 'dinov3_vitl16', source='local', weights=weights_path)`，`model.eval()`，自动检测 GPU/CPU
      - `_make_transform() -> transforms.Compose`：`Resize(518,518)` → `ToTensor` → `Normalize(ImageNet mean/std)`
      - `extract_species(species_name, image_dir, output_dir) -> ExtractStats`：DataLoader 批量加载，`torch.autocast('cuda')` 混合精度推理，输出 `{species}.npy` + `{species}_paths.json`
    - GPU 不可用时回退 CPU 并打印警告
    - _需求：2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7_

  - [x] 4.2 PBT — 属性 2：特征向量 .npy 保存/加载 round trip
    - **Property 2：特征向量 .npy 保存/加载 round trip**
    - **验证：需求 2.6**
    - 在 `test_feature_cleaning.py` 中实现
    - 生成随机形状的 float32 矩阵（N ≥ 1, D ≥ 1），保存为 `.npy` 后重新加载，验证 bit-exact 一致
    - `@settings(max_examples=100)`
    - 标签：`# Feature: feature-space-cleaning, Property 2: 特征向量 .npy 保存/加载 round trip`

- [x] 5. 离群点检测与语义去重模块
  - [x] 5.1 创建 `model/src/outlier_detector.py` — Mahalanobis 离群点检测
    - 实现 `OutlierStats` dataclass（species、input_count、removed_count、kept_count、used_pca、used_cosine_fallback、mean_distance、removal_ratio）
    - 实现 `detect_outliers(features, alpha=0.975) -> np.ndarray`：
      - N < 10：余弦距离 + IQR 回退
      - N < 1.5D：PCA 降维到 min(128, N-1) 维
      - 否则：直接 Mahalanobis distance
      - 协方差矩阵正则化 `+ np.eye * 1e-6`
      - 阈值 `chi2.ppf(alpha, df=d)`
    - 返回布尔数组（True = 正常，False = 离群点）
    - _需求：3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7_

  - [x] 5.2 创建 `model/src/semantic_dedup.py` — 余弦相似度语义去重
    - 实现 `DedupStats` dataclass（species、input_count、removed_count、kept_count）
    - 实现 `semantic_deduplicate(features, paths, threshold=0.95) -> tuple[np.ndarray, list[str]]`：
      - 计算余弦相似度矩阵
      - 重复组中保留距类中心最近（余弦距离最小）的样本
      - 返回去重后的特征矩阵和路径列表
    - _需求：4.1, 4.2, 4.3, 4.4, 4.5_

  - [x] 5.3 创建 `model/tests/test_feature_cleaning.py` — 离群点 + 语义去重单元测试
    - 离群点检测测试：正常样本保留、人工注入远离中心的样本被移除
    - PCA 降维路径测试：N < 1.5D 时自动触发 PCA
    - 余弦距离回退路径测试：N < 10 时使用余弦距离 + IQR
    - 语义去重测试：相同向量去重、不同向量保留、保留距中心最近的样本
    - 所有测试使用 numpy 合成特征向量（`np.random.default_rng(42)`），不依赖 GPU 或网络
    - _需求：3.1, 3.2, 3.3, 3.4, 3.6, 4.1, 4.2, 4.3, 4.4, 4.5, 8.2, 8.3, 8.7_

  - [x] 5.4 PBT — 属性 3：离群点检测正确识别注入的离群样本
    - **Property 3：离群点检测正确识别注入的离群样本**
    - **验证：需求 3.2, 3.4**
    - 在 `test_feature_cleaning.py` 中实现
    - 从多元正态分布采样正常特征矩阵（N ≥ 10），加入远离中心的人工离群样本，验证 `detect_outliers` 将注入样本标记为离群点；当 N < 1.5D 时验证 PCA 降维路径被触发
    - `@settings(max_examples=100)`
    - 标签：`# Feature: feature-space-cleaning, Property 3: 离群点检测正确识别注入的离群样本`

  - [x] 5.5 PBT — 属性 4：语义去重后余弦相似度不变量
    - **Property 4：语义去重后余弦相似度不变量**
    - **验证：需求 4.5**
    - 在 `test_feature_cleaning.py` 中实现
    - 生成随机特征向量集合（N ≥ 3, D ≥ 10），人工注入重复（微小扰动），经过 `semantic_deduplicate` 后，验证结果集中任意两个向量的余弦相似度 < threshold
    - `@settings(max_examples=100)`
    - 标签：`# Feature: feature-space-cleaning, Property 4: 语义去重后余弦相似度不变量`

- [x] 6. 检查点 — 离群点检测与语义去重验证
  - 执行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_feature_cleaning.py -v`
  - 确认所有测试通过（离线，不依赖 GPU 或网络）
  - 如有问题，询问用户

- [x] 7. Train/Val 划分模块
  - [x] 7.1 创建 `model/src/splitter.py` — 分层随机划分
    - 实现 `SplitStats` dataclass（species、total、train_count、val_count）
    - 实现 `split_dataset(image_paths, test_size=0.2, random_state=42) -> tuple[list[str], list[str]]`：
      - 图片数 < 5 时全部放入 train，val 为空
      - 否则使用 `sklearn.model_selection.train_test_split`
    - 实现 `split_and_copy(species_name, image_paths, train_dir, val_dir, random_state=42) -> SplitStats`：划分并复制图片到 ImageFolder 目录
    - _需求：5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7_

  - [x] 7.2 PBT — 属性 5：Train/Val 划分无交集 + 比例约束
    - **Property 5：Train/Val 划分无交集 + 比例约束**
    - **验证：需求 5.1, 5.6, 5.7**
    - 在 `test_feature_cleaning.py` 中实现
    - 生成随机长度 ≥ 5 的图片路径列表，验证 `split_dataset` 划分后 train/val 无交集（`set(train) ∩ set(val) == ∅`），且 val 比例在 15%-25% 范围内
    - `@settings(max_examples=100)`
    - 标签：`# Feature: feature-space-cleaning, Property 5: Train/Val 划分无交集 + 比例约束`

  - [x] 7.3 PBT — 属性 6：Train/Val 划分可复现
    - **Property 6：Train/Val 划分可复现**
    - **验证：需求 5.2**
    - 在 `test_feature_cleaning.py` 中实现
    - 生成随机图片路径列表和固定 seed，对同一输入调用 `split_dataset` 两次，验证产生完全相同的 train 和 val 列表
    - `@settings(max_examples=100)`
    - 标签：`# Feature: feature-space-cleaning, Property 6: Train/Val 划分可复现`

- [x] 8. CLI 入口与 SageMaker 部署脚本
  - [x] 8.1 创建 `model/clean_features.py` — 端到端清洗 CLI 入口
    - 实现 `detect_paths() -> dict`：自动检测 SageMaker（`/opt/ml/processing/`）或本地路径
    - 实现 argparse 参数解析：`--config`（必填）、`--skip-crop`、`--skip-extract`、`--species`、`--dinov3-repo`、`--dinov3-weights`、`--batch-size`、`--cosine-threshold`
    - 流程：加载配置 → YOLO 裁切（除非 --skip-crop）→ DINOv3 特征提取（除非 --skip-extract）→ 离群点检测 → 语义去重 → Train/Val 划分 → 打印统计报告
    - 生成 `cleaning_report.json` 统计报告
    - 某物种最终图片数为 0 时打印错误但继续处理其他物种
    - 某物种清洗后图片数 < 20 时打印警告但继续处理
    - _需求：6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8, 6.9_

  - [x] 8.2 创建 `model/launch_processing.py` — SageMaker Processing Job 启动脚本
    - 实现 argparse 参数解析：`--s3-bucket`（必填）、`--s3-prefix`、`--role`、`--instance-type`、`--wait`
    - 使用 `PyTorchProcessor` 创建 Processing Job
    - 配置 S3 输入通道（cleaned/ + species.yaml）和输出通道（train/ + val/ + features/ + report/）
    - 提交 Job 后打印 Job 名称 + CloudWatch Logs 链接
    - `--wait` 时等待 Job 完成并打印最终状态和耗时
    - Role ARN 从 `--role` 或 `SAGEMAKER_ROLE_ARN` 环境变量获取，都没有则报错退出
    - _需求：7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7, 7.8, 7.9_

  - [x] 8.3 创建 `scripts/create-sagemaker-role.sh` — IAM Role 创建脚本
    - 创建信任策略（`sagemaker.amazonaws.com` AssumeRole）
    - 创建 Role `raspi-eye-sagemaker-processing-role`
    - 附加最小权限策略：S3 读写（仅限指定桶）、CloudWatch Logs、ECR 拉取镜像
    - 输出 Role ARN
    - _需求：7.10, 7.11_

  - [x] 8.4 在 `test_feature_cleaning.py` 中添加 SageMaker 路径检测测试
    - 测试 `detect_paths()`：mock `/opt/ml/processing/` 存在时返回 SageMaker 路径
    - 测试 `detect_paths()`：mock `/opt/ml/processing/` 不存在时返回本地路径
    - 使用 `monkeypatch` mock `os.path.isdir`
    - _需求：6.3, 8.5_

- [x] 9. 最终检查点 — 全量测试验证
  - 执行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_cropper.py model/tests/test_feature_cleaning.py -v`
  - 确认所有测试通过（离线，不依赖 GPU 或网络）
  - 确认 `.gitignore` 已更新
  - 确认 `requirements.txt` 已更新
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户

## 备注

- 新建文件：`model/src/cropper.py`、`model/src/feature_extractor.py`、`model/src/outlier_detector.py`、`model/src/semantic_dedup.py`、`model/src/splitter.py`、`model/tests/test_cropper.py`、`model/tests/test_feature_cleaning.py`、`model/clean_features.py`、`model/launch_processing.py`、`scripts/create-sagemaker-role.sh`
- 修改文件：`model/src/config.py`（SpeciesEntry 增加 outlier_alpha）、`requirements.txt`、`.gitignore`
- 复用 Spec 27 的 `letterbox_resize`（`model/src/cleaner.py`）
- 所有测试离线可运行：YOLO 模型使用 mock，DINOv3 模型使用 mock，特征向量使用 numpy 合成，文件系统使用 `tmp_path` fixture，SageMaker 路径使用 `monkeypatch` mock
- PBT 覆盖全部 6 个正确性属性（属性 1-6），分布在 `test_cropper.py`（属性 1）和 `test_feature_cleaning.py`（属性 2, 3, 4, 5, 6）
- 标记 `*` 的子任务为可选（PBT 属性测试），可跳过以加速 MVP
- 端到端清洗需要 GPU（在 SageMaker 或 EC2 上运行），单元测试全部离线
- 执行任何 Python 命令前必须先 `source .venv-raspi-eye/bin/activate`
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
