#!/usr/bin/env bash
# reap.sh — CLIENT-SIDE idle failsafe.  Sweeps every iospharo-tagged EC2
# instance and terminates the ones "not in use": idle CPU over the recent
# window, or past a hard max age.  Runs from your machine, so it works even
# when a box's own idle-shutdown timer is disabled/broken — which is exactly
# how the x64 box ran 50h idle (its iospharo-idle.timer was 'disabled').
#
# Safe to run by hand or from a local cron (e.g. every 15 min):
#   */15 * * * * /Users/wohl/src/iospharo/scripts/aws/reap.sh >>/tmp/iospharo-reap.log 2>&1
#
#   ./reap.sh                 # ACT: terminate idle/old iospharo boxes
#   ./reap.sh --dry-run       # report only, terminate nothing
#
# Tunables (env):
#   IDLE_CPU=5      avg %CPU over LOOKBACK_MIN below which a box counts as idle
#   LOOKBACK_MIN=60 CPU averaging window (minutes)
#   MAX_AGE_H=12    hard cap: terminate at this age regardless of activity
#   GRACE_MIN=30    never touch a box younger than this (still booting/building)
#   REGIONS="us-east-2"   space-separated regions to sweep
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# Project region MUST be set before load-creds.sh (which otherwise defaults
# AWS_DEFAULT_REGION to us-east-1 — the wrong region; our boxes are in us-east-2,
# so a bare cron run would sweep an empty region and silently reap nothing).
: "${AWS_DEFAULT_REGION:=us-east-2}"
export AWS_DEFAULT_REGION
[ -n "${AWS_ACCESS_KEY_ID:-}" ] || source "$HERE/load-creds.sh" >/dev/null 2>&1 || true

IDLE_CPU="${IDLE_CPU:-5}"
LOOKBACK_MIN="${LOOKBACK_MIN:-60}"
MAX_AGE_H="${MAX_AGE_H:-12}"
GRACE_MIN="${GRACE_MIN:-30}"
REGIONS="${REGIONS:-${AWS_DEFAULT_REGION:-us-east-2}}"
DRY=0; [ "${1:-}" = "--dry-run" ] && DRY=1

now=$(date -u +%s)
start_iso=$(python3 -c "import datetime,sys;print((datetime.datetime.now(datetime.timezone.utc)-datetime.timedelta(minutes=int(sys.argv[1]))).strftime('%Y-%m-%dT%H:%M:%SZ'))" "$LOOKBACK_MIN")
end_iso=$(python3 -c "import datetime;print(datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'))")

reaped=0; kept=0
for REGION in $REGIONS; do
    # running iospharo-* instances: id \t launch \t name
    rows=$(aws ec2 describe-instances --region "$REGION" \
        --filters "Name=tag:Project,Values=iospharo-*" \
                  "Name=instance-state-name,Values=running" \
        --query 'Reservations[].Instances[].[InstanceId,LaunchTime,Tags[?Key==`Name`]|[0].Value]' \
        --output text 2>/dev/null)
    [ -z "$rows" ] && continue
    while IFS=$'\t' read -r IID LAUNCH NAME; do
        [ -n "$IID" ] || continue
        launch_s=$(python3 -c "import datetime,sys;print(int(datetime.datetime.strptime(sys.argv[1].replace('+00:00','Z'),'%Y-%m-%dT%H:%M:%S%z').timestamp()))" "$LAUNCH" 2>/dev/null || echo "$now")
        age_min=$(( (now - launch_s) / 60 ))
        if [ "$age_min" -lt "$GRACE_MIN" ]; then
            echo "keep   $IID ($NAME) [$REGION]: age ${age_min}m < grace ${GRACE_MIN}m"
            kept=$((kept+1)); continue
        fi
        reason=""
        if [ "$age_min" -ge "$(( MAX_AGE_H * 60 ))" ]; then
            reason="age $((age_min/60))h >= max ${MAX_AGE_H}h"
        else
            cpu=$(aws cloudwatch get-metric-statistics --region "$REGION" --namespace AWS/EC2 \
                --metric-name CPUUtilization --dimensions Name=InstanceId,Value="$IID" \
                --start-time "$start_iso" --end-time "$end_iso" --period "$(( LOOKBACK_MIN*60 ))" \
                --statistics Average --query 'Datapoints[0].Average' --output text 2>/dev/null)
            if [ -n "$cpu" ] && [ "$cpu" != "None" ]; then
                if awk "BEGIN{exit !($cpu < $IDLE_CPU)}"; then
                    reason="idle ${cpu}% < ${IDLE_CPU}% over ${LOOKBACK_MIN}m"
                fi
            fi
        fi
        if [ -n "$reason" ]; then
            if [ "$DRY" -eq 1 ]; then
                echo "WOULD-REAP $IID ($NAME) [$REGION]: $reason"
            else
                aws ec2 terminate-instances --region "$REGION" --instance-ids "$IID" >/dev/null 2>&1 \
                    && echo "REAPED $IID ($NAME) [$REGION]: $reason" \
                    || echo "FAILED to terminate $IID ($NAME) [$REGION]: $reason"
                aws cloudwatch delete-alarms --region "$REGION" \
                    --alarm-names "iospharo-idle-terminate-$IID" >/dev/null 2>&1 || true
            fi
            reaped=$((reaped+1))
        else
            echo "keep   $IID ($NAME) [$REGION]: age ${age_min}m cpu ${cpu:-?}%"
            kept=$((kept+1))
        fi
    done <<< "$rows"
done
echo "reap: ${reaped} $([ "$DRY" -eq 1 ] && echo would-reap || echo reaped), ${kept} kept"
