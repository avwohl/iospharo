#!/usr/bin/env bash
# aws-lease-beat-hook.sh [pre|post] - Claude Code hook: heartbeat the aws_watch
# keep-alive lease for the temporary cloud box this Claude is actively working on.
#
# This is the ONLY thing that updates a lease heartbeat, and it fires on Claude's
# tool use -- so the box stays alive exactly while a Claude is running / looping /
# in a goal, and the moment that Claude finishes and walks away the heartbeats
# stop, the lease goes stale, and aws_watch reaps the box. No cron, no daemon,
# no provisioning script ever beats; only a working Claude does.
#
# Wired as BOTH a PreToolUse hook (arg "pre") and a PostToolUse hook (arg "post").
# Each tool-use event sends a throttled beat. PreToolUse additionally starts a
# short-lived in-flight re-beater that keeps the lease fresh through a SINGLE
# long-running tool call (e.g. a multi-minute, low-CPU SUnit run that emits no
# tool events of its own); PostToolUse stops it. The re-beater also stops on its
# own if no `claude` process is left running (Claude died), so it can never keep
# a box alive past the Claude that owns it.
#
# Cheap + silent + NON-BLOCKING: every beat is detached with its own descriptors
# closed (it never holds the hook's stdout/stderr open), so it adds no latency to
# the tool call; the hook always exits 0.
#
# Box identity, cheapest-first (no network on either path):
#   1. $AWS_LEASE_IID                 - explicit (a Mac/control session driving a box).
#   2. EC2 Nitro DMI board_asset_tag  - the instance id, present only ON the box.
set -u

MODE="${1:-post}"
THROTTLE="${AWS_LEASE_THROTTLE:-300}"        # min seconds between throttled beats
REBEAT="${AWS_LEASE_REBEAT:-300}"            # re-beat cadence during a long call
MAXBEATS="${AWS_LEASE_MAXBEATS:-288}"        # safety cap on re-beats (288*300s=24h)
KEY="${AWS_LEASE_KEY:-$HOME/.ssh/aws-lease}"
HOST="${AWS_LEASE_HOST:-wohl@awohl.com}"
PORT="${AWS_LEASE_PORT:-24}"

IID="${AWS_LEASE_IID:-}"
[ -n "$IID" ] || IID="$(cat /sys/devices/virtual/dmi/id/board_asset_tag 2>/dev/null || true)"
case "$IID" in i-*) ;; *) exit 0 ;; esac     # not on/attached to a leased box

SLUG="${TMPDIR:-/tmp}/aws-lease-$IID"
STAMP="$SLUG.beat"
RBPID="$SLUG.rebeat.pid"

# One detached, fully fd-closed, non-blocking beat. Sends only the instance id;
# register (provision.sh) already set region/project, and a bare beat preserves
# them. Never waits, never blocks the tool call, never errors out.
beat() {
    ssh -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=10 \
        -o StrictHostKeyChecking=accept-new -p "$PORT" "$HOST" "beat $IID" \
        </dev/null >/dev/null 2>&1 &
}

throttled_beat() {
    local now last=0
    now="$(date +%s 2>/dev/null || echo 0)"
    [ -f "$STAMP" ] && last="$(cat "$STAMP" 2>/dev/null || echo 0)"
    case "$last" in ''|*[!0-9]*) last=0 ;; esac      # ignore a corrupt/partial stamp
    if [ "$now" -gt 0 ] && [ "$last" -gt 0 ] && [ $(( now - last )) -lt "$THROTTLE" ]; then
        return
    fi
    echo "$now" >"$STAMP" 2>/dev/null || true
    beat
}

stop_rebeater() {
    [ -f "$RBPID" ] || return 0
    local p; p="$(cat "$RBPID" 2>/dev/null || true)"
    case "$p" in ''|*[!0-9]*) ;; *) kill "$p" 2>/dev/null || true ;; esac
    rm -f "$RBPID" 2>/dev/null || true
}

case "$MODE" in
    pre)
        throttled_beat
        stop_rebeater                            # clear any prior in-flight re-beater
        ( i=0
          while [ "$i" -lt "$MAXBEATS" ]; do
              sleep "$REBEAT" || exit 0
              pgrep -f '[c]laude' >/dev/null 2>&1 || exit 0   # Claude gone -> stop
              ssh -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=10 \
                  -o StrictHostKeyChecking=accept-new -p "$PORT" "$HOST" "beat $IID" \
                  </dev/null >/dev/null 2>&1 || true
              i=$(( i + 1 ))
          done ) </dev/null >/dev/null 2>&1 &
        echo $! >"$RBPID" 2>/dev/null || true
        ;;
    *)  # post (default)
        stop_rebeater                            # call finished -> stop bridging
        throttled_beat
        ;;
esac
exit 0
