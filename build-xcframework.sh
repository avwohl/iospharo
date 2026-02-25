#!/bin/bash
# Build PharoVMCore.xcframework for iOS Device, Mac Catalyst, and iOS Simulator
#
# Builds arm64 and x86_64 slices for Mac Catalyst and iOS Simulator,
# then combines them with lipo into universal binaries.
#
# Uses cmake with Ninja generator (not Xcode) because cmake -G Xcode spawns
# xcodebuild for compiler identification which hangs in sandboxed environments.
# Cross-compilation is controlled via CFLAGS/sysroot, not CMAKE_SYSTEM_NAME=iOS,
# following the same pattern as scripts/build-third-party.sh.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_BASE="$SCRIPT_DIR/build-xcframework"
XCFRAMEWORK_OUTPUT="$SCRIPT_DIR/PharoVMCore.xcframework"

XCFRAMEWORK_TMP="$SCRIPT_DIR/PharoVMCore-tmp.xcframework"

echo "=== Building PharoVMCore.xcframework (iOS Device + Mac Catalyst + iOS Simulator) ==="

# Clean previous build intermediates (but keep existing xcframework until new one is ready)
rm -rf "$BUILD_BASE"
rm -rf "$XCFRAMEWORK_TMP"
mkdir -p "$BUILD_BASE"

# Helper: configure, build, and find the output library
build_slice() {
    local slice_name="$1"
    local xcfw_platform="$2"
    local sdk="$3"
    local arch="$4"
    local extra_cflags="$5"

    echo ""
    echo "=== Building for $slice_name ($arch) ==="

    local sdkpath
    sdkpath=$(xcrun --sdk "$sdk" --show-sdk-path)
    local cc
    cc=$(xcrun --sdk "$sdk" -f clang)
    local cxx
    cxx=$(xcrun --sdk "$sdk" -f clang++)
    local builddir="$BUILD_BASE/$slice_name"

    local cflags="-arch $arch -isysroot $sdkpath $extra_cflags -O2"

    mkdir -p "$builddir"

    cmake -G Ninja \
        -DCMAKE_C_COMPILER="$cc" \
        -DCMAKE_CXX_COMPILER="$cxx" \
        -DCMAKE_C_FLAGS="$cflags" \
        -DCMAKE_CXX_FLAGS="$cflags" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdkpath" \
        -DFORCE_XCFRAMEWORK_PLATFORM="$xcfw_platform" \
        -B "$builddir" \
        -S .

    cmake --build "$builddir" -- -j$(sysctl -n hw.ncpu)

    local lib="$builddir/libPharoVMCore.a"
    if [ ! -f "$lib" ]; then
        echo "ERROR: $slice_name build failed — library not found at $lib"
        exit 1
    fi
    echo "$slice_name: $(lipo -info "$lib" 2>&1)"
}

# Helper: create a universal (fat) library from two single-arch libraries
make_universal() {
    local output_dir="$1"
    local lib1="$2"
    local lib2="$3"

    mkdir -p "$output_dir"
    lipo -create "$lib1" "$lib2" -output "$output_dir/libPharoVMCore.a"
    echo "Universal: $(lipo -info "$output_dir/libPharoVMCore.a" 2>&1)"
}

# --- Build all slices ---

# iOS Device (arm64 only)
build_slice "iphoneos" "ios-arm64" "iphoneos" "arm64" \
    "-mios-version-min=15.0"

# Mac Catalyst (arm64 + x86_64)
build_slice "maccatalyst-arm64" "ios-arm64_x86_64-maccatalyst" "macosx" "arm64" \
    "-target arm64-apple-ios15.0-macabi"

build_slice "maccatalyst-x86_64" "ios-arm64_x86_64-maccatalyst" "macosx" "x86_64" \
    "-target x86_64-apple-ios15.0-macabi"

# iOS Simulator (arm64 + x86_64)
build_slice "simulator-arm64" "ios-arm64_x86_64-simulator" "iphonesimulator" "arm64" \
    "-mios-simulator-version-min=15.0"

build_slice "simulator-x86_64" "ios-arm64_x86_64-simulator" "iphonesimulator" "x86_64" \
    "-mios-simulator-version-min=15.0"

# --- Create universal binaries with lipo ---
echo ""
echo "=== Creating universal binaries ==="

make_universal "$BUILD_BASE/maccatalyst-universal" \
    "$BUILD_BASE/maccatalyst-arm64/libPharoVMCore.a" \
    "$BUILD_BASE/maccatalyst-x86_64/libPharoVMCore.a"

make_universal "$BUILD_BASE/simulator-universal" \
    "$BUILD_BASE/simulator-arm64/libPharoVMCore.a" \
    "$BUILD_BASE/simulator-x86_64/libPharoVMCore.a"

# --- Create XCFramework ---
echo ""
echo "=== Creating XCFramework ==="
xcodebuild -create-xcframework \
    -library "$BUILD_BASE/iphoneos/libPharoVMCore.a" \
    -library "$BUILD_BASE/maccatalyst-universal/libPharoVMCore.a" \
    -library "$BUILD_BASE/simulator-universal/libPharoVMCore.a" \
    -output "$XCFRAMEWORK_TMP"

# Atomic swap: only replace the old xcframework after the new one is fully built.
# This prevents Xcode from seeing a missing xcframework if the build fails midway.
rm -rf "$XCFRAMEWORK_OUTPUT"
mv "$XCFRAMEWORK_TMP" "$XCFRAMEWORK_OUTPUT"

# Touch Info.plist so Xcode freshness check passes
touch "$XCFRAMEWORK_OUTPUT/Info.plist"

echo ""
echo "=== Done! ==="
echo "XCFramework created at: $XCFRAMEWORK_OUTPUT"
echo "Slices:"
for dir in "$XCFRAMEWORK_OUTPUT"/*/; do
    [ -f "$dir/libPharoVMCore.a" ] && echo "  $(basename "$dir"): $(lipo -info "$dir/libPharoVMCore.a" 2>&1)"
done
