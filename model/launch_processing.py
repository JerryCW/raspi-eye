"""SageMaker Processing Job 启动脚本。

用法：
    python model/launch_processing.py \\
        --s3-bucket my-bucket \\
        --s3-prefix bird-data \\
        --role arn:aws:iam::123456789012:role/SageMakerRole

    python model/launch_processing.py \\
        --s3-bucket my-bucket \\
        --wait
"""

import argparse
import os
import sys
import time


def main():
    parser = argparse.ArgumentParser(description="启动 SageMaker Processing Job")
    parser.add_argument("--s3-bucket", required=True, help="S3 桶名")
    parser.add_argument("--s3-prefix", default="bird-data", help="S3 路径前缀")
    parser.add_argument("--role", type=str, default=None,
                        help="SageMaker Execution Role ARN")
    parser.add_argument("--instance-type", default="ml.g4dn.xlarge",
                        help="GPU 实例类型")
    parser.add_argument("--wait", action="store_true", help="等待 Job 完成")
    args = parser.parse_args()

    # Role ARN：CLI 参数 > 环境变量
    role = args.role or os.environ.get("SAGEMAKER_ROLE_ARN")
    if not role:
        print("错误: 必须通过 --role 或 SAGEMAKER_ROLE_ARN 环境变量指定 Role ARN")
        sys.exit(1)

    from sagemaker.pytorch import PyTorchProcessor
    from sagemaker.processing import ProcessingInput, ProcessingOutput

    s3_base = f"s3://{args.s3_bucket}/{args.s3_prefix}"

    processor = PyTorchProcessor(
        framework_version="2.1",
        py_version="py310",
        role=role,
        instance_type=args.instance_type,
        instance_count=1,
        base_job_name="bird-feature-cleaning",
    )

    job_start = time.time()

    processor.run(
        code="clean_features.py",
        source_dir="model/",
        inputs=[
            ProcessingInput(
                source=f"{s3_base}/cleaned/",
                destination="/opt/ml/processing/input/cleaned/",
            ),
            ProcessingInput(
                source=f"{s3_base}/config/species.yaml",
                destination="/opt/ml/processing/input/config/",
            ),
        ],
        outputs=[
            ProcessingOutput(
                source="/opt/ml/processing/output/train/",
                destination=f"{s3_base}/train/",
            ),
            ProcessingOutput(
                source="/opt/ml/processing/output/val/",
                destination=f"{s3_base}/val/",
            ),
            ProcessingOutput(
                source="/opt/ml/processing/output/features/",
                destination=f"{s3_base}/features/",
            ),
            ProcessingOutput(
                source="/opt/ml/processing/output/report/",
                destination=f"{s3_base}/report/",
            ),
        ],
        arguments=[
            "--config", "/opt/ml/processing/input/config/species.yaml",
        ],
        wait=False,
    )

    # 获取 Job 信息
    job_name = processor.latest_job_name
    region = processor.sagemaker_session.boto_region_name

    print(f"Processing Job 已提交: {job_name}")
    print(f"CloudWatch Logs: https://{region}.console.aws.amazon.com/"
          f"cloudwatch/home?region={region}#logsV2:log-groups/"
          f"log-group/$252Faws$252Fsagemaker$252FProcessingJobs/"
          f"log-events/{job_name}")

    if args.wait:
        print("等待 Job 完成...")
        processor.latest_job.wait()
        elapsed = time.time() - job_start
        status = processor.latest_job.describe()["ProcessingJobStatus"]
        print(f"Job 状态: {status}")
        print(f"耗时: {elapsed:.0f} 秒")


if __name__ == "__main__":
    main()
