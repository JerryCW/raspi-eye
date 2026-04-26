# Implementation Plan: Spec 30 — 推理链路 YOLO Crop 对齐

## Overview

在 SageMaker endpoint 推理链路中加入 YOLO crop 步骤，修改 inference.py（预处理 + 输出）、handler.py（crop 图片上传）、packager.py（打包 YOLO 模型），并更新对应测试。所有改动使用 Python，测试通过 pytest + Hypothesis 运行。

## Tasks

- [x] 1. 修改 inference.py：新增 YOLO crop 预处理和 crop 图片返回
  - [x] 1.1 新增 `_letterbox_resize` 和 `_yolo_crop` 函数，新增模块级变量 `_yolo_model` 和 `BIRD_CLASS_ID`
    - 新增 `import base64`（供 output_fn 使用）
    - 新增模块级变量 `_yolo_model = None` 和 `BIRD_CLASS_ID = 14`
    - `_letterbox_resize(image, target_size)`: 内联 letterbox resize 逻辑（等比缩放 + 黑色填充），与 `cleaning/cleaner.py` 中的 `letterbox_resize` 行为等效
    - `_yolo_crop(image, yolo_model, conf_threshold=0.3, padding=0.2)`: YOLO 检测鸟体 bbox → 取最高置信度 → padding 20%（clamp 到图片边界） → crop → `_letterbox_resize(cropped, _input_size)` → 返回 PIL Image 或 None
    - YOLO 推理输入为 PIL Image 对象，设置 `verbose=False`
    - _Requirements: 2.1, 2.2, 2.5, 2.6_
  - [x] 1.2 修改 `model_fn`：加载 YOLO 模型
    - 在现有 model_fn 末尾增加 YOLO 模型加载逻辑
    - 从 `model_dir` 加载 `yolo11s.pt`，使用 `from ultralytics import YOLO` 初始化
    - 设置模块级变量 `_yolo_model`
    - 不存在时记录 warning，`_yolo_model` 设为 None
    - 返回字典新增 `yolo_model` 键
    - _Requirements: 1.1, 1.2, 1.3_
  - [x] 1.3 修改 `input_fn`：返回 dict（tensor + cropped_image），返回类型从 `torch.Tensor` 变为 `dict`
    - YOLO 可用且检测到鸟体：crop → letterbox resize → ToImage + ToDtype + Normalize → `cropped_image` 为 PIL Image (input_size × input_size)
    - YOLO 不可用或未检测到鸟体：回退到 `_get_val_transform` 的 Resize + CenterCrop + Normalize → `cropped_image` 为 None
    - YOLO 推理抛出异常时：捕获异常，记录 warning，回退到原始预处理流程（保证 YOLO 相关失败不影响核心推理链路）
    - 返回 `{"tensor": tensor.unsqueeze(0), "cropped_image": cropped_image}`
    - _Requirements: 2.1, 2.3, 2.4, 2.7, 2.8, 2.9_
  - [x] 1.4 修改 `predict_fn`：接收 dict，透传 cropped_image
    - `input_data` 参数类型从 `torch.Tensor` 变为 `dict`，从 `input_data["tensor"]` 提取张量
    - 输出 dict 新增 `cropped_image` 键（透传自 input_data）
    - _Requirements: 3.4_
  - [x] 1.5 修改 `output_fn`：新增 cropped_image_b64 字段
    - 从 prediction dict 中 pop `cropped_image`
    - `cropped_image` 非 None 时：PIL Image → JPEG(quality=95) → base64 编码为 ascii 字符串
    - `cropped_image` 为 None 时：`cropped_image_b64` 设为 null
    - base64 编码失败时：捕获异常，`cropped_image_b64` 设为 null（参考 design 错误处理表）
    - _Requirements: 3.1, 3.2, 3.3_

- [x] 2. 修改 packager.py 和 requirements.txt
  - [x] 2.1 修改 `packager.py`：`package_model` 新增 `yolo_model_path` 可选参数
    - 提供时将 YOLO 模型文件打包到 model.tar.gz 根目录，arcname 为 `yolo11s.pt`
    - 支持本地路径和 S3 URI（复用现有 `_resolve_path`）
    - _Requirements: 4.1, 4.2_
  - [x] 2.2 修改 `requirements.txt`：新增 `ultralytics` 依赖
    - _Requirements: 5.1_

- [x] 3. 更新 test_endpoint.py：YOLO crop 相关测试
  - [x] 3.1 Property 1: letterbox_resize 输出尺寸不变量 (PBT)
    - **Property 1: letterbox_resize 输出尺寸不变量**
    - 随机宽高 [1, 4000]，验证 `_letterbox_resize` 输出尺寸恒为 (target_size, target_size)
    - **Validates: Requirements 2.2, 8.1**
  - [x] 3.2 Property 2: 内联 letterbox_resize 与 cleaning 版等价性 (PBT)
    - **Property 2: 内联 letterbox_resize 与 cleaning 版等价性**
    - 随机宽高 [1, 4000]，对比 `inference._letterbox_resize` 和 `cleaning.cleaner.letterbox_resize` 输出像素一致
    - **Validates: Requirements 2.2**
  - [x] 3.3 Property 3: input_fn 输出结构不变量 (PBT)
    - **Property 3: input_fn 输出结构不变量**
    - 随机图片 + 随机 YOLO 状态（有检测 / 无检测 / 模型不可用），验证返回 dict 含 `tensor`（shape (1, 3, input_size, input_size)）和 `cropped_image`（PIL Image 或 None）
    - mock YOLO 模型，不依赖真实权重
    - **Validates: Requirements 2.1, 2.3, 2.4, 2.7, 2.8, 2.9**
  - [x] 3.4 Property 4: 推理 round-trip 不变量（含 cropped_image_b64）(PBT)
    - **Property 4: 推理 round-trip 不变量（含 cropped_image_b64）**
    - 更新现有 `TestInferenceRoundTrip` 类，适配 input_fn 返回 dict 和 predict_fn 接收 dict 的变更
    - 验证 JSON 响应含 `cropped_image_b64` 字段（合法 base64 字符串或 null）
    - **Validates: Requirements 2.1, 2.3, 3.1, 3.2, 3.3, 3.4, 8.7**
  - [x] 3.5 单元测试：model_fn 加载 YOLO、input_fn 路径、output_fn 编码、打包结构
    - model_fn 加载 YOLO 模型：验证返回字典含 `yolo_model` 键（Requirements 8.5）
    - model_fn YOLO 不存在：验证 `yolo_model` 为 None（Requirements 1.3）
    - input_fn YOLO crop 路径：mock YOLO 返回 bbox，验证 `cropped_image` 非 None 且为 PIL Image (input_size × input_size)（Requirements 8.2）
    - input_fn 回退路径（YOLO 无检测）：验证 `cropped_image` 为 None，tensor shape 正确（Requirements 8.3）
    - input_fn 回退路径（YOLO 不可用）：`_yolo_model` 为 None，验证使用原始预处理流程（Requirements 8.4）
    - input_fn 返回 dict 结构：验证返回值包含 `tensor` 和 `cropped_image` 键（Requirements 8.9）
    - output_fn cropped_image_b64：YOLO crop 路径为合法 base64 字符串，回退路径为 null（Requirements 8.10）
    - model.tar.gz 打包含 yolo11s.pt：验证 tar.gz 根目录包含 `yolo11s.pt`（Requirements 8.6）
    - 所有测试使用 mock YOLO 模型，不依赖真实权重（Requirements 8.8）
    - 更新现有 `TestInputFnShapeInvariant` 适配 input_fn 返回 dict 的变更
    - _Requirements: 8.1, 8.2, 8.3, 8.4, 8.5, 8.6, 8.8, 8.9, 8.10_

- [x] 4. Checkpoint — 验证 endpoint 测试通过
  - 运行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_endpoint.py -v`
  - 确保所有测试通过（含现有 Spec 17 测试和新增 Spec 30 测试），ask the user if questions arise.

- [x] 5. 修改 handler.py：crop 图片提取和 S3 上传
  - [x] 5.1 修改推理结果处理循环：提取 `cropped_image_b64`，base64 解码后上传 S3
    - 新增 `import base64`
    - 在逐张推理循环内，从 `result_body` 提取 `cropped_image_b64` 字段
    - crop S3 key 构造：`jpg_key.rsplit(".", 1)[0] + "_cropped.jpg"`（原始文件名去掉扩展名 + `_cropped.jpg`）
    - 上传使用 `s3_client.put_object(Bucket=bucket, Key=cropped_s3_key, Body=cropped_bytes, ContentType="image/jpeg")`
    - 上传失败或 base64 解码失败：捕获 Exception，记录 warning 并继续，`cropped_s3_key` 设为 None
    - `inference_results` 列表每项新增 `cropped_s3_key` 字段
    - _Requirements: 6.1, 6.2, 6.3, 6.6, 6.7_
  - [x] 5.2 修改 DynamoDB update_item：新增 `inference_cropped_image_key` 字段
    - 在 `best` 不为 None 时，从 `inference_results` 中找到最佳预测对应的 `cropped_s3_key`
    - `update_expr_parts` 新增 `inference_cropped_image_key = :cropped_key`
    - `expr_values[":cropped_key"]` 为对应的 crop S3 key（可能为 None）
    - _Requirements: 6.4, 6.5_

- [x] 6. 更新 test_lambda.py：crop 上传相关测试
  - [x] 6.1 Property 5: crop 文件名转换不变量 (PBT)
    - **Property 5: crop 文件名转换不变量**
    - 注意：现有 test_lambda.py 中 Spec 17 的 Property 4/5 编号不冲突（Spec 30 design 中的 Property 5 是新增的独立 property）
    - 随机 S3 key（以文件扩展名结尾），验证 crop key 等于 `key.rsplit(".", 1)[0] + "_cropped.jpg"`
    - **Validates: Requirements 6.2**
  - [x] 6.2 单元测试：crop 上传、null 跳过、DynamoDB 字段、上传失败容错
    - crop 图片上传：mock endpoint 响应含 `cropped_image_b64`（合法 base64），验证 `s3_client.put_object` 调用参数（Bucket、Key、Body、ContentType）（Requirements 9.1）
    - crop 为 null 跳过：mock endpoint 响应 `cropped_image_b64` 为 null，验证不调用 `s3_client.put_object`（Requirements 9.2）
    - DynamoDB `inference_cropped_image_key`：验证 `table.update_item` 的 UpdateExpression 包含 `inference_cropped_image_key`，ExpressionAttributeValues 包含 `:cropped_key`（Requirements 9.3）
    - crop 上传失败不影响推理：mock `s3_client.put_object` 抛异常，验证推理结果仍写入 DynamoDB 且 `cropped_s3_key` 为 None（Requirements 9.4）
    - 注意：现有 test_lambda.py 的 mock 使用 `put_item`，但 handler.py 实际使用 `update_item`，新增测试应 mock `update_item`
    - _Requirements: 9.1, 9.2, 9.3, 9.4_

- [x] 7. 修改 deploy-inference-pipeline.sh：Lambda IAM 权限更新
  - 在 `_put_lambda_policy` 函数中，S3 权限从 `"Action": "s3:GetObject"` 变为 `"Action": ["s3:GetObject", "s3:PutObject"]`
  - 资源限定不变：`arn:aws:s3:::${S3_CAPTURES_BUCKET}/*`
  - _Requirements: 7.1, 7.2_

- [x] 8. Final checkpoint — 验证全部测试通过
  - 运行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_endpoint.py model/tests/test_lambda.py -v`
  - 确保所有测试通过，ask the user if questions arise.

## Notes

- 所有 Python 命令必须先 `source .venv-raspi-eye/bin/activate`
- 不使用 `cat <<` heredoc 写入文件
- 不在子代理最终检查点执行 git commit
- 不将 .pt 文件提交到 git
- 测试使用 mock YOLO 模型，不依赖真实权重
- 不在不确定 ultralytics YOLO API 用法时凭猜测编写代码
- Property tests 使用 Hypothesis，每个 ≥ 100 iterations
- Tasks marked with `*` are optional and can be skipped for faster MVP
- Property tests validate universal correctness properties, unit tests validate specific examples and edge cases

## 部署后修复（Spec 外改动）

以下改动在部署验证阶段发现并修复，不在原始 tasks 范围内：

1. **handler.py: DynamoDB top5 float → Decimal**：`expr_values[":top5"]` 中 predictions 列表的 confidence 是 JSON 解析出来的 float，DynamoDB 不接受。修复：逐项转为 `Decimal(str(round(p["confidence"], 6)))`
2. **S3 bucket region 不匹配**：model.tar.gz 需要上传到与 SageMaker endpoint 同 region（ap-southeast-1）的 bucket，而非 us-east-1 的 `raspi-eye-model-data`
3. **Lambda 函数名变更**：从 `raspi-eye-inference` 改为 `fn_raspi_eye_verifier`（统一 fn_ 前缀命名规范），deploy-inference-pipeline.sh 中 LAMBDA_FUNCTION 和 LAMBDA_ROLE_NAME 已更新
