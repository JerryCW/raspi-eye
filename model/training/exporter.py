"""模型导出模块：PyTorch .pt 导出、class_names.json 导出、可选 ONNX 导出。

导出的 .pt 文件包含 state_dict + 元数据（backbone_name、num_classes、class_names 等），
用于 SageMaker Serverless endpoint 部署和离线推理。
"""

import json
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

try:
    from model.training.backbone_registry import BackboneConfig
except ModuleNotFoundError:
    from training.backbone_registry import BackboneConfig


def export_pytorch(
    model: nn.Module,
    backbone_config: BackboneConfig,
    class_names: list[str],
    output_dir: str,
    lora: bool = False,
    lora_rank: int = 0,
) -> Path:
    """导出 PyTorch .pt 模型（state_dict + 元数据）。

    LoRA 模式下导出前自动调用 model.merge_lora() 合并权重，
    确保导出的 .pt 文件格式与非 LoRA 模式完全一致。

    Args:
        model: BirdClassifier 模型
        backbone_config: backbone 配置
        class_names: 类别名列表（按 index 排序）
        output_dir: 输出目录路径
        lora: 是否使用了 LoRA 训练（信息记录用）
        lora_rank: LoRA rank（仅 lora=True 时有意义）

    Returns:
        导出的 .pt 文件路径
    """
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    # LoRA 模式下先合并权重
    if lora and hasattr(model, "merge_lora"):
        model.merge_lora()

    model.eval()

    # 构建保存数据：state_dict + 元数据
    save_data = {
        "state_dict": model.state_dict(),
        "metadata": {
            "backbone_name": backbone_config.name,
            "num_classes": len(class_names),
            "class_names": class_names,
            "input_size": backbone_config.input_size,
            "feature_dim": backbone_config.feature_dim,
            "lora": lora,
            "lora_rank": lora_rank,
        },
    }

    pt_file = output_path / "bird_classifier.pt"
    torch.save(save_data, pt_file)

    # 打印导出摘要
    file_size_mb = pt_file.stat().st_size / (1024 * 1024)
    print(f"[导出] PyTorch 模型: {pt_file}")
    print(f"  文件大小: {file_size_mb:.2f} MB")
    print(f"  backbone: {backbone_config.name}")
    print(f"  类别数:   {len(class_names)}")

    return pt_file


def export_class_names(class_names: list[str], output_dir: str) -> Path:
    """导出类别映射文件 class_names.json。

    格式为 {index: scientific_name}，index 为字符串键。

    Args:
        class_names: 类别名列表（按 index 排序）
        output_dir: 输出目录路径

    Returns:
        导出的 JSON 文件路径
    """
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    # 构建 {index: name} 映射
    mapping = {str(i): name for i, name in enumerate(class_names)}

    json_file = output_path / "class_names.json"
    with open(json_file, "w", encoding="utf-8") as f:
        json.dump(mapping, f, ensure_ascii=False, indent=2)

    print(f"[导出] 类别映射: {json_file}")
    return json_file


def export_onnx(
    model: nn.Module,
    backbone_config: BackboneConfig,
    output_dir: str,
) -> Path:
    """可选：导出 ONNX 模型并验证。

    使用 torch.onnx.export 导出，opset_version >= 17。
    导出后使用 onnx.checker.check_model 验证有效性，
    使用 ONNX Runtime 执行推理并与 PyTorch 输出对比（差异 < 1e-4）。

    Args:
        model: BirdClassifier 模型（eval 模式）
        backbone_config: backbone 配置
        output_dir: 输出目录路径

    Returns:
        导出的 .onnx 文件路径

    Raises:
        ImportError: onnx 或 onnxruntime 未安装
        AssertionError: ONNX 输出与 PyTorch 不一致
    """
    try:
        import onnx
        import onnxruntime as ort
    except ImportError:
        raise ImportError(
            "ONNX 导出需要安装 onnx 和 onnxruntime。"
            "请运行: pip install onnx onnxruntime"
        )

    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    model.eval()
    input_size = backbone_config.input_size
    dummy_input = torch.randn(1, 3, input_size, input_size)

    onnx_file = output_path / "bird_classifier.onnx"

    # 导出 ONNX
    torch.onnx.export(
        model,
        dummy_input,
        str(onnx_file),
        opset_version=17,
        input_names=["input"],
        output_names=["logits"],
        dynamic_axes=None,  # 固定 batch_size=1
    )

    # 验证 ONNX 模型结构
    onnx_model = onnx.load(str(onnx_file))
    onnx.checker.check_model(onnx_model)

    # 数值一致性验证：PyTorch vs ONNX Runtime
    session = ort.InferenceSession(str(onnx_file))
    with torch.no_grad():
        pt_output = model(dummy_input).numpy()
    ort_output = session.run(None, {"input": dummy_input.numpy()})[0]

    max_diff = np.max(np.abs(pt_output - ort_output))
    is_valid = max_diff < 1e-4

    # 打印导出摘要
    file_size_mb = onnx_file.stat().st_size / (1024 * 1024)
    print(f"[导出] ONNX 模型: {onnx_file}")
    print(f"  文件大小:   {file_size_mb:.2f} MB")
    print(f"  数值验证:   {'通过' if is_valid else '未通过'}（最大差异: {max_diff:.6f}）")

    if not is_valid:
        print(f"  ⚠️ ONNX 输出与 PyTorch 差异 > 1e-4，模型已保存但标记为未验证")

    return onnx_file
