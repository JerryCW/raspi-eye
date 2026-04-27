# Implementation Plan: Spec 31 — Lambda 端多图投票与置信度门槛

## Overview

在 Lambda 推理链路中引入多数投票选择逻辑、置信度门槛标记、云端 YOLO 升级（yolo11x + 阈值 0.4）、packager 动态 arcname。涉及 5 个文件：handler.py、test_lambda.py、inference.py、packager.py、test_endpoint.py。所有改动使用 Python，测试通过 pytest + Hypothesis 运行。

## Tasks

- [ ] 1. 修改 inference.py：YOLO 模型回退加载 + 阈值提高
  - [ ] 1.1 修改 `model_fn`：YOLO 加载优先级 yolo11x → yolo11s → None
    - 优先加载 `yolo11x.pt`，不存在时回退 `yolo11s.pt`，都不存在则 `_yolo_model = None`
    - 替换现有的单一 `yolo11s.pt` 加载逻辑
    - _Requirements: 6.1, 6.4_
  - [ ] 1.2 修改 `_yolo_crop`：`conf_threshold` 默认值从 0.3 提高到 0.4
    - `BIRD_CLASS_ID = 14` 和 `padding = 0.2` 保持不变
    - _Requirements: 6.2, 6.5_

- [ ] 2. 修改 packager.py：arcname 动态推断
  - [ ] 2.1 修改 `package_model`：YOLO arcname 从 `yolo_model_path` 文件名推断
    - `_resolve_path` 的 filename 参数改为 `os.path.basename(yolo_model_path)`
    - `tar.add` 的 arcname 改为 `os.path.basename(yolo_model_path)`（不再硬编码 `yolo11s.pt`）
    - _Requirements: 6.3_

- [ ] 3. 更新 test_endpoint.py：YOLO 回退 + 阈值 + packager 测试
  - [ ] 3.1 单元测试：model_fn YOLO 回退加载
    - model_dir 中无 yolo11x.pt 但有 yolo11s.pt → 验证加载 yolo11s.pt（`_yolo_model` 非 None）
    - model_dir 中同时有 yolo11x.pt 和 yolo11s.pt → 验证加载 yolo11x.pt（优先级）
    - model_dir 中两者都没有 → 验证 `_yolo_model = None`（现有测试已覆盖，确认不回归）
    - _Requirements: 6.1, 6.4_
  - [ ] 3.2 单元测试：_yolo_crop 阈值测试
    - 验证 `_yolo_crop` 函数签名中 `conf_threshold` 默认值为 0.4
    - _Requirements: 6.2_
  - [ ] 3.3 单元测试：packager 动态 arcname 测试
    - 传入 `yolo11x.pt` 路径 → 验证 tar.gz 中 arcname 为 `yolo11x.pt`（不是 `yolo11s.pt`）
    - 传入 `yolo11s.pt` 路径 → 验证 tar.gz 中 arcname 为 `yolo11s.pt`（向后兼容）
    - _Requirements: 6.3_

- [ ] 4. Checkpoint — 验证 endpoint 测试通过
  - 运行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_endpoint.py -v`
  - 确保所有测试通过（含现有 Spec 17/30 测试和新增 Spec 31 测试），ask the user if questions arise.


- [ ] 5. 修改 handler.py：投票逻辑重构 + has_bird 过滤 + 置信度门槛 + DynamoDB 新字段
  - [ ] 5.1 新增模块级常量 `CONFIDENCE_THRESHOLD = 0.5`，新增 `from collections import Counter`
    - _Requirements: 2.1_
  - [ ] 5.2 重构 `select_best_prediction`：多数投票 + 回退逻辑
    - 每张图片取 top-1 species 作为投票
    - 统计票数，最高票数 >= 2 时投票生效，返回胜出 species + 该 species 在 votable_results 所有预测中的最高 confidence
    - 平票时选全局最高 confidence 的 species 打破平票
    - 票数不足 2 时回退到全局最高 confidence，vote_count = 1
    - 返回值新增 `vote_count` 字段
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 5.1, 5.2, 5.3, 5.4_
  - [ ] 5.3 修改推理循环：新增 `has_bird` 标记（基于 `cropped_image_b64` 是否为 null，不是 `cropped_s3_key`）
    - `inference_results` 每项新增 `has_bird` 字段
    - 投票前过滤：`votable_results = [r for r in inference_results if r.get("has_bird", False)]`
    - _Requirements: 7.1_
  - [ ] 5.4 修改 DynamoDB 写入：置信度门槛判断 + 新增字段
    - `votable_results` 非空时：调用 `select_best_prediction(votable_results)`，判断 confidence >= 0.5 → reliable=true / < 0.5 → species="uncertain", reliable=false
    - 所有图片无鸟时（`votable_results` 为空且 `inference_results` 非空）：写入 `no_bird_detected`、confidence=0、image_key="N/A"、reliable=false、vote_count=0
    - UpdateExpression 新增 `inference_reliable` 和 `inference_vote_count`
    - `:reliable` 为 bool，`:vote_count` 为 `Decimal(str(vote_count))`
    - 所有 DynamoDB 数值必须转为 `Decimal(str(round(value, 6)))`
    - _Requirements: 2.1, 2.2, 2.3, 3.1, 3.2, 3.3, 3.4, 5.5, 7.2_

- [ ] 6. 更新 test_lambda.py：投票 PBT + 门槛单元测试 + 更新现有 PBT
  - [ ] 6.1 更新现有 `TestSelectBestPredictionInvariant` PBT
    - 返回值新增 `vote_count` 字段断言
    - 原有 "confidence == 全局最大值" 断言仅在回退路径成立；投票路径下 confidence 是胜出 species 在 votable_results 所有预测中的最高 confidence（不一定是全局最大值）
    - 拆分为两个独立断言路径：投票生效时验证 vote_count >= 2 + species 票数最高；回退时验证 confidence == 全局最大值 + vote_count == 1
    - _Requirements: 1.1, 1.5, 5.2, 5.4_
  - [ ]* 6.2 Property 1: 投票胜出不变量 (PBT, Hypothesis >= 100 iterations)
    - **Property 1: 投票胜出不变量**
    - 生成 2-10 张图片、每张 1-5 个预测，确保存在 species 票数 >= 2
    - 验证：返回的 species 票数 >= 所有其他 species 票数；confidence 等于该 species 在所有预测中的最高 confidence；vote_count 等于实际票数
    - **Validates: Requirements 1.1, 1.2, 1.3, 1.4, 5.2, 5.3**
  - [ ]* 6.3 Property 2: 回退不变量 (PBT, Hypothesis >= 100 iterations)
    - **Property 2: 回退不变量**
    - 生成 1-10 张图片（确保无 species 票数 >= 2），验证回退逻辑
    - 验证：confidence 等于全局最高 confidence；species 等于全局最高 confidence 对应的 species；vote_count == 1
    - **Validates: Requirements 1.5, 1.6, 3.3, 5.4**
  - [ ]* 6.4 单元测试：投票场景（12 个测试）
    - 投票场景 — 3 张图片中 2 张 top-1 为同一 species：验证返回该 species 及正确 vote_count
    - 平票场景 — 2 个 species 各 2 票：验证选全局最高 confidence 的 species
    - 回退场景 — 3 张图片 top-1 各不同：验证回退到全局最高 confidence，vote_count = 1
    - 单图场景 — 仅 1 张图片：验证回退逻辑，vote_count = 1
    - 置信度门槛 — confidence >= 0.5：验证 DynamoDB 写入 species 不变，reliable = true
    - 置信度门槛 — confidence < 0.5：验证 DynamoDB 写入 "uncertain"，reliable = false
    - DynamoDB 字段完整性：验证 UpdateExpression 包含 inference_reliable 和 inference_vote_count
    - DynamoDB 类型正确性：验证 :reliable 为 bool，:vote_count 为 Decimal
    - 向后兼容 — 推理失败：验证 inference_error 写入逻辑不变
    - 向后兼容 — 现有字段保留：验证所有 Spec 17/30 定义的字段仍存在
    - has_bird 过滤 — 部分图片无鸟：验证仅 has_bird=True 的图片参与投票
    - no_bird_detected — 所有图片无鸟：验证 DynamoDB 写入 no_bird_detected、reliable=false、vote_count=0
    - _Requirements: 1.1, 1.2, 1.3, 1.5, 2.1, 2.2, 2.3, 3.1, 3.2, 3.3, 3.4, 4.1, 4.2, 4.3, 5.5, 7.1, 7.2_

- [ ] 7. Final checkpoint — 验证全部测试通过
  - 运行 `source .venv-raspi-eye/bin/activate && pytest model/tests/test_lambda.py model/tests/test_endpoint.py -v`
  - 确保所有测试通过，ask the user if questions arise.
  - 不在此步骤执行 git commit（由编排层统一处理）

## Notes

- 所有 Python 命令必须先 `source .venv-raspi-eye/bin/activate`
- 不使用 `cat <<` heredoc 写入文件
- 不在子代理最终检查点执行 git commit
- 测试使用 mock，不依赖真实 YOLO 权重或 AWS 服务
- DynamoDB 所有数值必须转为 `Decimal(str(round(value, 6)))`，包括 `inference_vote_count`
- Property tests 使用 Hypothesis，每个 >= 100 iterations
- Tasks marked with `*` are optional and can be skipped for faster MVP
- Property tests validate universal correctness properties, unit tests validate specific examples and edge cases
- 验证命令：`source .venv-raspi-eye/bin/activate && pytest model/tests/test_lambda.py model/tests/test_endpoint.py -v`
