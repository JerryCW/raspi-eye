"""SageMaker PyTorch Inference Toolkit 推理脚本。

实现四个钩子函数：model_fn、input_fn、predict_fn、output_fn。
完全自包含，不依赖外部 training 模块（容器内没有这些文件）。
"""

import base64
import io
import json
import logging
import os

import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image
from torchvision.transforms import v2

logger = logging.getLogger(__name__)

# input_fn 支持的 content type
SUPPORTED_CONTENT_TYPES = {"image/jpeg", "application/x-image"}

# ImageNet 标准化参数（与训练时一致）
IMAGENET_MEAN = [0.485, 0.456, 0.406]
IMAGENET_STD = [0.229, 0.224, 0.225]

# 模块级变量，供 input_fn 使用（model_fn 加载后设置）
_input_size = 518
_yolo_model = None
BIRD_CLASS_ID = 14  # COCO class 14 = bird


def _get_val_transform(input_size: int) -> v2.Compose:
    """验证集预处理变换（与 training/augmentation.py 的 get_val_transform 一致）。"""
    return v2.Compose([
        v2.Resize(input_size),
        v2.CenterCrop(input_size),
        v2.ToImage(),
        v2.ToDtype(torch.float32, scale=True),
        v2.Normalize(mean=IMAGENET_MEAN, std=IMAGENET_STD),
    ])


def _letterbox_resize(image: Image.Image, target_size: int) -> Image.Image:
    """Letterbox resize：等比缩放 + 黑色填充到 target_size × target_size。

    与 cleaning/cleaner.py 中的 letterbox_resize 行为等效（内联实现，不依赖 cleaning 模块）。
    """
    w, h = image.size
    scale = target_size / max(w, h)
    new_w = max(1, min(int(w * scale), target_size))
    new_h = max(1, min(int(h * scale), target_size))
    resized = image.resize((new_w, new_h), Image.LANCZOS)
    canvas = Image.new("RGB", (target_size, target_size), color=(0, 0, 0))
    paste_x = (target_size - new_w) // 2
    paste_y = (target_size - new_h) // 2
    canvas.paste(resized, (paste_x, paste_y))
    return canvas


def _yolo_crop(image: Image.Image, yolo_model, conf_threshold: float = 0.3, padding: float = 0.2) -> Image.Image | None:
    """YOLO 鸟体裁切。返回 letterbox resize 后的 PIL Image，未检测到鸟体时返回 None。"""
    results = yolo_model(image, verbose=False)
    bird_boxes = [
        box for box in results[0].boxes
        if int(box.cls) == BIRD_CLASS_ID and float(box.conf) >= conf_threshold
    ]
    if not bird_boxes:
        return None
    best = max(bird_boxes, key=lambda b: float(b.conf))
    x1, y1, x2, y2 = best.xyxy[0].tolist()
    w, h = x2 - x1, y2 - y1
    x1 = max(0, x1 - w * padding)
    y1 = max(0, y1 - h * padding)
    x2 = min(image.width, x2 + w * padding)
    y2 = min(image.height, y2 + h * padding)
    cropped = image.crop((int(x1), int(y1), int(x2), int(y2)))
    return _letterbox_resize(cropped, _input_size)


class _BirdClassifier(nn.Module):
    """推理用 BirdClassifier（简化版，仅用于 load_state_dict + forward）。"""

    def __init__(self, backbone: nn.Module, num_classes: int, feature_dim: int):
        super().__init__()
        self.backbone = backbone
        self.head = nn.Linear(feature_dim, num_classes)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        with torch.no_grad():
            features = self.backbone(x).last_hidden_state[:, 0]
        return self.head(features)


def _create_backbone_offline(backbone_name: str):
    """创建 backbone 模型结构。

    使用 AutoConfig.from_pretrained 下载 config.json（几 KB），
    然后 AutoModel.from_config 创建空模型结构（不下载权重）。
    需要 HF_TOKEN 环境变量（DINOv3 是 gated model）。
    """
    from transformers import AutoConfig, AutoModel

    hf_model_map = {
        "dinov3-vitl16": "facebook/dinov3-vitl16-pretrain-lvd1689m",
        "dinov2-vitl14": "facebook/dinov2-large",
    }

    base_name = backbone_name.replace("-lora", "")
    hf_model_id = hf_model_map.get(base_name)
    if not hf_model_id:
        raise ValueError(f"不支持的 backbone: {backbone_name}")

    token = os.environ.get("HF_TOKEN")
    logger.info("从 AutoConfig 创建空 backbone: %s", hf_model_id)
    config = AutoConfig.from_pretrained(hf_model_id, token=token)
    return AutoModel.from_config(config)


def model_fn(model_dir: str) -> dict:
    """加载模型（完全离线，不访问 HuggingFace）。"""
    global _input_size

    model_path = os.path.join(model_dir, "bird_classifier.pt")
    if not os.path.exists(model_path):
        contents = os.listdir(model_dir)
        raise FileNotFoundError(f"找不到 {model_path}。model_dir 内容: {contents}")

    checkpoint = torch.load(model_path, map_location="cpu", weights_only=False)

    if "metadata" not in checkpoint:
        raise KeyError("checkpoint 缺少 'metadata' 字段。")

    metadata = checkpoint["metadata"]
    backbone_name = metadata["backbone_name"]
    num_classes = metadata["num_classes"]
    class_names = metadata["class_names"]
    input_size = metadata["input_size"]
    feature_dim = metadata["feature_dim"]

    _input_size = input_size

    # 离线构建模型
    backbone = _create_backbone_offline(backbone_name)
    model = _BirdClassifier(backbone, num_classes, feature_dim)

    # 加载权重
    model.load_state_dict(checkpoint["state_dict"])
    model.eval()

    val_transform = _get_val_transform(input_size)

    logger.info("模型加载完成: backbone=%s, num_classes=%d, input_size=%d",
                backbone_name, num_classes, input_size)

    # 加载 YOLO 模型（可选）
    global _yolo_model
    yolo_path = os.path.join(model_dir, "yolo11s.pt")
    if os.path.exists(yolo_path):
        from ultralytics import YOLO
        _yolo_model = YOLO(yolo_path)
        logger.info("YOLO 模型加载完成: %s", yolo_path)
    else:
        _yolo_model = None
        logger.warning("未找到 YOLO 模型: %s，将使用原始预处理流程", yolo_path)

    return {
        "model": model,
        "transform": val_transform,
        "class_names": class_names,
        "metadata": metadata,
        "yolo_model": _yolo_model,
    }


def input_fn(request_body: bytes, content_type: str) -> dict:
    """反序列化输入：JPEG 二进制 → dict(tensor, cropped_image)。"""
    if content_type not in SUPPORTED_CONTENT_TYPES:
        raise ValueError(f"不支持的 content_type: '{content_type}'。支持: {sorted(SUPPORTED_CONTENT_TYPES)}")

    try:
        image = Image.open(io.BytesIO(request_body)).convert("RGB")
    except Exception as e:
        raise ValueError(f"无法解码图片数据: {e}") from e

    cropped_image = None
    if _yolo_model is not None:
        try:
            cropped_image = _yolo_crop(image, _yolo_model)
        except Exception as e:
            logger.warning("YOLO 推理异常，回退到原始预处理流程: %s", e)
            cropped_image = None

    if cropped_image is not None:
        # YOLO crop 路径：letterbox resize 后的图片 → ToImage → ToDtype → Normalize
        normalize = v2.Compose([
            v2.ToImage(),
            v2.ToDtype(torch.float32, scale=True),
            v2.Normalize(mean=IMAGENET_MEAN, std=IMAGENET_STD),
        ])
        tensor = normalize(cropped_image).unsqueeze(0)
    else:
        # 回退路径：原来的 Resize + CenterCrop + Normalize
        transform = _get_val_transform(_input_size)
        tensor = transform(image).unsqueeze(0)

    return {"tensor": tensor, "cropped_image": cropped_image}


def predict_fn(input_data: dict, model_dict: dict) -> dict:
    """执行推理：模型前向传播 → softmax → top-5，透传 cropped_image。"""
    tensor = input_data["tensor"]
    cropped_image = input_data["cropped_image"]
    model = model_dict["model"]
    class_names = model_dict["class_names"]
    metadata = model_dict["metadata"]

    with torch.no_grad():
        logits = model(tensor)
        probabilities = F.softmax(logits, dim=1)

    top_k = min(5, probabilities.shape[1])
    top_probs, top_indices = torch.topk(probabilities[0], top_k)

    predictions = []
    for prob, idx in zip(top_probs, top_indices):
        predictions.append({
            "species": class_names[idx.item()],
            "confidence": round(prob.item(), 6),
        })

    return {
        "predictions": predictions,
        "model_metadata": {
            "backbone": metadata["backbone_name"],
            "num_classes": metadata["num_classes"],
        },
        "cropped_image": cropped_image,
    }


def output_fn(prediction: dict, accept: str) -> tuple[str, str]:
    """序列化输出：prediction dict → JSON（含 cropped_image_b64）。"""
    cropped_image = prediction.pop("cropped_image", None)
    cropped_b64 = None
    if cropped_image is not None:
        try:
            buf = io.BytesIO()
            cropped_image.save(buf, format="JPEG", quality=95)
            cropped_b64 = base64.b64encode(buf.getvalue()).decode("ascii")
        except Exception as e:
            logger.warning("crop 图片 base64 编码失败: %s", e)
            cropped_b64 = None
    prediction["cropped_image_b64"] = cropped_b64
    return json.dumps(prediction, ensure_ascii=False), "application/json"
