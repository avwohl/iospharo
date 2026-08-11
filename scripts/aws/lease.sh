#!/usr/bin/env bash
# lease.sh - thin client for the aws_watch keep-alive lease registry on awohl.com.
#
# Wraps the locked-down "aws-lease" forced-command SSH key and passes verbs
# straight through to the server's lease_cmd.py.  That key can do NOTHING on
# awohl.com except register / heartbeat / release / inspect instance leases.
#
#   ./lease.sh register <iid> [region]   # list a new box (provision.sh)
#   ./lease.sh release  <iid>            # drop a box's lease (teardown.sh)
#   ./lease.sh beat     <iid>            # manual heartbeat (rarely needed)
#   ./lease.sh fresh    [minutes]        # print instance_ids with a live lease
#   ./lease.sh list                      # dump all lease rows
#
# IMPORTANT: the recurring HEARTBEAT is NOT this script's job.  Only an actively-
# working Claude heartbeats, via the PostToolUse hook (aws-lease-beat-hook.sh).
# This wrapper just does the one-time register (creator) and release (teardown).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1090
# Honour CONFIG_FILE the way provision.sh does.  Hardcoding config.env tagged
# the arm box's lease row with the x86 project (`iospharo-x64`), which makes
# `lease.sh list` lie about which box is which.
_cfg="${CONFIG_FILE:-$HERE/config.env}"
case "$_cfg" in /*) ;; *) _cfg="$(cd "$(dirname "$_cfg")" 2>/dev/null && pwd)/$(basename "$_cfg")" ;; esac
[ -f "$_cfg" ] && source "$_cfg" 2>/dev/null || true

KEY="${AWS_LEASE_KEY:-$HOME/.ssh/aws-lease}"
HOST="${AWS_LEASE_HOST:-wohl@awohl.com}"
PORT="${AWS_LEASE_PORT:-24}"
PROJECT="${PROJECT_TAG:-iospharo}"

beat_ssh() {   # $* = verb + args sent to lease_cmd.py via the forced command
    ssh -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=10 \
        -o StrictHostKeyChecking=accept-new -p "$PORT" "$HOST" "$*"
}

verb="${1:?usage: lease.sh register|release|beat|fresh|list <iid> [region]}"
case "$verb" in
    register)
        # region defaults to a non-empty placeholder: the wire protocol is
        # whitespace-positional, so an empty middle field would shift project into
        # the region slot (provision always passes a real region).
        iid="${2:?register needs an instance id}"; region="${3:-na}"
        beat_ssh "register $iid $region $PROJECT mac:provision" ;;
    beat|release)
        iid="${2:?$verb needs an instance id}"
        beat_ssh "$verb $iid" ;;
    fresh|list)
        beat_ssh "$verb ${2:-}" ;;
    *)
        echo "lease.sh: unknown verb '$verb'" >&2; exit 2 ;;
esac
