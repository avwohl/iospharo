#!/usr/bin/env bash
# preserve.sh — push all work off the (ephemeral) spot box so nothing is lost
# when it's reclaimed or idle-terminated.  Safe to run repeatedly.
#
#   $1 = reason string (for the note), default "manual"
#
# Two destinations:
#   1. git  — commit WIP and push it to a per-box `autosave/<instance-id>`
#            branch on GitHub (durable and fetchable, but never a branch anyone
#            builds from -- see "WHERE AN AUTOSAVE IS ALLOWED TO LAND" below).
#   2. S3   — sync notes + build logs + uncommitted diff to the bucket.
set -uo pipefail

REASON="${1:-manual}"
REPO=/home/ubuntu/src/iospharo
BUCKET="${BUCKET:-iospharo-build-670060058357}"
S3_PREFIX="s3://${BUCKET}/x64-builder"
ts=$(date -u +%Y%m%dT%H%M%SZ)

# --- WHERE AN AUTOSAVE IS ALLOWED TO LAND -----------------------------------
# NEVER the shared work branch.  This script snapshots the working tree with
# `git add -A` but commits it with the BOX's HEAD as parent, and those two can
# disagree arbitrarily.  The benign direction (stale parent, fresh tree) just
# makes an odd squashed commit; the dangerous direction is a box that cloned
# the current tip and then received a STALE tree -- `add -A` records the
# deletions, the parent IS the true tip, and the push is a clean fast-forward
# that GitHub accepts with no force flag.  `git diff --name-status e4c47863
# ede0fd65` shows that exact shape (D scripts/sweep-3way.sh), and 27d378d2
# (2026-06-03) reverted the asmjit Catalyst pin the same way and went unnoticed
# for 68 days.  origin/jit has no branch protection and the box holds a
# write-capable deploy key, so the push itself cannot be refused -- the only
# durable defence is to aim it somewhere nobody builds from.
#
# So: one namespace per box, `autosave/<instance-id>`.  The work is still
# durable and still fetchable (`git fetch origin autosave/<iid>`), it just
# cannot be what anyone's next `git pull` picks up.  Recovering from one is a
# deliberate act: fetch the ref, read the diff, cherry-pick what you want.
#
# PRESERVE_PUSH_BRANCH overrides it for a human who means it.  WORK_BRANCH is
# deliberately NOT consulted any more: it names the branch the box WORKS on
# (clone-and-build.sh checks it out), and conflating "what I build" with "where
# a crash dump lands" is the whole bug.
_iid="$(curl -s --max-time 2 -H 'X-aws-ec2-metadata-token-ttl-seconds: 60' \
            -X PUT http://169.254.169.254/latest/api/token 2>/dev/null \
        | { read -r t; [ -n "$t" ] && curl -s --max-time 2 \
            -H "X-aws-ec2-metadata-token: $t" \
            http://169.254.169.254/latest/meta-data/instance-id 2>/dev/null; })"
[ -n "$_iid" ] || _iid="$(hostname -s 2>/dev/null || echo unknown)"
PUSH_BRANCH="${PRESERVE_PUSH_BRANCH:-autosave/${_iid}}"

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
    if git -c safe.directory="$REPO" push -f origin "HEAD:refs/heads/${PUSH_BRANCH}"; then
        echo "preserve: pushed to ${PUSH_BRANCH} (autosave namespace, NOT a build branch)"
        echo "preserve @ ${ts}: pushed HEAD to origin/${PUSH_BRANCH}" \
            >>/home/ubuntu/notes/preserve.log
    else
        echo "preserve: git push failed (will rely on S3)" >&2
    fi
fi

# Sync notes + build log to S3 (best effort).
aws s3 sync /home/ubuntu/notes "${S3_PREFIX}/notes/" --no-progress 2>/dev/null || \
    echo "preserve: s3 sync notes failed" >&2
[ -f "$REPO/build/build.log" ] && \
    aws s3 cp "$REPO/build/build.log" "${S3_PREFIX}/logs/build-${ts}.log" --no-progress 2>/dev/null || true

echo "preserve: done (${REASON})"
