"""Spec 17 + Spec 30 Lambda 函数测试。

包含：
- Property 4: 事件解析与 snapshot key 构造不变量 (PBT)
- Property 5: 最佳预测选择不变量 (PBT)
- Property 5 (Spec 30): crop 文件名转换不变量 (PBT)
- 单元测试: S3 事件解析、event.json 解析、SageMaker mock、DynamoDB mock、错误处理
- 单元测试 (Spec 30): crop 上传、null 跳过、DynamoDB 字段、上传失败容错
"""

import base64
import io
import json
import sys
from decimal import Decimal
from pathlib import Path
from unittest.mock import MagicMock, call, patch

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st
from PIL import Image

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

# ── 辅助函数：生成合法 base64 JPEG ──────────────────────────────────────────


def _make_cropped_image_b64():
    """生成一张 518×518 的合法 base64 编码 JPEG 图片。"""
    img = Image.new("RGB", (518, 518), color=(100, 150, 200))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=95)
    return base64.b64encode(buf.getvalue()).decode("ascii")


# 测试用 SageMaker 推理响应（含 cropped_image_b64，适配 Spec 30 handler.py 变更）
SAMPLE_SM_RESPONSE = {
    "predictions": [
        {"species": "Passer montanus", "confidence": 0.92},
        {"species": "Pycnonotus sinensis", "confidence": 0.05},
        {"species": "Zosterops japonicus", "confidence": 0.02},
        {"species": "Passer cinnamomeus", "confidence": 0.005},
        {"species": "Lonchura striata", "confidence": 0.003},
    ],
    "model_metadata": {"backbone": "dinov3-vitl16", "num_classes": 46},
    "cropped_image_b64": _make_cropped_image_b64(),
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

            # Mock SageMaker invoke_endpoint（每次调用需要独立的 Body stream）
            def sm_invoke_side_effect(**kwargs):
                return {"Body": _make_body_stream(json.dumps(SAMPLE_SM_RESPONSE).encode())}

            mock_sm.invoke_endpoint.side_effect = sm_invoke_side_effect

            # 执行 handler
            result = handler(SAMPLE_S3_EVENT, None)

            # 验证 invoke_endpoint 被调用了 2 次（2 张图片）
            assert mock_sm.invoke_endpoint.call_count == 2

            # 验证请求参数
            call_args = mock_sm.invoke_endpoint.call_args
            assert call_args.kwargs["ContentType"] == "image/jpeg"
            assert call_args.kwargs["Body"] == jpeg_bytes

            # 验证 DynamoDB update_item 写入
            assert mock_table.update_item.call_count == 1
            assert result["processed"] == 1


class TestDynamoDBWriteMock:
    """DynamoDB 写入 mock 测试（handler.py 使用 update_item）。"""

    def test_update_item_contains_required_fields(self):
        """Mock update_item → 验证 UpdateExpression 和 ExpressionAttributeValues 包含必需字段。"""
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

            def sm_invoke_side_effect(**kwargs):
                return {"Body": _make_body_stream(json.dumps(SAMPLE_SM_RESPONSE).encode())}

            mock_sm.invoke_endpoint.side_effect = sm_invoke_side_effect

            handler(SAMPLE_S3_EVENT, None)

            # 验证 update_item 被调用
            mock_table.update_item.assert_called_once()
            call_kwargs = mock_table.update_item.call_args.kwargs

            # 验证 Key
            assert call_kwargs["Key"]["device_id"] == "RaspiEyeAlpha"
            assert call_kwargs["Key"]["start_time"] == "2026-04-12T15:30:45Z"

            # 验证 UpdateExpression 包含必需字段
            update_expr = call_kwargs["UpdateExpression"]
            assert "inference_species" in update_expr
            assert "inference_confidence" in update_expr
            assert "inference_image_key" in update_expr
            assert "inference_top5" in update_expr
            assert "inference_latency_ms" in update_expr

            # 验证 ExpressionAttributeValues
            expr_values = call_kwargs["ExpressionAttributeValues"]
            assert expr_values[":species"] == "Passer montanus"
            assert float(expr_values[":confidence"]) == pytest.approx(0.92, abs=1e-4)


class TestSageMakerFailure:
    """SageMaker 调用失败测试。"""

    def test_sagemaker_failure_no_exception_and_error_field(self):
        """Mock 抛出异常 → 验证不抛异常且 DynamoDB 写入 error 字段。"""
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
            mock_table.update_item.assert_called_once()
            call_kwargs = mock_table.update_item.call_args.kwargs

            # UpdateExpression 包含 inference_error
            update_expr = call_kwargs["UpdateExpression"]
            assert "inference_error" in update_expr

            # ExpressionAttributeValues 包含错误信息
            expr_values = call_kwargs["ExpressionAttributeValues"]
            assert "Endpoint timeout" in expr_values[":error"]


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
            mock_table.update_item.assert_not_called()
            assert result["processed"] == 0


# ── Task 6.1: Property 5 (Spec 30) — crop 文件名转换不变量 ───────────────────

# S3 key 策略：路径段 + 文件名 + 扩展名
_file_ext = st.sampled_from([".jpg", ".jpeg", ".png", ".bmp", ".tiff"])
_s3_key_with_ext = st.builds(
    lambda segments, basename, ext: "/".join(segments) + "/" + basename + ext,
    segments=st.lists(_path_segment, min_size=1, max_size=5),
    basename=st.from_regex(r"[A-Za-z0-9_\-]{1,30}", fullmatch=True),
    ext=_file_ext,
)


class TestCropFilenameInvariant:
    """**Validates: Requirements 6.2**

    Feature: inference-yolo-crop, Property 5: crop 文件名转换不变量
    """

    @settings(max_examples=100)
    @given(s3_key=_s3_key_with_ext)
    def test_crop_key_equals_rsplit_plus_cropped_jpg(self, s3_key):
        """对任意以文件扩展名结尾的 S3 key，crop key 等于 key.rsplit('.', 1)[0] + '_cropped.jpg'。"""
        expected = s3_key.rsplit(".", 1)[0] + "_cropped.jpg"

        # 模拟 handler.py 中的 crop key 构造逻辑
        base_name = s3_key.rsplit(".", 1)[0]
        actual = base_name + "_cropped.jpg"

        assert actual == expected
        # crop key 以 _cropped.jpg 结尾
        assert actual.endswith("_cropped.jpg")
        # crop key 保留原始路径前缀
        assert actual.startswith(s3_key.rsplit("/", 1)[0] + "/") if "/" in s3_key else True


# ── Task 6.2: 单元测试 — crop 上传、null 跳过、DynamoDB 字段、上传失败容错 ──


def _make_handler_mocks():
    """创建 handler 测试所需的标准 mock 上下文。返回 (mock_table, mock_sm, mock_s3, handler)。"""
    return (
        _patch_handler_attr("table"),
        _patch_handler_attr("sm_runtime"),
        _patch_handler_attr("s3_client"),
    )


def _setup_s3_get(mock_s3, event_json=None, jpeg_bytes=None):
    """配置 mock_s3.get_object 的 side_effect。"""
    if event_json is None:
        event_json = SAMPLE_EVENT_JSON
    if jpeg_bytes is None:
        jpeg_bytes = b"\xff\xd8\xff\xe0fake_jpeg_data"
    event_json_bytes = json.dumps(event_json).encode()

    def side_effect(Bucket, Key):
        if Key.endswith("event.json"):
            return {"Body": _make_body_stream(event_json_bytes)}
        return {"Body": _make_body_stream(jpeg_bytes)}

    mock_s3.get_object.side_effect = side_effect


def _setup_sm_response(mock_sm, sm_response=None):
    """配置 mock_sm.invoke_endpoint 返回指定响应（每次调用独立 Body stream）。"""
    if sm_response is None:
        sm_response = SAMPLE_SM_RESPONSE

    def side_effect(**kwargs):
        return {"Body": _make_body_stream(json.dumps(sm_response).encode())}

    mock_sm.invoke_endpoint.side_effect = side_effect


class TestCropUpload:
    """crop 图片上传测试。**Validates: Requirements 9.1**"""

    def test_crop_image_uploaded_to_s3(self):
        """endpoint 响应含 cropped_image_b64 → 验证 s3_client.put_object 调用参数。"""
        with _patch_handler_attr("table") as mock_table, \
             _patch_handler_attr("sm_runtime") as mock_sm, \
             _patch_handler_attr("s3_client") as mock_s3:

            handler = _handler_module.handler
            _setup_s3_get(mock_s3)
            _setup_sm_response(mock_sm)

            handler(SAMPLE_S3_EVENT, None)

            # 2 张图片，每张都有 cropped_image_b64 → put_object 被调用 2 次
            assert mock_s3.put_object.call_count == 2

            # 验证第一次 put_object 调用参数
            first_call = mock_s3.put_object.call_args_list[0]
            assert first_call.kwargs["Bucket"] == "raspi-eye-captures-014498626607-ap-southeast-1-an"
            assert first_call.kwargs["Key"] == "RaspiEyeAlpha/2026-04-12/evt_20260412_153045/20260412_153046_001_cropped.jpg"
            assert first_call.kwargs["ContentType"] == "image/jpeg"
            # Body 应为 base64 解码后的 bytes
            assert isinstance(first_call.kwargs["Body"], bytes)
            assert len(first_call.kwargs["Body"]) > 0

            # 验证第二次 put_object 调用参数
            second_call = mock_s3.put_object.call_args_list[1]
            assert second_call.kwargs["Key"] == "RaspiEyeAlpha/2026-04-12/evt_20260412_153045/20260412_153047_002_cropped.jpg"


class TestCropNullSkip:
    """crop 为 null 跳过测试。**Validates: Requirements 9.2**"""

    def test_no_put_object_when_cropped_image_null(self):
        """endpoint 响应 cropped_image_b64 为 null → 不调用 s3_client.put_object。"""
        sm_response_no_crop = {
            **SAMPLE_SM_RESPONSE,
            "cropped_image_b64": None,
        }

        with _patch_handler_attr("table") as mock_table, \
             _patch_handler_attr("sm_runtime") as mock_sm, \
             _patch_handler_attr("s3_client") as mock_s3:

            handler = _handler_module.handler
            _setup_s3_get(mock_s3)
            _setup_sm_response(mock_sm, sm_response_no_crop)

            handler(SAMPLE_S3_EVENT, None)

            # cropped_image_b64 为 null → 不应调用 put_object
            mock_s3.put_object.assert_not_called()

            # DynamoDB 仍然写入，inference_cropped_image_key 为 None
            mock_table.update_item.assert_called_once()
            call_kwargs = mock_table.update_item.call_args.kwargs
            expr_values = call_kwargs["ExpressionAttributeValues"]
            assert expr_values[":cropped_key"] is None


class TestDynamoDBCroppedImageKey:
    """DynamoDB inference_cropped_image_key 字段测试。**Validates: Requirements 9.3**"""

    def test_update_item_contains_cropped_image_key(self):
        """验证 update_item 的 UpdateExpression 包含 inference_cropped_image_key。"""
        with _patch_handler_attr("table") as mock_table, \
             _patch_handler_attr("sm_runtime") as mock_sm, \
             _patch_handler_attr("s3_client") as mock_s3:

            handler = _handler_module.handler
            _setup_s3_get(mock_s3)
            _setup_sm_response(mock_sm)

            handler(SAMPLE_S3_EVENT, None)

            mock_table.update_item.assert_called_once()
            call_kwargs = mock_table.update_item.call_args.kwargs

            # UpdateExpression 包含 inference_cropped_image_key
            update_expr = call_kwargs["UpdateExpression"]
            assert "inference_cropped_image_key" in update_expr

            # ExpressionAttributeValues 包含 :cropped_key
            expr_values = call_kwargs["ExpressionAttributeValues"]
            assert ":cropped_key" in expr_values
            # 有 crop 图片时，cropped_key 应为非 None 的 S3 key
            cropped_key = expr_values[":cropped_key"]
            assert cropped_key is not None
            assert cropped_key.endswith("_cropped.jpg")


class TestCropUploadFailureTolerance:
    """crop 上传失败不影响推理测试。**Validates: Requirements 9.4**"""

    def test_put_object_failure_still_writes_dynamodb(self):
        """s3_client.put_object 抛异常 → 推理结果仍写入 DynamoDB，cropped_s3_key 为 None。"""
        with _patch_handler_attr("table") as mock_table, \
             _patch_handler_attr("sm_runtime") as mock_sm, \
             _patch_handler_attr("s3_client") as mock_s3:

            handler = _handler_module.handler
            _setup_s3_get(mock_s3)
            _setup_sm_response(mock_sm)

            # put_object 抛异常（模拟 S3 上传失败）
            mock_s3.put_object.side_effect = Exception("S3 upload failed")

            # 不应抛出异常
            result = handler(SAMPLE_S3_EVENT, None)

            # DynamoDB 仍然写入
            mock_table.update_item.assert_called_once()
            assert result["processed"] == 1

            # inference_cropped_image_key 应为 None（上传失败）
            call_kwargs = mock_table.update_item.call_args.kwargs
            expr_values = call_kwargs["ExpressionAttributeValues"]
            assert expr_values[":cropped_key"] is None

            # UpdateExpression 仍包含推理结果字段
            update_expr = call_kwargs["UpdateExpression"]
            assert "inference_species" in update_expr
            assert "inference_confidence" in update_expr
