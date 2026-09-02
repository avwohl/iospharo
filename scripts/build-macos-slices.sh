#!/bin/bash
# build-macos-slices.sh — build ONLY the macOS slice of libffi and SDL2.
#
#   scripts/build-macos-slices.sh
#
# WHY THIS EXISTS
#
# build-libffi.sh and build-sdl2.sh produce full xcframeworks: iOS device, iOS
# simulator, Mac Catalyst and macOS. Three of those four need the iOS SDKs,
# which ship inside Xcode.app and not in the Command Line Tools. On a host with
# only the CLT installed (`xcode-select -p` answers
# /Library/Developer/CommandLineTools), both scripts stop at their preflight
# and nothing is built -- including the one slice the CMake dev build needs.
#
# CMakeLists.txt reads exactly two paths per library for a native build:
#
#     Frameworks/libffi.xcframework/macos-arm64_x86_64/{Headers,libffi.a}
#     Frameworks/SDL2.xcframework/macos-arm64_x86_64/{Headers,libSDL2.a}
#
# It never opens Info.plist. So on a CLT-only host this script builds the two
# macOS slices with the same flags the full scripts use and lays them out in
# those directories. That is enough for `cmake -B build-rel` and
# `cmake -B build-x86`, i.e. for every headless test tier.
#
# WHAT IT IS NOT
#
# Not an xcframework. There is no Info.plist, so Xcode cannot consume the
# result and the Mac Catalyst / iOS app builds still need the full scripts run
# on a host with Xcode. Also `xcodebuild -create-xcframework` itself refuses to
# run against a CLT-only developer dir, so there is no cheap way to add the
# manifest here. Running the full scripts later replaces this layout wholesale
# (they `rm -rf` the output first), so the two never mix.
#
# The per-arch header dispatch for ffi.h / ffitarget.h is copied from
# build-libffi.sh verbatim, because the two arches genuinely differ (ABI enum
# and ffi_closure layout) and shipping one arch's copy is a known, measured
# bug -- see the comment block in make_universal there.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_ROOT="${PROJECT_DIR}/third-party-build"
SOURCES_DIR="${BUILD_ROOT}/sources"
FRAMEWORKS="${PROJECT_DIR}/Frameworks"
SLICE=macos-arm64_x86_64
MIN_FLAG="-mmacosx-version-min=11.0"

LIBFFI_VERSION="3.5.2"
LIBFFI_URL="https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/libffi-${LIBFFI_VERSION}.tar.gz"
LIBFFI_SHA256="f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc"
SDL2_VERSION="2.26.5"
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz"
SDL2_SHA256="ad8fea3da1be64c83c45b1d363a6b4ba8fd60f5bde3b23ec73855709ec5eabf7"

log() { echo "[macos-slices] $*"; }

sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }

fetch() {   # name version url sha256
    local name="$1" ver="$2" url="$3" sha="$4"
    local tarball="${SOURCES_DIR}/${name}-${ver}.tar.gz"
    local src="${SOURCES_DIR}/${name}-${ver}"
    mkdir -p "$SOURCES_DIR"
    if [ -d "$src" ]; then log "source present: $src"; return; fi
    [ -f "$tarball" ] || { log "downloading ${name} ${ver}"; curl -fsSL -o "$tarball" "$url"; }
    local actual; actual="$(sha256_of "$tarball")"
    if [ "$actual" != "$sha" ]; then
        mv -f "$tarball" "${tarball}.rejected"
        echo "[macos-slices] CHECKSUM MISMATCH for $(basename "$tarball"): got $actual, want $sha" >&2
        exit 1
    fi
    log "checksum OK: $(basename "$tarball")"
    tar xzf "$tarball" -C "$SOURCES_DIR"
}

SDKPATH="$(xcrun --sdk macosx --show-sdk-path)"
CLANG="$(xcrun --sdk macosx -f clang)"
NCPU="$(sysctl -n hw.ncpu)"

# ---------------------------------------------------------------- libffi ---
build_libffi_arch() {   # arch host
    local arch="$1" host="$2"
    local build_dir="${BUILD_ROOT}/libffi-build/macos-${arch}"
    local install_dir="${BUILD_ROOT}/libffi-install/macos-${arch}"
    if [ -f "${install_dir}/lib/libffi.a" ]; then log "libffi ${arch}: already built"; return; fi
    log "libffi ${arch}: building"
    rm -rf "$build_dir" "$install_dir"; mkdir -p "$build_dir" "$install_dir"
    # --build != --host forces autoconf's cross mode so it never tries to RUN a
    # conftest binary (same trick as build-libffi.sh).
    local build_triple
    case "$host" in x86_64-*) build_triple=aarch64-apple-darwin ;; *) build_triple=x86_64-apple-darwin ;; esac
    ( cd "$build_dir" && \
      CC="$CLANG" \
      CFLAGS="-arch ${arch} -isysroot ${SDKPATH} ${MIN_FLAG} -O2" \
      LDFLAGS="-arch ${arch} -isysroot ${SDKPATH} ${MIN_FLAG}" \
      /bin/sh "${SOURCES_DIR}/libffi-${LIBFFI_VERSION}/configure" \
          --host="$host" --build="$build_triple" --prefix="$install_dir" \
          --enable-static --disable-shared --disable-docs --disable-multi-os-directory \
          > configure.log 2>&1 && \
      make -j"$NCPU" > make.log 2>&1 && make install > install.log 2>&1 ) \
      || { echo "[macos-slices] libffi ${arch} build failed; see ${build_dir}/*.log" >&2; exit 1; }
}

stage_libffi() {
    local out="${FRAMEWORKS}/libffi.xcframework/${SLICE}"
    local a="${BUILD_ROOT}/libffi-install/macos-arm64" b="${BUILD_ROOT}/libffi-install/macos-x86_64"
    rm -rf "$out"; mkdir -p "$out/Headers"
    lipo -create "$a/lib/libffi.a" "$b/lib/libffi.a" -output "$out/libffi.a"
    cp "$a"/include/*.h "$out/Headers/"
    # Per-arch dispatch headers -- see build-libffi.sh make_universal for why.
    for hdr in ffitarget ffi; do
        diff -q "$a/include/${hdr}.h" "$b/include/${hdr}.h" >/dev/null 2>&1 && continue
        cp "$a/include/${hdr}.h" "$out/Headers/${hdr}-macos-arm64.h"
        cp "$b/include/${hdr}.h" "$out/Headers/${hdr}-macos-x86_64.h"
        {
            echo "/* Generated by build-macos-slices.sh: arm64 and x86_64 need"
            echo "   different ${hdr}.h, and a fat slice has only one header dir. */"
            echo "#if defined(__aarch64__) || defined(__arm64__)"
            echo "#include \"${hdr}-macos-arm64.h\""
            echo "#elif defined(__x86_64__)"
            echo "#include \"${hdr}-macos-x86_64.h\""
            echo "#else"
            echo "#error \"libffi: no ${hdr}.h staged for this architecture\""
            echo "#endif"
        } > "$out/Headers/${hdr}.h"
        log "libffi: staged per-arch ${hdr}.h"
    done
    log "libffi slice: $(lipo -archs "$out/libffi.a")  -> $out"
}

# ------------------------------------------------------------------ SDL2 ---
build_sdl2_arch() {   # arch
    local arch="$1"
    local build_dir="${BUILD_ROOT}/sdl2-build/macos-${arch}"
    local install_dir="${BUILD_ROOT}/sdl2-install/macos-${arch}"
    if [ -f "${install_dir}/lib/libSDL2.a" ]; then log "SDL2 ${arch}: already built"; return; fi
    log "SDL2 ${arch}: building"
    rm -rf "$build_dir" "$install_dir"; mkdir -p "$build_dir"
    # Every subsystem off, exactly as build-sdl2.sh does: the VM only needs the
    # headers and the symbol table (src/vm/FFI.cpp stubs the SDL2 entry points
    # the image calls and bridges them to Metal).
    cmake -B "$build_dir" -S "${SOURCES_DIR}/SDL2-${SDL2_VERSION}" \
        -DCMAKE_INSTALL_PREFIX="$install_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_SYSROOT="$SDKPATH" \
        -DCMAKE_C_FLAGS="$MIN_FLAG" -DCMAKE_CXX_FLAGS="$MIN_FLAG" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF \
        -DSDL2_DISABLE_SDL2MAIN=ON \
        -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VIDEO=OFF -DSDL_AUDIO=OFF \
        -DSDL_RENDER=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_POWER=OFF \
        -DSDL_SENSOR=OFF -DSDL_JOYSTICK=OFF -DSDL_MISC=OFF -DSDL_LOCALE=OFF \
        > "$build_dir/configure.log" 2>&1 \
      && cmake --build "$build_dir" -j"$NCPU" > "$build_dir/make.log" 2>&1 \
      && cmake --install "$build_dir" > "$build_dir/install.log" 2>&1 \
      || { echo "[macos-slices] SDL2 ${arch} build failed; see ${build_dir}/*.log" >&2; exit 1; }
}

stage_sdl2() {
    local out="${FRAMEWORKS}/SDL2.xcframework/${SLICE}"
    local a="${BUILD_ROOT}/sdl2-install/macos-arm64" b="${BUILD_ROOT}/sdl2-install/macos-x86_64"
    rm -rf "$out"; mkdir -p "$out/Headers"
    lipo -create "$a/lib/libSDL2.a" "$b/lib/libSDL2.a" -output "$out/libSDL2.a"
    cp "$a"/include/SDL2/*.h "$out/Headers/"
    log "SDL2 slice: $(lipo -archs "$out/libSDL2.a")  -> $out"
}

# ------------------------------------------------------------------ main ---
log "SDK $SDKPATH"
fetch libffi "$LIBFFI_VERSION" "$LIBFFI_URL" "$LIBFFI_SHA256"
fetch SDL2   "$SDL2_VERSION"   "$SDL2_URL"   "$SDL2_SHA256"

build_libffi_arch arm64  aarch64-apple-darwin
build_libffi_arch x86_64 x86_64-apple-darwin
stage_libffi

build_sdl2_arch arm64
build_sdl2_arch x86_64
stage_sdl2

log "done. Configure the VM with:"
log "    cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release"
log "    cmake -S . -B build-x86 -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64"
