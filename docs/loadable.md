# Library & Plugin Comparison: Standard Pharo VM vs iOS Pharo VM

## Standard Pharo VM Bundles

From `Pharo.app/Contents/MacOS/Plugins/`:

### VM Plugin Dylibs (18)
| Plugin | Purpose |
|--------|---------|
| B2DPlugin | Balloon 2D vector graphics engine |
| BitBltPlugin | Bitwise block transfer (image compositing) |
| DSAPrims | Digital Signature Algorithm (SHA-1, DSA) |
| FileAttributesPlugin | POSIX file attributes, directory iteration |
| FilePlugin | File I/O (open, read, write, etc.) |
| FloatArrayPlugin | Fast float array access |
| JPEGReaderPlugin | Legacy JPEG decoder (DCT-based) |
| JPEGReadWriter2Plugin | Full JPEG read/write (bundles libjpeg 6b) |
| LargeIntegers | Large integer arithmetic |
| LocalePlugin | Locale and timezone info |
| MiscPrimitivePlugin | String hashing, searching, compression |
| SocketPlugin | TCP/UDP sockets and DNS resolution |
| SqueakSSL | SSL/TLS support |
| SurfacePlugin | Manual surface management for display |
| TestLibrary | FFI test harness (loaded via dlsym) |
| UnixOSProcessPlugin | Unix process management (fork/exec/pipe) |
| UUIDPlugin | UUID generation |
| tty | Pseudo-terminal helper |

### Third-Party Dylibs (12+)
| Library | Purpose |
|---------|---------|
| libcairo | 2D graphics rendering |
| libfreetype | Font rendering |
| libfontconfig | Font configuration/discovery |
| libharfbuzz | Text shaping |
| libpixman | Pixel manipulation (cairo backend) |
| libpng | PNG image format |
| SDL2 | Window management, events, audio |
| libssl | SSL/TLS (OpenSSL) |
| libcrypto | Cryptographic primitives (OpenSSL) |
| libgit2 | Git repository operations |
| libssh2 | SSH protocol |
| libjpeg | JPEG codec (also bundled in JPEGReadWriter2Plugin) |

## Our VM: Built-in Plugins

These are compiled statically into PharoVMCore:

| Plugin | Implementation |
|--------|---------------|
| B2DPlugin | VMMaker-generated C via InterpreterProxy |
| BitBltPlugin | Native C++ in Primitives.cpp |
| DSAPrims | VMMaker-generated C via InterpreterProxy |
| FilePlugin | Native C++ (POSIX file I/O) |
| FileAttributesPlugin | Native C++ (stat, opendir, etc.) |
| FloatArrayPlugin | Native C++ |
| JPEGReaderPlugin | VMMaker-generated C via InterpreterProxy |
| JPEGReadWriter2Plugin | VMMaker-generated C + bundled libjpeg 6b |
| LargeIntegers | Native C++ (Karatsuba, Montgomery, etc.) |
| LocalePlugin | Native C++ (timezone, daylight saving) |
| MiscPrimitivePlugin | Native C++ (string hash, search, compress) |
| SocketPlugin | Native C++ (DNS only; full sockets not yet) |
| SqueakSSL | VMMaker-generated C + Apple Security.framework |
| SurfacePlugin | Native C++ (manual surface management) |
| TestLibrary | Force-loaded symbols (available via dlsym) |
| UUIDPlugin | Native C++ (Apple CFUUID) |
| iOSPlugin | Native C++ (display, events, platform) |
| SecurityPlugin | Native C++ (sandbox paths) |
| TFFI | Native C++ (ThreadedFFI primitives) |

## Plugins NOT Included (and why)

| Plugin | Reason |
|--------|--------|
| UnixOSProcessPlugin | Requires fork()/exec(), prohibited on iOS App Store |
| tty | Pseudo-terminal helper, requires fork(), iOS-incompatible |

## Third-Party Libraries

### Mac Catalyst
All third-party libraries are bundled via `scripts/bundle-libs.sh`, which copies
Homebrew dylibs into the app bundle at build time. This includes cairo, freetype,
fontconfig, harfbuzz, pixman, libpng, libgit2, libssh2, libssl, libcrypto, and
their transitive dependencies (X11, glib, graphite2, etc.).

### iOS
Third-party libraries must be cross-compiled as static libraries and packaged as
xcframeworks. See `scripts/build-third-party.sh` for the build process.

**Minimum set for iOS** (in dependency order):
1. **libpng** — PNG image format support
2. **freetype** — Font rendering
3. **pixman** — Pixel manipulation (cairo backend)
4. **harfbuzz** (minimal build) — Text shaping
5. **cairo** (no X11) — 2D graphics rendering
6. **OpenSSL** (libssl + libcrypto) — conditional on `PHARO_WITH_CRYPTO`
7. **libssh2** — conditional (needs OpenSSL)
8. **libgit2** — Git support (can build without SSH)

**Skipped for iOS**:
- fontconfig — Complex to cross-compile; cairo works without it using FreeType directly
- X11 chain (libX11, libXext, libXrender, libxcb, libXau, libXdmcp) — No X11 on iOS
- glib, graphite2, intl, pcre2 — Only needed by harfbuzz full build

## Crypto Export Compliance

The `PHARO_WITH_CRYPTO` CMake option controls inclusion of cryptographic components:

**When ON (default)**:
- DSAPrims plugin compiled and registered
- SqueakSSL plugin compiled and registered (uses Apple Security.framework)
- `bundle-libs.sh` includes libssl, libcrypto (Mac Catalyst)
- Third-party xcframeworks include OpenSSL (iOS)

**When OFF (export-safe)**:
- DSAPrims excluded from build
- SqueakSSL excluded from build
- No OpenSSL bundled
- FFI stubs return errors for crypto functions

**Note**: Apple's Security.framework uses Apple's own crypto implementations.
Apps using only system frameworks are exempt from US export controls (ECCN 5D002).
The export concern is specifically for **bundled** OpenSSL/libcrypto.
