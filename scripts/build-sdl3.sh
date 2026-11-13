#!/bin/bash
# build-sdl3.sh — build the macOS SDL3 shared library (arm64 and x86_64).
#
#   scripts/build-sdl3.sh                 build both arches
#   scripts/build-sdl3.sh --stage DIR     also copy a universal libSDL3 into DIR
#
# WHY THIS EXISTS
#
# The image package pharo-graphics-pharosdl3 loads SDL3 through
# SDL3Library -> FFIMacLibraryFinder, which looks for libSDL3.dylib or
# libSDL3.0.dylib in: the image directory, <Smalltalk vm directory>/Plugins,
# <vm directory>/../Resources/lib, /opt/homebrew/lib, /usr/lib and
# /usr/local/lib.  Stock Cog ships Plugins/libSDL3.dylib.  This VM shipped no
# SDL3 at all, so every test of the package ended with "Cannot locate any of
# #('libSDL3.dylib' 'libSDL3.0.dylib')".
#
# This is a SEPARATE library from the SDL2 static slice that
# build-macos-slices.sh makes.  The SDL2 slice is linked into the VM and
# only gives headers and a symbol table (src/vm/FFI.cpp stubs SDL2 and bridges
# it to Metal).  The SDL3 library is a real shared library that the image
# loads with dlopen through FFI.
#
# The version matches the SDL3 in the stock Cog bundle (current version
# 401.10.0 is SDL 3.4.10), so the package gives the same result on both VMs.
#
# Output: third-party-build/sdl3-install/macos-<arch>/lib/libSDL3.0.dylib
# CMakeLists.txt copies the file for the build arch into
# <build dir>/Plugins/ beside test_load_image when it exists.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_ROOT="${PROJECT_DIR}/third-party-build"
SOURCES_DIR="${BUILD_ROOT}/sources"
MIN_FLAG="-mmacosx-version-min=11.0"

SDL3_VERSION="3.4.10"
SDL3_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-${SDL3_VERSION}.tar.gz"
# Recorded 2026-11-13.  The same bytes came from two independent hosts:
# github.com releases and libsdl.org/release.  To bump the version, change the
# version above, download from both hosts, compare, then update this hash.
# Never "fix" a mismatch by pasting in the actual hash.
SDL3_SHA256="12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785"

STAGE_DIR=""
if [ "${1:-}" = "--stage" ]; then
    STAGE_DIR="${2:?--stage needs a directory}"
fi

log() { echo "[sdl3] $*"; }

SDKPATH="$(xcrun --sdk macosx --show-sdk-path)"
NCPU="$(sysctl -n hw.ncpu)"

tarball="${SOURCES_DIR}/SDL3-${SDL3_VERSION}.tar.gz"
src="${SOURCES_DIR}/SDL3-${SDL3_VERSION}"
mkdir -p "$SOURCES_DIR"
if [ ! -d "$src" ]; then
    [ -f "$tarball" ] || { log "downloading SDL3 ${SDL3_VERSION}"; curl -fsSL -o "$tarball" "$SDL3_URL"; }
    actual="$(shasum -a 256 "$tarball" | cut -d' ' -f1)"
    if [ "$actual" != "$SDL3_SHA256" ]; then
        mv -f "$tarball" "${tarball}.rejected"
        echo "[sdl3] CHECKSUM MISMATCH: got $actual, want $SDL3_SHA256" >&2
        exit 1
    fi
    log "checksum OK"
    tar xzf "$tarball" -C "$SOURCES_DIR"
fi

build_arch() {   # arch
    local arch="$1"
    local build_dir="${BUILD_ROOT}/sdl3-build/macos-${arch}"
    local install_dir="${BUILD_ROOT}/sdl3-install/macos-${arch}"
    if [ -f "${install_dir}/lib/libSDL3.0.dylib" ]; then log "${arch}: already built"; return; fi
    log "${arch}: building"
    rm -rf "$build_dir" "$install_dir"; mkdir -p "$build_dir"
    cmake -B "$build_dir" -S "$src" \
        -DCMAKE_INSTALL_PREFIX="$install_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_SYSROOT="$SDKPATH" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
        -DCMAKE_C_FLAGS="$MIN_FLAG" -DCMAKE_CXX_FLAGS="$MIN_FLAG" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF \
        > "$build_dir/configure.log" 2>&1 \
      && cmake --build "$build_dir" -j"$NCPU" > "$build_dir/make.log" 2>&1 \
      && cmake --install "$build_dir" > "$build_dir/install.log" 2>&1 \
      || { echo "[sdl3] ${arch} build failed; see ${build_dir}/*.log" >&2; exit 1; }
    log "${arch}: $(lipo -archs "${install_dir}/lib/libSDL3.0.dylib")"
}

build_arch arm64
build_arch x86_64

if [ -n "$STAGE_DIR" ]; then
    mkdir -p "$STAGE_DIR"
    lipo -create \
        "${BUILD_ROOT}/sdl3-install/macos-arm64/lib/libSDL3.0.dylib" \
        "${BUILD_ROOT}/sdl3-install/macos-x86_64/lib/libSDL3.0.dylib" \
        -output "${STAGE_DIR}/libSDL3.0.dylib"
    ln -sf libSDL3.0.dylib "${STAGE_DIR}/libSDL3.dylib"
    log "staged universal library in ${STAGE_DIR}"
fi
log "done"
