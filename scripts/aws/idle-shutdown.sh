#!/usr/bin/env bash
# idle-shutdown.sh — runs every 5 min (iospharo-idle.timer).  If the box has
# been idle for IDLE_SECONDS, preserve state then terminate the instance.
#
# "Active" means any of: a logged-in SSH session, a build/dev process running
# (cmake/cc1plus/make/ninja/clang/gcc/node/claude/test_load_image), or 1-min
# load average >= 0.5.  Each active check refreshes the activity stamp.
set -uo pipefail

IDLE_SECONDS="${IDLE_SECONDS:-1800}"
STAMP=/var/lib/iospharo-last-active
PRESERVE=/opt/iospharo/preserve.sh

now=$(date +%s)
[ -f "$STAMP" ] || echo "$now" >"$STAMP"

active=0
# logged-in users (ssh)
[ "$(who | wc -l)" -gt 0 ] && active=1
# meaningful processes
if pgrep -x -f 'cmake|cc1plus|make|ninja|clang|cc1|gcc|node|claude|test_load_image|git' >/dev/null 2>&1; then
    active=1
fi
# load average
load1=$(awk '{print $1}' /proc/loadavg)
awk "BEGIN{exit !($load1 >= 0.5)}" && active=1

if [ "$active" -eq 1 ]; then
    echo "$now" >"$STAMP"
    echo "idle-shutdown: active (load=$load1), stamp refreshed"
    exit 0
fi

last=$(cat "$STAMP" 2>/dev/null || echo "$now")
idle=$(( now - last ))
echo "idle-shutdown: idle ${idle}s / threshold ${IDLE_SECONDS}s"

if [ "$idle" -ge "$IDLE_SECONDS" ]; then
    echo "idle-shutdown: threshold reached — preserving + terminating"
    [ -x "$PRESERVE" ] && "$PRESERVE" "idle-${idle}s" || true

    # IMDSv2 token -> instance id + region, then terminate self.
    TOKEN=$(curl -fsS -X PUT "http://169.254.169.254/latest/api/token" \
        -H "X-aws-ec2-metadata-token-ttl-seconds: 60" || true)
    IID=$(curl -fsS -H "X-aws-ec2-metadata-token: $TOKEN" \
        http://169.254.169.254/latest/meta-data/instance-id || true)
    REGION=$(curl -fsS -H "X-aws-ec2-metadata-token: $TOKEN" \
        http://169.254.169.254/latest/meta-data/placement/region || true)
    if [ -n "$IID" ] && [ -n "$REGION" ]; then
        aws ec2 terminate-instances --region "$REGION" --instance-ids "$IID" || \
            shutdown -h now
    else
        shutdown -h now
    fi
fi
