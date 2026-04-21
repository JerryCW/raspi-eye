# 需求文档：Spec 27 — iNaturalist 鸟类数据采集与清洗

## 简介

本 Spec 为 raspi-eye 项目的云端鸟类种类识别模型准备训练数据。采用四层清洗策略：第一层元数据硬过滤（API 参数级）→ 第二层视觉信号初筛（清晰度 + 曝光 + 尺寸）→ pHash 去重 → 统一 resize。第三层特征空间深度清洗（DINOv3 embedding 离群点检测 + 余弦相似度去重）和第四层场景模拟裁剪（YOLO 裁切 + 数据增强）放在 Spec 28 训练阶段实现。

从 iNaturalist 公开 API 下载指定鸟类物种的 research grade 成鸟观察照片，经过两层清洗和 pHash 去重后，产出按物种分目录的清洗后图片池及 taxonomy 映射表。同时生成物种 taxonomy 映射表（species → family/order/中文名等），供后续 Spec 18（Lambda + DynamoDB）写入时使用。

本 Spec 是 model 模块的第一个 Spec，产出的数据集直接供 Spec 28（bird-classifier-training）使用。

## 前置条件

- 无硬性前置 Spec 依赖（model 模块独立于 device 模块）
- Python ≥ 3.11 环境已就绪（`python3 -m venv .venv-raspi-eye`）
- 网络可访问 iNaturalist API（`api.inaturalist.org`）和图片 CDN（`inaturalist-open-data.s3.amazonaws.com` 等）

## 术语表

- **iNaturalist**：全球最大的公民科学生物多样性平台，提供公开 REST API 查询物种观察记录和照片
- **Research Grade**：iNaturalist 上经社区验证的高质量观察记录，物种鉴定由多人确认，`quality_grade=research`
- **Observation**：iNaturalist 上的一条观察记录，包含物种信息、照片 URL、地理位置、观察时间等
- **Taxon**：iNaturalist 的分类单元，包含物种的完整分类层级（界/门/纲/目/科/属/种）
- **Perceptual Hash（pHash）**：基于图像视觉内容的哈希算法，视觉相似的图像产生相近的哈希值，用于去重
- **Hamming Distance**：两个哈希值之间不同 bit 的数量，用于衡量 perceptual hash 的相似度
- **Letterbox Resize**：等比缩放 + 填充到目标尺寸，保持原图宽高比不变形
- **Species_Config**：物种配置文件（YAML），定义目标物种列表及每个物种的采集参数
- **Taxonomy_Map**：物种到分类层级的映射表（JSON），包含 species_cn、family、family_cn、order 等字段
- **Data_Collector**：数据采集模块，负责调用 iNaturalist API 下载观察照片
- **Data_Cleaner**：数据清洗模块，负责去重、质量过滤、统一 resize

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言 | Python ≥ 3.11 |
| 环境管理 | venv（`.venv-raspi-eye/`），所有命令必须先 `source .venv-raspi-eye/bin/activate` |
| 测试框架 | pytest |
| 代码目录 | `model/collection/`（采集模块）、`model/config/`（配置） |
| 脚本入口 | `model/collect_data.py`（采集）、`model/clean_data.py`（清洗）或合并为 `model/prepare_dataset.py` |
| 物种配置 | YAML 格式，路径 `model/config/species.yaml` |
| 输出数据集目录 | `model/data/cleaned/`（按物种分目录，供 Spec 28 进一步处理） |
| 输出 taxonomy 映射 | `model/data/taxonomy.json` |
| iNaturalist API 速率限制 | 每秒不超过 1 次请求（官方建议），实现中使用 `time.sleep` 或令牌桶控速 |
| 图片下载并发 | 多线程并发下载（默认 10 线程），在 EC2 上运行以利用高带宽 |
| 仅下载 research grade | `quality_grade=research`，不下载 needs_id 或 casual 级别 |
| 图片统一尺寸 | 518×518（通过配置可调，匹配下游模型输入尺寸） |
| 去重阈值 | pHash Hamming distance ≤ 8 视为重复（可配置） |
| train/val 划分比例 | 不在本 Spec 执行（由 Spec 28 在特征空间去噪后执行） |
| 每物种最少图片数 | 采集后清洗前 ≥ 50 张，清洗后 ≥ 30 张（不足时打印警告，不中断） |
| 数据集不提交 git | `model/data/` 加入 `.gitignore` |
| 运行环境 | EC2 实例（高带宽网络），代码通过 git clone 部署 |
| 涉及文件 | 5-8 个 |

### 目标物种列表（46 种）

按识别难度分为三级，差异化采集数量：
- A 类（地狱级难度）：max_images = 2000，预计清洗后留存 500-800 张。特征相近的噪鹛科和鹎科，需要海量样本覆盖极端光照和角度
- B 类（常见多样性高）：max_images = 1500，预计清洗后留存 500-700 张。特征明显但行为丰富，需要覆盖多样性
- C 类（家养鹦鹉与稀有种）：max_images = 800 或爬取上限，预计清洗后留存 200-500 张。iNaturalist 上 research grade 可能不足，有多少爬多少

| 级别 | taxon_id | 英文名 | 学名 | 中文名 | max_images |
|------|----------|--------|------|--------|------------|
| A | 1289311 | White-browed Laughingthrush | Pterorhinus sannio | 白颊噪鹛 | 2000 |
| A | 15159 | Chinese Hwamei | Garrulax canorus | 画眉 | 2000 |
| A | 1289299 | Masked Laughingthrush | Pterorhinus perspicillatus | 黑脸噪鹛 | 2000 |
| A | 14588 | Light-vented Bulbul | Pycnonotus sinensis | 白头鹎 | 2000 |
| A | 14594 | Brown-breasted Bulbul | Pycnonotus xanthorrhous | 黄臀鹎 | 2000 |
| A | 1289357 | Vinous-throated Parrotbill | Sinosuthora webbiana | 棕头鸦雀 | 2000 |
| B | 14522 | Red-billed Leiothrix | Leiothrix lutea | 红嘴相思鸟 | 1500 |
| B | 14674 | Collared Finchbill | Spizixos semitorques | 领雀嘴鹎 | 1500 |
| B | 14591 | Red-whiskered Bulbul | Pycnonotus jocosus | 红耳鹎 | 1500 |
| B | 71755 | Japanese Tit | Parus minor | 远东山雀 | 1500 |
| B | 7247 | Black-throated Bushtit | Aegithalos concinnus | 红头长尾山雀 | 1500 |
| B | 7246 | Long-tailed Tit | Aegithalos caudatus | 银喉长尾山雀 | 1500 |
| B | 8336 | Red-billed Blue Magpie | Urocissa erythroryncha | 红嘴蓝鹊 | 1500 |
| B | 144514 | Eurasian Magpie | Pica pica | 喜鹊 | 1500 |
| B | 8449 | Grey Treepie | Dendrocitta formosae | 灰树鹊 | 1500 |
| B | 1067253 | Chinese Blackbird | Turdus mandarinus | 乌鸫 | 1500 |
| B | 12975 | Oriental Magpie-Robin | Copsychus saularis | 鹊鸲 | 1500 |
| B | 144573 | Spotted Dove | Spilopelia chinensis | 珠颈斑鸠 | 1500 |
| B | 2960 | Oriental Turtle-Dove | Streptopelia orientalis | 山斑鸠 | 1500 |
| B | 13695 | Eurasian Tree Sparrow | Passer montanus | 树麻雀 | 1500 |
| B | 853879 | Warbling White-eye | Zosterops simplex | 暗绿绣眼鸟 | 1500 |
| B | 12034 | Long-tailed Shrike | Lanius schach | 棕背伯劳 | 1500 |
| B | 11893 | Barn Swallow | Hirundo rustica | 家燕 | 1500 |
| B | 68046 | Red-rumped Swallow | Cecropis daurica | 金腰燕 | 1500 |
| B | 13106 | Daurian Redstart | Phoenicurus auroreus | 北红尾鸲 | 1500 |
| B | 7120 | Eurasian Hoopoe | Upupa epops | 戴胜 | 1500 |
| B | 2391 | Common Kingfisher | Alcedo atthis | 普通翠鸟 | 1500 |
| B | 4942 | Little Egret | Egretta garzetta | 白鹭 | 1500 |
| B | 4958 | Chinese Pond Heron | Ardeola bacchus | 池鹭 | 1500 |
| B | 4967 | Black-crowned Night Heron | Nycticorax nycticorax | 夜鹭 | 1500 |
| B | 4827 | Common Moorhen | Gallinula chloropus | 黑水鸡 | 1500 |
| B | 3779 | Little Grebe | Tachybaptus ruficollis | 小鸊鷉 | 1500 |
| B | 4954 | Grey Heron | Ardea cinerea | 苍鹭 | 1500 |
| B | 13739 | White Wagtail | Motacilla alba | 白鹡鸰 | 1500 |
| B | 13735 | Grey Wagtail | Motacilla cinerea | 灰鹡鸰 | 1500 |
| B | 12044 | Black Drongo | Dicrurus macrocercus | 黑卷尾 | 1500 |
| B | 12024 | Brown Shrike | Lanius cristatus | 红尾伯劳 | 1500 |
| B | 8314 | Azure-winged Magpie | Cyanopica cyanus | 灰喜鹊 | 1500 |
| B | 3017 | Rock Dove | Columba livia | 家鸽/岩鸽 | 1500 |
| C | 18903 | Budgerigar | Melopsittacus undulatus | 虎皮鹦鹉 | 800 |
| C | 18910 | Cockatiel | Nymphicus hollandicus | 玄凤鹦鹉 | 800 |
| C | 19077 | Rosy-faced Lovebird | Agapornis roseicollis | 桃脸牡丹鹦鹉 | 800 |
| C | 18882 | Monk Parakeet | Myiopsitta monachus | 和尚鹦鹉 | 800 |
| C | 18898 | Sun Conure | Aratinga solstitialis | 金太阳锥尾鹦鹉 | 800 |
| C | 19150 | Pacific Parrotlet | Forpus coelestis | 太平洋鹦鹉 | 800 |
| C | 145321 | Yellow-bellied Tit | Pardaliparus venustulus | 黄腹山雀 | 800 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中实现模型训练或微调（属于 Spec 28: bird-classifier-training）
- SHALL NOT 在本 Spec 中实现 SageMaker endpoint 部署（属于 Spec 17）
- SHALL NOT 在本 Spec 中实现 DynamoDB 写入（属于 Spec 18，本 Spec 只生成 taxonomy.json）
- SHALL NOT 下载非 research grade 的观察照片（needs_id 和 casual 级别质量不可靠）
- SHALL NOT 在本 Spec 中实现数据增强（data augmentation 属于 Spec 28 训练阶段）

### Design 层

- SHALL NOT 在代码中硬编码 iNaturalist API URL 或物种 ID（通过配置文件管理）
- SHALL NOT 在日志或错误输出中打印 API token 等敏感信息（iNaturalist 公开 API 无需 token，但保持习惯）
- SHALL NOT 使用 OpenCV 做图片处理（使用 Pillow，保持轻量依赖）
- SHALL NOT 将下载的原始图片和清洗后的数据集混在同一目录（原始图片放 `model/data/raw/`，清洗后放 `model/data/cleaned/`）
- SHALL NOT 在不确定 iNaturalist API 返回格式时凭猜测解析字段（先查阅 API 文档确认）

### Tasks 层

- SHALL NOT 直接使用系统 `python` 或 `python3` 执行项目 Python 代码或测试，必须先 `source .venv-raspi-eye/bin/activate`
- SHALL NOT 将 `model/data/` 目录下的图片文件提交到 git（数据集体积大，通过脚本重新生成）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 将新建文件直接放到项目根目录（所有文件放 `model/` 目录下）
- SHALL NOT 在 Spec 文档未全部确定前单独 commit

## 需求

### 需求 1：物种配置文件

**用户故事：** 作为开发者，我需要通过配置文件定义目标鸟类物种列表，以便灵活调整采集范围而无需修改代码。

#### 验收标准

1. THE Species_Config SHALL 使用 YAML 格式，存放在 `model/config/species.yaml`
2. THE Species_Config SHALL 为每个目标物种定义以下字段：`taxon_id`（iNaturalist taxon ID）、`scientific_name`（学名）、`common_name_cn`（中文名）、`max_images`（最大采集数量，默认 200）
3. THE Species_Config SHALL 支持全局配置项：`image_size`（目标尺寸，默认 518）、`phash_threshold`（去重阈值，默认 8）、`rate_limit`（API 请求间隔秒数，默认 1.0）、`download_threads`（下载并发线程数，默认 10）
4. WHEN Species_Config 文件不存在或格式无效时，THE Data_Collector SHALL 打印明确的错误信息并退出，退出码非零
5. WHEN Species_Config 中某个物种缺少必填字段（`taxon_id`、`scientific_name`）时，THE Data_Collector SHALL 跳过该物种并打印警告


### 需求 2：iNaturalist API 数据采集

**用户故事：** 作为开发者，我需要从 iNaturalist API 批量下载指定物种的 research grade 观察照片，以便获取高质量的训练素材。

#### 验收标准

1. WHEN 执行采集脚本时，THE Data_Collector SHALL 读取 Species_Config 并逐物种调用 iNaturalist `/v1/observations` API，查询参数包含 `taxon_id`（而非学名，避免学名歧义）、`quality_grade=research`、`photos=true`、`per_page=200`、`term_id=1`、`term_value_id=2`（仅成鸟）
2. THE Data_Collector SHALL 遵守 API 速率限制，两次请求之间间隔不少于 `rate_limit` 秒（默认 1.0 秒）
3. THE Data_Collector SHALL 对每个观察记录提取第一张照片的 URL（`observation.photos[0].url`），替换尺寸后缀为 `original` 或 `large`（优先 `original`）
4. THE Data_Collector SHALL 将下载的图片保存到 `model/data/raw/{scientific_name}/` 目录，文件名格式为 `{observation_id}_{photo_id}.jpg`
5. WHEN 单张图片下载失败（网络超时、HTTP 4xx/5xx）时，THE Data_Collector SHALL 记录错误日志并跳过该图片，继续处理下一张
6. THE Data_Collector SHALL 支持断点续传：检测目标目录中已存在的文件，跳过已下载的图片
7. WHEN 某物种的 API 返回结果不足 `max_images` 时，THE Data_Collector SHALL 下载所有可用结果并打印实际数量
8. THE Data_Collector SHALL 在采集完成后打印每个物种的统计摘要：目标数量、实际下载数量、跳过数量、失败数量

### 需求 3：数据清洗 — 去重

**用户故事：** 作为开发者，我需要对下载的图片进行去重，以避免训练数据中的重复样本影响模型质量。

#### 验收标准

1. THE Data_Cleaner SHALL 使用 imagehash 库计算每张图片的 perceptual hash（pHash）
2. WHEN 同一物种目录下两张图片的 pHash Hamming distance ≤ `phash_threshold`（默认 8）时，THE Data_Cleaner SHALL 将其标记为重复
3. WHEN 检测到重复图片组时，THE Data_Cleaner SHALL 保留文件尺寸最大的一张（假设尺寸越大质量越高），移除其余
4. THE Data_Cleaner SHALL 在去重完成后打印每个物种的去重统计：原始数量、去重后数量、移除数量
5. FOR ALL 去重后的图片集合，同一物种内任意两张图片的 pHash Hamming distance SHALL 大于 `phash_threshold`（不变量属性）

### 需求 4：数据清洗 — 质量过滤

**用户故事：** 作为开发者，我需要过滤低质量图片，以确保训练数据的有效性。

#### 验收标准

1. WHEN 图片文件无法被 Pillow 正常打开（损坏、格式不支持）时，THE Data_Cleaner SHALL 移除该图片并记录警告
2. WHEN 图片的短边像素 < 800 时，THE Data_Cleaner SHALL 移除该图片（分辨率过低，resize 到 518 后细节损失严重）
3. WHEN 图片为纯灰度且标准差 < 10 时，THE Data_Cleaner SHALL 移除该图片（疑似纯色/损坏图片）
4. THE Data_Cleaner SHALL 在质量过滤完成后打印每个物种的过滤统计：输入数量、通过数量、各原因移除数量

### 需求 5：数据清洗 — 统一 Resize

**用户故事：** 作为开发者，我需要将所有图片统一到固定尺寸，以便直接作为模型训练输入。

#### 验收标准

1. THE Data_Cleaner SHALL 将通过质量过滤的图片统一 resize 到 `image_size × image_size`（默认 518×518，通过配置可调）
2. THE Data_Cleaner SHALL 使用 letterbox resize（等比缩放 + 黑色填充），保持原图宽高比
3. THE Data_Cleaner SHALL 将 resize 后的图片保存为 JPEG 格式，质量 95
4. FOR ALL resize 后的图片，输出尺寸 SHALL 恒为 `image_size × image_size`（不变量属性）
5. THE Data_Cleaner SHALL 将 resize 后的图片保存到 `model/data/cleaned/{scientific_name}/` 目录

### 需求 6：数据集产出验证

**用户故事：** 作为开发者，我需要验证清洗后的数据集质量，确保每个物种有足够的高质量图片供 Spec 28 进一步处理。

#### 验收标准

1. THE Data_Cleaner SHALL 将清洗后的图片按物种组织到 `model/data/cleaned/{scientific_name}/` 目录
2. WHEN 某物种清洗后图片数 < 30 时，THE Pipeline SHALL 打印警告但继续处理
3. THE Pipeline SHALL 在清洗完成后打印数据集统计：总物种数、每物种的原始/去重/过滤/最终数量
4. THE Pipeline SHALL 不做 train/val 划分（划分由 Spec 28 在 DINOv3 特征空间去噪后执行，确保最终数据集的纯度）

### 需求 7：Taxonomy 映射表生成

**用户故事：** 作为开发者，我需要生成物种的分类学映射表，以便后续 Spec 18 将识别结果写入 DynamoDB 时附带完整的分类信息。

#### 验收标准

1. THE Data_Collector SHALL 在采集过程中调用 iNaturalist `/v1/taxa/{taxon_id}` API 获取每个物种的完整分类层级
2. THE Taxonomy_Map SHALL 为每个物种生成以下字段：`taxon_id`、`scientific_name`、`common_name_cn`（中文名）、`common_name_en`（英文名）、`family`（科学名）、`family_cn`（科中文名）、`order`（目学名）、`order_cn`（目中文名）、`class_label`（模型输出的类别索引，从 0 开始）
3. THE Taxonomy_Map SHALL 以 JSON 格式保存到 `model/data/taxonomy.json`
4. THE Taxonomy_Map SHALL 同时生成 `class_label → scientific_name` 的反向映射，嵌入同一 JSON 文件的 `label_to_species` 字段
5. WHEN iNaturalist API 返回的分类信息缺少中文名时，THE Data_Collector SHALL 使用英文名填充中文名字段，并打印警告

### 需求 8：单元测试

**用户故事：** 作为开发者，我需要通过单元测试验证数据采集和清洗逻辑的正确性。

#### 验收标准

1. THE Test_Suite SHALL 包含 Species_Config 解析测试：有效配置、缺少必填字段、文件不存在
2. THE Test_Suite SHALL 包含 pHash 去重逻辑测试：相同图片 → 去重、不同图片 → 保留、阈值边界
3. THE Test_Suite SHALL 包含 letterbox resize 测试：正方形输入、横向矩形、纵向矩形，输出尺寸恒为目标尺寸（PBT 属性）
4. THE Test_Suite SHALL 包含 taxonomy JSON 格式验证测试：必填字段存在、class_label 连续且从 0 开始、label_to_species 反向映射一致
5. THE Test_Suite SHALL 使用 pytest 运行，命令为 `source .venv-raspi-eye/bin/activate && pytest model/tests/ -v`
6. THE Test_Suite SHALL 不依赖网络（使用 mock 或本地测试数据），确保离线可运行

### 需求 9：端到端验证

**用户故事：** 作为开发者，我需要一个端到端的验证流程，确认从采集到数据集产出的完整链路正常工作。

#### 验收标准

1. WHEN 执行 `python model/prepare_dataset.py --config model/config/species.yaml` 时，THE Pipeline SHALL 依次执行：采集 → 去重 → 质量过滤 → resize → taxonomy 生成
2. THE Pipeline SHALL 在执行完成后打印完整的统计报告：每物种的采集/去重/过滤/最终数量、总耗时
3. THE Pipeline SHALL 支持 `--skip-download` 参数，跳过采集步骤直接从已有的 raw 数据执行清洗流程
4. THE Pipeline SHALL 支持 `--species` 参数指定单个物种（scientific_name），用于调试单物种流程
5. IF 任何物种的最终图片数为 0，THEN THE Pipeline SHALL 打印错误信息但继续处理其他物种

## 参考代码

### iNaturalist API 调用示例

```python
import requests
import time

def fetch_observations(taxon_id: int, per_page: int = 200, page: int = 1) -> dict:
    """查询指定物种的 research grade 观察记录"""
    url = "https://api.inaturalist.org/v1/observations"
    params = {
        "taxon_id": taxon_id,
        "quality_grade": "research",
        "photos": "true",
        "per_page": per_page,
        "page": page,
        "order": "desc",
        "order_by": "votes",  # 优先高票数（质量更好）
    }
    resp = requests.get(url, params=params, timeout=30)
    resp.raise_for_status()
    return resp.json()

def get_photo_url(observation: dict, size: str = "original") -> str | None:
    """从观察记录中提取照片 URL"""
    photos = observation.get("photos", [])
    if not photos:
        return None
    url = photos[0]["url"]
    # iNaturalist URL 格式: .../square.jpg → 替换为 original.jpg 或 large.jpg
    return url.replace("square", size)
```

### Perceptual Hash 去重示例

```python
import imagehash
from PIL import Image

def compute_phash(image_path: str) -> imagehash.ImageHash:
    """计算图片的 perceptual hash"""
    img = Image.open(image_path)
    return imagehash.phash(img)

def find_duplicates(image_paths: list[str], threshold: int = 8) -> list[set[str]]:
    """找出重复图片组"""
    hashes = [(p, compute_phash(p)) for p in image_paths]
    duplicates = []
    # 两两比较 Hamming distance
    # ...
```

### Species Config YAML 示例

```yaml
# model/config/species.yaml
global:
  image_size: 518
  phash_threshold: 8
  rate_limit: 1.0
  random_seed: 42
  download_threads: 10

species:
  # --- 噪鹛科（核心区分目标）---
  - taxon_id: 144814
    scientific_name: "Pterorhinus sannio"
    common_name_cn: "白颊噪鹛"
    max_images: 200

  - taxon_id: 12727
    scientific_name: "Garrulax canorus"
    common_name_cn: "画眉"
    max_images: 200

  - taxon_id: 18688
    scientific_name: "Leiothrix lutea"
    common_name_cn: "红嘴相思鸟"
    max_images: 200

  - taxon_id: 793012
    scientific_name: "Pterorhinus perspicillatus"
    common_name_cn: "黑脸噪鹛"
    max_images: 200

  # --- 鹎科 ---
  - taxon_id: 13482
    scientific_name: "Pycnonotus sinensis"
    common_name_cn: "白头鹎"
    max_images: 200

  # --- 鸠鸽类 ---
  - taxon_id: 3454
    scientific_name: "Spilopelia chinensis"
    common_name_cn: "珠颈斑鸠"
    max_images: 200

  # --- 雀类 ---
  - taxon_id: 13858
    scientific_name: "Passer montanus"
    common_name_cn: "树麻雀"
    max_images: 200

  # ... 共 37 个物种，完整列表见 model/config/species.yaml
```

注意事项：
- `taxon_id` 必须从 iNaturalist 网站确认（搜索学名 → URL 中的数字即为 taxon_id），上面的 ID 为示例值
- 部分物种学名在不同分类系统中有差异（如 `Suthora webbiana` vs `Sinosuthora webbiana`），以 iNaturalist 采用的学名为准，通过 taxon_id 查询可避免学名歧义
- 中国特有种（如黄腹山雀 `Pardaliparus venustulus`）在 iNaturalist 上 research grade 照片可能不足 200 张，脚本会下载所有可用结果并打印实际数量

### Taxonomy JSON 输出示例

```json
{
  "species": {
    "Pterorhinus sannio": {
      "taxon_id": 144814,
      "scientific_name": "Pterorhinus sannio",
      "common_name_cn": "白颊噪鹛",
      "common_name_en": "White-browed Laughingthrush",
      "family": "Leiothrichidae",
      "family_cn": "噪鹛科",
      "order": "Passeriformes",
      "order_cn": "雀形目",
      "class_label": 0
    },
    "Garrulax canorus": {
      "taxon_id": 12727,
      "scientific_name": "Garrulax canorus",
      "common_name_cn": "画眉",
      "common_name_en": "Chinese Hwamei",
      "family": "Leiothrichidae",
      "family_cn": "噪鹛科",
      "order": "Passeriformes",
      "order_cn": "雀形目",
      "class_label": 1
    }
  },
  "label_to_species": {
    "0": "Pterorhinus sannio",
    "1": "Garrulax canorus"
  }
}
```

## 验证命令

```bash
# 激活 venv
source .venv-raspi-eye/bin/activate

# 运行单元测试（离线，不依赖网络）
pytest model/tests/ -v

# 端到端采集 + 清洗（需要网络）
python model/prepare_dataset.py --config model/config/species.yaml

# 仅清洗已有数据（跳过下载）
python model/prepare_dataset.py --config model/config/species.yaml --skip-download

# 单物种调试
python model/prepare_dataset.py --config model/config/species.yaml --species "Passer montanus"
```

预期结果：单元测试全部通过（离线）；端到端执行后 `model/data/cleaned/` 下按物种分目录生成清洗后图片，`model/data/taxonomy.json` 包含完整分类信息。

## 明确不包含

- 模型训练或微调（Spec 28: bird-classifier-training）
- SageMaker endpoint 部署（Spec 17）
- Lambda 触发器和 DynamoDB 写入（Spec 18，本 Spec 只生成 taxonomy.json 供其使用）
- 数据增强（data augmentation，属于 Spec 28 训练阶段）
- 图片标注或 bounding box 标注（本 Spec 做图像分类数据集，不做目标检测数据集）
- Web UI 浏览数据集
- 自动化定期更新数据集（一次性采集脚本）
- 多来源数据融合（仅使用 iNaturalist）
- 视频数据采集（仅采集静态图片）

