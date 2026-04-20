"""SageMaker Processing Job 启动脚本（boto3 原生 API，兼容 sagemaker SDK v2/v3）。

用法：
    python model/launch_processing.py \\
        --s3-bucket raspi-eye-model-data \\
        --s3-prefix bird-data \\
        --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role

    python model/launch_processing.py \\
        --s3-bucket raspi-eye-model-data \\
        --wait
"""

import argparse
import os
import sys
import time
from datetime import datetime, timezone

import boto3


# PyTorch 2.1 GPU 预构建容器镜像 URI 格式
# 参考: https://github.com/aws/deep-learning-containers/blob/master/available_images.md
PYTORCH_IMAGE_URIS = {
    "us-east-1": "763104351884.dkr.ecr.us-east-1.amazonaws.com/pytorch-training:2.1.0-gpu-py310-cu121-ubuntu20.04-sagemaker",
    "ap-southeast-1": "763104351884.dkr.ecr.ap-southeast-1.amazonaws.com/pytorch-training:2.1.0-gpu-py310-cu121-ubuntu20.04-sagemaker",
}


def get_image_uri(region: str) -> str:
    """获取 PyTorch GPU 预构建容器镜像 URI。"""
    if region in PYTORCH_IMAGE_URIS:
        return PYTORCH_IMAGE_URIS[region]
    # 通用格式回退
    return f"763104351884.dkr.ecr.{region}.amazonaws.com/pytorch-training:2.1.0-gpu-py310-cu121-ubuntu20.04-sagemaker"


def main():
    parser = argparse.ArgumentParser(description="启动 SageMaker Processing Job")
    parser.add_argument("--s3-bucket", required=True, help="S3 桶名")
    parser.add_argument("--s3-prefix", default="bird-data", help="S3 路径前缀")
    parser.add_argument("--role", type=str, default=None,
                        help="SageMaker Execution Role ARN")
    parser.add_argument("--instance-type", default="ml.g4dn.xlarge",
                        help="GPU 实例类型")
    parser.add_argument("--region", type=str, default=None,
                        help="AWS Region（默认从 AWS_DEFAULT_REGION 或 boto3 配置获取）")
    parser.add_argument("--wait", action="store_true", help="等待 Job 完成")
    args = parser.parse_args()

    # Role ARN：CLI 参数 > 环境变量
    role = args.role or os.environ.get("SAGEMAKER_ROLE_ARN")
    if not role:
        print("错误: 必须通过 --role 或 SAGEMAKER_ROLE_ARN 环境变量指定 Role ARN")
        sys.exit(1)

    # Region
    region = args.region or os.environ.get("AWS_DEFAULT_REGION")
    sm_client = boto3.client("sagemaker", region_name=region)
    region = sm_client.meta.region_name  # 确保有值

    s3_base = f"s3://{args.s3_bucket}/{args.s3_prefix}"
    image_uri = get_image_uri(region)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    job_name = f"bird-feature-cleaning-{timestamp}"

    print(f"Region: {region}")
    print(f"Image: {image_uri}")
    print(f"Instance: {args.instance_type}")
    print(f"S3: {s3_base}")
    print(f"Job: {job_name}")

    job_start = time.time()

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
            "ContainerEntrypoint": [
                "bash", "-c",
                "pip install ultralytics scikit-learn scipy imagehash && "
                "python3 /opt/ml/processing/input/code/clean_features.py "
                "--config /opt/ml/processing/input/config/species.yaml"
            ],
        },
        RoleArn=role,
        ProcessingInputs=[
            {
                "InputName": "cleaned",
                "S3Input": {
                    "S3Uri": f"{s3_base}/cleaned/",
                    "LocalPath": "/opt/ml/processing/input/cleaned",
                    "S3DataType": "S3Prefix",
                    "S3InputMode": "File",
                },
            },
            {
                "InputName": "config",
                "S3Input": {
                    "S3Uri": f"{s3_base}/config/",
                    "LocalPath": "/opt/ml/processing/input/config",
                    "S3DataType": "S3Prefix",
                    "S3InputMode": "File",
                },
            },
            {
                "InputName": "code",
                "S3Input": {
                    "S3Uri": f"{s3_base}/code/",
                    "LocalPath": "/opt/ml/processing/input/code",
                    "S3DataType": "S3Prefix",
                    "S3InputMode": "File",
                },
            },
        ],
        ProcessingOutputConfig={
            "Outputs": [
                {
                    "OutputName": "train",
                    "S3Output": {
                        "S3Uri": f"{s3_base}/train/",
                        "LocalPath": "/opt/ml/processing/output/train",
                        "S3UploadMode": "EndOfJob",
                    },
                },
                {
                    "OutputName": "val",
                    "S3Output": {
                        "S3Uri": f"{s3_base}/val/",
                        "LocalPath": "/opt/ml/processing/output/val",
                        "S3UploadMode": "EndOfJob",
                    },
                },
                {
                    "OutputName": "features",
                    "S3Output": {
                        "S3Uri": f"{s3_base}/features/",
                        "LocalPath": "/opt/ml/processing/output/features",
                        "S3UploadMode": "EndOfJob",
                    },
                },
                {
                    "OutputName": "report",
                    "S3Output": {
                        "S3Uri": f"{s3_base}/report/",
                        "LocalPath": "/opt/ml/processing/output/report",
                        "S3UploadMode": "EndOfJob",
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
                WaiterConfig={"Delay": 30, "MaxAttempts": 240},  # 最多等 2 小时
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
