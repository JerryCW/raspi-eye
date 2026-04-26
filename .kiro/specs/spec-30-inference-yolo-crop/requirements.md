# 需求文档：Spec 30 — 推理链路 YOLO Crop 对齐

## 简介

本 Spec 在 SageMaker endpoint 的推理链路中加入 YOLO crop 步骤，使推理时的图片预处理流程与训练时一致，并将 crop 后的图片通过 Lambda 上传到 S3 供审核使用。

Spec 28（特征空间清洗）训练数据准备时，先用 YOLO 11x 检测鸟体 bbox → crop + padding 20% → letterbox resize 518×518，然后才送入 DINOv3 做特征提取和训练。但 Spec 17 当前推理时，直接把 S3 原始截图送入 inference.py，只做 Resize(518) + CenterCrop(518) + Normalize，没有 YOLO crop 步骤。这导致训练和推理的输入分布不一致（train-serving skew），分类效果打折扣。

方案核心：
1. 在 inference.py 的 `input_fn` 中加入 YOLO crop 步骤，使用轻量 YOLO 模型（yolo11s，~22MB）做 bbox 检测，crop 逻辑与训练时一致。YOLO 模型文件打包进 model.tar.gz，model_fn 加载时同时加载 YOLO 模型。
2. `input_fn` 返回 dict（包含预处理张量 + crop 后的 PIL Image），`output_fn` 将 crop 图片 base64 编码后放入 JSON 响应的 `cropped_image_b64` 字段。
3. Lambda 从 endpoint 响应中提取 `cropped_image_b64`，base64 解码后上传到原始截图同目录（文件名加 `_cropped` 后缀），DynamoDB 写回时增加 `inference_cropped_image_key` 字段。主要目的是审核，后期可能加前端展示。

## 前置条件

- Spec 17（SageMaker endpoint）已完成，推理链路正常工作 ✅
- Spec 28（特征空间清洗）已完成，训练数据使用 YOLO crop 预处理 ✅
- Spec 29（鸟类分类模型训练）已完成，模型已部署 ✅
- yolo11s.pt 模型文件可从 ultralytics 官方获取

## 术语表

- **Inference_Script**：推理脚本（`model/endpoint/inference.py`），实现 SageMaker PyTorch Inference Toolkit 的四个钩子函数，完全自包含，在 SageMaker 推理容器内执行
- **Model_Packager**：模型打包模块（`model/endpoint/packager.py`），将模型文件和推理代码打包为 `model.tar.gz`
- **YOLO_Detector**：推理脚本内联的 YOLO 鸟体检测逻辑，使用 yolo11s 模型检测 COCO class 14（bird）的 bounding box
- **Letterbox_Resize**：等比缩放 + 黑色填充到目标尺寸的图片变换，与训练时 `cleaning/cleaner.py` 中的 `letterbox_resize` 逻辑等效
- **Crop_Pipeline**：YOLO 检测 → 取最高置信度 bbox → padding 20% → letterbox resize 518×518 → Normalize 的完整预处理流程
- **Fallback_Pipeline**：YOLO 未检测到鸟体时的回退预处理流程，即原来的 Resize(518) + CenterCrop(518) + Normalize
- **Inference_Lambda**：Lambda 函数（`model/lambda/handler.py`），接收 S3 事件通知，读取截图，调用 SageMaker endpoint 推理，提取 crop 图片上传 S3，将结果写入 DynamoDB
- **Deploy_Script**：一键部署脚本（`scripts/deploy-inference-pipeline.sh`），部署整条云端推理链路

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言 | Python >= 3.11 |
| 环境管理 | venv（`.venv-raspi-eye/`），所有命令必须先 `source .venv-raspi-eye/bin/activate` |
| 测试框架 | pytest + Hypothesis（PBT） |
| 代码目录 | `model/endpoint/`（推理脚本和打包逻辑）、`model/lambda/`（Lambda 函数代码） |
| 测试文件 | `model/tests/test_endpoint.py`、`model/tests/test_lambda.py` |
| 推理容器 | PyTorch 2.6 CPU 预构建推理容器（`pytorch-inference:2.6.0-cpu-py312-ubuntu22.04-sagemaker`） |
| Serverless 内存 | 6144 MB（DINOv3 ~1.2GB + yolo11s ~22MB，总计 ~1.22GB，6GB 完全够用） |
| YOLO 模型 | yolo11s.pt（~22MB），轻量版，仅需定位鸟体位置 |
| YOLO crop 参数 | conf_threshold=0.3, padding=0.2, BIRD_CLASS_ID=14（与 Spec 28 训练时一致） |
| inference.py | 完全自包含，不依赖 cleaning 模块 |
| S3 bucket（设备截图） | `raspi-eye-captures-014498626607-ap-southeast-1-an` |
| S3 截图路径格式 | `{device_id}/{date}/{event_id}/{filename}` |
| crop 图片响应体大小 | 一张 518×518 JPEG 约 50-100KB，base64 后约 70-140KB |
| SageMaker 响应体限制 | 6MB（Serverless endpoint），单张 crop 图片 base64 约 70-140KB，远低于限制 |
| 涉及文件 | 7 个（inference.py、packager.py、requirements.txt、test_endpoint.py、handler.py、test_lambda.py、deploy-inference-pipeline.sh） |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中修改训练代码或模型格式（Spec 28/29 已完成）
- SHALL NOT 使用 yolo11x（~140MB）做推理检测（yolo11s ~22MB 足够定位鸟体位置，不需要 x 版本的精度）
- SHALL NOT 在 SageMaker endpoint 中直接写 S3（crop 图片由 Lambda 负责上传，endpoint 只返回 base64 编码的 JPEG bytes）

### Design 层

- SHALL NOT 在推理脚本中使用 OpenCV 做图片处理（使用 Pillow，保持与 Spec 28/29 一致）
- SHALL NOT 在推理脚本中 import cleaning 模块（inference.py 必须完全自包含，SageMaker 容器内没有 cleaning 模块）
- SHALL NOT 在 YOLO 未检测到鸟体时抛出异常（回退到原来的 Resize + CenterCrop 流程，保证兼容性）

### Tasks 层

- SHALL NOT 直接使用系统 `python` 或 `python3` 执行项目 Python 代码或测试，必须先 `source .venv-raspi-eye/bin/activate`
- SHALL NOT 将模型权重文件（.pt、model.tar.gz）提交到 git
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 在子代理最终检查点执行 git commit

## 需求

### 需求 1：model_fn 加载 YOLO 模型

**用户故事：** 作为推理系统，我需要在模型加载阶段同时加载 YOLO 检测模型，以便在推理时对输入图片进行鸟体裁切。

#### 验收标准

1. WHEN `model_fn` 加载模型时，THE Inference_Script SHALL 从 `model_dir` 加载 `yolo11s.pt` 文件，使用 `ultralytics.YOLO` 初始化 YOLO 检测模型
2. THE Inference_Script SHALL 将加载的 YOLO 模型存入返回字典的 `yolo_model` 键中，供 `input_fn` 使用
3. IF `model_dir` 中不存在 `yolo11s.pt` 文件，THEN THE Inference_Script SHALL 记录警告日志并将 `yolo_model` 设为 None（回退到无 YOLO crop 的原始流程）

### 需求 2：input_fn YOLO crop 预处理

**用户故事：** 作为推理系统，我需要在图片预处理阶段对输入图片进行 YOLO 鸟体检测和裁切，使推理时的预处理流程与训练时一致，并将 crop 后的 PIL Image 传递给后续步骤。

#### 验收标准

1. WHEN YOLO 模型可用且检测到鸟体（COCO class 14，置信度 >= 0.3）时，THE Inference_Script SHALL 取置信度最高的 bounding box，四周扩展 20% padding（clamp 到图片边界），裁切后进行 letterbox resize 到 input_size × input_size，再应用 Normalize 变换
2. THE Inference_Script SHALL 内联 letterbox resize 逻辑（等比缩放 + 黑色填充），与 `cleaning/cleaner.py` 中的 `letterbox_resize` 函数行为等效
3. WHEN YOLO 模型可用但未检测到鸟体时，THE Inference_Script SHALL 回退到原来的 Resize + CenterCrop + Normalize 流程
4. WHEN YOLO 模型不可用（`yolo_model` 为 None）时，THE Inference_Script SHALL 使用原来的 Resize + CenterCrop + Normalize 流程
5. THE Inference_Script SHALL 使用模块级变量存储 YOLO 模型引用，供 `input_fn` 访问（与现有 `_input_size` 模式一致）
6. THE Inference_Script SHALL 在 YOLO 推理时设置 `verbose=False`，避免 ultralytics 输出大量日志。YOLO 推理输入为 PIL Image 对象（非文件路径），ultralytics YOLO 原生支持 PIL Image 输入
7. THE Inference_Script 的 `input_fn` SHALL 返回一个 dict，包含 `tensor` 键（预处理后的张量，shape (1, 3, input_size, input_size)）和 `cropped_image` 键（crop 后的 PIL Image，用于 output_fn 编码返回）
8. WHEN 走 YOLO crop 路径时，`cropped_image` SHALL 为 letterbox resize 后的 PIL Image（518×518）
9. WHEN 走回退路径（YOLO 未检测到鸟体或 YOLO 模型不可用）时，`cropped_image` SHALL 为 None

### 需求 3：output_fn 返回 crop 图片

**用户故事：** 作为推理系统，我需要在 JSON 响应中包含 crop 后的图片（base64 编码），以便 Lambda 提取并上传到 S3 供审核使用。

#### 验收标准

1. THE Inference_Script 的 `output_fn` SHALL 在 JSON 响应中包含 `cropped_image_b64` 字段（base64 编码的 JPEG bytes）
2. WHEN `predict_fn` 的输出中 `cropped_image` 不为 None 时，THE Inference_Script SHALL 将 PIL Image 编码为 JPEG（quality=95），再 base64 编码为字符串，写入 `cropped_image_b64` 字段
3. WHEN `cropped_image` 为 None 时（回退路径），THE Inference_Script SHALL 将 `cropped_image_b64` 设为 null
4. THE Inference_Script 的 `predict_fn` SHALL 将 `input_fn` 返回的 `cropped_image` 透传到输出 dict 中，供 `output_fn` 使用。`predict_fn` 的 `input_data` 参数类型从 `torch.Tensor` 变为 `dict`，需从 `input_data["tensor"]` 提取张量进行推理

### 需求 4：model.tar.gz 打包 YOLO 模型

**用户故事：** 作为部署系统，我需要将 YOLO 模型文件打包进 model.tar.gz，使 SageMaker 容器内可以加载 YOLO 模型。

#### 验收标准

1. THE Model_Packager SHALL 支持通过 `yolo_model_path` 参数指定 YOLO 模型文件路径（本地路径或 S3 URI）
2. WHEN `yolo_model_path` 提供时，THE Model_Packager SHALL 将 YOLO 模型文件打包到 model.tar.gz 根目录，arcname 为 `yolo11s.pt`

### 需求 5：推理依赖更新

**用户故事：** 作为部署系统，我需要在推理依赖中加入 ultralytics，使 SageMaker 容器能运行 YOLO 检测。

#### 验收标准

1. THE Inference_Script 的 `requirements.txt` SHALL 包含 `ultralytics` 依赖声明

### 需求 6：Lambda 上传 crop 图片到 S3

**用户故事：** 作为推理链路，我需要 Lambda 从 endpoint 响应中提取每张截图的 crop 后图片并上传到 S3，存到原始截图同目录下，供后续审核和前端展示使用。

#### 验收标准

1. THE Inference_Lambda SHALL 对每张截图调用 endpoint 后，从响应中提取 `cropped_image_b64` 字段
2. WHEN `cropped_image_b64` 不为 null 时，THE Inference_Lambda SHALL 将其 base64 解码为 JPEG bytes，上传到原始截图同目录下的 S3 路径，文件名为原始文件名去掉扩展名后加 `_cropped.jpg` 后缀（如原始文件 `20260412_153046_001.jpg` → `20260412_153046_001_cropped.jpg`）。每张截图的 crop 图片都独立上传
3. WHEN `cropped_image_b64` 为 null 时（回退路径），THE Inference_Lambda SHALL 跳过该张图片的 crop 上传
4. THE Inference_Lambda SHALL 在 DynamoDB `update_item` 写回时增加 `inference_cropped_image_key` 字段，值为最佳预测（`select_best_prediction` 选出的最高置信度）对应的那张 crop 图片的完整 S3 key
5. WHEN 最佳预测对应的截图没有 crop 图片（`cropped_image_b64` 为 null）时，THE Inference_Lambda SHALL 将 `inference_cropped_image_key` 设为 null
6. THE Inference_Lambda SHALL 使用 `s3:PutObject` 上传 crop 图片，ContentType 设为 `image/jpeg`
7. IF crop 图片 S3 上传失败，THEN THE Inference_Lambda SHALL 记录警告日志并继续处理（不影响推理结果写入 DynamoDB，不抛出异常）

### 需求 7：Lambda IAM 权限更新

**用户故事：** 作为部署系统，我需要更新 Lambda 执行角色的 IAM 权限，增加 S3 写入权限以支持 crop 图片上传。

#### 验收标准

1. THE Deploy_Script SHALL 为 Lambda 执行角色增加 `s3:PutObject` 权限（之前仅有 `s3:GetObject`），资源限定为截图 bucket（`raspi-eye-captures-014498626607-ap-southeast-1-an`）
2. THE Deploy_Script SHALL 在 `scripts/deploy-inference-pipeline.sh` 中更新 Lambda IAM 内联策略，S3 权限从 `s3:GetObject` 变为 `s3:GetObject` + `s3:PutObject`

### 需求 8：Endpoint 单元测试更新

**用户故事：** 作为开发者，我需要更新单元测试以覆盖 YOLO crop 逻辑和 crop 图片返回逻辑，确保新增的预处理步骤和输出格式正确无误。

#### 验收标准

1. THE Test_Suite SHALL 包含 letterbox resize 输出尺寸不变量测试（PBT）：对任意合法图片尺寸（宽高 ∈ [1, 4000]），letterbox resize 输出尺寸恒为 (target_size, target_size)
2. THE Test_Suite SHALL 包含 YOLO crop + letterbox resize 预处理路径测试：模拟 YOLO 检测到鸟体 → crop + padding → letterbox resize → Normalize，验证输出张量 shape 为 (1, 3, input_size, input_size)
3. THE Test_Suite SHALL 包含 YOLO 未检测到鸟体的回退测试：模拟 YOLO 返回空结果 → 验证回退到 Resize + CenterCrop + Normalize 流程，输出张量 shape 正确
4. THE Test_Suite SHALL 包含 YOLO 模型不可用的回退测试：`yolo_model` 为 None → 验证使用原始预处理流程
5. THE Test_Suite SHALL 包含 model_fn 加载 YOLO 模型测试：验证返回字典包含 `yolo_model` 键
6. THE Test_Suite SHALL 包含 model.tar.gz 打包结构测试：验证 tar.gz 根目录包含 `yolo11s.pt` 文件
7. THE Test_Suite SHALL 更新现有的推理 round-trip PBT 测试，确保在 YOLO crop 路径下仍满足所有输出不变量
8. THE Test_Suite SHALL 不依赖真实 YOLO 模型权重（使用 mock YOLO 模型返回预设 bbox），确保本地 CPU 环境可运行
9. THE Test_Suite SHALL 包含 input_fn 返回 dict 结构测试：验证返回值包含 `tensor` 和 `cropped_image` 键
10. THE Test_Suite SHALL 包含 output_fn cropped_image_b64 测试：验证 YOLO crop 路径下 JSON 响应包含非 null 的 `cropped_image_b64` 字段（合法 base64 字符串），回退路径下为 null
11. THE Test_Suite SHALL 使用 pytest 运行，命令为 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_endpoint.py -v`

### 需求 9：Lambda 单元测试更新

**用户故事：** 作为开发者，我需要更新 Lambda 单元测试以覆盖 crop 图片提取和 S3 上传逻辑，确保新增功能正确无误。

#### 验收标准

1. THE Test_Suite SHALL 包含 crop 图片上传测试：验证从 endpoint 响应中提取 `cropped_image_b64`，base64 解码后通过 `s3:PutObject` 上传到正确的 S3 路径（原始文件名加 `_cropped.jpg` 后缀）
2. THE Test_Suite SHALL 包含 crop 图片为 null 的跳过测试：验证 `cropped_image_b64` 为 null 时不调用 `s3:PutObject`，`inference_cropped_image_key` 写入 DynamoDB 时为 null
3. THE Test_Suite SHALL 包含 DynamoDB 写回 `inference_cropped_image_key` 字段测试：验证 `update_item` 的 UpdateExpression 包含该字段
4. THE Test_Suite SHALL 不依赖真实 AWS 服务（使用 mock），确保本地环境可运行
5. THE Test_Suite SHALL 使用 pytest 运行，命令为 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_lambda.py -v`

## 参考代码

### 训练时的 crop 逻辑（model/cleaning/cropper.py — 只读参考）

```python
BIRD_CLASS_ID = 14  # COCO class 14 = bird

def crop_bird(image_path, model, conf_threshold=0.3, padding=0.2, min_bbox_ratio=0.01):
    img = Image.open(image_path).convert("RGB")
    results = model(image_path, verbose=False)
    bird_boxes = [
        box for box in results[0].boxes
        if int(box.cls) == BIRD_CLASS_ID and float(box.conf) >= conf_threshold
    ]
    if not bird_boxes:
        return None
    best = max(bird_boxes, key=lambda b: float(b.conf))
    x1, y1, x2, y2 = best.xyxy[0].tolist()
    w, h = x2 - x1, y2 - y1
    x1 = max(0, x1 - w * padding)
    y1 = max(0, y1 - h * padding)
    x2 = min(img.width, x2 + w * padding)
    y2 = min(img.height, y2 + h * padding)
    cropped = img.crop((int(x1), int(y1), int(x2), int(y2)))
    return letterbox_resize(cropped, 518)
```

### letterbox resize 逻辑（model/cleaning/cleaner.py — 只读参考）

```python
def letterbox_resize(image, target_size):
    w, h = image.size
    scale = target_size / max(w, h)
    new_w, new_h = max(1, min(int(w * scale), target_size)), max(1, min(int(h * scale), target_size))
    resized = image.resize((new_w, new_h), Image.LANCZOS)
    canvas = Image.new("RGB", (target_size, target_size), color=(0, 0, 0))
    paste_x, paste_y = (target_size - new_w) // 2, (target_size - new_h) // 2
    canvas.paste(resized, (paste_x, paste_y))
    return canvas
```

### input_fn 返回 dict 结构（新增）

```python
def input_fn(request_body: bytes, content_type: str) -> dict:
    """反序列化输入：JPEG 二进制 → dict(tensor, cropped_image)。

    SageMaker Inference Toolkit 的 input_fn 签名固定返回一个对象给 predict_fn，
    返回 dict 可以同时传递预处理张量和 crop 后的 PIL Image。
    """
    # ... YOLO crop 或 fallback 预处理 ...
    return {
        "tensor": tensor.unsqueeze(0),       # shape: (1, 3, input_size, input_size)
        "cropped_image": cropped_pil_image,   # PIL Image 或 None（回退路径）
    }
```

### predict_fn 透传 cropped_image（新增）

```python
def predict_fn(input_data: dict, model_dict: dict) -> dict:
    """执行推理，透传 cropped_image 给 output_fn。"""
    tensor = input_data["tensor"]
    cropped_image = input_data["cropped_image"]
    # ... 模型推理 ...
    return {
        "predictions": predictions,
        "model_metadata": {...},
        "cropped_image": cropped_image,  # 透传给 output_fn
    }
```

### output_fn 编码 crop 图片（新增）

```python
import base64

def output_fn(prediction: dict, accept: str) -> tuple[str, str]:
    """序列化输出：prediction dict → JSON（含 cropped_image_b64）。"""
    cropped_image = prediction.pop("cropped_image", None)
    cropped_b64 = None
    if cropped_image is not None:
        buf = io.BytesIO()
        cropped_image.save(buf, format="JPEG", quality=95)
        cropped_b64 = base64.b64encode(buf.getvalue()).decode("ascii")
    prediction["cropped_image_b64"] = cropped_b64
    return json.dumps(prediction, ensure_ascii=False), "application/json"
```

### Lambda 提取 crop 图片并上传 S3（新增）

```python
import base64

# 在推理结果处理后：
result_body = json.loads(response["Body"].read())
cropped_b64 = result_body.get("cropped_image_b64")
cropped_s3_key = None

if cropped_b64:
    # 原始文件名: 20260412_153046_001.jpg → 20260412_153046_001_cropped.jpg
    base_name = jpg_key.rsplit(".", 1)[0]  # 去掉 .jpg
    cropped_s3_key = base_name + "_cropped.jpg"
    cropped_bytes = base64.b64decode(cropped_b64)
    s3_client.put_object(
        Bucket=bucket,
        Key=cropped_s3_key,
        Body=cropped_bytes,
        ContentType="image/jpeg",
    )
```

### DynamoDB 记录结构（更新后）

```json
{
    "device_id": "RaspiEyeAlpha",
    "start_time": "2026-04-12T15:30:45Z",
    "inference_species": "Passer montanus",
    "inference_confidence": 0.92,
    "inference_image_key": "RaspiEyeAlpha/2026-04-12/evt_20260412_153045/20260412_153046_001.jpg",
    "inference_cropped_image_key": "RaspiEyeAlpha/2026-04-12/evt_20260412_153045/20260412_153046_001_cropped.jpg",
    "inference_top5": [
        {"species": "Passer montanus", "confidence": 0.92},
        {"species": "Pycnonotus sinensis", "confidence": 0.05}
    ],
    "inference_latency_ms": 3200,
    "inference_error": null
}
```

### model.tar.gz 目录结构（更新后）

```
model.tar.gz
├── bird_classifier.pt          # 分类模型（state_dict + 元数据）
├── class_names.json            # 类别映射
├── yolo11s.pt                  # YOLO 检测模型（~22MB，新增）
└── code/
    ├── inference.py            # 推理脚本（含 YOLO crop 逻辑 + crop 图片返回）
    └── requirements.txt        # 推理依赖（含 ultralytics）
```

## 验证命令

```bash
# 激活 venv
source .venv-raspi-eye/bin/activate

# 运行 endpoint 单元测试（离线，不依赖 GPU 或网络）
pytest model/tests/test_endpoint.py -v

# 运行 Lambda 单元测试（离线，使用 mock）
pytest model/tests/test_lambda.py -v
```

预期结果：所有测试通过，包括新增的 YOLO crop 相关测试、crop 图片返回测试、Lambda crop 上传测试和更新后的 round-trip PBT 测试。

## 明确不包含

- 前端 UI 展示 crop 图片（后期可能加，本 Spec 不包含）
- 训练代码或模型格式修改（Spec 28/29 已完成）
- deploy_endpoint.py 修改（model.tar.gz 结构变化对部署脚本透明，仅 packager.py 需要更新）
- YOLO 模型训练或微调（使用 ultralytics 官方预训练 yolo11s）
- 设备端 YOLO 检测逻辑修改（设备端使用 ONNX Runtime，与云端 PyTorch 推理无关）
- min_bbox_ratio 检查（训练时用于丢弃鸟体太小的图片，推理时不需要丢弃，直接回退即可）
