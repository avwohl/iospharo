#!/usr/bin/env bash
# provision.sh — stand up the iospharo x64 build spot instance end to end.
#
#   ./scripts/aws/provision.sh
#
# Idempotent: re-running reuses the bucket/key/SG/role and (unless one is
# already running) launches a fresh spot instance.  All live resource IDs are
# written to scripts/aws/state.env.  Tear everything down with teardown.sh.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/config.env"
source "$HERE/load-creds.sh"

aws sts get-caller-identity >/dev/null
echo "== identity OK =="

# Reset state file.
: >"$STATE_FILE"
note() { echo "$1=$2" >>"$STATE_FILE"; echo "  $1=$2"; }

# --- 1. S3 bucket (private, versioned) — lives in its own home region -------
BREGION="${BUCKET_REGION:-$AWS_DEFAULT_REGION}"
if ! aws s3api head-bucket --bucket "$BUCKET" --region "$BREGION" 2>/dev/null; then
    if [ "$BREGION" = "us-east-1" ]; then
        aws s3api create-bucket --bucket "$BUCKET" --region "$BREGION" >/dev/null
    else
        aws s3api create-bucket --bucket "$BUCKET" --region "$BREGION" \
            --create-bucket-configuration "LocationConstraint=$BREGION" >/dev/null
    fi
    aws s3api put-public-access-block --bucket "$BUCKET" --region "$BREGION" --public-access-block-configuration \
        BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true
    aws s3api put-bucket-versioning --bucket "$BUCKET" --region "$BREGION" --versioning-configuration Status=Enabled
fi
note BUCKET "$BUCKET"

# --- 2. IAM role + instance profile -----------------------------------------
bash "$HERE/iam-setup.sh"
note PROFILE_NAME "$PROFILE_NAME"

# --- 3. SSH key pair (for our login) ----------------------------------------
PEM="$HOME/.ssh/${KEY_NAME}.pem"
if ! aws ec2 describe-key-pairs --key-names "$KEY_NAME" >/dev/null 2>&1; then
    aws ec2 create-key-pair --key-name "$KEY_NAME" \
        --query 'KeyMaterial' --output text >"$PEM"
    chmod 600 "$PEM"
    echo "created key pair, saved $PEM"
elif [ ! -f "$PEM" ]; then
    echo "WARNING: key pair $KEY_NAME exists in AWS but $PEM is missing locally." >&2
    echo "Delete it (aws ec2 delete-key-pair --key-name $KEY_NAME) and re-run." >&2
    exit 1
fi
note KEY_NAME "$KEY_NAME"
note PEM "$PEM"

# --- 4. Security group: SSH from this machine's public IP -------------------
MYIP=$(curl -fsS https://checkip.amazonaws.com)/32
VPC_ID=$(aws ec2 describe-vpcs --filters Name=isDefault,Values=true \
    --query 'Vpcs[0].VpcId' --output text)
SG_ID=$(aws ec2 describe-security-groups --filters "Name=group-name,Values=$SG_NAME" \
    --query 'SecurityGroups[0].GroupId' --output text 2>/dev/null || echo "None")
if [ "$SG_ID" = "None" ] || [ -z "$SG_ID" ]; then
    SG_ID=$(aws ec2 create-security-group --group-name "$SG_NAME" \
        --description "iospharo x64 builder SSH" --vpc-id "$VPC_ID" \
        --query 'GroupId' --output text)
fi
# (Re)authorize SSH from our current IP; ignore duplicate errors.
aws ec2 authorize-security-group-ingress --group-id "$SG_ID" \
    --protocol tcp --port 22 --cidr "$MYIP" >/dev/null 2>&1 || true
note SG_ID "$SG_ID"
note SSH_CIDR "$MYIP"

# --- 5. Latest Ubuntu 24.04 x86_64 AMI via SSM public parameter -------------
AMI_ID=$(aws ssm get-parameter \
    --name /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id \
    --query 'Parameter.Value' --output text)
note AMI_ID "$AMI_ID"

# --- 6. Already running? ----------------------------------------------------
EXISTING=$(aws ec2 describe-instances \
    --filters "Name=tag:Project,Values=$PROJECT_TAG" \
              "Name=instance-state-name,Values=pending,running" \
    --query 'Reservations[].Instances[].InstanceId' --output text)
if [ -n "$EXISTING" ]; then
    # Reuse the running box.  Set the shell vars (not just note them) so the
    # wait/IP/SSH steps below don't hit an unbound var under `set -u`.
    INSTANCE_ID="$(echo "$EXISTING" | awk '{print $1}')"
    INSTANCE_TYPE_USED="existing"
    LIFECYCLE="existing"
    echo "An instance is already running for $PROJECT_TAG: $INSTANCE_ID"
    note INSTANCE_ID "$INSTANCE_ID"
else
    # --- 7. Launch: spot across types x AZs, then on-demand as last resort ---
    BDM="[{\"DeviceName\":\"/dev/sda1\",\"Ebs\":{\"VolumeSize\":${ROOT_VOLUME_GB},\"VolumeType\":\"gp3\",\"DeleteOnTermination\":true}}]"
    TAGS="ResourceType=instance,Tags=[{Key=Name,Value=$INSTANCE_NAME},{Key=Project,Value=$PROJECT_TAG}]"
    VTAGS="ResourceType=volume,Tags=[{Key=Project,Value=$PROJECT_TAG}]"
    SPOT_MARKET='MarketType=spot,SpotOptions={SpotInstanceType=one-time,InstanceInterruptionBehavior=terminate}'
    SUBNETS=$(aws ec2 describe-subnets --filters "Name=vpc-id,Values=$VPC_ID" \
        Name=default-for-az,Values=true --query 'Subnets[].SubnetId' --output text)

    launch_try() {   # $1=type  $2=market-args ("" => on-demand)  $3=subnet
        local extra=()
        [ -n "$2" ] && extra+=(--instance-market-options "$2")
        [ -n "$3" ] && extra+=(--subnet-id "$3")
        aws ec2 run-instances \
            --image-id "$AMI_ID" --instance-type "$1" --count 1 \
            --key-name "$KEY_NAME" --security-group-ids "$SG_ID" \
            --iam-instance-profile "Name=$PROFILE_NAME" \
            --block-device-mappings "$BDM" \
            --metadata-options 'HttpTokens=required,HttpEndpoint=enabled' \
            --user-data "file://$HERE/bootstrap.sh" \
            --tag-specifications "$TAGS" "$VTAGS" \
            "${extra[@]}" \
            --query 'Instances[0].InstanceId' --output text 2>&1
    }

    INSTANCE_ID=""; INSTANCE_TYPE_USED=""; LIFECYCLE=""
    # Pass 1: spot — every candidate type across every default subnet (AZ).
    # Skipped when FORCE_ONDEMAND=1: this fresh account's spot is reclaimed
    # ~every 10 min even at 16 vCPU, which kills multi-minute verification runs.
    if [ "${FORCE_ONDEMAND:-0}" != "1" ]; then
    for TYPE in $INSTANCE_TYPE $INSTANCE_TYPE_FALLBACKS; do
        for SUBNET in $SUBNETS; do
            echo "trying spot $TYPE in $SUBNET ..."
            set +e; OUT=$(launch_try "$TYPE" "$SPOT_MARKET" "$SUBNET"); set -e
            if [[ "$OUT" == i-* ]]; then INSTANCE_ID="$OUT"; INSTANCE_TYPE_USED="$TYPE"; LIFECYCLE=spot; break 2; fi
            echo "  -> $OUT"
            # Quota/cap error => this type is too big for the spot limit; skip it.
            if echo "$OUT" | grep -qi 'MaxSpotInstanceCountExceeded\|InstanceLimitExceeded\|VcpuLimitExceeded'; then
                echo "  ($TYPE exceeds spot vCPU limit — skipping type)"; break
            fi
            # else InsufficientInstanceCapacity / transient => next subnet.
        done
    done
    fi  # end FORCE_ONDEMAND guard
    # Pass 2: on-demand (last resort after spot, or the direct path when
    # FORCE_ONDEMAND=1).  Fits the 16-vCPU on-demand quota.
    if [ -z "$INSTANCE_ID" ] && [ "${ALLOW_ONDEMAND:-1}" = "1" ]; then
        [ "${FORCE_ONDEMAND:-0}" = "1" ] && echo "FORCE_ONDEMAND=1 — launching on-demand" \
                                        || echo "spot unavailable in all types/AZs — falling back to ON-DEMAND"
        for TYPE in $INSTANCE_TYPE $INSTANCE_TYPE_FALLBACKS; do
            for SUBNET in $SUBNETS; do
                echo "trying on-demand $TYPE in $SUBNET ..."
                set +e; OUT=$(launch_try "$TYPE" "" "$SUBNET"); set -e
                if [[ "$OUT" == i-* ]]; then INSTANCE_ID="$OUT"; INSTANCE_TYPE_USED="$TYPE"; LIFECYCLE=on-demand; break 2; fi
                echo "  -> $OUT"
                echo "$OUT" | grep -qi 'InstanceLimitExceeded\|VcpuLimitExceeded' && { echo "  ($TYPE exceeds on-demand quota — skipping type)"; break; }
            done
        done
    fi
    [ -n "$INSTANCE_ID" ] || { echo "could not launch any instance (spot + on-demand exhausted)"; exit 1; }
    note INSTANCE_ID "$INSTANCE_ID"
    note INSTANCE_TYPE_USED "$INSTANCE_TYPE_USED"
    note LIFECYCLE "$LIFECYCLE"
    echo "launched $INSTANCE_ID ($INSTANCE_TYPE_USED, $LIFECYCLE)"
fi

# --- 8. Wait for running + public IP ----------------------------------------
echo "waiting for instance to run ..."
aws ec2 wait instance-running --instance-ids "$INSTANCE_ID"
PUBIP=$(aws ec2 describe-instances --instance-ids "$INSTANCE_ID" \
    --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
note PUBLIC_IP "$PUBIP"
echo "instance $INSTANCE_ID @ $PUBIP"

# --- 9. Wait for SSH ---------------------------------------------------------
echo "waiting for SSH ..."
for i in $(seq 1 40); do
    if timeout 10 ssh -i "$PEM" -o StrictHostKeyChecking=accept-new \
        -o ConnectTimeout=8 -o BatchMode=yes ubuntu@"$PUBIP" true 2>/dev/null; then
        echo "SSH up"; break
    fi
    sleep 15
done

# --- 10. Wait for cloud-init/bootstrap to finish ----------------------------
echo "waiting for bootstrap (cloud-init) to finish ..."
timeout 900 ssh -i "$PEM" -o StrictHostKeyChecking=accept-new ubuntu@"$PUBIP" \
    'while [ ! -f /var/lib/iospharo-bootstrap.done ]; do sleep 10; done; echo bootstrap-done'

# --- 11. Deploy key: generate, register with GitHub, ship to box ------------
DKEY="$HOME/.ssh/${KEY_NAME}-deploy"
[ -f "$DKEY" ] || ssh-keygen -t ed25519 -N "" -C "$DEPLOY_KEY_TITLE" -f "$DKEY"
# Re-register so the registered pubkey always matches our local private key.
EXIST_KEY_ID=$(gh repo deploy-key list --repo avwohl/iospharo --json id,title \
    -q ".[] | select(.title==\"$DEPLOY_KEY_TITLE\") | .id" 2>/dev/null || true)
[ -n "$EXIST_KEY_ID" ] && gh repo deploy-key delete "$EXIST_KEY_ID" --repo avwohl/iospharo || true
gh repo deploy-key add "$DKEY.pub" --repo avwohl/iospharo --title "$DEPLOY_KEY_TITLE" --allow-write
scp -i "$PEM" -o StrictHostKeyChecking=accept-new "$DKEY" ubuntu@"$PUBIP":/home/ubuntu/.ssh/iospharo-x64-deploy

# --- 12. Ship on-instance scripts + env, enable units -----------------------
scp -i "$PEM" "$HERE/idle-shutdown.sh" "$HERE/spot-watch.sh" "$HERE/preserve.sh" \
    "$HERE/clone-and-build.sh" ubuntu@"$PUBIP":/tmp/
ssh -i "$PEM" ubuntu@"$PUBIP" "sudo install -d /opt/iospharo && \
    sudo install -m755 /tmp/idle-shutdown.sh /tmp/spot-watch.sh /tmp/preserve.sh /opt/iospharo/ && \
    install -m755 /tmp/clone-and-build.sh /home/ubuntu/clone-and-build.sh && \
    printf 'BUCKET=%s\nWORK_BRANCH=%s\nBASE_BRANCH=%s\nIDLE_SECONDS=%s\nGIT_REMOTE=%s\n' \
        '$BUCKET' '$WORK_BRANCH' '$BASE_BRANCH' '$IDLE_SECONDS' '$GIT_REMOTE' | sudo tee /opt/iospharo/env >/dev/null && \
    sudo systemctl daemon-reload && \
    sudo systemctl enable --now iospharo-idle.timer iospharo-spot-watch.service"

# --- 13. Clone + build (backgrounded on the box; tail with ssh) -------------
echo "kicking off clone-and-build on the instance (logs: build/build.log) ..."
ssh -i "$PEM" ubuntu@"$PUBIP" \
    "BUCKET='$BUCKET' WORK_BRANCH='$WORK_BRANCH' BASE_BRANCH='$BASE_BRANCH' GIT_REMOTE='$GIT_REMOTE' \
     nohup bash /home/ubuntu/clone-and-build.sh >/home/ubuntu/clone-and-build.log 2>&1 &"

cat <<EOF

=== PROVISIONED ===
  instance : $INSTANCE_ID  (${INSTANCE_TYPE_USED:-existing})
  public ip: $PUBIP
  ssh      : ssh -i $PEM ubuntu@$PUBIP
  bucket   : s3://$BUCKET
  branch   : $WORK_BRANCH (off $BASE_BRANCH)
  idle term: ${IDLE_SECONDS}s
State written to $STATE_FILE
Watch the build:  ssh -i $PEM ubuntu@$PUBIP 'tail -f clone-and-build.log'
EOF
