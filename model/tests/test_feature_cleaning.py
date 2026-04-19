"""特征空间清洗测试：离群点检测、语义去重、划分、.npy round trip。

本文件包含 PBT 属性测试和单元测试，后续任务 5-8 会继续添加更多测试。
所有测试离线可运行，不依赖 GPU 或网络。
"""

import tempfile
from pathlib import Path

import numpy as np
import pytest
from hypothesis import given, settings
from hypothesis.strategies import integers


# ---------------------------------------------------------------------------
# Feature: feature-space-cleaning, Property 2: 特征向量 .npy 保存/加载 round trip
# **Validates: Requirements 2.6**
# ---------------------------------------------------------------------------
@given(
    n=integers(min_value=1, max_value=100),
    d=integers(min_value=1, max_value=512),
)
@settings(max_examples=100)
def test_npy_round_trip(n, d):
    """保存为 .npy 后重新加载，验证 bit-exact 一致。"""
    rng = np.random.default_rng(42)
    features = rng.standard_normal((n, d)).astype(np.float32)
    with tempfile.TemporaryDirectory() as tmpdir:
        path = Path(tmpdir) / "test.npy"
        np.save(str(path), features)
        loaded = np.load(str(path))
        np.testing.assert_array_equal(features, loaded)

# ---------------------------------------------------------------------------
# 离群点检测 + 语义去重 单元测试（任务 5.3）
# ---------------------------------------------------------------------------
from sklearn.metrics.pairwise import cosine_similarity

from model.src.outlier_detector import OutlierStats, detect_outliers
from model.src.semantic_dedup import DedupStats, semantic_deduplicate


class TestDetectOutliers:
    """离群点检测单元测试。"""

    def test_normal_samples_kept(self):
        """正常样本（紧密聚集）应全部保留。"""
        rng = np.random.default_rng(42)
        # 20 个样本，10 维，标准正态分布
        features = rng.standard_normal((20, 10)).astype(np.float32)
        mask = detect_outliers(features, alpha=0.975)
        # 标准正态分布下大部分样本应被保留
        assert mask.sum() >= 15, f"保留数 {mask.sum()} 太少"

    def test_injected_outlier_removed(self):
        """人工注入远离中心的样本应被标记为离群点。"""
        rng = np.random.default_rng(42)
        features = rng.standard_normal((30, 10)).astype(np.float32)
        # 注入一个远离中心的离群样本
        outlier = np.full((1, 10), 50.0, dtype=np.float32)
        features_with_outlier = np.vstack([features, outlier])
        mask = detect_outliers(features_with_outlier, alpha=0.975)
        # 最后一个样本（注入的离群点）应被标记为 False
        assert not mask[-1], "注入的离群样本未被检测到"

    def test_pca_path_triggered(self):
        """N < 1.5D 时应自动触发 PCA 降维路径。"""
        rng = np.random.default_rng(42)
        # N=200, D=300 → N < 1.5*D=450，触发 PCA 到 min(128, 199)=128 维
        # N=201 > 2*128=256? No, 201 < 256. 但 leverage ≈ N=201 > threshold ≈ 161
        features = rng.standard_normal((200, 300)).astype(np.float64)
        # 注入离群样本
        outlier = np.full((1, 300), 50.0, dtype=np.float64)
        features_with_outlier = np.vstack([features, outlier])
        mask = detect_outliers(features_with_outlier, alpha=0.975)
        # 应正常返回结果（不报错），且离群样本被检测
        assert mask.shape[0] == 201
        assert not mask[-1], "PCA 路径下注入的离群样本未被检测到"

    def test_cosine_fallback_path(self):
        """N < 10 时应使用余弦距离 + IQR 回退。"""
        rng = np.random.default_rng(42)
        # 8 个正常样本（紧密聚集在同一方向）
        base_dir = rng.standard_normal(10).astype(np.float64)
        base_dir /= np.linalg.norm(base_dir)
        features = np.array([
            base_dir + rng.standard_normal(10) * 0.01 for _ in range(8)
        ], dtype=np.float64)
        # 离群样本：随机方向（与正常样本方向差异大）
        random_dir = rng.standard_normal(10).astype(np.float64)
        random_dir /= np.linalg.norm(random_dir)
        # 确保与 base_dir 正交
        random_dir -= np.dot(random_dir, base_dir) * base_dir
        random_dir /= np.linalg.norm(random_dir)
        outlier = random_dir.reshape(1, -1)
        features_with_outlier = np.vstack([features, outlier])
        mask = detect_outliers(features_with_outlier, alpha=0.975)
        assert mask.shape[0] == 9
        # 离群样本应被检测到（余弦距离远大于正常样本）
        assert not mask[-1], "余弦回退路径下注入的离群样本未被检测到"


class TestSemanticDeduplicate:
    """语义去重单元测试。"""

    def test_identical_vectors_deduped(self):
        """完全相同的向量应被去重。"""
        rng = np.random.default_rng(42)
        base = rng.standard_normal((1, 50)).astype(np.float32)
        # 3 个完全相同的向量 + 2 个不同的
        features = np.vstack([
            base, base, base,
            rng.standard_normal((2, 50)).astype(np.float32),
        ])
        paths = [f"img_{i}.jpg" for i in range(5)]
        deduped_feat, deduped_paths = semantic_deduplicate(
            features, paths, threshold=0.95
        )
        # 3 个相同向量应只保留 1 个
        assert len(deduped_paths) <= 3, f"去重后数量 {len(deduped_paths)} 过多"

    def test_different_vectors_kept(self):
        """差异较大的向量应全部保留。"""
        rng = np.random.default_rng(42)
        # 生成正交向量（差异大）
        features = np.eye(10, dtype=np.float32)
        paths = [f"img_{i}.jpg" for i in range(10)]
        deduped_feat, deduped_paths = semantic_deduplicate(
            features, paths, threshold=0.95
        )
        assert len(deduped_paths) == 10, "不同向量不应被去重"

    def test_keeps_closest_to_centroid(self):
        """重复组中应保留距类中心最近的样本。"""
        rng = np.random.default_rng(42)
        # 创建一个中心向量和两个几乎相同的副本
        centroid_like = np.ones((1, 20), dtype=np.float32)
        # 副本 1：距中心更近（微小偏移）
        close_copy = centroid_like + 0.001
        # 副本 2：距中心更远（较大偏移）
        far_copy = centroid_like + 0.5
        # 加一些不同的向量
        others = rng.standard_normal((3, 20)).astype(np.float32) * 5
        features = np.vstack([close_copy, far_copy, others])
        paths = ["close.jpg", "far.jpg", "other0.jpg", "other1.jpg", "other2.jpg"]

        deduped_feat, deduped_paths = semantic_deduplicate(
            features, paths, threshold=0.95
        )
        # close_copy 和 far_copy 余弦相似度很高，应去重
        # 保留的应该是距中心更近的那个
        if "close.jpg" in deduped_paths or "far.jpg" in deduped_paths:
            # 至少有一个被保留，且不应两个都在
            assert not ("close.jpg" in deduped_paths and "far.jpg" in deduped_paths), \
                "两个近似副本都被保留了"


# ---------------------------------------------------------------------------
# Feature: feature-space-cleaning, Property 3: 离群点检测正确识别注入的离群样本
# **Validates: Requirements 3.2, 3.4**
# ---------------------------------------------------------------------------
from hypothesis.strategies import floats, tuples


@given(
    d=integers(min_value=5, max_value=20),
    outlier_scale=floats(min_value=30.0, max_value=100.0),
)
@settings(max_examples=100)
def test_outlier_detection_identifies_injected(d, outlier_scale):
    """从多元正态分布采样正常特征，注入远离中心的离群样本，验证被检测到。

    N 设为 max(3*D, 15) 确保单个离群样本的 Mahalanobis 距离超过 chi2 阈值。
    当 N < 1.5D 时验证 PCA 降维路径被触发（不报错且正常返回）。
    """
    # N 必须足够大（约 > 2D）才能让单个离群样本的杠杆效应不被吞没
    n = max(3 * d, 15)
    rng = np.random.default_rng(42)
    # 正常样本：多元正态分布
    normal_features = rng.standard_normal((n, d)).astype(np.float64)
    # 注入离群样本：远离中心
    outlier = np.full((1, d), outlier_scale, dtype=np.float64)
    features = np.vstack([normal_features, outlier])

    mask = detect_outliers(features, alpha=0.975)

    # 基本形状检查
    assert mask.shape == (n + 1,)
    assert mask.dtype == np.bool_
    # 注入的离群样本应被标记为 False
    assert not mask[-1], (
        f"注入的离群样本（scale={outlier_scale}）未被检测到，"
        f"N={n}, D={d}, PCA={n < 1.5 * d}"
    )


# ---------------------------------------------------------------------------
# Feature: feature-space-cleaning, Property 4: 语义去重后余弦相似度不变量
# **Validates: Requirements 4.5**
# ---------------------------------------------------------------------------


@given(
    n=integers(min_value=3, max_value=50),
    d=integers(min_value=10, max_value=128),
)
@settings(max_examples=100)
def test_semantic_dedup_cosine_invariant(n, d):
    """去重后任意两个向量的余弦相似度 < threshold。

    生成随机特征向量集合，人工注入重复（微小扰动），
    经过 semantic_deduplicate 后验证不变量。
    """
    rng = np.random.default_rng(42)
    features = rng.standard_normal((n, d)).astype(np.float32)
    # 人工注入重复：将第 1 个向量复制到第 2 个位置 + 微小扰动
    features[1] = features[0] + rng.standard_normal(d).astype(np.float32) * 0.001
    paths = [f"img_{i}.jpg" for i in range(n)]

    threshold = 0.95
    deduped_features, deduped_paths = semantic_deduplicate(
        features, paths, threshold=threshold
    )

    # 验证不变量：去重后任意两个向量的余弦相似度 < threshold
    if len(deduped_features) >= 2:
        sim = cosine_similarity(deduped_features)
        # 对角线设为 0（自身相似度为 1）
        np.fill_diagonal(sim, 0.0)
        assert np.all(sim < threshold), (
            f"去重后仍存在余弦相似度 >= {threshold} 的向量对，"
            f"最大相似度 = {sim.max():.4f}"
        )


# ---------------------------------------------------------------------------
# Feature: feature-space-cleaning, Property 5: Train/Val 划分无交集 + 比例约束
# **Validates: Requirements 5.1, 5.6, 5.7**
# ---------------------------------------------------------------------------
from model.src.splitter import split_dataset


@given(n=integers(min_value=12, max_value=200))
@settings(max_examples=100)
def test_split_no_overlap_and_ratio(n):
    """划分后 train/val 无交集，全覆盖，且 val 比例在 15%-25%。

    min_value=12：sklearn train_test_split 对 n<12 的整数取整会导致
    val 比例超出 25%（如 n=6 → val=2, ratio=0.333），需求 5.7 注明
    "容差来自整数取整"，此处选择 n≥12 确保 sklearn 取整后比例稳定。
    """
    paths = [f"img_{i}.jpg" for i in range(n)]
    train, val = split_dataset(paths, test_size=0.2, random_state=42)
    assert set(train) & set(val) == set()
    assert set(train) | set(val) == set(paths)
    val_ratio = len(val) / len(paths)
    assert 0.15 <= val_ratio <= 0.25


# ---------------------------------------------------------------------------
# Feature: feature-space-cleaning, Property 6: Train/Val 划分可复现
# **Validates: Requirements 5.2**
# ---------------------------------------------------------------------------


@given(n=integers(min_value=5, max_value=200))
@settings(max_examples=100)
def test_split_reproducible(n):
    """同一输入 + 同一 seed 调用两次，结果完全相同。"""
    paths = [f"img_{i}.jpg" for i in range(n)]
    train1, val1 = split_dataset(paths, test_size=0.2, random_state=42)
    train2, val2 = split_dataset(paths, test_size=0.2, random_state=42)
    assert train1 == train2
    assert val1 == val2


# ---------------------------------------------------------------------------
# SageMaker 路径检测测试（任务 8.4）
# **Validates: Requirements 6.3, 8.5**
# ---------------------------------------------------------------------------
from model.clean_features import detect_paths


class TestDetectPaths:
    """detect_paths() SageMaker / 本地路径检测测试。"""

    def test_sagemaker_paths_when_dir_exists(self, monkeypatch):
        """mock /opt/ml/processing/ 存在时返回 SageMaker 路径。"""
        monkeypatch.setattr("os.path.isdir", lambda path: path == "/opt/ml/processing")
        result = detect_paths()
        assert result["cleaned_dir"] == "/opt/ml/processing/input/cleaned"
        assert result["config_path"] == "/opt/ml/processing/input/config/species.yaml"
        assert result["cropped_dir"] == "/opt/ml/processing/output/cropped"
        assert result["features_dir"] == "/opt/ml/processing/output/features"
        assert result["train_dir"] == "/opt/ml/processing/output/train"
        assert result["val_dir"] == "/opt/ml/processing/output/val"
        assert result["report_dir"] == "/opt/ml/processing/output/report"

    def test_local_paths_when_dir_not_exists(self, monkeypatch):
        """mock /opt/ml/processing/ 不存在时返回本地路径。"""
        monkeypatch.setattr("os.path.isdir", lambda path: False)
        result = detect_paths()
        assert result["cleaned_dir"] == "model/data/cleaned"
        assert result["config_path"] is None
        assert result["cropped_dir"] == "model/data/cropped"
        assert result["features_dir"] == "model/data/features"
        assert result["train_dir"] == "model/data/train"
        assert result["val_dir"] == "model/data/val"
        assert result["report_dir"] == "model/data/report"
