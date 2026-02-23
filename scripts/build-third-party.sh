#!/bin/bash
# build-third-party.sh — Cross-compile third-party libraries for iOS as xcframeworks.
#
# Builds static libraries for:
#   - iOS Simulator (arm64 + x86_64)
#   - Mac Catalyst (arm64 + x86_64)
#   - iOS Device (arm64)  [future]
#
# Then packages each as an .xcframework.
#
# Usage:
#   ./scripts/build-third-party.sh [--no-crypto]
#
# Prerequisites:
#   - Xcode command-line tools
#   - Source tarballs downloaded (script will download if missing)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_ROOT="${PROJECT_DIR}/third-party-build"
INSTALL_ROOT="${BUILD_ROOT}/install"
SOURCES_DIR="${BUILD_ROOT}/sources"

# Parse arguments
WITH_CRYPTO=1
for arg in "$@"; do
    case "$arg" in
        --no-crypto) WITH_CRYPTO=0 ;;
    esac
done

# Library versions
LIBPNG_VERSION="1.6.43"
FREETYPE_VERSION="2.13.3"
PIXMAN_VERSION="0.43.4"
HARFBUZZ_VERSION="10.1.0"
CAIRO_VERSION="1.18.2"
OPENSSL_VERSION="3.4.0"
LIBSSH2_VERSION="1.11.1"
LIBGIT2_VERSION="1.8.4"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[build]${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }
err() { echo -e "${RED}[error]${NC} $*" >&2; }

# =====================================================================
# Platform configuration
# =====================================================================

# We build for two platforms initially:
#   1. iOS Simulator: arm64 + x86_64
#   2. Mac Catalyst: arm64 + x86_64
# iOS Device (arm64) can be added later.

setup_ios_simulator_arm64() {
    export SDKROOT=$(xcrun --sdk iphonesimulator --show-sdk-path)
    export CC=$(xcrun --sdk iphonesimulator -f clang)
    export CXX=$(xcrun --sdk iphonesimulator -f clang++)
    export AR=$(xcrun --sdk iphonesimulator -f ar)
    export RANLIB=$(xcrun --sdk iphonesimulator -f ranlib)
    export CFLAGS="-arch arm64 -isysroot $SDKROOT -mios-simulator-version-min=15.0 -O2"
    export CXXFLAGS="$CFLAGS"
    export LDFLAGS="-arch arm64 -isysroot $SDKROOT -mios-simulator-version-min=15.0"
    PLATFORM_NAME="ios-simulator-arm64"
    # Autotools config.sub doesn't understand iOS triples.
    # Use aarch64-apple-darwin and rely on CFLAGS for actual target.
    HOST_TRIPLE="aarch64-apple-darwin"
    # For CMake cross-compilation
    CMAKE_SYSTEM_NAME="iOS"
    CMAKE_OSX_SYSROOT="iphonesimulator"
    MESON_CPU="aarch64"
}

setup_ios_simulator_x86_64() {
    export SDKROOT=$(xcrun --sdk iphonesimulator --show-sdk-path)
    export CC=$(xcrun --sdk iphonesimulator -f clang)
    export CXX=$(xcrun --sdk iphonesimulator -f clang++)
    export AR=$(xcrun --sdk iphonesimulator -f ar)
    export RANLIB=$(xcrun --sdk iphonesimulator -f ranlib)
    export CFLAGS="-arch x86_64 -isysroot $SDKROOT -mios-simulator-version-min=15.0 -O2"
    export CXXFLAGS="$CFLAGS"
    export LDFLAGS="-arch x86_64 -isysroot $SDKROOT -mios-simulator-version-min=15.0"
    PLATFORM_NAME="ios-simulator-x86_64"
    HOST_TRIPLE="x86_64-apple-darwin"
    CMAKE_SYSTEM_NAME="iOS"
    CMAKE_OSX_SYSROOT="iphonesimulator"
    MESON_CPU="x86_64"
}

setup_maccatalyst_arm64() {
    export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
    export CC=$(xcrun --sdk macosx -f clang)
    export CXX=$(xcrun --sdk macosx -f clang++)
    export AR=$(xcrun --sdk macosx -f ar)
    export RANLIB=$(xcrun --sdk macosx -f ranlib)
    export CFLAGS="-arch arm64 -isysroot $SDKROOT -target arm64-apple-ios15.0-macabi -O2"
    export CXXFLAGS="$CFLAGS"
    export LDFLAGS="-arch arm64 -isysroot $SDKROOT -target arm64-apple-ios15.0-macabi"
    PLATFORM_NAME="maccatalyst-arm64"
    HOST_TRIPLE="aarch64-apple-darwin"
    CMAKE_SYSTEM_NAME="Darwin"
    CMAKE_OSX_SYSROOT="macosx"
    MESON_CPU="aarch64"
}

setup_maccatalyst_x86_64() {
    export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
    export CC=$(xcrun --sdk macosx -f clang)
    export CXX=$(xcrun --sdk macosx -f clang++)
    export AR=$(xcrun --sdk macosx -f ar)
    export RANLIB=$(xcrun --sdk macosx -f ranlib)
    export CFLAGS="-arch x86_64 -isysroot $SDKROOT -target x86_64-apple-ios15.0-macabi -O2"
    export CXXFLAGS="$CFLAGS"
    export LDFLAGS="-arch x86_64 -isysroot $SDKROOT -target x86_64-apple-ios15.0-macabi"
    PLATFORM_NAME="maccatalyst-x86_64"
    HOST_TRIPLE="x86_64-apple-darwin"
    CMAKE_SYSTEM_NAME="Darwin"
    CMAKE_OSX_SYSROOT="macosx"
    MESON_CPU="x86_64"
}

# =====================================================================
# Download sources
# =====================================================================
download_sources() {
    mkdir -p "$SOURCES_DIR"
    cd "$SOURCES_DIR"

    local downloads=(
        "libpng-${LIBPNG_VERSION}.tar.xz:https://download.sourceforge.net/libpng/libpng-${LIBPNG_VERSION}.tar.xz"
        "freetype-${FREETYPE_VERSION}.tar.xz:https://download.savannah.gnu.org/releases/freetype/freetype-${FREETYPE_VERSION}.tar.xz"
        "pixman-${PIXMAN_VERSION}.tar.gz:https://cairographics.org/releases/pixman-${PIXMAN_VERSION}.tar.gz"
        "harfbuzz-${HARFBUZZ_VERSION}.tar.xz:https://github.com/harfbuzz/harfbuzz/releases/download/${HARFBUZZ_VERSION}/harfbuzz-${HARFBUZZ_VERSION}.tar.xz"
        "cairo-${CAIRO_VERSION}.tar.xz:https://cairographics.org/releases/cairo-${CAIRO_VERSION}.tar.xz"
    )

    if [ "$WITH_CRYPTO" = "1" ]; then
        downloads+=(
            "openssl-${OPENSSL_VERSION}.tar.gz:https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
            "libssh2-${LIBSSH2_VERSION}.tar.xz:https://www.libssh2.org/download/libssh2-${LIBSSH2_VERSION}.tar.xz"
            "libgit2-${LIBGIT2_VERSION}.tar.gz:https://github.com/libgit2/libgit2/archive/refs/tags/v${LIBGIT2_VERSION}.tar.gz"
        )
    fi

    for entry in "${downloads[@]}"; do
        local filename="${entry%%:*}"
        local url="${entry#*:}"
        if [ ! -f "$filename" ]; then
            log "Downloading $filename..."
            curl -L -o "$filename" "$url"
        fi
    done
}

# =====================================================================
# Build functions for each library
# =====================================================================

# Helper: build an autotools library for the current platform
build_autotools() {
    local name="$1"
    local srcdir="$2"
    shift 2
    local configure_args=("$@")

    local builddir="${BUILD_ROOT}/build-${name}-${PLATFORM_NAME}"
    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"

    rm -rf "$builddir"
    mkdir -p "$builddir"
    cd "$builddir"

    log "Configuring $name for $PLATFORM_NAME..."
    "$srcdir/configure" \
        --host="$HOST_TRIPLE" \
        --prefix="$prefix" \
        --enable-static \
        --disable-shared \
        "${configure_args[@]}" \
        > configure.log 2>&1 || { err "Configure failed for $name"; cat configure.log; return 1; }

    log "Building $name for $PLATFORM_NAME..."
    make -j$(sysctl -n hw.ncpu) > build.log 2>&1 || { err "Build failed for $name"; tail -20 build.log; return 1; }
    make install > install.log 2>&1

    log "$name built for $PLATFORM_NAME"
}

# Helper: build a CMake library for the current platform
build_cmake() {
    local name="$1"
    local srcdir="$2"
    shift 2
    local cmake_args=("$@")

    local builddir="${BUILD_ROOT}/build-${name}-${PLATFORM_NAME}"
    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"

    rm -rf "$builddir"
    mkdir -p "$builddir"
    cd "$builddir"

    log "Configuring $name (CMake) for $PLATFORM_NAME..."
    cmake "$srcdir" \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_C_FLAGS="$CFLAGS" \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
        -DCMAKE_SYSTEM_NAME="$CMAKE_SYSTEM_NAME" \
        -DCMAKE_OSX_SYSROOT="$CMAKE_OSX_SYSROOT" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        "${cmake_args[@]}" \
        > configure.log 2>&1 || { err "CMake configure failed for $name"; cat configure.log; return 1; }

    log "Building $name for $PLATFORM_NAME..."
    cmake --build . -j$(sysctl -n hw.ncpu) > build.log 2>&1 || { err "Build failed for $name"; tail -20 build.log; return 1; }
    cmake --install . > install.log 2>&1

    log "$name built for $PLATFORM_NAME"
}

build_libpng() {
    local srcdir="${SOURCES_DIR}/libpng-${LIBPNG_VERSION}"
    [ -d "$srcdir" ] || (cd "$SOURCES_DIR" && tar xf "libpng-${LIBPNG_VERSION}.tar.xz")

    build_cmake "libpng" "$srcdir" \
        -DPNG_TESTS=OFF \
        -DPNG_TOOLS=OFF \
        -DPNG_FRAMEWORK=OFF \
        -DPNG_SHARED=OFF \
        -DPNG_ARM_NEON=off
}

build_freetype() {
    local srcdir="${SOURCES_DIR}/freetype-${FREETYPE_VERSION}"
    [ -d "$srcdir" ] || (cd "$SOURCES_DIR" && tar xf "freetype-${FREETYPE_VERSION}.tar.xz")

    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"
    local builddir="${BUILD_ROOT}/build-freetype-${PLATFORM_NAME}"
    rm -rf "$builddir"
    mkdir -p "$builddir"
    cd "$builddir"

    # FreeType uses CMake which handles cross-compilation better than autotools.
    # The autotools build tries to run host tools (apinames) built for the target.
    log "Configuring freetype (CMake) for $PLATFORM_NAME..."
    cmake "$srcdir" \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_C_FLAGS="$CFLAGS" \
        -DCMAKE_SYSTEM_NAME="$CMAKE_SYSTEM_NAME" \
        -DCMAKE_OSX_SYSROOT="$CMAKE_OSX_SYSROOT" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DFT_DISABLE_HARFBUZZ=ON \
        -DFT_DISABLE_BZIP2=ON \
        -DFT_DISABLE_BROTLI=ON \
        -DFT_REQUIRE_PNG=ON \
        -DPNG_PNG_INCLUDE_DIR="${prefix}/include" \
        -DPNG_LIBRARY="${prefix}/lib/libpng16.a" \
        -DZLIB_LIBRARY="" \
        > configure.log 2>&1 || { err "CMake configure failed for freetype"; cat configure.log; return 1; }

    log "Building freetype for $PLATFORM_NAME..."
    cmake --build . -j$(sysctl -n hw.ncpu) > build.log 2>&1 || { err "Build failed for freetype"; tail -20 build.log; return 1; }
    cmake --install . > install.log 2>&1

    log "freetype built for $PLATFORM_NAME"
}

build_pixman() {
    local srcdir="${SOURCES_DIR}/pixman-${PIXMAN_VERSION}"
    [ -d "$srcdir" ] || (cd "$SOURCES_DIR" && tar xf "pixman-${PIXMAN_VERSION}.tar.gz")

    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"
    local builddir="${BUILD_ROOT}/build-pixman-${PLATFORM_NAME}"
    rm -rf "$builddir"
    mkdir -p "$builddir"

    # Create meson cross file (same pattern as cairo)
    local crossfile="${builddir}/cross.txt"
    local meson_c_args=""
    for flag in $CFLAGS; do
        [ -n "$meson_c_args" ] && meson_c_args="$meson_c_args, "
        meson_c_args="$meson_c_args'$flag'"
    done
    local meson_link_args=""
    for flag in $LDFLAGS; do
        [ -n "$meson_link_args" ] && meson_link_args="$meson_link_args, "
        meson_link_args="$meson_link_args'$flag'"
    done

    cat > "$crossfile" <<CROSSEOF
[binaries]
c = '$CC'
ar = '$AR'
ranlib = '$RANLIB'

[built-in options]
c_args = [$meson_c_args]
c_link_args = [$meson_link_args]

[host_machine]
system = 'darwin'
cpu_family = '$MESON_CPU'
cpu = '$MESON_CPU'
endian = 'little'
CROSSEOF

    cd "$builddir"
    log "Configuring pixman (meson) for $PLATFORM_NAME..."
    meson setup builddir "$srcdir" \
        --cross-file "$crossfile" \
        --prefix="$prefix" \
        --default-library=static \
        -Dgtk=disabled \
        -Dlibpng=disabled \
        -Dtests=disabled \
        -Ddemos=disabled \
        > configure.log 2>&1 || { err "Meson configure failed for pixman"; cat configure.log; return 1; }

    log "Building pixman for $PLATFORM_NAME..."
    ninja -C builddir -j$(sysctl -n hw.ncpu) > build.log 2>&1 || { err "Build failed for pixman"; tail -20 build.log; return 1; }
    ninja -C builddir install > install.log 2>&1

    log "pixman built for $PLATFORM_NAME"
}

build_harfbuzz() {
    local srcdir="${SOURCES_DIR}/harfbuzz-${HARFBUZZ_VERSION}"
    [ -d "$srcdir" ] || (cd "$SOURCES_DIR" && tar xf "harfbuzz-${HARFBUZZ_VERSION}.tar.xz")

    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"
    local builddir="${BUILD_ROOT}/build-harfbuzz-${PLATFORM_NAME}"
    rm -rf "$builddir"
    mkdir -p "$builddir"
    cd "$builddir"

    # Harfbuzz uses meson/CMake. Use CMake for cross-compilation.
    log "Configuring harfbuzz (CMake) for $PLATFORM_NAME..."
    cmake "$srcdir" \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_C_FLAGS="$CFLAGS" \
        -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
        -DCMAKE_SYSTEM_NAME="$CMAKE_SYSTEM_NAME" \
        -DCMAKE_OSX_SYSROOT="$CMAKE_OSX_SYSROOT" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DHB_HAVE_FREETYPE=ON \
        -DHB_HAVE_GLIB=OFF \
        -DHB_HAVE_GOBJECT=OFF \
        -DHB_HAVE_GRAPHITE2=OFF \
        -DHB_HAVE_ICU=OFF \
        -DHB_HAVE_CORETEXT=ON \
        -DFREETYPE_INCLUDE_DIRS="${prefix}/include/freetype2" \
        -DFREETYPE_LIBRARY="${prefix}/lib/libfreetype.a" \
        > configure.log 2>&1 || { err "CMake configure failed for harfbuzz"; cat configure.log; return 1; }

    log "Building harfbuzz for $PLATFORM_NAME..."
    cmake --build . -j$(sysctl -n hw.ncpu) > build.log 2>&1 || { err "Build failed for harfbuzz"; tail -20 build.log; return 1; }
    cmake --install . > install.log 2>&1

    log "harfbuzz built for $PLATFORM_NAME"
}

build_cairo() {
    local srcdir="${SOURCES_DIR}/cairo-${CAIRO_VERSION}"
    [ -d "$srcdir" ] || (cd "$SOURCES_DIR" && tar xf "cairo-${CAIRO_VERSION}.tar.xz")

    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"

    # Cairo 1.18+ uses meson. We'll try autotools if available, otherwise meson.
    # For cross-compilation, meson with a cross file is more reliable.
    local builddir="${BUILD_ROOT}/build-cairo-${PLATFORM_NAME}"
    rm -rf "$builddir"
    mkdir -p "$builddir"

    # Create meson cross file
    local crossfile="${builddir}/cross.txt"

    # Build properly quoted c_args and c_link_args arrays for meson
    local meson_c_args=""
    for flag in $CFLAGS; do
        [ -n "$meson_c_args" ] && meson_c_args="$meson_c_args, "
        meson_c_args="$meson_c_args'$flag'"
    done
    local meson_link_args=""
    for flag in $LDFLAGS; do
        [ -n "$meson_link_args" ] && meson_link_args="$meson_link_args, "
        meson_link_args="$meson_link_args'$flag'"
    done

    # Create a wrapper script for pkg-config that does NOT prepend sysroot
    local pkg_config_wrapper="${builddir}/pkg-config-wrapper.sh"
    cat > "$pkg_config_wrapper" <<'PKGEOF'
#!/bin/sh
exec pkg-config "$@"
PKGEOF
    chmod +x "$pkg_config_wrapper"

    cat > "$crossfile" <<CROSSEOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
ranlib = '$RANLIB'
pkgconfig = '$pkg_config_wrapper'

[built-in options]
c_args = [$meson_c_args]
c_link_args = [$meson_link_args]

[host_machine]
system = 'darwin'
cpu_family = '$MESON_CPU'
cpu = '$MESON_CPU'
endian = 'little'
CROSSEOF

    cd "$builddir"
    log "Configuring cairo (meson) for $PLATFORM_NAME..."

    # Set PKG_CONFIG_PATH so meson finds our built libs
    export PKG_CONFIG_PATH="${prefix}/lib/pkgconfig:${prefix}/share/pkgconfig"

    meson setup builddir "$srcdir" \
        --cross-file "$crossfile" \
        --prefix="$prefix" \
        --default-library=static \
        -Dxlib=disabled \
        -Dxcb=disabled \
        -Dquartz=disabled \
        -Dfreetype=enabled \
        -Dfontconfig=disabled \
        -Dpng=enabled \
        -Dtests=disabled \
        -Dspectre=disabled \
        > configure.log 2>&1 || { err "Meson configure failed for cairo"; cat configure.log; return 1; }

    log "Building cairo for $PLATFORM_NAME..."
    ninja -C builddir -j$(sysctl -n hw.ncpu) > build.log 2>&1 || { err "Build failed for cairo"; tail -20 build.log; return 1; }
    ninja -C builddir install > install.log 2>&1

    log "cairo built for $PLATFORM_NAME"
}

build_openssl() {
    [ "$WITH_CRYPTO" = "1" ] || return 0

    local srcdir="${SOURCES_DIR}/openssl-${OPENSSL_VERSION}"
    [ -d "$srcdir" ] || (cd "$SOURCES_DIR" && tar xf "openssl-${OPENSSL_VERSION}.tar.gz")

    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"
    local builddir="${BUILD_ROOT}/build-openssl-${PLATFORM_NAME}"

    # OpenSSL uses its own build system — need fresh source copy per platform
    rm -rf "$builddir"
    cp -a "$srcdir" "$builddir"
    cd "$builddir"

    local target
    case "$PLATFORM_NAME" in
        ios-simulator-arm64) target="iossimulator-xcrun" ;;
        ios-simulator-x86_64) target="iossimulator-xcrun" ;;
        maccatalyst-arm64) target="darwin64-arm64-cc" ;;
        maccatalyst-x86_64) target="darwin64-x86_64-cc" ;;
    esac

    log "Configuring OpenSSL for $PLATFORM_NAME..."
    ./Configure "$target" \
        --prefix="$prefix" \
        no-shared \
        no-tests \
        no-ui-console \
        "$CFLAGS" \
        > configure.log 2>&1 || { err "Configure failed for OpenSSL"; cat configure.log; return 1; }

    log "Building OpenSSL for $PLATFORM_NAME..."
    make -j$(sysctl -n hw.ncpu) > build.log 2>&1 || { err "Build failed for OpenSSL"; tail -20 build.log; return 1; }
    make install_sw > install.log 2>&1

    log "OpenSSL built for $PLATFORM_NAME"
}

build_libssh2() {
    [ "$WITH_CRYPTO" = "1" ] || return 0

    local srcdir="${SOURCES_DIR}/libssh2-${LIBSSH2_VERSION}"
    [ -d "$srcdir" ] || (cd "$SOURCES_DIR" && tar xf "libssh2-${LIBSSH2_VERSION}.tar.xz")

    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"
    build_cmake "libssh2" "$srcdir" \
        -DCRYPTO_BACKEND=OpenSSL \
        -DOPENSSL_ROOT_DIR="$prefix" \
        -DBUILD_TESTING=OFF \
        -DBUILD_EXAMPLES=OFF
}

build_libgit2() {
    local srcdir="${SOURCES_DIR}/libgit2-${LIBGIT2_VERSION}"
    [ -d "$srcdir" ] || (cd "$SOURCES_DIR" && tar xf "libgit2-${LIBGIT2_VERSION}.tar.gz" && mv "libgit2-${LIBGIT2_VERSION}" "$srcdir" 2>/dev/null || true)

    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"
    local cmake_extra=()

    if [ "$WITH_CRYPTO" = "1" ]; then
        cmake_extra+=(
            -DUSE_SSH=ON
            -DOPENSSL_ROOT_DIR="$prefix"
            "-DCMAKE_PREFIX_PATH=$prefix"
        )
    else
        cmake_extra+=(
            -DUSE_SSH=OFF
            -DUSE_HTTPS=OFF
        )
    fi

    build_cmake "libgit2" "$srcdir" \
        -DBUILD_TESTS=OFF \
        -DBUILD_CLI=OFF \
        -DUSE_BUNDLED_ZLIB=ON \
        "${cmake_extra[@]}"
}

# =====================================================================
# Build all libraries for one platform
# =====================================================================
build_all_for_platform() {
    local prefix="${INSTALL_ROOT}/${PLATFORM_NAME}"
    mkdir -p "$prefix"

    # Build in dependency order
    build_libpng
    build_freetype
    build_pixman
    build_harfbuzz
    build_cairo

    if [ "$WITH_CRYPTO" = "1" ]; then
        build_openssl
        build_libssh2
    fi

    build_libgit2
}

# =====================================================================
# Create fat libraries (lipo) and xcframeworks
# =====================================================================
create_fat_lib() {
    local libname="$1"
    local arch1_dir="$2"
    local arch2_dir="$3"
    local output_dir="$4"

    local lib1="${arch1_dir}/lib/${libname}"
    local lib2="${arch2_dir}/lib/${libname}"

    if [ -f "$lib1" ] && [ -f "$lib2" ]; then
        mkdir -p "$output_dir/lib"
        lipo -create "$lib1" "$lib2" -output "$output_dir/lib/${libname}"
        log "Created fat library: $output_dir/lib/${libname}"
    elif [ -f "$lib1" ]; then
        mkdir -p "$output_dir/lib"
        cp "$lib1" "$output_dir/lib/${libname}"
    fi
}

create_xcframework() {
    local libname="$1"     # e.g., libcairo.a
    local xcf_name="$2"    # e.g., cairo

    local sim_fat="${BUILD_ROOT}/fat-ios-simulator"
    local cat_fat="${BUILD_ROOT}/fat-maccatalyst"
    local output="${PROJECT_DIR}/${xcf_name}.xcframework"

    rm -rf "$output"

    local args=()
    if [ -f "${sim_fat}/lib/${libname}" ]; then
        # Copy headers
        local header_dir="${INSTALL_ROOT}/ios-simulator-arm64/include"
        args+=(-library "${sim_fat}/lib/${libname}")
        if [ -d "$header_dir" ]; then
            args+=(-headers "$header_dir")
        fi
    fi
    if [ -f "${cat_fat}/lib/${libname}" ]; then
        local header_dir="${INSTALL_ROOT}/maccatalyst-arm64/include"
        args+=(-library "${cat_fat}/lib/${libname}")
        if [ -d "$header_dir" ]; then
            args+=(-headers "$header_dir")
        fi
    fi

    if [ ${#args[@]} -gt 0 ]; then
        xcodebuild -create-xcframework "${args[@]}" -output "$output"
        log "Created $output"
    else
        warn "No libraries found for $xcf_name"
    fi
}

package_xcframeworks() {
    log "Creating fat libraries..."

    local sim_arm64="${INSTALL_ROOT}/ios-simulator-arm64"
    local sim_x86="${INSTALL_ROOT}/ios-simulator-x86_64"
    local cat_arm64="${INSTALL_ROOT}/maccatalyst-arm64"
    local cat_x86="${INSTALL_ROOT}/maccatalyst-x86_64"
    local sim_fat="${BUILD_ROOT}/fat-ios-simulator"
    local cat_fat="${BUILD_ROOT}/fat-maccatalyst"

    rm -rf "$sim_fat" "$cat_fat"
    mkdir -p "$sim_fat" "$cat_fat"

    # Create fat binaries for each platform
    local libs=("libpng16.a" "libfreetype.a" "libpixman-1.a" "libharfbuzz.a" "libcairo.a")
    if [ "$WITH_CRYPTO" = "1" ]; then
        libs+=("libssl.a" "libcrypto.a" "libssh2.a")
    fi
    libs+=("libgit2.a")

    for lib in "${libs[@]}"; do
        create_fat_lib "$lib" "$sim_arm64" "$sim_x86" "$sim_fat"
        create_fat_lib "$lib" "$cat_arm64" "$cat_x86" "$cat_fat"
    done

    log "Creating xcframeworks..."

    create_xcframework "libpng16.a" "libpng16"
    create_xcframework "libfreetype.a" "freetype"
    create_xcframework "libpixman-1.a" "pixman"
    create_xcframework "libharfbuzz.a" "harfbuzz"
    create_xcframework "libcairo.a" "cairo"
    create_xcframework "libgit2.a" "libgit2"

    if [ "$WITH_CRYPTO" = "1" ]; then
        create_xcframework "libssl.a" "libssl"
        create_xcframework "libcrypto.a" "libcrypto"
        create_xcframework "libssh2.a" "libssh2"
    fi
}

# =====================================================================
# Main
# =====================================================================
main() {
    log "Building third-party libraries for iOS"
    log "Crypto: $([ "$WITH_CRYPTO" = "1" ] && echo "enabled" || echo "disabled")"

    mkdir -p "$BUILD_ROOT"

    # Download sources
    download_sources

    # Build for iOS Simulator (arm64 + x86_64)
    log "=== Building for iOS Simulator arm64 ==="
    setup_ios_simulator_arm64
    build_all_for_platform

    log "=== Building for iOS Simulator x86_64 ==="
    setup_ios_simulator_x86_64
    build_all_for_platform

    # Build for Mac Catalyst (arm64 + x86_64)
    log "=== Building for Mac Catalyst arm64 ==="
    setup_maccatalyst_arm64
    build_all_for_platform

    log "=== Building for Mac Catalyst x86_64 ==="
    setup_maccatalyst_x86_64
    build_all_for_platform

    # Package as xcframeworks
    package_xcframeworks

    log "Done! xcframeworks created in ${PROJECT_DIR}/"
    ls -la "${PROJECT_DIR}"/*.xcframework 2>/dev/null || warn "No xcframeworks found"
}

main "$@"
