/*
 * apple_shared.cpp - implementation shared between mac.cpp and ios.cpp
 *
 * Code that's BIT-FOR-BIT identical between macOS desktop, macOS
 * Catalyst, and iOS device builds lives here.  Code that diverges
 * (NSApplication swizzles vs sandbox stubs vs ...) lives in mac.cpp
 * or ios.cpp respectively.
 *
 * Linked into both mac and ios builds.  Never linked into linux.
 */

#include "Platform.hpp"

#include <sys/mman.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <CoreFoundation/CoreFoundation.h>

namespace pharo {
namespace platform {

// ===== JIT code zone allocation (MAP_JIT for Apple Silicon W^X) =====

void* allocCodeMemory(size_t bytes) {
    // MAP_JIT is required on Apple Silicon for the per-thread W^X
    // toggle.  PROT_EXEC must be set so pthread_jit_write_protect_np
    // can flip to executable mode without a syscall.
    void* p = mmap(nullptr, bytes,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT,
                   -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
}

void freeCodeMemory(void* ptr, size_t bytes) {
    if (ptr) munmap(ptr, bytes);
}

// ===== W^X toggle (Apple Silicon: per-thread MSR flip) =====
//
// Use the same __asm-renamed declaration as Platform.hpp's inline
// flip helpers — bypasses the SDK availability annotation that
// breaks Catalyst (iOS-SDK-targeted) and iOS device builds.  See
// Platform.hpp for the full rationale.

#if defined(__arm64__)
extern "C" void pharo_pthread_jit_write_protect_np(int)
    __asm("_pthread_jit_write_protect_np");
#endif

// Thread-local shadow of the last mode set through these wrappers
// (1 = executable, 0 = writable).  Accurate because EVERY flip in the
// VM goes through makeWritable/makeExecutable.  Exists so a nested
// write scope (the PMS patcher inside a compile/eviction window) can
// RESTORE the mode it entered with instead of force-flipping to X and
// SIGBUSing the outer window's next emit store.  A depth counter was
// rejected: the codebase has deliberate unmatched defensive force-X
// calls (Interpreter.cpp), so "last call wins" must keep working.
static thread_local int g_wxShadowMode = 1;

int currentWXShadowMode() { return g_wxShadowMode; }

bool makeWritable(void* /*ptr*/, size_t /*bytes*/) {
#if defined(__arm64__)
    pharo_pthread_jit_write_protect_np(0);
    g_wxShadowMode = 0;
    return true;
#else
    // x86_64 macOS: MAP_JIT not required; use page-level mprotect
    // (rare path — Apple Silicon is the production target).
    g_wxShadowMode = 0;
    return true;
#endif
}

bool makeExecutable(void* /*ptr*/, size_t /*bytes*/) {
#if defined(__arm64__)
    pharo_pthread_jit_write_protect_np(1);
    g_wxShadowMode = 1;
    return true;
#else
    g_wxShadowMode = 1;
    return true;
#endif
}

void flushICache(void* ptr, size_t bytes) {
    sys_icache_invalidate(ptr, bytes);
}

// flipJitToWritable / flipJitToExecutable are defined inline in
// Platform.hpp (hot-path: must inline at every call site).

// ===== Stack bounds =====

bool getStackBounds(uint8_t** top, uint8_t** bot) {
    pthread_t self = pthread_self();
    *top = static_cast<uint8_t*>(pthread_get_stackaddr_np(self));
    if (*top == nullptr) {
        *bot = nullptr;
        return false;
    }
    *bot = *top - pthread_get_stacksize_np(self);
    return true;
}

// jitTrampolineJMSize is defined in src/platform/jit_jmsize_{arm64,other}.cpp
// (per-arch file selected by CMake) so this OS-specific file stays free
// of arch ifdefs.

// ===== Cooperative scheduling (CFRunLoop) =====

void relinquishCPU(uint64_t microseconds) {
    // Pump the main run loop so UI events / timers can fire.  The
    // CFTimeInterval is in seconds, so divide by 1e6.
    CFTimeInterval seconds = (CFTimeInterval)microseconds / 1.0e6;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, /*returnAfterSourceHandled=*/false);
}

}  // namespace platform
}  // namespace pharo
