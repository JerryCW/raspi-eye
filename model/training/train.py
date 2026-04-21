"""训练入口脚本：在 SageMaker 容器内或本地执行鸟类分类模型训练。

流程：解析超参数 → 加载 backbone → 构建 BirdClassifier → 数据加载 →
训练循环（CrossEntropyLoss + AdamW + CosineAnnealingLR + AMP）→
评估 → 导出模型。

支持 CLI 参数和 SageMaker 环境变量（SM_CHANNEL_TRAIN、SM_CHANNEL_VAL、SM_MODEL_DIR）。
"""

import argparse
import json
import os
import sys
import time
from collections import Counter
from pathlib import Path

import torch
import torch.nn as nn
from torch.optim import AdamW
from torch.optim.lr_scheduler import CosineAnnealingLR
from torch.utils.data import DataLoader
from torchvision.datasets import ImageFolder

# 兼容两种运行环境的 import 路径：
# 1. 本地开发：从项目根目录运行，model.training.* 可用
# 2. SageMaker 容器：Training Toolkit 解压 sourcedir.tar.gz 后，
#    train.py 在 training/train.py，同级模块在 training/ 下，
#    tar.gz 根目录已在 sys.path 中，所以 training.* 可用
_this_dir = Path(__file__).resolve().parent          # model/training/
_model_dir = _this_dir.parent                        # model/
_project_root = _model_dir.parent                    # 项目根目录
sys.path.insert(0, str(_project_root))               # 本地开发：model.training.* 可用
sys.path.insert(0, str(_model_dir))                  # 容器内：training.* 可用

try:
    from model.training.augmentation import get_train_transform, get_val_transform
    from model.training.backbone_registry import get_backbone
    from model.training.classifier import BirdClassifier
    from model.training.evaluator import evaluate, save_evaluation_report
    from model.training.exporter import export_class_names, export_onnx, export_pytorch
except ModuleNotFoundError:
    from training.augmentation import get_train_transform, get_val_transform
    from training.backbone_registry import get_backbone
    from training.classifier import BirdClassifier
    from training.evaluator import evaluate, save_evaluation_report
    from training.exporter import export_class_names, export_onnx, export_pytorch


def _str2bool(v: str) -> bool:
    """将字符串转为 bool，兼容 SageMaker Training Toolkit 传参方式。

    Toolkit 把 HyperParameters 的值原样传给脚本（如 --lora true），
    而 argparse 的 store_true 不接受值参数，所以用自定义类型解析。
    """
    if isinstance(v, bool):
        return v
    if v.lower() in ("true", "1", "yes"):
        return True
    if v.lower() in ("false", "0", "no"):
        return False
    raise argparse.ArgumentTypeError(f"无法解析为 bool: {v}")


def parse_args() -> argparse.Namespace:
    """解析超参数：支持 CLI 参数和 SageMaker 环境变量。"""
    parser = argparse.ArgumentParser(description="鸟类分类模型训练")

    # 模型参数
    parser.add_argument("--backbone", type=str, default="dinov3-vitl16",
                        help="backbone 注册名称（默认 dinov3-vitl16）")
    parser.add_argument("--lora", type=_str2bool, nargs="?", const=True, default=False,
                        help="启用 LoRA 微调")
    parser.add_argument("--lora-rank", type=int, default=8,
                        help="LoRA rank（默认 8）")

    # 训练参数
    parser.add_argument("--epochs", type=int, default=10,
                        help="训练轮数（默认 10）")
    parser.add_argument("--batch-size", type=int, default=64,
                        help="批大小（默认 64）")
    parser.add_argument("--lr", type=float, default=1e-3,
                        help="学习率（默认 1e-3）")
    parser.add_argument("--weight-decay", type=float, default=1e-4,
                        help="权重衰减（默认 1e-4）")
    parser.add_argument("--num-workers", type=int, default=4,
                        help="DataLoader 工作线程数（默认 4）")

    # 导出参数
    parser.add_argument("--export-onnx", type=_str2bool, nargs="?", const=True, default=False,
                        help="可选：额外导出 ONNX 格式")

    # 数据/输出目录（支持 SageMaker 环境变量回退）
    parser.add_argument("--train-dir", type=str,
                        default=os.environ.get("SM_CHANNEL_TRAIN", ""),
                        help="训练数据目录（默认从 SM_CHANNEL_TRAIN）")
    parser.add_argument("--val-dir", type=str,
                        default=os.environ.get("SM_CHANNEL_VAL", ""),
                        help="验证数据目录（默认从 SM_CHANNEL_VAL）")
    parser.add_argument("--output-dir", type=str,
                        default=os.environ.get("SM_MODEL_DIR", ""),
                        help="模型输出目录（默认从 SM_MODEL_DIR）")
    parser.add_argument("--checkpoint-dir", type=str,
                        default=os.environ.get("SM_CHECKPOINT_DIR",
                                               "/opt/ml/checkpoints"),
                        help="Checkpoint 目录（SageMaker 自动同步到 S3）")
    parser.add_argument("--s3-model-prefix", type=str, default="",
                        help="训练完成后自动上传产物到此 S3 前缀（如 s3://bucket/training/models/dinov2-vitl14/）")

    return parser.parse_args()


def print_dataset_stats(train_dataset: ImageFolder, val_dataset: ImageFolder) -> None:
    """打印数据集统计信息：图片数、类别数、每类样本数范围。"""
    train_counts = Counter(train_dataset.targets)
    val_counts = Counter(val_dataset.targets)

    train_per_class = list(train_counts.values())
    min_train = min(train_per_class) if train_per_class else 0
    max_train = max(train_per_class) if train_per_class else 0

    print(f"[数据集] 训练集: {len(train_dataset)} 张图片")
    print(f"[数据集] 验证集: {len(val_dataset)} 张图片")
    print(f"[数据集] 类别数: {len(train_dataset.classes)}")
    print(f"[数据集] 每类训练样本数: {min_train} ~ {max_train}")


def train_one_epoch(
    model: nn.Module,
    dataloader: DataLoader,
    criterion: nn.Module,
    optimizer: torch.optim.Optimizer,
    device: torch.device,
    use_amp: bool,
    scaler: torch.amp.GradScaler | None,
    epoch: int = 0,
) -> float:
    """执行一个 epoch 的训练，返回平均 loss。"""
    model.train()
    # 非 LoRA 模式下保持 backbone eval
    if hasattr(model, "lora_enabled") and not model.lora_enabled:
        model.backbone.eval()

    total_loss = 0.0
    total_correct = 0
    total_samples = 0
    num_batches = 0
    total_batches = len(dataloader)
    log_interval = max(total_batches // 5, 1)  # 每 epoch 打印约 5 次进度
    epoch_start = time.time()

    for images, labels in dataloader:
        images = images.to(device)
        labels = labels.to(device)

        optimizer.zero_grad()

        if use_amp:
            with torch.amp.autocast("cuda"):
                logits = model(images)
                loss = criterion(logits, labels)
            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()
        else:
            logits = model(images)
            loss = criterion(logits, labels)
            loss.backward()
            optimizer.step()

        total_loss += loss.item()
        total_correct += (logits.argmax(dim=1) == labels).sum().item()
        total_samples += labels.size(0)
        num_batches += 1

        if num_batches % log_interval == 0:
            avg_loss = total_loss / num_batches
            acc = total_correct / total_samples
            elapsed = time.time() - epoch_start
            eta = elapsed / num_batches * (total_batches - num_batches)
            lr = optimizer.param_groups[0]["lr"]
            print(f"  [epoch {epoch}] {num_batches}/{total_batches} "
                  f"loss={avg_loss:.4f} acc={acc:.4f} "
                  f"lr={lr:.6f} ETA={eta:.0f}s", flush=True)

    return total_loss / max(num_batches, 1)


def validate(
    model: nn.Module,
    dataloader: DataLoader,
    criterion: nn.Module,
    device: torch.device,
) -> tuple[float, float, float]:
    """在验证集上评估，返回 (val_loss, top1_accuracy, top5_accuracy)。"""
    model.eval()
    total_loss = 0.0
    top1_correct = 0
    top5_correct = 0
    total = 0
    num_classes = None

    with torch.no_grad():
        for images, labels in dataloader:
            images = images.to(device)
            labels = labels.to(device)

            logits = model(images)
            loss = criterion(logits, labels)
            total_loss += loss.item()

            # top-1
            preds = logits.argmax(dim=1)
            top1_correct += (preds == labels).sum().item()

            # top-5
            if num_classes is None:
                num_classes = logits.shape[1]
            k = min(5, num_classes)
            top5_preds = logits.topk(k, dim=1).indices
            for i in range(labels.size(0)):
                if labels[i] in top5_preds[i]:
                    top5_correct += 1

            total += labels.size(0)

    val_loss = total_loss / max(1, len(dataloader))
    top1_acc = top1_correct / max(total, 1)
    top5_acc = top5_correct / max(total, 1)

    return val_loss, top1_acc, top5_acc


def train(args: argparse.Namespace) -> dict:
    """核心训练函数，可被测试直接调用。

    Args:
        args: 解析后的超参数 Namespace

    Returns:
        训练摘要字典，包含 best_epoch、best_top1、best_top5、total_time 等
    """
    start_time = time.time()

    # ── GPU/CPU 自动检测 ──────────────────────────────────────────────────
    if torch.cuda.is_available():
        device = torch.device("cuda")
        print(f"[设备] 使用 GPU: {torch.cuda.get_device_name(0)}")
        use_amp = True
    else:
        device = torch.device("cpu")
        print("[设备] ⚠️ GPU 不可用，回退到 CPU（训练会很慢）")
        use_amp = False  # CPU 模式下跳过 AMP

    # ── HF_TOKEN：从 Secrets Manager 获取（不通过环境变量传入，避免明文泄露） ──
    if not os.environ.get("HF_TOKEN"):
        try:
            import boto3
            region = os.environ.get("AWS_REGION") or os.environ.get("AWS_DEFAULT_REGION")
            sm_secret = boto3.client("secretsmanager", region_name=region)
            resp = sm_secret.get_secret_value(SecretId="raspi-eye/huggingface-token")
            os.environ["HF_TOKEN"] = resp["SecretString"]
            print("[HF_TOKEN] 从 Secrets Manager 获取")
        except Exception as e:
            print(f"[HF_TOKEN] 警告: 无法从 Secrets Manager 获取: {e}")

    # ── 加载 backbone 配置 ────────────────────────────────────────────────
    backbone_config = get_backbone(args.backbone)
    input_size = backbone_config.input_size

    # ── 数据加载 ──────────────────────────────────────────────────────────
    train_transform = get_train_transform(input_size)
    val_transform = get_val_transform(input_size)

    train_dataset = ImageFolder(args.train_dir, transform=train_transform)
    val_dataset = ImageFolder(args.val_dir, transform=val_transform)

    if len(train_dataset) == 0:
        raise FileNotFoundError(
            f"训练数据目录为空: {args.train_dir}。"
            "期望 ImageFolder 格式: {train_dir}/{class_name}/*.jpg"
        )

    print_dataset_stats(train_dataset, val_dataset)

    class_names = train_dataset.classes
    num_classes = len(class_names)

    train_loader = DataLoader(
        train_dataset,
        batch_size=args.batch_size,
        shuffle=True,
        num_workers=args.num_workers,
        pin_memory=(device.type == "cuda"),
    )
    val_loader = DataLoader(
        val_dataset,
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.num_workers,
        pin_memory=(device.type == "cuda"),
    )

    # ── 构建模型 ──────────────────────────────────────────────────────────
    model = BirdClassifier(
        num_classes=num_classes,
        backbone_config=backbone_config,
        lora=args.lora,
        lora_rank=args.lora_rank,
    )
    model.to(device)

    # ── 优化器和调度器 ────────────────────────────────────────────────────
    criterion = nn.CrossEntropyLoss(label_smoothing=0.1)
    optimizer = AdamW(
        model.trainable_parameters(),
        lr=args.lr,
        weight_decay=args.weight_decay,
    )
    scheduler = CosineAnnealingLR(optimizer, T_max=args.epochs)

    # AMP：GPU 模式下使用 autocast + GradScaler
    scaler = torch.amp.GradScaler("cuda") if use_amp else None

    # ── Checkpoint 恢复 ───────────────────────────────────────────────────
    checkpoint_dir = Path(args.checkpoint_dir)
    checkpoint_dir.mkdir(parents=True, exist_ok=True)
    checkpoint_file = checkpoint_dir / "latest_checkpoint.pth"

    start_epoch = 0
    best_top1 = 0.0
    best_top5 = 0.0
    best_epoch = 0

    if checkpoint_file.exists():
        print(f"[恢复] 从 checkpoint 恢复: {checkpoint_file}")
        ckpt = torch.load(checkpoint_file, map_location=device, weights_only=False)
        model.load_state_dict(ckpt["model_state_dict"])
        optimizer.load_state_dict(ckpt["optimizer_state_dict"])
        scheduler.load_state_dict(ckpt["scheduler_state_dict"])
        if scaler is not None and "scaler_state_dict" in ckpt:
            scaler.load_state_dict(ckpt["scaler_state_dict"])
        start_epoch = ckpt["epoch"] + 1
        best_top1 = ckpt.get("best_top1", 0.0)
        best_top5 = ckpt.get("best_top5", 0.0)
        best_epoch = ckpt.get("best_epoch", 0)
        print(f"  从 epoch {start_epoch} 继续，best top-1: {best_top1:.4f}")

    # ── 训练循环 ──────────────────────────────────────────────────────────
    best_state_dict = None
    epoch_train_losses: list[float] = []  # 记录每 epoch 训练 loss
    training_history: list[dict] = []     # 记录每 epoch 完整指标

    print(f"\n[训练] 开始训练: epoch {start_epoch}-{args.epochs - 1}")

    for epoch in range(start_epoch, args.epochs):
        # 训练
        train_loss = train_one_epoch(
            model, train_loader, criterion, optimizer, device, use_amp, scaler,
            epoch=epoch,
        )

        # 验证
        val_loss, top1_acc, top5_acc = validate(
            model, val_loader, criterion, device
        )

        scheduler.step()
        epoch_train_losses.append(train_loss)

        # 记录 epoch 指标
        current_lr = optimizer.param_groups[0]["lr"]
        training_history.append({
            "epoch": epoch,
            "train_loss": round(train_loss, 4),
            "val_loss": round(val_loss, 4),
            "val_top1": round(top1_acc, 4),
            "val_top5": round(top5_acc, 4),
            "lr": round(current_lr, 8),
        })

        # 打印 epoch 结果
        best_mark = " ★" if top1_acc > best_top1 else ""
        print(
            f"  [epoch {epoch}] train_loss={train_loss:.4f} val_loss={val_loss:.4f} "
            f"val_top1={top1_acc:.4f} val_top5={top5_acc:.4f}{best_mark}",
            flush=True,
        )

        # 保存 best checkpoint（val top-1 最高）
        if top1_acc > best_top1:
            best_top1 = top1_acc
            best_top5 = top5_acc
            best_epoch = epoch
            best_state_dict = {
                k: v.cpu().clone() for k, v in model.state_dict().items()
            }

        # 保存 checkpoint 到 checkpoint-dir（SageMaker 自动同步到 S3）
        ckpt_data = {
            "epoch": epoch,
            "model_state_dict": model.state_dict(),
            "optimizer_state_dict": optimizer.state_dict(),
            "scheduler_state_dict": scheduler.state_dict(),
            "best_top1": best_top1,
            "best_top5": best_top5,
            "best_epoch": best_epoch,
        }
        if scaler is not None:
            ckpt_data["scaler_state_dict"] = scaler.state_dict()
        if best_state_dict is not None:
            ckpt_data["best_state_dict"] = best_state_dict
        torch.save(ckpt_data, checkpoint_file)

    # ── 训练完成后处理 ────────────────────────────────────────────────────
    output_dir = args.output_dir
    Path(output_dir).mkdir(parents=True, exist_ok=True)

    # 保存 best checkpoint 文件到 output_dir
    best_checkpoint_path = Path(output_dir) / "best_checkpoint.pth"
    if best_state_dict is None and checkpoint_file.exists():
        # 从 checkpoint 文件恢复 best_state_dict（断点续传场景）
        ckpt = torch.load(checkpoint_file, map_location="cpu", weights_only=False)
        best_state_dict = ckpt.get("best_state_dict")
    if best_state_dict is not None:
        torch.save(best_state_dict, best_checkpoint_path)
        print(f"\n[检查点] 已保存 best checkpoint: {best_checkpoint_path}")

    # 加载 best checkpoint
    if best_state_dict is not None:
        model.load_state_dict(best_state_dict)
    model.to(device)

    # 完整评估（调用 evaluator.evaluate）
    print("\n[评估] 加载 best checkpoint 进行完整评估...")
    report = evaluate(model, val_loader, class_names, device)
    save_evaluation_report(report, output_dir)
    print(f"  overall top-1: {report.top1_accuracy:.4f}")
    print(f"  overall top-5: {report.top5_accuracy:.4f}")

    # LoRA 模式下先 merge_lora()
    if args.lora:
        print("[LoRA] 合并 LoRA 权重到 backbone...")
        model.merge_lora()

    # 导出 .pt + class_names.json
    export_pytorch(
        model, backbone_config, class_names, output_dir,
        lora=args.lora, lora_rank=args.lora_rank,
    )
    export_class_names(class_names, output_dir)

    # 保存训练历史（每 epoch 的 loss/accuracy/lr）
    history_path = Path(output_dir) / "training_history.json"
    with open(history_path, "w") as f:
        json.dump(training_history, f, indent=2)
    print(f"[导出] 训练历史: {history_path}")

    # 可选 ONNX 导出
    if args.export_onnx:
        try:
            export_onnx(model, backbone_config, output_dir)
        except Exception as e:
            print(f"[ONNX] ⚠️ ONNX 导出失败: {e}")

    # 自动上传产物到 S3 models 目录（容器内直接上传，不依赖 --wait）
    if args.s3_model_prefix:
        try:
            import boto3 as _boto3
            from urllib.parse import urlparse
            parsed = urlparse(args.s3_model_prefix)
            s3_bucket = parsed.netloc
            s3_prefix = parsed.path.lstrip("/")
            s3 = _boto3.client("s3", region_name=os.environ.get("AWS_REGION"))
            uploaded = 0
            for fname in os.listdir(output_dir):
                fpath = os.path.join(output_dir, fname)
                if os.path.isfile(fpath) and fname != "best_checkpoint.pth":
                    dst_key = f"{s3_prefix}{fname}"
                    s3.upload_file(fpath, s3_bucket, dst_key)
                    size_mb = os.path.getsize(fpath) / (1024 * 1024)
                    print(f"[S3 上传] {fname} ({size_mb:.1f} MB) → s3://{s3_bucket}/{dst_key}")
                    uploaded += 1
            print(f"[S3 上传] 共 {uploaded} 个文件上传到 s3://{s3_bucket}/{s3_prefix}")
        except Exception as e:
            print(f"[S3 上传] ⚠️ 上传失败: {e}")

    # 打印训练摘要
    total_time = time.time() - start_time
    print(f"\n{'=' * 60}")
    print(f"[训练摘要]")
    print(f"  总 epoch:     {args.epochs}")
    print(f"  最佳 epoch:   {best_epoch}")
    print(f"  最佳 top-1:   {best_top1:.4f}")
    print(f"  最佳 top-5:   {best_top5:.4f}")
    print(f"  总耗时:       {total_time:.1f} 秒")
    print(f"{'=' * 60}")

    return {
        "best_epoch": best_epoch,
        "best_top1": best_top1,
        "best_top5": best_top5,
        "total_time": total_time,
        "output_dir": output_dir,
        "epoch_train_losses": epoch_train_losses,
    }


def main():
    """训练入口：解析参数并执行训练。"""
    args = parse_args()

    # 验证必要目录
    if not args.train_dir:
        print("错误: 未指定训练数据目录。请设置 --train-dir 或 SM_CHANNEL_TRAIN 环境变量。")
        sys.exit(1)
    if not args.val_dir:
        print("错误: 未指定验证数据目录。请设置 --val-dir 或 SM_CHANNEL_VAL 环境变量。")
        sys.exit(1)
    if not args.output_dir:
        print("错误: 未指定输出目录。请设置 --output-dir 或 SM_MODEL_DIR 环境变量。")
        sys.exit(1)

    train(args)


if __name__ == "__main__":
    main()
