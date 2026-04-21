"""Backbone 注册表：统一管理不同预训练模型的加载、特征提取和配置。

通过 BACKBONE_REGISTRY 字典注册所有可用 backbone，
训练/评估/导出代码通过 get_backbone(name) 获取配置，无需硬编码特定 backbone 逻辑。
"""

import os
from dataclasses import dataclass
from typing import Callable

import torch
import torch.nn as nn


@dataclass
class BackboneConfig:
    """Backbone 配置。"""

    name: str  # 注册名称
    load_fn: Callable[[], nn.Module]  # 加载 backbone 模型
    extract_fn: Callable[[nn.Module, torch.Tensor], torch.Tensor]  # 提取特征向量
    input_size: int  # 输入图片尺寸（正方形边长）
    feature_dim: int  # 特征向量维度
    needs_hf_token: bool = False  # 是否需要 HuggingFace token


# ── DINOv3 ViT-L/16 ──────────────────────────────────────────────────────────


def _load_dinov3() -> nn.Module:
    """加载 DINOv3 ViT-L/16（HuggingFace gated model，需要 HF_TOKEN）。"""
    from transformers import AutoModel

    token = os.environ.get("HF_TOKEN")
    if not token:
        raise RuntimeError(
            "DINOv3 是 gated model，需要设置环境变量 HF_TOKEN。"
            "请从 https://huggingface.co/settings/tokens 获取 token。"
        )
    return AutoModel.from_pretrained(
        "facebook/dinov3-vitl16-pretrain-lvd1689m", token=token
    )


# ── DINOv2 ViT-L/14 ──────────────────────────────────────────────────────────


def _load_dinov2() -> nn.Module:
    """加载 DINOv2 ViT-L/14（公开模型，无需 token）。"""
    from transformers import AutoModel

    return AutoModel.from_pretrained("facebook/dinov2-large")


# ── DINOv3/DINOv2 共用特征提取 ───────────────────────────────────────────────


def _extract_cls_token(model: nn.Module, x: torch.Tensor) -> torch.Tensor:
    """提取 CLS token 特征（last_hidden_state[:, 0]）。"""
    return model(x).last_hidden_state[:, 0]


# ── Backbone 注册表 ───────────────────────────────────────────────────────────

BACKBONE_REGISTRY: dict[str, BackboneConfig] = {
    "dinov3-vitl16": BackboneConfig(
        name="dinov3-vitl16",
        load_fn=_load_dinov3,
        extract_fn=_extract_cls_token,
        input_size=518,
        feature_dim=1024,
        needs_hf_token=True,
    ),
    "dinov2-vitl14": BackboneConfig(
        name="dinov2-vitl14",
        load_fn=_load_dinov2,
        extract_fn=_extract_cls_token,
        input_size=518,
        feature_dim=1024,
        needs_hf_token=False,
    ),
}


def get_backbone(name: str) -> BackboneConfig:
    """根据名称获取 backbone 配置。

    Args:
        name: backbone 注册名称

    Returns:
        对应的 BackboneConfig

    Raises:
        ValueError: 名称未注册时抛出，并列出所有可用 backbone
    """
    if name not in BACKBONE_REGISTRY:
        available = ", ".join(sorted(BACKBONE_REGISTRY.keys()))
        raise ValueError(
            f"未知的 backbone: '{name}'。可用的 backbone: {available}"
        )
    return BACKBONE_REGISTRY[name]
