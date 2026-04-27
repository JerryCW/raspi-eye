"""模型打包模块：将 .pt 模型文件 + 推理代码打包为 model.tar.gz。

打包后的 tar.gz 内部结构：
├── bird_classifier.pt
├── class_names.json
└── code/
    ├── inference.py
    └── requirements.txt
"""

import os
import shutil
import tarfile
import tempfile

import boto3


def package_model(
    model_path: str,
    class_names_path: str,
    output_dir: str,
    s3_bucket: str | None = None,
    backbone_name: str = "dinov3-vitl16",
    yolo_model_path: str | None = None,
) -> str:
    """打包 model.tar.gz 并可选上传到 S3。

    Args:
        model_path: 本地 .pt 文件路径或 S3 URI（s3://bucket/key）
        class_names_path: class_names.json 路径
        output_dir: 本地输出目录
        s3_bucket: 上传目标 bucket（None 则不上传）
        backbone_name: backbone 名称（决定 S3 路径）
        yolo_model_path: YOLO 模型文件路径（本地或 S3 URI，None 则不打包）

    Returns:
        S3 URI（如果上传）或本地 tar.gz 路径
    """
    os.makedirs(output_dir, exist_ok=True)

    # 推理代码所在目录
    endpoint_dir = os.path.dirname(os.path.abspath(__file__))
    inference_py = os.path.join(endpoint_dir, "inference.py")
    requirements_txt = os.path.join(endpoint_dir, "requirements.txt")

    with tempfile.TemporaryDirectory() as staging_dir:
        # 获取模型文件（支持 S3 URI）
        local_model_path = _resolve_path(model_path, staging_dir, "bird_classifier.pt")
        local_class_names_path = _resolve_path(class_names_path, staging_dir, "class_names.json")

        tar_path = os.path.join(output_dir, "model.tar.gz")

        with tarfile.open(tar_path, "w:gz") as tar:
            tar.add(local_model_path, arcname="bird_classifier.pt")
            tar.add(local_class_names_path, arcname="class_names.json")
            tar.add(inference_py, arcname="code/inference.py")
            tar.add(requirements_txt, arcname="code/requirements.txt")
            # 打包 YOLO 模型（可选）
            if yolo_model_path:
                yolo_filename = os.path.basename(yolo_model_path)
                local_yolo_path = _resolve_path(yolo_model_path, staging_dir, yolo_filename)
                tar.add(local_yolo_path, arcname=yolo_filename)

    tar_size = os.path.getsize(tar_path)
    print(f"model.tar.gz 大小: {tar_size / 1024 / 1024:.1f} MB")

    if s3_bucket:
        s3_key = f"endpoint/{backbone_name}/model.tar.gz"
        s3_uri = f"s3://{s3_bucket}/{s3_key}"
        print(f"上传到 {s3_uri} ...")
        s3_client = boto3.client("s3")
        s3_client.upload_file(tar_path, s3_bucket, s3_key)
        print(f"上传完成: {s3_uri}")
        return s3_uri

    print(f"本地路径: {tar_path}")
    return tar_path


def _resolve_path(path: str, staging_dir: str, filename: str) -> str:
    """解析路径：如果是 S3 URI 则下载到临时目录，否则返回本地路径。

    自动检测 bucket 所在 region，避免跨 region 403 错误。
    """
    if path.startswith("s3://"):
        local_path = os.path.join(staging_dir, filename)
        bucket, key = _parse_s3_uri(path)
        # 检测 bucket region，用对应 region 的 client 下载
        region = _get_bucket_region(bucket)
        s3_client = boto3.client("s3", region_name=region)
        print(f"从 S3 下载: {path} -> {local_path} (region={region})")
        s3_client.download_file(bucket, key, local_path)
        return local_path
    return path


def _get_bucket_region(bucket: str) -> str:
    """获取 S3 bucket 所在 region。"""
    s3_client = boto3.client("s3")
    try:
        resp = s3_client.get_bucket_location(Bucket=bucket)
        # us-east-1 返回 None
        return resp["LocationConstraint"] or "us-east-1"
    except Exception:
        return "us-east-1"


def _parse_s3_uri(uri: str) -> tuple[str, str]:
    """解析 S3 URI 为 (bucket, key)。"""
    # s3://bucket/key/path
    parts = uri.replace("s3://", "").split("/", 1)
    return parts[0], parts[1]
