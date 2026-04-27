"""Lambda 函数：S3 事件触发 → 读取截图 → SageMaker 推理 → DynamoDB 写入。

通过环境变量获取配置：
- ENDPOINT_NAME: SageMaker endpoint 名称
- TABLE_NAME: DynamoDB 表名
"""

import base64
import json
import logging
import os
import time
from collections import Counter
from decimal import Decimal

import boto3

logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

ENDPOINT_NAME = os.environ.get("ENDPOINT_NAME", "raspi-eye-bird-classifier")
TABLE_NAME = os.environ.get("TABLE_NAME", "raspi-eye-events")
CONFIDENCE_THRESHOLD = 0.5

s3_client = boto3.client("s3")
sm_runtime = boto3.client("sagemaker-runtime")
dynamodb = boto3.resource("dynamodb")
table = dynamodb.Table(TABLE_NAME)


def parse_event_json(event_data: dict) -> dict:
    """解析 event.json，提取必需字段。

    Args:
        event_data: event.json 解析后的字典

    Returns:
        包含 event_id、device_id、start_time、detections_summary、snapshots 的字典

    Raises:
        KeyError: 缺少必需字段
    """
    return {
        "event_id": event_data["event_id"],
        "device_id": event_data["device_id"],
        "start_time": event_data["start_time"],
        "detections_summary": event_data.get("detections_summary", {}),
        "snapshots": event_data.get("snapshots", []),
    }


def build_snapshot_keys(event_key: str, snapshots: list[str]) -> list[str]:
    """从 event.json 所在目录前缀 + snapshot 文件名构造完整 S3 key。

    Args:
        event_key: event.json 的完整 S3 key
            例: "RaspiEyeAlpha/2026-04-12/evt_20260412_153045/event.json"
        snapshots: snapshot 文件名列表
            例: ["20260412_153046_001.jpg", "20260412_153047_002.jpg"]

    Returns:
        完整 S3 key 列表
            例: ["RaspiEyeAlpha/2026-04-12/evt_20260412_153045/20260412_153046_001.jpg", ...]
    """
    prefix = event_key.rsplit("/", 1)[0] + "/"
    return [prefix + fname for fname in snapshots]


def _max_confidence_for_species(results: list[dict], species: str) -> float:
    """返回指定 species 在 results 所有预测中的最高 confidence。"""
    max_conf = 0.0
    for result in results:
        for pred in result["predictions"]:
            if pred["species"] == species and pred["confidence"] > max_conf:
                max_conf = pred["confidence"]
    return max_conf


def _find_best_result_for_species(results: list[dict], species: str) -> dict:
    """找到给出指定 species 最高 confidence 的那张图片的 result。"""
    best_result = None
    best_conf = -1.0
    for result in results:
        for pred in result["predictions"]:
            if pred["species"] == species and pred["confidence"] > best_conf:
                best_conf = pred["confidence"]
                best_result = result
    return best_result


def select_best_prediction(results: list[dict]) -> dict:
    """从多张图片的推理结果中选取最佳预测（多数投票 + 回退）。

    投票逻辑：
    1. 每张图片取 top-1（confidence 最高）预测的 species 作为投票
    2. 统计每个 species 的票数，选最高票数的 species
    3. 平票时选全局最高 confidence 的 species 打破平票
    4. 票数不足 2 时回退到全局最高 confidence

    Args:
        results: 推理结果列表（仅包含 has_bird=True 的图片），每项包含:
            - image_key: S3 图片 key
            - predictions: top-5 预测列表
            - latency_ms: 推理耗时

    Returns:
        最佳预测 dict，包含:
            species, confidence, image_key, top5_predictions, latency_ms, vote_count
    """
    # Step 1: 每张图片取 top-1 species
    votes = []
    for result in results:
        top1 = max(result["predictions"], key=lambda p: p["confidence"])
        votes.append(top1["species"])

    # Step 2: 统计票数
    vote_counts = Counter(votes)
    max_count = max(vote_counts.values())

    # Step 3: 判断是否启用投票
    if max_count >= 2:
        # 投票生效：取最高票数的 species
        candidates = [sp for sp, cnt in vote_counts.items() if cnt == max_count]
        if len(candidates) == 1:
            winner = candidates[0]
        else:
            # 平票：每个 candidate 取其在 results 所有预测中的最高 confidence，选最大的
            winner = max(candidates, key=lambda sp: _max_confidence_for_species(results, sp))

        best_confidence = _max_confidence_for_species(results, winner)
        vote_count = vote_counts[winner]

        # 从给出最高 confidence 的那张图片取 image_key、top5_predictions、latency_ms
        best_result = _find_best_result_for_species(results, winner)
        return {
            "species": winner,
            "confidence": best_confidence,
            "image_key": best_result["image_key"],
            "top5_predictions": best_result["predictions"],
            "latency_ms": best_result["latency_ms"],
            "vote_count": vote_count,
        }
    else:
        # 回退：全局最高 confidence
        best = None
        for result in results:
            for pred in result["predictions"]:
                if best is None or pred["confidence"] > best["confidence"]:
                    best = {
                        "species": pred["species"],
                        "confidence": pred["confidence"],
                        "image_key": result["image_key"],
                        "top5_predictions": result["predictions"],
                        "latency_ms": result["latency_ms"],
                        "vote_count": 1,
                    }
        return best


def handler(event: dict, context) -> dict:
    """Lambda 入口函数。

    处理 S3 ObjectCreated 事件，读取 event.json，逐张调用 SageMaker 推理，
    将结果写入 DynamoDB。

    Args:
        event: S3 事件通知
        context: Lambda context

    Returns:
        处理结果摘要
    """
    processed = 0

    for record in event.get("Records", []):
        bucket = record["s3"]["bucket"]["name"]
        key = record["s3"]["object"]["key"]

        # 过滤非 event.json 的 key
        if not key.endswith("event.json"):
            logger.info("跳过非 event.json 文件: %s", key)
            continue

        # 从 S3 读取 event.json
        try:
            obj = s3_client.get_object(Bucket=bucket, Key=key)
            event_data = json.loads(obj["Body"].read())
        except Exception as e:
            logger.error("读取 S3 对象失败 %s/%s: %s", bucket, key, e)
            continue

        # 解析 event.json
        try:
            parsed = parse_event_json(event_data)
        except (KeyError, TypeError) as e:
            logger.warning("event.json 格式错误 %s: %s", key, e)
            continue

        event_id = parsed["event_id"]
        device_id = parsed["device_id"]
        start_time = parsed["start_time"]
        detections_summary = parsed["detections_summary"]
        snapshots = parsed["snapshots"]

        if not snapshots:
            logger.warning("event.json 无 snapshots: %s", key)
            continue

        # 构造完整 S3 key
        snapshot_keys = build_snapshot_keys(key, snapshots)

        # 逐张推理
        inference_results = []
        errors = []

        for jpg_key in snapshot_keys:
            try:
                img_obj = s3_client.get_object(Bucket=bucket, Key=jpg_key)
                img_bytes = img_obj["Body"].read()
            except Exception as e:
                logger.warning("读取图片失败 %s: %s", jpg_key, e)
                continue

            try:
                start = time.time()
                response = sm_runtime.invoke_endpoint(
                    EndpointName=ENDPOINT_NAME,
                    ContentType="image/jpeg",
                    Body=img_bytes,
                )
                latency_ms = (time.time() - start) * 1000
                result_body = json.loads(response["Body"].read())

                # 提取并上传 crop 图片
                cropped_b64 = result_body.get("cropped_image_b64")
                cropped_s3_key = None
                if cropped_b64:
                    base_name = jpg_key.rsplit(".", 1)[0]
                    cropped_s3_key = base_name + "_cropped.jpg"
                    try:
                        cropped_bytes = base64.b64decode(cropped_b64)
                        s3_client.put_object(
                            Bucket=bucket,
                            Key=cropped_s3_key,
                            Body=cropped_bytes,
                            ContentType="image/jpeg",
                        )
                    except Exception as e:
                        logger.warning("crop 图片上传失败 %s: %s", cropped_s3_key, e)
                        cropped_s3_key = None

                has_bird = cropped_b64 is not None
                inference_results.append({
                    "image_key": jpg_key,
                    "predictions": result_body["predictions"],
                    "latency_ms": latency_ms,
                    "cropped_s3_key": cropped_s3_key,
                    "has_bird": has_bird,
                })
            except Exception as e:
                logger.error("SageMaker 推理失败 %s: %s", jpg_key, e)
                errors.append(f"{jpg_key}: {e}")

        # 投票前过滤：仅 has_bird=True 的图片参与投票
        votable_results = [r for r in inference_results if r.get("has_bird", False)]
        best = select_best_prediction(votable_results) if votable_results else None

        # 用 update_item 把推理结果写回已有的事件记录
        # raspi-eye-events 表 PK: device_id (HASH) + start_time (RANGE)
        update_expr_parts = []
        expr_values = {}

        if best:
            species = best["species"]
            confidence = best["confidence"]
            vote_count = best["vote_count"]

            # 置信度门槛判断
            if confidence >= CONFIDENCE_THRESHOLD:
                reliable = True
            else:
                species = "uncertain"
                reliable = False

            update_expr_parts.extend([
                "inference_species = :species",
                "inference_confidence = :confidence",
                "inference_image_key = :image_key",
                "inference_top5 = :top5",
                "inference_latency_ms = :latency_ms",
                "inference_cropped_image_key = :cropped_key",
                "inference_reliable = :reliable",
                "inference_vote_count = :vote_count",
            ])
            expr_values[":species"] = species
            expr_values[":confidence"] = Decimal(str(round(confidence, 6)))
            expr_values[":image_key"] = best["image_key"]
            expr_values[":top5"] = [
                {"species": p["species"], "confidence": Decimal(str(round(p["confidence"], 6)))}
                for p in best["top5_predictions"]
            ]
            expr_values[":latency_ms"] = Decimal(str(round(best["latency_ms"], 1)))
            # 找到最佳预测对应的 cropped_s3_key
            best_cropped_key = None
            for r in inference_results:
                if r["image_key"] == best["image_key"]:
                    best_cropped_key = r.get("cropped_s3_key")
                    break
            expr_values[":cropped_key"] = best_cropped_key
            expr_values[":reliable"] = reliable
            expr_values[":vote_count"] = Decimal(str(vote_count))
        elif inference_results:
            # 所有图片云端 YOLO 均未检测到鸟
            update_expr_parts.extend([
                "inference_species = :species",
                "inference_confidence = :confidence",
                "inference_image_key = :image_key",
                "inference_reliable = :reliable",
                "inference_vote_count = :vote_count",
            ])
            expr_values[":species"] = "no_bird_detected"
            expr_values[":confidence"] = Decimal("0")
            expr_values[":image_key"] = "N/A"
            expr_values[":reliable"] = False
            expr_values[":vote_count"] = Decimal("0")

        if errors:
            update_expr_parts.append("inference_error = :error")
            expr_values[":error"] = "; ".join(errors)

        if not update_expr_parts:
            update_expr_parts.append("inference_error = :error")
            expr_values[":error"] = "no inference results"

        try:
            table.update_item(
                Key={"device_id": device_id, "start_time": start_time},
                UpdateExpression="SET " + ", ".join(update_expr_parts),
                ExpressionAttributeValues=expr_values,
            )
            processed += 1
            logger.info("推理结果已更新 DynamoDB: device_id=%s, start_time=%s, species=%s",
                        device_id, start_time, best["species"] if best else "N/A")
        except Exception as e:
            logger.error("DynamoDB 更新失败 device_id=%s, start_time=%s: %s", device_id, start_time, e)

    return {"processed": processed}
