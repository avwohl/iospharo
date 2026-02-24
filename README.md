# iospharo

A Pharo Smalltalk VM for iOS and macOS, written as a clean C++ interpreter.

## Overview

iospharo runs standard Pharo 13 images on iOS devices and Mac (via Catalyst).
It is a from-scratch interpreter implementation — not a port of the Cog JIT VM —
with full support for the Sista V1 bytecode set, FFI with callbacks, and the
standard Pharo test suite.

## Status

- **99.90% test pass rate** on Mac Catalyst (13,040 / 13,053)
- **99.55% test pass rate** on iOS Simulator
- Full GUI: Pharo desktop, menus, windows, keyboard and mouse input
- FFI with callbacks (sigsetjmp/siglongjmp)
- All standard VM plugins built-in (B2D, JPEG, DSA, SSL, etc.)
- Third-party libraries: cairo, freetype, harfbuzz, pixman, libpng, OpenSSL, libssh2, libgit2

## Prerequisites

Install these before building:

```bash
# Xcode command-line tools (includes clang, make, etc.)
xcode-select --install

# CMake (build system)
brew install cmake

# For third-party library builds (cairo, freetype, etc.)
brew install meson ninja pkg-config autoconf automake libtool
```

You also need:
- **Xcode 15+** (for the iOS/Mac Catalyst app)
- **A Pharo 13 image** — download from https://pharo.org/download

## Building

### Step 1: Build libffi and SDL2 xcframeworks

```bash
scripts/build-libffi.sh
scripts/build-sdl2.sh
```

This downloads, cross-compiles, and packages libffi (FFI/callbacks) and SDL2
(display driver) as xcframeworks for iOS device, simulator, Mac Catalyst, and
macOS. Takes about 10 minutes. The xcframeworks are gitignored due to size.

### Step 2: Build third-party libraries

Cairo, freetype, harfbuzz, pixman, libpng, OpenSSL, libssh2, and libgit2
are cross-compiled as static xcframeworks:

```bash
scripts/build-third-party.sh
```

This downloads source tarballs and builds for iOS device, Simulator, and Mac Catalyst.
Takes about 15 minutes on first run. Use `--no-crypto` to skip OpenSSL and libssh2
(libgit2 is always built for local repository support).

### Step 3: Build the VM xcframework

```bash
./build-xcframework.sh
```

This produces `PharoVMCore.xcframework` with slices for:
- iOS device (arm64)
- iOS Simulator (arm64)
- Mac Catalyst (arm64)

### Step 4: Build the app

```bash
open iospharo.xcodeproj
```

Select your target (iOS device, Simulator, or My Mac - Catalyst) and build.

**Code signing (optional):** To deploy to a physical device or the App Store,
copy `Local.xcconfig.example` to `Local.xcconfig` and fill in your Apple
Developer Team ID. `Local.xcconfig` is gitignored.

### Quick development build (Mac only)

For faster iteration without xcframeworks:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run headless with a Pharo image
./build/test_load_image /path/to/Pharo.image
```

## Project Structure

```
iospharo/
├── src/vm/           C++ VM: Interpreter, ObjectMemory, Primitives, FFI
├── src/include/      VM headers (vmCallback.h, etc.)
├── src/platform/     Platform abstraction (EventQueue, display)
├── src/ios/          Generated interpreter reference (cointerp-cpp.c)
├── iospharo/         SwiftUI app (Metal renderer, bridge, views)
├── scripts/          Build scripts for third-party libraries
├── docs/             Technical reference (bytecode spec, architecture)
├── CMakeLists.txt    CMake build for the VM library
└── build-xcframework.sh   Builds PharoVMCore.xcframework
```

## Architecture

```
┌─────────────────────────────┐
│   SwiftUI App               │
│   (ContentView, Settings)   │
├─────────────────────────────┤
│   PharoBridge.swift         │  VM lifecycle, event bridge
├─────────────────────────────┤
│   MetalRenderer.swift       │  GPU display rendering
├─────────────────────────────┤
│   C++ Interpreter           │  Sista V1 bytecodes, GC,
│   (libPharoVMCore.a)        │  FFI, primitives, plugins
└─────────────────────────────┘
```

The image's OSSDL2Driver calls SDL2 functions via FFI. Our SDL2 stubs bridge
these to the Metal rendering pipeline. Touch gestures are mapped to Pharo
mouse events (tap=left, long-press=right, two-finger=middle).

## Configuration

VM parameters in `iospharo/Info.plist`:

| Key | Default | Description |
|-----|---------|-------------|
| `PharoImageFile` | `Pharo.image` | Image filename |
| `PharoMaxOldSpaceSize` | 536870912 (512 MB) | Max heap size |
| `PharoEdenSize` | 10485760 (10 MB) | Young generation size |
| `PharoCodeSize` | 67108864 (64 MB) | Code space size |
| `PharoHeadless` | false | Run without display |

## License

MIT — see [LICENSE](LICENSE).

## Credits

Built on the [Pharo](https://pharo.org) project. Uses
[libffi](https://github.com/libffi/libffi) and
[SDL2](https://www.libsdl.org).
