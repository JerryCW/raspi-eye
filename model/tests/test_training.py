"""鸟类分类模型训练框架属性测试。

使用 Hypothesis PBT 验证模型构建不变量、数据增强、评估指标等。
所有测试使用 MockBackbone，不依赖 GPU/网络/真实模型权重。
"""

import torch
import torch.nn as nn
from hypothesis import given, settings
from hypothesis import strategies as st

from model.training.backbone_registry import BackboneConfig
from model.training.classifier import BirdClassifier


# ── 测试用 MockBackbone ──────────────────────────────────────────────────────


class MockBackbone(nn.Module):
    """测试用小型 backbone，随机初始化，不依赖网络。"""

    def __init__(self, feature_dim: int = 64):
        super().__init__()
        self.proj = nn.Linear(3 * 32 * 32, feature_dim)

    def forward(self, x):
        return self.proj(x.flatten(1))


def _make_mock_config(feature_dim: int = 64) -> BackboneConfig:
    """构造使用 MockBackbone 的 BackboneConfig。"""
    return BackboneConfig(
        name="mock-backbone",
        load_fn=lambda: MockBackbone(feature_dim),
        extract_fn=lambda model, x: model(x),
        input_size=32,
        feature_dim=feature_dim,
    )


# ── Property 1: 模型构建不变量 ────────────────────────────────────────────────
# Frozen backbone + Trainable head + 正确维度
# **Validates: Requirements 1.3, 1.4, 1.5, 7.1**


@given(num_classes=st.integers(min_value=2, max_value=100))
@settings(max_examples=100)
def test_model_construction_invariants(num_classes: int):
    """Property 1: 模型构建不变量。

    对任意 num_classes ∈ [2, 100]，验证：
    - backbone 参数全部 requires_grad=False
    - head 参数全部 requires_grad=True
    - head.in_features == feature_dim
    - head.out_features == num_classes
    - trainable_parameters() 恰好等于 head 参数
    """
    feature_dim = 64
    config = _make_mock_config(feature_dim)
    model = BirdClassifier(num_classes=num_classes, backbone_config=config)

    # backbone 参数全部冻结
    for param in model.backbone.parameters():
        assert not param.requires_grad, "backbone 参数应全部 requires_grad=False"

    # head 参数全部可训练
    for param in model.head.parameters():
        assert param.requires_grad, "head 参数应全部 requires_grad=True"

    # head 维度正确
    assert model.head.in_features == feature_dim, (
        f"head.in_features 应为 {feature_dim}，实际为 {model.head.in_features}"
    )
    assert model.head.out_features == num_classes, (
        f"head.out_features 应为 {num_classes}，实际为 {model.head.out_features}"
    )

    # trainable_parameters() 恰好等于 head 参数
    trainable_set = {id(p) for p in model.trainable_parameters()}
    head_set = {id(p) for p in model.head.parameters()}
    assert trainable_set == head_set, (
        "trainable_parameters() 应恰好等于 head 参数集合"
    )



# ── 单元测试：backbone_registry ───────────────────────────────────────────────


def test_get_backbone_registered_names():
    """验证所有内置 backbone 名称可正常获取（仅 dinov3-vitl16 和 dinov2-vitl14）。"""
    from model.training.backbone_registry import BACKBONE_REGISTRY, get_backbone

    # 确保只有两个 backbone
    assert set(BACKBONE_REGISTRY.keys()) == {"dinov3-vitl16", "dinov2-vitl14"}

    for name in BACKBONE_REGISTRY:
        config = get_backbone(name)
        assert config.name == name
        assert config.input_size > 0
        assert config.feature_dim > 0


def test_get_backbone_unknown_raises_valueerror():
    """验证未注册名称抛出 ValueError 并列出可用 backbone。"""
    import pytest

    from model.training.backbone_registry import get_backbone

    with pytest.raises(ValueError, match="未知的 backbone"):
        get_backbone("nonexistent-backbone")


def test_bird_classifier_forward_shape():
    """验证 BirdClassifier forward 输出 shape 正确。"""
    config = _make_mock_config(feature_dim=64)
    model = BirdClassifier(num_classes=10, backbone_config=config)
    x = torch.randn(4, 3, 32, 32)
    out = model(x)
    assert out.shape == (4, 10), f"输出 shape 应为 (4, 10)，实际为 {out.shape}"


# ── Property 2: 数据增强输出尺寸不变量 ────────────────────────────────────────
# 对任意 input_size ∈ {224, 518} 和任意合法 RGB 图片（宽高 ∈ [32, 2048]），
# 验证训练增强和验证预处理输出 shape 恒为 (3, input_size, input_size)
# **Validates: Requirements 1.6, 2.2, 2.3, 7.2**

from PIL import Image
import numpy as np

from model.training.augmentation import get_train_transform, get_val_transform


def _random_pil_image(width: int, height: int) -> Image.Image:
    """生成指定尺寸的随机 RGB PIL Image。"""
    arr = np.random.randint(0, 256, (height, width, 3), dtype=np.uint8)
    return Image.fromarray(arr, mode="RGB")


@given(
    input_size=st.sampled_from([224, 518]),
    width=st.integers(min_value=32, max_value=2048),
    height=st.integers(min_value=32, max_value=2048),
)
@settings(max_examples=100)
def test_augmentation_output_shape_invariant(
    input_size: int, width: int, height: int
):
    """Property 2: 数据增强输出尺寸不变量。

    对任意 input_size ∈ {224, 518} 和任意合法 RGB 图片（宽高 ∈ [32, 2048]），
    验证训练增强和验证预处理输出 shape 恒为 (3, input_size, input_size)。
    """
    img = _random_pil_image(width, height)
    expected_shape = (3, input_size, input_size)

    # 训练增强
    train_tf = get_train_transform(input_size)
    train_out = train_tf(img)
    assert train_out.shape == expected_shape, (
        f"训练增强输出 shape 应为 {expected_shape}，实际为 {train_out.shape}"
    )

    # 验证预处理
    val_tf = get_val_transform(input_size)
    val_out = val_tf(img)
    assert val_out.shape == expected_shape, (
        f"验证预处理输出 shape 应为 {expected_shape}，实际为 {val_out.shape}"
    )


# ── Property 3: 评估指标数学不变量 ────────────────────────────────────────────
# 对任意随机 logits 矩阵 [N, C] 和标签 [N]，验证：
# - top5_accuracy >= top1_accuracy
# - per_class_accuracy ∈ [0.0, 1.0]
# - per_class_count 之和 == N
# **Validates: Requirements 4.2**

from model.training.evaluator import (
    EvaluationReport,
    compute_confusion_matrix,
    evaluate_from_logits,
    find_top_confused_pairs,
)


@given(
    n=st.integers(min_value=10, max_value=200),
    c=st.integers(min_value=2, max_value=20),
)
@settings(max_examples=100)
def test_evaluation_metrics_math_invariants(n: int, c: int):
    """Property 3: 评估指标数学不变量。

    对任意随机 logits 矩阵 [N, C] 和标签 [N]，验证：
    - top5_accuracy >= top1_accuracy
    - per_class_accuracy ∈ [0.0, 1.0]
    - per_class_count 之和 == N
    """
    # 生成随机 logits 和标签
    logits = torch.randn(n, c)
    labels = torch.randint(0, c, (n,))
    class_names = [f"species_{i}" for i in range(c)]

    report = evaluate_from_logits(logits, labels, class_names)

    # top5_accuracy >= top1_accuracy
    assert report.top5_accuracy >= report.top1_accuracy, (
        f"top5_accuracy ({report.top5_accuracy}) 应 >= top1_accuracy ({report.top1_accuracy})"
    )

    # per_class_accuracy ∈ [0.0, 1.0]
    for name, acc in report.per_class_accuracy.items():
        assert 0.0 <= acc <= 1.0, (
            f"类别 {name} 的 accuracy ({acc}) 应在 [0.0, 1.0] 范围内"
        )

    # per_class_count 之和 == N
    total_count = sum(report.per_class_count.values())
    assert total_count == n, (
        f"per_class_count 之和 ({total_count}) 应等于 N ({n})"
    )



# ── Property 4: 混淆矩阵正确性与 top-k 易混淆对提取 ─────────────────────────
# 对任意随机预测标签和真实标签（N 样本，C 类别），验证：
# - shape == (C, C)
# - 元素 >= 0
# - 每行之和 == 该类样本数
# - 总和 == N
# - top-k 对确实是非对角线最大值降序
# **Validates: Requirements 4.3, 4.4**


@given(
    n=st.integers(min_value=10, max_value=200),
    c=st.integers(min_value=2, max_value=20),
)
@settings(max_examples=100)
def test_confusion_matrix_correctness(n: int, c: int):
    """Property 4: 混淆矩阵正确性与 top-k 易混淆对提取。

    对任意随机预测标签和真实标签（N 样本，C 类别），验证：
    - shape == (C, C)
    - 元素 >= 0
    - 每行之和 == 该类样本数
    - 总和 == N
    - top-k 对确实是非对角线最大值降序
    """
    # 生成随机预测和真实标签
    preds = np.random.randint(0, c, size=n)
    labels = np.random.randint(0, c, size=n)
    class_names = [f"species_{i}" for i in range(c)]

    cm = compute_confusion_matrix(preds, labels, c)

    # shape == (C, C)
    assert cm.shape == (c, c), f"混淆矩阵 shape 应为 ({c}, {c})，实际为 {cm.shape}"

    # 元素 >= 0
    assert np.all(cm >= 0), "混淆矩阵所有元素应 >= 0"

    # 每行之和 == 该类样本数
    for i in range(c):
        row_sum = cm[i].sum()
        expected = (labels == i).sum()
        assert row_sum == expected, (
            f"第 {i} 行之和 ({row_sum}) 应等于该类样本数 ({expected})"
        )

    # 总和 == N
    assert cm.sum() == n, f"混淆矩阵总和 ({cm.sum()}) 应等于 N ({n})"

    # top-k 对确实是非对角线最大值降序
    k = 5
    top_pairs = find_top_confused_pairs(cm, class_names, k=k)

    # 收集所有非对角线元素值
    off_diag_values = []
    for i in range(c):
        for j in range(c):
            if i != j and cm[i, j] > 0:
                off_diag_values.append(cm[i, j])
    off_diag_values.sort(reverse=True)

    # top_pairs 的混淆次数应与非对角线最大值一致（降序）
    pair_counts = [count for _, _, count in top_pairs]
    expected_top = off_diag_values[:k]
    assert pair_counts == expected_top, (
        f"top-k 混淆次数 {pair_counts} 应等于非对角线最大 {k} 个值 {expected_top}"
    )

    # 降序验证
    for i in range(len(pair_counts) - 1):
        assert pair_counts[i] >= pair_counts[i + 1], (
            f"top-k 对应降序排列，但 {pair_counts[i]} < {pair_counts[i + 1]}"
        )


# ── Property 5: 模型导出 round-trip ───────────────────────────────────────────
# 对任意 num_classes ∈ [2, 50] 的随机初始化 BirdClassifier（MockBackbone，非 LoRA），
# 导出 .pt → 重新加载 → 验证 state_dict 数值一致、元数据完全恢复、推理输出一致
# **Validates: Requirements 5.1, 7.4**

import tempfile

from model.training.exporter import export_pytorch


@given(num_classes=st.integers(min_value=2, max_value=50))
@settings(max_examples=100)
def test_export_roundtrip(num_classes: int):
    """Property 5: 模型导出 round-trip。

    对任意 num_classes ∈ [2, 50] 的随机初始化 BirdClassifier（MockBackbone），
    导出 .pt → 重新加载 → 验证：
    - state_dict 数值完全一致
    - 元数据（backbone_name、num_classes、input_size、feature_dim、class_names）完全恢复
    - 对相同输入张量，推理输出完全一致
    """
    feature_dim = 64
    config = _make_mock_config(feature_dim)
    class_names = [f"species_{i}" for i in range(num_classes)]

    # 构建模型
    model = BirdClassifier(num_classes=num_classes, backbone_config=config)
    model.eval()

    with tempfile.TemporaryDirectory() as tmpdir:
        # 导出
        pt_path = export_pytorch(
            model, config, class_names, tmpdir, lora=False, lora_rank=0
        )
        assert pt_path.exists(), f"导出文件不存在: {pt_path}"

        # 重新加载
        loaded = torch.load(pt_path, weights_only=False)
        loaded_state = loaded["state_dict"]
        loaded_meta = loaded["metadata"]

        # 验证元数据完全恢复
        assert loaded_meta["backbone_name"] == config.name
        assert loaded_meta["num_classes"] == num_classes
        assert loaded_meta["input_size"] == config.input_size
        assert loaded_meta["feature_dim"] == config.feature_dim
        assert loaded_meta["class_names"] == class_names
        assert loaded_meta["lora"] is False
        assert loaded_meta["lora_rank"] == 0

        # 验证 state_dict 数值完全一致
        original_state = model.state_dict()
        assert set(loaded_state.keys()) == set(original_state.keys()), (
            f"state_dict 键不一致: "
            f"多余={set(loaded_state.keys()) - set(original_state.keys())}, "
            f"缺失={set(original_state.keys()) - set(loaded_state.keys())}"
        )
        for key in original_state:
            assert torch.equal(original_state[key], loaded_state[key]), (
                f"state_dict['{key}'] 数值不一致"
            )

        # 构建新模型并加载权重，验证推理输出一致
        model2 = BirdClassifier(num_classes=num_classes, backbone_config=config)
        model2.load_state_dict(loaded_state)
        model2.eval()

        # 使用固定随机输入
        x = torch.randn(2, 3, config.input_size, config.input_size)
        with torch.no_grad():
            out1 = model(x)
            out2 = model2(x)
        assert torch.equal(out1, out2), "加载后模型推理输出与原始模型不一致"


# ── 单元测试：训练循环（合成小数据集）────────────────────────────────────────
# 合成 2 类 × 10 张 × 32×32 随机图片，MockBackbone，3 epoch
# 验证：loss 下降、best checkpoint 文件存在、导出文件存在
# **Validates: Requirements 7.3, 7.4**

import os
import argparse
from unittest.mock import patch


def _create_synthetic_dataset(root_dir, num_classes=2, images_per_class=10, size=32):
    """创建合成 ImageFolder 数据集：每类使用不同亮度的图片，确保可分离。

    Args:
        root_dir: 数据集根目录
        num_classes: 类别数
        images_per_class: 每类图片数
        size: 图片尺寸（正方形边长）

    Returns:
        类别名列表
    """
    class_names = [f"species_{i}" for i in range(num_classes)]
    for cls_idx, cls_name in enumerate(class_names):
        cls_dir = os.path.join(root_dir, cls_name)
        os.makedirs(cls_dir, exist_ok=True)
        for j in range(images_per_class):
            # 每类使用不同的基础亮度，确保特征可分离
            # 类 0: 暗色（0-50），类 1: 亮色（200-255）
            low = cls_idx * 200
            high = low + 55
            arr = np.random.randint(low, min(high, 256), (size, size, 3), dtype=np.uint8)
            img = Image.fromarray(arr, mode="RGB")
            img.save(os.path.join(cls_dir, f"img_{j:04d}.jpg"), format="JPEG")
    return class_names


def test_training_loop_synthetic():
    """训练循环单元测试：合成小数据集，验证 loss 下降和文件导出。

    使用 MockBackbone（input_size=32, feature_dim=64），3 epoch 训练。
    验证：
    - loss 下降（epoch 2 的 loss < epoch 0 的 loss）
    - best checkpoint 文件存在
    - 导出文件存在（bird_classifier.pt、class_names.json、evaluation_report.json）
    """
    from model.training.train import train

    # 固定随机种子，确保测试可重复
    torch.manual_seed(42)
    np.random.seed(42)

    with tempfile.TemporaryDirectory() as tmpdir:
        # 创建合成数据集
        train_dir = os.path.join(tmpdir, "train")
        val_dir = os.path.join(tmpdir, "val")
        output_dir = os.path.join(tmpdir, "output")

        _create_synthetic_dataset(train_dir, num_classes=2, images_per_class=10, size=32)
        _create_synthetic_dataset(val_dir, num_classes=2, images_per_class=5, size=32)

        # 构造 mock backbone 配置
        mock_config = _make_mock_config(feature_dim=64)

        # 构造训练参数
        checkpoint_dir = os.path.join(tmpdir, "checkpoints")
        args = argparse.Namespace(
            backbone="mock-backbone",
            lora=False,
            lora_rank=8,
            epochs=10,
            batch_size=4,
            lr=1e-1,  # 大学习率，确保在合成数据上 loss 下降
            weight_decay=0.0,  # 不用权重衰减，让模型更容易拟合
            num_workers=0,  # 测试中不用多进程
            export_onnx=False,
            train_dir=train_dir,
            val_dir=val_dir,
            output_dir=output_dir,
            checkpoint_dir=checkpoint_dir,
            s3_model_prefix="",
        )

        # monkey-patch get_backbone 返回 MockBackbone 配置
        with patch("model.training.train.get_backbone", return_value=mock_config):
            result = train(args)

        # 验证 best checkpoint 文件存在
        assert os.path.exists(os.path.join(output_dir, "best_checkpoint.pth")), \
            "best_checkpoint.pth 应存在"

        # 验证导出文件存在
        assert os.path.exists(os.path.join(output_dir, "bird_classifier.pt")), \
            "bird_classifier.pt 应存在"
        assert os.path.exists(os.path.join(output_dir, "class_names.json")), \
            "class_names.json 应存在"
        assert os.path.exists(os.path.join(output_dir, "evaluation_report.json")), \
            "evaluation_report.json 应存在"

        # 验证训练摘要
        assert "best_epoch" in result
        assert "best_top1" in result
        assert result["best_top1"] >= 0.0

        # 验证 loss 下降（最后一个 epoch 的 loss < 第一个 epoch 的 loss）
        losses = result["epoch_train_losses"]
        assert len(losses) == 10, f"应有 10 个 epoch 的 loss，实际 {len(losses)}"
        assert losses[-1] < losses[0], (
            f"最后 epoch 的 loss ({losses[-1]:.4f}) 应 < 第一个 epoch 的 loss ({losses[0]:.4f})"
        )
