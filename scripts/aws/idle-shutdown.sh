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
# meaningful processes (NB: -f matches full cmdline; do NOT combine with -x,
# which would require the whole cmdline to equal the alternation and never match)
#
# `pharo` and the suite drivers MUST be here.  They were missing, and it cost a
# completed run: the 200-package A/B sweep spends most of its time in Metacello
# loads driven by the STOCK `pharo` VM, which is network-bound, so no listed
# process was running and 1-min load sat around 0.2 — well under the 0.5 floor
# below.  The box counted itself idle for 30 min and terminated ITSELF 74 min
# into the sweep (i-05fa7bff75e0eb0ad, 2026-08-11 07:18 UTC, confirmed in
# CloudTrail: caller was the instance's own role from its own IP).  All sweep
# results were lost, because preserve.sh syncs notes and build logs, not
# results.
#
# This is the same blind spot the README calls out for the CPU-based CloudWatch
# alarm ("false-kills low-CPU-but-active SUnit boxes") — it just also existed
# here, in the process-based check that was supposed to be the safe alternative.
if pgrep -f 'cmake|cc1plus|make|ninja|clang|gcc|node|claude|test_load_image|git |pharo|run-manifest|run-sweep|fullsuite|sunit' >/dev/null 2>&1; then
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
