/*
 * PlatformJIT.hpp - Platform abstraction for JIT memory management
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Provides cross-platform wrappers for:
 * - Allocating executable memory (mmap / VirtualAlloc)
 * - W^X transitions (write->execute, execute->write)
 * - Instruction cache flushing
 * - Thread-safe JIT writes on Apple Silicon (MAP_JIT + pthread_jit_write_protect)
 */

#ifndef PHARO_PLATFORM_JIT_HPP
#define PHARO_PLATFORM_JIT_HPP

#include "JITConfig.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

#if PHARO_JIT_ENABLED

// Platform headers
#if defined(__APPLE__)
  #include <sys/mman.h>
  #include <unistd.h>
  #include <libkern/OSCacheControl.h>
  #include <pthread.h>
#elif defined(__linux__)
  #include <sys/mman.h>
  #include <unistd.h>
#elif defined(_WIN32)
  #include <windows.h>
#endif

namespace pharo {
namespace jit {

// ===== MEMORY ALLOCATION =====

// Allocate a region suitable for JIT code. Returns nullptr on failure.
// The region starts writable (not executable). Use makeExecutable() after
// writing code.
inline void* allocateCodeMemory(size_t size) {
#if defined(__APPLE__)
    // MAP_JIT is required on Apple Silicon for W^X per-thread toggling.
    // PROT_EXEC must be included so pthread_jit_write_protect_np can
    // toggle between write and execute modes. Without it, the memory
    // never becomes executable.
    void* p = mmap(nullptr, size,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT,
                   -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;

#elif defined(__linux__)
    void* p = mmap(nullptr, size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;

#elif defined(_WIN32)
    return VirtualAlloc(nullptr, size,
                        MEM_COMMIT | MEM_RESERVE,
                        PAGE_READWRITE);
#else
    (void)size;
    return nullptr;
#endif
}

// Free a code memory region.
inline void freeCodeMemory(void* ptr, size_t size) {
#if defined(__APPLE__) || defined(__linux__)
    if (ptr) munmap(ptr, size);
#elif defined(_WIN32)
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
    (void)size;
#else
    (void)ptr; (void)size;
#endif
}

// ===== W^X PROTECTION =====
//
// On Apple Silicon, MAP_JIT regions use per-thread W^X toggling via
// pthread_jit_write_protect_np(). This is fast (no syscall, just a
// register write) and doesn't affect other threads.
//
// On other platforms, we use mprotect/VirtualProtect which changes
// protection for all threads and requires a syscall.

// Make a region writable (for code generation). On Apple Silicon this
// is a per-thread toggle; the region stays executable for other threads.
inline bool makeWritable(void* ptr, size_t size) {
#if defined(__APPLE__) && defined(__arm64__)
    // Per-thread W^X toggle (Apple Silicon)
    pthread_jit_write_protect_np(0);  // 0 = writable
    (void)ptr; (void)size;
    return true;

#elif defined(__APPLE__) || defined(__linux__)
    return mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0;

#elif defined(_WIN32)
    DWORD oldProtect;
    return VirtualProtect(ptr, size, PAGE_READWRITE, &oldProtect) != 0;

#else
    (void)ptr; (void)size;
    return false;
#endif
}

// Make a region executable (after code generation is complete).
inline bool makeExecutable(void* ptr, size_t size) {
#if defined(__APPLE__) && defined(__arm64__)
    // Per-thread W^X toggle (Apple Silicon)
    pthread_jit_write_protect_np(1);  // 1 = executable
    (void)ptr; (void)size;
    return true;

#elif defined(__APPLE__) || defined(__linux__)
    return mprotect(ptr, size, PROT_READ | PROT_EXEC) == 0;

#elif defined(_WIN32)
    DWORD oldProtect;
    return VirtualProtect(ptr, size, PAGE_EXECUTE_READ, &oldProtect) != 0;

#else
    (void)ptr; (void)size;
    return false;
#endif
}

// ===== INSTRUCTION CACHE =====

// Flush the instruction cache for a range of addresses.
// Required after writing machine code before executing it.
// On x86_64 this is a no-op (coherent I/D caches).
// On ARM64 this is mandatory.
inline void flushICache(void* ptr, size_t size) {
#if defined(__APPLE__)
    sys_icache_invalidate(ptr, size);

#elif defined(__aarch64__) && defined(__linux__)
    // GCC/Clang builtin for ARM64 Linux
    __builtin___clear_cache(static_cast<char*>(ptr),
                            static_cast<char*>(ptr) + size);

#elif defined(_WIN32)
    FlushInstructionCache(GetCurrentProcess(), ptr, size);

#else
    // x86_64 Linux: I-cache is coherent, no flush needed.
    // But __builtin___clear_cache is always safe to call.
    __builtin___clear_cache(static_cast<char*>(ptr),
                            static_cast<char*>(ptr) + size);
#endif
}

// ===== SCOPED W^X HELPER =====
//
// RAII guard that makes the code zone writable on construction and
// executable on destruction. Use this when generating or patching code.

class ScopedWriteAccess {
public:
    ScopedWriteAccess(void* ptr, size_t size)
        : ptr_(ptr), size_(size)
    {
        makeWritable(ptr_, size_);
    }

    ~ScopedWriteAccess() {
        flushICache(ptr_, size_);
        makeExecutable(ptr_, size_);
    }

    // Non-copyable, non-movable
    ScopedWriteAccess(const ScopedWriteAccess&) = delete;
    ScopedWriteAccess& operator=(const ScopedWriteAccess&) = delete;

private:
    void* ptr_;
    size_t size_;
};

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_PLATFORM_JIT_HPP
