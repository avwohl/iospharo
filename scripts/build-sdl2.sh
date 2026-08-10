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
# Pinning the version pins WHICH release we ask for, not WHAT BYTES arrive — a
# release asset can be replaced, and a truncated download is silently valid to
# `tar`.  Recorded 2026-08-10 from the upstream release asset.
SDL2_SHA256="ad8fea3da1be64c83c45b1d363a6b4ba8fd60f5bde3b23ec73855709ec5eabf7"

OUTPUT="${PROJECT_DIR}/Frameworks/SDL2.xcframework"

GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[sdl2]${NC} $*"; }

# =====================================================================
# Download
# =====================================================================

sha256_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256sum "$1" | cut -d' ' -f1
    fi
}

# Refuse to build anything whose bytes we did not pin.  The rejected file is
# kept as <name>.rejected for inspection rather than deleted, and moving it
# aside means the next run re-downloads instead of failing on the same cache.
verify_sha256() {
    local file="$1" expected="$2" actual
    actual="$(sha256_of "$file")"
    if [ "$actual" != "$expected" ]; then
        mv -f "$file" "${file}.rejected" 2>/dev/null || true
        echo "[sdl2] CHECKSUM MISMATCH for $(basename "$file")" >&2
        echo "  expected: $expected" >&2
        echo "  actual  : $actual" >&2
        echo "  kept at : ${file}.rejected" >&2
        echo "  Refusing to build.  If this is a legitimate upstream re-release," >&2
        echo "  verify it independently and update SDL2_SHA256." >&2
        exit 1
    fi
    log "checksum OK: $(basename "$file")"
}

download() {
    mkdir -p "$SOURCES_DIR"
    if [ ! -d "$SDL2_SRC" ]; then
        local tarball="${SOURCES_DIR}/SDL2-${SDL2_VERSION}.tar.gz"
        log "Downloading SDL2 ${SDL2_VERSION}..."
        # Download to a file first: a `curl | tar` pipe cannot be verified,
        # because tar has already extracted the bytes by the time we could
        # hash them.
        curl -fsSL -o "$tarball" "$SDL2_URL"
        verify_sha256 "$tarball" "$SDL2_SHA256"
        tar xzf "$tarball" -C "$SOURCES_DIR"
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
# Preflight: an Xcode with iOS SDKs must be selected
# =====================================================================
# The iOS/Catalyst SDKs ship inside Xcode.app.  When xcode-select points at
# /Library/Developer/CommandLineTools -- the default after a CLT-only install,
# and where some macOS updates leave it -- every `xcrun --sdk iphoneos` below
# fails, and the build dies much later inside a compiler try-run with
# "library 'System' not found", which says nothing about the real cause.
# Detect it up front, and prefer an installed Xcode for this invocation only
# (no sudo, no change to the machine-wide setting).
require_ios_sdk() {
    if xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
        return 0
    fi
    # Read the current selection BEFORE exporting DEVELOPER_DIR below:
    # `xcode-select -p` honours DEVELOPER_DIR, so querying it afterwards would
    # just echo back the value we had only just set.
    local selected
    selected="$(xcode-select -p 2>/dev/null || echo '(unset)')"
    local candidate
    for candidate in "${DEVELOPER_DIR:-}" /Applications/Xcode*.app/Contents/Developer; do
        [ -n "$candidate" ] && [ -d "$candidate" ] || continue
        if DEVELOPER_DIR="$candidate" xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
            export DEVELOPER_DIR="$candidate"
            log "xcode-select points at ${selected} — no iOS SDK there"
            log "using DEVELOPER_DIR=$DEVELOPER_DIR for this build"
            return 0
        fi
    done
    echo "[sdl2] ERROR: no iOS SDK available — cannot build for iOS/Catalyst." >&2
    echo "  xcode-select -p: ${selected}" >&2
    echo "  The iOS SDKs ship inside Xcode.app, not the Command Line Tools." >&2
    echo "  Fix with either:" >&2
    echo "    sudo xcode-select -s /Applications/Xcode.app/Contents/Developer   # machine-wide" >&2
    echo "    DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer $0       # this run only" >&2
    exit 1
}

# =====================================================================
# Main
# =====================================================================

log "=== Building SDL2 ${SDL2_VERSION} xcframework ==="
require_ios_sdk
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

# Fail loudly if the packaged xcframework is not well-formed.
# `xcodebuild -create-xcframework` can leave a directory behind without ever
# writing Info.plist.  That is exactly the state this xcframework sat in from
# 2026-06-02 to 2026-08-10: an orphaned macos-arm64_x86_64 directory, no
# manifest, unusable by Xcode -- and because Frameworks/ is gitignored, nothing
# ever surfaced it.  Producing wreckage and still exiting 0 is the failure mode
# worth closing.
#
# Validation is driven by the manifest rather than hardcoded slice names, so it
# keeps working if the platform set changes: every slice the plist declares
# must exist on disk with its library, and the count must match what we asked
# xcodebuild to package.
validate_xcframework() {
    local fw="$1" expected="$2"
    local name
    name="$(basename "$fw")"

    if [ ! -d "$fw" ]; then
        echo "[sdl2] ERROR: $name was not created" >&2
        exit 1
    fi
    if [ ! -f "$fw/Info.plist" ]; then
        echo "[sdl2] ERROR: $name has no Info.plist — xcodebuild did not finish writing it." >&2
        echo "  An xcframework without a manifest cannot be consumed by Xcode." >&2
        exit 1
    fi

    local count
    count="$(plutil -extract AvailableLibraries raw -o - "$fw/Info.plist" 2>/dev/null)" || count=""
    case "$count" in
        ''|*[!0-9]*)
            echo "[sdl2] ERROR: $name Info.plist unreadable, or has no AvailableLibraries array" >&2
            exit 1
            ;;
    esac
    if [ "$count" -ne "$expected" ]; then
        echo "[sdl2] ERROR: $name declares $count slice(s), expected $expected" >&2
        exit 1
    fi

    local i=0 ident libpath
    while [ "$i" -lt "$count" ]; do
        ident="$(plutil -extract "AvailableLibraries.$i.LibraryIdentifier" raw -o - "$fw/Info.plist" 2>/dev/null)" || ident=""
        libpath="$(plutil -extract "AvailableLibraries.$i.LibraryPath" raw -o - "$fw/Info.plist" 2>/dev/null)" || libpath=""
        if [ -z "$ident" ] || [ -z "$libpath" ] || [ ! -f "$fw/$ident/$libpath" ]; then
            echo "[sdl2] ERROR: $name declared slice '${ident:-?}' is missing ${libpath:-<library>} on disk" >&2
            exit 1
        fi
        i=$((i + 1))
    done
    log "validated $name ($count slices)"
}

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

# Four -library arguments above => four slices expected.
validate_xcframework "$OUTPUT" 4

log "=== Done! ==="
log "Output: $OUTPUT"
echo ""
for dir in "$OUTPUT"/*/; do
    [ -d "$dir" ] && echo "  $(basename "$dir")"
done
