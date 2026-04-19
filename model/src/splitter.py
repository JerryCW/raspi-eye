"""Train/Val 分层随机划分模块。

对每个物种独立执行 80/20 分层随机划分，输出 ImageFolder 格式目录。
"""

import shutil
from dataclasses import dataclass
from pathlib import Path

from sklearn.model_selection import train_test_split


@dataclass
class SplitStats:
    """划分统计。"""

    species: str = ""
    total: int = 0
    train_count: int = 0
    val_count: int = 0


def split_dataset(
    image_paths: list[str],
    test_size: float = 0.2,
    random_state: int = 42,
) -> tuple[list[str], list[str]]:
    """分层随机划分 train/val。

    图片数 < 5 时全部放入 train，val 为空。

    Args:
        image_paths: 图片路径列表
        test_size: 验证集比例（默认 0.2）
        random_state: 随机种子（默认 42）

    Returns:
        (train 路径列表, val 路径列表)
    """
    if len(image_paths) < 5:
        return list(image_paths), []

    train, val = train_test_split(
        image_paths, test_size=test_size, random_state=random_state
    )
    return list(train), list(val)


def split_and_copy(
    species_name: str,
    image_paths: list[str],
    train_dir: str,
    val_dir: str,
    random_state: int = 42,
) -> SplitStats:
    """划分并复制图片到 ImageFolder 目录。

    Args:
        species_name: 物种名称（用作子目录名）
        image_paths: 图片路径列表
        train_dir: 训练集根目录
        val_dir: 验证集根目录
        random_state: 随机种子

    Returns:
        SplitStats 统计信息
    """
    train_paths, val_paths = split_dataset(
        image_paths, test_size=0.2, random_state=random_state
    )

    # 创建 ImageFolder 子目录
    train_species_dir = Path(train_dir) / species_name
    val_species_dir = Path(val_dir) / species_name
    train_species_dir.mkdir(parents=True, exist_ok=True)
    val_species_dir.mkdir(parents=True, exist_ok=True)

    # 复制文件
    for p in train_paths:
        shutil.copy2(p, train_species_dir / Path(p).name)

    for p in val_paths:
        shutil.copy2(p, val_species_dir / Path(p).name)

    total = len(image_paths)
    if total < 5:
        print(f"警告: {species_name} 图片数 {total} < 5，全部放入 train，val 为空")

    return SplitStats(
        species=species_name,
        total=total,
        train_count=len(train_paths),
        val_count=len(val_paths),
    )
