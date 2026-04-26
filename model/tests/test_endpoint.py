"""Spec 17 + Spec 30 推理脚本 + 打包模块测试。

包含：
- Spec 17 Property 1: input_fn 输出 shape 不变量 (PBT)
- Spec 17 Property 2: input_fn 拒绝非法 content_type (PBT)
- Spec 17 Property 3: 推理 round-trip (PBT)（Spec 30 更新：含 cropped_image_b64）
- Spec 30 Property 1: letterbox_resize 输出尺寸不变量 (PBT)
- Spec 30 Property 2: 内联 letterbox_resize 与 cleaning 版等价性 (PBT)
- Spec 30 Property 3: input_fn 输出结构不变量 (PBT)
- 单元测试: model_fn 加载、HF_TOKEN 回退、损坏图片、打包结构
- Spec 30 单元测试: model_fn YOLO 加载、input_fn 路径、output_fn 编码、打包含 yolo11s.pt
"""

import base64
import io
import json
import os
import random
import sys
import tarfile
import tempfile
from pathlib import Path
from unittest.mock import MagicMock, patch

import numpy as np
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


class _MockBackboneOutput:
    """MockBackbone 的输出，模拟 HuggingFace 模型的 last_hidden_state 接口。"""

    def __init__(self, features: torch.Tensor):
        # last_hidden_state shape: (batch, seq_len, feature_dim)
        # _BirdClassifier 取 [:, 0]，所以 seq_len >= 1
        self.last_hidden_state = features.unsqueeze(1)


class MockBackbone(nn.Module):
    """测试用小型 backbone，随机初始化，不依赖网络。"""

    def __init__(self, feature_dim: int = 64):
        super().__init__()
        self.proj = nn.Linear(3 * MOCK_INPUT_SIZE * MOCK_INPUT_SIZE, feature_dim)

    def forward(self, x):
        features = self.proj(x.flatten(1))
        return _MockBackboneOutput(features)


def mock_load_fn():
    return MockBackbone(feature_dim=MOCK_FEATURE_DIM)


def mock_extract_fn(model, x):
    output = model(x)
    # BirdClassifier (training) 使用 extract_fn，返回 features tensor
    return output.last_hidden_state[:, 0]


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
        """对任意合法 JPEG 图片，input_fn 输出 tensor shape 恒为 (1, 3, input_size, input_size)。

        Spec 30 更新：input_fn 返回 dict，从 "tensor" 键提取张量检查 shape。
        """
        import model.endpoint.inference as inf_module

        inf_module._input_size = MOCK_INPUT_SIZE

        jpeg_bytes = _make_jpeg_bytes(width, height)
        result = inf_module.input_fn(jpeg_bytes, "image/jpeg")
        # Spec 30: input_fn 返回 dict
        assert isinstance(result, dict)
        assert "tensor" in result
        assert result["tensor"].shape == (1, 3, MOCK_INPUT_SIZE, MOCK_INPUT_SIZE)


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
    """**Validates: Requirements 1.3, 1.6, 1.7, 5.3, 5.4, 5.6, 2.1, 2.3, 3.1, 3.2, 3.3, 3.4, 8.7**

    Spec 30 Property 4: 推理 round-trip 不变量（含 cropped_image_b64）
    """

    @pytest.fixture(autouse=True)
    def _setup_model(self):
        """在 PBT 循环外一次性加载模型。"""
        import model.endpoint.inference as inf_module

        # 注入 mock backbone 到 registry
        BACKBONE_REGISTRY["mock-backbone"] = MOCK_BACKBONE_CONFIG

        model_dir = _create_mock_model_dir()
        with patch.object(inf_module, "_create_backbone_offline", return_value=MockBackbone(MOCK_FEATURE_DIM)):
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
        """完整链路 input_fn → predict_fn → output_fn 输出满足所有不变量。

        **Validates: Requirements 2.1, 2.3, 3.1, 3.2, 3.3, 3.4, 8.7**

        Spec 30 更新：input_fn 返回 dict，predict_fn 接收 dict，
        output_fn 输出含 cropped_image_b64 字段。
        由于 _yolo_model 默认为 None，round-trip 走回退路径，cropped_image_b64 为 null。
        """
        import model.endpoint.inference as inf_module

        jpeg_bytes = _make_jpeg_bytes(width, height)

        # input_fn → predict_fn → output_fn（Spec 30: input_fn 返回 dict）
        input_result = inf_module.input_fn(jpeg_bytes, "image/jpeg")
        prediction = inf_module.predict_fn(input_result, self._model_dict)
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

        # 所有 confidence 之和 ≤ 1.0（softmax 属性，top-k 子集，浮点精度容差 1e-4）
        assert total_confidence <= 1.0 + 1e-4

        # model_metadata
        meta = data["model_metadata"]
        assert isinstance(meta["backbone"], str) and len(meta["backbone"]) > 0
        assert isinstance(meta["num_classes"], int) and meta["num_classes"] > 0

        # Spec 30: cropped_image_b64 字段存在
        assert "cropped_image_b64" in data
        # 回退路径（_yolo_model 为 None）：cropped_image_b64 为 null
        cropped_b64 = data["cropped_image_b64"]
        if cropped_b64 is not None:
            # 如果非 null，必须是合法 base64 字符串，可解码为 JPEG bytes
            decoded = base64.b64decode(cropped_b64)
            assert len(decoded) > 0
            # 验证是合法 JPEG（JPEG magic bytes: FF D8 FF）
            assert decoded[:2] == b"\xff\xd8"


# ── Task 1.6: 单元测试 ───────────────────────────────────────────────────────


class TestModelFn:
    """model_fn 加载测试。"""

    def test_model_fn_loads_and_returns_dict(self, mock_model_dir):
        """MockBackbone 创建 .pt → model_fn 加载 → 验证返回字典结构。"""
        import model.endpoint.inference as inf_module

        with patch.object(inf_module, "_create_backbone_offline", return_value=MockBackbone(MOCK_FEATURE_DIM)):
            result = inf_module.model_fn(mock_model_dir)

        assert "model" in result
        assert "transform" in result
        assert "class_names" in result
        assert "metadata" in result
        # Spec 30: 新增 yolo_model 键
        assert "yolo_model" in result

        assert result["class_names"] == MOCK_CLASS_NAMES
        assert result["metadata"]["backbone_name"] == "mock-backbone"
        assert result["metadata"]["num_classes"] == MOCK_NUM_CLASSES


class TestHfTokenFallback:
    """HF_TOKEN 回退测试。

    注意：_get_hf_token 已在 inference.py 重构中移除（model_fn 改为离线加载），
    这些测试标记为 skip。
    """

    @pytest.mark.skip(reason="_get_hf_token 已从 inference.py 移除")
    def test_hf_token_fallback_to_env(self, monkeypatch):
        """Mock Secrets Manager 失败 → 验证从环境变量获取。"""
        pass

    @pytest.mark.skip(reason="_get_hf_token 已从 inference.py 移除")
    def test_hf_token_returns_none_when_both_fail(self, monkeypatch):
        """Secrets Manager 和环境变量都不可用时返回 None。"""
        pass


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


# ── Spec 30 辅助函数 ──────────────────────────────────────────────────────────


def _create_mock_yolo_model(detect_bird=True, conf=0.85, bbox=(100, 100, 300, 300)):
    """创建 mock YOLO 模型。"""
    mock_model = MagicMock()
    if detect_bird:
        mock_box = MagicMock()
        mock_box.cls = torch.tensor([14])  # BIRD_CLASS_ID
        mock_box.conf = torch.tensor([conf])
        mock_box.xyxy = torch.tensor([[bbox[0], bbox[1], bbox[2], bbox[3]]])
        mock_result = MagicMock()
        mock_result.boxes = [mock_box]
        mock_model.return_value = [mock_result]
    else:
        mock_result = MagicMock()
        mock_result.boxes = []
        mock_model.return_value = [mock_result]
    return mock_model


# ── Spec 30 Task 3.1: Property 1 — letterbox_resize 输出尺寸不变量 ────────────


class TestLetterboxResizeSizeInvariant:
    """**Validates: Requirements 2.2, 8.1**

    Spec 30 Property 1: letterbox_resize 输出尺寸不变量
    """

    @settings(max_examples=100)
    @given(
        width=st.integers(min_value=1, max_value=4000),
        height=st.integers(min_value=1, max_value=4000),
        target_size=st.integers(min_value=1, max_value=1024),
    )
    def test_letterbox_resize_output_size_invariant(self, width, height, target_size):
        """对任意 PIL Image（宽高 ∈ [1, 4000]），_letterbox_resize 输出尺寸恒为 (target_size, target_size)。"""
        from model.endpoint.inference import _letterbox_resize

        img = Image.new("RGB", (width, height), color=(128, 128, 128))
        result = _letterbox_resize(img, target_size)

        assert result.size == (target_size, target_size)
        assert result.mode == "RGB"


# ── Spec 30 Task 3.2: Property 2 — 内联 letterbox_resize 与 cleaning 版等价性 ─


class TestLetterboxResizeEquivalence:
    """**Validates: Requirements 2.2**

    Spec 30 Property 2: 内联 letterbox_resize 与 cleaning 版等价性
    """

    @settings(max_examples=100, deadline=None)
    @given(
        width=st.integers(min_value=1, max_value=4000),
        height=st.integers(min_value=1, max_value=4000),
    )
    def test_letterbox_resize_equivalence_with_cleaning(self, width, height):
        """inference._letterbox_resize 和 cleaning.cleaner.letterbox_resize 输出像素一致。"""
        from model.cleaning.cleaner import letterbox_resize as cleaning_letterbox
        from model.endpoint.inference import _letterbox_resize as inference_letterbox

        img = Image.new("RGB", (width, height), color=(random.randint(0, 255), random.randint(0, 255), random.randint(0, 255)))
        target_size = 518

        result_inference = inference_letterbox(img, target_size)
        result_cleaning = cleaning_letterbox(img, target_size)

        # 像素级一致
        arr_inf = np.array(result_inference)
        arr_clean = np.array(result_cleaning)
        assert arr_inf.shape == arr_clean.shape
        assert np.array_equal(arr_inf, arr_clean)


# ── Spec 30 Task 3.3: Property 3 — input_fn 输出结构不变量 ───────────────────


# YOLO 状态策略：有检测 / 无检测 / 模型不可用
_yolo_state_strategy = st.sampled_from(["detect_bird", "no_detection", "yolo_unavailable"])


class TestInputFnOutputStructureInvariant:
    """**Validates: Requirements 2.1, 2.3, 2.4, 2.7, 2.8, 2.9**

    Spec 30 Property 3: input_fn 输出结构不变量
    """

    @settings(
        max_examples=100,
        suppress_health_check=[HealthCheck.too_slow],
    )
    @given(
        width=st.integers(min_value=32, max_value=2048),
        height=st.integers(min_value=32, max_value=2048),
        yolo_state=_yolo_state_strategy,
    )
    def test_input_fn_output_structure_invariant(self, width, height, yolo_state):
        """对任意图片 + 任意 YOLO 状态，input_fn 返回 dict 含正确结构。"""
        import model.endpoint.inference as inf_module

        inf_module._input_size = MOCK_INPUT_SIZE
        original_yolo = inf_module._yolo_model

        try:
            if yolo_state == "detect_bird":
                # bbox 在图片范围内
                bx1 = min(width // 4, width - 2)
                by1 = min(height // 4, height - 2)
                bx2 = min(bx1 + max(width // 2, 1), width)
                by2 = min(by1 + max(height // 2, 1), height)
                inf_module._yolo_model = _create_mock_yolo_model(
                    detect_bird=True, conf=0.85, bbox=(bx1, by1, bx2, by2)
                )
            elif yolo_state == "no_detection":
                inf_module._yolo_model = _create_mock_yolo_model(detect_bird=False)
            else:  # yolo_unavailable
                inf_module._yolo_model = None

            jpeg_bytes = _make_jpeg_bytes(width, height)
            result = inf_module.input_fn(jpeg_bytes, "image/jpeg")

            # 验证返回 dict 结构
            assert isinstance(result, dict)
            assert "tensor" in result
            assert "cropped_image" in result

            # tensor shape 恒为 (1, 3, input_size, input_size)
            tensor = result["tensor"]
            assert tensor.shape == (1, 3, MOCK_INPUT_SIZE, MOCK_INPUT_SIZE)
            assert tensor.dtype == torch.float32

            # cropped_image 类型检查
            cropped = result["cropped_image"]
            if yolo_state == "detect_bird":
                assert isinstance(cropped, Image.Image)
                assert cropped.size == (MOCK_INPUT_SIZE, MOCK_INPUT_SIZE)
            else:
                assert cropped is None
        finally:
            inf_module._yolo_model = original_yolo


# ── Spec 30 Task 3.5: 单元测试 ───────────────────────────────────────────────


class TestModelFnYoloLoading:
    """Spec 30 单元测试：model_fn YOLO 模型加载。"""

    def test_model_fn_returns_yolo_model_key(self, mock_model_dir):
        """验证 model_fn 返回字典含 yolo_model 键（Requirements 8.5）。"""
        import model.endpoint.inference as inf_module

        with patch.object(inf_module, "_create_backbone_offline", return_value=MockBackbone(MOCK_FEATURE_DIM)):
            result = inf_module.model_fn(mock_model_dir)
        assert "yolo_model" in result

    def test_model_fn_yolo_none_when_missing(self, mock_model_dir):
        """model_dir 中无 yolo11s.pt → yolo_model 为 None（Requirements 1.3）。"""
        import model.endpoint.inference as inf_module

        with patch.object(inf_module, "_create_backbone_offline", return_value=MockBackbone(MOCK_FEATURE_DIM)):
            result = inf_module.model_fn(mock_model_dir)
        assert result["yolo_model"] is None


class TestInputFnYoloPaths:
    """Spec 30 单元测试：input_fn YOLO crop 路径和回退路径。"""

    def test_input_fn_yolo_crop_path(self):
        """mock YOLO 返回 bbox → cropped_image 非 None 且为 PIL Image（Requirements 8.2）。"""
        import model.endpoint.inference as inf_module

        inf_module._input_size = MOCK_INPUT_SIZE
        original_yolo = inf_module._yolo_model

        try:
            inf_module._yolo_model = _create_mock_yolo_model(
                detect_bird=True, conf=0.85, bbox=(50, 50, 200, 200)
            )
            jpeg_bytes = _make_jpeg_bytes(300, 300)
            result = inf_module.input_fn(jpeg_bytes, "image/jpeg")

            assert isinstance(result, dict)
            assert isinstance(result["cropped_image"], Image.Image)
            assert result["cropped_image"].size == (MOCK_INPUT_SIZE, MOCK_INPUT_SIZE)
            assert result["tensor"].shape == (1, 3, MOCK_INPUT_SIZE, MOCK_INPUT_SIZE)
        finally:
            inf_module._yolo_model = original_yolo

    def test_input_fn_fallback_no_detection(self):
        """YOLO 无检测 → cropped_image 为 None，tensor shape 正确（Requirements 8.3）。"""
        import model.endpoint.inference as inf_module

        inf_module._input_size = MOCK_INPUT_SIZE
        original_yolo = inf_module._yolo_model

        try:
            inf_module._yolo_model = _create_mock_yolo_model(detect_bird=False)
            jpeg_bytes = _make_jpeg_bytes(300, 300)
            result = inf_module.input_fn(jpeg_bytes, "image/jpeg")

            assert isinstance(result, dict)
            assert result["cropped_image"] is None
            assert result["tensor"].shape == (1, 3, MOCK_INPUT_SIZE, MOCK_INPUT_SIZE)
        finally:
            inf_module._yolo_model = original_yolo

    def test_input_fn_fallback_yolo_unavailable(self):
        """_yolo_model 为 None → 使用原始预处理流程（Requirements 8.4）。"""
        import model.endpoint.inference as inf_module

        inf_module._input_size = MOCK_INPUT_SIZE
        original_yolo = inf_module._yolo_model

        try:
            inf_module._yolo_model = None
            jpeg_bytes = _make_jpeg_bytes(300, 300)
            result = inf_module.input_fn(jpeg_bytes, "image/jpeg")

            assert isinstance(result, dict)
            assert result["cropped_image"] is None
            assert result["tensor"].shape == (1, 3, MOCK_INPUT_SIZE, MOCK_INPUT_SIZE)
        finally:
            inf_module._yolo_model = original_yolo

    def test_input_fn_returns_dict_structure(self):
        """验证 input_fn 返回值包含 tensor 和 cropped_image 键（Requirements 8.9）。"""
        import model.endpoint.inference as inf_module

        inf_module._input_size = MOCK_INPUT_SIZE
        jpeg_bytes = _make_jpeg_bytes(256, 256)
        result = inf_module.input_fn(jpeg_bytes, "image/jpeg")

        assert isinstance(result, dict)
        assert "tensor" in result
        assert "cropped_image" in result
        assert isinstance(result["tensor"], torch.Tensor)


class TestOutputFnCroppedImageB64:
    """Spec 30 单元测试：output_fn cropped_image_b64 编码（Requirements 8.10）。"""

    def test_output_fn_with_cropped_image(self):
        """YOLO crop 路径：cropped_image_b64 为合法 base64 字符串。"""
        from model.endpoint.inference import output_fn

        cropped_img = Image.new("RGB", (MOCK_INPUT_SIZE, MOCK_INPUT_SIZE), color=(100, 150, 200))
        prediction = {
            "predictions": [{"species": "test_bird", "confidence": 0.9}],
            "model_metadata": {"backbone": "mock", "num_classes": 5},
            "cropped_image": cropped_img,
        }

        json_str, content_type = output_fn(prediction, "application/json")
        data = json.loads(json_str)

        assert "cropped_image_b64" in data
        assert data["cropped_image_b64"] is not None
        # 验证合法 base64
        decoded = base64.b64decode(data["cropped_image_b64"])
        assert len(decoded) > 0
        # 验证 JPEG magic bytes
        assert decoded[:2] == b"\xff\xd8"

    def test_output_fn_without_cropped_image(self):
        """回退路径：cropped_image_b64 为 null。"""
        from model.endpoint.inference import output_fn

        prediction = {
            "predictions": [{"species": "test_bird", "confidence": 0.9}],
            "model_metadata": {"backbone": "mock", "num_classes": 5},
            "cropped_image": None,
        }

        json_str, content_type = output_fn(prediction, "application/json")
        data = json.loads(json_str)

        assert "cropped_image_b64" in data
        assert data["cropped_image_b64"] is None


class TestPackageModelWithYolo:
    """Spec 30 单元测试：model.tar.gz 打包含 yolo11s.pt（Requirements 8.6）。"""

    def test_package_model_includes_yolo(self, tmp_path):
        """提供 yolo_model_path → tar.gz 根目录包含 yolo11s.pt。"""
        from model.endpoint.packager import package_model

        # 创建临时模型文件
        model_file = tmp_path / "test_model.pt"
        torch.save({"dummy": "data"}, str(model_file))

        class_names_file = tmp_path / "class_names.json"
        class_names_file.write_text(json.dumps(MOCK_CLASS_NAMES))

        # 创建临时 YOLO 模型文件
        yolo_file = tmp_path / "yolo11s.pt"
        yolo_file.write_bytes(b"fake_yolo_weights")

        output_dir = tmp_path / "output"

        result_path = package_model(
            model_path=str(model_file),
            class_names_path=str(class_names_file),
            output_dir=str(output_dir),
            s3_bucket=None,
            backbone_name="mock-backbone",
            yolo_model_path=str(yolo_file),
        )

        # 解压并验证
        extract_dir = tmp_path / "extracted"
        with tarfile.open(result_path, "r:gz") as tar:
            tar.extractall(path=str(extract_dir))

        # 验证根目录含 yolo11s.pt
        assert (extract_dir / "yolo11s.pt").exists()
        # 验证其他文件仍存在
        assert (extract_dir / "bird_classifier.pt").exists()
        assert (extract_dir / "class_names.json").exists()
        assert (extract_dir / "code" / "inference.py").exists()
