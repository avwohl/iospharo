#!/bin/bash
# build-sdl2.sh — Cross-compile SDL2 for iOS as an xcframework.
#
# Builds static libraries for:
#   - iOS Device (arm64)
#   - iOS Simulator (arm64 + x86_64)
#   - Mac Catalyst (arm64 + x86_64)
#   - macOS (arm64 + x86_64)
#
# Then packages as SDL2.xcframework.
#
# Usage:
#   ./scripts/build-sdl2.sh
#
# Prerequisites:
#   - Xcode command-line tools
#   - CMake (brew install cmake)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_ROOT="${PROJECT_DIR}/third-party-build"
SOURCES_DIR="${BUILD_ROOT}/sources"

SDL2_VERSION="2.26.5"
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz"
SDL2_SRC="${SOURCES_DIR}/SDL2-${SDL2_VERSION}"

OUTPUT="${PROJECT_DIR}/Frameworks/SDL2.xcframework"

GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[sdl2]${NC} $*"; }

# =====================================================================
# Download
# =====================================================================

download() {
    mkdir -p "$SOURCES_DIR"
    if [ ! -d "$SDL2_SRC" ]; then
        log "Downloading SDL2 ${SDL2_VERSION}..."
        curl -sL "$SDL2_URL" | tar xz -C "$SOURCES_DIR"
    else
        log "Source already exists at $SDL2_SRC"
    fi
}

# =====================================================================
# Build for one platform using CMake
# =====================================================================

build_slice() {
    local arch="$1"
    local sdk="$2"
    local system_name="$3"   # iOS or Darwin
    local target_flag="$4"   # e.g. "-target arm64-apple-ios15.0-macabi"
    local min_flag="$5"      # e.g. "-miphoneos-version-min=15.0"
    local label="$6"

    local build_dir="${BUILD_ROOT}/sdl2-build/${label}"
    local install_dir="${BUILD_ROOT}/sdl2-install/${label}"

    if [ -f "${install_dir}/lib/libSDL2.a" ]; then
        log "  ${label}: already built, skipping"
        return
    fi

    log "  Building ${label}..."
    rm -rf "$build_dir" "$install_dir"
    mkdir -p "$build_dir"

    local sdkpath
    sdkpath=$(xcrun --sdk "$sdk" --show-sdk-path)

    local extra_c_flags="${target_flag} ${min_flag}"

    local cmake_args=(
        -DCMAKE_INSTALL_PREFIX="$install_dir"
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DCMAKE_OSX_ARCHITECTURES="$arch"
        -DCMAKE_OSX_SYSROOT="$sdkpath"
        -DCMAKE_C_FLAGS="$extra_c_flags"
        -DCMAKE_CXX_FLAGS="$extra_c_flags"
        -DSDL_SHARED=OFF
        -DSDL_STATIC=ON
        -DSDL_TEST=OFF
        -DSDL_TESTS=OFF
        -DSDL2_DISABLE_SDL2MAIN=ON
        # Disable all SDL2 subsystems — we only need the headers and symbol table.
        # The VM (src/vm/FFI.cpp) provides stub implementations for all SDL2 functions
        # that Pharo calls via FFI, bridging them to the Metal rendering pipeline.
        -DSDL_OPENGL=OFF
        -DSDL_OPENGLES=OFF
        -DSDL_VIDEO=OFF
        -DSDL_AUDIO=OFF
        -DSDL_RENDER=OFF
        -DSDL_HAPTIC=OFF
        -DSDL_HIDAPI=OFF
        -DSDL_POWER=OFF
        -DSDL_SENSOR=OFF
        -DSDL_JOYSTICK=OFF
        -DSDL_MISC=OFF
        -DSDL_LOCALE=OFF
    )

    if [ "$system_name" = "iOS" ]; then
        cmake_args+=(-DCMAKE_SYSTEM_NAME=iOS)
    fi

    cmake -B "$build_dir" -S "$SDL2_SRC" "${cmake_args[@]}" 2>&1 | tail -5
    cmake --build "$build_dir" -j"$(sysctl -n hw.ncpu)" 2>&1 | tail -5
    cmake --install "$build_dir" 2>&1 | tail -3

    cd "$PROJECT_DIR"
}

# =====================================================================
# Create fat (universal) library from two slices
# =====================================================================

make_universal() {
    local label="$1"
    local slice_a="$2"
    local slice_b="$3"

    local install_dir="${BUILD_ROOT}/sdl2-install/${label}"
    local dir_a="${BUILD_ROOT}/sdl2-install/${slice_a}"
    local dir_b="${BUILD_ROOT}/sdl2-install/${slice_b}"

    mkdir -p "${install_dir}/lib" "${install_dir}/include"
    lipo -create "${dir_a}/lib/libSDL2.a" "${dir_b}/lib/libSDL2.a" \
         -output "${install_dir}/lib/libSDL2.a"
    cp -R "${dir_a}/include/"* "${install_dir}/include/"
    log "  Created universal: ${label}"
}

# =====================================================================
# Main
# =====================================================================

log "=== Building SDL2 ${SDL2_VERSION} xcframework ==="
download

log "Building iOS Device..."
build_slice arm64 iphoneos iOS "" "-miphoneos-version-min=15.0" ios-device-arm64

log "Building iOS Simulator..."
build_slice arm64 iphonesimulator iOS "" "-mios-simulator-version-min=15.0" ios-sim-arm64
build_slice x86_64 iphonesimulator iOS "" "-mios-simulator-version-min=15.0" ios-sim-x86_64
make_universal ios-sim-universal ios-sim-arm64 ios-sim-x86_64

log "Building Mac Catalyst..."
build_slice arm64 macosx Catalyst "-target arm64-apple-ios15.0-macabi" "" catalyst-arm64
build_slice x86_64 macosx Catalyst "-target x86_64-apple-ios15.0-macabi" "" catalyst-x86_64
make_universal catalyst-universal catalyst-arm64 catalyst-x86_64

log "Building macOS..."
build_slice arm64 macosx Darwin "" "-mmacosx-version-min=11.0" macos-arm64
build_slice x86_64 macosx Darwin "" "-mmacosx-version-min=11.0" macos-x86_64
make_universal macos-universal macos-arm64 macos-x86_64

# Prepare header directories — SDL2 installs to include/SDL2/
for slice in ios-device-arm64 ios-sim-universal catalyst-universal macos-universal; do
    install_dir="${BUILD_ROOT}/sdl2-install/${slice}"
    headers_dir="${install_dir}/Headers"
    mkdir -p "$headers_dir"
    # SDL2 headers are in include/SDL2/
    if [ -d "${install_dir}/include/SDL2" ]; then
        cp "${install_dir}/include/SDL2/"*.h "$headers_dir/"
    elif [ -d "${install_dir}/include" ]; then
        cp "${install_dir}/include/"*.h "$headers_dir/" 2>/dev/null || true
    fi
done

log "Creating xcframework..."
mkdir -p "$(dirname "$OUTPUT")"
rm -rf "$OUTPUT"

xcodebuild -create-xcframework \
    -library "${BUILD_ROOT}/sdl2-install/ios-device-arm64/lib/libSDL2.a" \
    -headers "${BUILD_ROOT}/sdl2-install/ios-device-arm64/Headers" \
    -library "${BUILD_ROOT}/sdl2-install/ios-sim-universal/lib/libSDL2.a" \
    -headers "${BUILD_ROOT}/sdl2-install/ios-sim-universal/Headers" \
    -library "${BUILD_ROOT}/sdl2-install/catalyst-universal/lib/libSDL2.a" \
    -headers "${BUILD_ROOT}/sdl2-install/catalyst-universal/Headers" \
    -library "${BUILD_ROOT}/sdl2-install/macos-universal/lib/libSDL2.a" \
    -headers "${BUILD_ROOT}/sdl2-install/macos-universal/Headers" \
    -output "$OUTPUT"

log "=== Done! ==="
log "Output: $OUTPUT"
echo ""
for dir in "$OUTPUT"/*/; do
    [ -d "$dir" ] && echo "  $(basename "$dir")"
done
