# 需求文档：Spec 31 — Lambda 端多图投票与置信度门槛

## 简介

当前 Lambda `handler.py` 的 `select_best_prediction` 逻辑从所有图片的所有预测中选全局最高 confidence，缺乏跨图片投票机制。一张模糊图片可能因模型过拟合给出高 confidence 的错误预测。同时没有置信度门槛，低 confidence 结果也直接写入 DynamoDB。

本 Spec 实现三项改进：
1. 多图投票（方案 A）：每张图片取 top-1 预测的 species，统计多数投票，投票胜出的 species + 该 species 在所有图片中的最高 confidence 作为最终结果。票数不足 2 时回退到原始最高 confidence 逻辑。
2. 置信度门槛（方案 C）：最终 confidence < 0.5 时标记 `inference_species = "uncertain"`，DynamoDB 新增 `inference_reliable` 和 `inference_vote_count` 字段。
3. 云端 YOLO 升级为 yolo11x + 阈值提高到 0.4：更精准的鸟体检测，云端未检测到鸟的图片不参与投票，有效过滤设备端误报。

## 前置条件

- Spec 30（推理链路 YOLO Crop 对齐）已完成 ✅

## 术语表

- **Inference_Lambda**：Lambda 函数（`model/lambda/handler.py`），接收 S3 事件通知，读取截图，调用 SageMaker endpoint 推理，将结果写入 DynamoDB
- **Majority_Vote**：多数投票机制，每张图片取 top-1 预测的 species，统计出现次数最多的 species 作为最终结果
- **Vote_Count**：投票胜出的 species 在所有图片中获得的票数
- **Confidence_Threshold**：置信度门槛，固定值 0.5，低于此值的推理结果标记为不可靠
- **Fallback_Selection**：回退选择逻辑，当没有 species 获得 >= 2 票时，使用原始的全局最高 confidence 逻辑

## 约束条件

| 约束 | 值 |
|------|-----|
| 语言 | Python >= 3.11 |
| 环境管理 | venv（`.venv-raspi-eye/`），所有命令必须先 `source .venv-raspi-eye/bin/activate` |
| 测试框架 | pytest + Hypothesis（PBT） |
| 涉及文件 | 5 个（`model/lambda/handler.py`、`model/tests/test_lambda.py`、`model/endpoint/inference.py`、`model/endpoint/packager.py`、`model/tests/test_endpoint.py`） |
| DynamoDB 数值类型 | 所有数值必须转为 `Decimal(str(round(value, 6)))`，包括 `inference_vote_count` |
| Lambda 函数名 | `fn_raspi_eye_verifier` |
| 置信度门槛 | 0.5（硬编码常量） |
| 投票回退条件 | 没有 species 获得 >= 2 票 |
| 云端 YOLO 模型 | yolo11x.pt（~140MB），替换 yolo11s.pt（~22MB） |
| 云端 YOLO 检测阈值 | 0.4（从 0.3 提高，与训练时 Spec 28 一致） |

## 明确不包含

- 设备端代码改动（不动 device/ 目录）
- 部署脚本改动（不动 `scripts/deploy-inference-pipeline.sh`）
- DynamoDB 表结构迁移脚本（新字段通过 update_item 动态添加）
- 前端展示 uncertain 状态的 UI 逻辑
- 训练代码改动（Spec 28 清洗时已用 yolo11x，训练数据不受影响）

## 禁止项

- SHALL NOT 在 Lambda handler 中将 JSON 解析出的 float 直接写入 DynamoDB（来源：spec-30 部署验证）
  - 原因：DynamoDB 不支持 Python float 类型
  - 建议：所有写入 DynamoDB 的数值必须先转为 `Decimal(str(round(value, 6)))`，包括 `inference_vote_count`

## 需求

### Requirement 1: 多图投票选择逻辑

**User Story:** 作为系统运维者，我希望推理结果基于多张图片的投票共识而非单张图片的最高 confidence，以减少模糊图片导致的错误识别。

#### Acceptance Criteria

1. WHEN 多张图片的推理结果可用，THE Inference_Lambda SHALL 从每张图片的 predictions 列表中取 top-1（confidence 最高的）预测的 species 作为该图片的投票
2. WHEN 所有图片的投票已收集，THE Inference_Lambda SHALL 统计每个 species 的出现次数，选择出现次数最多的 species 作为 Majority_Vote 胜出者
3. WHEN 存在两个或多个 species 票数相同且均为最高票数，THE Inference_Lambda SHALL 选择其中全局最高 confidence 的 species 作为 Majority_Vote 胜出者
4. WHEN Majority_Vote 胜出者确定，THE Inference_Lambda SHALL 将该 species 在参与投票的图片中的最高 confidence 作为最终 confidence
5. WHEN 没有任何 species 获得 >= 2 票，THE Inference_Lambda SHALL 使用 Fallback_Selection 逻辑（从所有图片所有预测中选全局最高 confidence 的预测）
6. WHEN 仅有 1 张图片的推理结果可用，THE Inference_Lambda SHALL 使用 Fallback_Selection 逻辑

### Requirement 2: 置信度门槛标记

**User Story:** 作为系统运维者，我希望低置信度的推理结果被明确标记，以便后续人工审核或过滤。

#### Acceptance Criteria

1. WHEN 最终 confidence >= Confidence_Threshold（0.5），THE Inference_Lambda SHALL 将 `inference_species` 设为投票或回退选出的 species 名称，`inference_reliable` 设为 `true`
2. WHEN 最终 confidence < Confidence_Threshold（0.5），THE Inference_Lambda SHALL 将 `inference_species` 设为 `"uncertain"`，`inference_reliable` 设为 `false`
3. THE Inference_Lambda SHALL 将 `inference_reliable` 作为 boolean 类型写入 DynamoDB

### Requirement 3: DynamoDB 新增字段

**User Story:** 作为系统运维者，我希望 DynamoDB 记录包含投票统计信息，以便分析推理质量。

#### Acceptance Criteria

1. THE Inference_Lambda SHALL 在 DynamoDB update_item 中新增 `inference_vote_count` 字段，值为 Majority_Vote 胜出 species 获得的票数
2. THE Inference_Lambda SHALL 在 DynamoDB update_item 中新增 `inference_reliable` 字段，值为 boolean 类型
3. WHEN 使用 Fallback_Selection 逻辑时，THE Inference_Lambda SHALL 将 `inference_vote_count` 设为 `1`
4. THE Inference_Lambda SHALL 将 `inference_vote_count` 转为 `Decimal` 类型后写入 DynamoDB

### Requirement 4: 向后兼容

**User Story:** 作为系统运维者，我希望新逻辑不破坏现有的推理链路和 DynamoDB 记录格式。

#### Acceptance Criteria

1. THE Inference_Lambda SHALL 保留现有的 `inference_species`、`inference_confidence`、`inference_image_key`、`inference_top5`、`inference_latency_ms`、`inference_cropped_image_key` 字段
2. WHEN 推理失败（SageMaker 调用异常），THE Inference_Lambda SHALL 保持现有的 `inference_error` 写入逻辑不变
3. WHEN event.json 格式错误或无 snapshots，THE Inference_Lambda SHALL 保持现有的跳过逻辑不变

### Requirement 5: select_best_prediction 函数签名变更

**User Story:** 作为开发者，我希望 `select_best_prediction` 函数的返回值包含投票信息，以便 handler 写入 DynamoDB。

#### Acceptance Criteria

1. THE `select_best_prediction` 函数 SHALL 接受与当前相同的 `results: list[dict]` 参数
2. THE `select_best_prediction` 函数 SHALL 返回包含 `species`、`confidence`、`image_key`、`top5_predictions`、`latency_ms`、`vote_count` 字段的字典
3. WHEN Majority_Vote 逻辑生效，THE `select_best_prediction` 函数返回的 `vote_count` SHALL 等于胜出 species 的实际票数
4. WHEN Fallback_Selection 逻辑生效，THE `select_best_prediction` 函数返回的 `vote_count` SHALL 等于 1
5. WHEN 置信度门槛生效（confidence < 0.5），THE handler SHALL 在调用 `select_best_prediction` 之后、写入 DynamoDB 之前，将 `inference_species` 替换为 `"uncertain"`

### Requirement 6: 云端 YOLO 模型升级为 yolo11x

**User Story:** 作为系统运维者，我希望云端使用更高精度的 YOLO 模型做鸟体检测，减少设备端误报导致的无效分类。

#### Acceptance Criteria

1. THE Inference_Script SHALL 从 `model_dir` 加载 `yolo11x.pt`（替换 `yolo11s.pt`），使用 `ultralytics.YOLO` 初始化
2. THE Inference_Script 的 `_yolo_crop` SHALL 使用 `conf_threshold=0.4`（从 0.3 提高）
3. THE Model_Packager SHALL 支持打包 YOLO 模型文件，arcname 从 `yolo_model_path` 的文件名推断（不再硬编码 `yolo11s.pt`）
4. WHEN `yolo11x.pt` 不存在于 `model_dir` 时，THE Inference_Script SHALL 回退尝试加载 `yolo11s.pt`，再不存在则 `_yolo_model = None`（向后兼容）
5. THE Inference_Script 的 `BIRD_CLASS_ID` 和 `_yolo_crop` 的 `padding` 参数 SHALL 保持不变（14 和 0.2）

### Requirement 7: 云端 YOLO 未检测到鸟时跳过分类

**User Story:** 作为系统运维者，我希望云端 YOLO 未检测到鸟的图片不参与投票，避免设备端误报污染投票结果。

#### Acceptance Criteria

1. WHEN 云端 YOLO 未检测到鸟（endpoint 响应中 `cropped_image_b64` 为 null），THE Inference_Lambda SHALL 在 `inference_results` 中标记该图片 `has_bird = False`，不参与 `select_best_prediction` 的投票
2. WHEN 所有图片均未检测到鸟，THE Inference_Lambda SHALL 将 `inference_species` 设为 `"no_bird_detected"`，`inference_reliable` 设为 `false`，`inference_vote_count` 设为 `0`
3. THE Inference_Lambda SHALL 在 DynamoDB 中记录实际参与投票的图片数量（通过 `inference_vote_count` 间接体现）
