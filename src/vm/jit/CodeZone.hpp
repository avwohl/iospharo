/*
 * CodeZone.hpp - Machine code zone for JIT-compiled methods
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Manages a contiguous region of executable memory where JIT-compiled
 * methods live. Based on Cog's machine code zone design:
 *
 * - Fixed-size mmap'd region (default 16 MB)
 * - Bump-pointer allocation (fast, no fragmentation until eviction)
 * - LRU eviction when zone fills up
 * - Linked list of methods for iteration and compaction
 *
 * ZONE LAYOUT:
 *
 *     zoneStart_                                              zoneEnd_
 *     |                                                       |
 *     v                                                       v
 *     +----------+----------+----------+--- - - ---+----------+
 *     | Method 1 | Method 2 | Method 3 |  (free)   |          |
 *     +----------+----------+----------+--- - - ---+----------+
 *                                       ^
 *                                       |
 *                                       freePtr_ (bump allocator)
 *
 * LIFECYCLE:
 *
 *     1. initialize(size) — mmap the zone
 *     2. allocate(codeSize, numIC) — bump-allocate a JITMethod
 *     3. ... write machine code into the allocated region ...
 *     4. finalize(method) — flush icache, mark executable
 *     5. When full: evictLRU() frees cold methods, compact() slides survivors
 *     6. destroy() — munmap
 */

#ifndef PHARO_CODE_ZONE_HPP
#define PHARO_CODE_ZONE_HPP

#include "JITConfig.hpp"
#include "JITMethod.hpp"
#include "PlatformJIT.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

#if PHARO_JIT_ENABLED

namespace pharo {
namespace jit {

class CodeZone {
public:
    CodeZone() = default;
    ~CodeZone() { destroy(); }

    // Non-copyable
    CodeZone(const CodeZone&) = delete;
    CodeZone& operator=(const CodeZone&) = delete;

    // ===== LIFECYCLE =====

    // Allocate and initialize the code zone. Returns false on failure.
    bool initialize(size_t size = DefaultCodeZoneSize) {
        if (zoneStart_) return false;  // Already initialized

        // Clamp to valid range
        if (size < MinCodeZoneSize) size = MinCodeZoneSize;
        if (size > MaxCodeZoneSize) size = MaxCodeZoneSize;

        // Round up to page boundary
        size = (size + PageSize - 1) & ~(PageSize - 1);

        void* mem = allocateCodeMemory(size);
        if (!mem) return false;

        zoneStart_ = static_cast<uint8_t*>(mem);
        zoneEnd_ = zoneStart_ + size;
        freePtr_ = zoneStart_;
        zoneSize_ = size;
        methodCount_ = 0;
        bytesUsed_ = 0;
        epoch_ = 0;
        firstMethod_ = nullptr;
        lastMethod_ = nullptr;

        return true;
    }

    // Free the entire code zone.
    void destroy() {
        if (zoneStart_) {
            freeCodeMemory(zoneStart_, zoneSize_);
            zoneStart_ = nullptr;
            zoneEnd_ = nullptr;
            freePtr_ = nullptr;
            zoneSize_ = 0;
            methodCount_ = 0;
            bytesUsed_ = 0;
            firstMethod_ = nullptr;
            lastMethod_ = nullptr;
        }
    }

    bool isInitialized() const { return zoneStart_ != nullptr; }

    // ===== ALLOCATION =====

    // Allocate space for a JIT method with the given code size and number
    // of inline cache entries. Returns nullptr if the zone is full
    // (caller should trigger eviction and retry).
    //
    // The returned JITMethod has its geometry fields set but the code
    // area is zeroed. Caller must:
    //   1. Fill in the header fields (compiledMethodOop, selector, etc.)
    //   2. Write machine code to method->codeStart()
    //   3. Call finalize(method) when done
    JITMethod* allocate(uint32_t codeSize, uint16_t numICEntries) {
        size_t icSize = numICEntries * sizeof(ICEntry);
        size_t totalSize = sizeof(JITMethod) + codeSize + icSize;

        // Align to MethodAlignment
        totalSize = (totalSize + MethodAlignment - 1) & ~(MethodAlignment - 1);

        if (freePtr_ + totalSize > zoneEnd_) {
            return nullptr;  // Zone full
        }

        // Bump-allocate
        JITMethod* method = reinterpret_cast<JITMethod*>(freePtr_);
        freePtr_ += totalSize;
        bytesUsed_ += totalSize;
        methodCount_++;

        // Zero the entire allocation
        std::memset(method, 0, totalSize);

        // Set geometry
        method->codeSize = codeSize;
        method->numICEntries = numICEntries;
        method->totalSize = static_cast<uint32_t>(totalSize);
        method->state = MethodState::Interpreted;  // Not yet executable
        method->lastUsedEpoch = epoch_;

        // Initialize IC entries
        for (uint16_t i = 0; i < numICEntries; i++) {
            method->icEntries()[i].reset();
        }

        // Link into the method list
        method->prevInZone = lastMethod_;
        method->nextInZone = nullptr;
        if (lastMethod_) {
            lastMethod_->nextInZone = method;
        } else {
            firstMethod_ = method;
        }
        lastMethod_ = method;

        return method;
    }

    // Finalize a method after code generation: flush icache and mark executable.
    // Call this after writing all machine code and IC entries.
    bool finalize(JITMethod* method) {
        if (!method || !contains(method)) return false;

        // Flush icache for the code region
        flushICache(method->codeStart(), method->codeSize);

        // Mark as compiled
        method->state = MethodState::Compiled;

        return true;
    }

    // ===== EVICTION =====

    // Increment the global epoch counter. Call this periodically
    // (e.g., every N method compilations or every GC cycle).
    void advanceEpoch() { epoch_++; }

    // Touch a method (update its LRU epoch). Call on each entry.
    void touch(JITMethod* method) {
        method->lastUsedEpoch = epoch_;
    }

    // Evict the oldest methods until at least `bytesNeeded` are free.
    // Returns the number of methods evicted.
    size_t evictLRU(size_t bytesNeeded) {
        size_t freed = 0;
        size_t evicted = 0;

        // Find the coldest method (lowest lastUsedEpoch)
        // Simple strategy: scan the list and invalidate methods below a threshold.
        // A more sophisticated approach would sort by epoch, but for 16 MB
        // with typical method sizes of 200-2000 bytes, linear scan is fine.

        uint32_t threshold = epoch_ > 10 ? epoch_ - 10 : 0;

        JITMethod* m = firstMethod_;
        while (m && freed < bytesNeeded) {
            JITMethod* next = m->nextInZone;
            if (m->state == MethodState::Compiled && m->lastUsedEpoch < threshold) {
                m->invalidate();
                freed += m->allocationSize();
                evicted++;
            }
            m = next;
        }

        // If we couldn't free enough with the threshold, lower it
        if (freed < bytesNeeded) {
            m = firstMethod_;
            while (m && freed < bytesNeeded) {
                JITMethod* next = m->nextInZone;
                if (m->state == MethodState::Compiled) {
                    m->invalidate();
                    freed += m->allocationSize();
                    evicted++;
                }
                m = next;
            }
        }

        return evicted;
    }

    // Compact the zone by sliding live (Compiled) methods toward the start,
    // eliminating gaps from invalidated/evicted methods. This requires
    // updating all pointers to compiled code (in the MethodMap and IC entries).
    //
    // Returns the number of bytes reclaimed.
    //
    // NOTE: This is expensive and should only be called when eviction alone
    // doesn't free enough space (highly fragmented zone).
    size_t compact() {
        if (!firstMethod_) return 0;

        // Zone is kept in writable mode by default, so no W^X toggle needed.

        uint8_t* dest = zoneStart_;
        JITMethod* prev = nullptr;
        JITMethod* m = firstMethod_;
        size_t reclaimed = 0;

        firstMethod_ = nullptr;
        lastMethod_ = nullptr;
        methodCount_ = 0;

        while (m) {
            JITMethod* next = m->nextInZone;
            size_t mSize = m->allocationSize();

            if (m->state == MethodState::Compiled) {
                // Keep this method — slide it down if there's a gap
                uint8_t* src = reinterpret_cast<uint8_t*>(m);
                if (dest != src) {
                    std::memmove(dest, src, mSize);
                }

                JITMethod* moved = reinterpret_cast<JITMethod*>(dest);
                moved->prevInZone = prev;
                moved->nextInZone = nullptr;

                if (prev) {
                    prev->nextInZone = moved;
                } else {
                    firstMethod_ = moved;
                }
                lastMethod_ = moved;
                prev = moved;
                methodCount_++;

                dest += mSize;
            } else {
                // Invalidated/free — skip it
                reclaimed += mSize;
            }

            m = next;
        }

        freePtr_ = dest;
        bytesUsed_ -= reclaimed;

        // Flush icache for all relocated code
        if (reclaimed > 0 && dest > zoneStart_) {
            flushICache(zoneStart_, static_cast<size_t>(dest - zoneStart_));
        }

        return reclaimed;
    }

    // ===== QUERIES =====

    bool contains(const void* ptr) const {
        auto p = static_cast<const uint8_t*>(ptr);
        return p >= zoneStart_ && p < zoneEnd_;
    }

    // Find the JITMethod that contains the given code address.
    // Used for: stack walking, deoptimization, IC miss handling.
    JITMethod* findMethodContaining(const void* codeAddr) const {
        auto addr = static_cast<const uint8_t*>(codeAddr);
        if (!contains(addr)) return nullptr;

        // Linear scan (could be replaced with a sorted index if needed)
        JITMethod* m = firstMethod_;
        while (m) {
            const uint8_t* start = reinterpret_cast<const uint8_t*>(m);
            const uint8_t* end = start + m->allocationSize();
            if (addr >= start && addr < end) return m;
            m = m->nextInZone;
        }
        return nullptr;
    }

    uint8_t* rawStart() const { return zoneStart_; }
    size_t freeBytes() const { return static_cast<size_t>(zoneEnd_ - freePtr_); }
    size_t usedBytes() const { return bytesUsed_; }
    size_t totalBytes() const { return zoneSize_; }
    size_t methodCount() const { return methodCount_; }
    uint32_t currentEpoch() const { return epoch_; }

    // Utilization as a percentage (0-100)
    int utilizationPercent() const {
        if (zoneSize_ == 0) return 0;
        return static_cast<int>(bytesUsed_ * 100 / zoneSize_);
    }

    JITMethod* firstMethod() const { return firstMethod_; }

    // ===== DIAGNOSTICS =====

    void printStats() const {
        fprintf(stderr, "[JIT CodeZone] %zu / %zu bytes used (%d%%), %zu methods, epoch %u\n",
                bytesUsed_, zoneSize_, utilizationPercent(),
                methodCount_, epoch_);
    }

private:
    uint8_t* zoneStart_ = nullptr;
    uint8_t* zoneEnd_ = nullptr;
    uint8_t* freePtr_ = nullptr;
    size_t   zoneSize_ = 0;
    size_t   methodCount_ = 0;
    size_t   bytesUsed_ = 0;
    uint32_t epoch_ = 0;

    // Doubly-linked list of methods in allocation order
    JITMethod* firstMethod_ = nullptr;
    JITMethod* lastMethod_ = nullptr;
};

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_CODE_ZONE_HPP
