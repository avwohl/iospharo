#!/usr/bin/env bash
# preserve.sh — push all work off the (ephemeral) spot box so nothing is lost
# when it's reclaimed or idle-terminated.  Safe to run repeatedly.
#
#   $1 = reason string (for the note), default "manual"
#
# Two destinations:
#   1. git  — commit WIP and push the work branch to GitHub (durable, mergeable).
#   2. S3   — sync notes + build logs + uncommitted diff to the bucket.
set -uo pipefail

REASON="${1:-manual}"
REPO=/home/ubuntu/src/iospharo
BUCKET="${BUCKET:-iospharo-build-670060058357}"
WORK_BRANCH="${WORK_BRANCH:-jit-x86}"
S3_PREFIX="s3://${BUCKET}/x64-builder"
ts=$(date -u +%Y%m%dT%H%M%SZ)

mkdir -p /home/ubuntu/notes
{
    echo "preserve @ ${ts}  reason=${REASON}  host=$(hostname)"
    echo "load: $(cat /proc/loadavg)"
} >>/home/ubuntu/notes/preserve.log

if [ -d "$REPO/.git" ]; then
    cd "$REPO"
    # Capture uncommitted state to S3 even if we can't push.
    git -c safe.directory="$REPO" diff > "/home/ubuntu/notes/wip-${ts}.diff" 2>/dev/null || true
    git -c safe.directory="$REPO" status -sb > "/home/ubuntu/notes/status-${ts}.txt" 2>/dev/null || true

    # Commit WIP on the work branch and push.  Uses the deploy key in
    # ~/.ssh/iospharo-x64-deploy (configured via ~/.ssh/config Host github-x64).
    if ! git -c safe.directory="$REPO" diff --quiet || \
       ! git -c safe.directory="$REPO" diff --cached --quiet; then
        git -c safe.directory="$REPO" add -A
        git -c safe.directory="$REPO" \
            -c user.name="iospharo-x64-builder" \
            -c user.email="builder@iospharo.local" \
            commit -m "wip(x64): autosave on ${REASON} @ ${ts}" || true
    fi
    git -c safe.directory="$REPO" push origin "HEAD:${WORK_BRANCH}" || \
        echo "preserve: git push failed (will rely on S3)" >&2
fi

# Sync notes + build log to S3 (best effort).
aws s3 sync /home/ubuntu/notes "${S3_PREFIX}/notes/" --no-progress 2>/dev/null || \
    echo "preserve: s3 sync notes failed" >&2
[ -f "$REPO/build/build.log" ] && \
    aws s3 cp "$REPO/build/build.log" "${S3_PREFIX}/logs/build-${ts}.log" --no-progress 2>/dev/null || true

echo "preserve: done (${REASON})"
