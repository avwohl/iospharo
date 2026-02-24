#!/bin/bash
# Build PharoVMCore.xcframework for iOS Device, Mac Catalyst, and iOS Simulator
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

echo "=== Building PharoVMCore.xcframework (iOS Device + Mac Catalyst + iOS Simulator) ==="

# Clean previous builds
rm -rf "$BUILD_BASE"
rm -rf "$XCFRAMEWORK_OUTPUT"
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

# --- Build all three slices ---
build_slice "maccatalyst" "ios-arm64_x86_64-maccatalyst" "macosx" "arm64" \
    "-target arm64-apple-ios15.0-macabi"

build_slice "iphoneos" "ios-arm64" "iphoneos" "arm64" \
    "-mios-version-min=15.0"

build_slice "simulator" "ios-arm64_x86_64-simulator" "iphonesimulator" "arm64" \
    "-mios-simulator-version-min=15.0"

# --- Create XCFramework ---
echo ""
echo "=== Creating XCFramework ==="
xcodebuild -create-xcframework \
    -library "$BUILD_BASE/iphoneos/libPharoVMCore.a" \
    -library "$BUILD_BASE/maccatalyst/libPharoVMCore.a" \
    -library "$BUILD_BASE/simulator/libPharoVMCore.a" \
    -output "$XCFRAMEWORK_OUTPUT"

# Touch Info.plist so Xcode freshness check passes
touch "$XCFRAMEWORK_OUTPUT/Info.plist"

echo ""
echo "=== Done! ==="
echo "XCFramework created at: $XCFRAMEWORK_OUTPUT"
echo "Slices:"
for dir in "$XCFRAMEWORK_OUTPUT"/*/; do
    [ -f "$dir/libPharoVMCore.a" ] && echo "  $(basename "$dir"): $(lipo -info "$dir/libPharoVMCore.a" 2>&1)"
done
