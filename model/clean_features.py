"""端到端特征空间清洗管道。

用法：
    python model/clean_features.py --config model/config/species.yaml
    python model/clean_features.py --config model/config/species.yaml --skip-crop
    python model/clean_features.py --config model/config/species.yaml --skip-extract
    python model/clean_features.py --config model/config/species.yaml --species "Passer montanus"

SageMaker Processing Job 中自动检测 /opt/ml/processing/ 路径。
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

import numpy as np


def detect_paths() -> dict:
    """自动检测运行环境，返回输入/输出路径字典。

    SageMaker 模式：/opt/ml/processing/ 存在时使用 SageMaker 路径。
    本地模式：使用 model/data/ 下的相对路径。
    """
    sagemaker_base = "/opt/ml/processing"
    if os.path.isdir(sagemaker_base):
        return {
            "cleaned_dir": f"{sagemaker_base}/input/cleaned",
            "config_path": f"{sagemaker_base}/input/config/species.yaml",
            "cropped_dir": f"{sagemaker_base}/output/cropped",
            "features_dir": f"{sagemaker_base}/output/features",
            "train_dir": f"{sagemaker_base}/output/train",
            "val_dir": f"{sagemaker_base}/output/val",
            "report_dir": f"{sagemaker_base}/output/report",
        }
    return {
        "cleaned_dir": "model/data/cleaned",
        "config_path": None,
        "cropped_dir": "model/data/cropped",
        "features_dir": "model/data/features",
        "train_dir": "model/data/train",
        "val_dir": "model/data/val",
        "report_dir": "model/data/report",
    }


def main():
    parser = argparse.ArgumentParser(description="DINOv3 特征空间深度清洗")
    parser.add_argument("--config", required=True, help="物种配置文件路径")
    parser.add_argument("--skip-crop", action="store_true", help="跳过 YOLO 裁切")
    parser.add_argument("--skip-extract", action="store_true", help="跳过特征提取（复用 .npy）")
    parser.add_argument("--species", type=str, default=None, help="指定单个物种（scientific_name）")
    parser.add_argument("--dinov3-repo", type=str, default="third_party/dinov3",
                        help="DINOv3 本地 clone 路径")
    parser.add_argument("--dinov3-weights", type=str, default=None,
                        help="DINOv3 权重文件路径")
    parser.add_argument("--batch-size", type=int, default=32, help="特征提取 batch size")
    parser.add_argument("--cosine-threshold", type=float, default=0.95,
                        help="语义去重余弦相似度阈值")
    args = parser.parse_args()

    start_time = time.time()

    # ---- 1. 检测路径 + 加载配置 ----
    paths = detect_paths()
    config_path = paths["config_path"] or args.config

    from model.src.config import load_config
    config = load_config(config_path)

    # 筛选物种
    species_list = config.species
    if args.species:
        species_list = [s for s in species_list if s.scientific_name == args.species]
        if not species_list:
            print(f"错误: 未找到物种 '{args.species}'")
            sys.exit(1)

    cleaned_dir = paths["cleaned_dir"]
    cropped_dir = paths["cropped_dir"]
    features_dir = paths["features_dir"]
    train_dir = paths["train_dir"]
    val_dir = paths["val_dir"]
    report_dir = paths["report_dir"]

    report: dict = {
        "total_species": len(species_list),
        "total_input": 0,
        "total_output": 0,
        "total_train": 0,
        "total_val": 0,
        "elapsed_seconds": 0,
        "per_species": {},
    }

    # ---- 2. YOLO 裁切（除非 --skip-crop）----
    if not args.skip_crop:
        print("=" * 60)
        print("阶段 1: YOLO 鸟体裁切")
        print("=" * 60)
        from ultralytics import YOLO
        from model.src.cropper import crop_species

        yolo_model = YOLO("yolo11x.pt")
        for entry in species_list:
            species_input = os.path.join(cleaned_dir, entry.scientific_name)
            species_output = os.path.join(cropped_dir, entry.scientific_name)
            crop_stats = crop_species(
                entry.scientific_name, species_input, species_output, yolo_model
            )
            report["per_species"].setdefault(entry.scientific_name, {})
            report["per_species"][entry.scientific_name]["input"] = crop_stats.total
            report["per_species"][entry.scientific_name]["after_crop"] = crop_stats.cropped
            report["per_species"][entry.scientific_name]["crop_discarded"] = crop_stats.discarded
    else:
        print("跳过 YOLO 裁切（--skip-crop）")
        # 使用 cleaned 目录作为裁切后目录
        cropped_dir = cleaned_dir
        for entry in species_list:
            species_input = os.path.join(cleaned_dir, entry.scientific_name)
            n_images = len(list(Path(species_input).glob("*.jpg"))) if os.path.isdir(species_input) else 0
            report["per_species"].setdefault(entry.scientific_name, {})
            report["per_species"][entry.scientific_name]["input"] = n_images
            report["per_species"][entry.scientific_name]["after_crop"] = n_images
            report["per_species"][entry.scientific_name]["crop_discarded"] = 0

    # ---- 3. DINOv3 特征提取（除非 --skip-extract）----
    if not args.skip_extract:
        print("=" * 60)
        print("阶段 2: DINOv3 特征提取")
        print("=" * 60)
        from model.src.feature_extractor import FeatureExtractor

        extractor = FeatureExtractor(
            repo_dir=args.dinov3_repo,
            weights_path=args.dinov3_weights or "",
            batch_size=args.batch_size,
        )
        for entry in species_list:
            species_cropped = os.path.join(cropped_dir, entry.scientific_name)
            extractor.extract_species(entry.scientific_name, species_cropped, features_dir)
    else:
        print("跳过特征提取（--skip-extract）")

    # ---- 4. 离群点检测 → 语义去重 → Train/Val 划分 ----
    print("=" * 60)
    print("阶段 3: 离群点检测 + 语义去重 + Train/Val 划分")
    print("=" * 60)
    from model.src.outlier_detector import detect_outliers
    from model.src.semantic_dedup import semantic_deduplicate
    from model.src.splitter import split_and_copy

    for entry in species_list:
        sp_name = entry.scientific_name
        sp_report = report["per_species"].setdefault(sp_name, {})

        # 加载特征向量和路径映射
        npy_path = os.path.join(features_dir, f"{sp_name}.npy")
        paths_json = os.path.join(features_dir, f"{sp_name}_paths.json")

        if not os.path.isfile(npy_path) or not os.path.isfile(paths_json):
            print(f"错误: {sp_name} 特征文件不存在: {npy_path}，跳过")
            sp_report["after_outlier"] = 0
            sp_report["after_dedup"] = 0
            sp_report["train"] = 0
            sp_report["val"] = 0
            continue

        features = np.load(npy_path)
        with open(paths_json, "r", encoding="utf-8") as f:
            image_paths = json.load(f)

        if len(features) == 0:
            print(f"错误: {sp_name} 最终图片数为 0，跳过")
            sp_report["after_outlier"] = 0
            sp_report["after_dedup"] = 0
            sp_report["train"] = 0
            sp_report["val"] = 0
            continue

        # 离群点检测
        alpha = entry.outlier_alpha if entry.outlier_alpha is not None else 0.975
        mask = detect_outliers(features, alpha=alpha)
        features_clean = features[mask]
        paths_clean = [p for p, m in zip(image_paths, mask) if m]
        sp_report["after_outlier"] = len(paths_clean)
        sp_report["outlier_removed"] = int(len(image_paths) - len(paths_clean))
        sp_report["used_pca"] = bool(len(features) < 1.5 * features.shape[1] and len(features) >= 10)
        sp_report["used_cosine_fallback"] = bool(len(features) < 10)
        sp_report["outlier_alpha"] = alpha

        print(f"  [{sp_name}] 离群点检测: {len(image_paths)} → {len(paths_clean)} "
              f"(移除 {sp_report['outlier_removed']})")

        if len(paths_clean) == 0:
            print(f"错误: {sp_name} 离群点检测后图片数为 0，跳过")
            sp_report["after_dedup"] = 0
            sp_report["train"] = 0
            sp_report["val"] = 0
            continue

        # 语义去重
        features_dedup, paths_dedup = semantic_deduplicate(
            features_clean, paths_clean, threshold=args.cosine_threshold
        )
        sp_report["after_dedup"] = len(paths_dedup)
        sp_report["dedup_removed"] = int(len(paths_clean) - len(paths_dedup))

        print(f"  [{sp_name}] 语义去重: {len(paths_clean)} → {len(paths_dedup)} "
              f"(移除 {sp_report['dedup_removed']})")

        if len(paths_dedup) == 0:
            print(f"错误: {sp_name} 最终图片数为 0，跳过")
            sp_report["train"] = 0
            sp_report["val"] = 0
            continue

        if len(paths_dedup) < 20:
            print(f"警告: {sp_name} 清洗后图片数 {len(paths_dedup)} < 20")

        # Train/Val 划分
        split_stats = split_and_copy(
            sp_name, paths_dedup, train_dir, val_dir, random_state=42
        )
        sp_report["train"] = split_stats.train_count
        sp_report["val"] = split_stats.val_count

        print(f"  [{sp_name}] 划分: train={split_stats.train_count} val={split_stats.val_count}")

    # ---- 5. 汇总统计 ----
    elapsed = time.time() - start_time
    report["elapsed_seconds"] = round(elapsed, 1)

    for sp_name, sp_report in report["per_species"].items():
        report["total_input"] += sp_report.get("input", 0)
        report["total_output"] += sp_report.get("after_dedup", 0)
        report["total_train"] += sp_report.get("train", 0)
        report["total_val"] += sp_report.get("val", 0)

    # 保存报告
    os.makedirs(report_dir, exist_ok=True)
    report_path = os.path.join(report_dir, "cleaning_report.json")
    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)

    # 打印汇总
    print("=" * 60)
    print("清洗完成")
    print(f"  物种数: {report['total_species']}")
    print(f"  总输入: {report['total_input']}")
    print(f"  总输出: {report['total_output']}")
    print(f"  训练集: {report['total_train']}")
    print(f"  验证集: {report['total_val']}")
    print(f"  耗时: {elapsed:.1f} 秒")
    print(f"  报告: {report_path}")
    print("=" * 60)


if __name__ == "__main__":
    main()
