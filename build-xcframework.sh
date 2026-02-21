#!/bin/bash
# Build PharoVMCore.xcframework for Mac Catalyst (arm64 only for now)
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_BASE="$SCRIPT_DIR/build-xcframework"
XCFRAMEWORK_OUTPUT="$SCRIPT_DIR/PharoVMCore.xcframework"

echo "=== Building PharoVMCore.xcframework (Mac Catalyst) ==="

# Clean previous builds
rm -rf "$BUILD_BASE"
rm -rf "$XCFRAMEWORK_OUTPUT"
mkdir -p "$BUILD_BASE"

# Build for Mac Catalyst (arm64)
echo ""
echo "=== Building for Mac Catalyst (arm64) ==="
mkdir -p "$BUILD_BASE/maccatalyst"

# Configure for iOS — xcframework platform forced to maccatalyst for correct include paths
cmake -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DFORCE_XCFRAMEWORK_PLATFORM=ios-arm64_x86_64-maccatalyst \
    -B "$BUILD_BASE/maccatalyst" \
    -S .

cd "$BUILD_BASE/maccatalyst"

# Build with Mac Catalyst destination — xcodebuild adds macabi target
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
    echo "ERROR: Build failed — library not found at $CATALYST_LIB"
    echo "Searching for libPharoVMCore.a..."
    find "$BUILD_BASE" -name "libPharoVMCore.a" 2>/dev/null
    exit 1
fi

# Create xcframework with just the catalyst slice
echo ""
echo "=== Creating XCFramework ==="
xcodebuild -create-xcframework \
    -library "$CATALYST_LIB" \
    -output "$XCFRAMEWORK_OUTPUT"

# Touch Info.plist so Xcode freshness check passes
touch "$XCFRAMEWORK_OUTPUT/Info.plist"

echo ""
echo "=== Done! ==="
echo "XCFramework created at: $XCFRAMEWORK_OUTPUT"
lipo -info "$XCFRAMEWORK_OUTPUT"/*/libPharoVMCore.a
