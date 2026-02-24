# iospharo — Programmer's Overview

A clean-room Pharo VM for iOS (and Mac Catalyst), built to run standard Pharo 13 images
on Apple platforms where JIT compilation is forbidden.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  SwiftUI App  (iospharo/)                                       │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────────────┐ │
│  │ ImageLibrary │  │ PharoCanvas  │  │ MetalRenderer          │ │
│  │ View         │→ │ View (MTKView│  │ (30fps display-buffer  │ │
│  │ (pick image) │  │  + gestures) │  │  → GPU texture blit)   │ │
│  └──────────────┘  └──────┬───────┘  └────────────┬───────────┘ │
│                           │ touch/key              │ pixels      │
│  ┌────────────────────────▼────────────────────────▼───────────┐ │
│  │ PharoBridge  (@MainActor, publishes isRunning)              │ │
│  │ vm_postMouseEvent / vm_postKeyEvent / ios_getDisplayBits    │ │
│  └────────────────────────┬────────────────────────────────────┘ │
└───────────────────────────┼─────────────────────────────────────┘
                            │ C API
┌───────────────────────────▼─────────────────────────────────────┐
│  PharoVMCore  (static library, C++)                             │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────────────────┐ │
│  │ Interpreter  │  │ ObjectMemory │  │ Primitives (577 slots) │ │
│  │ (Sista V1    │  │ (Spur 64-bit │  │ + FFI / SDL2 stubs     │ │
│  │  bytecodes)  │  │  iOS tags)   │  │ + B2DPlugin, BitBlt,   │ │
│  │              │  │              │  │   FilePlugin, etc.)    │ │
│  └──────┬───────┘  └──────────────┘  └────────────────────────┘ │
│         │                                                       │
│  ┌──────▼───────────────────────────────────────────────────────┐│
│  │ Platform layer: EventQueue, SimpleDisplaySurface,           ││
│  │ iOSBridge (C entry points for Swift)                        ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

### Key design decisions

| Decision | Rationale |
|----------|-----------|
| Interpreter-only (no JIT) | Apple forbids W+X memory on iOS |
| Low-bit OOP tags | iOS ASLR uses high address bits; standard Spur uses high bits for tags |
| SDL2 stubs via FFI | Standard Pharo images use OSSDL2Driver; our stubs bridge to Metal |
| Static xcframework | Single `PharoVMCore.xcframework` links into the Xcode app for all Apple platforms |
| No image modifications | Standard Pharo 13 images work unmodified |

### Display pipeline

1. Pharo image calls SDL2 functions via FFI (OSSDL2Driver)
2. Our `FFI.cpp` stubs intercept these calls and write to a `SimpleDisplaySurface` pixel buffer
3. `MetalRenderer` reads that pixel buffer every frame (30 fps) and uploads it to a GPU texture
4. A fullscreen quad renders the texture to the `MTKView`

### Input pipeline

1. UIKit delivers touch/key events to `PharoMTKView`
2. Swift calls `vm_postMouseEvent()` / `vm_postKeyEvent()` with UIKit-relative coordinates
3. Events enter the `EventQueue` (thread-safe, mutex-protected)
4. Pharo's event loop reads them via primitive 264 (`ioGetNextEvent`)

## Source layout

```
src/
  vm/                    # Clean C++ VM (the core)
    Interpreter.cpp/hpp  #   Bytecode interpreter, process scheduler
    ObjectMemory.cpp/hpp #   Spur heap, GC, allocation
    Primitives.cpp       #   577 numbered primitives
    Oop.hpp              #   Tagged pointer with iOS low-bit encoding
    ObjectHeader.hpp     #   Spur object header (64-bit)
    ImageLoader.cpp      #   .image file loader (converts standard tags → iOS tags)
    ImageWriter.cpp      #   Snapshot writer (converts back to standard Spur)
    FFI.cpp              #   SDL2 stubs + TFFI primitive dispatch
    InterpreterProxy.cpp #   Legacy plugin API shim
    plugins/             #   C plugins (B2DPlugin, BitBltPlugin, FilePlugin, etc.)
  platform/              # Platform abstraction
    EventQueue.cpp       #   Thread-safe event queue
    PlatformBridge.cpp   #   SimpleDisplaySurface (pixel buffer)
    iOSBridge.cpp        #   C entry points: vm_init, vm_run, vm_stop, vm_post*
  ios/                   # Reference / generated files (not compiled by current build)
    cointerp-cpp.c       #   VMMaker-generated interpreter (94K lines, used as reference)
    primitives.json      #   Authoritative primitive table from VMMaker
  include/               # Shared C headers for the bridging layer

iospharo/                # Swift app (Mac Catalyst + iOS)
  App/                   #   iosparoApp.swift, ContentView.swift
  Bridge/                #   PharoBridge.swift (VM lifecycle, event posting)
  Metal/                 #   MetalRenderer.swift, Shaders.metal
  Views/                 #   PharoCanvasView.swift (MTKView + gestures), ImageLibraryView
  Image/                 #   ImageManager.swift (download, catalog, import)

scripts/                 # Build and test helpers
  build-third-party.sh   #   Builds cairo, freetype, libffi, etc. as xcframeworks
  build-xcframework.sh   #   Builds PharoVMCore.xcframework (3 platform slices)
  run_sunit_tests.st     #   Injects SUnit test runner into a Pharo image
  run_batch_tests.sh     #   Runs full test suite in batches

docs/                    # Documentation
  SistaV1-Bytecode-Spec.md  # Local copy of the Sista V1 bytecode spec
  performance.md             # Performance comparison with standard VM
  non-passing-tests.md       # Test compatibility analysis
```

## Building

### Prerequisites
- Xcode 16+ with iOS 15+ SDK
- CMake 3.20+ and Ninja
- A Pharo 13 image (downloaded automatically by the app, or manually via `curl -sL https://get.pharo.org/64/130 | bash`)

### Quick build (local testing)
```bash
cmake -B build -G Ninja && cmake --build build
./build/test_load_image /path/to/Pharo.image
```

### Full Mac Catalyst app
```bash
# 1. Build third-party libs (one-time, ~5 min)
./scripts/build-third-party.sh

# 2. Build PharoVMCore.xcframework (3 slices: device, simulator, Mac Catalyst)
./build-xcframework.sh

# 3. Build the app
cp Local.xcconfig.example Local.xcconfig   # edit with your Apple team ID
xcodebuild -project iospharo.xcodeproj -scheme iospharo \
  -configuration Debug -destination 'platform=macOS,variant=Mac Catalyst' build
```

## OOP encoding

Standard Spur uses high address bits for tag encoding. iOS ASLR randomizes those bits,
so this VM moves tags to the low 3 bits:

| Tag (low 3 bits) | Type |
|-------------------|------|
| `001` | SmallInteger |
| `010` | SmallFloat64 |
| `011` | Character |
| `000` | Object pointer (8-byte aligned) |

`ImageLoader` converts standard Spur tags → iOS tags on load.
`ImageWriter` converts back to standard Spur on snapshot (primitive 97).

## Test compatibility

The VM runs the full Pharo 13 SUnit test suite (577 classes, 13,040 tests).
All non-passing tests also fail on the official Pharo VM — zero VM-specific failures.
See `docs/non-passing-tests.md` for details.

## Code review findings (2026-02-24)

A full code review was performed across all languages. The findings below are
prioritized for community readiness.

### Must fix before wider adoption

| # | Area | Issue |
|---|------|-------|
| 1 | **VM stop on app exit** | `applicationWillTerminate` schedules a `Task` then sleeps the main thread — `vm_stop()` likely never executes. Call `stop()` synchronously instead. (iosparoApp.swift:62) |
| 2 | **Clipboard deadlock** | `DispatchQueue.main.sync` in clipboard-get callback can deadlock if the main thread is waiting on the VM. (PharoBridge.swift:72) |
| 3 | **SDL\_free stub missing** | `SDL_GetClipboardText` returns `strdup`'d memory; without an `SDL_free` stub mapped to `free()`, every paste leaks. (FFI.cpp) |
| 4 | **mmap/free mismatch** | ObjectMemory error path calls `std::free()` on an mmap'd region. (ObjectMemory.cpp:68) |
| 5 | **Semaphore signal loss** | `signalExternalSemaphore` stores a single index — rapid signals from different sources lose all but the last. (Interpreter.cpp:1846) |
| 6 | **Hardcoded class indices** | Magic numbers `3117` (CompiledBlock) and `38` (FullBlockClosure) will silently break on different images. (Interpreter.cpp:2585) |

### Should fix (quality / maintenance)

| # | Area | Issue |
|---|------|-------|
| 7 | **Dead code** | ~1,500 lines in `src/ios/` (iosDisplay.m, iosEvents.m, etc.) are not compiled by any build target. Remove or move to a `reference/` directory. |
| 8 | **Dead Swift code** | `displayNeedsUpdate` is never set to true; `handleSingleTap`/`handleDoubleTap`/`handlePan` gesture handlers are never connected; `getDisplayBits()`/`getDisplaySize()`/`getDisplayDepth()` are never called; `NSEventMonitorHelper` is imported but unused. |
| 9 | **Debug leftovers** | 50+ commented-out `cerr`/debug lines and 40+ `static int` debug counters in Interpreter.cpp. |
| 10 | **Stale project.yml** | References nonexistent `app/macOS` and `app/Shared` directories. Delete it. |
| 11 | **scripts/README.md** | References 3 deleted files (`explore-vmmaker.st`, `ios-simulator-setup.st`, `debug-unrelocated-pointers.st`). |
| 12 | **Incomplete xcframework freshness check** | Xcode build phase only monitors 9 source files; changes to FFI.cpp, InterpreterProxy.cpp, plugins, or platform/ sources won't trigger a rebuild. |
| 13 | **Stale header search paths** | Xcode project references `$(PROJECT_DIR)/../pharo-vm/` which doesn't exist. |
| 14 | **Duplicate VMParameters struct** | Defined separately in iOSBridge.cpp and test\_ios\_bridge.cpp — could diverge. |
| 15 | **`framebufferOnly = false`** | Disables a Metal performance optimization for no benefit (no drawable readback is done). (MetalRenderer.swift:81) |

### Nice to have

| # | Area | Issue |
|---|------|-------|
| 16 | Logging | Mix of `NSLog`, `fputs(stderr)`, `fprintf` — pick one approach (ideally `os_log`). |
| 17 | Hardcoded version | Settings view shows "1.0.0" instead of reading from bundle. (SettingsView.swift:28) |
| 18 | Eden GC | Scavenge copies objects but never reclaims eden space — effectively no young-gen GC. (ObjectMemory.cpp:1149) |
| 19 | docs/WIP.md | Referenced in CLAUDE.md but doesn't exist. |
| 20 | Deprecated API | `UI_USER_INTERFACE_IDIOM()` and `.autocapitalization(.none)` are deprecated. |
| 21 | `renderWorldMorphs` | 730-line C++ method doing pixel-level menu rendering — belongs in a separate file if kept. (Interpreter.cpp:581) |
| 22 | x86\_64 slices | `build-xcframework.sh` only builds arm64 — no Intel Mac support. |
| 23 | `crash.dmp` | Untracked file not in `.gitignore`. Add `*.dmp` pattern. |

### What's good

- **Core abstractions are clean**: `Oop.hpp`, `ObjectHeader.hpp`, `ObjectMemory.hpp` are well-designed with proper encapsulation.
- **SDL2 stub layer is thorough**: ~60 functions with proper state tracking, creative poll-count delay for startup timing.
- **Thread safety in active code**: `EventQueue` and `SimpleDisplaySurface` use `std::lock_guard<std::mutex>` consistently.
- **Test compatibility is excellent**: Zero VM-specific test failures.
- **Image compatibility**: Standard Pharo 13 images work unmodified, including snapshot save/reload.
- **Touch-to-mouse translation**: Comprehensive gesture handling for iOS (tap, long-press, two-finger, pinch, drag, hardware keyboard).
- **Documentation**: Bytecode spec, performance analysis, and test compatibility are well-documented.
- **License**: MIT, clean and appropriate.

## CLAUDE.md note

The `CLAUDE.md` file contains instructions for the Claude Code AI assistant that was
used during development. It mixes project documentation with AI-specific directives
(timeout rules, agent delegation, visual verification requirements). For human
contributors, this programmer's overview and the README are the primary references.
