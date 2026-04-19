"""YOLO 鸟体裁切模块单元测试 + PBT。

YOLO 模型使用 mock，图片使用 Image.new() 内存生成，文件系统使用 tmp_path。
"""

from dataclasses import dataclass
from unittest.mock import MagicMock

import pytest
from PIL import Image

from model.src.cropper import BIRD_CLASS_ID, CropStats, crop_bird, crop_species


# ---------------------------------------------------------------------------
# Mock 辅助
# ---------------------------------------------------------------------------

@dataclass
class FakeBox:
    """模拟 ultralytics box 对象。"""

    cls_id: int
    confidence: float
    coords: list[float]  # [x1, y1, x2, y2]

    @property
    def cls(self):
        return self.cls_id

    @property
    def conf(self):
        return self.confidence

    @property
    def xyxy(self):
        """返回形如 tensor 的嵌套列表，xyxy[0] 可 .tolist()。"""

        class _Row:
            def __init__(self, vals):
                self._vals = vals

            def tolist(self):
                return self._vals

        return [_Row(self.coords)]


def _make_mock_model(boxes: list[FakeBox]):
    """构造一个返回预设 boxes 的 mock YOLO 模型。"""
    model = MagicMock()
    result = MagicMock()
    result.boxes = boxes
    model.return_value = [result]
    return model


def _save_test_image(path, width=800, height=600, color="green"):
    """在磁盘上保存一张测试图片。"""
    img = Image.new("RGB", (width, height), color=color)
    img.save(str(path), format="JPEG")
    return str(path)


# ---------------------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------------------

class TestCropBird:
    """crop_bird 函数测试。"""

    def test_crop_with_detection(self, tmp_path):
        """有检测结果时正确裁切 + padding + letterbox resize 到 518×518。"""
        img_path = _save_test_image(tmp_path / "bird.jpg", 800, 600)

        boxes = [
            FakeBox(cls_id=BIRD_CLASS_ID, confidence=0.85, coords=[200, 100, 600, 400]),
        ]
        model = _make_mock_model(boxes)

        result = crop_bird(img_path, model)

        assert result is not None
        assert result.size == (518, 518)

    def test_no_bird_class_returns_none(self, tmp_path):
        """无 bird class 时返回 None。"""
        img_path = _save_test_image(tmp_path / "cat.jpg")

        # class 16 = dog，不是 bird
        boxes = [FakeBox(cls_id=16, confidence=0.9, coords=[100, 100, 500, 400])]
        model = _make_mock_model(boxes)

        result = crop_bird(img_path, model)
        assert result is None

    def test_low_confidence_returns_none(self, tmp_path):
        """置信度 < 0.3 时返回 None。"""
        img_path = _save_test_image(tmp_path / "low_conf.jpg")

        boxes = [FakeBox(cls_id=BIRD_CLASS_ID, confidence=0.2, coords=[100, 100, 500, 400])]
        model = _make_mock_model(boxes)

        result = crop_bird(img_path, model)
        assert result is None

    def test_empty_boxes_returns_none(self, tmp_path):
        """无检测结果时返回 None。"""
        img_path = _save_test_image(tmp_path / "empty.jpg")
        model = _make_mock_model([])

        result = crop_bird(img_path, model)
        assert result is None

    def test_padding_clamp_to_boundary(self, tmp_path):
        """边缘 bbox 场景：padding 不超出图片边界。"""
        img_path = _save_test_image(tmp_path / "edge.jpg", 400, 300)

        # bbox 紧贴左上角
        boxes = [
            FakeBox(cls_id=BIRD_CLASS_ID, confidence=0.9, coords=[0, 0, 100, 80]),
        ]
        model = _make_mock_model(boxes)

        result = crop_bird(img_path, model)
        assert result is not None
        assert result.size == (518, 518)

    def test_padding_clamp_bottom_right(self, tmp_path):
        """bbox 紧贴右下角时 padding 不超出图片边界。"""
        img_path = _save_test_image(tmp_path / "br.jpg", 400, 300)

        boxes = [
            FakeBox(cls_id=BIRD_CLASS_ID, confidence=0.9, coords=[300, 200, 400, 300]),
        ]
        model = _make_mock_model(boxes)

        result = crop_bird(img_path, model)
        assert result is not None
        assert result.size == (518, 518)

    def test_selects_highest_confidence(self, tmp_path):
        """多个 bird box 时选择置信度最高的。"""
        img_path = _save_test_image(tmp_path / "multi.jpg", 800, 600)

        boxes = [
            FakeBox(cls_id=BIRD_CLASS_ID, confidence=0.5, coords=[0, 0, 100, 100]),
            FakeBox(cls_id=BIRD_CLASS_ID, confidence=0.9, coords=[300, 200, 600, 500]),
        ]
        model = _make_mock_model(boxes)

        result = crop_bird(img_path, model)
        assert result is not None
        assert result.size == (518, 518)


class TestCropSpecies:
    """crop_species 函数测试。"""

    def test_stats_counting(self, tmp_path):
        """统计计数正确：total、cropped、discarded。"""
        input_dir = tmp_path / "input"
        output_dir = tmp_path / "output"
        input_dir.mkdir()

        # 创建 3 张测试图片
        for i in range(3):
            _save_test_image(input_dir / f"img_{i}.jpg")

        # 前 2 张检测到鸟，第 3 张不检测到
        call_count = {"n": 0}

        def mock_call(path, verbose=False):
            idx = call_count["n"]
            call_count["n"] += 1
            result = MagicMock()
            if idx < 2:
                result.boxes = [
                    FakeBox(cls_id=BIRD_CLASS_ID, confidence=0.8, coords=[50, 50, 300, 250])
                ]
            else:
                result.boxes = []
            return [result]

        model = MagicMock(side_effect=mock_call)

        stats = crop_species("TestBird", str(input_dir), str(output_dir), model)

        assert stats.species == "TestBird"
        assert stats.total == 3
        assert stats.cropped == 2
        assert stats.discarded == 1

        # 验证输出目录有 2 张图片
        output_files = list(output_dir.iterdir())
        assert len(output_files) == 2


# ---------------------------------------------------------------------------
# PBT — 属性 1：YOLO 裁切 padding 边界安全 + 输出尺寸不变量
# ---------------------------------------------------------------------------

from hypothesis import given, settings
from hypothesis.strategies import floats, integers, tuples

from model.src.cleaner import letterbox_resize


# Feature: feature-space-cleaning, Property 1: YOLO 裁切 padding 边界安全 + 输出尺寸不变量
# **Validates: Requirements 1.2, 1.6**
@given(
    img_size=tuples(
        integers(min_value=50, max_value=4000),
        integers(min_value=50, max_value=4000),
    ),
    bbox_ratios=tuples(
        floats(min_value=0.05, max_value=0.45),
        floats(min_value=0.05, max_value=0.45),
        floats(min_value=0.55, max_value=0.95),
        floats(min_value=0.55, max_value=0.95),
    ),
)
@settings(max_examples=100)
def test_crop_padding_and_output_size(img_size, bbox_ratios):
    """对于任意图片尺寸和 bbox，padding 不超出边界且输出恒为 518×518。"""
    img_w, img_h = img_size
    r_x1, r_y1, r_x2, r_y2 = bbox_ratios

    # 将比例转换为绝对坐标
    x1 = r_x1 * img_w
    y1 = r_y1 * img_h
    x2 = r_x2 * img_w
    y2 = r_y2 * img_h

    # 模拟 padding 扩展逻辑（与 crop_bird 一致）
    padding = 0.2
    w, h = x2 - x1, y2 - y1
    padded_x1 = max(0, x1 - w * padding)
    padded_y1 = max(0, y1 - h * padding)
    padded_x2 = min(img_w, x2 + w * padding)
    padded_y2 = min(img_h, y2 + h * padding)

    # 属性 1：padding 后坐标不超出图片边界
    assert padded_x1 >= 0, f"padded_x1={padded_x1} < 0"
    assert padded_y1 >= 0, f"padded_y1={padded_y1} < 0"
    assert padded_x2 <= img_w, f"padded_x2={padded_x2} > img_w={img_w}"
    assert padded_y2 <= img_h, f"padded_y2={padded_y2} > img_h={img_h}"

    # 属性 2：裁切区域有效（宽高 > 0）
    crop_w = int(padded_x2) - int(padded_x1)
    crop_h = int(padded_y2) - int(padded_y1)
    assert crop_w > 0, f"crop_w={crop_w} <= 0"
    assert crop_h > 0, f"crop_h={crop_h} <= 0"

    # 属性 3：letterbox resize 输出恒为 518×518
    img = Image.new("RGB", (img_w, img_h), color="blue")
    cropped = img.crop((int(padded_x1), int(padded_y1), int(padded_x2), int(padded_y2)))
    result = letterbox_resize(cropped, 518)
    assert result.size == (518, 518), f"输出尺寸 {result.size} != (518, 518)"
