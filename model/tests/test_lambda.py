"""Spec 17 Lambda 函数测试。

包含：
- Property 4: 事件解析与 snapshot key 构造不变量 (PBT)
- Property 5: 最佳预测选择不变量 (PBT)
- 单元测试: S3 事件解析、event.json 解析、SageMaker mock、DynamoDB mock、错误处理
"""

import io
import json
import sys
from decimal import Decimal
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

# 确保项目根目录在 sys.path 中
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

import importlib

# "lambda" 是 Python 关键字，无法直接 import model.lambda.handler
_handler_module = importlib.import_module("model.lambda.handler")
build_snapshot_keys = _handler_module.build_snapshot_keys
parse_event_json = _handler_module.parse_event_json
select_best_prediction = _handler_module.select_best_prediction


# ── Hypothesis strategies ─────────────────────────────────────────────────────

# S3 key 路径段：字母数字 + 下划线/连字符，非空
_path_segment = st.from_regex(r"[A-Za-z0-9_\-]{1,30}", fullmatch=True)

# 日期格式
_date_str = st.from_regex(r"20[2-3][0-9]\-[01][0-9]\-[0-3][0-9]", fullmatch=True)

# 事件 ID
_event_id = st.from_regex(r"evt_[0-9]{8}_[0-9]{6}", fullmatch=True)

# JPEG 文件名
_jpg_filename = st.from_regex(r"[0-9]{8}_[0-9]{6}_[0-9]{3}\.jpg", fullmatch=True)


# ── Task 4.2: Property 4 — 事件解析与 snapshot key 构造不变量 ─────────────────


class TestBuildSnapshotKeysInvariant:
    """**Validates: Requirements 6.2, 6.3, 12.2**"""

    @settings(max_examples=100)
    @given(
        device_id=_path_segment,
        date=_date_str,
        event_id=_event_id,
        snapshots=st.lists(_jpg_filename, min_size=0, max_size=10),
    )
    def test_build_snapshot_keys_invariant(self, device_id, date, event_id, snapshots):
        """对任意合法 event.json S3 key 和 snapshots 列表，构造的 key 满足不变量。"""
        event_key = f"{device_id}/{date}/{event_id}/event.json"
        prefix = f"{device_id}/{date}/{event_id}/"

        keys = build_snapshot_keys(event_key, snapshots)

        # key 数量等于 snapshots 列表长度
        assert len(keys) == len(snapshots)

        for key, fname in zip(keys, snapshots):
            # 每个 key 以 event.json 所在目录为前缀
            assert key.startswith(prefix)
            # 每个 key 以对应的 snapshot 文件名为后缀
            assert key.endswith(fname)


# ── Task 4.3: Property 5 — 最佳预测选择不变量 ────────────────────────────────

# 单个预测项策略
_prediction_item = st.fixed_dictionaries({
    "species": st.from_regex(r"[A-Z][a-z]+ [a-z]+", fullmatch=True),
    "confidence": st.floats(min_value=0.001, max_value=1.0, allow_nan=False, allow_infinity=False),
})

# 单张图片的推理结果（含 top-5 预测）
_inference_result = st.fixed_dictionaries({
    "image_key": st.from_regex(r"[a-z]+/[0-9]+/[a-z]+\.jpg", fullmatch=True),
    "predictions": st.lists(_prediction_item, min_size=1, max_size=5),
    "latency_ms": st.floats(min_value=1.0, max_value=10000.0, allow_nan=False, allow_infinity=False),
})


class TestSelectBestPredictionInvariant:
    """**Validates: Requirements 6.5, 12.6**"""

    @settings(max_examples=100)
    @given(results=st.lists(_inference_result, min_size=1, max_size=10))
    def test_select_best_prediction_invariant(self, results):
        """选出的 confidence 恒等于所有图片所有预测中的最大值。"""
        best = select_best_prediction(results)

        # 计算全局最大 confidence
        global_max = max(
            pred["confidence"]
            for result in results
            for pred in result["predictions"]
        )

        assert best is not None
        assert best["confidence"] == global_max


# ── Task 4.4: 单元测试 — Lambda 事件处理 + mock AWS 服务 ─────────────────────

# 测试用 S3 事件
SAMPLE_S3_EVENT = {
    "Records": [{
        "s3": {
            "bucket": {"name": "raspi-eye-captures-014498626607-ap-southeast-1-an"},
            "object": {"key": "RaspiEyeAlpha/2026-04-12/evt_20260412_153045/event.json"},
        }
    }]
}

# 测试用 event.json
SAMPLE_EVENT_JSON = {
    "event_id": "evt_20260412_153045",
    "device_id": "RaspiEyeAlpha",
    "start_time": "2026-04-12T15:30:45Z",
    "detections_summary": {"bird": {"count": 8, "max_confidence": 0.92}},
    "snapshots": ["20260412_153046_001.jpg", "20260412_153047_002.jpg"],
}

# 测试用 SageMaker 推理响应
SAMPLE_SM_RESPONSE = {
    "predictions": [
        {"species": "Passer montanus", "confidence": 0.92},
        {"species": "Pycnonotus sinensis", "confidence": 0.05},
        {"species": "Zosterops japonicus", "confidence": 0.02},
        {"species": "Passer cinnamomeus", "confidence": 0.005},
        {"species": "Lonchura striata", "confidence": 0.003},
    ],
    "model_metadata": {"backbone": "dinov3-vitl16", "num_classes": 46},
}


class TestS3EventParsing:
    """S3 事件解析测试。"""

    def test_extract_bucket_and_key(self):
        """构造 S3 事件 → 验证提取 bucket 和 key。"""
        record = SAMPLE_S3_EVENT["Records"][0]
        bucket = record["s3"]["bucket"]["name"]
        key = record["s3"]["object"]["key"]

        assert bucket == "raspi-eye-captures-014498626607-ap-southeast-1-an"
        assert key == "RaspiEyeAlpha/2026-04-12/evt_20260412_153045/event.json"
        assert key.endswith("event.json")


class TestEventJsonParsing:
    """event.json 解析测试。"""

    def test_parse_event_json_extracts_all_fields(self):
        """构造 event.json → 验证提取所有字段。"""
        parsed = parse_event_json(SAMPLE_EVENT_JSON)

        assert parsed["event_id"] == "evt_20260412_153045"
        assert parsed["device_id"] == "RaspiEyeAlpha"
        assert parsed["start_time"] == "2026-04-12T15:30:45Z"
        assert parsed["detections_summary"] == {"bird": {"count": 8, "max_confidence": 0.92}}
        assert parsed["snapshots"] == ["20260412_153046_001.jpg", "20260412_153047_002.jpg"]


def _make_body_stream(data: bytes) -> MagicMock:
    """创建模拟 S3/SageMaker 响应 Body 的 StreamingBody mock。"""
    mock_body = MagicMock()
    mock_body.read.return_value = data
    return mock_body


# 用于 patch 的模块路径（"lambda" 是 Python 关键字，无法用 dotted path）
_MODULE_PATH = "model.lambda.handler"


def _patch_handler_attr(attr_name):
    """创建 patch 对象，直接 patch 模块属性。"""
    return patch.object(_handler_module, attr_name)


class TestSageMakerInvokeMock:
    """SageMaker 调用 mock 测试。"""

    def test_invoke_endpoint_params_and_response(self):
        """Mock invoke_endpoint → 验证请求参数和响应解析。"""
        with _patch_handler_attr("table") as mock_table, \
             _patch_handler_attr("sm_runtime") as mock_sm, \
             _patch_handler_attr("s3_client") as mock_s3:

            handler = _handler_module.handler

            # Mock S3 get_object: event.json
            event_json_bytes = json.dumps(SAMPLE_EVENT_JSON).encode()
            jpeg_bytes = b"\xff\xd8\xff\xe0fake_jpeg_data"

            def s3_get_side_effect(Bucket, Key):
                if Key.endswith("event.json"):
                    return {"Body": _make_body_stream(event_json_bytes)}
                return {"Body": _make_body_stream(jpeg_bytes)}

            mock_s3.get_object.side_effect = s3_get_side_effect

            # Mock SageMaker invoke_endpoint
            sm_response_bytes = json.dumps(SAMPLE_SM_RESPONSE).encode()
            mock_sm.invoke_endpoint.return_value = {
                "Body": _make_body_stream(sm_response_bytes),
            }

            # 执行 handler
            result = handler(SAMPLE_S3_EVENT, None)

            # 验证 invoke_endpoint 被调用了 2 次（2 张图片）
            assert mock_sm.invoke_endpoint.call_count == 2

            # 验证请求参数
            call_args = mock_sm.invoke_endpoint.call_args
            assert call_args.kwargs["ContentType"] == "image/jpeg"
            assert call_args.kwargs["Body"] == jpeg_bytes

            # 验证 DynamoDB 写入
            assert mock_table.put_item.call_count == 1
            assert result["processed"] == 1


class TestDynamoDBWriteMock:
    """DynamoDB 写入 mock 测试。"""

    def test_put_item_contains_required_fields(self):
        """Mock put_item → 验证记录包含所有必需字段。"""
        with _patch_handler_attr("table") as mock_table, \
             _patch_handler_attr("sm_runtime") as mock_sm, \
             _patch_handler_attr("s3_client") as mock_s3:

            handler = _handler_module.handler

            event_json_bytes = json.dumps(SAMPLE_EVENT_JSON).encode()
            jpeg_bytes = b"\xff\xd8\xff\xe0fake_jpeg_data"

            def s3_get_side_effect(Bucket, Key):
                if Key.endswith("event.json"):
                    return {"Body": _make_body_stream(event_json_bytes)}
                return {"Body": _make_body_stream(jpeg_bytes)}

            mock_s3.get_object.side_effect = s3_get_side_effect

            sm_response_bytes = json.dumps(SAMPLE_SM_RESPONSE).encode()
            mock_sm.invoke_endpoint.return_value = {
                "Body": _make_body_stream(sm_response_bytes),
            }

            handler(SAMPLE_S3_EVENT, None)

            # 验证 put_item 被调用
            mock_table.put_item.assert_called_once()
            item = mock_table.put_item.call_args.kwargs["Item"]

            # 验证必需字段
            assert "event_id" in item
            assert "device_id" in item
            assert "timestamp" in item
            assert "species" in item
            assert "confidence" in item
            assert "image_key" in item
            assert "top5_predictions" in item
            assert "detections_summary" in item
            assert "inference_latency_ms" in item

            # 验证字段值
            assert item["event_id"] == "evt_20260412_153045"
            assert item["device_id"] == "RaspiEyeAlpha"
            assert item["timestamp"] == "2026-04-12T15:30:45Z"
            assert item["species"] == "Passer montanus"


class TestSageMakerFailure:
    """SageMaker 调用失败测试。"""

    def test_sagemaker_failure_no_exception_and_error_field(self):
        """Mock 抛出异常 → 验证不抛异常且 error 字段有值。"""
        with _patch_handler_attr("table") as mock_table, \
             _patch_handler_attr("sm_runtime") as mock_sm, \
             _patch_handler_attr("s3_client") as mock_s3:

            handler = _handler_module.handler

            event_json_bytes = json.dumps(SAMPLE_EVENT_JSON).encode()
            jpeg_bytes = b"\xff\xd8\xff\xe0fake_jpeg_data"

            def s3_get_side_effect(Bucket, Key):
                if Key.endswith("event.json"):
                    return {"Body": _make_body_stream(event_json_bytes)}
                return {"Body": _make_body_stream(jpeg_bytes)}

            mock_s3.get_object.side_effect = s3_get_side_effect

            # SageMaker 调用抛出异常
            mock_sm.invoke_endpoint.side_effect = Exception("Endpoint timeout")

            # 不应抛出异常
            result = handler(SAMPLE_S3_EVENT, None)

            # DynamoDB 仍然写入（带 error 字段）
            mock_table.put_item.assert_called_once()
            item = mock_table.put_item.call_args.kwargs["Item"]

            # species 不在 item 中（因为 best 为 None，None 值被过滤）
            assert "species" not in item or item.get("species") is None
            # error 字段有值
            assert "error" in item
            assert "Endpoint timeout" in item["error"]


class TestEventJsonFormatError:
    """event.json 格式错误测试。"""

    def test_malformed_event_json_skipped(self):
        """传入缺少字段的 JSON → 验证跳过且不抛异常。"""
        with _patch_handler_attr("table") as mock_table, \
             _patch_handler_attr("sm_runtime") as mock_sm, \
             _patch_handler_attr("s3_client") as mock_s3:

            handler = _handler_module.handler

            # event.json 缺少 event_id 字段
            bad_event_json = {"device_id": "test", "snapshots": []}
            event_json_bytes = json.dumps(bad_event_json).encode()

            mock_s3.get_object.return_value = {
                "Body": _make_body_stream(event_json_bytes),
            }

            # 不应抛出异常
            result = handler(SAMPLE_S3_EVENT, None)

            # 不应调用 SageMaker 或 DynamoDB
            mock_sm.invoke_endpoint.assert_not_called()
            mock_table.put_item.assert_not_called()
            assert result["processed"] == 0
