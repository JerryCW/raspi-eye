"""SageMaker Training Job 启动脚本（boto3 原生 API，script mode）。

使用 SageMaker 预构建容器的 script mode 部署训练代码：
将 model/ 目录打包为 sourcedir.tar.gz 上传到 S3，通过 HyperParameters 中的
sagemaker_program 和 sagemaker_submit_directory 让 Training Toolkit 自动
解压代码、安装 requirements.txt 依赖并执行训练脚本。

不设置 ContainerEntrypoint，保留容器默认入口（SageMaker Training Toolkit），
确保 CloudWatch Logs 管道正常工作。

Training Job 数据通道路径：
    /opt/ml/input/data/train/   ← S3 dataset/train/
    /opt/ml/input/data/val/     ← S3 dataset/val/
    /opt/ml/model/              ← 模型输出（自动打包上传到 S3）

用法：
    python model/launch_training.py \\
        --s3-bucket raspi-eye-model-data \\
        --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \\
        --backbone dinov3-vitl16 --wait
"""

import argparse
import os
import sys
import tarfile
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

import boto3


# PyTorch 2.6 GPU 预构建容器镜像 URI
PYTORCH_IMAGE_URIS = {
    "us-east-1": (
        "763104351884.dkr.ecr.us-east-1.amazonaws.com/"
        "pytorch-training:2.6.0-gpu-py312-cu126-ubuntu22.04-sagemaker"
    ),
    "ap-southeast-1": (
        "763104351884.dkr.ecr.ap-southeast-1.amazonaws.com/"
        "pytorch-training:2.6.0-gpu-py312-cu126-ubuntu22.04-sagemaker"
    ),
}


def get_image_uri(region: str) -> str:
    """获取 PyTorch GPU 预构建容器镜像 URI。"""
    if region in PYTORCH_IMAGE_URIS:
        return PYTORCH_IMAGE_URIS[region]
    return (
        f"763104351884.dkr.ecr.{region}.amazonaws.com/"
        "pytorch-training:2.6.0-gpu-py312-cu126-ubuntu22.04-sagemaker"
    )


def get_hf_token(region: str | None) -> str | None:
    """从 Secrets Manager 获取 HuggingFace token。"""
    try:
        client = boto3.client(
            "secretsmanager",
            region_name=region or os.environ.get("AWS_DEFAULT_REGION"),
        )
        resp = client.get_secret_value(SecretId="raspi-eye/huggingface-token")
        print("HF_TOKEN 从 Secrets Manager 获取")
        return resp["SecretString"]
    except Exception as e:
        print(f"警告: 无法从 Secrets Manager 获取 HF_TOKEN: {e}")
        return None


def _tar_filter(tarinfo: tarfile.TarInfo) -> tarfile.TarInfo | None:
    """打包过滤器：排除 data/、tests/、__pycache__/、.pytest_cache/、*.pyc。"""
    name = tarinfo.name
    # 排除目录
    excluded_dirs = ("data/", "tests/", "__pycache__/", ".pytest_cache/")
    for excl in excluded_dirs:
        if f"/{excl}" in name + "/" or name.startswith(excl):
            return None
    # 排除 .pyc 文件
    if name.endswith(".pyc"):
        return None
    return tarinfo


def pack_sourcedir(model_dir: str) -> str:
    """将 model/ 目录打包为 sourcedir.tar.gz。

    打包规则：tar.add("model/", arcname=".") 使得解压后
    training/train.py 位于根目录下的 training/train.py。

    Args:
        model_dir: model/ 目录的路径

    Returns:
        tar.gz 文件的临时路径
    """
    tar_path = os.path.join(tempfile.gettempdir(), "sourcedir.tar.gz")
    with tarfile.open(tar_path, "w:gz") as tar:
        tar.add(model_dir, arcname=".", filter=_tar_filter)
        # Training Toolkit 在解压后的根目录找 requirements.txt，
        # 但实际文件在 training/requirements.txt，需要额外添加一份到根目录
        req_path = os.path.join(model_dir, "training", "requirements.txt")
        if os.path.exists(req_path):
            tar.add(req_path, arcname="requirements.txt")

    size_mb = os.path.getsize(tar_path) / (1024 * 1024)
    print(f"sourcedir.tar.gz 打包完成: {size_mb:.1f} MB")
    return tar_path


def copy_model_artifacts(s3_client, bucket, job_name, backbone, lora):
    """将模型产物从 Job 输出解压并复制到 models/{backbone}/ 目录。

    SageMaker 把 /opt/ml/model/ 打包成 model.tar.gz 上传，
    需要下载 → 解压 → 逐文件上传到目标目录。
    """
    target_dir = f"{backbone}-lora" if lora else backbone
    tar_s3_key = f"training/jobs/{job_name}/output/model.tar.gz"
    target_prefix = f"training/models/{target_dir}/"

    print(f"\n解压模型产物: s3://{bucket}/{tar_s3_key}")
    print(f"  → s3://{bucket}/{target_prefix}")

    # 下载 model.tar.gz 到临时目录
    tar_local = os.path.join(tempfile.gettempdir(), "model_output.tar.gz")
    try:
        s3_client.download_file(bucket, tar_s3_key, tar_local)
    except Exception as e:
        print(f"  警告: 无法下载 model.tar.gz: {e}")
        return

    # 解压并逐文件上传
    extract_dir = os.path.join(tempfile.gettempdir(), "model_output")
    os.makedirs(extract_dir, exist_ok=True)
    with tarfile.open(tar_local, "r:gz") as tar:
        tar.extractall(extract_dir)

    copied = 0
    for root, _dirs, files in os.walk(extract_dir):
        for fname in files:
            local_path = os.path.join(root, fname)
            relative = os.path.relpath(local_path, extract_dir)
            dst_key = f"{target_prefix}{relative}"
            s3_client.upload_file(local_path, bucket, dst_key)
            size_mb = os.path.getsize(local_path) / (1024 * 1024)
            print(f"  上传: {relative} ({size_mb:.1f} MB)")
            copied += 1

    # 清理临时文件
    os.remove(tar_local)
    import shutil
    shutil.rmtree(extract_dir, ignore_errors=True)

    print(f"  共上传 {copied} 个文件到 s3://{bucket}/{target_prefix}")


def main():
    parser = argparse.ArgumentParser(description="启动 SageMaker Training Job")
    parser.add_argument("--s3-bucket", required=True, help="S3 桶名")
    parser.add_argument("--role", type=str, default=None,
                        help="SageMaker Role ARN")
    parser.add_argument("--backbone", default="dinov3-vitl16")
    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--weight-decay", type=float, default=1e-4)
    parser.add_argument("--instance-type", default="ml.g4dn.xlarge")
    parser.add_argument("--region", type=str, default=None)
    parser.add_argument("--wait", action="store_true")
    parser.add_argument("--export-onnx", action="store_true")
    parser.add_argument("--lora", action="store_true")
    parser.add_argument("--lora-rank", type=int, default=8)
    args = parser.parse_args()

    role = args.role or os.environ.get("SAGEMAKER_ROLE_ARN")
    if not role:
        print("错误: 必须通过 --role 或 SAGEMAKER_ROLE_ARN 指定 Role ARN")
        sys.exit(1)

    region = args.region or os.environ.get("AWS_DEFAULT_REGION")
    sm_client = boto3.client("sagemaker", region_name=region)
    region = sm_client.meta.region_name

    image_uri = get_image_uri(region)

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    mode = "lora" if args.lora else "lp"  # lp = linear probe
    job_name = f"bird-v1-{args.backbone}-{mode}-{timestamp}"

    bucket = args.s3_bucket
    s3_train = f"s3://{bucket}/dataset/train/"
    s3_val = f"s3://{bucket}/dataset/val/"
    s3_output = f"s3://{bucket}/training/jobs/"

    print(f"Region: {region}")
    print(f"Image: {image_uri}")
    print(f"Instance: {args.instance_type}")
    print(f"Backbone: {args.backbone}")
    print(f"LoRA: {args.lora}" + (f" (rank={args.lora_rank})" if args.lora else ""))
    print(f"Job: {job_name}")

    # ── 打包 model/ 为 sourcedir.tar.gz 并上传到 S3 ──────────────────────
    model_dir = str(Path(__file__).resolve().parent)  # model/ 目录
    tar_path = pack_sourcedir(model_dir)

    s3_client = boto3.client("s3", region_name=region)
    tar_s3_key = f"pipeline/sourcedir-{timestamp}.tar.gz"
    s3_client.upload_file(tar_path, bucket, tar_s3_key)
    submit_dir = f"s3://{bucket}/{tar_s3_key}"
    print(f"sourcedir 已上传: {submit_dir}")

    # 清理临时文件
    os.remove(tar_path)

    # ── 构建 HyperParameters ─────────────────────────────────────────────
    # SageMaker Training Toolkit 把 HyperParameters 的 key 原样作为 --key 传给脚本，
    # 所以 key 必须和 argparse 定义的参数名一致（连字符，不是下划线）
    hyperparameters = {
        "sagemaker_program": "training/train.py",
        "sagemaker_submit_directory": submit_dir,
        "backbone": args.backbone,
        "epochs": str(args.epochs),
        "batch-size": str(args.batch_size),
        "lr": str(args.lr),
        "weight-decay": str(args.weight_decay),
    }
    if args.export_onnx:
        hyperparameters["export-onnx"] = "true"
    if args.lora:
        hyperparameters["lora"] = "true"
        hyperparameters["lora-rank"] = str(args.lora_rank)

    # 容器内训练完成后自动上传产物到 S3 models 目录
    target_dir = f"{args.backbone}-lora" if args.lora else args.backbone
    hyperparameters["s3-model-prefix"] = f"s3://{bucket}/training/models/{target_dir}/"

    # HF_TOKEN 不通过环境变量传入（会明文显示在 SageMaker 控制台），
    # 改由 train.py 在容器内从 Secrets Manager 获取
    environment = {}

    job_start = time.time()

    # ── 创建 Training Job ────────────────────────────────────────────────
    sm_client.create_training_job(
        TrainingJobName=job_name,
        AlgorithmSpecification={
            "TrainingImage": image_uri,
            "TrainingInputMode": "File",
            # 不设置 ContainerEntrypoint，保留容器默认入口（Training Toolkit）
        },
        RoleArn=role,
        InputDataConfig=[
            {
                "ChannelName": "train",
                "DataSource": {
                    "S3DataSource": {
                        "S3DataType": "S3Prefix",
                        "S3Uri": s3_train,
                        "S3DataDistributionType": "FullyReplicated",
                    }
                },
            },
            {
                "ChannelName": "val",
                "DataSource": {
                    "S3DataSource": {
                        "S3DataType": "S3Prefix",
                        "S3Uri": s3_val,
                        "S3DataDistributionType": "FullyReplicated",
                    }
                },
            },
            # 不再需要 code 通道，代码通过 sourcedir.tar.gz 部署
        ],
        OutputDataConfig={"S3OutputPath": s3_output},
        ResourceConfig={
            "InstanceType": args.instance_type,
            "InstanceCount": 1,
            "VolumeSizeInGB": 50,
        },
        HyperParameters=hyperparameters,
        CheckpointConfig={
            "S3Uri": f"s3://{bucket}/training/checkpoints/{job_name}/",
            "LocalPath": "/opt/ml/checkpoints",
        },
        StoppingCondition={"MaxRuntimeInSeconds": 43200},  # 12 小时
        Environment=environment,
    )

    print(f"\nTraining Job 已提交: {job_name}")
    print(f"CloudWatch Logs: https://{region}.console.aws.amazon.com/"
          f"cloudwatch/home?region={region}#logsV2:log-groups/"
          f"log-group/$252Faws$252Fsagemaker$252FTrainingJobs/"
          f"log-events/{job_name}")
    print(f"\n查看状态: aws sagemaker describe-training-job "
          f"--training-job-name {job_name} --region {region} "
          f"--query TrainingJobStatus")

    if args.wait:
        print("\n等待 Job 完成...")
        while True:
            resp = sm_client.describe_training_job(TrainingJobName=job_name)
            status = resp["TrainingJobStatus"]
            if status in ("Completed", "Failed", "Stopped"):
                break
            secondary = resp.get("SecondaryStatus", "")
            elapsed = time.time() - job_start
            print(f"  {status} ({secondary}) - {elapsed:.0f}s", end="\r")
            time.sleep(30)

        print()
        elapsed = time.time() - job_start
        print(f"Job 状态: {status}")
        print(f"耗时: {elapsed:.0f} 秒")

        if status == "Failed":
            reason = resp.get("FailureReason", "unknown")
            print(f"失败原因: {reason}")
            sys.exit(1)

        if status == "Completed":
            copy_model_artifacts(
                s3_client, bucket, job_name, args.backbone, args.lora
            )


if __name__ == "__main__":
    main()
