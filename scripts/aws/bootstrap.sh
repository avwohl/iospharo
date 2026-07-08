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

# --- OpenSSL 1.1 + libssh2 compat for the bundled pharo libgit2 --------------
# The stock Cog VM (get.pharo.org) bundles libgit2.so.1.4.4, which links the
# OpenSSL 1.1 world (libssl.so.1.1 / libcrypto.so.1.1 / libssh2.so.1.9.0).
# Without these, UFFI loading libgit2 for Metacello github:// loads SEGFAULTS.
# Available on 22.04 (jammy); ABSENT on 24.04 (noble) — see provision.sh note.
apt-get install -y --no-install-recommends libssl1.1 libssh2-1 2>/dev/null || \
  echo "WARN: libssl1.1/libssh2-1 unavailable (24.04?) — catalog github loads will fail"

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

# --- ship the preservation + idle scripts onto the box ------------------------
# provision.sh scp's idle-shutdown.sh and spot-watch.sh to /opt/iospharo before
# enabling these units; we just install the unit files here.
install -d /opt/iospharo

cat >/etc/systemd/system/iospharo-idle.service <<'UNIT'
[Unit]
Description=iospharo idle auto-terminate check
[Service]
Type=oneshot
EnvironmentFile=-/opt/iospharo/env
ExecStart=/opt/iospharo/idle-shutdown.sh
UNIT

cat >/etc/systemd/system/iospharo-idle.timer <<'UNIT'
[Unit]
Description=run iospharo idle check every 5 minutes
[Timer]
OnBootSec=15min
OnUnitActiveSec=5min
[Install]
WantedBy=timers.target
UNIT

cat >/etc/systemd/system/iospharo-spot-watch.service <<'UNIT'
[Unit]
Description=iospharo spot-interruption watcher (preserve state on 2-min notice)
[Service]
Type=simple
EnvironmentFile=-/opt/iospharo/env
ExecStart=/opt/iospharo/spot-watch.sh
Restart=always
RestartSec=10
User=ubuntu
UNIT

# Timers/units are enabled by provision.sh AFTER the scripts are delivered.
systemctl daemon-reload

touch "$MARKER"
echo "bootstrap.sh complete"
