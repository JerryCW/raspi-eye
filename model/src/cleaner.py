"""数据清洗模块：pHash 去重 + 质量过滤 + letterbox resize。

提供 DataCleaner 类和模块级函数（deduplicate、filter_quality、letterbox_resize），
将 raw/ 目录下的原始图片清洗后保存到 cleaned/ 目录。
"""

import os
from dataclasses import dataclass
from pathlib import Path

import imagehash
from PIL import Image

from model.src.config import DatasetConfig


@dataclass
class CleanStats:
    """单物种清洗统计。"""

    species: str = ""
    input_count: int = 0
    after_dedup: int = 0
    removed_corrupt: int = 0
    removed_small: int = 0
    removed_lowvar: int = 0
    after_filter: int = 0
    output_count: int = 0


def deduplicate(image_paths: list[str], threshold: int) -> list[str]:
    """pHash 去重：计算所有图片的 pHash，移除 Hamming distance ≤ threshold 的重复图片。

    重复组中保留文件尺寸最大的一张。
    返回去重后的图片路径列表。
    """
    if not image_paths:
        return []

    # 计算每张图片的 pHash，跳过无法打开的图片
    hashes: list[tuple[str, imagehash.ImageHash]] = []
    for p in image_paths:
        try:
            img = Image.open(p)
            h = imagehash.phash(img)
            hashes.append((p, h))
        except Exception:
            # 无法打开的图片跳过（后续 filter_quality 会处理）
            hashes.append((p, None))

    # 标记需要移除的索引
    removed: set[int] = set()

    for i in range(len(hashes)):
        if i in removed:
            continue
        if hashes[i][1] is None:
            continue

        # 收集与 i 重复的所有图片（包括 i 自身）
        group = [i]
        for j in range(i + 1, len(hashes)):
            if j in removed:
                continue
            if hashes[j][1] is None:
                continue
            if hashes[i][1] - hashes[j][1] <= threshold:
                group.append(j)

        if len(group) > 1:
            # 保留文件尺寸最大的一张
            best = max(group, key=lambda idx: os.path.getsize(hashes[idx][0]))
            for idx in group:
                if idx != best:
                    removed.add(idx)

    return [hashes[i][0] for i in range(len(hashes)) if i not in removed]


def filter_quality(image_paths: list[str]) -> tuple[list[str], dict]:
    """质量过滤：
    - 无法打开 → 移除（corrupt）
    - 短边 < 800px → 移除（small）
    - 灰度图且标准差 < 10 → 移除（lowvar）

    返回 (通过的路径列表, 各原因移除计数字典)。
    """
    passed: list[str] = []
    counts = {"corrupt": 0, "small": 0, "lowvar": 0}

    for p in image_paths:
        # 检查是否可以打开
        try:
            img = Image.open(p)
            img.load()  # 强制加载像素数据，检测截断/损坏
        except Exception:
            counts["corrupt"] += 1
            continue

        # 检查短边尺寸
        w, h = img.size
        if min(w, h) < 800:
            counts["small"] += 1
            continue

        # 检查灰度低方差（纯色/损坏）
        # 仅对灰度图（mode == "L"）或实际为灰度的 RGB 图检查
        is_grayscale = img.mode == "L"
        if not is_grayscale and img.mode in ("RGB", "RGBA"):
            # 检查 RGB 图是否实际为灰度（R == G == B）
            # 采样检查以提高性能
            rgb_data = list(img.convert("RGB").getdata())
            if rgb_data:
                is_grayscale = all(r == g == b for r, g, b in rgb_data[:1000])

        if is_grayscale:
            gray = img.convert("L")
            pixels = list(gray.getdata())
            n = len(pixels)
            if n == 0:
                counts["lowvar"] += 1
                continue
            mean = sum(pixels) / n
            variance = sum((x - mean) ** 2 for x in pixels) / n
            std = variance ** 0.5
            if std < 10:
                counts["lowvar"] += 1
                continue

        passed.append(p)

    return passed, counts


def letterbox_resize(image: Image.Image, target_size: int) -> Image.Image:
    """Letterbox resize：等比缩放 + 黑色填充到 target_size × target_size。

    不变量：输出尺寸恒为 (target_size, target_size)。
    """
    w, h = image.size
    scale = target_size / max(w, h)
    new_w = int(w * scale)
    new_h = int(h * scale)

    # 确保缩放后尺寸 ≥ 1（极端宽高比时 int() 可能向下取整为 0）
    new_w = max(1, new_w)
    new_h = max(1, new_h)

    # 确保缩放后尺寸不超过 target_size（浮点精度保护）
    new_w = min(new_w, target_size)
    new_h = min(new_h, target_size)

    resized = image.resize((new_w, new_h), Image.LANCZOS)

    # 创建黑色背景并居中粘贴
    canvas = Image.new("RGB", (target_size, target_size), color=(0, 0, 0))
    paste_x = (target_size - new_w) // 2
    paste_y = (target_size - new_h) // 2
    canvas.paste(resized, (paste_x, paste_y))

    return canvas


class DataCleaner:
    """数据清洗器：去重 → 质量过滤 → letterbox resize → 保存。"""

    def __init__(
        self,
        config: DatasetConfig,
        raw_dir: str = "model/data/raw",
        cleaned_dir: str = "model/data/cleaned",
    ):
        self.config = config
        self.raw_dir = Path(raw_dir)
        self.cleaned_dir = Path(cleaned_dir)

    def deduplicate(self, image_paths: list[str], threshold: int) -> list[str]:
        """pHash 去重（委托给模块级函数）。"""
        return deduplicate(image_paths, threshold)

    def filter_quality(self, image_paths: list[str]) -> tuple[list[str], dict]:
        """质量过滤（委托给模块级函数）。"""
        return filter_quality(image_paths)

    def letterbox_resize(self, image: Image.Image, target_size: int) -> Image.Image:
        """Letterbox resize（委托给模块级函数）。"""
        return letterbox_resize(image, target_size)

    def clean_species(self, species_name: str) -> CleanStats:
        """清洗单个物种：去重 → 质量过滤 → resize → 保存到 cleaned/。"""
        stats = CleanStats(species=species_name)

        species_raw_dir = self.raw_dir / species_name
        if not species_raw_dir.is_dir():
            print(f"警告: 物种目录不存在: {species_raw_dir}")
            return stats

        # 收集所有图片路径
        image_paths = sorted([
            str(p)
            for p in species_raw_dir.iterdir()
            if p.suffix.lower() in (".jpg", ".jpeg", ".png", ".webp")
        ])
        stats.input_count = len(image_paths)

        if not image_paths:
            print(f"警告: {species_name} 无图片文件")
            return stats

        # 1. pHash 去重
        threshold = self.config.global_config.phash_threshold
        deduped = self.deduplicate(image_paths, threshold)
        stats.after_dedup = len(deduped)

        # 2. 质量过滤
        passed, counts = self.filter_quality(deduped)
        stats.removed_corrupt = counts.get("corrupt", 0)
        stats.removed_small = counts.get("small", 0)
        stats.removed_lowvar = counts.get("lowvar", 0)
        stats.after_filter = len(passed)

        # 3. Letterbox resize + 保存
        target_size = self.config.global_config.image_size
        species_cleaned_dir = self.cleaned_dir / species_name
        species_cleaned_dir.mkdir(parents=True, exist_ok=True)

        saved = 0
        for p in passed:
            try:
                img = Image.open(p)
                img = img.convert("RGB")
                resized = self.letterbox_resize(img, target_size)
                out_name = Path(p).stem + ".jpg"
                out_path = species_cleaned_dir / out_name
                resized.save(str(out_path), format="JPEG", quality=95)
                saved += 1
            except Exception as e:
                print(f"警告: resize/保存失败 {p}: {e}")

        stats.output_count = saved

        # 打印统计
        print(
            f"[{species_name}] 输入={stats.input_count} "
            f"去重后={stats.after_dedup} "
            f"过滤后={stats.after_filter} "
            f"(损坏={stats.removed_corrupt} 小图={stats.removed_small} 低方差={stats.removed_lowvar}) "
            f"最终={stats.output_count}"
        )

        if stats.output_count < 30:
            print(f"警告: {species_name} 清洗后图片数 {stats.output_count} < 30")

        return stats

    def clean_all(self) -> list[CleanStats]:
        """清洗所有物种，返回统计列表。"""
        results: list[CleanStats] = []
        for entry in self.config.species:
            stats = self.clean_species(entry.scientific_name)
            results.append(stats)

            if stats.output_count == 0:
                print(f"错误: {entry.scientific_name} 最终图片数为 0，跳过")

        return results
