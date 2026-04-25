"""Spec 17 推理脚本 + 打包模块测试。

包含：
- Property 1: input_fn 输出 shape 不变量 (PBT)
- Property 2: input_fn 拒绝非法 content_type (PBT)
- Property 3: 推理 round-trip (PBT)
- 单元测试: model_fn 加载、HF_TOKEN 回退、损坏图片、打包结构
"""

import io
import json
import os
import random
import sys
import tarfile
import tempfile
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest
import torch
import torch.nn as nn
from hypothesis import HealthCheck, assume, given, settings
from hypothesis import strategies as st
from PIL import Image

# 确保项目根目录在 sys.path 中
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

from model.training.augmentation import get_val_transform
from model.training.backbone_registry import BACKBONE_REGISTRY, BackboneConfig
from model.training.classifier import BirdClassifier

# ── MockBackbone 配置 ─────────────────────────────────────────────────────────

MOCK_INPUT_SIZE = 32
MOCK_FEATURE_DIM = 64
MOCK_NUM_CLASSES = 5
MOCK_CLASS_NAMES = ["species_a", "species_b", "species_c", "species_d", "species_e"]


class MockBackbone(nn.Module):
    """测试用小型 backbone，随机初始化，不依赖网络。"""

    def __init__(self, feature_dim: int = 64):
        super().__init__()
        self.proj = nn.Linear(3 * MOCK_INPUT_SIZE * MOCK_INPUT_SIZE, feature_dim)

    def forward(self, x):
        return self.proj(x.flatten(1))


def mock_load_fn():
    return MockBackbone(feature_dim=MOCK_FEATURE_DIM)


def mock_extract_fn(model, x):
    return model(x)


MOCK_BACKBONE_CONFIG = BackboneConfig(
    name="mock-backbone",
    load_fn=mock_load_fn,
    extract_fn=mock_extract_fn,
    input_size=MOCK_INPUT_SIZE,
    feature_dim=MOCK_FEATURE_DIM,
    needs_hf_token=False,
)

# 合法 content type 集合
VALID_TYPES = {"image/jpeg", "application/x-image"}


# ── Fixtures ──────────────────────────────────────────────────────────────────


@pytest.fixture(autouse=True)
def _patch_backbone_registry(monkeypatch):
    """所有测试自动 monkey-patch BACKBONE_REGISTRY，注入 MockBackbone。"""
    monkeypatch.setitem(BACKBONE_REGISTRY, "mock-backbone", MOCK_BACKBONE_CONFIG)


@pytest.fixture
def mock_model_dir(tmp_path):
    """创建包含 MockBackbone 模型的临时目录，模拟 model_fn 的输入。"""
    model = BirdClassifier(
        num_classes=MOCK_NUM_CLASSES, backbone_config=MOCK_BACKBONE_CONFIG
    )
    checkpoint = {
        "state_dict": model.state_dict(),
        "metadata": {
            "backbone_name": "mock-backbone",
            "num_classes": MOCK_NUM_CLASSES,
            "class_names": MOCK_CLASS_NAMES,
            "input_size": MOCK_INPUT_SIZE,
            "feature_dim": MOCK_FEATURE_DIM,
        },
    }
    torch.save(checkpoint, tmp_path / "bird_classifier.pt")
    return str(tmp_path)


def _make_jpeg_bytes(width: int, height: int) -> bytes:
    """生成指定尺寸的随机 JPEG 图片字节。"""
    r = random.randint(0, 255)
    g = random.randint(0, 255)
    b = random.randint(0, 255)
    img = Image.new("RGB", (width, height), color=(r, g, b))
    buf = io.BytesIO()
    img.save(buf, format="JPEG")
    return buf.getvalue()


def _create_mock_model_dir() -> str:
    """创建临时模型目录（用于 PBT 测试，不依赖 pytest fixture）。"""
    tmp_dir = tempfile.mkdtemp()
    model = BirdClassifier(
        num_classes=MOCK_NUM_CLASSES, backbone_config=MOCK_BACKBONE_CONFIG
    )
    checkpoint = {
        "state_dict": model.state_dict(),
        "metadata": {
            "backbone_name": "mock-backbone",
            "num_classes": MOCK_NUM_CLASSES,
            "class_names": MOCK_CLASS_NAMES,
            "input_size": MOCK_INPUT_SIZE,
            "feature_dim": MOCK_FEATURE_DIM,
        },
    }
    torch.save(checkpoint, os.path.join(tmp_dir, "bird_classifier.pt"))
    return tmp_dir


# ── Task 1.3: Property 1 — input_fn 输出 shape 不变量 ─────────────────────────


class TestInputFnShapeInvariant:
    """**Validates: Requirements 1.3, 5.2**"""

    @settings(max_examples=100)
    @given(
        width=st.integers(min_value=32, max_value=2048),
        height=st.integers(min_value=32, max_value=2048),
    )
    def test_input_fn_output_shape_invariant(self, width, height):
        """对任意合法 JPEG 图片，input_fn 输出 shape 恒为 (1, 3, input_size, input_size)。"""
        import model.endpoint.inference as inf_module

        inf_module._input_size = MOCK_INPUT_SIZE

        jpeg_bytes = _make_jpeg_bytes(width, height)
        tensor = inf_module.input_fn(jpeg_bytes, "image/jpeg")
        assert tensor.shape == (1, 3, MOCK_INPUT_SIZE, MOCK_INPUT_SIZE)


# ── Task 1.4: Property 2 — input_fn 拒绝非法 content_type ────────────────────


class TestInputFnRejectsInvalidContentType:
    """**Validates: Requirements 1.4**"""

    @settings(max_examples=100)
    @given(content_type=st.text(min_size=1, max_size=100))
    def test_input_fn_rejects_invalid_content_type(self, content_type):
        """对任意非法 content_type，input_fn 应抛出 ValueError。"""
        assume(content_type not in VALID_TYPES)

        from model.endpoint.inference import input_fn

        with pytest.raises(ValueError):
            input_fn(b"dummy", content_type)


# ── Task 1.5: Property 3 — 推理 round-trip ───────────────────────────────────


class TestInferenceRoundTrip:
    """**Validates: Requirements 1.3, 1.6, 1.7, 5.3, 5.4, 5.6**"""

    @pytest.fixture(autouse=True)
    def _setup_model(self):
        """在 PBT 循环外一次性加载模型。"""
        import model.endpoint.inference as inf_module

        # 注入 mock backbone 到 registry
        BACKBONE_REGISTRY["mock-backbone"] = MOCK_BACKBONE_CONFIG

        model_dir = _create_mock_model_dir()
        self._model_dict = inf_module.model_fn(model_dir)
        yield
        # 清理临时目录
        import shutil
        shutil.rmtree(model_dir, ignore_errors=True)

    @settings(
        max_examples=100,
        suppress_health_check=[HealthCheck.function_scoped_fixture],
    )
    @given(
        width=st.integers(min_value=32, max_value=2048),
        height=st.integers(min_value=32, max_value=2048),
    )
    def test_inference_round_trip(self, width, height):
        """完整链路 input_fn → predict_fn → output_fn 输出满足所有不变量。"""
        import model.endpoint.inference as inf_module

        jpeg_bytes = _make_jpeg_bytes(width, height)

        # input_fn → predict_fn → output_fn
        tensor = inf_module.input_fn(jpeg_bytes, "image/jpeg")
        prediction = inf_module.predict_fn(tensor, self._model_dict)
        json_str, content_type = inf_module.output_fn(prediction, "application/json")

        # 验证输出为合法 JSON
        assert content_type == "application/json"
        data = json.loads(json_str)

        # predictions 列表，长度 ∈ [1, 5]
        preds = data["predictions"]
        assert isinstance(preds, list)
        assert 1 <= len(preds) <= 5

        # 每项含 species（非空字符串）和 confidence（∈ (0.0, 1.0]）
        total_confidence = 0.0
        for p in preds:
            assert isinstance(p["species"], str) and len(p["species"]) > 0
            assert 0.0 < p["confidence"] <= 1.0
            total_confidence += p["confidence"]

        # 所有 confidence 之和 ≤ 1.0（softmax 属性，top-k 子集）
        assert total_confidence <= 1.0 + 1e-6

        # model_metadata
        meta = data["model_metadata"]
        assert isinstance(meta["backbone"], str) and len(meta["backbone"]) > 0
        assert isinstance(meta["num_classes"], int) and meta["num_classes"] > 0


# ── Task 1.6: 单元测试 ───────────────────────────────────────────────────────


class TestModelFn:
    """model_fn 加载测试。"""

    def test_model_fn_loads_and_returns_dict(self, mock_model_dir):
        """MockBackbone 创建 .pt → model_fn 加载 → 验证返回字典结构。"""
        import model.endpoint.inference as inf_module

        result = inf_module.model_fn(mock_model_dir)

        assert "model" in result
        assert "transform" in result
        assert "class_names" in result
        assert "metadata" in result

        assert isinstance(result["model"], BirdClassifier)
        assert result["class_names"] == MOCK_CLASS_NAMES
        assert result["metadata"]["backbone_name"] == "mock-backbone"
        assert result["metadata"]["num_classes"] == MOCK_NUM_CLASSES

        # 验证 eval 模式
        assert not result["model"].training


class TestHfTokenFallback:
    """HF_TOKEN 回退测试。"""

    def test_hf_token_fallback_to_env(self, monkeypatch):
        """Mock Secrets Manager 失败 → 验证从环境变量获取。"""
        from model.endpoint.inference import _get_hf_token

        # boto3 在 _get_hf_token 内部 import，需要 mock boto3 模块级别
        mock_client = MagicMock()
        mock_client.get_secret_value.side_effect = Exception("SecretsManager unavailable")

        mock_boto3 = MagicMock()
        mock_boto3.client.return_value = mock_client

        monkeypatch.setenv("HF_TOKEN", "test-token-from-env")
        with patch.dict("sys.modules", {"boto3": mock_boto3}):
            token = _get_hf_token()
            assert token == "test-token-from-env"

    def test_hf_token_returns_none_when_both_fail(self, monkeypatch):
        """Secrets Manager 和环境变量都不可用时返回 None。"""
        from model.endpoint.inference import _get_hf_token

        mock_client = MagicMock()
        mock_client.get_secret_value.side_effect = Exception("SecretsManager unavailable")

        mock_boto3 = MagicMock()
        mock_boto3.client.return_value = mock_client

        monkeypatch.delenv("HF_TOKEN", raising=False)
        with patch.dict("sys.modules", {"boto3": mock_boto3}):
            token = _get_hf_token()
            assert token is None


class TestInputFnCorruptedImage:
    """input_fn 损坏图片测试。"""

    def test_input_fn_raises_on_corrupted_image(self):
        """随机字节流 → 验证 ValueError。"""
        from model.endpoint.inference import input_fn

        corrupted_bytes = os.urandom(256)
        with pytest.raises(ValueError, match="无法解码图片数据"):
            input_fn(corrupted_bytes, "image/jpeg")


# ── Task 2.2: 单元测试 — model.tar.gz 打包结构 ───────────────────────────────


class TestPackageModel:
    """model.tar.gz 打包结构验证。"""

    def test_package_model_tar_structure(self, tmp_path):
        """创建临时 .pt 和 class_names.json → 打包 → 解压验证内部结构。"""
        from model.endpoint.packager import package_model

        # 创建临时模型文件
        model_file = tmp_path / "test_model.pt"
        torch.save({"dummy": "data"}, str(model_file))

        # 创建临时 class_names.json
        class_names_file = tmp_path / "class_names.json"
        class_names_file.write_text(json.dumps(MOCK_CLASS_NAMES))

        output_dir = tmp_path / "output"

        # 打包（不上传 S3）
        result_path = package_model(
            model_path=str(model_file),
            class_names_path=str(class_names_file),
            output_dir=str(output_dir),
            s3_bucket=None,
            backbone_name="mock-backbone",
        )

        # 验证返回路径
        assert result_path.endswith("model.tar.gz")
        assert os.path.exists(result_path)

        # 解压并验证内部结构
        extract_dir = tmp_path / "extracted"
        with tarfile.open(result_path, "r:gz") as tar:
            tar.extractall(path=str(extract_dir))

        # 验证根目录含 bird_classifier.pt、class_names.json
        assert (extract_dir / "bird_classifier.pt").exists()
        assert (extract_dir / "class_names.json").exists()

        # 验证 code/ 含 inference.py、requirements.txt
        assert (extract_dir / "code" / "inference.py").exists()
        assert (extract_dir / "code" / "requirements.txt").exists()

        # 验证 class_names.json 内容正确
        with open(extract_dir / "class_names.json") as f:
            loaded = json.load(f)
        assert loaded == MOCK_CLASS_NAMES
