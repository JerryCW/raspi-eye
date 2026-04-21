"""模型评估模块：计算分类指标、混淆矩阵和易混淆物种对。

提供完整的评估流程：遍历 dataloader 收集预测 → 计算 top-1/top-5 accuracy →
生成 per-class accuracy → 构建混淆矩阵 → 提取 top-k 易混淆对。
"""

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn


@dataclass
class EvaluationReport:
    """评估报告，包含所有评估指标。"""

    top1_accuracy: float
    top5_accuracy: float
    per_class_accuracy: dict[str, float]
    per_class_count: dict[str, int]
    confusion_matrix: list[list[int]]
    top5_confused_pairs: list[tuple[str, str, int]]
    class_names: list[str]


def evaluate(
    model: nn.Module,
    dataloader: torch.utils.data.DataLoader,
    class_names: list[str],
    device: torch.device,
) -> EvaluationReport:
    """在验证集上执行完整评估。

    Args:
        model: 分类模型（eval 模式）
        dataloader: 验证集 DataLoader
        class_names: 类别名列表（按 index 排序）
        device: 计算设备

    Returns:
        EvaluationReport 包含所有评估指标
    """
    model.eval()
    all_logits_list: list[torch.Tensor] = []
    all_labels_list: list[torch.Tensor] = []

    with torch.no_grad():
        for images, labels in dataloader:
            images = images.to(device)
            logits = model(images)
            all_logits_list.append(logits.cpu())
            all_labels_list.append(labels)

    all_logits = torch.cat(all_logits_list, dim=0)
    all_labels = torch.cat(all_labels_list, dim=0)

    return evaluate_from_logits(all_logits, all_labels, class_names)


def evaluate_from_logits(
    all_logits: torch.Tensor,
    all_labels: torch.Tensor,
    class_names: list[str],
) -> EvaluationReport:
    """从 logits 和标签直接计算评估指标（不需要模型和 dataloader）。

    用于属性测试中直接验证评估逻辑的数学正确性。

    Args:
        all_logits: 预测 logits，shape (N, C)
        all_labels: 真实标签，shape (N,)，值 ∈ [0, C)
        class_names: 类别名列表，长度 == C

    Returns:
        EvaluationReport 包含所有评估指标
    """
    num_classes = len(class_names)
    n = all_logits.shape[0]

    # 计算 top-1 和 top-5 预测
    all_preds = all_logits.argmax(dim=1)

    # top-1 accuracy
    top1_correct = (all_preds == all_labels).sum().item()
    top1_accuracy = top1_correct / n if n > 0 else 0.0

    # top-5 accuracy（取 min(5, C) 防止类别数不足 5）
    k = min(5, num_classes)
    top5_preds = all_logits.topk(k, dim=1).indices
    top5_correct = 0
    for i in range(n):
        if all_labels[i] in top5_preds[i]:
            top5_correct += 1
    top5_accuracy = top5_correct / n if n > 0 else 0.0

    # per-class accuracy 和 per-class count
    per_class_accuracy: dict[str, float] = {}
    per_class_count: dict[str, int] = {}
    for c in range(num_classes):
        mask = all_labels == c
        count = mask.sum().item()
        per_class_count[class_names[c]] = int(count)
        if count > 0:
            correct = (all_preds[mask] == c).sum().item()
            per_class_accuracy[class_names[c]] = correct / count
        else:
            per_class_accuracy[class_names[c]] = 0.0

    # 混淆矩阵
    cm = compute_confusion_matrix(
        all_preds.numpy(), all_labels.numpy(), num_classes
    )

    # top-5 易混淆对
    top5_confused = find_top_confused_pairs(cm, class_names, k=5)

    return EvaluationReport(
        top1_accuracy=top1_accuracy,
        top5_accuracy=top5_accuracy,
        per_class_accuracy=per_class_accuracy,
        per_class_count=per_class_count,
        confusion_matrix=cm.tolist(),
        top5_confused_pairs=top5_confused,
        class_names=class_names,
    )


def compute_confusion_matrix(
    all_preds: np.ndarray,
    all_labels: np.ndarray,
    num_classes: int,
) -> np.ndarray:
    """计算混淆矩阵。

    cm[i][j] 表示真实类别为 i、预测为 j 的样本数。

    Args:
        all_preds: 预测标签数组，shape (N,)
        all_labels: 真实标签数组，shape (N,)
        num_classes: 类别总数

    Returns:
        混淆矩阵，shape (num_classes, num_classes)，dtype int
    """
    cm = np.zeros((num_classes, num_classes), dtype=int)
    for pred, label in zip(all_preds, all_labels):
        cm[int(label), int(pred)] += 1
    return cm


def find_top_confused_pairs(
    cm: np.ndarray,
    class_names: list[str],
    k: int = 5,
) -> list[tuple[str, str, int]]:
    """从混淆矩阵非对角线元素中找出最大的 k 个值（降序）。

    Args:
        cm: 混淆矩阵，shape (C, C)
        class_names: 类别名列表，长度 == C
        k: 返回的易混淆对数量

    Returns:
        列表，每个元素为 (真实类名, 预测类名, 混淆次数)，按混淆次数降序
    """
    num_classes = cm.shape[0]
    pairs: list[tuple[str, str, int]] = []

    for i in range(num_classes):
        for j in range(num_classes):
            if i != j and cm[i, j] > 0:
                pairs.append((class_names[i], class_names[j], int(cm[i, j])))

    # 按混淆次数降序排列
    pairs.sort(key=lambda x: x[2], reverse=True)
    return pairs[:k]


def save_evaluation_report(report: EvaluationReport, output_dir: str) -> None:
    """保存评估报告到 JSON 文件。

    生成两个文件：
    - evaluation_report.json：包含 accuracy、per-class 指标和 top-5 易混淆对
    - confusion_matrix.json：包含类别名和完整混淆矩阵

    Args:
        report: 评估报告
        output_dir: 输出目录路径
    """
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    # 保存 evaluation_report.json
    eval_data = {
        "top1_accuracy": report.top1_accuracy,
        "top5_accuracy": report.top5_accuracy,
        "per_class": {
            name: {
                "accuracy": report.per_class_accuracy[name],
                "count": report.per_class_count[name],
            }
            for name in report.class_names
        },
        "top5_confused_pairs": [
            {"species_a": a, "species_b": b, "count": c}
            for a, b, c in report.top5_confused_pairs
        ],
    }
    eval_file = output_path / "evaluation_report.json"
    with open(eval_file, "w", encoding="utf-8") as f:
        json.dump(eval_data, f, ensure_ascii=False, indent=2)

    # 保存 confusion_matrix.json
    cm_data = {
        "class_names": report.class_names,
        "matrix": report.confusion_matrix,
    }
    cm_file = output_path / "confusion_matrix.json"
    with open(cm_file, "w", encoding="utf-8") as f:
        json.dump(cm_data, f, ensure_ascii=False, indent=2)
