#!/bin/bash
# bundle-libs.sh — Copy, relink, and re-sign third-party dylibs into the app bundle.
# Called as an Xcode "Run Script" build phase or manually.
#
# Mac Catalyst enforces team-ID matching for dlopen'd libraries.
# Homebrew libraries have different team IDs, so they must be copied
# into the app bundle, have their dependency paths fixed to @loader_path,
# and be re-signed with our identity.
#
# On iOS, these libraries would need to be cross-compiled for ARM64 iOS,
# which is a separate build step. For now, this script handles macOS/Mac Catalyst.

set -euo pipefail

# Destination inside app bundle
FRAMEWORKS_DIR="${BUILT_PRODUCTS_DIR}/${FRAMEWORKS_FOLDER_PATH}"
mkdir -p "$FRAMEWORKS_DIR"

# Homebrew library paths (ARM Mac)
BREW_LIB="/opt/homebrew/lib"
# Intel Mac fallback
if [ ! -d "$BREW_LIB" ]; then
    BREW_LIB="/usr/local/lib"
fi

# Libraries to bundle — core libraries used by Pharo image via FFI
LIBS=(
    # Core rendering / text
    "libcairo.2.dylib"
    "libfreetype.6.dylib"
    "libpixman-1.0.dylib"
    "libpng16.16.dylib"
    "libharfbuzz.0.dylib"
    "libfontconfig.1.dylib"
    # Git support
    "libgit2.1.9.dylib"
    # Transitive dependencies of cairo (X11 backend)
    "libX11.6.dylib"
    "libXext.6.dylib"
    "libXrender.1.dylib"
    "libxcb.1.dylib"
    "libxcb-render.0.dylib"
    "libxcb-shm.0.dylib"
    # Transitive dependencies of libgit2
    "libssh2.1.dylib"
    # Transitive dependencies of harfbuzz
    "libglib-2.0.0.dylib"
    "libgraphite2.3.dylib"
    # Transitive dependencies of fontconfig/glib
    "libintl.8.dylib"
    "libpcre2-8.0.dylib"
    # Transitive dependencies of libxcb
    "libXau.6.dylib"
    "libXdmcp.6.dylib"
    # Transitive dependencies of libssh2
    "libssl.3.dylib"
    "libcrypto.3.dylib"
)

# Versionless symlinks (e.g., libcairo.dylib -> libcairo.2.dylib)
SYMLINKS=(
    "libcairo.dylib:libcairo.2.dylib"
    "libfreetype.dylib:libfreetype.6.dylib"
    "libgit2.dylib:libgit2.1.9.dylib"
    "libpixman-1.dylib:libpixman-1.0.dylib"
    "libpng.dylib:libpng16.16.dylib"
    "libpng16.dylib:libpng16.16.dylib"
    "libharfbuzz.dylib:libharfbuzz.0.dylib"
    "libfontconfig.dylib:libfontconfig.1.dylib"
    "libssh2.dylib:libssh2.1.dylib"
    "libglib-2.0.dylib:libglib-2.0.0.dylib"
)

COPIED=0
FAILED=0

for lib in "${LIBS[@]}"; do
    SRC="$BREW_LIB/$lib"
    DST="$FRAMEWORKS_DIR/$lib"

    if [ ! -f "$SRC" ]; then
        echo "warning: $SRC not found, skipping"
        FAILED=$((FAILED + 1))
        continue
    fi

    # Only copy if source is newer or destination doesn't exist
    if [ ! -f "$DST" ] || [ "$SRC" -nt "$DST" ]; then
        cp "$SRC" "$DST"
        chmod u+w "$DST"

        # Fix install name to use @loader_path
        install_name_tool -id "@loader_path/$lib" "$DST" 2>/dev/null || true

        # Fix all dependency paths that point to Homebrew to use @loader_path
        # This is critical: libraries reference each other via absolute Homebrew paths
        # (e.g., freetype depends on /opt/homebrew/opt/libpng/lib/libpng16.16.dylib)
        otool -L "$DST" | tail -n +2 | awk '{print $1}' | while read -r dep; do
            case "$dep" in
                /opt/homebrew/*)
                    # Extract just the library filename
                    depname=$(basename "$dep")
                    install_name_tool -change "$dep" "@loader_path/$depname" "$DST" 2>/dev/null || true
                    ;;
                /usr/local/*)
                    depname=$(basename "$dep")
                    install_name_tool -change "$dep" "@loader_path/$depname" "$DST" 2>/dev/null || true
                    ;;
            esac
        done

        # Re-sign with development identity (Mac Catalyst requires matching team ID)
        # Use CODE_SIGN_IDENTITY from Xcode if available, otherwise find a valid identity
        SIGN_IDENTITY="${CODE_SIGN_IDENTITY:-}"
        if [ -z "$SIGN_IDENTITY" ]; then
            # Try to find an Apple Development identity
            SIGN_IDENTITY=$(security find-identity -v -p codesigning | grep "Apple Development" | head -1 | sed 's/.*"\(.*\)"/\1/')
        fi
        if [ -n "$SIGN_IDENTITY" ]; then
            codesign --force --sign "$SIGN_IDENTITY" "$DST"
        else
            # Fallback to ad-hoc (won't work in Mac Catalyst with team ID validation)
            echo "warning: No development signing identity found, using ad-hoc for $lib"
            codesign --force --sign - "$DST"
        fi
        echo "Bundled: $lib"
        COPIED=$((COPIED + 1))
    fi
done

# Create symlinks
for pair in "${SYMLINKS[@]}"; do
    LINK="${pair%%:*}"
    TARGET="${pair##*:}"
    if [ -f "$FRAMEWORKS_DIR/$TARGET" ]; then
        ln -sf "$TARGET" "$FRAMEWORKS_DIR/$LINK"
    fi
done

echo "bundle-libs: $COPIED copied, $FAILED missing (of ${#LIBS[@]} libraries)"
