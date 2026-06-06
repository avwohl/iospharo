#!/usr/bin/env bash
# clone-and-build.sh — runs ON the instance as user `ubuntu`, after the deploy
# key has been delivered to ~/.ssh/iospharo-x64-deploy.  Clones the repo, makes
# the x86 work branch off the latest `jit`, builds the x64 VM, and smoke-tests
# it against a freshly downloaded Pharo image.
set -euxo pipefail

GIT_REMOTE="${GIT_REMOTE:-git@github.com:avwohl/iospharo.git}"
BASE_BRANCH="${BASE_BRANCH:-jit}"
WORK_BRANCH="${WORK_BRANCH:-jit-x86}"
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
if git ls-remote --exit-code --heads origin "$WORK_BRANCH" >/dev/null 2>&1; then
    git checkout -B "$WORK_BRANCH" "origin/$WORK_BRANCH"
else
    git checkout -B "$WORK_BRANCH" "$BASE_BRANCH"
fi

# Submodules (asmjit, pharo-headless-test) are public HTTPS — init them so the
# cmake add_subdirectory(third_party/asmjit) sees a populated tree.
git submodule update --init --recursive

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
