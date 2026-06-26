#!/usr/bin/env bash
# build-linux.sh — configure + build PharoVM on Linux
#
# Usage:
#   ./scripts/build-linux.sh                    # release build with JIT
#   PHARO_DISABLE_LTO=1 ./scripts/build-linux.sh   # skip LTO (faster build)
#
# Required system packages (Debian/Ubuntu):
#   apt install build-essential cmake pkg-config libffi-dev libsdl2-dev
#
# Required system packages (Fedora/RHEL):
#   dnf install gcc-c++ cmake pkgconfig libffi-devel SDL2-devel
#
# Required system packages (Arch):
#   pacman -S base-devel cmake pkgconfig libffi sdl2

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$REPO_ROOT"

mkdir -p build
cd build

# Configure: assume Release; pass through PHARO_DISABLE_LTO if set.
EXTRA_FLAGS=()
if [ -n "${PHARO_DISABLE_LTO:-}" ]; then
    EXTRA_FLAGS+=(-DPHARO_DISABLE_LTO=ON)
fi

# Crypto ON: native SHA1/MD5/DSA (DSAPrims) + a real OpenSSL SqueakSSL backend
# (sqGenericSSL.c, linked against system libssl/libcrypto — needs libssl-dev).
# Was OFF, which silently disabled native SHA1 and left HTTPS as ZdcPluginMissing.
# Override with PHARO_WITH_CRYPTO=OFF in the environment for a crypto-less build.
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DPHARO_WITH_CRYPTO="${PHARO_WITH_CRYPTO:-ON}" \
    "${EXTRA_FLAGS[@]}"

cmake --build . -j"$(nproc)"

echo
echo "Build complete:"
ls -la test_load_image 2>/dev/null && echo "  → test_load_image OK" || echo "  → test_load_image MISSING"
