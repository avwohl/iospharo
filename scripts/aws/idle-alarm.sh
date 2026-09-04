#!/usr/bin/env bash
# idle-alarm.sh — SERVER-SIDE idle failsafe.  Creates a CloudWatch alarm that
# TERMINATES the instance after ~1h of sustained low CPU.  This runs inside AWS,
# so it fires even when the box's own idle-shutdown timer is dead — which is
# exactly what bit us: the x64 box's iospharo-idle.timer was 'disabled', so it
# ran 50h idle.  Independent of the box's OS/cron entirely.
#
#   ./idle-alarm.sh <instance-id> [region]
#   IDLE_CPU=4 IDLE_PERIODS=4 IDLE_PERIOD=900 ./idle-alarm.sh i-abc us-east-2
#
# Defaults: <4% CPU averaged over 4 x 15min = 1h of idle -> terminate.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -n "${AWS_ACCESS_KEY_ID:-}" ] || source "${SPOT_DIR:-${AWS_WATCH_DIR:-$HOME/src/aws_watch}/spot}/load-creds.sh" >/dev/null 2>&1 || true

IID="${1:?usage: idle-alarm.sh <instance-id> [region]}"
REGION="${2:-${AWS_DEFAULT_REGION:-us-east-2}}"
PERIODS="${IDLE_PERIODS:-4}"
PERIOD="${IDLE_PERIOD:-900}"
MINS=$(( PERIODS * PERIOD / 60 ))

# CPUUtilization is a percentage of ALL vCPUs, so a fixed percentage means
# different amounts of real work on different box sizes — and this alarm
# TERMINATES.  On 2026-08-11 it killed a c7g.16xlarge (64 vCPU) an hour into a
# 24-worker package sweep: the workers are network-bound, so ~2-8 busy cores is
# 3-12% on 16 vCPU but only 0.8-3% on 64 vCPU, and the 1h average fell under the
# flat 4%.  Neither safety net caught it — the on-box idle-shutdown is
# process-aware and knew the sweep was running, and the keep-alive lease was
# live, but an `arn:aws:automate:...:ec2:terminate` alarm action bypasses both.
# So express the threshold in CORES, not percent: default "fewer than 0.64 cores
# busy", which is what 4% meant on the 16-vCPU box this default was written for.
IDLE_CORES="${IDLE_CORES:-0.64}"
VCPUS="$(aws ec2 describe-instances --region "$REGION" --instance-ids "$IID"     --query 'Reservations[].Instances[].CpuOptions.[CoreCount,ThreadsPerCore]'     --output text 2>/dev/null | awk 'NF==2{print $1*$2; exit}')"
if [ -n "${IDLE_CPU:-}" ]; then
    THRESH="$IDLE_CPU"                      # explicit override wins
elif [ -n "${VCPUS:-}" ] && [ "${VCPUS:-0}" -gt 0 ] 2>/dev/null; then
    THRESH="$(awk -v c="$IDLE_CORES" -v n="$VCPUS" 'BEGIN{printf "%.2f", 100*c/n}')"
    echo "idle-alarm: $VCPUS vCPU -> threshold ${THRESH}% (= ${IDLE_CORES} cores busy)"
else
    echo "idle-alarm: WARNING could not read vCPU count for $IID; using flat 4%" >&2
    THRESH=4
fi

aws cloudwatch put-metric-alarm --region "$REGION" \
    --alarm-name "iospharo-idle-terminate-$IID" \
    --alarm-description "Idle failsafe: terminate $IID after ${MINS}m of <${THRESH}% CPU" \
    --namespace AWS/EC2 --metric-name CPUUtilization \
    --dimensions Name=InstanceId,Value="$IID" \
    --statistic Average --period "$PERIOD" --evaluation-periods "$PERIODS" \
    --threshold "$THRESH" --comparison-operator LessThanThreshold \
    --treat-missing-data notBreaching \
    --alarm-actions "arn:aws:automate:${REGION}:ec2:terminate"

echo "idle-alarm: armed for $IID  (<${THRESH}% CPU for ${MINS}m -> auto-terminate)"
