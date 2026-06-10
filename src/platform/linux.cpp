/*
 * linux.cpp - Linux platform implementations
 *
 * Linux is the second tier-1 target after macOS.  Goals here:
 *
 *   - Use mmap(RWX) once and never call mprotect again.  This makes
 *     makeWritable / makeExecutable no-ops, eliminating the per-call
 *     W^X overhead that costs ~55% of CPU on Mac (per profiling).
 *
 *   - Replace CFRunLoopRunInMode with usleep() — Linux doesn't have
 *     a UI event loop the VM needs to pump.  Future GUI work may
 *     need to integrate with X11/Wayland, but headless and SUnit
 *     don't.
 *
 * Linked into Linux builds only.  Never linked on Apple platforms.
 */

#include "Platform.hpp"
#include "DisplaySurface.hpp"

#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <cstring>

// =====================================================================
// Headless Linux stubs for symbols normally provided by the iOS / Catalyst
// host app (PlatformBridge.cpp).  test_load_image runs entirely without a
// display, clipboard, or IME — these stubs let it link.
// =====================================================================

namespace pharo {
DisplaySurface* gDisplaySurface = nullptr;
}

extern "C" const char* vm_getClipboardText(void) { return ""; }
extern "C" void vm_setClipboardText(const char* /*text*/) {}
extern "C" void vm_startTextInput(void) {}
extern "C" void vm_stopTextInput(void) {}

namespace pharo {
namespace platform {

// ===== JIT code zone (RWX from mmap, no per-call flips needed) =====

void* allocCodeMemory(size_t bytes) {
    void* p = mmap(nullptr, bytes,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
}

void freeCodeMemory(void* ptr, size_t bytes) {
    if (ptr) munmap(ptr, bytes);
}

int currentWXShadowMode() { return 1; }  // RWX platform: never in W-only mode

bool makeWritable(void* /*ptr*/, size_t /*bytes*/) {
    // No-op: pages are already RWX from mmap.  This is the entire
    // win we're chasing — every per-call W^X flip on Mac becomes
    // free here.
    return true;
}

bool makeExecutable(void* /*ptr*/, size_t /*bytes*/) {
    // No-op: see makeWritable.
    return true;
}

void flushICache(void* ptr, size_t bytes) {
    // ARM64 needs explicit cache flush after writing new code.
    // x86_64 has coherent I/D caches but the builtin is safe there.
    __builtin___clear_cache(static_cast<char*>(ptr),
                            static_cast<char*>(ptr) + bytes);
}

// flipJitToWritable / flipJitToExecutable are defined inline in
// Platform.hpp (hot-path: must inline at every call site).

// ===== Stack bounds =====

bool getStackBounds(uint8_t** top, uint8_t** bot) {
    // glibc reports the LOW end (base) + size; HIGH end is base + size.
    pthread_t self = pthread_self();
    pthread_attr_t attr;
    *top = nullptr;
    *bot = nullptr;
    if (pthread_getattr_np(self, &attr) != 0) return false;
    void* base = nullptr;
    size_t size = 0;
    int ok = pthread_attr_getstack(&attr, &base, &size);
    pthread_attr_destroy(&attr);
    if (ok != 0 || !base) return false;
    *bot = static_cast<uint8_t*>(base);
    *top = *bot + size;
    return true;
}

// jitTrampolineJMSize is defined in src/platform/jit_jmsize_{arm64,other}.cpp
// (per-arch file selected by CMake) so this OS-specific file stays free
// of arch ifdefs.

// ===== Cooperative scheduling =====

void relinquishCPU(uint64_t microseconds) {
    // No host UI loop to pump on Linux headless builds.  Future:
    // when a GUI is added, this may need to dispatch X11/Wayland
    // events.
    if (microseconds > 0) {
        usleep(static_cast<useconds_t>(microseconds));
    }
}

// ===== One-time init =====

void platformInit() {
    // Nothing to do yet.  Future: install signal handlers if needed.
}

}  // namespace platform
}  // namespace pharo
