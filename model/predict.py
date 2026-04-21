#!/usr/bin/env python3
"""离线推理验证脚本：用 Pi 5 实拍照片验证模型分类效果。

用法示例：
    python model/predict.py --model bird_classifier.pt --images /path/to/photos/
    python model/predict.py --model bird_classifier.pt --images photo.jpg --top-k 3
    python model/predict.py --model bird_classifier.pt --images test/ --output results.json
"""

import argparse
import json
import sys
from pathlib import Path

import torch
import torch.nn.functional as F
from PIL import Image

# 兼容本地开发和 SageMaker 容器两种 import 路径
_this_dir = Path(__file__).resolve().parent
sys.path.insert(0, str(_this_dir.parent))
sys.path.insert(0, str(_this_dir))

try:
    from model.training.augmentation import get_val_transform
    from model.training.backbone_registry import BackboneConfig, get_backbone
    from model.training.classifier import BirdClassifier
except ModuleNotFoundError:
    from training.augmentation import get_val_transform
    from training.backbone_registry import BackboneConfig, get_backbone
    from training.classifier import BirdClassifier


# 支持的图片扩展名
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".tiff", ".webp"}


def load_model(model_path: str) -> tuple[BirdClassifier, dict, torch.device]:
    """加载 .pt 模型文件，从元数据恢复 BirdClassifier。

    Args:
        model_path: .pt 文件路径

    Returns:
        (model, metadata, device) 三元组

    Raises:
        FileNotFoundError: 模型文件不存在
        ValueError: 模型格式错误（缺少 metadata）
    """
    path = Path(model_path)
    if not path.exists():
        raise FileNotFoundError(f"模型文件不存在: {model_path}")

    # 自动检测设备
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"[推理] 使用设备: {device}")

    # 加载模型数据
    save_data = torch.load(str(path), map_location=device, weights_only=False)

    if "metadata" not in save_data or "state_dict" not in save_data:
        raise ValueError(
            f"模型格式错误: 缺少 metadata 或 state_dict。"
            f"请确认文件是由 exporter.py 导出的 .pt 文件。"
        )

    metadata = save_data["metadata"]
    backbone_name = metadata["backbone_name"]
    input_size = metadata["input_size"]
    feature_dim = metadata["feature_dim"]
    num_classes = metadata["num_classes"]
    class_names = metadata["class_names"]

    print(f"[推理] backbone: {backbone_name}")
    print(f"[推理] input_size: {input_size}")
    print(f"[推理] 类别数: {num_classes}")

    # 尝试从 Registry 获取 backbone 配置；如果不在 Registry 中，
    # 仅使用元数据中的 input_size 构建（推理时已有完整 state_dict）
    try:
        backbone_config = get_backbone(backbone_name)
    except ValueError:
        print(f"[推理] 警告: backbone '{backbone_name}' 不在 Registry 中，使用元数据构建模型")
        # 构建一个最小 BackboneConfig，仅用于模型重建
        backbone_config = BackboneConfig(
            name=backbone_name,
            load_fn=lambda: _build_dummy_backbone(feature_dim),
            extract_fn=lambda model, x: model(x),
            input_size=input_size,
            feature_dim=feature_dim,
            needs_hf_token=False,
        )

    # 构建模型并加载权重
    model = BirdClassifier(num_classes=num_classes, backbone_config=backbone_config)
    model.load_state_dict(save_data["state_dict"])
    model.to(device)
    model.eval()

    return model, metadata, device


def _build_dummy_backbone(feature_dim: int) -> torch.nn.Module:
    """构建占位 backbone（仅在 backbone 不在 Registry 时使用）。"""
    import torch.nn as nn

    class DummyBackbone(nn.Module):
        def __init__(self):
            super().__init__()
            self.proj = nn.Linear(feature_dim, feature_dim)

        def forward(self, x):
            return x

    return DummyBackbone()


def collect_images(images_path: str) -> list[Path]:
    """收集图片路径列表。

    Args:
        images_path: 图片目录或单张图片路径

    Returns:
        图片路径列表
    """
    path = Path(images_path)
    if path.is_file():
        if path.suffix.lower() in IMAGE_EXTENSIONS:
            return [path]
        else:
            print(f"[推理] 警告: 不支持的文件格式: {path.suffix}")
            return []

    if path.is_dir():
        images = []
        for ext in IMAGE_EXTENSIONS:
            images.extend(path.rglob(f"*{ext}"))
            images.extend(path.rglob(f"*{ext.upper()}"))
        # 去重并排序
        images = sorted(set(images))
        return images

    print(f"[推理] 警告: 路径不存在: {images_path}")
    return []


def detect_species_dirs(images_path: str) -> dict[str, list[Path]] | None:
    """检测图片目录是否包含物种子目录结构。

    如果目录结构为 {images_path}/{species}/*.jpg，返回物种到图片的映射。
    否则返回 None。

    Args:
        images_path: 图片目录路径

    Returns:
        物种名 → 图片路径列表的映射，或 None
    """
    path = Path(images_path)
    if not path.is_dir():
        return None

    species_map: dict[str, list[Path]] = {}
    for subdir in sorted(path.iterdir()):
        if not subdir.is_dir():
            continue
        # 收集子目录中的图片
        imgs = []
        for ext in IMAGE_EXTENSIONS:
            imgs.extend(subdir.glob(f"*{ext}"))
            imgs.extend(subdir.glob(f"*{ext.upper()}"))
        if imgs:
            species_map[subdir.name] = sorted(set(imgs))

    # 至少有 2 个物种子目录才认为是分类目录结构
    if len(species_map) >= 2:
        return species_map
    return None


def predict_single(
    model: BirdClassifier,
    image_path: Path,
    transform,
    class_names: list[str],
    device: torch.device,
    top_k: int,
) -> list[dict[str, object]] | None:
    """对单张图片执行推理。

    Args:
        model: BirdClassifier 模型
        image_path: 图片路径
        transform: 验证集预处理变换
        class_names: 类别名列表
        device: 推理设备
        top_k: 输出 top-k 预测

    Returns:
        top-k 预测列表 [{species, confidence}, ...]，图片损坏时返回 None
    """
    try:
        img = Image.open(image_path).convert("RGB")
    except Exception as e:
        print(f"[推理] 警告: 无法打开图片 {image_path}: {e}")
        return None

    # 预处理 → 模型推理 → softmax → top-k
    input_tensor = transform(img).unsqueeze(0).to(device)

    with torch.no_grad():
        logits = model(input_tensor)
        probs = F.softmax(logits, dim=1)

    # 取 top-k
    k = min(top_k, len(class_names))
    top_probs, top_indices = torch.topk(probs[0], k)

    results = []
    for prob, idx in zip(top_probs.cpu().tolist(), top_indices.cpu().tolist()):
        results.append({
            "species": class_names[idx],
            "confidence": round(prob, 6),
        })

    return results


def run_prediction(
    model: BirdClassifier,
    metadata: dict,
    device: torch.device,
    images_path: str,
    top_k: int,
    output_path: str | None,
) -> None:
    """执行批量推理。

    Args:
        model: BirdClassifier 模型
        metadata: 模型元数据
        device: 推理设备
        images_path: 图片目录或单张图片路径
        top_k: 输出 top-k 预测
        output_path: 可选 JSON 输出路径
    """
    input_size = metadata["input_size"]
    class_names = metadata["class_names"]
    transform = get_val_transform(input_size)

    # 收集图片
    images = collect_images(images_path)
    if not images:
        print("[推理] 未找到任何图片")
        return

    print(f"[推理] 共找到 {len(images)} 张图片")
    print()

    # 逐张推理
    all_results: dict[str, list[dict]] = {}
    skipped = 0

    for img_path in images:
        preds = predict_single(model, img_path, transform, class_names, device, top_k)
        if preds is None:
            skipped += 1
            continue

        # 输出结果
        filename = str(img_path)
        all_results[filename] = preds

        print(f"  {img_path.name}:")
        for p in preds:
            print(f"    {p['species']:40s}  {p['confidence']:.4f}")
        print()

    # 统计
    print(f"[推理] 完成: {len(all_results)} 张成功, {skipped} 张跳过")

    # 检测物种子目录结构，自动计算 per-class accuracy
    species_dirs = detect_species_dirs(images_path)
    if species_dirs is not None:
        _compute_accuracy(all_results, species_dirs, class_names)

    # 保存 JSON 结果
    if output_path:
        _save_results(all_results, output_path)


def _compute_accuracy(
    all_results: dict[str, list[dict]],
    species_dirs: dict[str, list[Path]],
    class_names: list[str],
) -> None:
    """根据物种子目录结构计算 per-class accuracy。

    Args:
        all_results: 推理结果 {filename: [{species, confidence}, ...]}
        species_dirs: 物种名 → 图片路径列表
        class_names: 模型类别名列表
    """
    print("=" * 60)
    print("[推理] Per-class Accuracy（基于目录名匹配）")
    print("=" * 60)

    total_correct = 0
    total_count = 0
    per_class_stats: dict[str, dict] = {}

    for species_name, img_paths in sorted(species_dirs.items()):
        correct = 0
        count = 0
        for img_path in img_paths:
            filename = str(img_path)
            if filename not in all_results:
                continue
            count += 1
            # top-1 预测
            top1_species = all_results[filename][0]["species"]
            if top1_species == species_name:
                correct += 1

        accuracy = correct / count if count > 0 else 0.0
        per_class_stats[species_name] = {
            "accuracy": round(accuracy, 4),
            "correct": correct,
            "count": count,
        }
        total_correct += correct
        total_count += count

        print(f"  {species_name:40s}  {correct:3d}/{count:3d}  ({accuracy:.2%})")

    overall = total_correct / total_count if total_count > 0 else 0.0
    print("-" * 60)
    print(f"  {'Overall':40s}  {total_correct:3d}/{total_count:3d}  ({overall:.2%})")
    print()


def _save_results(all_results: dict[str, list[dict]], output_path: str) -> None:
    """保存推理结果为 JSON 文件。

    Args:
        all_results: 推理结果 {filename: [{species, confidence}, ...]}
        output_path: JSON 输出路径
    """
    out = Path(output_path)
    out.parent.mkdir(parents=True, exist_ok=True)

    with open(out, "w", encoding="utf-8") as f:
        json.dump(all_results, f, ensure_ascii=False, indent=2)

    print(f"[推理] 结果已保存: {out}")


def parse_args() -> argparse.Namespace:
    """解析命令行参数。"""
    parser = argparse.ArgumentParser(
        description="离线推理验证脚本：用实拍照片验证模型分类效果"
    )
    parser.add_argument(
        "--model",
        required=True,
        help="模型文件路径（.pt 文件）",
    )
    parser.add_argument(
        "--images",
        required=True,
        help="图片目录或单张图片路径",
    )
    parser.add_argument(
        "--top-k",
        type=int,
        default=5,
        help="输出 top-k 预测（默认 5）",
    )
    parser.add_argument(
        "--output",
        default=None,
        help="可选：JSON 输出路径",
    )
    return parser.parse_args()


def main() -> None:
    """主入口。"""
    args = parse_args()

    # 加载模型
    model, metadata, device = load_model(args.model)

    # 执行推理
    run_prediction(
        model=model,
        metadata=metadata,
        device=device,
        images_path=args.images,
        top_k=args.top_k,
        output_path=args.output,
    )


if __name__ == "__main__":
    main()
