"""清洗逻辑单元测试 + PBT 属性测试。

覆盖：
- pHash 去重（deduplicate）
- 质量过滤（filter_quality）
- Letterbox resize（letterbox_resize）
- PBT 属性 1：去重后不变量
- PBT 属性 2：质量过滤尺寸规则
- PBT 属性 3：Letterbox resize 输出不变量

所有测试使用 Pillow Image.new() 在内存中生成测试图片，不依赖网络。
"""

import os
from itertools import combinations

import imagehash
import pytest
from PIL import Image, ImageDraw

from model.src.cleaner import deduplicate, filter_quality, letterbox_resize


# ============================================================
# 辅助函数
# ============================================================

def _save_image(tmp_path, filename, size=(1000, 1000), color=(255, 0, 0), fmt="JPEG"):
    """生成纯色图片并保存到 tmp_path，返回路径字符串。"""
    img = Image.new("RGB", size, color=color)
    path = tmp_path / filename
    img.save(str(path), format=fmt, quality=95)
    return str(path)


def _save_patterned_image(tmp_path, filename, size=(1000, 1000), seed_color=(255, 0, 0)):
    """生成带图案的图片（非纯色），用于通过灰度低方差检查。"""
    img = Image.new("RGB", size, color=seed_color)
    draw = ImageDraw.Draw(img)
    # 画一些不同颜色的矩形，确保方差足够大
    w, h = size
    draw.rectangle([0, 0, w // 3, h // 3], fill=(0, 0, 255))
    draw.rectangle([w // 3, h // 3, 2 * w // 3, 2 * h // 3], fill=(0, 255, 0))
    draw.ellipse([w // 4, h // 4, 3 * w // 4, 3 * h // 4], fill=(255, 255, 0))
    path = tmp_path / filename
    img.save(str(path), format="JPEG", quality=95)
    return str(path)


# ============================================================
# 1. pHash 去重测试
# ============================================================

class TestDeduplicate:
    """pHash 去重逻辑测试。"""

    def test_identical_images_deduped(self, tmp_path):
        """相同图片（同一内容保存两次）应去重为一张。"""
        p1 = _save_image(tmp_path, "a.jpg", color=(100, 100, 100))
        p2 = _save_image(tmp_path, "b.jpg", color=(100, 100, 100))
        result = deduplicate([p1, p2], threshold=8)
        assert len(result) == 1

    def test_different_images_kept(self, tmp_path):
        """视觉差异大的图片应全部保留。"""
        p1 = _save_patterned_image(tmp_path, "red.jpg", seed_color=(255, 0, 0))
        p2 = _save_patterned_image(tmp_path, "blue.jpg", seed_color=(0, 0, 255))
        # 确认两张图片 pHash 差异确实大于阈值
        h1 = imagehash.phash(Image.open(p1))
        h2 = imagehash.phash(Image.open(p2))
        assert h1 - h2 > 8, f"测试前提不成立：两张图片 pHash 距离 {h1 - h2} <= 8"
        result = deduplicate([p1, p2], threshold=8)
        assert len(result) == 2

    def test_threshold_boundary(self, tmp_path):
        """阈值为 0 时，只有完全相同 pHash 的图片才被去重。"""
        p1 = _save_image(tmp_path, "a.jpg", color=(128, 128, 128))
        p2 = _save_image(tmp_path, "b.jpg", color=(128, 128, 128))
        # threshold=0 → 只有 Hamming distance == 0 才去重
        result = deduplicate([p1, p2], threshold=0)
        assert len(result) == 1

    def test_empty_list(self, tmp_path):
        """空列表返回空列表。"""
        result = deduplicate([], threshold=8)
        assert result == []

    def test_single_image(self, tmp_path):
        """单张图片直接返回。"""
        p = _save_image(tmp_path, "only.jpg")
        result = deduplicate([p], threshold=8)
        assert len(result) == 1
        assert result[0] == p

    def test_keeps_largest_file(self, tmp_path):
        """重复组中保留文件尺寸最大的一张。"""
        # 小图（低质量 JPEG）
        img = Image.new("RGB", (200, 200), color=(50, 50, 50))
        p_small = tmp_path / "small.jpg"
        img.save(str(p_small), format="JPEG", quality=10)
        # 大图（高质量 JPEG，同内容）
        p_large = tmp_path / "large.jpg"
        img.save(str(p_large), format="JPEG", quality=95)

        assert os.path.getsize(str(p_large)) > os.path.getsize(str(p_small))
        result = deduplicate([str(p_small), str(p_large)], threshold=8)
        assert len(result) == 1
        assert result[0] == str(p_large)


# ============================================================
# 2. 质量过滤测试
# ============================================================

class TestFilterQuality:
    """质量过滤逻辑测试。"""

    def test_corrupt_image_removed(self, tmp_path):
        """损坏图片（无法打开）被移除。"""
        bad = tmp_path / "corrupt.jpg"
        bad.write_bytes(b"not a real image file content")
        passed, counts = filter_quality([str(bad)])
        assert len(passed) == 0
        assert counts["corrupt"] == 1

    def test_small_image_removed(self, tmp_path):
        """短边 < 800 的图片被移除。"""
        p = _save_patterned_image(tmp_path, "tiny.jpg", size=(400, 1200))
        passed, counts = filter_quality([p])
        assert len(passed) == 0
        assert counts["small"] == 1

    def test_grayscale_lowvar_removed(self, tmp_path):
        """灰度低方差图片被移除。"""
        # 创建纯灰色图片（方差 = 0）
        img = Image.new("L", (1000, 1000), color=128)
        p = tmp_path / "gray_flat.jpg"
        img.save(str(p), format="JPEG", quality=95)
        passed, counts = filter_quality([str(p)])
        assert len(passed) == 0
        assert counts["lowvar"] == 1

    def test_normal_image_kept(self, tmp_path):
        """正常图片（短边 ≥ 800、非损坏、非纯色）被保留。"""
        p = _save_patterned_image(tmp_path, "good.jpg", size=(1000, 1200))
        passed, counts = filter_quality([p])
        assert len(passed) == 1
        assert passed[0] == p

    def test_exact_boundary_800_kept(self, tmp_path):
        """短边恰好 800 的图片被保留。"""
        p = _save_patterned_image(tmp_path, "boundary.jpg", size=(800, 1200))
        passed, counts = filter_quality([p])
        assert len(passed) == 1

    def test_exact_boundary_799_removed(self, tmp_path):
        """短边 799 的图片被移除。"""
        p = _save_patterned_image(tmp_path, "below.jpg", size=(799, 1200))
        passed, counts = filter_quality([p])
        assert len(passed) == 0
        assert counts["small"] == 1

    def test_empty_list(self, tmp_path):
        """空列表返回空结果。"""
        passed, counts = filter_quality([])
        assert passed == []
        assert counts == {"corrupt": 0, "small": 0, "lowvar": 0}


# ============================================================
# 3. Letterbox resize 测试
# ============================================================

class TestLetterboxResize:
    """Letterbox resize 逻辑测试。"""

    def test_square_input(self):
        """正方形输入 → 输出为目标尺寸正方形。"""
        img = Image.new("RGB", (500, 500), color="red")
        result = letterbox_resize(img, 256)
        assert result.size == (256, 256)

    def test_landscape_input(self):
        """横向矩形输入 → 输出为目标尺寸正方形。"""
        img = Image.new("RGB", (1000, 500), color="green")
        result = letterbox_resize(img, 518)
        assert result.size == (518, 518)

    def test_portrait_input(self):
        """纵向矩形输入 → 输出为目标尺寸正方形。"""
        img = Image.new("RGB", (500, 1000), color="blue")
        result = letterbox_resize(img, 518)
        assert result.size == (518, 518)

    def test_very_small_input(self):
        """极小输入（1×1）→ 输出仍为目标尺寸。"""
        img = Image.new("RGB", (1, 1), color="white")
        result = letterbox_resize(img, 100)
        assert result.size == (100, 100)

    def test_output_is_rgb(self):
        """输出图片模式为 RGB。"""
        img = Image.new("RGB", (300, 600), color="yellow")
        result = letterbox_resize(img, 256)
        assert result.mode == "RGB"



# ============================================================
# PBT 属性测试
# ============================================================

from hypothesis import given, settings, assume, HealthCheck
from hypothesis.strategies import (
    integers,
    tuples,
    lists,
    composite,
)


# Feature: inat-data-collection, Property 1: 去重后不变量
# **Validates: Requirements 3.5**

@composite
def distinct_color_images(draw, min_count=2, max_count=8):
    """Hypothesis 策略：生成一组视觉不同的 RGB 图片参数（颜色 + 尺寸）。

    返回 list of (width, height, r, g, b) 元组。
    """
    count = draw(integers(min_value=min_count, max_value=max_count))
    images = []
    for i in range(count):
        w = draw(integers(min_value=64, max_value=512))
        h = draw(integers(min_value=64, max_value=512))
        # 使用差异较大的颜色通道值，确保 pHash 差异足够大
        r = draw(integers(min_value=0, max_value=255))
        g = draw(integers(min_value=0, max_value=255))
        b = draw(integers(min_value=0, max_value=255))
        images.append((w, h, r, g, b))
    return images


@given(image_specs=distinct_color_images())
@settings(max_examples=100, suppress_health_check=[HealthCheck.too_slow])
def test_deduplicate_invariant(image_specs, tmp_path_factory):
    """去重后不变量：结果集中任意两张图片的 pHash Hamming distance > threshold。

    生成随机图片集合，经过 deduplicate() 后，验证结果集中
    同一物种内任意两张图片的 pHash Hamming distance > threshold。
    """
    threshold = 8
    tmp_path = tmp_path_factory.mktemp("dedup")

    # 生成图片文件并保存到磁盘
    paths = []
    for i, (w, h, r, g, b) in enumerate(image_specs):
        img = Image.new("RGB", (w, h), color=(r, g, b))
        # 在图片上画不同图案增加视觉差异
        draw = ImageDraw.Draw(img)
        draw.rectangle([0, 0, w // 2, h // 2], fill=((r + 100) % 256, g, b))
        draw.ellipse([w // 4, h // 4, 3 * w // 4, 3 * h // 4], fill=(r, (g + 100) % 256, b))
        p = tmp_path / f"img_{i}.jpg"
        img.save(str(p), format="JPEG", quality=95)
        paths.append(str(p))

    # 执行去重
    result = deduplicate(paths, threshold=threshold)

    # 验证不变量：结果集中任意两张图片的 pHash 距离 > threshold
    hashes = []
    for p in result:
        h = imagehash.phash(Image.open(p))
        hashes.append(h)

    for i, j in combinations(range(len(hashes)), 2):
        dist = hashes[i] - hashes[j]
        assert dist > threshold, (
            f"去重后不变量违反：图片 {i} 和 {j} 的 pHash 距离 {dist} <= {threshold}"
        )


# Feature: inat-data-collection, Property 2: 质量过滤尺寸规则
# **Validates: Requirements 4.2**

@given(
    size=tuples(
        integers(min_value=1, max_value=2000),
        integers(min_value=1, max_value=2000),
    )
)
@settings(max_examples=100, suppress_health_check=[HealthCheck.too_slow])
def test_filter_quality_size_rule(size, tmp_path_factory):
    """质量过滤尺寸规则：短边 < 800 被移除，短边 ≥ 800 的有效图片被保留。

    生成随机尺寸的有效 RGB 图片，经过 filter_quality() 后，
    验证短边 < 800 的图片被移除，短边 ≥ 800 的非损坏/非纯色图片被保留。
    """
    w, h = size
    short_side = min(w, h)
    tmp_path = tmp_path_factory.mktemp("quality")

    # 生成带图案的图片（非纯色，确保不被 lowvar 过滤）
    img = Image.new("RGB", (w, h), color=(200, 50, 50))
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, 0, w // 2, h // 2], fill=(50, 200, 50))
    draw.ellipse([0, 0, w, h], fill=(50, 50, 200))

    p = tmp_path / "test.jpg"
    img.save(str(p), format="JPEG", quality=95)

    passed, counts = filter_quality([str(p)])

    if short_side < 800:
        assert len(passed) == 0, f"短边 {short_side} < 800 的图片应被移除"
        assert counts["small"] == 1
    else:
        assert len(passed) == 1, f"短边 {short_side} >= 800 的有效图片应被保留"
        assert passed[0] == str(p)


# Feature: inat-data-collection, Property 3: Letterbox resize 输出不变量
# **Validates: Requirements 5.1, 5.2, 5.4**

@given(
    size=tuples(
        integers(min_value=1, max_value=4000),
        integers(min_value=1, max_value=4000),
    ),
    target=integers(min_value=32, max_value=1024),
)
@settings(max_examples=100)
def test_letterbox_resize_output_size(size, target):
    """Letterbox resize 输出不变量：对于任意尺寸输入，输出恒为 target × target。"""
    img = Image.new("RGB", size, color="red")
    result = letterbox_resize(img, target)
    assert result.size == (target, target), (
        f"输入 {size}，target={target}，输出 {result.size} != ({target}, {target})"
    )
