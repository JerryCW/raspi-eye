"""DINOv3 特征提取模块。

使用 DINOv3 ViT-L/16 frozen backbone 提取图片的 class token 特征向量，
支持 GPU 混合精度推理和 CPU 回退。

需求：2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import torch
from PIL import Image
from torch.utils.data import DataLoader, Dataset
from torchvision.transforms import v2


@dataclass
class ExtractStats:
    """特征提取统计。"""

    species: str = ""
    num_images: int = 0
    feature_dim: int = 0
    device: str = "cpu"


class _ImageFolderDataset(Dataset):
    """简单的图片文件夹 Dataset，返回预处理后的 tensor。"""

    def __init__(self, image_paths: list[str], transform: v2.Compose) -> None:
        self.image_paths = image_paths
        self.transform = transform

    def __len__(self) -> int:
        return len(self.image_paths)

    def __getitem__(self, idx: int) -> torch.Tensor:
        img = Image.open(self.image_paths[idx]).convert("RGB")
        return self.transform(img)


class FeatureExtractor:
    """DINOv3 ViT-L/16 特征提取器。

    frozen backbone，仅提取 class token 特征向量，不做任何参数更新。
    """

    def __init__(
        self,
        repo_dir: str,
        weights_path: str,
        batch_size: int = 32,
        num_workers: int = 4,
    ) -> None:
        self.repo_dir = repo_dir
        self.weights_path = weights_path
        self.batch_size = batch_size
        self.num_workers = num_workers

        # 自动检测 GPU/CPU
        if torch.cuda.is_available():
            self.device = torch.device("cuda")
        else:
            print("警告: GPU 不可用，回退到 CPU 推理（速度较慢）")
            self.device = torch.device("cpu")

        self.model = self._load_model()
        self.transform = self._make_transform()

    def _load_model(self) -> torch.nn.Module:
        """加载 DINOv3 ViT-L/16 frozen backbone。

        优先从本地 repo 加载（source='local'），
        如果本地 repo 不存在则通过 Hugging Face Transformers 加载。
        """
        # 尝试本地 repo 加载
        if self.repo_dir and os.path.isfile(os.path.join(self.repo_dir, "hubconf.py")):
            print(f"从本地 repo 加载 DINOv3: {self.repo_dir}")
            model = torch.hub.load(
                self.repo_dir,
                "dinov3_vitl16",
                source="local",
                weights=self.weights_path if self.weights_path else None,
            )
            model.eval()
            model.to(self.device)
            return model

        # Hugging Face Transformers 加载
        model_id = "facebook/dinov3-vitl16-pretrain-lvd1689m"
        print(f"从 Hugging Face 加载 DINOv3: {model_id}")
        from transformers import AutoImageProcessor, AutoModel as HFAutoModel

        self._hf_processor = AutoImageProcessor.from_pretrained(model_id)
        hf_model = HFAutoModel.from_pretrained(model_id)
        hf_model.eval()
        hf_model.to(self.device)

        # 包装成返回 class token 的 callable
        class _HFWrapper(torch.nn.Module):
            def __init__(self, hf_model):
                super().__init__()
                self.hf_model = hf_model

            def forward(self, x):
                outputs = self.hf_model(pixel_values=x)
                return outputs.last_hidden_state[:, 0]

        wrapper = _HFWrapper(hf_model)
        wrapper.eval()
        wrapper.to(self.device)
        return wrapper

    def _make_transform(self) -> v2.Compose:
        """DINOv3 标准预处理。

        如果使用 HF 加载，用 AutoImageProcessor 的参数；
        否则使用默认 ImageNet 预处理。
        """
        if hasattr(self, '_hf_processor') and self._hf_processor is not None:
            # 从 HF processor 提取 normalize 参数
            mean = self._hf_processor.image_mean
            std = self._hf_processor.image_std
            size = self._hf_processor.size.get("height", 518)
        else:
            mean = (0.485, 0.456, 0.406)
            std = (0.229, 0.224, 0.225)
            size = 518

        return v2.Compose([
            v2.ToImage(),
            v2.Resize((size, size), antialias=True),
            v2.ToDtype(torch.float32, scale=True),
            v2.Normalize(mean=mean, std=std),
        ])

    def extract_species(
        self,
        species_name: str,
        image_dir: str,
        output_dir: str,
    ) -> ExtractStats:
        """提取单个物种的所有图片特征向量，保存为 .npy。

        使用 DataLoader 批量加载，torch.autocast('cuda') 混合精度推理。
        输出：output_dir/{species_name}.npy (N, feature_dim)
              output_dir/{species_name}_paths.json
        """
        image_dir_path = Path(image_dir)
        output_dir_path = Path(output_dir)
        output_dir_path.mkdir(parents=True, exist_ok=True)

        # 收集图片路径
        extensions = {".jpg", ".jpeg", ".png"}
        image_paths = sorted(
            str(p)
            for p in image_dir_path.iterdir()
            if p.suffix.lower() in extensions
        )

        if not image_paths:
            print(f"警告: {species_name} 在 {image_dir} 中没有找到图片")
            return ExtractStats(species=species_name, device=str(self.device))

        # 构建 DataLoader
        dataset = _ImageFolderDataset(image_paths, self.transform)
        dataloader = DataLoader(
            dataset,
            batch_size=self.batch_size,
            num_workers=self.num_workers,
            pin_memory=(self.device.type == "cuda"),
        )

        # 批量推理
        all_features: list[np.ndarray] = []
        with torch.no_grad():
            with torch.autocast("cuda", dtype=torch.float16):
                for batch in dataloader:
                    batch = batch.to(self.device)
                    features = self.model(batch)
                    all_features.append(features.cpu().numpy())

        # 拼接并保存
        features_array = np.concatenate(all_features, axis=0)
        npy_path = output_dir_path / f"{species_name}.npy"
        paths_json_path = output_dir_path / f"{species_name}_paths.json"

        np.save(str(npy_path), features_array)
        with open(paths_json_path, "w", encoding="utf-8") as f:
            json.dump(image_paths, f, ensure_ascii=False)

        stats = ExtractStats(
            species=species_name,
            num_images=len(image_paths),
            feature_dim=features_array.shape[1],
            device=str(self.device),
        )
        print(
            f"  {species_name}: {stats.num_images} 张图片 → "
            f"特征维度 {stats.feature_dim}，设备 {stats.device}"
        )
        return stats
