"""数据增强模块：训练集增强与验证集预处理。

使用 torchvision.transforms.v2，Normalize 使用 ImageNet 标准值。
input_size 参数化，由 BackboneConfig 决定。
"""

import torch
from torchvision.transforms import v2

# ImageNet 标准化参数
IMAGENET_MEAN = [0.485, 0.456, 0.406]
IMAGENET_STD = [0.229, 0.224, 0.225]


def get_train_transform(input_size: int) -> v2.Compose:
    """训练集数据增强变换。

    按顺序应用：RandomResizedCrop → RandomHorizontalFlip → ColorJitter
    → ToImage → ToDtype(float32) → Normalize(ImageNet)。

    Args:
        input_size: 输入图片尺寸（正方形边长），由 BackboneConfig 决定
    """
    return v2.Compose([
        v2.RandomResizedCrop(input_size, scale=(0.6, 1.0)),
        v2.RandomHorizontalFlip(p=0.5),
        v2.ColorJitter(brightness=0.3, contrast=0.3, saturation=0.2, hue=0.1),
        v2.ToImage(),
        v2.ToDtype(torch.float32, scale=True),
        v2.Normalize(mean=IMAGENET_MEAN, std=IMAGENET_STD),
    ])


def get_val_transform(input_size: int) -> v2.Compose:
    """验证集预处理变换（无随机增强）。

    按顺序应用：Resize → CenterCrop → ToImage → ToDtype(float32)
    → Normalize(ImageNet)。

    Args:
        input_size: 输入图片尺寸（正方形边长），由 BackboneConfig 决定
    """
    return v2.Compose([
        v2.Resize(input_size),
        v2.CenterCrop(input_size),
        v2.ToImage(),
        v2.ToDtype(torch.float32, scale=True),
        v2.Normalize(mean=IMAGENET_MEAN, std=IMAGENET_STD),
    ])
