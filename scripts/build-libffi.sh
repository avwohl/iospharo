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
# Pinning the version pins WHICH release we ask for, not WHAT BYTES arrive — a
# release asset can be replaced, and a truncated download is silently valid to
# `tar`.  Recorded 2026-08-10 from the upstream release asset.
LIBFFI_SHA256="f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc"

OUTPUT="${PROJECT_DIR}/Frameworks/libffi.xcframework"

GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[libffi]${NC} $*"; }

# =====================================================================
# Download (if missing)
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
        echo "[libffi] CHECKSUM MISMATCH for $(basename "$file")" >&2
        echo "  expected: $expected" >&2
        echo "  actual  : $actual" >&2
        echo "  kept at : ${file}.rejected" >&2
        echo "  Refusing to build.  If this is a legitimate upstream re-release," >&2
        echo "  verify it independently and update LIBFFI_SHA256." >&2
        exit 1
    fi
    log "checksum OK: $(basename "$file")"
}

download() {
    mkdir -p "$SOURCES_DIR"
    if [ ! -d "$LIBFFI_SRC" ]; then
        local tarball="${SOURCES_DIR}/libffi-${LIBFFI_VERSION}.tar.gz"
        log "Downloading libffi ${LIBFFI_VERSION}..."
        # Download to a file first: a `curl | tar` pipe cannot be verified,
        # because tar has already extracted the bytes by the time we could
        # hash them.
        curl -fsSL -o "$tarball" "$LIBFFI_URL"
        verify_sha256 "$tarball" "$LIBFFI_SHA256"
        tar xzf "$tarball" -C "$SOURCES_DIR"
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
    echo "[libffi] ERROR: no iOS SDK available — cannot build for iOS/Catalyst." >&2
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

log "=== Building libffi ${LIBFFI_VERSION} xcframework ==="
require_ios_sdk
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

# Fail loudly if the packaged xcframework is not well-formed.
# `xcodebuild -create-xcframework` can leave a directory behind without ever
# writing Info.plist.  SDL2.xcframework sat in exactly that state from
# 2026-06-02 to 2026-08-10 -- an orphaned slice directory, no manifest,
# unusable by Xcode -- and because Frameworks/ is gitignored, nothing surfaced
# it.  Producing wreckage and still exiting 0 is the failure mode worth
# closing.
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
        echo "[libffi] ERROR: $name was not created" >&2
        exit 1
    fi
    if [ ! -f "$fw/Info.plist" ]; then
        echo "[libffi] ERROR: $name has no Info.plist — xcodebuild did not finish writing it." >&2
        echo "  An xcframework without a manifest cannot be consumed by Xcode." >&2
        exit 1
    fi

    local count
    count="$(plutil -extract AvailableLibraries raw -o - "$fw/Info.plist" 2>/dev/null)" || count=""
    case "$count" in
        ''|*[!0-9]*)
            echo "[libffi] ERROR: $name Info.plist unreadable, or has no AvailableLibraries array" >&2
            exit 1
            ;;
    esac
    if [ "$count" -ne "$expected" ]; then
        echo "[libffi] ERROR: $name declares $count slice(s), expected $expected" >&2
        exit 1
    fi

    local i=0 ident libpath
    while [ "$i" -lt "$count" ]; do
        ident="$(plutil -extract "AvailableLibraries.$i.LibraryIdentifier" raw -o - "$fw/Info.plist" 2>/dev/null)" || ident=""
        libpath="$(plutil -extract "AvailableLibraries.$i.LibraryPath" raw -o - "$fw/Info.plist" 2>/dev/null)" || libpath=""
        if [ -z "$ident" ] || [ -z "$libpath" ] || [ ! -f "$fw/$ident/$libpath" ]; then
            echo "[libffi] ERROR: $name declared slice '${ident:-?}' is missing ${libpath:-<library>} on disk" >&2
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
    -library "${BUILD_ROOT}/libffi-install/ios-device-arm64/lib/libffi.a" \
    -headers "${BUILD_ROOT}/libffi-install/ios-device-arm64/Headers" \
    -library "${BUILD_ROOT}/libffi-install/ios-sim-universal/lib/libffi.a" \
    -headers "${BUILD_ROOT}/libffi-install/ios-sim-universal/Headers" \
    -library "${BUILD_ROOT}/libffi-install/catalyst-universal/lib/libffi.a" \
    -headers "${BUILD_ROOT}/libffi-install/catalyst-universal/Headers" \
    -library "${BUILD_ROOT}/libffi-install/macos-universal/lib/libffi.a" \
    -headers "${BUILD_ROOT}/libffi-install/macos-universal/Headers" \
    -output "$OUTPUT"

# Four -library arguments above => four slices expected.
validate_xcframework "$OUTPUT" 4

log "=== Done! ==="
log "Output: $OUTPUT"
echo ""
for dir in "$OUTPUT"/*/; do
    [ -d "$dir" ] && echo "  $(basename "$dir")"
done
