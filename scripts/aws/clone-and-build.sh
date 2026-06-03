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
mkdir -p build
PHARO_DISABLE_LTO="${PHARO_DISABLE_LTO:-}" ./scripts/build-linux.sh 2>&1 | tee build/build.log

# --- smoke test against a fresh Pharo image ---------------------------------
HARNESS=/home/ubuntu/harness
mkdir -p "$HARNESS"
if [ ! -f "$HARNESS/Pharo.image" ]; then
    (cd "$HARNESS" && curl -sL https://get.pharo.org/64/130 | bash)
fi
echo "=== smoke test: test_load_image ==="
timeout 120 ./build/test_load_image "$HARNESS/Pharo.image" 2>&1 | tail -40 || \
    echo "smoke test returned non-zero (expected while x86 JIT is WIP)"

echo "clone-and-build.sh complete; built on $(uname -m) / $(nproc) cpus"
