#!/bin/bash
# 一键部署云端推理链路：SageMaker Endpoint + Lambda + S3 事件通知
# DynamoDB 使用已有的 raspi-eye-events 表（Lambda 通过 update_item 写回推理结果）
#
# 部署顺序：
# 1. SageMaker Endpoint（调用 deploy_endpoint.py）
# 2. Lambda IAM 角色（aws iam create-role + put-role-policy）
# 3. Lambda 函数（zip handler.py → aws lambda create-function）
# 4. S3 事件通知（aws lambda add-permission + s3api put-bucket-notification-configuration）
#
# 注意：DynamoDB 使用已有的 raspi-eye-events 表（由设备端创建），
# Lambda 通过 update_item 写回推理结果，不创建新表。
#
# 支持参数：
# --skip-endpoint  跳过 SageMaker endpoint 部署
# --delete         逆序删除所有资源
# --e2e-test       端到端验证
# --role           SageMaker 执行角色 ARN（部署时必需）
# --s3-bucket      S3 bucket 名称（默认 raspi-eye-model-data）

set -euo pipefail

# ─── 常量 ───────────────────────────────────────────────────────────────────────
REGION="ap-southeast-1"
ACCOUNT_ID="014498626607"
S3_CAPTURES_BUCKET="raspi-eye-captures-${ACCOUNT_ID}-${REGION}-an"
DYNAMODB_TABLE="raspi-eye-events"
LAMBDA_FUNCTION="raspi-eye-inference"
LAMBDA_ROLE_NAME="raspi-eye-inference-lambda-role"
LAMBDA_RUNTIME="python3.12"
LAMBDA_MEMORY=256
LAMBDA_TIMEOUT=120
ENDPOINT_NAME="raspi-eye-bird-classifier"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ─── 默认参数 ───────────────────────────────────────────────────────────────────
SKIP_ENDPOINT=false
DELETE_MODE=false
E2E_TEST=false
ROLE_ARN=""
S3_BUCKET="raspi-eye-model-data"

# ─── 颜色输出 ───────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }
step()  { echo -e "${BLUE}[STEP]${NC} $*"; }

# ─── 参数解析 ───────────────────────────────────────────────────────────────────
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --skip-endpoint   跳过 SageMaker endpoint 部署"
    echo "  --delete          按逆序删除所有资源"
    echo "  --e2e-test        端到端验证"
    echo "  --role ARN        SageMaker 执行角色 ARN（部署时必需）"
    echo "  --s3-bucket NAME  S3 bucket 名称（默认 raspi-eye-model-data）"
    echo "  -h, --help        显示帮助信息"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-endpoint)
            SKIP_ENDPOINT=true
            shift
            ;;
        --delete)
            DELETE_MODE=true
            shift
            ;;
        --e2e-test)
            E2E_TEST=true
            shift
            ;;
        --role)
            ROLE_ARN="$2"
            shift 2
            ;;
        --s3-bucket)
            S3_BUCKET="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            error "未知参数: $1"
            usage
            ;;
    esac
done

# ─── ARN 构造 ──────────────────────────────────────────────────────────────────
LAMBDA_ROLE_ARN="arn:aws:iam::${ACCOUNT_ID}:role/${LAMBDA_ROLE_NAME}"
LAMBDA_ARN="arn:aws:lambda:${REGION}:${ACCOUNT_ID}:function:${LAMBDA_FUNCTION}"
DYNAMODB_TABLE_ARN="arn:aws:dynamodb:${REGION}:${ACCOUNT_ID}:table/${DYNAMODB_TABLE}"
ENDPOINT_ARN="arn:aws:sagemaker:${REGION}:${ACCOUNT_ID}:endpoint/${ENDPOINT_NAME}"
S3_CAPTURES_ARN="arn:aws:s3:::${S3_CAPTURES_BUCKET}"

# ═══════════════════════════════════════════════════════════════════════════════
# 部署函数
# ═══════════════════════════════════════════════════════════════════════════════

# ─── Step 1: SageMaker Endpoint ─────────────────────────────────────────────────
deploy_endpoint() {
    step "1/4 部署 SageMaker Endpoint..."

    if [[ "${SKIP_ENDPOINT}" == "true" ]]; then
        warn "跳过 SageMaker endpoint 部署（--skip-endpoint）"
        return 0
    fi

    if [[ -z "${ROLE_ARN}" ]]; then
        error "--role 参数是必需的（SageMaker 执行角色 ARN）"
        exit 1
    fi

    source "${PROJECT_ROOT}/.venv-raspi-eye/bin/activate"
    python "${PROJECT_ROOT}/model/deploy_endpoint.py" \
        --role "${ROLE_ARN}" \
        --s3-bucket "${S3_BUCKET}" \
        --region "${REGION}" \
        --endpoint-name "${ENDPOINT_NAME}" \
        --wait

    info "SageMaker Endpoint 部署完成: ${ENDPOINT_NAME}"
}

# ─── Step 2: Lambda IAM 角色 ────────────────────────────────────────────────────
deploy_lambda_role() {
    step "2/4 创建 Lambda IAM 角色..."

    # 检查角色是否已存在
    if aws iam get-role \
        --role-name "${LAMBDA_ROLE_NAME}" \
        --output text > /dev/null 2>&1; then
        warn "IAM 角色已存在: ${LAMBDA_ROLE_NAME}，跳过创建"
        # 更新内联策略（确保权限最新）
        _put_lambda_policy
        return 0
    fi

    # 创建角色（信任策略允许 Lambda 服务承担）
    aws iam create-role \
        --role-name "${LAMBDA_ROLE_NAME}" \
        --assume-role-policy-document '{
            "Version": "2012-10-17",
            "Statement": [{
                "Effect": "Allow",
                "Principal": {"Service": "lambda.amazonaws.com"},
                "Action": "sts:AssumeRole"
            }]
        }' \
        --region "${REGION}" \
        --output text > /dev/null

    # 附加内联策略
    _put_lambda_policy

    # 等待角色传播（IAM 最终一致性）
    info "等待 IAM 角色传播（10 秒）..."
    sleep 10

    info "Lambda IAM 角色创建完成: ${LAMBDA_ROLE_NAME}"
}

_put_lambda_policy() {
    aws iam put-role-policy \
        --role-name "${LAMBDA_ROLE_NAME}" \
        --policy-name "raspi-eye-inference-lambda-policy" \
        --policy-document '{
            "Version": "2012-10-17",
            "Statement": [
                {
                    "Effect": "Allow",
                    "Action": "s3:GetObject",
                    "Resource": "arn:aws:s3:::'"${S3_CAPTURES_BUCKET}"'/*"
                },
                {
                    "Effect": "Allow",
                    "Action": "sagemaker:InvokeEndpoint",
                    "Resource": "arn:aws:sagemaker:'"${REGION}"':'"${ACCOUNT_ID}"':endpoint/'"${ENDPOINT_NAME}"'"
                },
                {
                    "Effect": "Allow",
                    "Action": "dynamodb:UpdateItem",
                    "Resource": "arn:aws:dynamodb:'"${REGION}"':'"${ACCOUNT_ID}"':table/'"${DYNAMODB_TABLE}"'"
                },
                {
                    "Effect": "Allow",
                    "Action": ["logs:CreateLogGroup", "logs:CreateLogStream", "logs:PutLogEvents"],
                    "Resource": "arn:aws:logs:'"${REGION}"':'"${ACCOUNT_ID}"':*"
                }
            ]
        }' \
        --output text > /dev/null

    info "Lambda 内联策略已更新"
}

# ─── Step 3: Lambda 函数 ────────────────────────────────────────────────────────
deploy_lambda() {
    step "3/4 创建 Lambda 函数..."

    # 检查函数是否已存在
    if aws lambda get-function \
        --function-name "${LAMBDA_FUNCTION}" \
        --region "${REGION}" \
        --output text > /dev/null 2>&1; then
        warn "Lambda 函数已存在: ${LAMBDA_FUNCTION}，跳过创建"
        return 0
    fi

    # 打包 handler.py 为 zip
    local tmp_zip="/tmp/lambda-deploy-$$.zip"
    rm -f "${tmp_zip}"
    (cd "${PROJECT_ROOT}/model/lambda" && zip -j "${tmp_zip}" handler.py > /dev/null)

    aws lambda create-function \
        --function-name "${LAMBDA_FUNCTION}" \
        --runtime "${LAMBDA_RUNTIME}" \
        --role "${LAMBDA_ROLE_ARN}" \
        --handler "handler.handler" \
        --zip-file "fileb://${tmp_zip}" \
        --memory-size "${LAMBDA_MEMORY}" \
        --timeout "${LAMBDA_TIMEOUT}" \
        --environment "Variables={ENDPOINT_NAME=${ENDPOINT_NAME},TABLE_NAME=${DYNAMODB_TABLE}}" \
        --region "${REGION}" \
        --output text > /dev/null

    rm -f "${tmp_zip}"

    # 等待函数变为 Active
    aws lambda wait function-active-v2 \
        --function-name "${LAMBDA_FUNCTION}" \
        --region "${REGION}"

    info "Lambda 函数创建完成: ${LAMBDA_FUNCTION}"
}

# ─── Step 4: S3 事件通知 ────────────────────────────────────────────────────────
deploy_s3_notification() {
    step "4/4 配置 S3 事件通知..."

    # 添加 Lambda resource-based policy（允许 S3 调用）
    # 先尝试移除旧的 permission（忽略错误）
    aws lambda remove-permission \
        --function-name "${LAMBDA_FUNCTION}" \
        --statement-id "s3-invoke-${LAMBDA_FUNCTION}" \
        --region "${REGION}" \
        --output text > /dev/null 2>&1 || true

    aws lambda add-permission \
        --function-name "${LAMBDA_FUNCTION}" \
        --statement-id "s3-invoke-${LAMBDA_FUNCTION}" \
        --action "lambda:InvokeFunction" \
        --principal "s3.amazonaws.com" \
        --source-arn "${S3_CAPTURES_ARN}" \
        --source-account "${ACCOUNT_ID}" \
        --region "${REGION}" \
        --output text > /dev/null

    # 配置 S3 事件通知
    aws s3api put-bucket-notification-configuration \
        --bucket "${S3_CAPTURES_BUCKET}" \
        --notification-configuration '{
            "LambdaFunctionConfigurations": [{
                "LambdaFunctionArn": "'"${LAMBDA_ARN}"'",
                "Events": ["s3:ObjectCreated:*"],
                "Filter": {
                    "Key": {
                        "FilterRules": [
                            {"Name": "suffix", "Value": "event.json"}
                        ]
                    }
                }
            }]
        }' \
        --region "${REGION}"

    info "S3 事件通知配置完成: ${S3_CAPTURES_BUCKET} → ${LAMBDA_FUNCTION}"
}

# ═══════════════════════════════════════════════════════════════════════════════
# 删除函数（逆序）
# ═══════════════════════════════════════════════════════════════════════════════

delete_s3_notification() {
    step "1/4 移除 S3 事件通知..."

    # 清空通知配置
    aws s3api put-bucket-notification-configuration \
        --bucket "${S3_CAPTURES_BUCKET}" \
        --notification-configuration '{}' \
        --region "${REGION}" 2>/dev/null || true

    # 移除 Lambda permission
    aws lambda remove-permission \
        --function-name "${LAMBDA_FUNCTION}" \
        --statement-id "s3-invoke-${LAMBDA_FUNCTION}" \
        --region "${REGION}" \
        --output text > /dev/null 2>&1 || true

    info "S3 事件通知已移除"
}

delete_lambda() {
    step "2/4 删除 Lambda 函数..."

    aws lambda delete-function \
        --function-name "${LAMBDA_FUNCTION}" \
        --region "${REGION}" \
        --output text > /dev/null 2>&1 || true

    info "Lambda 函数已删除: ${LAMBDA_FUNCTION}"
}

delete_lambda_role() {
    step "3/4 删除 Lambda IAM 角色..."

    # 先删除内联策略
    aws iam delete-role-policy \
        --role-name "${LAMBDA_ROLE_NAME}" \
        --policy-name "raspi-eye-inference-lambda-policy" \
        --output text > /dev/null 2>&1 || true

    # 再删除角色
    aws iam delete-role \
        --role-name "${LAMBDA_ROLE_NAME}" \
        --output text > /dev/null 2>&1 || true

    info "Lambda IAM 角色已删除: ${LAMBDA_ROLE_NAME}"
}

delete_endpoint() {
    step "4/4 删除 SageMaker Endpoint..."

    source "${PROJECT_ROOT}/.venv-raspi-eye/bin/activate"
    python "${PROJECT_ROOT}/model/deploy_endpoint.py" \
        --endpoint-name "${ENDPOINT_NAME}" \
        --region "${REGION}" \
        --delete 2>/dev/null || true

    info "SageMaker Endpoint 已删除: ${ENDPOINT_NAME}"
}

# ═══════════════════════════════════════════════════════════════════════════════
# 端到端验证
# ═══════════════════════════════════════════════════════════════════════════════

run_e2e_test() {
    step "端到端验证开始..."

    local test_timestamp
    test_timestamp=$(date +%Y%m%d_%H%M%S)
    local test_event_id="e2e_test_${test_timestamp}"
    local test_device_id="e2e-test-device"
    local test_prefix="${test_device_id}/$(date +%Y-%m-%d)/${test_event_id}"
    local test_jpg_name="test_${test_timestamp}.jpg"

    # 生成一个最小的合法 JPEG 测试图片（1x1 像素红色）
    local tmp_jpg
    tmp_jpg=$(mktemp /tmp/test-XXXXXX.jpg)
    # 最小合法 JPEG: 1x1 红色像素
    printf '\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00' > "${tmp_jpg}"
    printf '\xff\xdb\x00C\x00\x08\x06\x06\x07\x06\x05\x08\x07\x07\x07\t\t' >> "${tmp_jpg}"
    printf '\x08\n\x0c\x14\r\x0c\x0b\x0b\x0c\x19\x12\x13\x0f\x14\x1d\x1a' >> "${tmp_jpg}"
    printf '\x1f\x1e\x1d\x1a\x1c\x1c $.\x27 ",#\x1c\x1c(7),01444\x1f\x27444' >> "${tmp_jpg}"

    # 使用 Python 生成合法测试 JPEG（更可靠）
    source "${PROJECT_ROOT}/.venv-raspi-eye/bin/activate"
    python -c "
from PIL import Image
import io, sys
img = Image.new('RGB', (224, 224), color=(255, 0, 0))
img.save(sys.argv[1], 'JPEG')
" "${tmp_jpg}"

    # 构造 event.json
    local tmp_event
    tmp_event=$(mktemp /tmp/event-XXXXXX.json)
    local test_start_time_val
    test_start_time_val=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    python -c "
import json, sys
event = {
    'event_id': sys.argv[1],
    'device_id': sys.argv[2],
    'start_time': sys.argv[4],
    'end_time': sys.argv[4],
    'frame_count': 1,
    'detections_summary': {'bird': {'count': 1, 'max_confidence': 0.95}},
    'snapshots': [sys.argv[3]]
}
with open(sys.argv[5], 'w') as f:
    json.dump(event, f)
" "${test_event_id}" "${test_device_id}" "${test_jpg_name}" "${test_start_time_val}" "${tmp_event}"

    info "上传测试图片: s3://${S3_CAPTURES_BUCKET}/${test_prefix}/${test_jpg_name}"
    aws s3 cp "${tmp_jpg}" "s3://${S3_CAPTURES_BUCKET}/${test_prefix}/${test_jpg_name}" \
        --region "${REGION}" --quiet

    info "上传测试 event.json: s3://${S3_CAPTURES_BUCKET}/${test_prefix}/event.json"
    aws s3 cp "${tmp_event}" "s3://${S3_CAPTURES_BUCKET}/${test_prefix}/event.json" \
        --region "${REGION}" --quiet

    rm -f "${tmp_jpg}" "${tmp_event}"

    # 轮询 DynamoDB（最多 180 秒）
    # raspi-eye-events 表 PK: device_id (HASH) + start_time (RANGE)
    info "等待 DynamoDB 记录（最多 180 秒，含 SageMaker 冷启动）..."
    local elapsed=0
    local poll_interval=10
    local max_wait=180
    local found=false

    while [[ ${elapsed} -lt ${max_wait} ]]; do
        local result
        result=$(aws dynamodb get-item \
            --table-name "${DYNAMODB_TABLE}" \
            --key "{\"device_id\": {\"S\": \"${test_device_id}\"}, \"start_time\": {\"S\": \"${test_start_time_val}\"}}" \
            --region "${REGION}" \
            --output json 2>/dev/null || echo "{}")

        if echo "${result}" | python -c "
import sys, json
d = json.load(sys.stdin)
item = d.get('Item', {})
# 检查推理结果字段是否已写入
sys.exit(0 if 'inference_species' in item or 'inference_error' in item else 1)
" 2>/dev/null; then
            found=true
            break
        fi

        echo -n "."
        sleep "${poll_interval}"
        elapsed=$((elapsed + poll_interval))
    done
    echo ""

    if [[ "${found}" == "true" ]]; then
        info "端到端验证通过！DynamoDB 推理结果已写入。"

        # 打印推理结果（字段带 inference_ 前缀）
        aws dynamodb get-item \
            --table-name "${DYNAMODB_TABLE}" \
            --key "{\"device_id\": {\"S\": \"${test_device_id}\"}, \"start_time\": {\"S\": \"${test_start_time_val}\"}}" \
            --region "${REGION}" \
            --output json | python -c "
import sys, json
data = json.load(sys.stdin)
item = data.get('Item', {})
print('  device_id:', item.get('device_id', {}).get('S', 'N/A'))
print('  start_time:', item.get('start_time', {}).get('S', 'N/A'))
print('  inference_species:', item.get('inference_species', {}).get('S', 'N/A'))
print('  inference_confidence:', item.get('inference_confidence', {}).get('N', 'N/A'))
print('  inference_latency_ms:', item.get('inference_latency_ms', {}).get('N', 'N/A'))
print('  inference_error:', item.get('inference_error', {}).get('S', 'None'))
"
    else
        error "端到端验证失败：180 秒内未检测到 DynamoDB 记录"
        echo ""
        echo "Lambda CloudWatch Logs:"
        echo "  https://${REGION}.console.aws.amazon.com/cloudwatch/home?region=${REGION}#logsV2:log-groups/log-group/\$252Faws\$252Flambda\$252F${LAMBDA_FUNCTION}"
        echo ""
    fi

    # 清理测试数据
    info "清理测试数据..."
    aws s3 rm "s3://${S3_CAPTURES_BUCKET}/${test_prefix}/" \
        --recursive --region "${REGION}" --quiet 2>/dev/null || true
    # raspi-eye-events 表 PK: device_id + start_time
    # 注意：e2e 测试写入的是 Lambda 通过 update_item 追加的字段，
    # 不删除整条记录（因为记录可能不是 e2e 测试创建的）
    # 如果需要清理，使用 delete-item
    aws dynamodb delete-item \
        --table-name "${DYNAMODB_TABLE}" \
        --key "{\"device_id\": {\"S\": \"${test_device_id}\"}, \"start_time\": {\"S\": \"${test_start_time_val}\"}}" \
        --region "${REGION}" \
        --output text > /dev/null 2>&1 || true

    info "测试数据已清理"

    if [[ "${found}" != "true" ]]; then
        exit 1
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# 资源 ARN 汇总
# ═══════════════════════════════════════════════════════════════════════════════

print_summary() {
    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo "  部署完成 — 资源 ARN 汇总"
    echo "═══════════════════════════════════════════════════════════════"
    echo "  SageMaker Endpoint : ${ENDPOINT_ARN}"
    echo "  DynamoDB Table     : ${DYNAMODB_TABLE_ARN}"
    echo "  Lambda Function    : ${LAMBDA_ARN}"
    echo "  Lambda Role        : ${LAMBDA_ROLE_ARN}"
    echo "  S3 Captures Bucket : ${S3_CAPTURES_ARN}"
    echo "  Region             : ${REGION}"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
}

# ═══════════════════════════════════════════════════════════════════════════════
# 主流程
# ═══════════════════════════════════════════════════════════════════════════════

main() {
    echo ""
    echo "╔═══════════════════════════════════════════════════════════════╗"
    echo "║  raspi-eye 云端推理链路部署脚本                              ║"
    echo "╚═══════════════════════════════════════════════════════════════╝"
    echo ""

    # 删除模式
    if [[ "${DELETE_MODE}" == "true" ]]; then
        warn "删除模式：按逆序删除所有资源"
        delete_s3_notification
        delete_lambda
        delete_lambda_role
        delete_endpoint
        info "所有资源已删除"
        return 0
    fi

    # 端到端验证模式
    if [[ "${E2E_TEST}" == "true" ]]; then
        run_e2e_test
        return 0
    fi

    # 部署模式：检查必需参数
    if [[ "${SKIP_ENDPOINT}" != "true" ]] && [[ -z "${ROLE_ARN}" ]]; then
        error "--role 参数是必需的（SageMaker 执行角色 ARN）"
        echo "  示例: $0 --role arn:aws:iam::014498626607:role/raspi-eye-sagemaker-processing-role"
        echo "  或使用 --skip-endpoint 跳过 endpoint 部署"
        exit 1
    fi

    # 按序部署
    deploy_endpoint
    deploy_lambda_role
    deploy_lambda
    deploy_s3_notification

    print_summary
}

main
