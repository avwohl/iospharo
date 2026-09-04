#!/usr/bin/env bash
# clone-and-build.sh — runs ON the instance as user `ubuntu`, after the deploy
# key has been delivered to ~/.ssh/iospharo-x64-deploy.  Clones the repo, makes
# the x86 work branch off the latest `jit`, builds the x64 VM, and smoke-tests
# it against a freshly downloaded Pharo image.
set -euxo pipefail

GIT_REMOTE="${GIT_REMOTE:-git@github.com:avwohl/iospharo.git}"
BASE_BRANCH="${BASE_BRANCH:-jit}"
WORK_BRANCH="${WORK_BRANCH:-jit}"   # x86 merged into jit; jit-x86 deleted 2026-06-17
REPO=/home/ubuntu/src/iospharo

# --- ssh config so `git` uses the scoped deploy key for github --------------
KEY=~/.ssh/iospharo-x64-deploy
chmod 600 "$KEY"
if ! grep -q 'Host github.com' ~/.ssh/config 2>/dev/null; then
    cat >>~/.ssh/config <<EOF
Host github.com
    HostName github.com
    User git
    IdentityFile $KEY
    IdentitiesOnly yes
    StrictHostKeyChecking accept-new
EOF
    chmod 600 ~/.ssh/config
fi

# --- clone (or update) -------------------------------------------------------
if [ ! -d "$REPO/.git" ]; then
    git clone "$GIT_REMOTE" "$REPO"
fi
cd "$REPO"
git fetch origin
git checkout "$BASE_BRANCH"
git pull --ff-only origin "$BASE_BRANCH"
# Create or fast-forward the work branch from base.
#
# A pre-existing remote work branch used to win unconditionally.  That is how a
# 2026-08-11 arm box built `jit-arm-linux` as it stood on 2026-06-10 — a stale
# spot-interruption autosave — reported "clone-and-build.sh complete", and ran
# a whole sweep against two-month-old code.  Reuse the remote branch ONLY when
# it already contains base; otherwise say so and reset it to base.
#
# Note what this guard can and cannot do.  On the x86 config BASE_BRANCH and
# WORK_BRANCH are BOTH `jit`, so "origin/jit contains origin/jit" is trivially
# true and the else-arm below is unreachable -- the check only bites when a
# topic WORK_BRANCH has drifted behind base, which is the arm config's shape
# (jit-arm-linux) and exactly the 2026-08-11 case it was written for.  Against
# a stale autosave landing on `jit` itself it could do nothing, because such an
# autosave is a DESCENDANT of base, not behind it.  That hole is closed at the
# source instead: preserve.sh no longer puts an autosave in git at all.
if git ls-remote --exit-code --heads origin "$WORK_BRANCH" >/dev/null 2>&1; then
    if git merge-base --is-ancestor "origin/$BASE_BRANCH" "origin/$WORK_BRANCH"; then
        git checkout -B "$WORK_BRANCH" "origin/$WORK_BRANCH"
    else
        echo "WARNING: origin/$WORK_BRANCH does not contain origin/$BASE_BRANCH" >&2
        echo "  ($(git log --oneline -1 "origin/$WORK_BRANCH"))" >&2
        echo "  Resetting it to $BASE_BRANCH so this box builds current code." >&2
        git checkout -B "$WORK_BRANCH" "origin/$BASE_BRANCH"
    fi
else
    git checkout -B "$WORK_BRANCH" "$BASE_BRANCH"
fi
echo "== work branch $WORK_BRANCH at $(git log --oneline -1) =="

# Submodules.  third_party/asmjit is REQUIRED (cmake add_subdirectory needs a
# populated tree).  scripts/pharo-headless-test is only the SUnit harness (.st
# files) and is OPTIONAL for building — its gitlink is sometimes an unpushed
# commit, so a plain `--init --recursive` aborts on it and (worse) leaves asmjit
# half-initialized.  Init asmjit explicitly with a clean-retry, and never let the
# optional harness submodule fail the build.
if ! git submodule update --init third_party/asmjit 2>&1 \
        || [ ! -f third_party/asmjit/asmjit/core/virtmem.cpp ]; then
    echo "asmjit submodule incomplete — forcing clean re-init"
    git submodule deinit -f third_party/asmjit 2>/dev/null || true
    rm -rf .git/modules/third_party/asmjit third_party/asmjit
    git submodule update --init third_party/asmjit
fi
[ -f third_party/asmjit/asmjit/core/virtmem.cpp ] \
    || { echo "FATAL: asmjit did not populate"; exit 1; }

# "Populated" is NOT the same as "at the pinned commit", and the difference bit
# us badly.  Our asmjit pin lives on the fork branch named in .gitmodules
# (iospharo-catalyst); that commit is NOT reachable from the fork's default
# branch.  A default submodule clone therefore fetches master only, fails to
# check out the pin, and leaves the tree sitting on master — where virtmem.cpp
# still exists, so the check above passes.  The box then builds against
# unpatched asmjit while the gitlink shows as modified, and the autosave used to
# commit that drift as a real pin change (27d378d2, which reverted the Catalyst
# fix for two months).  Demand the exact pinned SHA, fetching the branch if the
# initial update could not reach it.
want=$(git rev-parse "HEAD:third_party/asmjit")
have=$(git -C third_party/asmjit rev-parse HEAD 2>/dev/null || echo none)
if [ "$want" != "$have" ]; then
    echo "asmjit at ${have}, pin wants ${want} — fetching pinned commit"
    br=$(git config -f .gitmodules --get submodule.third_party/asmjit.branch 2>/dev/null || true)
    if [ -n "$br" ]; then
        git -C third_party/asmjit fetch --tags origin "$br" || true
    fi
    git -C third_party/asmjit fetch --tags origin "$want" 2>/dev/null \
        || git -C third_party/asmjit fetch --tags origin || true
    git -C third_party/asmjit checkout -q --detach "$want" \
        || { echo "FATAL: asmjit pin ${want} unreachable after fetch"; exit 1; }
fi
have=$(git -C third_party/asmjit rev-parse HEAD)
[ "$want" = "$have" ] \
    || { echo "FATAL: asmjit at ${have}, expected pinned ${want}"; exit 1; }
echo "asmjit at pinned commit ${want}"
git submodule update --init scripts/pharo-headless-test 2>/dev/null \
    || echo "WARN: pharo-headless-test submodule unavailable (SUnit harness only) — build proceeds"

# --- build the x64 VM --------------------------------------------------------
# Default to LTO OFF: a ~3-5 min build comfortably finishes inside a spot
# lifetime (a full LTO build is ~15 min — long enough to get reclaimed
# mid-link), and the x86 JIT dev loop doesn't need an LTO-optimized binary.
# Override with PHARO_DISABLE_LTO= (empty) for a release-grade build.
mkdir -p build
PHARO_DISABLE_LTO="${PHARO_DISABLE_LTO-1}" ./scripts/build-linux.sh 2>&1 | tee build/build.log

# --- smoke test against a fresh Pharo image ---------------------------------
HARNESS=/home/ubuntu/harness
mkdir -p "$HARNESS"
if [ ! -f "$HARNESS/Pharo.image" ]; then
    (cd "$HARNESS" && curl -sL https://get.pharo.org/64/130 | bash)
fi
echo "=== smoke test: test_load_image ==="
SMOKE=/home/ubuntu/smoke-result.txt
# The smoke exit is informational, not pass/fail: a headless image that never
# self-quits makes `timeout` return 124, and the VM may exit non-zero while x86
# JIT work is in progress.  Disable set -e/pipefail around it so a non-zero
# exit doesn't abort the script before the S3 upload below.
set +e +o pipefail
{
    echo "host=$(uname -m) cpus=$(nproc) date=$(date -u +%Y%m%dT%H%M%SZ)"
    echo "binary: $(file ./build/test_load_image)"
    echo "--- test_load_image output (90s cap) ---"
    timeout 90 ./build/test_load_image "$HARNESS/Pharo.image" 2>&1 | tail -45
    echo "--- exit: ${PIPESTATUS[0]} (124=timeout/headless-no-quit — both fine) ---"
} 2>&1 | tee "$SMOKE"
set -e -o pipefail

# Capture the build + smoke result to S3 so it survives a spot reclaim.
# Arch-keyed prefix ($(uname -m): x86_64 | aarch64) so an arm64 box's artifacts
# never clobber the x86 box's, and vice versa.
S3="s3://${BUCKET:-iospharo-build-670060058357}/$(uname -m)-builder/smoke"
ts=$(date -u +%Y%m%dT%H%M%SZ)
aws s3 cp "$SMOKE" "$S3/result-${ts}.txt" --no-progress 2>/dev/null || true
[ -f build/build.log ] && aws s3 cp build/build.log "$S3/build-${ts}.log" --no-progress 2>/dev/null || true

echo "clone-and-build.sh complete; built on $(uname -m) / $(nproc) cpus"
