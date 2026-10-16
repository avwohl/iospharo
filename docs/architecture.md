# Project structure and architecture

This text was in README.md until 2026-10-17.

## Project Structure

```
iospharo/
├── src/vm/           C++ VM: Interpreter, ObjectMemory, Primitives, FFI
├── src/include/      VM headers (vmCallback.h, etc.)
├── src/platform/     Platform abstraction (EventQueue, display)
├── src/ios/          Generated interpreter reference (cointerp-cpp.c)
├── iospharo/         SwiftUI app (Metal renderer, bridge, views)
├── scripts/          Build scripts (VM xcframework, third-party libraries)
├── docs/             Technical reference (bytecode spec, architecture)
├── Frameworks/       Built xcframeworks (gitignored)
└── CMakeLists.txt    CMake build for the VM library
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
mouse events (tap=left-click, long-press=right-click, two-finger tap=right-click).

### Startup patches

The app applies Smalltalk patches on every launch to fix image bugs and adapt
to VM differences (stubbed SDL2, missing font glyphs, etc.). Patches are
version-specific — the app detects the Pharo version and loads `startup-13.st`
or `startup-14.st` accordingly. Users can add custom patches by creating
`startup-user.st` next to the image file (it is never overwritten).
See [docs/startup-system.md](docs/startup-system.md) for the full details.

## Configuration

VM parameters are set in `PharoBridge.swift` when calling `vm_init()`:

  maxOldSpaceSize   2 GB    Max heap (virtual, lazy commit)
  edenSize          10 MB   Young generation size
  maxCodeSize       0       JIT code space (unused)
