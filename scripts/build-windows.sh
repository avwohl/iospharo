#!/usr/bin/env bash
#
# build-windows.sh - configure + build the Pharo VM on Windows.
#
# Windows is a clang-only target (Interpreter.cpp uses computed gotos + GNU
# inline asm, which MSVC cannot compile).  We use the MSYS2 CLANG64 toolchain:
# clang targeting x86_64-w64-windows-gnu (LLVM-MinGW), plus cmake, ninja,
# libffi and dlfcn-win32 from pacman.  See docs/windows-port-plan.md.
#
# Milestone 1 is headless + interpreter-only (JIT disabled — the x86-64 Tier-1
# JIT hardcodes the System V ABI; enabling it is milestone 2).
#
# Run it from the MSYS2 CLANG64 shell, or from anywhere via:
#
#   C:\msys64\usr\bin\bash.exe -lc \
#     "MSYSTEM=CLANG64 source /etc/profile; \
#      /c/temp/src/iospharo-jit/scripts/build-windows.sh"
#
set -euo pipefail

# Re-exec under the CLANG64 environment if we're not already in it, so the
# script works no matter how it was launched.
if [[ "${MSYSTEM:-}" != "CLANG64" ]]; then
    exec env MSYSTEM=CLANG64 /c/msys64/usr/bin/bash.exe -lc \
        "exec '$(cygpath -u "${BASH_SOURCE[0]}" 2>/dev/null || echo "$0")' $*"
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-win}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
TARGET="${1:-test_load_image}"

echo "== Toolchain =="
clang --version | head -1
cmake --version | head -1
ninja --version
pkg-config --modversion libffi >/dev/null 2>&1 \
    && echo "libffi: $(pkg-config --modversion libffi)" \
    || { echo "ERROR: libffi not found (pacman -S mingw-w64-clang-x86_64-libffi)"; exit 1; }

echo "== Configure =="
cmake -G Ninja -B "${BUILD_DIR}" -S "${REPO_ROOT}" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DPHARO_WITH_CRYPTO=OFF \
    -DPHARO_DISABLE_LTO=ON

echo "== Build (${TARGET}) =="
cmake --build "${BUILD_DIR}" --target "${TARGET}" -j

echo "== Done =="
ls -la "${BUILD_DIR}/${TARGET}.exe" 2>/dev/null || ls -la "${BUILD_DIR}/${TARGET}" 2>/dev/null || true
