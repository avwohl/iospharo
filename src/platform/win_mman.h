/*
 * win_mman.h - minimal <sys/mman.h> replacement over Win32 virtual memory,
 * for the Windows (clang/LLVM-MinGW) build.  Covers exactly what
 * ObjectMemory.cpp uses: an anonymous private read/write reservation plus
 * MADV_DONTNEED to drop physical pages of the trailing free gap.
 *
 * VirtualAlloc/VirtualFree are declared here directly (rather than via
 * <windows.h>) so this header can be pulled into the large ObjectMemory.cpp
 * TU without dragging in windows.h's macro soup (min/max/ERROR/...).
 *
 * Commit policy (milestone 1): MEM_RESERVE|MEM_COMMIT the whole old-space
 * region up-front.  Windows commit is demand-zero — committed pages consume
 * pagefile *commit charge* but no physical RAM until first touched, so this
 * preserves the Linux lazy-RSS behavior the ObjectMemory comment relies on;
 * only the commit limit (RAM + pagefile) must cover the reservation.  A
 * later milestone can switch to MEM_RESERVE + a fault-driven commit handler
 * to lower the commit charge.  See docs/windows-port-plan.md.
 */

#ifndef PHARO_WIN_MMAN_H
#define PHARO_WIN_MMAN_H

#ifdef _WIN32

#include <cstddef>
#include <cstdint>

// Win32 VirtualAlloc/VirtualFree, declared locally to avoid <windows.h>.
extern "C" __declspec(dllimport) void* __stdcall
    VirtualAlloc(void* lpAddress, size_t dwSize,
                 unsigned long flAllocationType, unsigned long flProtect);
extern "C" __declspec(dllimport) int __stdcall
    VirtualFree(void* lpAddress, size_t dwSize, unsigned long dwFreeType);
extern "C" __declspec(dllimport) int __stdcall
    VirtualProtect(void* lpAddress, size_t dwSize,
                   unsigned long flNewProtect, unsigned long* lpflOldProtect);

// Allocation-type / protection / free-type constants (from memoryapi.h).
#define PHARO_MEM_COMMIT   0x00001000UL
#define PHARO_MEM_RESERVE  0x00002000UL
#define PHARO_MEM_RESET    0x00080000UL
#define PHARO_MEM_DECOMMIT 0x00004000UL
#define PHARO_MEM_RELEASE  0x00008000UL
#define PHARO_PAGE_NOACCESS            0x01UL
#define PHARO_PAGE_READONLY            0x02UL
#define PHARO_PAGE_READWRITE           0x04UL
#define PHARO_PAGE_EXECUTE_READWRITE   0x40UL

// POSIX mmap/madvise constants the call sites reference.
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define MAP_ANON       MAP_ANONYMOUS
#define MAP_FAILED     (reinterpret_cast<void*>(-1))
#define MADV_DONTNEED  4
#define MADV_NORMAL    0

static inline void* mmap(void* /*addr*/, size_t len, int prot, int /*flags*/,
                         int /*fd*/, long /*offset*/) {
    unsigned long protect =
        (prot & PROT_EXEC)  ? PHARO_PAGE_EXECUTE_READWRITE :
        (prot & PROT_WRITE) ? PHARO_PAGE_READWRITE :
        (prot & PROT_READ)  ? PHARO_PAGE_READONLY : PHARO_PAGE_READWRITE;
    void* p = VirtualAlloc(nullptr, len,
                           PHARO_MEM_RESERVE | PHARO_MEM_COMMIT, protect);
    return p ? p : MAP_FAILED;
}

static inline int munmap(void* addr, size_t /*len*/) {
    // MEM_RELEASE requires dwSize == 0 and frees the whole reservation.
    return VirtualFree(addr, 0, PHARO_MEM_RELEASE) ? 0 : -1;
}

static inline int madvise(void* addr, size_t len, int advice) {
    if (advice == MADV_DONTNEED) {
        // CRITICAL: MADV_DONTNEED on an anonymous mapping discards the pages AND
        // zero-fills them on next access.  The VM's bump allocator relies on
        // that zero-fill to cheaply nil-initialize new objects in the reclaimed
        // free gap.  MEM_RESET does NOT zero — it leaves page content UNDEFINED
        // on re-access, which gave new objects garbage slots (corrupt class
        // indices -> DNU -> crash).  Decommit + recommit instead: it drops the
        // physical pages (the RSS win) and guarantees zero-filled pages on next
        // touch, matching MADV_DONTNEED exactly.  Page-align so we never
        // decommit a partially-live page.
        const uintptr_t pg = 4096;
        uintptr_t lo = (reinterpret_cast<uintptr_t>(addr) + (pg - 1)) & ~(pg - 1);
        uintptr_t hi = (reinterpret_cast<uintptr_t>(addr) + len) & ~(pg - 1);
        if (hi > lo) {
            void* p = reinterpret_cast<void*>(lo);
            size_t n = static_cast<size_t>(hi - lo);
            if (VirtualFree(p, n, PHARO_MEM_DECOMMIT)) {
                VirtualAlloc(p, n, PHARO_MEM_COMMIT, PHARO_PAGE_READWRITE);
            }
        }
    }
    return 0;
}

#endif  // _WIN32
#endif  // PHARO_WIN_MMAN_H
