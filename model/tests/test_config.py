"""配置解析单元测试。

测试 model/src/config.py 中的 load_config 函数。
所有测试使用 tmp_path fixture，不依赖网络。
"""

import yaml
import pytest

from model.src.config import (
    DatasetConfig,
    GlobalConfig,
    SpeciesEntry,
    load_config,
)


def _write_yaml(tmp_path, data, filename="species.yaml"):
    """辅助：将 Python 对象写入临时 YAML 文件并返回路径。"""
    path = tmp_path / filename
    path.write_text(yaml.dump(data, allow_unicode=True), encoding="utf-8")
    return str(path)


# ---------- 1. 有效配置解析 ----------

def test_load_valid_config(tmp_path):
    """有效 YAML，验证全局配置和物种列表正确解析。"""
    data = {
        "global": {
            "image_size": 224,
            "phash_threshold": 10,
            "rate_limit": 2.0,
            "random_seed": 99,
            "download_threads": 4,
        },
        "species": [
            {
                "taxon_id": 144814,
                "scientific_name": "Pterorhinus sannio",
                "common_name_cn": "白颊噪鹛",
                "max_images": 2000,
            },
            {
                "taxon_id": 13695,
                "scientific_name": "Passer montanus",
                "common_name_cn": "树麻雀",
            },
        ],
    }
    cfg = load_config(_write_yaml(tmp_path, data))

    assert isinstance(cfg, DatasetConfig)
    # 全局配置
    assert cfg.global_config.image_size == 224
    assert cfg.global_config.phash_threshold == 10
    assert cfg.global_config.rate_limit == 2.0
    assert cfg.global_config.random_seed == 99
    assert cfg.global_config.download_threads == 4
    # 物种列表
    assert len(cfg.species) == 2
    assert cfg.species[0].taxon_id == 144814
    assert cfg.species[0].scientific_name == "Pterorhinus sannio"
    assert cfg.species[0].common_name_cn == "白颊噪鹛"
    assert cfg.species[0].max_images == 2000
    # 第二个物种使用 max_images 默认值
    assert cfg.species[1].taxon_id == 13695
    assert cfg.species[1].max_images == 200


# ---------- 2. 缺少必填字段的物种被跳过 ----------

def test_missing_required_fields_skipped(tmp_path):
    """物种缺少 taxon_id 或 scientific_name 被跳过。"""
    data = {
        "species": [
            {"scientific_name": "No Taxon"},           # 缺 taxon_id
            {"taxon_id": 999},                          # 缺 scientific_name
            {"common_name_cn": "啥都没有"},              # 两个都缺
            {"taxon_id": 100, "scientific_name": "OK"},  # 有效
        ],
    }
    cfg = load_config(_write_yaml(tmp_path, data))

    assert len(cfg.species) == 1
    assert cfg.species[0].scientific_name == "OK"


# ---------- 3. 文件不存在 ----------

def test_file_not_found(tmp_path):
    """不存在的路径 raise FileNotFoundError。"""
    with pytest.raises(FileNotFoundError):
        load_config(str(tmp_path / "nonexistent.yaml"))


# ---------- 4. 非法 YAML ----------

def test_invalid_yaml(tmp_path):
    """非法 YAML 内容 raise ValueError。"""
    bad = tmp_path / "bad.yaml"
    bad.write_text(":\n  - :\n    {{invalid", encoding="utf-8")
    with pytest.raises(ValueError, match="YAML"):
        load_config(str(bad))


# ---------- 5. 全局配置默认值 ----------

def test_global_defaults(tmp_path):
    """不提供 global 段时使用默认值。"""
    data = {
        "species": [
            {"taxon_id": 1, "scientific_name": "Test species"},
        ],
    }
    cfg = load_config(_write_yaml(tmp_path, data))

    assert cfg.global_config.image_size == 518
    assert cfg.global_config.phash_threshold == 8
    assert cfg.global_config.rate_limit == 1.0
    assert cfg.global_config.random_seed == 42
    assert cfg.global_config.download_threads == 10


# ---------- 6. species 为空列表 ----------

def test_empty_species_list(tmp_path):
    """species 为空列表时返回空列表。"""
    data = {"species": []}
    cfg = load_config(_write_yaml(tmp_path, data))

    assert cfg.species == []
    assert isinstance(cfg.global_config, GlobalConfig)


# ---------- 7. species 不是列表 ----------

def test_species_not_list(tmp_path):
    """species 不是列表时 raise ValueError。"""
    data = {"species": "not a list"}
    with pytest.raises(ValueError, match="列表"):
        load_config(_write_yaml(tmp_path, data))
