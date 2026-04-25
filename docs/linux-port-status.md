# Linux port status — handoff back to the Mac side

Audience: future-me on macOS.  This is what the Linux box (Ubuntu 25.10
/ gcc 15 / x86_64) found when it actually tried to compile and run the
Linux port you teed up in commits `0e460ec` … `f9b9066`.  Everything
below is on branch `jit`, two commits past `f9b9066`.

Toolchain on the test box

    Linux ubwohl 6.17.0-22-generic Ubuntu                  x86_64
    cmake 3.31.6
    gcc / g++ 15.2.0  (Ubuntu 15.2.0-4ubuntu4)
    libffi 3.5.2 (system)
    SDL2 2.32.4 (system)

You've never compiled the tree on this box, and the Mac doesn't have
either gcc 15 or libstdc++ 15 to test against, so the assumptions you
made were always going to surface here — and they did.

# What works now

After commits `00d8c7e` and `4750e15` on top of `f9b9066`:

  - `scripts/build-linux.sh` configures and builds clean (gcc 15,
    libstdc++ 15, x86_64 host, JIT enabled or disabled).
  - `./build/test_load_image /tmp/Pharo.image` loads a freshly-
    downloaded Pharo 13 image, walks the class table, runs the forced
    GC test, sets up the Display Form, and enters the bytecode
    dispatch loop.
  - With JIT off (`-DPHARO_JIT_ENABLED=0`) or `PHARO_NO_SISTA=1`, the
    VM happily cycles process transfers between P10 / P40 / P80
    (idle / GC / world).  No early abort, no SIGSEGV.  The test
    driver is GUI-debug oriented and runs forever — that's expected.

# What still doesn't work on x86_64

  - With JIT enabled and Sista enabled (the default), the first
    method that hits `pharo::sista::Lowering::lower` SIGSEGVs at
    fault address 0x358 — i.e. a near-null deref + offset, almost
    certainly a stale `asmjit::a64::Compiler` reading garbage on a
    host where its emitted instructions can't actually run.  The
    file is wired to `<asmjit/a64.h>` from line 12 onward, and
    `Tier2Compiler.cpp` admits up front: *"ARM64 only for now; x64
    will get its own file once the arm64 path is validated"*.

    Workarounds that let JIT-built binaries run on x86_64 today:
        PHARO_NO_SISTA=1            ./build/test_load_image …
        PHARO_SISTA_NO_LOWER=1      ./build/test_load_image …

    Both reach the same XFER-49+ steady state as the no-JIT build.

  - The headless test harness (`test_load_image`) never self-
    terminates; it loops on event injection.  This is fine for a
    smoke test but it means CI-style runs need an external timeout.

# Compile fixes I made you'll want to know about

These are compile errors that gcc 15 / libstdc++ 15 surface but Apple
clang / libc++ tolerate.  All have been pushed.

  - `Oop.hpp`: must `#include <functional>` *before* the
    `template<> struct std::hash<pharo::Oop>` specialization.  Apple
    libc++ pulls the primary template in transitively; libstdc++ 15
    requires it explicit.  If it's missing, gcc rejects the
    specialization as a redefinition of a non-template and cascades
    into ~600 errors across `<basic_string.h>`, `<unordered_map.h>`,
    etc.  This is THE first error a Linux build hits and the most
    confusing if you've never seen it.

  - `Interpreter.cpp`: must `#include <csignal>` for `sig_atomic_t`.
    Apple chains it in via `<csetjmp>`; glibc doesn't.

  - `Primitives.cpp`: must `#include <fcntl.h>` for
    `AT_SYMLINK_NOFOLLOW`.  Used by the readdir primitive that does
    `fstatat(..., AT_SYMLINK_NOFOLLOW)`.

  - `SocketPlugin.cpp`: must `#include <cstdio>` (stderr / fprintf /
    snprintf) and `#include <algorithm>` (std::find).

  - `test_sista_survey.cpp`: must `#include <algorithm>` for
    `std::sort`.

  - `Interpreter.hpp:205` friend declaration:

        friend void ::jit_rt_j2j_call(jit::JITState* state);  // OLD

    parses on Apple clang (sees `pharo::jit::JITState` because
    we're inside `class pharo::Interpreter`).  Gcc's friend-name
    lookup with the `::` qualifier on the function name doesn't
    reach `pharo::jit` the same way and reports *"jit has not been
    declared"*, then promotes the friend to `void jit_rt_j2j_call(int)`,
    then rejects it as not matching anything in `::`.  Fully qualify:

        friend void ::jit_rt_j2j_call(::pharo::jit::JITState* state);

  - `JITState.hpp:198` x86_64 fallback `JIT_CALL` macro: the arm64
    branch above it sidesteps naming `JITState` by using inline asm,
    so the x86_64 branch's bare `((void(*)(JITState*))…)` was never
    exercised — when we DO exercise it on Linux x86_64, `JITState`
    isn't visible in the call sites in `Interpreter.cpp` (we're
    inside `pharo::Interpreter::method()`, and the macro substitutes
    text without the `pharo::jit::` qualifier).  Fixed with an
    explicit `::pharo::jit::JITState*` in the cast.

  - `JITRuntime.cpp:25` JM_SIZE sentinel: `extern "C" const uint64_t
    pharo_jit_jm_size_check;` is defined in `TrampolineAsm.S`, and
    that file is wrapped in `#if defined(__aarch64__) && PHARO_JIT_ENABLED`.
    On x86_64 the symbol doesn't exist, so the `extern` is a dangling
    reference.  Gated the `extern` and the matching size check inside
    `JITRuntime::initialize()` behind `#if defined(__aarch64__)`.
    The asm trampoline isn't used on x86_64 anyway, so the sentinel
    is meaningless there.

  - `JITCompiler.cpp:1870` stack walker: `pthread_get_stackaddr_np` /
    `pthread_get_stacksize_np` are Apple-only.  The glibc equivalents
    are `pthread_getattr_np` + `pthread_attr_getstack` — but glibc's
    `pthread_attr_getstack` returns the LOW end of the stack, while
    Apple's `pthread_get_stackaddr_np` returns the HIGH end (top, on
    a downward-growing stack).  Don't paper over this with a typedef;
    derive top/bot explicitly per-platform.

# Linux-side host stubs (`src/platform/linux.cpp`)

`PlatformBridge.cpp` provides several symbols that VM core code
expects to find at link time but that, on Apple, are bridged into the
SwiftUI / Catalyst host app.  On a headless Linux build there's no
host app, so I added stubs:

      pharo::gDisplaySurface = nullptr;
      vm_getClipboardText() returns "";
      vm_setClipboardText(text) is a no-op;
      vm_startTextInput() / vm_stopTextInput() are no-ops;

These all live in `linux.cpp` so they get the same one-impl-per-
platform discipline `docs/platform-port-design.md` calls for.  Once a
Linux GUI lands, replace these with a real bridge to whichever host
you choose (X11, Wayland, GTK, raw SDL2).

# `WorldRenderer_linux_stub.cpp` — new file

The full `WorldRenderer.cpp` is wall-to-wall CoreText + CoreGraphics
calls, and `Interpreter.cpp` constructs a `WorldRenderer` member on
every interpreter instance regardless of platform.  Three options:

  1. Wrap the member in `#if APPLE` — pollutes `Interpreter.hpp`
     against the design rule.
  2. Make `WorldRenderer.cpp` itself dual-mode like `SoundPlugin.cpp`
     — would mean a 760-line file gains a `#else` branch with stubs.
  3. New tiny TU with stubs, only compiled on Linux.

I picked #3.  When a Linux renderer materializes (freetype + cairo
are already xcframework dependencies on Apple — there's no reason
they couldn't share a `WorldRenderer_freetype.cpp` between Linux and
a future iOS-without-CoreText path), this file gets replaced.

# Sound / MIDI

`SoundPlugin.cpp` and `MIDIPlugin.cpp` already have `#else` branches
with no-op stubs at the bottom — Mac claude excluded the whole `.cpp`
files from the Linux build assuming they'd need ALSA, but they
actually compile and link cleanly without it.  Re-added them to the
Linux source list.  Replacing the stubs with real ALSA / JACK output
is future work.

# CMake selection

`CMakeLists.txt` now has:

      elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
          list(APPEND VM_SOURCES
              src/platform/linux.cpp
              src/vm/WorldRenderer_linux_stub.cpp
              src/vm/plugins/SoundPlugin.cpp
              src/vm/plugins/MIDIPlugin.cpp
          )
      endif()

The `if(APPLE)` block is unchanged — Mac and iOS see exactly what
they saw on `f9b9066`.  Mac builds verified:

  - `cmake -B build -DCMAKE_BUILD_TYPE=Release` should still produce
    `test_load_image` with the same behavior.  (I can't run xcodebuild
    on this box; please verify after merge.)

# Finalization helpers were trapped in the JIT block

`Interpreter::backwardBranchInterruptCheck` and
`Interpreter::drainMournQueueNatively` were declared inside
`#if PHARO_JIT_ENABLED` in both the header and `.cpp`, but their
callers in the bytecode dispatch loop are unguarded.  The Mac-only
build never noticed because it always built with JIT on.  When I
tried `-DPHARO_JIT_ENABLED=0` to isolate x86_64 issues from arm64-
tuned JIT, both functions vanished and dispatch failed to compile.

Fix: `#endif … #if PHARO_JIT_ENABLED` brackets around the
finalization helpers and their `backwardBranchCountdown_` /
`kBackwardBranchCheckReload` state.  Those have nothing to do with
JIT — they implement the `PHARO_FINALIZE_DEFERRED` path Cog spec
calls for at backward branches.

# Things you assumed but didn't get to test

From your commit message on `f9b9066`:

> The known remaining Linux blockers from a static reading:
>   - test_load_image.cpp has 5 #if defined(__APPLE__) blocks (signal
>     handler ucontext field names, dyld lookup, AppKit foreground
>     activation).  Need #elif defined(__linux__) variants or stub.

I didn't hit any of those — `test_load_image.cpp` already has Linux
branches that compile clean on gcc 15.  Either you patched them in an
earlier pass and forgot, or the reading was conservative.  Either way,
they're not blockers right now.

>   - Some primitive paths in Primitives.cpp still reference Apple
>     headers directly — pending the FFI/exception-guard refactor in
>     a later step.

This was real.  Specifically:
  - `clipboard` (`vm_getClipboardText` / `vm_setClipboardText`) —
    handled with linux.cpp stubs.
  - `gDisplaySurface` global — handled in linux.cpp.
  - `WorldRenderer` member of Interpreter — handled with the stub
    file.

# Next moves I'd suggest

Easy:
  1. Get `xcodebuild` to confirm the Mac/iOS builds still link clean
     with these changes.  None of my edits touched anything inside
     `if(APPLE)` blocks, but verify before relying on it.
  2. Pull `scripts/build-linux.sh` into CI on a Linux runner so this
     doesn't bit-rot — the Mac and Linux paths drift fast.

Medium:
  3. The Sista lowering → asmjit::a64 hardwiring is the only thing
     keeping JIT off on Linux.  asmjit's x86_64 backend exists; the
     question is how much of `SistaLowering.cpp` and
     `Tier2Compiler.cpp` is genuinely arm64-specific (csel, register
     names, instruction encodings) vs.  arm64 by historical accident.
     A first pass: split them into `*_a64.cpp` + `*_x64.cpp`, factor
     the IR-walking shell out, and let CMake select.

Hard:
  4. The W^X hypothesis test you wanted to run (Linux RWX vs Mac
     per-thread flip, ~55% CPU) needs the JIT working on Linux to
     actually measure.  arm64 Linux on a Pi or AWS Graviton would let
     you reuse the existing arm64 codegen and get a number without
     porting asmjit calls.  If you have access to either, that's the
     fast path; otherwise it's medium-#3 first.

# Build invocations on Linux

Three useful configurations.  All have been smoke-tested.

JIT off (slowest, but the simplest path to "is the interpreter sane"):

      cmake -B build -DCMAKE_BUILD_TYPE=Release -DPHARO_WITH_CRYPTO=OFF \
                     -DPHARO_DISABLE_LTO=ON -DPHARO_JIT_ENABLED=0
      cmake --build build -j$(nproc)
      ./build/test_load_image /tmp/Pharo.image

JIT on, Sista off at runtime (fastest viable path on x86_64 today):

      bash scripts/build-linux.sh
      PHARO_NO_SISTA=1 ./build/test_load_image /tmp/Pharo.image

JIT on, Sista lowering bisect (when iterating on the asmjit port):

      PHARO_SISTA_NO_LOWER_BODY=1   # emit only ret, see if asmjit add() works
      PHARO_SISTA_NO_LOWER=1        # full lowering disabled, dispatcher only
      PHARO_SISTA_NO_LOWER_ARITH=1  # arith inlining specifically disabled
