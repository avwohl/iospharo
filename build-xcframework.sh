#!/bin/bash
# Build PharoVMCore.xcframework for iOS Device, Mac Catalyst, and iOS Simulator
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

# --- Mac Catalyst (arm64) ---
echo ""
echo "=== Building for Mac Catalyst (arm64) ==="
mkdir -p "$BUILD_BASE/maccatalyst"

cmake -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DFORCE_XCFRAMEWORK_PLATFORM=ios-arm64_x86_64-maccatalyst \
    -B "$BUILD_BASE/maccatalyst" \
    -S .

cd "$BUILD_BASE/maccatalyst"

xcodebuild -project PharoVM.xcodeproj \
    -scheme PharoVMCore \
    -configuration Release \
    -destination 'platform=macOS,variant=Mac Catalyst,arch=arm64' \
    SUPPORTS_MACCATALYST=YES \
    ARCHS=arm64 \
    ONLY_ACTIVE_ARCH=NO \
    -quiet

cd "$SCRIPT_DIR"

CATALYST_LIB="$BUILD_BASE/maccatalyst/Release-maccatalyst/libPharoVMCore.a"

if [ ! -f "$CATALYST_LIB" ]; then
    echo "ERROR: Mac Catalyst build failed — library not found at $CATALYST_LIB"
    echo "Searching for libPharoVMCore.a..."
    find "$BUILD_BASE/maccatalyst" -name "libPharoVMCore.a" 2>/dev/null
    exit 1
fi

# --- iOS Device (arm64) ---
echo ""
echo "=== Building for iOS Device (arm64) ==="
mkdir -p "$BUILD_BASE/iphoneos"

cmake -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DFORCE_XCFRAMEWORK_PLATFORM=ios-arm64 \
    -B "$BUILD_BASE/iphoneos" \
    -S .

cd "$BUILD_BASE/iphoneos"

xcodebuild -project PharoVM.xcodeproj \
    -scheme PharoVMCore \
    -configuration Release \
    -destination 'generic/platform=iOS' \
    ARCHS=arm64 \
    ONLY_ACTIVE_ARCH=NO \
    -quiet

cd "$SCRIPT_DIR"

DEVICE_LIB="$BUILD_BASE/iphoneos/Release-iphoneos/libPharoVMCore.a"

if [ ! -f "$DEVICE_LIB" ]; then
    echo "ERROR: iOS Device build failed — library not found at $DEVICE_LIB"
    echo "Searching for libPharoVMCore.a..."
    find "$BUILD_BASE/iphoneos" -name "libPharoVMCore.a" 2>/dev/null
    exit 1
fi

# --- iOS Simulator (arm64) ---
echo ""
echo "=== Building for iOS Simulator (arm64) ==="
mkdir -p "$BUILD_BASE/simulator"

cmake -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DFORCE_XCFRAMEWORK_PLATFORM=ios-arm64_x86_64-simulator \
    -B "$BUILD_BASE/simulator" \
    -S .

cd "$BUILD_BASE/simulator"

xcodebuild -project PharoVM.xcodeproj \
    -scheme PharoVMCore \
    -configuration Release \
    -destination 'generic/platform=iOS Simulator' \
    ARCHS=arm64 \
    ONLY_ACTIVE_ARCH=NO \
    -quiet

cd "$SCRIPT_DIR"

SIMULATOR_LIB="$BUILD_BASE/simulator/Release-iphonesimulator/libPharoVMCore.a"

if [ ! -f "$SIMULATOR_LIB" ]; then
    echo "ERROR: iOS Simulator build failed — library not found at $SIMULATOR_LIB"
    echo "Searching for libPharoVMCore.a..."
    find "$BUILD_BASE/simulator" -name "libPharoVMCore.a" 2>/dev/null
    exit 1
fi

# --- Create XCFramework with both slices ---
echo ""
echo "=== Creating XCFramework ==="
xcodebuild -create-xcframework \
    -library "$DEVICE_LIB" \
    -library "$CATALYST_LIB" \
    -library "$SIMULATOR_LIB" \
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
