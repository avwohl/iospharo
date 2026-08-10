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
WORK_BRANCH="${WORK_BRANCH:-jit}"   # x86 merged into jit; jit-x86 deleted 2026-06-17
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
    # ~/.ssh/iospharo-x64-deploy (configured via ~/.ssh/config Host github.com).
    git -c safe.directory="$REPO" add -A

    # Never let an autosave move a submodule pointer.  The box's submodules
    # drift (a plain `submodule update` checks out the upstream default rather
    # than the fork branch named in .gitmodules), and `add -A` stages that drift
    # as if it were a deliberate pin change.  That is exactly how 27d378d2
    # reverted the asmjit Catalyst pin to unpatched upstream, breaking fresh
    # clones for two months before 86151021 restored it.  Pin bumps are a
    # deliberate act made on a dev machine — an autosave must never make one.
    #
    # Paths come from .gitmodules rather than a hardcoded list so a submodule
    # added later is protected automatically.  Falls back to the known pair if
    # .gitmodules is somehow unreadable, so this can only over-protect.
    SUBMODS=()
    while IFS= read -r p; do
        [ -n "$p" ] && SUBMODS+=("$p")
    done < <(git -c safe.directory="$REPO" config -f .gitmodules \
                 --get-regexp '^submodule\..*\.path$' 2>/dev/null | cut -d' ' -f2-)
    [ ${#SUBMODS[@]} -eq 0 ] && SUBMODS=(third_party/asmjit scripts/pharo-headless-test)

    # Record what we drop.  The 27d378d2 regression stayed invisible for two
    # months partly because nothing ever said a pin had been touched.
    dropped=$(git -c safe.directory="$REPO" diff --cached --name-only \
                  -- "${SUBMODS[@]}" 2>/dev/null)
    if [ -n "$dropped" ]; then
        {
            echo "preserve @ ${ts}: refused to autosave submodule-pointer drift:"
            git -c safe.directory="$REPO" diff --cached --submodule=short \
                -- "${SUBMODS[@]}" 2>/dev/null
        } | tee -a /home/ubuntu/notes/submodule-drift.log >&2
    fi
    git -c safe.directory="$REPO" reset -q -- "${SUBMODS[@]}" 2>/dev/null || true
    if ! git -c safe.directory="$REPO" diff --cached --quiet; then
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
