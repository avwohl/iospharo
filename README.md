# iospharo

A Pharo Smalltalk VM for iOS and macOS, written as a clean C++ interpreter.

## Overview

iospharo runs standard Pharo 13 images on iOS devices and Mac (via Catalyst).
It is a from-scratch interpreter implementation — not a port of the Cog JIT VM —
with full support for the Sista V1 bytecode set, FFI, and the standard Pharo
test suite.

## Status

- **99.90% test pass rate** on Mac Catalyst (13,040 pass / 13,053 run)
- **99.55% test pass rate** on iOS Simulator
- Full GUI: Pharo desktop, menus, windows, keyboard and mouse input
- FFI with callbacks (sigsetjmp/siglongjmp)
- All standard VM plugins built-in (B2D, JPEG, DSA, SSL, etc.)
- Third-party libraries: cairo, freetype, harfbuzz, pixman, libpng, OpenSSL, libssh2, libgit2

## Features

- **Metal Rendering**: GPU-accelerated display via SDL2 FFI stubs bridged to Metal
- **Touch Support**: Full gesture mapping for Pharo mouse events
  - Tap = Left click
  - Long press = Right click
  - Two-finger tap = Middle click
  - Drag = Mouse move with button down
- **Mac Catalyst**: Runs natively on macOS
- **Standard Images**: Works with unmodified Pharo 13 images

## Requirements

- iOS 15.0+ / macOS (Catalyst)
- Xcode 15.0+
- CMake 3.20+
- pharo-vm source (sibling directory, for headers)

## Building

### Quick Start (Mac Catalyst for development)

```bash
# Build the VM library
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests with a Pharo image
./build/test_load_image /path/to/Pharo.image
```

### XCFramework (for iOS + Mac Catalyst)

```bash
# Build third-party libraries (cairo, freetype, OpenSSL, etc.)
scripts/build-third-party.sh

# Build PharoVMCore.xcframework (device + simulator + catalyst)
./build-xcframework.sh

# Open in Xcode and build
open iospharo.xcodeproj
```

### Architecture

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

Key directories:
- `src/vm/` — Clean C++ VM (Interpreter, ObjectMemory, Primitives, ImageLoader)
- `src/ios/` — iOS platform layer and generated interpreter reference
- `iospharo/` — Swift/SwiftUI app (Metal renderer, bridge, views)
- `scripts/` — Build scripts for third-party libraries
- `docs/` — Technical reference documentation

## Configuration

Edit `Info.plist` to configure VM parameters:

| Key | Default | Description |
|-----|---------|-------------|
| `PharoImageFile` | `Pharo.image` | Default image filename |
| `PharoMaxOldSpaceSize` | 536870912 (512 MB) | Max heap size |
| `PharoEdenSize` | 10485760 (10 MB) | Young generation size |
| `PharoCodeSize` | 67108864 (64 MB) | Code space size |
| `PharoHeadless` | false | Run without display |

## License

MIT License — see [LICENSE](LICENSE).

## Credits

Built on the [Pharo](https://pharo.org) project. Uses [libffi](https://github.com/libffi/libffi),
[SDL2](https://www.libsdl.org), and other open source libraries.
