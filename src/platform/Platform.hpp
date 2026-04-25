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
