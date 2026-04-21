"""SageMaker Processing Job 启动脚本（boto3 原生 API，自动打包代码）。

自动将 model/ 目录打包为 sourcedir.tar.gz 上传到 S3，
ContainerEntrypoint 在容器内解压代码、安装依赖并执行清洗脚本。
不再需要手动 aws s3 sync 同步代码。

S3 目录结构：
    s3://{bucket}/
    ├── bird-data/          # 源数据
    │   ├── cleaned/        # Spec 27 像素级清洗后（按物种子目录）
    │   └── config/         # species.yaml
    ├── pipeline/           # 清洗管道
    │   ├── sourcedir/      # 自动打包的代码 tar.gz
    │   ├── cropped/        # YOLO 裁切缓存（断点续传）
    │   ├── features/       # DINOv3 特征向量 .npy
    │   └── report/         # cleaning_report.json
    ├── dataset/            # 训练就绪（→ Spec 29 训练输入）
    │   ├── train/{species} # ImageFolder 格式
    │   └── val/{species}
    ├── training/           # Spec 29 训练产出
    └── endpoint/           # Spec 17 推理部署

用法：
    python model/launch_processing.py \\
        --s3-bucket raspi-eye-model-data \\
        --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role

    python model/launch_processing.py \\
        --s3-bucket raspi-eye-model-data \\
        --species "Passer montanus" --wait
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


def _tar_filter(tarinfo: tarfile.TarInfo) -> tarfile.TarInfo | None:
    """打包过滤器：排除 data/、tests/、__pycache__/、.pytest_cache/、models/、samples/、*.pyc。"""
    name = tarinfo.name
    excluded_dirs = ("data/", "tests/", "__pycache__/", ".pytest_cache/",
                     "models/", "samples/", "output/")
    for excl in excluded_dirs:
        if f"/{excl}" in name + "/" or name.startswith(excl):
            return None
    if name.endswith(".pyc"):
        return None
    return tarinfo


def pack_sourcedir(model_dir: str) -> str:
    """将 model/ 目录打包为 sourcedir.tar.gz。

    打包规则：tar.add("model/", arcname=".") 使得解压后
    cleaning/clean_features.py 位于根目录下的 cleaning/clean_features.py。

    同时将 cleaning/requirements.txt 额外添加到根目录，
    方便 ContainerEntrypoint 中统一 pip install。
    """
    tar_path = os.path.join(tempfile.gettempdir(), "sourcedir-processing.tar.gz")
    with tarfile.open(tar_path, "w:gz") as tar:
        tar.add(model_dir, arcname=".", filter=_tar_filter)
        # 将 cleaning/requirements.txt 额外添加到根目录
        req_path = os.path.join(model_dir, "cleaning", "requirements.txt")
        if os.path.exists(req_path):
            tar.add(req_path, arcname="requirements.txt")

    size_mb = os.path.getsize(tar_path) / (1024 * 1024)
    print(f"sourcedir.tar.gz 打包完成: {size_mb:.1f} MB")
    return tar_path


def main():
    parser = argparse.ArgumentParser(description="启动 SageMaker Processing Job")
    parser.add_argument("--s3-bucket", required=True, help="S3 桶名")
    parser.add_argument("--role", type=str, default=None,
                        help="SageMaker Execution Role ARN")
    parser.add_argument("--instance-type", default="ml.g4dn.xlarge",
                        help="GPU 实例类型")
    parser.add_argument("--region", type=str, default=None,
                        help="AWS Region（默认从 AWS_DEFAULT_REGION 或 boto3 配置获取）")
    parser.add_argument("--wait", action="store_true", help="等待 Job 完成")
    parser.add_argument("--species", type=str, default=None,
                        help="指定单个物种（用于测试），传给 clean_features.py --species")
    args = parser.parse_args()

    # Role ARN：CLI 参数 > 环境变量
    role = args.role or os.environ.get("SAGEMAKER_ROLE_ARN")
    if not role:
        print("错误: 必须通过 --role 或 SAGEMAKER_ROLE_ARN 环境变量指定 Role ARN")
        sys.exit(1)

    # Region
    region = args.region or os.environ.get("AWS_DEFAULT_REGION")
    sm_client = boto3.client("sagemaker", region_name=region)
    region = sm_client.meta.region_name

    image_uri = get_image_uri(region)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    job_name = f"bird-feature-cleaning-{timestamp}"

    bucket = args.s3_bucket
    s3_data = f"s3://{bucket}/bird-data"
    s3_pipeline = f"s3://{bucket}/pipeline"
    s3_dataset = f"s3://{bucket}/dataset"

    print(f"Region: {region}")
    print(f"Image: {image_uri}")
    print(f"Instance: {args.instance_type}")
    print(f"Job: {job_name}")

    # ── 打包 model/ 为 sourcedir.tar.gz 并上传到 S3 ──────────────────────
    model_dir = str(Path(__file__).resolve().parent)  # model/ 目录
    tar_path = pack_sourcedir(model_dir)

    s3_client = boto3.client("s3", region_name=region)
    tar_s3_key = f"pipeline/sourcedir-processing-{timestamp}.tar.gz"
    s3_client.upload_file(tar_path, bucket, tar_s3_key)
    submit_dir = f"s3://{bucket}/{tar_s3_key}"
    print(f"sourcedir 已上传: {submit_dir}")

    # 清理临时文件
    os.remove(tar_path)

    # ── HF_TOKEN：从 Secrets Manager 获取 ────────────────────────────────
    hf_token = os.environ.get("HF_TOKEN")
    if not hf_token:
        try:
            sm_secret = boto3.client("secretsmanager", region_name=region)
            resp = sm_secret.get_secret_value(SecretId="raspi-eye/huggingface-token")
            hf_token = resp["SecretString"]
            print("HF_TOKEN 从 Secrets Manager 获取")
        except Exception:
            pass
    if not hf_token:
        print("警告: 未设置 HF_TOKEN，gated model（如 DINOv3）可能无法下载")

    # ── 构建 clean_features.py 的命令行参数 ───────────────────────────────
    clean_args = "--config /opt/ml/code/config/species.yaml"
    if args.species:
        clean_args += f" --species '{args.species}'"

    # ── 检查 S3 上是否已有 cropped 数据（断点续传）─────────────────────────
    cropped_prefix = "pipeline/cropped/"
    has_cropped = False
    try:
        resp = s3_client.list_objects_v2(
            Bucket=bucket, Prefix=cropped_prefix, MaxKeys=1
        )
        has_cropped = resp.get("KeyCount", 0) > 0
    except Exception:
        pass

    # ── 构建 ProcessingInputs ─────────────────────────────────────────────
    processing_inputs = [
        {
            "InputName": "cleaned",
            "S3Input": {
                "S3Uri": f"{s3_data}/cleaned/",
                "LocalPath": "/opt/ml/processing/input/cleaned",
                "S3DataType": "S3Prefix",
                "S3InputMode": "File",
            },
        },
        {
            "InputName": "code",
            "S3Input": {
                "S3Uri": submit_dir,
                "LocalPath": "/opt/ml/processing/input/code",
                "S3DataType": "S3Prefix",
                "S3InputMode": "File",
            },
        },
        # config/species.yaml 已随 sourcedir.tar.gz 打包，不再需要单独的 S3 输入通道
    ]
    if has_cropped:
        processing_inputs.append({
            "InputName": "cropped-cache",
            "S3Input": {
                "S3Uri": f"{s3_pipeline}/cropped/",
                "LocalPath": "/opt/ml/processing/input/cropped",
                "S3DataType": "S3Prefix",
                "S3InputMode": "File",
                "S3DataDistributionType": "FullyReplicated",
            },
        })
        print("S3 已有 cropped 数据，将作为输入通道下载（已裁切物种自动跳过）")
    else:
        print("S3 无 cropped 数据，将从头裁切所有物种")

    job_start = time.time()

    # ── ContainerEntrypoint：解压代码 → 安装依赖 → 执行清洗 ──────────────
    # sourcedir.tar.gz 通过 code 输入通道下载到 /opt/ml/processing/input/code/
    entrypoint_script = (
        "mkdir -p /opt/ml/code && "
        "tar xzf /opt/ml/processing/input/code/*.tar.gz -C /opt/ml/code && "
        "pip install -q -r /opt/ml/code/requirements.txt && "
        f"cd /opt/ml/code && python3 cleaning/clean_features.py {clean_args}"
    )

    sm_client.create_processing_job(
        ProcessingJobName=job_name,
        ProcessingResources={
            "ClusterConfig": {
                "InstanceCount": 1,
                "InstanceType": args.instance_type,
                "VolumeSizeInGB": 50,
            }
        },
        AppSpecification={
            "ImageUri": image_uri,
            "ContainerEntrypoint": ["bash", "-c", entrypoint_script],
        },
        Environment={
            **({"HF_TOKEN": hf_token} if hf_token else {}),
        },
        RoleArn=role,
        ProcessingInputs=processing_inputs,
        ProcessingOutputConfig={
            "Outputs": [
                {
                    "OutputName": "train",
                    "S3Output": {
                        "S3Uri": f"{s3_dataset}/train/",
                        "LocalPath": "/opt/ml/processing/output/train",
                        "S3UploadMode": "EndOfJob",
                    },
                },
                {
                    "OutputName": "val",
                    "S3Output": {
                        "S3Uri": f"{s3_dataset}/val/",
                        "LocalPath": "/opt/ml/processing/output/val",
                        "S3UploadMode": "EndOfJob",
                    },
                },
                {
                    "OutputName": "features",
                    "S3Output": {
                        "S3Uri": f"{s3_pipeline}/features/",
                        "LocalPath": "/opt/ml/processing/output/features",
                        "S3UploadMode": "EndOfJob",
                    },
                },
                {
                    "OutputName": "report",
                    "S3Output": {
                        "S3Uri": f"{s3_pipeline}/report/",
                        "LocalPath": "/opt/ml/processing/output/report",
                        "S3UploadMode": "EndOfJob",
                    },
                },
                {
                    "OutputName": "cropped",
                    "S3Output": {
                        "S3Uri": f"{s3_pipeline}/cropped/",
                        "LocalPath": "/opt/ml/processing/output/cropped",
                        "S3UploadMode": "Continuous",
                    },
                },
            ],
        },
        StoppingCondition={
            "MaxRuntimeInSeconds": 7200,  # 2 小时超时
        },
    )

    print(f"\nProcessing Job 已提交: {job_name}")
    print(f"CloudWatch Logs: https://{region}.console.aws.amazon.com/"
          f"cloudwatch/home?region={region}#logsV2:log-groups/"
          f"log-group/$252Faws$252Fsagemaker$252FProcessingJobs/"
          f"log-events/{job_name}")
    print(f"\n查看状态: aws sagemaker describe-processing-job "
          f"--processing-job-name {job_name} --region {region} "
          f"--query ProcessingJobStatus")

    if args.wait:
        print("\n等待 Job 完成...")
        waiter = sm_client.get_waiter("processing_job_completed_or_stopped")
        try:
            waiter.wait(
                ProcessingJobName=job_name,
                WaiterConfig={"Delay": 30, "MaxAttempts": 240},
            )
        except Exception as e:
            print(f"等待异常: {e}")

        resp = sm_client.describe_processing_job(ProcessingJobName=job_name)
        status = resp["ProcessingJobStatus"]
        elapsed = time.time() - job_start
        print(f"Job 状态: {status}")
        print(f"耗时: {elapsed:.0f} 秒")

        if status == "Failed":
            reason = resp.get("FailureReason", "unknown")
            print(f"失败原因: {reason}")
            sys.exit(1)


if __name__ == "__main__":
    main()
