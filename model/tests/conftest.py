"""pytest fixtures：内存测试图片生成、临时目录等。

所有 fixture 不依赖网络，使用 Pillow 在内存中生成测试图片。
"""

import sys
from pathlib import Path

# 将项目根目录加入 sys.path，使 model.src.* 可导入
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

import pytest
from PIL import Image


@pytest.fixture
def tmp_dir(tmp_path):
    """提供临时目录（基于 pytest 内置 tmp_path）。"""
    return tmp_path


@pytest.fixture
def sample_rgb_image():
    """生成内存中的 RGB 测试图片（128×128，红色填充）。"""
    return Image.new("RGB", (128, 128), color=(255, 0, 0))


@pytest.fixture
def sample_image_file(tmp_path, sample_rgb_image):
    """将测试图片保存到临时目录并返回路径。"""
    path = tmp_path / "test_image.jpg"
    sample_rgb_image.save(path, format="JPEG", quality=95)
    return path
