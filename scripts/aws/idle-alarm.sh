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
[ -n "${AWS_ACCESS_KEY_ID:-}" ] || source "$HERE/load-creds.sh" >/dev/null 2>&1 || true

IID="${1:?usage: idle-alarm.sh <instance-id> [region]}"
REGION="${2:-${AWS_DEFAULT_REGION:-us-east-2}}"
THRESH="${IDLE_CPU:-4}"
PERIODS="${IDLE_PERIODS:-4}"
PERIOD="${IDLE_PERIOD:-900}"
MINS=$(( PERIODS * PERIOD / 60 ))

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
