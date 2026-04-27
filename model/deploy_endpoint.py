#!/usr/bin/env python3
"""SageMaker Serverless Endpoint 部署脚本。

使用 boto3 三步部署：create_model → create_endpoint_config → create_endpoint。
支持创建、更新、删除、等待和测试 endpoint。

用法示例：
    # 部署（含打包上传）
    python model/deploy_endpoint.py \
        --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
        --wait --test

    # 跳过打包，仅部署
    python model/deploy_endpoint.py \
        --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
        --skip-package --wait

    # 更新现有 endpoint
    python model/deploy_endpoint.py \
        --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role \
        --update --wait

    # 删除 endpoint
    python model/deploy_endpoint.py --delete

    # 测试已部署的 endpoint
    python model/deploy_endpoint.py --test --skip-package
"""

import argparse
import glob
import json
import os
import sys
import time

import boto3
from botocore.exceptions import ClientError

# PyTorch 2.6 CPU 预构建推理容器镜像 URI（按区域）
PYTORCH_INFERENCE_IMAGE_URIS = {
    "ap-southeast-1": (
        "763104351884.dkr.ecr.ap-southeast-1.amazonaws.com/"
        "pytorch-inference:2.6.0-cpu-py312-ubuntu22.04-sagemaker"
    ),
}


def _fetch_hf_token() -> str | None:
    """从 Secrets Manager (us-east-1) 获取 HF_TOKEN，失败时从环境变量回退。"""
    try:
        sm_client = boto3.client("secretsmanager", region_name="us-east-1")
        response = sm_client.get_secret_value(SecretId="raspi-eye/huggingface-token")
        secret = response["SecretString"]
        try:
            parsed = json.loads(secret)
            token = parsed.get("HF_TOKEN", secret)
        except (json.JSONDecodeError, TypeError):
            token = secret
        print("从 Secrets Manager 获取 HF_TOKEN 成功")
        return token
    except Exception as e:
        print(f"Secrets Manager 获取 HF_TOKEN 失败: {e}，回退到环境变量")
        return os.environ.get("HF_TOKEN")


def get_image_uri(region: str) -> str:
    """获取指定区域的 PyTorch 推理容器镜像 URI。"""
    if region not in PYTORCH_INFERENCE_IMAGE_URIS:
        print(f"错误: 区域 {region} 暂不支持，可用区域: {list(PYTORCH_INFERENCE_IMAGE_URIS.keys())}")
        sys.exit(1)
    return PYTORCH_INFERENCE_IMAGE_URIS[region]


def endpoint_exists(sm_client, endpoint_name: str) -> bool:
    """检查 endpoint 是否已存在。"""
    try:
        sm_client.describe_endpoint(EndpointName=endpoint_name)
        return True
    except ClientError as e:
        if e.response["Error"]["Code"] == "ValidationException":
            return False
        raise


def deploy_endpoint(
    sm_client,
    endpoint_name: str,
    role: str,
    model_data_url: str,
    region: str,
    instance_type: str,
    instance_count: int,
    serverless: bool = False,
    memory_size: int = 6144,
    max_concurrency: int = 5,
) -> None:
    """boto3 三步部署：create_model → create_endpoint_config → create_endpoint。"""
    image_uri = get_image_uri(region)
    config_name = f"{endpoint_name}-config"

    # 构建环境变量
    env = {
        "SAGEMAKER_PROGRAM": "inference.py",
        "SAGEMAKER_SUBMIT_DIRECTORY": "/opt/ml/model/code",
        # 大模型加载需要更长时间（默认 60s 不够 1.2GB DINOv3）
        "SAGEMAKER_MODEL_SERVER_TIMEOUT": "300",
    }

    # 从 Secrets Manager 获取 HF_TOKEN 注入容器环境变量
    hf_token = _fetch_hf_token()
    if hf_token:
        env["HF_TOKEN"] = hf_token

    # Step 1: Create Model
    print(f"\n[1/3] 创建 SageMaker Model: {endpoint_name}")
    sm_client.create_model(
        ModelName=endpoint_name,
        ExecutionRoleArn=role,
        PrimaryContainer={
            "Image": image_uri,
            "ModelDataUrl": model_data_url,
            "Environment": env,
        },
    )
    print(f"  Model 创建成功: {endpoint_name}")

    # Step 2: Create Endpoint Config
    print(f"\n[2/3] 创建 EndpointConfig: {config_name}")
    if serverless:
        variant = {
            "ModelName": endpoint_name,
            "VariantName": "AllTraffic",
            "ServerlessConfig": {
                "MemorySizeInMB": memory_size,
                "MaxConcurrency": max_concurrency,
            },
        }
        print(f"  Serverless 配置: MemorySize={memory_size}MB, MaxConcurrency={max_concurrency}")
    else:
        variant = {
            "ModelName": endpoint_name,
            "VariantName": "AllTraffic",
            "InstanceType": instance_type,
            "InitialInstanceCount": instance_count,
            "ModelDataDownloadTimeoutInSeconds": 600,
            "ContainerStartupHealthCheckTimeoutInSeconds": 600,
        }
        print(f"  Real-time 配置: InstanceType={instance_type}, Count={instance_count}")
    sm_client.create_endpoint_config(
        EndpointConfigName=config_name,
        ProductionVariants=[variant],
    )
    print(f"  EndpointConfig 创建成功: {config_name}")

    # Step 3: Create Endpoint
    print(f"\n[3/3] 创建 Endpoint: {endpoint_name}")
    sm_client.create_endpoint(
        EndpointName=endpoint_name,
        EndpointConfigName=config_name,
    )
    print(f"  Endpoint 创建中: {endpoint_name}")


def update_endpoint(
    sm_client,
    endpoint_name: str,
    role: str,
    model_data_url: str,
    region: str,
    instance_type: str,
    instance_count: int,
    serverless: bool = False,
    memory_size: int = 6144,
    max_concurrency: int = 5,
) -> None:
    """更新现有 endpoint：创建新 Model + 新 EndpointConfig（带时间戳后缀）→ update_endpoint。"""
    image_uri = get_image_uri(region)
    timestamp = time.strftime("%Y%m%d%H%M%S")
    new_model_name = f"{endpoint_name}-{timestamp}"
    new_config_name = f"{endpoint_name}-config-{timestamp}"

    # 构建环境变量（与 deploy_endpoint 一致）
    env = {
        "SAGEMAKER_PROGRAM": "inference.py",
        "SAGEMAKER_SUBMIT_DIRECTORY": "/opt/ml/model/code",
        "SAGEMAKER_MODEL_SERVER_TIMEOUT": "300",
    }
    hf_token = _fetch_hf_token()
    if hf_token:
        env["HF_TOKEN"] = hf_token

    # 创建新 Model
    print(f"\n[1/3] 创建新 Model: {new_model_name}")
    sm_client.create_model(
        ModelName=new_model_name,
        ExecutionRoleArn=role,
        PrimaryContainer={
            "Image": image_uri,
            "ModelDataUrl": model_data_url,
            "Environment": env,
        },
    )
    print(f"  Model 创建成功: {new_model_name}")

    # 创建新 EndpointConfig（支持 Serverless + Real-time 双模式）
    print(f"\n[2/3] 创建新 EndpointConfig: {new_config_name}")
    if serverless:
        variant = {
            "ModelName": new_model_name,
            "VariantName": "AllTraffic",
            "ServerlessConfig": {
                "MemorySizeInMB": memory_size,
                "MaxConcurrency": max_concurrency,
            },
        }
        print(f"  Serverless 配置: MemorySize={memory_size}MB, MaxConcurrency={max_concurrency}")
    else:
        variant = {
            "ModelName": new_model_name,
            "VariantName": "AllTraffic",
            "InstanceType": instance_type,
            "InitialInstanceCount": instance_count,
            "ModelDataDownloadTimeoutInSeconds": 600,
            "ContainerStartupHealthCheckTimeoutInSeconds": 600,
        }
        print(f"  Real-time 配置: InstanceType={instance_type}, Count={instance_count}")
    sm_client.create_endpoint_config(
        EndpointConfigName=new_config_name,
        ProductionVariants=[variant],
    )
    print(f"  EndpointConfig 创建成功: {new_config_name}")

    # 更新 Endpoint
    print(f"\n[3/3] 更新 Endpoint: {endpoint_name}")
    sm_client.update_endpoint(
        EndpointName=endpoint_name,
        EndpointConfigName=new_config_name,
    )
    print(f"  Endpoint 更新中: {endpoint_name}")


def delete_endpoint(sm_client, endpoint_name: str) -> None:
    """删除 Endpoint → EndpointConfig → Model。"""
    config_name = f"{endpoint_name}-config"

    # 删除 Endpoint
    print(f"\n[1/3] 删除 Endpoint: {endpoint_name}")
    try:
        sm_client.delete_endpoint(EndpointName=endpoint_name)
        print(f"  Endpoint 已删除: {endpoint_name}")
    except ClientError as e:
        if e.response["Error"]["Code"] == "ValidationException":
            print(f"  Endpoint 不存在，跳过: {endpoint_name}")
        else:
            raise

    # 删除 EndpointConfig
    print(f"\n[2/3] 删除 EndpointConfig: {config_name}")
    try:
        sm_client.delete_endpoint_config(EndpointConfigName=config_name)
        print(f"  EndpointConfig 已删除: {config_name}")
    except ClientError as e:
        if e.response["Error"]["Code"] == "ValidationException":
            print(f"  EndpointConfig 不存在，跳过: {config_name}")
        else:
            raise

    # 删除 Model
    print(f"\n[3/3] 删除 Model: {endpoint_name}")
    try:
        sm_client.delete_model(ModelName=endpoint_name)
        print(f"  Model 已删除: {endpoint_name}")
    except ClientError as e:
        if e.response["Error"]["Code"] == "ValidationException":
            print(f"  Model 不存在，跳过: {endpoint_name}")
        else:
            raise

    print(f"\n所有资源已清理完毕。")


def wait_for_endpoint(sm_client, endpoint_name: str) -> str:
    """轮询等待 endpoint 变为 InService 状态。"""
    print(f"\n等待 Endpoint 变为 InService: {endpoint_name}")
    while True:
        resp = sm_client.describe_endpoint(EndpointName=endpoint_name)
        status = resp["EndpointStatus"]
        print(f"  状态: {status}", end="\r")

        if status == "InService":
            print(f"\n  Endpoint 已就绪: {endpoint_name} (InService)")
            return status
        elif status in ("Failed", "RollbackFailed"):
            reason = resp.get("FailureReason", "未知原因")
            print(f"\n  Endpoint 部署失败: {reason}")
            print(f"  CloudWatch Logs: https://console.aws.amazon.com/cloudwatch/home"
                  f"?region={sm_client.meta.region_name}#logsV2:log-groups")
            sys.exit(1)

        time.sleep(30)


def test_endpoint(sm_runtime, endpoint_name: str, samples_dir: str) -> None:
    """使用样本图片调用 endpoint 验证，打印 top-5 结果和推理延迟。"""
    sample_files = sorted(glob.glob(os.path.join(samples_dir, "*.jpg")))
    if not sample_files:
        print(f"\n未找到样本图片: {samples_dir}/*.jpg")
        return

    print(f"\n测试 Endpoint: {endpoint_name}")
    print(f"样本目录: {samples_dir} ({len(sample_files)} 张图片)")
    print("-" * 60)

    for img_path in sample_files:
        filename = os.path.basename(img_path)
        with open(img_path, "rb") as f:
            img_bytes = f.read()

        start = time.time()
        try:
            resp = sm_runtime.invoke_endpoint(
                EndpointName=endpoint_name,
                ContentType="image/jpeg",
                Body=img_bytes,
            )
            latency_ms = (time.time() - start) * 1000
            result = json.loads(resp["Body"].read())

            print(f"\n{filename} (延迟: {latency_ms:.0f}ms)")
            for pred in result.get("predictions", []):
                species = pred["species"]
                confidence = pred["confidence"]
                bar = "█" * int(confidence * 30)
                print(f"  {confidence:6.2%} {bar} {species}")

        except ClientError as e:
            latency_ms = (time.time() - start) * 1000
            print(f"\n{filename} (延迟: {latency_ms:.0f}ms) 调用失败: {e}")

    print("-" * 60)


def print_deploy_summary(endpoint_name: str, region: str) -> None:
    """部署完成后打印 endpoint 名称和调用示例命令。"""
    print(f"\n{'=' * 60}")
    print(f"部署完成!")
    print(f"{'=' * 60}")
    print(f"  Endpoint 名称: {endpoint_name}")
    print(f"  区域: {region}")
    print(f"\n调用示例:")
    print(f"  aws sagemaker-runtime invoke-endpoint \\")
    print(f"      --endpoint-name {endpoint_name} \\")
    print(f"      --content-type image/jpeg \\")
    print(f"      --body fileb://path/to/image.jpg \\")
    print(f"      --region {region} \\")
    print(f"      output.json")
    print(f"\nPython 调用示例:")
    print(f'  import boto3, json')
    print(f'  sm = boto3.client("sagemaker-runtime", region_name="{region}")')
    print(f'  with open("image.jpg", "rb") as f:')
    print(f'      resp = sm.invoke_endpoint(')
    print(f'          EndpointName="{endpoint_name}",')
    print(f'          ContentType="image/jpeg", Body=f.read())')
    print(f'  print(json.loads(resp["Body"].read()))')
    print(f"{'=' * 60}")


def parse_args() -> argparse.Namespace:
    """解析 CLI 参数。"""
    parser = argparse.ArgumentParser(
        description="SageMaker Serverless Endpoint 部署脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--backbone", default="dinov3-vitl16",
        help="Backbone 名称，决定模型 S3 路径 (默认: dinov3-vitl16)",
    )
    parser.add_argument(
        "--s3-bucket", default="raspi-eye-model-data",
        help="训练产物所在的 S3 bucket（us-east-1，默认: raspi-eye-model-data）",
    )
    parser.add_argument(
        "--deploy-bucket", default=None,
        help="model.tar.gz 上传目标 bucket（必须与 endpoint 同 region）。"
             "不指定时使用 --s3-bucket",
    )
    parser.add_argument(
        "--yolo-model-path", default=None,
        help="YOLO 模型本地路径（如 ./yolo11x.pt），打包进 model.tar.gz",
    )
    parser.add_argument(
        "--role", required="--delete" not in sys.argv and "--test" not in sys.argv,
        help="SageMaker 执行角色 ARN (必需，--delete 和 --test 模式除外)",
    )
    parser.add_argument(
        "--region", default="ap-southeast-1",
        help="AWS 区域 (默认: ap-southeast-1)",
    )
    parser.add_argument(
        "--instance-type", default="ml.m5.large",
        help="Real-time endpoint 实例类型 (默认: ml.m5.large)",
    )
    parser.add_argument(
        "--instance-count", type=int, default=1,
        help="实例数量 (默认: 1)",
    )
    parser.add_argument(
        "--serverless", action="store_true",
        help="使用 Serverless Inference（默认 Real-time）",
    )
    parser.add_argument(
        "--memory-size", type=int, default=6144,
        help="Serverless 内存大小 MB (默认: 6144，仅 --serverless 时有效)",
    )
    parser.add_argument(
        "--max-concurrency", type=int, default=5,
        help="Serverless 最大并发数 (默认: 5，仅 --serverless 时有效)",
    )
    parser.add_argument(
        "--endpoint-name", default="raspi-eye-bird-classifier",
        help="Endpoint 名称 (默认: raspi-eye-bird-classifier)",
    )
    parser.add_argument(
        "--skip-package", action="store_true",
        help="跳过模型打包步骤（假设 model.tar.gz 已在 S3）",
    )
    parser.add_argument(
        "--wait", action="store_true",
        help="等待 endpoint 变为 InService 状态",
    )
    parser.add_argument(
        "--test", action="store_true",
        help="使用样本图片测试 endpoint",
    )
    parser.add_argument(
        "--update", action="store_true",
        help="更新现有 endpoint（创建新 EndpointConfig）",
    )
    parser.add_argument(
        "--delete", action="store_true",
        help="删除 endpoint 及关联资源",
    )
    return parser.parse_args()


def main() -> None:
    """CLI 入口。"""
    args = parse_args()

    sm_client = boto3.client("sagemaker", region_name=args.region)
    # Serverless 冷启动需要较长时间（下载 1.2GB 模型 + 安装依赖），read timeout 设为 5 分钟
    from botocore.config import Config
    sm_runtime = boto3.client(
        "sagemaker-runtime",
        region_name=args.region,
        config=Config(read_timeout=300),
    )

    # --delete 模式
    if args.delete:
        delete_endpoint(sm_client, args.endpoint_name)
        return

    # --test 模式（仅测试，不部署）
    if args.test and args.skip_package and not args.update and not args.wait:
        samples_dir = os.path.join(os.path.dirname(__file__), "samples")
        test_endpoint(sm_runtime, args.endpoint_name, samples_dir)
        return

    # 模型数据 S3 URI — 目标 bucket（与 endpoint 同 region）
    deploy_bucket = args.deploy_bucket or args.s3_bucket
    model_data_url = f"s3://{deploy_bucket}/endpoint/{args.backbone}/model.tar.gz"

    # 打包并上传 model.tar.gz
    if not args.skip_package:
        # 延迟导入，避免在 --delete/--test 模式下依赖 packager
        from endpoint.packager import package_model

        # 训练产物从源 bucket（可能在不同 region）下载
        model_s3_path = (
            f"s3://{args.s3_bucket}/training/models/{args.backbone}/bird_classifier.pt"
        )
        class_names_s3_path = (
            f"s3://{args.s3_bucket}/training/models/{args.backbone}/class_names.json"
        )
        output_dir = os.path.join(os.path.dirname(__file__), "output")

        print(f"打包模型: backbone={args.backbone}")
        print(f"  训练产物源: s3://{args.s3_bucket}/training/models/{args.backbone}/")
        print(f"  上传目标:   s3://{deploy_bucket}/endpoint/{args.backbone}/")
        model_data_url = package_model(
            model_path=model_s3_path,
            class_names_path=class_names_s3_path,
            output_dir=output_dir,
            s3_bucket=deploy_bucket,
            backbone_name=args.backbone,
            yolo_model_path=args.yolo_model_path,
        )

    # --update 模式
    if args.update:
        if not endpoint_exists(sm_client, args.endpoint_name):
            print(f"错误: Endpoint {args.endpoint_name} 不存在，无法更新。请先部署。")
            sys.exit(1)
        update_endpoint(
            sm_client, args.endpoint_name, args.role,
            model_data_url, args.region,
            args.instance_type, args.instance_count,
            serverless=args.serverless,
            memory_size=args.memory_size,
            max_concurrency=args.max_concurrency,
        )
    else:
        # 检查 endpoint 是否已存在
        if endpoint_exists(sm_client, args.endpoint_name):
            print(f"\nEndpoint 已存在: {args.endpoint_name}")
            print(f"  使用 --update 更新现有 endpoint")
            print(f"  使用 --delete 删除后重新部署")
            return

        deploy_endpoint(
            sm_client, args.endpoint_name, args.role,
            model_data_url, args.region,
            args.instance_type, args.instance_count,
            serverless=args.serverless,
            memory_size=args.memory_size,
            max_concurrency=args.max_concurrency,
        )

    # --wait 模式
    if args.wait:
        wait_for_endpoint(sm_client, args.endpoint_name)

    # 打印部署摘要
    print_deploy_summary(args.endpoint_name, args.region)

    # --test 模式
    if args.test:
        samples_dir = os.path.join(os.path.dirname(__file__), "samples")
        test_endpoint(sm_runtime, args.endpoint_name, samples_dir)


if __name__ == "__main__":
    main()
