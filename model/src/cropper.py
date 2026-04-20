"""YOLO 鸟体裁切模块。

使用 YOLOv11 检测图片中的鸟体 bounding box，裁切置信度最高的区域并扩展 padding，
letterbox resize 到 518×518。未检测到鸟体时丢弃。
"""

from dataclasses import dataclass
from pathlib import Path

from PIL import Image

from src.cleaner import letterbox_resize

BIRD_CLASS_ID = 14  # COCO class 14 = bird


@dataclass
class CropStats:
    """单物种裁切统计。"""

    species: str = ""
    total: int = 0
    cropped: int = 0
    discarded: int = 0


def crop_bird(
    image_path: str,
    model,
    conf_threshold: float = 0.3,
    padding: float = 0.2,
    min_bbox_ratio: float = 0.01,
) -> Image.Image | None:
    """YOLO 鸟体裁切。

    Args:
        image_path: 输入图片路径
        model: YOLOv11 模型实例（ultralytics.YOLO）
        conf_threshold: 最低置信度阈值
        padding: bounding box 四周扩展比例（默认 20%）
        min_bbox_ratio: bbox 面积占原图面积的最小比例（默认 1%），低于此值丢弃

    Returns:
        裁切后的 PIL Image（518×518 letterbox resize），未检测到鸟体时返回 None
    """
    img = Image.open(image_path).convert("RGB")
    results = model(image_path, verbose=False)

    # 筛选 bird class，置信度 ≥ 阈值
    bird_boxes = [
        box
        for box in results[0].boxes
        if int(box.cls) == BIRD_CLASS_ID and float(box.conf) >= conf_threshold
    ]

    if not bird_boxes:
        return None

    # 取置信度最高的 box
    best = max(bird_boxes, key=lambda b: float(b.conf))
    x1, y1, x2, y2 = best.xyxy[0].tolist()

    # bbox 面积占比检查：鸟太小则丢弃
    bbox_area = (x2 - x1) * (y2 - y1)
    img_area = img.width * img.height
    if img_area > 0 and bbox_area / img_area < min_bbox_ratio:
        return None

    # 扩展 padding（clamp 到图片边界）
    w, h = x2 - x1, y2 - y1
    x1 = max(0, x1 - w * padding)
    y1 = max(0, y1 - h * padding)
    x2 = min(img.width, x2 + w * padding)
    y2 = min(img.height, y2 + h * padding)

    cropped = img.crop((int(x1), int(y1), int(x2), int(y2)))
    return letterbox_resize(cropped, 518)


def crop_species(
    species_name: str,
    input_dir: str,
    output_dir: str,
    model,
    conf_threshold: float = 0.3,
    padding: float = 0.2,
    min_bbox_ratio: float = 0.01,
) -> CropStats:
    """裁切单个物种所有图片。

    Args:
        species_name: 物种名称
        input_dir: 输入图片目录（该物种的子目录）
        output_dir: 输出目录（裁切后图片保存位置）
        model: YOLOv11 模型实例
        conf_threshold: 最低置信度阈值
        padding: bounding box 四周扩展比例

    Returns:
        CropStats: 裁切统计
    """
    input_path = Path(input_dir)
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    image_paths = sorted([
        str(p)
        for p in input_path.iterdir()
        if p.suffix.lower() in (".jpg", ".jpeg", ".png", ".webp")
    ])

    stats = CropStats(species=species_name, total=len(image_paths))
    discarded_files: list[str] = []

    for img_path in image_paths:
        result = crop_bird(img_path, model, conf_threshold, padding, min_bbox_ratio)
        if result is not None:
            out_name = Path(img_path).stem + ".jpg"
            result.save(str(output_path / out_name), format="JPEG", quality=95)
            stats.cropped += 1
        else:
            stats.discarded += 1
            discarded_files.append(Path(img_path).name)

    # 保存丢弃文件列表，方便人工验证
    if discarded_files:
        discard_log = output_path / "discarded.txt"
        discard_log.write_text("\n".join(discarded_files) + "\n", encoding="utf-8")

    print(
        f"[{species_name}] 裁切统计: 总数={stats.total} "
        f"成功={stats.cropped} 丢弃={stats.discarded}"
    )

    # 写入完成标记，用于断点续传判断
    (output_path / ".done").write_text(
        f"total={stats.total} cropped={stats.cropped} discarded={stats.discarded}\n",
        encoding="utf-8",
    )

    return stats
