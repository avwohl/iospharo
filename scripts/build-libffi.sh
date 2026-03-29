#!/bin/bash
# build-libffi.sh — Cross-compile libffi for iOS as an xcframework.
#
# Builds static libraries for:
#   - iOS Device (arm64)
#   - iOS Simulator (arm64 + x86_64)
#   - Mac Catalyst (arm64 + x86_64)
#   - macOS (arm64 + x86_64)
#
# Then packages as libffi.xcframework.
#
# Usage:
#   ./scripts/build-libffi.sh
#
# Prerequisites:
#   - Xcode command-line tools
#   - autoconf, automake, libtool (brew install autoconf automake libtool)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_ROOT="${PROJECT_DIR}/third-party-build"
SOURCES_DIR="${BUILD_ROOT}/sources"

LIBFFI_VERSION="3.5.2"
LIBFFI_URL="https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/libffi-${LIBFFI_VERSION}.tar.gz"
LIBFFI_SRC="${SOURCES_DIR}/libffi-${LIBFFI_VERSION}"

OUTPUT="${PROJECT_DIR}/Frameworks/libffi.xcframework"

GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[libffi]${NC} $*"; }

# =====================================================================
# Download (if missing)
# =====================================================================

download() {
    mkdir -p "$SOURCES_DIR"
    if [ ! -d "$LIBFFI_SRC" ]; then
        log "Downloading libffi ${LIBFFI_VERSION}..."
        curl -sL "$LIBFFI_URL" | tar xz -C "$SOURCES_DIR"
    else
        log "Source already exists at $LIBFFI_SRC"
    fi
}

# =====================================================================
# Build for one platform
# =====================================================================

build_slice() {
    local arch="$1"
    local sdk="$2"
    local target_flag="$3"  # e.g. "" or "-target arm64-apple-ios15.0-macabi"
    local min_flag="$4"     # e.g. "-miphoneos-version-min=15.0"
    local host="$5"         # e.g. "aarch64-apple-darwin"
    local label="$6"        # e.g. "ios-device-arm64"

    local build_dir="${BUILD_ROOT}/libffi-build/${label}"
    local install_dir="${BUILD_ROOT}/libffi-install/${label}"

    if [ -f "${install_dir}/lib/libffi.a" ]; then
        log "  ${label}: already built, skipping"
        return
    fi

    log "  Building ${label}..."
    rm -rf "$build_dir" "$install_dir"
    mkdir -p "$build_dir" "$install_dir"

    local sdkpath
    sdkpath=$(xcrun --sdk "$sdk" --show-sdk-path)
    local cc
    cc=$(xcrun --sdk "$sdk" -f clang)

    export CC="$cc"
    export CFLAGS="-arch ${arch} -isysroot ${sdkpath} ${target_flag} ${min_flag} -O2"
    export LDFLAGS="-arch ${arch} -isysroot ${sdkpath} ${target_flag} ${min_flag}"

    cd "$LIBFFI_SRC"

    # libffi uses autoconf — regenerate if needed
    if [ ! -f configure ]; then
        autoreconf -fi
    fi

    cd "$build_dir"

    # Force cross-compilation mode for ALL builds. Autoconf tries to
    # compile and run test programs (conftest) which can hang when the
    # compiled binary can't execute (sandboxed CI, wrong SDK, etc).
    # Using --build different from --host makes autoconf skip run tests.
    local build_triple
    case "$host" in
        x86_64-*)  build_triple="--build=aarch64-apple-darwin" ;;
        *)         build_triple="--build=x86_64-apple-darwin" ;;
    esac

    /bin/sh "$LIBFFI_SRC/configure" \
        --host="$host" \
        $build_triple \
        --prefix="$install_dir" \
        --enable-static \
        --disable-shared \
        --disable-docs \
        --disable-multi-os-directory \
        2>&1 | tail -5

    make -j"$(sysctl -n hw.ncpu)" 2>&1 | tail -3
    make install 2>&1 | tail -3

    unset CC CFLAGS LDFLAGS
    cd "$PROJECT_DIR"
}

# =====================================================================
# Create fat (universal) library from two slices
# =====================================================================

make_universal() {
    local label="$1"
    local slice_a="$2"
    local slice_b="$3"

    local install_dir="${BUILD_ROOT}/libffi-install/${label}"
    local dir_a="${BUILD_ROOT}/libffi-install/${slice_a}"
    local dir_b="${BUILD_ROOT}/libffi-install/${slice_b}"

    mkdir -p "${install_dir}/lib" "${install_dir}/include"
    lipo -create "${dir_a}/lib/libffi.a" "${dir_b}/lib/libffi.a" \
         -output "${install_dir}/lib/libffi.a"
    cp -R "${dir_a}/include/"* "${install_dir}/include/"
    log "  Created universal: ${label}"
}

# =====================================================================
# Main
# =====================================================================

log "=== Building libffi ${LIBFFI_VERSION} xcframework ==="
download

log "Building iOS Device..."
build_slice arm64 iphoneos "" "-miphoneos-version-min=15.0" aarch64-apple-darwin ios-device-arm64

log "Building iOS Simulator..."
build_slice arm64 iphonesimulator "" "-mios-simulator-version-min=15.0" aarch64-apple-darwin ios-sim-arm64
build_slice x86_64 iphonesimulator "" "-mios-simulator-version-min=15.0" x86_64-apple-darwin ios-sim-x86_64
make_universal ios-sim-universal ios-sim-arm64 ios-sim-x86_64

log "Building Mac Catalyst..."
build_slice arm64 macosx "-target arm64-apple-ios15.0-macabi" "" aarch64-apple-darwin catalyst-arm64
build_slice x86_64 macosx "-target x86_64-apple-ios15.0-macabi" "" x86_64-apple-darwin catalyst-x86_64
make_universal catalyst-universal catalyst-arm64 catalyst-x86_64

log "Building macOS..."
build_slice arm64 macosx "" "-mmacosx-version-min=11.0" aarch64-apple-darwin macos-arm64
build_slice x86_64 macosx "" "-mmacosx-version-min=11.0" x86_64-apple-darwin macos-x86_64
make_universal macos-universal macos-arm64 macos-x86_64

# Prepare header directories for each slice
for slice in ios-device-arm64 ios-sim-universal catalyst-universal macos-universal; do
    install_dir="${BUILD_ROOT}/libffi-install/${slice}"
    mkdir -p "${install_dir}/Headers"
    cp "${install_dir}/include/"*.h "${install_dir}/Headers/" 2>/dev/null || true
    # Also check lib/libffi-*/include for ffi.h and ffitarget.h
    find "${install_dir}" -name "ffi.h" -exec cp {} "${install_dir}/Headers/" \; 2>/dev/null || true
    find "${install_dir}" -name "ffitarget.h" -exec cp {} "${install_dir}/Headers/" \; 2>/dev/null || true
done

log "Creating xcframework..."
mkdir -p "$(dirname "$OUTPUT")"
rm -rf "$OUTPUT"

xcodebuild -create-xcframework \
    -library "${BUILD_ROOT}/libffi-install/ios-device-arm64/lib/libffi.a" \
    -headers "${BUILD_ROOT}/libffi-install/ios-device-arm64/Headers" \
    -library "${BUILD_ROOT}/libffi-install/ios-sim-universal/lib/libffi.a" \
    -headers "${BUILD_ROOT}/libffi-install/ios-sim-universal/Headers" \
    -library "${BUILD_ROOT}/libffi-install/catalyst-universal/lib/libffi.a" \
    -headers "${BUILD_ROOT}/libffi-install/catalyst-universal/Headers" \
    -library "${BUILD_ROOT}/libffi-install/macos-universal/lib/libffi.a" \
    -headers "${BUILD_ROOT}/libffi-install/macos-universal/Headers" \
    -output "$OUTPUT"

log "=== Done! ==="
log "Output: $OUTPUT"
echo ""
for dir in "$OUTPUT"/*/; do
    [ -d "$dir" ] && echo "  $(basename "$dir")"
done
