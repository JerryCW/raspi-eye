"""Mahalanobis 离群点检测模块。

Per-class 离群点检测，三条路径：
- N < 10：余弦距离 + IQR 回退
- N < 1.5D：PCA 降维到 min(128, N-1) 维 → Mahalanobis
- 否则：直接 Mahalanobis distance

协方差矩阵正则化 + np.eye * 1e-6，阈值 chi2.ppf(alpha, df=d)。
"""

from dataclasses import dataclass

import numpy as np
from scipy.spatial.distance import mahalanobis
from scipy.stats import chi2
from sklearn.metrics.pairwise import cosine_similarity


@dataclass
class OutlierStats:
    """离群点检测统计。"""

    species: str = ""
    input_count: int = 0
    removed_count: int = 0
    kept_count: int = 0
    used_pca: bool = False
    used_cosine_fallback: bool = False
    mean_distance: float = 0.0
    removal_ratio: float = 0.0


def detect_outliers(features: np.ndarray, alpha: float = 0.975) -> np.ndarray:
    """Per-class Mahalanobis distance 离群点检测（含 PCA 降维保护）。

    Args:
        features: (N, D) 特征矩阵
        alpha: χ² 分位数（默认 0.975，即 97.5% 置信度）

    Returns:
        布尔数组，True = 正常样本，False = 离群点
    """
    N, D = features.shape

    # 路径 1：N < 10 → 余弦距离 + IQR 回退
    if N < 10:
        centroid = np.mean(features, axis=0)
        cos_dist = 1 - cosine_similarity(
            features, centroid.reshape(1, -1)
        ).flatten()
        q1, q3 = np.percentile(cos_dist, [25, 75])
        iqr = q3 - q1
        return cos_dist <= q3 + 1.5 * iqr

    # 路径 2：N < 1.5D → PCA 降维到 min(128, N-1, D) 维
    used_pca = False
    if N < 1.5 * D:
        from sklearn.decomposition import PCA

        n_components = min(128, N - 1, D)
        pca = PCA(n_components=n_components)
        features = pca.fit_transform(features)
        used_pca = True

    # 路径 3（或路径 2 降维后）：Mahalanobis distance
    mean = np.mean(features, axis=0)
    cov = np.cov(features, rowvar=False)
    # 正则化：防止奇异矩阵
    cov += np.eye(cov.shape[0]) * 1e-6
    cov_inv = np.linalg.inv(cov)

    d = features.shape[1]
    threshold = chi2.ppf(alpha, df=d)

    distances = np.array(
        [mahalanobis(f, mean, cov_inv) ** 2 for f in features]
    )

    return distances <= threshold
