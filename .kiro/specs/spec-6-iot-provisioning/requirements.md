# 需求文档：Spec 6 — AWS IoT 设备注册与证书配置

## 简介

本 Spec 通过 Bash 脚本 + AWS CLI 完成 AWS IoT 设备注册的全部云端配置：创建 IoT Thing、生成 X.509 证书、配置 IoT Policy、创建 IAM Role 和 Role Alias。脚本执行完毕后，设备端所需的证书文件和 endpoint 信息全部就绪，供 Spec 7（credential-provider）的 C++ 模块使用。

本 Spec 不涉及 C++ 代码，不修改 device/ 目录，纯运维脚本。

## 前置条件

- AWS CLI v2 已安装并配置（`aws configure`）
- AWS 账户具有 IoT Core、IAM 管理权限
- Spec 0-5, 9, 9.5 已完成 ✅

## 术语表

- **IoT_Thing**：AWS IoT Core 中注册的设备实体
- **X509_Certificate**：AWS IoT Core 签发的设备证书，用于 mTLS 认证
- **IoT_Policy**：附加到证书上的 JSON 策略，定义设备权限
- **Role_Alias**：IoT Credentials Provider 中的角色别名，映射到 IAM Role
- **IAM_Role**：AWS IAM 角色，信任 `iot.credentials.amazonaws.com`，定义设备获取的临时凭证权限
- **Provisioning_Script**：本 Spec 新增的 Bash 脚本，一键完成设备注册

## 约束条件

| 约束 | 值 |
|------|-----|
| 脚本语言 | Bash（`#!/usr/bin/env bash`，`set -euo pipefail`） |
| 依赖工具 | AWS CLI v2、jq |
| 证书输出目录 | 脚本参数指定，默认 `device/certs/` |
| 配置文件路径 | `device/config/raspi-eye.toml`（脚本生成 `[aws]` section） |
| 配置文件格式 | TOML（后续 Spec 按需追加 `[camera]`、`[pipeline]`、`[yolo]` 等 section） |
| 证书文件 | .gitignore 排除，不提交 git |
| 脚本位置 | `scripts/provision-device.sh` |
| 日志语言 | 英文 |

## 禁止项

### Requirements 层

- SHALL NOT 在本 Spec 中编写 C++ 代码或修改 device/src/ 目录
- SHALL NOT 创建 KVS 流、S3 桶、DynamoDB 表等下游资源（推迟到各自 Spec）
- SHALL NOT 在脚本中硬编码 AWS Account ID 或 Region（通过 AWS CLI 配置或参数获取）

### Tasks 层

- SHALL NOT 将 .pem、.key 证书文件提交到 git
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件

## 需求

### 需求 1：IoT Thing 注册

**用户故事：** 作为开发者，我需要一键注册设备到 AWS IoT Core。

#### 验收标准

1. THE Provisioning_Script SHALL 通过 `aws iot create-thing` 创建 IoT_Thing，Thing 名称通过 `--thing-name` 参数指定
2. IF Thing 名称已存在，THEN THE Provisioning_Script SHALL 报告已存在并跳过创建（幂等）
3. THE Provisioning_Script SHALL 输出 Thing ARN

### 需求 2：X.509 证书生成与下载

**用户故事：** 作为开发者，我需要生成设备证书并下载到本地。

#### 验收标准

1. THE Provisioning_Script SHALL 通过 `aws iot create-keys-and-certificate` 创建证书，状态设为 ACTIVE
2. THE Provisioning_Script SHALL 将设备证书（`device-cert.pem`）、私钥（`device-private.key`）保存到输出目录
3. THE Provisioning_Script SHALL 下载 Amazon Root CA 1（`root-ca.pem`）到输出目录
4. THE Provisioning_Script SHALL 通过 `aws iot attach-thing-principal` 将证书附加到 Thing
5. THE Provisioning_Script SHALL 输出证书 ARN 和证书 ID

### 需求 3：IoT Policy 配置

**用户故事：** 作为开发者，我需要配置设备的 IoT 权限策略。

#### 验收标准

1. THE Provisioning_Script SHALL 创建 IoT_Policy，授予 `iot:Connect` 和 `iot:AssumeRoleWithCertificate` 权限
2. THE Provisioning_Script SHALL 将 IoT_Policy 附加到证书
3. IF Policy 已存在，THEN THE Provisioning_Script SHALL 跳过创建，直接附加（幂等）

### 需求 4：IAM Role 与 Role Alias

**用户故事：** 作为开发者，我需要创建 IAM Role 和 Role Alias，以便设备通过证书获取临时凭证。

#### 验收标准

1. THE Provisioning_Script SHALL 创建 IAM_Role，信任策略允许 `iot.credentials.amazonaws.com` 代入
2. THE IAM_Role SHALL 初始包含空权限策略（仅信任策略），后续 Spec 按需追加 KVS、S3 等权限
3. THE Provisioning_Script SHALL 通过 `aws iot create-role-alias` 创建 Role_Alias
4. IF Role 或 Role Alias 已存在，THEN THE Provisioning_Script SHALL 跳过创建（幂等）
5. THE Provisioning_Script SHALL 输出 IoT Credentials Provider endpoint（`aws iot describe-endpoint --endpoint-type iot:CredentialProvider`）

### 需求 5：验证与输出

**用户故事：** 作为开发者，我需要确认所有资源创建成功。

#### 验收标准

1. THE Provisioning_Script SHALL 在执行结束时输出摘要：Thing 名称、证书 ARN、Policy 名称、Role ARN、Role Alias、Credentials Provider endpoint、证书文件路径
2. THE Provisioning_Script SHALL 提供 `--verify` 模式，检查已有资源是否完整（不创建新资源）
3. THE Provisioning_Script SHALL 提供 `--cleanup` 模式，删除本次创建的所有资源（证书、Thing、Policy、Role Alias、Role）

### 需求 6：TOML 配置文件输出

**用户故事：** 作为开发者，我需要 provisioning 脚本自动生成 TOML 配置文件，以便设备端 C++ 模块直接读取 AWS 连接信息。

#### 验收标准

1. THE Provisioning_Script SHALL 在输出目录生成 `raspi-eye.toml` 配置文件，包含 `[aws]` section
2. THE `[aws]` section SHALL 包含以下字段：`thing_name`、`credential_endpoint`、`role_alias`、`cert_path`、`key_path`、`ca_path`
3. THE 配置文件中的证书路径 SHALL 使用相对于项目根目录的路径
4. IF 配置文件已存在，THEN THE Provisioning_Script SHALL 仅更新 `[aws]` section，保留其他 section 不变
5. THE 配置文件 SHALL 被 `.gitignore` 排除（包含设备特定信息）

## 验证命令

```bash
# 执行 provisioning
bash scripts/provision-device.sh --thing-name raspi-eye-001 --output-dir device/certs

# 验证资源
bash scripts/provision-device.sh --thing-name raspi-eye-001 --verify

# 清理资源（开发测试用）
bash scripts/provision-device.sh --thing-name raspi-eye-001 --cleanup
```

## 明确不包含

- 设备端 C++ 凭证获取模块（Spec 7: credential-provider）
- KVS 流、S3 桶等下游资源创建
- OTA 证书轮换
- IaC（CDK/CloudFormation/Terraform）
- 配置文件加载（Spec 18）
