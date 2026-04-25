/*
 * Platform.hpp - cross-platform abstraction layer
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * This is the SINGLE entry point through which VM core sources reach
 * platform-specific behavior.  No file under src/vm/ should #include
 * platform headers (CoreFoundation, pthread, sys/epoll, ...) directly;
 * everything goes through these declarations.
 *
 * Each function declared here has exactly ONE definition per target
 * OS, in src/platform/{mac,ios,linux}.cpp (with code shared between
 * mac+ios in src/platform/apple_shared.cpp).  CMake selects the right
 * .cpp at build time based on CMAKE_SYSTEM_NAME.
 *
 * Adding a new function:
 *   1. Declare it here (in pharo::platform namespace).
 *   2. Implement it in mac.cpp + ios.cpp + linux.cpp (or in
 *      apple_shared.cpp if mac+ios share the impl).
 *   3. Call it from VM core code.  No #ifdef in the call site.
 */

#ifndef PHARO_PLATFORM_HPP
#define PHARO_PLATFORM_HPP

#include <cstddef>
#include <cstdint>

namespace pharo {
namespace platform {

// =====================================================================
// W^X memory management (JIT code zone)
// =====================================================================

// Allocate a region suitable for JIT code.  On Apple Silicon this
// uses MAP_JIT (per-thread W^X toggle).  On Linux this uses
// mmap(RWX) — no per-call flips needed afterward.  Returns nullptr
// on failure.
void* allocCodeMemory(size_t bytes);

// Release a region allocated via allocCodeMemory.
void freeCodeMemory(void* ptr, size_t bytes);

// Toggle the calling thread's view of [ptr, ptr+bytes) to writable.
//
// On Apple Silicon (MAP_JIT): per-thread MSR flip via
//   pthread_jit_write_protect_np(0).  Other threads still see the
//   region as executable.  ~24 cycles.
//
// On Linux: mmap'd RWX once at allocation time; this is a NO-OP.
// The 12ms-per-block(500K)-bench overhead measured on Mac
// disappears entirely.
//
// On Windows: VirtualProtect to PAGE_READWRITE (all threads).
bool makeWritable(void* ptr, size_t bytes);

// Toggle the calling thread's view of [ptr, ptr+bytes) to executable.
// Mirror of makeWritable — see notes there.
bool makeExecutable(void* ptr, size_t bytes);

// Flush instruction cache for a range.  Required on ARM64 after
// writing new code before executing it.  No-op on x86_64.
void flushICache(void* ptr, size_t bytes);

// Per-thread W^X mode toggles WITHOUT a specific region argument.
// On Apple Silicon these flip the per-thread MSR (region-independent);
// on Linux they are no-ops.  Use these from VM hot-path sites
// (Interpreter.cpp post-stencil) where the "region" concept doesn't
// apply naturally — the JIT zone is global and the flip affects all
// of it for this thread.
//
// Defined inline here (rather than as ordinary function calls in the
// per-OS .cpp) because they're called from the JIT hot path — 13
// sites in Interpreter.cpp, plus JIT_CALL_PRE in JITState.hpp.
// Indirect calls add ~5ns per site × 1.5M iterations of block(500K)
// bench = ~7ms regression vs inline.  This is the ONE place a
// platform #ifdef lives in a header — all VM-core sources stay clean.
}  // namespace platform
}  // namespace pharo

#if defined(__APPLE__) && defined(__arm64__)
  #include <pthread.h>
  namespace pharo { namespace platform {
    inline void flipJitToWritable()   { pthread_jit_write_protect_np(0); }
    inline void flipJitToExecutable() { pthread_jit_write_protect_np(1); }
  }}
#else
  namespace pharo { namespace platform {
    inline void flipJitToWritable()   { /* no-op: RWX on Linux, etc. */ }
    inline void flipJitToExecutable() { /* no-op */ }
  }}
#endif

namespace pharo {
namespace platform {

// (Placeholder so the closing braces below stay balanced — the inline
// flip functions are defined above, outside this namespace block.)

// =====================================================================
// Cooperative scheduling (let host UI breathe)
// =====================================================================

// Yield CPU for approximately `microseconds`.  On macOS/iOS this
// pumps CFRunLoopRunInMode(kCFRunLoopDefaultMode, ...) so the main
// thread can dispatch UI events.  On Linux this is just usleep().
void relinquishCPU(uint64_t microseconds);

// =====================================================================
// One-time initialization
// =====================================================================

// Called once during VM startup.  Apple impl installs ObjC exception
// handler + Catalyst NSApp/NSMenu swizzles.  Linux impl is currently
// a no-op.
void platformInit();

}  // namespace platform
}  // namespace pharo

#endif  // PHARO_PLATFORM_HPP
