# Linux port + platform refactor — design

Goal: first-class Linux build, with macOS Catalyst and iOS preserved.
No `#ifdef __APPLE__` / `#ifdef __linux__` scattered through VM core
sources after the refactor — all platform divergence lives behind
single-implementation function calls in per-OS files.

## Three rules

  1. **One implementation per platform.** Every function declared in
     `src/platform/Platform.hpp` has exactly ONE `.cpp` definition per
     target OS.  CMake picks the right `.cpp` by `CMAKE_SYSTEM_NAME`.
     If two platforms genuinely share the same code, they share a
     helper `.cpp`, not a `#ifdef` inside the per-OS file.
  2. **VM core never reads platform macros.**  No `#if defined(__APPLE__)`
     or `__linux__` in `src/vm/`.  All it sees is `pharo::platform::*`
     calls and gets the right behavior at link time.
  3. **No new ifdefs in headers either.**  Platform-specific includes
     (`CoreFoundation/`, `pthread.h`, `sys/epoll.h`) live ONLY inside
     per-OS `.cpp` files, never in shared headers.  Headers expose
     opaque handles (`void*` or forward decls) when crossing the
     platform boundary.

## File layout

    src/platform/
      Platform.hpp              -- shared abstract API
      mac.cpp                   -- macOS desktop + Catalyst
      ios.cpp                   -- iOS device & simulator
      linux.cpp                 -- Linux ARM64 / x86_64
      apple_shared.cpp          -- code identical across mac+ios
                                   (CoreAudio, CoreMIDI, CFRunLoop,
                                    pthread W^X flip)
      apple_shared.hpp          -- internal header for apple_shared
                                   helpers (only included from mac.cpp
                                   and ios.cpp)

    src/platform/PlatformBridge.cpp     -- DELETED, contents migrated
    src/platform/iOSBridge.cpp          -- DELETED, contents migrated
    src/platform/EventQueue.cpp         -- KEPT, may grow per-OS .cpp
    src/platform/MotionData.cpp         -- KEPT (already abstracted)

    src/vm/jit/
      PlatformJIT.hpp           -- becomes a thin re-export of
                                   pharo::platform::makeWritable etc.
                                   (instead of its own #ifdef branches)

## API surface (`Platform.hpp`)

Functions grouped by what they do.  Each is a pure C function so the
linker resolves them across translation-unit boundaries cleanly.

### W^X for the JIT code zone

    bool platformInit();             // one-time setup (RWX mmap on Linux,
                                     // MAP_JIT alloc on Apple)
    void* allocCodeZone(size_t bytes);
    void  freeCodeZone(void* ptr, size_t bytes);
    void  makeWritable(void* ptr, size_t bytes);
    void  makeExecutable(void* ptr, size_t bytes);
    void  flushICache(void* ptr, size_t bytes);

  - Apple impl: `pthread_jit_write_protect_np` toggle, MAP_JIT alloc
  - Linux impl: `mmap(RWX)` once, `make{Writable,Executable}` are
    no-ops, `flushICache` uses `__builtin___clear_cache`

### Run loop pump (let host UI thread breathe)

    void platformRelinquishCPU(uint64_t microseconds);

  - Apple impl: `CFRunLoopRunInMode` (drains main-thread events)
  - Linux impl: `usleep(microseconds)`

### FFI / library loading

    void* dlopenLibrary(const char* name);
    void* dlsymSymbol(void* lib, const char* name);
    const char* platformBundlePath();   // app bundle resources dir;
                                        // returns NULL on Linux

  - Apple impl: dlopen + CFBundle resolution
  - Linux impl: dlopen + return NULL bundle path

### ObjC exception guarding (no-op on Linux)

    struct PlatformExceptionScope { ... };  // RAII, defined per-OS

  - Apple impl: @try/@catch wrapper
  - Linux impl: empty struct, optimizes out

### Audio / MIDI

    bool platformAudioInit();
    bool platformMidiInit();

  - Apple impl: AudioQueueServices, CoreMIDI
  - Linux impl: ALSA stubs (start with "audio not implemented" returns)

### System info

    void platformInstallSwizzles();   // Catalyst NSApp/NSMenu fix
    void platformInstallSignalHandlers();
    bool platformIsSandboxed();       // true on iOS device

  - Apple impl: ObjC runtime swizzles + sigaction
  - Linux impl: sigaction only

## CMake selection

In `CMakeLists.txt` the source list expands based on platform:

    set(PLATFORM_SOURCES src/platform/Platform.hpp)
    if(APPLE)
        list(APPEND PLATFORM_SOURCES src/platform/apple_shared.cpp)
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            list(APPEND PLATFORM_SOURCES src/platform/ios.cpp)
        else()
            list(APPEND PLATFORM_SOURCES src/platform/mac.cpp)
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND PLATFORM_SOURCES src/platform/linux.cpp)
    endif()

No more `if(APPLE)` blocks scattered through the link rules — the
per-OS `.cpp` includes whatever frameworks/libs it needs and CMake's
`if(APPLE)` framework block stays as the single canonical link step.

## Migration order

  1. Create empty `Platform.hpp` + `apple_shared.hpp` + the four
     `.cpp` files.  Build still passes (nothing using them yet).
  2. Move W^X functions first (highest perf leverage).  Replace
     `PlatformJIT.hpp`'s `#if defined(__APPLE__)` branches with calls
     to `pharo::platform::makeWritable`.
  3. Move `pthread_jit_write_protect_np` calls scattered in
     `Interpreter.cpp` to the same path.  Now Linux has a no-op flip
     and Mac has the existing flip — same bench numbers on Mac.
  4. Move CFRunLoop pump → `platformRelinquishCPU`.
  5. Move ObjC exception guards → `PlatformExceptionScope`.
  6. Move bundle path / dlopen → platform layer.
  7. Move SoundPlugin / MIDIPlugin bodies into `apple_shared.cpp` +
     stub `linux.cpp`.
  8. Move Catalyst NSApp swizzle into `mac.cpp`.
  9. Add Linux to CMake.  Build `test_load_image`, run SUnit.

Each step is a separate commit.  Mac builds stay green throughout —
verified by xcodebuild after each step.  iOS builds verified at the
end (full xcframework rebuild).

## What this commits to

  - **iOS:** still buildable, code identical to today (just moved into
    `ios.cpp` + `apple_shared.cpp`).  No behavior change.
  - **Mac Catalyst:** still buildable, perf identical to today (W^X
    flips still happen, just behind a function call).
  - **Linux:** new target.  No GUI initially (headless `test_load_image`
    + SUnit only).  GUI deferred until after the bench validates the
    perf hypothesis.
