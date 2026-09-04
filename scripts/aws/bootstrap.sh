#!/usr/bin/env bash
# bootstrap.sh — runs ONCE as root via cloud-init user-data on a fresh
# Ubuntu 24.04 spot instance (x86_64 OR arm64/Graviton — this script is
# arch-agnostic: the AWS CLI download keys off `uname -m`, and every apt
# package below is multi-arch).  Installs everything needed to build the
# iospharo JIT VM and run Claude Code, plus the spot-preservation and
# idle-shutdown machinery.  It does NOT clone the repo — that happens in
# clone-and-build.sh once the deploy key has been delivered.
#
# Re-runnable by hand: `sudo bash bootstrap.sh`.  Everything here is idempotent.
#
# This file IS the canonical record of the build environment.  To rebuild a
# future spot instance from scratch, that's all you need.
set -euxo pipefail

export DEBIAN_FRONTEND=noninteractive
MARKER=/var/lib/iospharo-bootstrap.done

# --- system build toolchain (matches scripts/build-linux.sh requirements) ----
apt-get update -y
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config ccache \
    clang lld gdb lldb \
    libffi-dev libsdl2-dev zlib1g-dev libssl-dev \
    libcairo2 libfreetype6 \
    git curl wget unzip jq xz-utils ca-certificates \
    python3 python3-pip python3-venv \
    htop tmux less

# NOTE: the bundled pharo libgit2 (for stock-Cog Metacello github:// loads in
# the catalog build) needs the OpenSSL 1.1 world (libssl.so.1.1 /
# libcrypto.so.1.1 / libssh2.so.1.9.0), which no current Ubuntu ships (jammy &
# noble both dropped libssl1.1).  The catalog build script bundles the focal
# .so files itself onto LD_LIBRARY_PATH — see build-merged-catalog-bisect.sh.

# --- AWS CLI v2 (for S3 sync + self-terminate via instance profile) ----------
if ! command -v aws >/dev/null 2>&1; then
    arch=$(uname -m)   # x86_64 on Intel/AMD, aarch64 on Graviton — both published
    curl -fsSL "https://awscli.amazonaws.com/awscli-exe-linux-${arch}.zip" -o /tmp/awscliv2.zip
    (cd /tmp && unzip -q -o awscliv2.zip && ./aws/install --update)
    rm -rf /tmp/aws /tmp/awscliv2.zip
fi

# --- Node.js 20 + Claude Code -------------------------------------------------
if ! command -v node >/dev/null 2>&1; then
    curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
    apt-get install -y nodejs
fi
npm install -g @anthropic-ai/claude-code || true

# --- work dir owned by ubuntu -------------------------------------------------
install -d -o ubuntu -g ubuntu /home/ubuntu/src

# --- spot preservation + idle shutdown ---------------------------------------
# NOT set up here any more.  That machinery is project-independent and lives in
# avwohl/aws_watch under spot/; provision.sh ships that directory to the box and
# runs `spot/install-box.sh`, which installs the scripts under $SPOT_PREFIX and
# writes and enables the systemd units.  Nothing to do at cloud-init time --
# the scripts cannot be delivered until the deploy key is, which is after this
# runs.

touch "$MARKER"
echo "bootstrap.sh complete"
