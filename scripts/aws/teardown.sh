#!/usr/bin/env bash
# teardown.sh — remove the spot instance and (optionally) the supporting AWS
# resources.  The S3 bucket is preserved by default since it holds saved state;
# pass --bucket to delete it too.
#
#   ./scripts/aws/teardown.sh            # terminate instance + SG + role/profile
#   ./scripts/aws/teardown.sh --bucket   # also empty & delete the S3 bucket
#   ./scripts/aws/teardown.sh --instance-only
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# CONFIG_FILE=scripts/aws/config-arm.env ./teardown.sh tears down the arm64 box.
source "${CONFIG_FILE:-$HERE/config.env}"
source "$HERE/load-creds.sh"

DEL_BUCKET=0; INSTANCE_ONLY=0
for a in "$@"; do
    case "$a" in
        --bucket) DEL_BUCKET=1;;
        --instance-only) INSTANCE_ONLY=1;;
    esac
done

# --- terminate any instances for this project -------------------------------
IDS=$(aws ec2 describe-instances \
    --filters "Name=tag:Project,Values=$PROJECT_TAG" \
              "Name=instance-state-name,Values=pending,running,stopping,stopped" \
    --query 'Reservations[].Instances[].InstanceId' --output text)
if [ -n "$IDS" ]; then
    echo "terminating: $IDS"
    aws ec2 terminate-instances --instance-ids $IDS >/dev/null
    aws ec2 wait instance-terminated --instance-ids $IDS
fi
[ "$INSTANCE_ONLY" -eq 1 ] && { echo "instance-only teardown done"; exit 0; }

# --- security group ---------------------------------------------------------
SG_ID=$(aws ec2 describe-security-groups --filters "Name=group-name,Values=$SG_NAME" \
    --query 'SecurityGroups[0].GroupId' --output text 2>/dev/null || echo None)
[ "$SG_ID" != "None" ] && [ -n "$SG_ID" ] && aws ec2 delete-security-group --group-id "$SG_ID" && echo "deleted SG $SG_ID" || true

# --- IAM instance profile + role --------------------------------------------
aws iam remove-role-from-instance-profile --instance-profile-name "$PROFILE_NAME" --role-name "$ROLE_NAME" 2>/dev/null || true
aws iam delete-instance-profile --instance-profile-name "$PROFILE_NAME" 2>/dev/null || true
aws iam delete-role-policy --role-name "$ROLE_NAME" --policy-name "${ROLE_NAME}-policy" 2>/dev/null || true
aws iam delete-role --role-name "$ROLE_NAME" 2>/dev/null || true
echo "removed IAM role/profile (if present)"

# --- GitHub deploy key ------------------------------------------------------
KID=$(gh repo deploy-key list --repo avwohl/iospharo --json id,title \
    -q ".[] | select(.title==\"$DEPLOY_KEY_TITLE\") | .id" 2>/dev/null || true)
[ -n "$KID" ] && gh repo deploy-key delete "$KID" --repo avwohl/iospharo && echo "deleted GitHub deploy key" || true

# Key pair is kept by default (cheap, lets you relaunch).  Uncomment to remove:
# aws ec2 delete-key-pair --key-name "$KEY_NAME"

if [ "$DEL_BUCKET" -eq 1 ]; then
    echo "emptying + deleting bucket $BUCKET ..."
    aws s3 rm "s3://$BUCKET" --recursive || true
    # remove all versions (bucket is versioned)
    aws s3api list-object-versions --bucket "$BUCKET" \
        --query '{Objects: Versions[].{Key:Key,VersionId:VersionId}}' --output json 2>/dev/null \
        | jq 'if .Objects then {Objects: .Objects} else empty end' >/tmp/del.json || true
    [ -s /tmp/del.json ] && aws s3api delete-objects --bucket "$BUCKET" --delete file:///tmp/del.json || true
    aws s3api delete-bucket --bucket "$BUCKET" && echo "bucket deleted" || true
fi
echo "teardown done"
