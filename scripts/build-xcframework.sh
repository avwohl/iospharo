#!/bin/bash
# Build PharoVMCore.xcframework for iOS Device, Mac Catalyst, and iOS Simulator
#
# Builds arm64 and x86_64 slices for Mac Catalyst and iOS Simulator,
# then combines them with lipo into universal binaries.
#
# Uses cmake with Ninja generator (not Xcode) because cmake -G Xcode spawns
# xcodebuild for compiler identification which hangs in sandboxed environments.
# Cross-compilation uses CMAKE_SYSTEM_NAME=iOS with FORCE_XCFRAMEWORK_PLATFORM
# to select the correct xcframework slice for each platform's dependencies.
set -e

# Ensure Homebrew tools (cmake, ninja) are in PATH when run from Xcode build phases
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

BUILD_BASE="$PROJECT_DIR/build-xcframework"
XCFRAMEWORK_OUTPUT="$PROJECT_DIR/Frameworks/PharoVMCore.xcframework"

XCFRAMEWORK_TMP="$PROJECT_DIR/Frameworks/PharoVMCore-tmp.xcframework"

echo "=== Building PharoVMCore.xcframework (iOS Device + Mac Catalyst + iOS Simulator) ==="

# Workaround: vendored asmjit's virtmem.cpp guards
# <libkern/OSCacheControl.h> behind TARGET_OS_OSX but then calls
# sys_icache_invalidate() unconditionally on __APPLE__.  Mac Catalyst
# (TARGET_OS_OSX=0) fails with "undeclared identifier" at the call site.
# Patch in place (idempotent: skips if already applied).  Upstream PR
# should fix this properly in asmjit.
ASMJIT_VM="$PROJECT_DIR/third_party/asmjit/asmjit/core/virtmem.cpp"
if [ -f "$ASMJIT_VM" ]; then
    if ! grep -q 'sys_icache_invalidate() is available on all Apple' "$ASMJIT_VM"; then
        echo "=== Patching asmjit virtmem.cpp for Mac Catalyst ==="
        # Move the OSCacheControl include outside the TARGET_OS_OSX guard.
        # Use a python inline for precise edits.
        python3 - "$ASMJIT_VM" <<'PY'
import sys, re
path = sys.argv[1]
with open(path) as f: src = f.read()
old = '''    #if TARGET_OS_OSX
      #include <sys/utsname.h>
      #include <libkern/OSCacheControl.h> // sys_icache_invalidate().
    #endif'''
new = '''    #if TARGET_OS_OSX
      #include <sys/utsname.h>
    #endif
    // sys_icache_invalidate() is available on all Apple platforms
    // (iOS, Mac Catalyst, macOS).  Without this include, Catalyst
    // builds fail with "undeclared identifier" at the call site.
    #include <libkern/OSCacheControl.h>'''
if old in src:
    src = src.replace(old, new)
    with open(path, 'w') as f: f.write(src)
    print("Patched")
else:
    print("Pattern not found — may need manual update for newer asmjit")
PY
    fi
fi

# Clean previous build intermediates (but keep existing xcframework until new one is ready)
rm -rf "$BUILD_BASE"
rm -rf "$XCFRAMEWORK_TMP"
mkdir -p "$BUILD_BASE"
mkdir -p "$PROJECT_DIR/Frameworks"

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

    # LTO must be OFF: xcodebuild -create-xcframework rejects archives
    # containing LLVM bitcode objects produced by -flto=thin.
    cmake -G Ninja \
        -DCMAKE_C_COMPILER="$cc" \
        -DCMAKE_CXX_COMPILER="$cxx" \
        -DCMAKE_OBJC_COMPILER="$cc" \
        -DCMAKE_ASM_COMPILER="$cc" \
        -DCMAKE_C_FLAGS="$cflags" \
        -DCMAKE_CXX_FLAGS="$cflags" \
        -DCMAKE_OBJC_FLAGS="$cflags" \
        -DCMAKE_ASM_FLAGS="$cflags" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPHARO_DISABLE_LTO=ON \
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

    # Merge all third-party static libraries into PharoVMCore.a so that
    # all symbols (cairo, freetype, libgit2, etc.) are available via dlsym
    # at runtime. Without this, the Pharo image can't find them via FFI.
    local third_party_libs=()
    local thindir="$builddir/thin-libs"
    mkdir -p "$thindir"
    # Note: SDL2 is NOT merged — FFI.cpp intercepts all SDL_ calls with stubs.
    # libffi is NOT merged — it's linked separately in the Xcode project via xcframework.
    for xcf_entry in \
        "cairo:libcairo.a" \
        "freetype:libfreetype.a" \
        "pixman:libpixman-1.a" \
        "harfbuzz:libharfbuzz.a" \
        "libpng16:libpng16.a" \
        "libgit2:libgit2.a" \
        "libssl:libssl.a" \
        "libcrypto:libcrypto.a" \
        "libssh2:libssh2.a"; do
        local xcf_name="${xcf_entry%%:*}"
        local lib_file="${xcf_entry##*:}"
        local lib_path="$PROJECT_DIR/Frameworks/${xcf_name}.xcframework/${xcfw_platform}/${lib_file}"
        if [ -f "$lib_path" ]; then
            # Extract single-arch slice from potentially fat libraries
            local thin_path="$thindir/${xcf_name}-${lib_file}"
            if lipo -info "$lib_path" 2>&1 | grep -q "Non-fat"; then
                cp "$lib_path" "$thin_path"
            else
                lipo -thin "$arch" "$lib_path" -output "$thin_path" 2>/dev/null || cp "$lib_path" "$thin_path"
            fi
            third_party_libs+=("$thin_path")
        fi
    done

    if [ ${#third_party_libs[@]} -gt 0 ]; then
        echo "  Merging ${#third_party_libs[@]} third-party libraries into PharoVMCore.a"
        mv "$lib" "$builddir/libPharoVMCore-vm-only.a"
        libtool -static -o "$lib" "$builddir/libPharoVMCore-vm-only.a" "${third_party_libs[@]}"
        rm "$builddir/libPharoVMCore-vm-only.a"
    fi
    rm -rf "$thindir"

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
