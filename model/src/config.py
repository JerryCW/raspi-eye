"""配置解析与验证模块。

解析 species.yaml 配置文件，提供 GlobalConfig、SpeciesEntry、DatasetConfig 三个 dataclass。
"""

import os
from dataclasses import dataclass, field

import yaml


@dataclass
class GlobalConfig:
    """全局配置项。"""

    image_size: int = 518
    phash_threshold: int = 8
    rate_limit: float = 1.0
    random_seed: int = 42
    download_threads: int = 10


@dataclass
class SpeciesEntry:
    """单个物种配置。"""

    taxon_id: int
    scientific_name: str
    common_name_cn: str = ""
    max_images: int = 200


@dataclass
class DatasetConfig:
    """数据集完整配置（全局 + 物种列表）。"""

    global_config: GlobalConfig
    species: list[SpeciesEntry]


def load_config(config_path: str) -> DatasetConfig:
    """加载并验证 species.yaml 配置文件。

    Raises:
        FileNotFoundError: 配置文件不存在
        ValueError: YAML 格式无效或缺少必填字段
    Returns:
        DatasetConfig: 解析后的配置对象（跳过缺少必填字段的物种并打印警告）
    """
    if not os.path.isfile(config_path):
        raise FileNotFoundError(f"配置文件不存在: {config_path}")

    with open(config_path, "r", encoding="utf-8") as f:
        try:
            raw = yaml.safe_load(f)
        except yaml.YAMLError as e:
            raise ValueError(f"YAML 格式无效: {e}") from e

    if not isinstance(raw, dict):
        raise ValueError("YAML 顶层必须是字典")

    # 解析全局配置
    global_raw = raw.get("global", {})
    if not isinstance(global_raw, dict):
        global_raw = {}
    global_config = GlobalConfig(**{
        k: v for k, v in global_raw.items() if k in GlobalConfig.__dataclass_fields__
    })

    # 解析物种列表
    species_raw = raw.get("species", [])
    if not isinstance(species_raw, list):
        raise ValueError("species 字段必须是列表")

    species: list[SpeciesEntry] = []
    for i, entry in enumerate(species_raw):
        if not isinstance(entry, dict):
            print(f"警告: species[{i}] 不是字典，跳过")
            continue

        # 检查必填字段
        missing = []
        if "taxon_id" not in entry:
            missing.append("taxon_id")
        if "scientific_name" not in entry:
            missing.append("scientific_name")

        if missing:
            print(f"警告: species[{i}] 缺少必填字段 {missing}，跳过")
            continue

        species.append(SpeciesEntry(**{
            k: v
            for k, v in entry.items()
            if k in SpeciesEntry.__dataclass_fields__
        }))

    return DatasetConfig(global_config=global_config, species=species)
