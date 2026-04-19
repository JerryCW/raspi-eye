"""余弦相似度语义去重模块。

计算余弦相似度矩阵，重复组中保留距类中心最近（余弦距离最小）的样本。
"""

from dataclasses import dataclass

import numpy as np
from sklearn.metrics.pairwise import cosine_similarity


@dataclass
class DedupStats:
    """语义去重统计。"""

    species: str = ""
    input_count: int = 0
    removed_count: int = 0
    kept_count: int = 0


def semantic_deduplicate(
    features: np.ndarray,
    paths: list[str],
    threshold: float = 0.95,
) -> tuple[np.ndarray, list[str]]:
    """基于余弦相似度的语义去重。

    重复组中保留距类中心最近（余弦距离最小）的样本。

    Args:
        features: (N, D) 特征矩阵
        paths: 对应的图片路径列表
        threshold: 余弦相似度阈值（≥ threshold 视为重复）

    Returns:
        (去重后的特征矩阵, 去重后的路径列表)
    """
    if len(features) <= 1:
        return features.copy(), list(paths)

    sim_matrix = cosine_similarity(features)
    centroid = np.mean(features, axis=0)

    # 预计算每个样本到类中心的余弦距离
    centroid_sim = cosine_similarity(
        features, centroid.reshape(1, -1)
    ).flatten()
    centroid_dist = 1.0 - centroid_sim  # 距离越小越具代表性

    removed: set[int] = set()

    for i in range(len(paths)):
        if i in removed:
            continue
        for j in range(i + 1, len(paths)):
            if j in removed:
                continue
            if sim_matrix[i, j] >= threshold:
                # 保留距类中心更近的（余弦距离更小的）
                if centroid_dist[i] <= centroid_dist[j]:
                    removed.add(j)
                else:
                    removed.add(i)
                    break  # i 被移除，跳出内层循环

    kept = [idx for idx in range(len(paths)) if idx not in removed]
    return features[kept], [paths[idx] for idx in kept]
