#!/bin/bash
# Build PharoVMCore.xcframework for iOS, iOS Simulator, and Mac Catalyst
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PHARO_VM_DIR="../pharo-vm"
BUILD_BASE="$SCRIPT_DIR/build-xcframework"
XCFRAMEWORK_OUTPUT="$SCRIPT_DIR/PharoVMCore.xcframework"

echo "=== Building PharoVMCore.xcframework ==="

# Clean previous builds
rm -rf "$BUILD_BASE"
rm -rf "$XCFRAMEWORK_OUTPUT"
mkdir -p "$BUILD_BASE"

# Build for iOS device (arm64)
echo ""
echo "=== Building for iOS device (arm64) ==="
mkdir -p "$BUILD_BASE/ios-device"
cmake -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DPHARO_VM_DIR="$PHARO_VM_DIR" \
    -B "$BUILD_BASE/ios-device" \
    -S .

xcodebuild -project "$BUILD_BASE/ios-device/iospharo.xcodeproj" \
    -scheme PharoVMCore \
    -configuration Release \
    -sdk iphoneos \
    -arch arm64

IOS_DEVICE_LIB="$BUILD_BASE/ios-device/Release-iphoneos/libPharoVMCore.a"

# Build for iOS Simulator (arm64 + x86_64)
echo ""
echo "=== Building for iOS Simulator (arm64 + x86_64) ==="
mkdir -p "$BUILD_BASE/ios-simulator"
cmake -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DPHARO_VM_DIR="$PHARO_VM_DIR" \
    -B "$BUILD_BASE/ios-simulator" \
    -S .

xcodebuild -project "$BUILD_BASE/ios-simulator/iospharo.xcodeproj" \
    -scheme PharoVMCore \
    -configuration Release \
    -sdk iphonesimulator \
    ARCHS="arm64 x86_64" \
    ONLY_ACTIVE_ARCH=NO

IOS_SIM_LIB="$BUILD_BASE/ios-simulator/Release-iphonesimulator/libPharoVMCore.a"

# Build for Mac Catalyst (arm64 + x86_64)
echo ""
echo "=== Building for Mac Catalyst (arm64 + x86_64) ==="
mkdir -p "$BUILD_BASE/maccatalyst"
cmake -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DPHARO_VM_DIR="$PHARO_VM_DIR" \
    -B "$BUILD_BASE/maccatalyst" \
    -S .

cd "$BUILD_BASE/maccatalyst"

# Build arm64
xcodebuild -project iospharo.xcodeproj \
    -scheme PharoVMCore \
    -configuration Release \
    -destination 'platform=macOS,variant=Mac Catalyst,arch=arm64' \
    SUPPORTS_MACCATALYST=YES \
    ARCHS=arm64 \
    ONLY_ACTIVE_ARCH=NO

mv Release-maccatalyst/libPharoVMCore.a Release-maccatalyst/libPharoVMCore-arm64.a

# Build x86_64
xcodebuild -project iospharo.xcodeproj \
    -scheme PharoVMCore \
    -configuration Release \
    -destination 'platform=macOS,variant=Mac Catalyst,arch=x86_64' \
    SUPPORTS_MACCATALYST=YES \
    ARCHS=x86_64 \
    ONLY_ACTIVE_ARCH=NO

mv Release-maccatalyst/libPharoVMCore.a Release-maccatalyst/libPharoVMCore-x86_64.a

# Create fat binary
lipo -create \
    Release-maccatalyst/libPharoVMCore-arm64.a \
    Release-maccatalyst/libPharoVMCore-x86_64.a \
    -output Release-maccatalyst/libPharoVMCore.a

cd "$SCRIPT_DIR"

CATALYST_LIB="$BUILD_BASE/maccatalyst/Release-maccatalyst/libPharoVMCore.a"

# Create xcframework
echo ""
echo "=== Creating XCFramework ==="
xcodebuild -create-xcframework \
    -library "$IOS_DEVICE_LIB" \
    -library "$IOS_SIM_LIB" \
    -library "$CATALYST_LIB" \
    -output "$XCFRAMEWORK_OUTPUT"

echo ""
echo "=== Done! ==="
echo "XCFramework created at: $XCFRAMEWORK_OUTPUT"
echo ""
echo "Contents:"
ls -la "$XCFRAMEWORK_OUTPUT"
echo ""
echo "Architectures:"
for lib in "$XCFRAMEWORK_OUTPUT"/*/libPharoVMCore.a; do
    echo "  $(dirname "$lib" | xargs basename):"
    lipo -info "$lib" | sed 's/^/    /'
done
