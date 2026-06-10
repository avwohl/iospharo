/*
 * PlatformJIT.hpp - JIT-side W^X helpers (thin wrapper around platform layer)
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * After the platform refactor (2026-04-25): this header is a thin
 * adapter that re-exports pharo::platform::{makeWritable,...} into
 * the pharo::jit:: namespace.  All actual platform divergence lives
 * in src/platform/{mac,ios,linux}.cpp via Platform.hpp.
 *
 * Why keep this header at all?  Existing JIT code uses
 * pharo::jit::makeWritable(...) — keeping the same API avoids
 * touching every call site.  The real implementation just delegates.
 */

#ifndef PHARO_PLATFORM_JIT_HPP
#define PHARO_PLATFORM_JIT_HPP

#include "JITConfig.hpp"
#include "../../platform/Platform.hpp"
#include <cstddef>
#include <cstdint>

#if PHARO_JIT_ENABLED

namespace pharo {
namespace jit {

inline void* allocateCodeMemory(size_t size) {
    return pharo::platform::allocCodeMemory(size);
}

inline void freeCodeMemory(void* ptr, size_t size) {
    pharo::platform::freeCodeMemory(ptr, size);
}

inline bool makeWritable(void* ptr, size_t size) {
    return pharo::platform::makeWritable(ptr, size);
}

inline bool makeExecutable(void* ptr, size_t size) {
    return pharo::platform::makeExecutable(ptr, size);
}

inline void flushICache(void* ptr, size_t size) {
    pharo::platform::flushICache(ptr, size);
}

// RAII guard: writable on construction, executable on destruction.
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

    ScopedWriteAccess(const ScopedWriteAccess&) = delete;
    ScopedWriteAccess& operator=(const ScopedWriteAccess&) = delete;

private:
    void* ptr_;
    size_t size_;
};

// Nesting-safe write scope for the PMS patcher (docs/patched-ic-design.md
// §4.1).  linkSendSite can run INSIDE a compile/eviction window that
// already holds W mode (CodeZone::allocate -> ... -> finalize); a plain
// ScopedWriteAccess destructor would force-flip the thread back to X
// and the outer window's next emit store SIGBUSes.  This scope restores
// the mode it ENTERED with, using the platform shadow (a depth counter
// was rejected — the VM has deliberate unmatched defensive force-X
// calls whose "last call wins" semantics must keep working).  icache
// flushes are the caller's job (ranged, per touched words — never
// whole-zone) and are mode-independent.
class ScopedPatchWriteAccess {
public:
    explicit ScopedPatchWriteAccess(void* ptr, size_t size)
        : enteredExecutable_(pharo::platform::currentWXShadowMode() == 1)
    {
        if (enteredExecutable_) makeWritable(ptr, size);
    }

    ~ScopedPatchWriteAccess() {
        if (enteredExecutable_) makeExecutable(nullptr, 0);
    }

    ScopedPatchWriteAccess(const ScopedPatchWriteAccess&) = delete;
    ScopedPatchWriteAccess& operator=(const ScopedPatchWriteAccess&) = delete;

private:
    bool enteredExecutable_;
};

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_PLATFORM_JIT_HPP
