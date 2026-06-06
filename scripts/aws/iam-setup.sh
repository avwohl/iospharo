#!/usr/bin/env bash
# iam-setup.sh — create the IAM role + instance profile the spot box runs under.
# Scoped to exactly what it needs: read/write its S3 prefix, and terminate /
# describe ONLY instances tagged Project=iospharo-x64.  No long-lived keys land
# on the (ephemeral, possibly-reclaimed) box.  Idempotent.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# Honor CONFIG_FILE (set by provision.sh / the operator) so an arm64 run
# (config-arm.env) creates its own role/profile, not the x86 one.
source "${CONFIG_FILE:-$HERE/config.env}"

trust='{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"ec2.amazonaws.com"},"Action":"sts:AssumeRole"}]}'

policy=$(cat <<JSON
{
  "Version": "2012-10-17",
  "Statement": [
    { "Sid": "S3Bucket", "Effect": "Allow",
      "Action": ["s3:ListBucket","s3:GetBucketLocation"],
      "Resource": "arn:aws:s3:::${BUCKET}" },
    { "Sid": "S3Objects", "Effect": "Allow",
      "Action": ["s3:GetObject","s3:PutObject","s3:DeleteObject"],
      "Resource": "arn:aws:s3:::${BUCKET}/*" },
    { "Sid": "DescribeForSelfTerminate", "Effect": "Allow",
      "Action": ["ec2:DescribeInstances","ec2:DescribeTags"], "Resource": "*" },
    { "Sid": "TerminateOnlyTagged", "Effect": "Allow",
      "Action": "ec2:TerminateInstances", "Resource": "*",
      "Condition": { "StringEquals": { "ec2:ResourceTag/Project": "${PROJECT_TAG}" } } }
  ]
}
JSON
)

if ! aws iam get-role --role-name "$ROLE_NAME" >/dev/null 2>&1; then
    aws iam create-role --role-name "$ROLE_NAME" \
        --assume-role-policy-document "$trust" \
        --description "iospharo x64 build spot instance role" >/dev/null
    echo "created role $ROLE_NAME"
fi
aws iam put-role-policy --role-name "$ROLE_NAME" \
    --policy-name "${ROLE_NAME}-policy" --policy-document "$policy"
echo "attached inline policy to $ROLE_NAME"

if ! aws iam get-instance-profile --instance-profile-name "$PROFILE_NAME" >/dev/null 2>&1; then
    aws iam create-instance-profile --instance-profile-name "$PROFILE_NAME" >/dev/null
    aws iam add-role-to-instance-profile \
        --instance-profile-name "$PROFILE_NAME" --role-name "$ROLE_NAME"
    echo "created instance profile $PROFILE_NAME and added role"
    # IAM is eventually consistent; give it a moment before RunInstances uses it.
    sleep 10
fi
echo "iam-setup: done"
