#!/bin/bash
# 创建 SageMaker Processing Job 所需的 IAM Role
# 用法: ./scripts/create-sagemaker-role.sh <s3-bucket-name>
#
# 创建 Role: raspi-eye-sagemaker-processing-role
# 附加最小权限策略: S3 读写（仅限指定桶）、CloudWatch Logs、ECR 拉取镜像

set -euo pipefail

BUCKET_NAME="${1:?用法: $0 <s3-bucket-name>}"
ROLE_NAME="raspi-eye-sagemaker-processing-role"
POLICY_NAME="raspi-eye-sagemaker-processing-policy"

echo "创建 SageMaker Processing Role: ${ROLE_NAME}"
echo "S3 桶: ${BUCKET_NAME}"

# 1. 创建信任策略（sagemaker.amazonaws.com AssumeRole）
TRUST_POLICY='{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Principal": {
        "Service": "sagemaker.amazonaws.com"
      },
      "Action": "sts:AssumeRole"
    }
  ]
}'

# 2. 创建 Role
ROLE_ARN=$(aws iam create-role \
  --role-name "${ROLE_NAME}" \
  --assume-role-policy-document "${TRUST_POLICY}" \
  --description "SageMaker Processing Job role for raspi-eye bird feature cleaning" \
  --query "Role.Arn" \
  --output text)

echo "Role 已创建: ${ROLE_ARN}"

# 3. 创建并附加最小权限策略
POLICY_DOC=$(cat <<-POLICY
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "S3ReadWrite",
      "Effect": "Allow",
      "Action": [
        "s3:GetObject",
        "s3:PutObject",
        "s3:ListBucket"
      ],
      "Resource": [
        "arn:aws:s3:::${BUCKET_NAME}",
        "arn:aws:s3:::${BUCKET_NAME}/*"
      ]
    },
    {
      "Sid": "CloudWatchLogs",
      "Effect": "Allow",
      "Action": [
        "logs:CreateLogGroup",
        "logs:CreateLogStream",
        "logs:PutLogEvents"
      ],
      "Resource": [
        "arn:aws:logs:*:*:log-group:/aws/sagemaker/ProcessingJobs*",
        "arn:aws:logs:*:*:log-group:/aws/sagemaker/TrainingJobs*"
      ]
    },
    {
      "Sid": "ECRPullImage",
      "Effect": "Allow",
      "Action": [
        "ecr:GetAuthorizationToken",
        "ecr:BatchGetImage",
        "ecr:GetDownloadUrlForLayer"
      ],
      "Resource": "*"
    },
    {
      "Sid": "SecretsManagerRead",
      "Effect": "Allow",
      "Action": [
        "secretsmanager:GetSecretValue"
      ],
      "Resource": "arn:aws:secretsmanager:*:*:secret:raspi-eye/*"
    }
  ]
}
POLICY
)

POLICY_ARN=$(aws iam create-policy \
  --policy-name "${POLICY_NAME}" \
  --policy-document "${POLICY_DOC}" \
  --description "Minimal permissions for raspi-eye SageMaker Processing Job" \
  --query "Policy.Arn" \
  --output text)

aws iam attach-role-policy \
  --role-name "${ROLE_NAME}" \
  --policy-arn "${POLICY_ARN}"

echo "策略已附加: ${POLICY_ARN}"
echo ""
echo "=========================================="
echo "Role ARN: ${ROLE_ARN}"
echo "=========================================="
echo ""
echo "使用方式:"
echo "  python model/launch_processing.py --s3-bucket ${BUCKET_NAME} --role ${ROLE_ARN}"
echo "  或设置环境变量:"
echo "  export SAGEMAKER_ROLE_ARN=${ROLE_ARN}"
