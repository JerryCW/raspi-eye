"""Taxonomy 格式验证测试 + PBT 属性 4。

覆盖：
- taxonomy JSON 必填字段存在
- class_label 从 0 开始连续编号
- label_to_species 反向映射与 species 段一致
- 缺少中文名时用英文名填充
- PBT 属性 4：Taxonomy 结构一致性

所有测试使用 mock API 响应，不依赖网络。
"""

from unittest.mock import patch, MagicMock

import pytest

from model.collection.config import DatasetConfig, GlobalConfig, SpeciesEntry
from model.collection.collector import DataCollector


# ============================================================
# 辅助：构造 mock API 响应
# ============================================================

def _make_taxa_response(
    scientific_name: str,
    common_name_en: str = "English Name",
    family: str = "Leiothrichidae",
    family_cn: str = "噪鹛科",
    order: str = "Passeriformes",
    order_cn: str = "雀形目",
):
    """构造 iNaturalist /v1/taxa/{id} 的 mock 响应。"""
    return {
        "results": [
            {
                "name": scientific_name,
                "preferred_common_name": common_name_en,
                "ancestors": [
                    {"rank": "order", "name": order, "preferred_common_name": order_cn},
                    {"rank": "family", "name": family, "preferred_common_name": family_cn},
                ],
            }
        ]
    }


def _make_config(species_list: list[SpeciesEntry]) -> DatasetConfig:
    """构造 DatasetConfig。"""
    return DatasetConfig(global_config=GlobalConfig(), species=species_list)


# ============================================================
# 1. taxonomy JSON 必填字段存在
# ============================================================

class TestTaxonomyRequiredFields:
    """验证 taxonomy 输出包含所有必填字段。"""

    def test_required_fields_present(self):
        """build_taxonomy 输出的每个物种应包含所有必填字段。"""
        species = [
            SpeciesEntry(taxon_id=144814, scientific_name="Pterorhinus sannio", common_name_cn="白颊噪鹛"),
            SpeciesEntry(taxon_id=13695, scientific_name="Passer montanus", common_name_cn="树麻雀"),
        ]
        config = _make_config(species)
        collector = DataCollector(config)

        mock_responses = {
            "/v1/taxa/144814": _make_taxa_response(
                "Pterorhinus sannio",
                common_name_en="White-browed Laughingthrush",
            ),
            "/v1/taxa/13695": _make_taxa_response(
                "Passer montanus",
                common_name_en="Eurasian Tree Sparrow",
                family="Passeridae",
                family_cn="麻雀科",
            ),
        }

        def mock_api_get(path, params=None):
            return mock_responses[path]

        with patch.object(collector, "_api_get", side_effect=mock_api_get):
            taxonomy = collector.build_taxonomy()

        required_fields = {
            "taxon_id", "scientific_name", "common_name_cn",
            "common_name_en", "family", "order", "class_label",
        }

        for name, info in taxonomy["species"].items():
            missing = required_fields - set(info.keys())
            assert not missing, f"{name} 缺少必填字段: {missing}"


# ============================================================
# 2. class_label 从 0 开始连续编号
# ============================================================

class TestClassLabel:
    """验证 class_label 编号规则。"""

    def test_class_labels_consecutive_from_zero(self):
        """class_label 应从 0 开始，按物种顺序连续编号。"""
        species = [
            SpeciesEntry(taxon_id=1, scientific_name="Species_A", common_name_cn="物种A"),
            SpeciesEntry(taxon_id=2, scientific_name="Species_B", common_name_cn="物种B"),
            SpeciesEntry(taxon_id=3, scientific_name="Species_C", common_name_cn="物种C"),
        ]
        config = _make_config(species)
        collector = DataCollector(config)

        def mock_api_get(path, params=None):
            return _make_taxa_response(path.split("/")[-1])

        with patch.object(collector, "_api_get", side_effect=mock_api_get):
            taxonomy = collector.build_taxonomy()

        labels = sorted(info["class_label"] for info in taxonomy["species"].values())
        assert labels == list(range(len(species)))


# ============================================================
# 3. label_to_species 反向映射与 species 段一致
# ============================================================

class TestLabelToSpecies:
    """验证 label_to_species 反向映射一致性。"""

    def test_reverse_mapping_consistent(self):
        """label_to_species[str(label)] 应等于对应物种的 scientific_name。"""
        species = [
            SpeciesEntry(taxon_id=100, scientific_name="Alpha beta", common_name_cn="甲"),
            SpeciesEntry(taxon_id=200, scientific_name="Gamma delta", common_name_cn="乙"),
        ]
        config = _make_config(species)
        collector = DataCollector(config)

        def mock_api_get(path, params=None):
            return _make_taxa_response("mock")

        with patch.object(collector, "_api_get", side_effect=mock_api_get):
            taxonomy = collector.build_taxonomy()

        # 正向：species 段中每个物种的 class_label 在 label_to_species 中有对应
        for name, info in taxonomy["species"].items():
            label_str = str(info["class_label"])
            assert label_str in taxonomy["label_to_species"]
            assert taxonomy["label_to_species"][label_str] == name

        # 反向：label_to_species 中每个条目在 species 段中有对应
        for label_str, sci_name in taxonomy["label_to_species"].items():
            assert sci_name in taxonomy["species"]
            assert taxonomy["species"][sci_name]["class_label"] == int(label_str)


# ============================================================
# 4. 缺少中文名时用英文名填充
# ============================================================

class TestChineseNameFallback:
    """验证缺少中文名时的填充逻辑。"""

    def test_missing_cn_name_filled_with_en(self):
        """common_name_cn 为空时，应用英文名填充。"""
        species = [
            SpeciesEntry(taxon_id=999, scientific_name="Rarus bird", common_name_cn=""),
        ]
        config = _make_config(species)
        collector = DataCollector(config)

        def mock_api_get(path, params=None):
            return _make_taxa_response(
                "Rarus bird",
                common_name_en="Rare Bird",
            )

        with patch.object(collector, "_api_get", side_effect=mock_api_get):
            taxonomy = collector.build_taxonomy()

        info = taxonomy["species"]["Rarus bird"]
        assert info["common_name_cn"] == "Rare Bird"
        assert info["common_name_en"] == "Rare Bird"


# ============================================================
# PBT 属性 4：Taxonomy 结构一致性
# ============================================================

from hypothesis import given, settings, HealthCheck
from hypothesis.strategies import (
    integers,
    text,
    lists,
    composite,
)



# Feature: inat-data-collection, Property 4: Taxonomy 结构一致性
# **Validates: Requirements 7.2, 7.4**


@composite
def species_entries(draw, min_count=1, max_count=50):
    """Hypothesis 策略：生成随机物种列表（1~50 个物种）。

    每个物种有唯一的 scientific_name 和 taxon_id。
    """
    count = draw(integers(min_value=min_count, max_value=max_count))
    entries = []
    used_names = set()
    for i in range(count):
        # 生成唯一的 scientific_name
        name = f"Genus_{i} species_{i}"
        while name in used_names:
            suffix = draw(integers(min_value=0, max_value=99999))
            name = f"Genus_{i} species_{suffix}"
        used_names.add(name)
        entries.append(
            SpeciesEntry(
                taxon_id=1000 + i,
                scientific_name=name,
                common_name_cn=f"中文名{i}",
            )
        )
    return entries


@given(species=species_entries())
@settings(max_examples=100, suppress_health_check=[HealthCheck.too_slow], deadline=None)
def test_taxonomy_structure_consistency(species):
    """Taxonomy 结构一致性：class_label 从 0 开始连续编号，
    label_to_species 双向映射一致。

    生成随机物种列表，调用 build_taxonomy，验证：
    1. class_label 从 0 开始连续编号
    2. label_to_species[str(label)] == 对应物种的 scientific_name
    """
    config = _make_config(species)
    collector = DataCollector(config)

    # mock fetch_taxonomy 返回固定格式 dict，避免真实 API 调用
    def mock_fetch_taxonomy(entry):
        return {
            "taxon_id": entry.taxon_id,
            "scientific_name": entry.scientific_name,
            "common_name_cn": entry.common_name_cn or entry.scientific_name,
            "common_name_en": entry.scientific_name,
            "family": "MockFamily",
            "family_cn": "模拟科",
            "order": "MockOrder",
            "order_cn": "模拟目",
        }

    with patch.object(collector, "fetch_taxonomy", side_effect=mock_fetch_taxonomy):
        taxonomy = collector.build_taxonomy()

    n = len(species)

    # 验证 1：class_label 从 0 开始连续编号
    labels = sorted(info["class_label"] for info in taxonomy["species"].values())
    assert labels == list(range(n)), (
        f"class_label 不连续：期望 {list(range(n))}，实际 {labels}"
    )

    # 验证 2：label_to_species 双向映射一致
    assert len(taxonomy["label_to_species"]) == n

    for name, info in taxonomy["species"].items():
        label_str = str(info["class_label"])
        assert taxonomy["label_to_species"][label_str] == name, (
            f"label_to_species[{label_str}] = {taxonomy['label_to_species'].get(label_str)}, "
            f"期望 {name}"
        )

    for label_str, sci_name in taxonomy["label_to_species"].items():
        assert sci_name in taxonomy["species"], (
            f"label_to_species 中的 {sci_name} 不在 species 段中"
        )
        assert taxonomy["species"][sci_name]["class_label"] == int(label_str)
