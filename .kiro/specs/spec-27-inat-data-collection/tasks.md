# 实施计划：Spec 27 — iNaturalist 鸟类数据采集与清洗

## 概述

在 `model/` 目录下构建 Python 数据管道，从 iNaturalist 公开 API 批量采集 46 种目标鸟类的 research grade 成鸟观察照片，经过 pHash 去重、质量过滤、letterbox resize 后，产出按物种分目录的清洗后图片池及 taxonomy 映射表。实现语言为 Python ≥ 3.11，测试框架 pytest + Hypothesis PBT。

## 禁止项（Tasks 层）

- SHALL NOT 直接使用系统 `python` 或 `python3` 执行项目 Python 代码或测试，必须先 `source .venv-raspi-eye/bin/activate`
- SHALL NOT 将 `model/data/` 目录下的图片文件提交到 git（数据集体积大，通过脚本重新生成）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 将新建文件直接放到项目根目录（所有文件放 `model/` 目录下）
- SHALL NOT 在子代理的最终检查点任务中执行 git commit
- SHALL NOT 使用 OpenCV 做图片处理（使用 Pillow，保持轻量依赖）
- SHALL NOT 在代码中硬编码 iNaturalist API URL 或物种 ID（通过配置文件管理）
- SHALL NOT 将下载的原始图片和清洗后的数据集混在同一目录（原始 `model/data/raw/`，清洗后 `model/data/cleaned/`）
- SHALL NOT 在本 Spec 中实现 train/val 划分（由 Spec 28 在特征空间去噪后执行）

## 任务

- [x] 1. 项目结构搭建与依赖安装
  - [x] 1.1 创建 `model/` 目录结构与 Python 包初始化
    - 创建目录：`model/src/`、`model/config/`、`model/tests/`、`model/data/`
    - 创建 `model/src/__init__.py`、`model/tests/__init__.py`
    - 创建 `model/tests/conftest.py`（pytest fixtures：内存测试图片生成、临时目录等）
    - 更新 `.gitignore` 添加 `model/data/` 排除规则
    - _需求：约束条件（代码目录 model/）_

  - [x] 1.2 创建 `requirements.txt` 并安装依赖
    - 在项目根目录创建或更新 `requirements.txt`，添加：`requests>=2.31`、`Pillow>=10.0`、`imagehash>=4.3`、`PyYAML>=6.0`、`hypothesis>=6.0`、`pytest>=7.0`
    - 执行 `source .venv-raspi-eye/bin/activate && pip install -r requirements.txt`
    - _需求：约束条件（Python 依赖）_

- [x] 2. 配置解析模块
  - [x] 2.1 创建 `model/src/config.py` — 配置解析与验证
    - 实现 `GlobalConfig`、`SpeciesEntry`、`DatasetConfig` 三个 dataclass
    - 实现 `load_config(config_path: str) -> DatasetConfig` 函数
    - 文件不存在时 raise `FileNotFoundError`，格式无效时 raise `ValueError`
    - 物种缺少必填字段（`taxon_id`、`scientific_name`）时跳过并打印警告
    - 全局配置项：`image_size`(518)、`phash_threshold`(8)、`rate_limit`(1.0)、`download_threads`(10)
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5_

  - [x] 2.2 创建 `model/config/species.yaml` — 46 种鸟类完整配置
    - 包含 `global` 段（image_size、phash_threshold、rate_limit、download_threads、random_seed）
    - 包含 `species` 列表（46 个物种，按 A/B/C 三级设置 max_images：2000/1500/800）
    - 每个物种包含：`taxon_id`、`scientific_name`、`common_name_cn`、`max_images`
    - taxon_id 使用需求文档中确认的值
    - _需求：1.1, 1.2_

  - [x] 2.3 创建 `model/tests/test_config.py` — 配置解析单元测试
    - 测试有效配置解析（全局配置 + 物种列表）
    - 测试缺少必填字段的物种被跳过
    - 测试文件不存在时抛出 `FileNotFoundError`
    - 测试格式无效（非法 YAML）时抛出 `ValueError`
    - 测试全局配置默认值
    - 所有测试使用 `tmp_path` fixture，不依赖网络
    - _需求：1.1, 1.2, 1.3, 1.4, 1.5, 8.1_

- [x] 3. 数据清洗模块
  - [x] 3.1 创建 `model/src/cleaner.py` — 去重 + 质量过滤 + resize
    - 实现 `CleanStats` dataclass（species、input_count、after_dedup、removed_corrupt、removed_small、removed_lowvar、after_filter、output_count）
    - 实现 `deduplicate(image_paths, threshold) -> list[str]`：pHash 去重，重复组保留文件尺寸最大的一张
    - 实现 `filter_quality(image_paths) -> tuple[list[str], dict]`：损坏/短边<800/灰度低方差 过滤
    - 实现 `letterbox_resize(image, target_size) -> Image`：等比缩放 + 黑色填充
    - 实现 `clean_species(species_name) -> CleanStats`：去重 → 质量过滤 → resize → 保存到 cleaned/
    - 实现 `clean_all() -> list[CleanStats]`
    - resize 后保存为 JPEG 质量 95
    - _需求：3.1, 3.2, 3.3, 3.4, 3.5, 4.1, 4.2, 4.3, 4.4, 5.1, 5.2, 5.3, 5.4, 5.5, 6.1_

  - [x] 3.2 创建 `model/tests/test_cleaner.py` — 清洗逻辑单元测试 + PBT
    - pHash 去重测试：相同图片去重、不同图片保留、阈值边界
    - 质量过滤测试：损坏图片移除、短边<800移除、灰度低方差移除、正常图片保留
    - letterbox resize 测试：正方形输入、横向矩形、纵向矩形，输出尺寸恒为目标尺寸
    - 所有测试使用 Pillow `Image.new()` 在内存中生成测试图片，不依赖网络
    - _需求：3.1, 3.2, 3.3, 4.1, 4.2, 4.3, 5.1, 5.2, 5.4, 8.2, 8.3_

  - [x] 3.3 PBT — 属性 1：去重后不变量
    - **Property 1：去重后不变量**
    - **验证：需求 3.5**
    - 在 `test_cleaner.py` 中实现
    - 生成随机图片集合（Hypothesis 策略生成不同颜色/尺寸的 RGB 图片），经过 `deduplicate()` 后，验证结果集中同一物种内任意两张图片的 pHash Hamming distance > threshold
    - `@settings(max_examples=100)`
    - 标签：`# Feature: inat-data-collection, Property 1: 去重后不变量`

  - [x] 3.4 PBT — 属性 2：质量过滤尺寸规则
    - **Property 2：质量过滤尺寸规则**
    - **验证：需求 4.2**
    - 在 `test_cleaner.py` 中实现
    - 生成随机尺寸（1~2000 × 1~2000）的有效 RGB 图片，经过 `filter_quality()` 后，验证短边 < 800 的图片被移除，短边 ≥ 800 的非损坏/非纯色图片被保留
    - `@settings(max_examples=100)`
    - 标签：`# Feature: inat-data-collection, Property 2: 质量过滤尺寸规则`

  - [x] 3.5 PBT — 属性 3：Letterbox resize 输出不变量
    - **Property 3：Letterbox resize 输出不变量**
    - **验证：需求 5.1, 5.2, 5.4**
    - 在 `test_cleaner.py` 中实现
    - 生成随机宽高（1~4000 × 1~4000）的输入图片和随机 target_size（32~1024），验证 `letterbox_resize()` 输出尺寸恒为 `(target_size, target_size)`
    - `@settings(max_examples=100)`
    - 标签：`# Feature: inat-data-collection, Property 3: Letterbox resize 输出不变量`

- [x] 4. 检查点 — 配置与清洗模块验证
  - 执行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_config.py model/tests/test_cleaner.py -v`
  - 确认所有测试通过（离线，不依赖网络）
  - 如有问题，询问用户

- [x] 5. 数据采集模块
  - [x] 5.1 创建 `model/src/collector.py` — iNaturalist API 采集 + taxonomy 获取
    - 实现 `CollectStats` dataclass（species、target、downloaded、skipped、failed）
    - 实现 `DataCollector` 类：
      - `__init__(config, output_dir="model/data/raw")`
      - `collect_species(entry) -> CollectStats`：分页调用 `/v1/observations` API，提取照片 URL（替换为 original 尺寸），多线程并发下载，支持断点续传
      - `fetch_taxonomy(entry) -> dict`：调用 `/v1/taxa/{taxon_id}` 获取分类层级，缺少中文名时用英文名填充
      - `collect_all(species_filter=None) -> list[CollectStats]`
    - API 查询参数：`taxon_id`、`quality_grade=research`、`photos=true`、`per_page=200`、`term_id=1`、`term_value_id=2`（成鸟）
    - 速率限制：两次 API 请求间隔 ≥ `rate_limit` 秒
    - 下载并发：`concurrent.futures.ThreadPoolExecutor`，默认 10 线程
    - 单张下载失败时记录错误日志并跳过，继续下一张
    - 采集完成后打印每物种统计摘要
    - _需求：2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 2.8, 7.1, 7.2, 7.3, 7.4, 7.5_

  - [x] 5.2 创建 `model/tests/test_taxonomy.py` — taxonomy 格式验证测试 + PBT
    - 测试 taxonomy JSON 必填字段存在（taxon_id、scientific_name、common_name_cn、common_name_en、family、order、class_label）
    - 测试 class_label 从 0 开始连续编号
    - 测试 label_to_species 反向映射与 species 段一致
    - 测试缺少中文名时用英文名填充
    - 所有测试使用 mock API 响应，不依赖网络
    - _需求：7.2, 7.3, 7.4, 7.5, 8.4_

  - [x] 5.3 PBT — 属性 4：Taxonomy 结构一致性
    - **Property 4：Taxonomy 结构一致性**
    - **验证：需求 7.2, 7.4**
    - 在 `test_taxonomy.py` 中实现
    - 生成随机物种列表（1~50 个物种，随机 scientific_name），调用 taxonomy 构建函数，验证：`class_label` 从 0 开始连续编号；`label_to_species[str(label)]` 等于对应物种的 `scientific_name`（双向映射一致）
    - `@settings(max_examples=100)`
    - 标签：`# Feature: inat-data-collection, Property 4: Taxonomy 结构一致性`

- [x] 6. CLI 入口与端到端集成
  - [x] 6.1 创建 `model/prepare_dataset.py` — CLI 入口
    - 实现 argparse 参数解析：`--config`（必填）、`--skip-download`、`--species`
    - 流程：加载配置 → 采集（除非 --skip-download）→ 清洗（去重 → 质量过滤 → resize）→ 生成 taxonomy.json → 打印统计报告
    - 支持 `--species` 指定单个物种（scientific_name）用于调试
    - 某物种最终图片数为 0 时打印错误信息但继续处理其他物种
    - 某物种清洗后图片数 < 30 时打印警告但继续处理
    - 打印完整统计报告：每物种的采集/去重/过滤/最终数量、总耗时
    - _需求：6.2, 6.3, 6.4, 9.1, 9.2, 9.3, 9.4, 9.5_

- [x] 7. 最终检查点 — 全量测试验证
  - 执行 `source .venv-raspi-eye/bin/activate && pytest model/tests/ -v`
  - 确认所有测试通过（离线，不依赖网络）
  - 确认 `model/data/` 已加入 `.gitignore`
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户

## 备注

- 新建文件：`model/src/__init__.py`、`model/src/config.py`、`model/src/collector.py`、`model/src/cleaner.py`、`model/tests/__init__.py`、`model/tests/conftest.py`、`model/tests/test_config.py`、`model/tests/test_cleaner.py`、`model/tests/test_taxonomy.py`、`model/config/species.yaml`、`model/prepare_dataset.py`
- 修改文件：`.gitignore`、`requirements.txt`
- 所有测试离线可运行：iNaturalist API 使用 `unittest.mock.patch` mock，图片使用 `Image.new()` 内存生成，文件系统使用 `tmp_path` fixture
- PBT 覆盖全部 4 个正确性属性（属性 1-4），分布在 `test_cleaner.py`（属性 1, 2, 3）和 `test_taxonomy.py`（属性 4）
- 标记 `*` 的子任务为可选（PBT 属性测试），可跳过以加速 MVP
- 不做 train/val 划分（由 Spec 28 在 DINOv3 特征空间去噪后执行）
- 产出：`model/data/cleaned/` 按物种分目录 + `model/data/taxonomy.json`
- 端到端采集需要网络（在 EC2 上运行），单元测试全部离线
- 执行任何 Python 命令前必须先 `source .venv-raspi-eye/bin/activate`
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
