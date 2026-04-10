# 实施计划：Spec 6 — AWS IoT 设备注册与证书配置

## 概述

实现 Bash 脚本 `scripts/provision-device.sh`，通过 AWS CLI 完成 IoT 设备注册全流程。脚本支持三种模式（provision / verify / cleanup），所有创建操作幂等。同时创建 TOML 配置模板文件 `device/config/config.toml.example`。实现语言为 Bash，不涉及 C++ 代码。

## 禁止项（Tasks 层）

- SHALL NOT 将 .pem、.key 证书文件提交到 git（已在 .gitignore 排除，提交前用 `git status` 确认）
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件（使用 `printf` 或变量赋值 + 重定向）
- SHALL NOT 在脚本中硬编码 AWS Account ID 或 Region（通过 `aws sts get-caller-identity` 和 `aws configure get region` 动态获取）
- SHALL NOT 在日志中打印证书内容、私钥内容（仅输出 ARN、ID 等标识符）
- SHALL NOT 在 Spec 文档未全部确定前单独 commit
- SHALL NOT 在子代理的最终检查点任务中执行 git commit，由编排层统一执行
- SHALL NOT 将新建文件直接放到项目根目录

## 任务

- [x] 1. 创建 TOML 配置模板文件
  - [x] 1.1 创建 `device/config/config.toml.example`
    - 包含 `[aws]` section 的所有字段，值使用占位符说明
    - 字段：`thing_name`、`credential_endpoint`、`role_alias`、`cert_path`、`key_path`、`ca_path`
    - 添加注释说明此文件由 `scripts/provision-device.sh` 自动生成实际配置
    - 此文件提交到 git，作为配置参考
    - _需求：6.1, 6.2, 6.3_

- [x] 2. 实现 provision-device.sh — 基础框架与工具函数
  - [x] 2.1 创建 `scripts/provision-device.sh` — shebang、全局变量、工具函数
    - `#!/usr/bin/env bash` + `set -euo pipefail`
    - 全局变量声明：`THING_NAME`、`OUTPUT_DIR`（默认 `device/certs`）、`POLICY_NAME`、`ROLE_NAME`、`ROLE_ALIAS`、`MODE`（默认 `provision`）
    - 运行时变量：`AWS_ACCOUNT_ID`、`AWS_REGION`、`CERT_ARN`、`CERT_ID`
    - 工具函数：`log_info()`、`log_warn()`、`log_error()`、`log_success()`（格式：`[INFO]`/`[WARN]`/`[ERROR]`/`[OK]` + message）
    - `check_dependencies()`：检查 `aws`、`jq`、`curl` 是否可用，检查 `aws sts get-caller-identity` 验证凭证
    - `get_aws_context()`：动态获取 `AWS_ACCOUNT_ID` 和 `AWS_REGION`
    - `parse_args()`：解析 `--thing-name`（必选）、`--output-dir`、`--policy-name`、`--role-name`、`--role-alias`、`--verify`、`--cleanup`、`--help`
    - 默认值推导：`POLICY_NAME="${THING_NAME}-policy"`、`ROLE_NAME="${THING_NAME}-role"`、`ROLE_ALIAS="${THING_NAME}-role-alias"`
    - `print_usage()` 帮助信息
    - 设置可执行权限 `chmod +x`
    - _需求：1.1, 5.1_
  - [x] 2.2 实现 `main()` 入口逻辑
    - 调用 `parse_args`、`check_dependencies`、`get_aws_context`
    - 根据 `MODE` 分发到 `do_provision`、`verify_resources`、`cleanup_resources`
    - _需求：5.2, 5.3_

- [x] 3. 实现 provision 模式 — IoT Thing 与证书
  - [x] 3.1 实现 `create_thing()`
    - 幂等检查：`aws iot describe-thing --thing-name "$THING_NAME"` 返回 0 则跳过
    - 创建：`aws iot create-thing --thing-name "$THING_NAME"`
    - 输出 Thing ARN
    - _需求：1.1, 1.2, 1.3_
  - [x] 3.2 实现 `create_certificate()`
    - 检查本地证书文件是否已存在，已存在则跳过创建
    - 创建目录 `mkdir -p "$OUTPUT_DIR"`
    - `aws iot create-keys-and-certificate --set-as-active` 获取响应
    - 用 `jq` 提取 `certificatePem` → `device-cert.pem`、`keyPair.PrivateKey` → `device-private.key`
    - 用 `printf` 写入文件（不使用 heredoc）
    - 设置文件权限 `chmod 600`
    - 记录 `CERT_ARN` 和 `CERT_ID`
    - _需求：2.1, 2.2, 2.5_
  - [x] 3.3 实现 `download_root_ca()`
    - 幂等：文件已存在则跳过
    - `curl -o "${OUTPUT_DIR}/root-ca.pem" https://www.amazontrust.com/repository/AmazonRootCA1.pem`
    - _需求：2.3_
  - [x] 3.4 实现 `attach_cert_to_thing()`
    - 幂等：`aws iot list-thing-principals` 检查是否已附加
    - `aws iot attach-thing-principal --thing-name "$THING_NAME" --principal "$CERT_ARN"`
    - _需求：2.4_
  - [x] 3.5 实现证书 ARN 恢复逻辑
    - 当本地证书文件存在但 `CERT_ARN` 未知时，通过 `aws iot list-thing-principals` 恢复
    - 本地文件存在但 AWS 端无证书时，报错提示先 `--cleanup` 再重新 provision
    - _需求：1.2（幂等）_

- [x] 4. 实现 provision 模式 — Policy、Role、Role Alias
  - [x] 4.1 实现 `create_iot_policy()`
    - 幂等：`aws iot get-policy` 检查是否存在
    - Policy JSON 动态替换 `REGION`、`ACCOUNT_ID`、`ROLE_ALIAS`（不硬编码）
    - 授予 `iot:Connect` 和 `iot:AssumeRoleWithCertificate` 权限
    - 使用变量赋值构建 JSON（不使用 heredoc）
    - _需求：3.1, 3.3_
  - [x] 4.2 实现 `attach_policy_to_cert()`
    - 幂等：`aws iot list-attached-policies` 检查是否已附加
    - `aws iot attach-policy --policy-name "$POLICY_NAME" --target "$CERT_ARN"`
    - _需求：3.2_
  - [x] 4.3 实现 `create_iam_role()`
    - 幂等：`aws iam get-role` 检查是否存在
    - 信任策略允许 `credentials.iot.amazonaws.com` 代入
    - 不附加权限策略（空权限），后续 Spec 按需追加
    - _需求：4.1, 4.2, 4.4_
  - [x] 4.4 实现 `create_role_alias()`
    - 幂等：`aws iot describe-role-alias` 检查是否存在
    - `aws iot create-role-alias --role-alias "$ROLE_ALIAS" --role-arn "$ROLE_ARN"`
    - _需求：4.3, 4.4_
  - [x] 4.5 实现 `get_credential_endpoint()`
    - `aws iot describe-endpoint --endpoint-type iot:CredentialProvider`
    - _需求：4.5_

- [x] 5. 实现 provision 模式 — TOML 配置生成与摘要输出
  - [x] 5.1 实现 `generate_toml_config()`
    - 生成 `device/config/config.toml`，包含 `[aws]` section
    - 字段：`thing_name`、`credential_endpoint`、`role_alias`、`cert_path`、`key_path`、`ca_path`
    - 证书路径使用相对于项目根目录的路径
    - 如果文件已存在，用 `awk` 删除现有 `[aws]` section，保留其他 section，追加新 `[aws]`
    - 如果文件不存在，直接写入
    - 使用 `printf` 写入（不使用 heredoc）
    - _需求：6.1, 6.2, 6.3, 6.4, 6.5_
  - [x] 5.2 实现 `print_summary()`
    - 输出所有资源信息：Thing 名称、证书 ARN、Policy 名称、Role ARN、Role Alias、Credential Endpoint、证书文件路径
    - 不输出证书内容或私钥内容
    - _需求：5.1_
  - [x] 5.3 实现 `do_provision()` 编排函数
    - 按顺序调用：`create_thing` → `create_certificate` → `download_root_ca` → `attach_cert_to_thing` → `create_iot_policy` → `attach_policy_to_cert` → `create_iam_role` → `create_role_alias` → `get_credential_endpoint` → `generate_toml_config` → `print_summary`
    - _需求：1.1, 2.1, 3.1, 4.1, 5.1, 6.1_

- [x] 6. 检查点 — provision 模式代码审查
  - 确认脚本语法正确（`bash -n scripts/provision-device.sh`）
  - 确认无 heredoc 使用
  - 确认无硬编码 Account ID / Region
  - 确认日志中无证书/私钥内容输出
  - 如有问题，询问用户

- [x] 7. 实现 verify 模式
  - [x] 7.1 实现 `verify_resources()`
    - 逐项检查：Thing（`aws iot describe-thing`）、证书文件（本地文件存在性）、Policy（`aws iot get-policy`）、Role（`aws iam get-role`）、Role Alias（`aws iot describe-role-alias`）、TOML 配置文件（文件存在性）
    - 每项输出 `[OK]` 或 `[FAIL]`
    - 最终输出总结：全部通过或列出失败项
    - _需求：5.2_

- [x] 8. 实现 cleanup 模式
  - [x] 8.1 实现 `cleanup_resources()`
    - 按依赖顺序删除（先 detach 再 delete）：
      1. detach 证书与 Thing（`aws iot detach-thing-principal`）
      2. detach Policy 与证书（`aws iot detach-policy`）
      3. 停用证书（`aws iot update-certificate --new-status INACTIVE`）+ 删除证书（`aws iot delete-certificate`）
      4. 删除 Policy（`aws iot delete-policy`）
      5. 删除 Role Alias（`aws iot delete-role-alias`）
      6. 删除 IAM Role（`aws iam delete-role`）
      7. 删除 Thing（`aws iot delete-thing`）
      8. 删除本地证书文件和 TOML 配置
    - 每步独立执行，失败不中断（记录警告继续）
    - 最终输出清理结果（成功 / 有 N 个警告）
    - _需求：5.3_

- [x] 9. 最终检查点 — 全量验证
  - 执行 `bash -n scripts/provision-device.sh` 确认语法正确
  - 确认 `device/config/config.toml.example` 存在且内容正确
  - 确认 `.gitignore` 已排除 `device/certs/`、`device/config/config.toml`
  - 确认现有 C++ 测试不受影响：`ctest --test-dir device/build --output-on-failure`
  - 确认脚本中无 heredoc、无硬编码 Account ID/Region、日志无敏感信息输出
  - SHALL NOT 在此检查点执行 git commit
  - 如有问题，询问用户
  - _需求：1.1, 2.1, 3.1, 4.1, 5.1, 5.2, 5.3, 6.1_

## 备注

- 新建文件：`scripts/provision-device.sh`、`device/config/config.toml.example`
- 不修改文件：`device/src/` 下所有文件、`device/tests/` 下所有文件、`device/CMakeLists.txt`
- `.gitignore` 已包含 `device/certs/`、`*.pem`、`*.key`、`device/config/config.toml` 排除规则，无需修改
- 本 Spec 不包含自动化测试（PBT 不适用，设计文档已说明原因），验证通过手动执行脚本完成
- 脚本日志语言统一使用英文
- git commit 由编排层在所有 post-task hook 完成后统一执行，子代理不自行 commit
