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
 *     2. allocate(codeSize, numIC) — bump-allocate or reuse from free list
 *     3. ... write machine code into the allocated region ...
 *     4. finalize(method) — flush icache, mark executable
 *     5. When full: freeMethod() evicts cold methods into free list,
 *        allocate() reuses freed space. compact() is a last resort
 *        that only works when ALL methods are invalidated first.
 *     6. destroy() — munmap
 *
 * ALLOCATION STRATEGY:
 *
 *     allocate() tries in order:
 *       1. Bump pointer (fast path, no fragmentation)
 *       2. Free list best-fit (reuse evicted method slots)
 *
 *     freeMethod() returns a method's space to the free list without
 *     moving any other methods (avoids ADRP+LDR relocation issues).
 *     Adjacent free blocks are coalesced.
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

// Free block in the code zone (stored in-place in freed method space)
struct FreeBlock {
    uint32_t   blockSize;   // Total size of this free block (including header)
    uint32_t   _pad;        // Alignment padding
    FreeBlock* next;        // Next free block (address-ordered list)
};

static_assert(sizeof(FreeBlock) <= MethodAlignment,
              "FreeBlock must fit in minimum allocation");

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
        freeListBytes_ = 0;
        epoch_ = 0;
        firstMethod_ = nullptr;
        lastMethod_ = nullptr;
        freeList_ = nullptr;

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
            freeListBytes_ = 0;
            firstMethod_ = nullptr;
            lastMethod_ = nullptr;
            freeList_ = nullptr;
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

        JITMethod* method = nullptr;

        // 1. Try bump-pointer (fast path)
        if (freePtr_ + totalSize <= zoneEnd_) {
            method = reinterpret_cast<JITMethod*>(freePtr_);
            freePtr_ += totalSize;
        }
        // 2. Try free list (best-fit)
        else if (freeListBytes_ >= totalSize) {
            method = allocateFromFreeList(totalSize);
        }

        if (!method) return nullptr;

        // Initialize the allocation
        bytesUsed_ += totalSize;
        methodCount_++;

        std::memset(method, 0, totalSize);
        method->codeSize = codeSize;
        method->numICEntries = numICEntries;
        method->totalSize = static_cast<uint32_t>(totalSize);
        method->state = MethodState::Interpreted;
        method->lastUsedEpoch = epoch_;

        for (uint16_t i = 0; i < numICEntries; i++) {
            method->icEntries()[i].reset();
        }

        // Link into the method list (address-ordered for cache locality)
        linkMethod(method);

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

    // ===== FREE METHOD =====

    // Free a compiled method: unlink from method list, add space to free list.
    // Returns the compiledMethodOop (for MethodMap cleanup by the caller).
    // Caller must remove the method from MethodMap separately.
    uint64_t freeMethod(JITMethod* method) {
        if (!method || !contains(method)) return 0;

        uint64_t methodOop = method->compiledMethodOop;
        size_t size = method->allocationSize();

        // Unlink from the doubly-linked method list
        if (method->prevInZone)
            method->prevInZone->nextInZone = method->nextInZone;
        else
            firstMethod_ = method->nextInZone;

        if (method->nextInZone)
            method->nextInZone->prevInZone = method->prevInZone;
        else
            lastMethod_ = method->prevInZone;

        methodCount_--;
        bytesUsed_ -= size;

        // Add to free list
        addToFreeList(reinterpret_cast<uint8_t*>(method), size);

        return methodOop;
    }

    // ===== EVICTION =====

    // Increment the global epoch counter. Call this periodically
    // (e.g., every N method compilations or every GC cycle).
    void advanceEpoch() { epoch_++; }

    // Touch a method (update its LRU epoch). Call on each entry.
    void touch(JITMethod* method) {
        method->lastUsedEpoch = epoch_;
    }

    // Evict the oldest methods until at least `bytesNeeded` are freed.
    // Freed space goes to the free list for reuse by allocate().
    // Caller must handle MethodMap cleanup for each evicted method
    // (use the callback variant evictLRU with a callback).
    //
    // Returns the number of bytes freed.
    size_t evictLRU(size_t bytesNeeded,
                    void (*onEvict)(uint64_t methodOop, void* ctx) = nullptr,
                    void* ctx = nullptr,
                    void (*onPreEvict)(JITMethod* m, void* ctx2) = nullptr,
                    void* ctx2 = nullptr) {
        size_t freed = 0;
        uint32_t threshold = epoch_ > 10 ? epoch_ - 10 : 0;

        // First pass: evict methods older than threshold
        JITMethod* m = firstMethod_;
        while (m && freed < bytesNeeded) {
            JITMethod* next = m->nextInZone;
            if (m->state == MethodState::Compiled && m->lastUsedEpoch < threshold) {
                if (onPreEvict) onPreEvict(m, ctx2);
                size_t sz = m->allocationSize();
                uint64_t oop = freeMethod(m);
                if (onEvict) onEvict(oop, ctx);
                freed += sz;
            }
            m = next;
        }

        // Second pass with lower threshold if needed
        if (freed < bytesNeeded) {
            m = firstMethod_;
            while (m && freed < bytesNeeded) {
                JITMethod* next = m->nextInZone;
                if (m->state == MethodState::Compiled) {
                    if (onPreEvict) onPreEvict(m, ctx2);
                    size_t sz = m->allocationSize();
                    uint64_t oop = freeMethod(m);
                    if (onEvict) onEvict(oop, ctx);
                    freed += sz;
                }
                m = next;
            }
        }

        return freed;
    }

    // Reset the zone: clear the free list and reset the bump pointer.
    // Only safe when NO live methods remain (all have been freed via
    // freeMethod). This is the "full flush" last resort when the free
    // list is too fragmented to satisfy an allocation.
    //
    // WARNING: Do NOT call this when live methods exist — it would
    // overwrite them. The method list must be empty before calling.
    size_t compact() {
        if (firstMethod_) {
            // There are still live methods — we can't safely reset.
            // Just return 0; caller should free all methods first.
            return 0;
        }

        size_t reclaimed = freeListBytes_;

        // Clear the free list and reset bump pointer to start of zone
        freeList_ = nullptr;
        freeListBytes_ = 0;
        freePtr_ = zoneStart_;
        // bytesUsed_ should already be 0 since all methods were freed

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
    size_t freeBytes() const {
        return static_cast<size_t>(zoneEnd_ - freePtr_) + freeListBytes_;
    }
    size_t bumpFreeBytes() const { return static_cast<size_t>(zoneEnd_ - freePtr_); }
    size_t freeListFreeBytes() const { return freeListBytes_; }
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

    // Find JIT method containing the given PC address (for crash diagnostics)
    JITMethod* findMethodByPC(uint64_t pc) const {
        uint8_t* addr = reinterpret_cast<uint8_t*>(pc);
        if (addr < zoneStart_ || addr >= zoneEnd_) return nullptr;
        JITMethod* m = firstMethod_;
        while (m) {
            uint8_t* start = m->codeStart();
            uint8_t* end = start + m->codeSize;
            if (addr >= start && addr < end) return m;
            m = m->nextInZone;
        }
        return nullptr;
    }

    uint8_t* zoneStart() const { return zoneStart_; }

    // ===== DIAGNOSTICS =====

    void printStats() const {
        fprintf(stderr, "[JIT CodeZone] %zu / %zu bytes used (%d%%), %zu methods, epoch %u, "
                "freeList=%zu bytes\n",
                bytesUsed_, zoneSize_, utilizationPercent(),
                methodCount_, epoch_, freeListBytes_);
    }

private:
    uint8_t* zoneStart_ = nullptr;
    uint8_t* zoneEnd_ = nullptr;
    uint8_t* freePtr_ = nullptr;
    size_t   zoneSize_ = 0;
    size_t   methodCount_ = 0;
    size_t   bytesUsed_ = 0;
    size_t   freeListBytes_ = 0;  // Total bytes in free list
    uint32_t epoch_ = 0;

    // Doubly-linked list of methods in allocation order
    JITMethod* firstMethod_ = nullptr;
    JITMethod* lastMethod_ = nullptr;

    // Free list (address-ordered, coalesced)
    FreeBlock* freeList_ = nullptr;

    // ===== PRIVATE HELPERS =====

    // Add freed space to the free list with coalescing.
    void addToFreeList(uint8_t* ptr, size_t size) {
        if (size < sizeof(FreeBlock)) return;

        // Insert in address order (enables coalescing)
        FreeBlock** prev = &freeList_;
        FreeBlock*  curr = freeList_;
        while (curr && reinterpret_cast<uint8_t*>(curr) < ptr) {
            prev = &curr->next;
            curr = curr->next;
        }

        // Create the new free block
        FreeBlock* block = reinterpret_cast<FreeBlock*>(ptr);
        block->blockSize = static_cast<uint32_t>(size);
        block->next = curr;
        *prev = block;
        freeListBytes_ += size;

        // Coalesce with next block if adjacent
        if (curr) {
            uint8_t* blockEnd = ptr + block->blockSize;
            if (blockEnd == reinterpret_cast<uint8_t*>(curr)) {
                block->blockSize += curr->blockSize;
                block->next = curr->next;
            }
        }

        // Coalesce with previous block if adjacent
        // prev points to the 'next' field of the predecessor (or &freeList_)
        if (prev != &freeList_) {
            // The predecessor FreeBlock contains the 'next' field that prev points to
            FreeBlock* predBlock = reinterpret_cast<FreeBlock*>(
                reinterpret_cast<uint8_t*>(prev) - offsetof(FreeBlock, next));
            uint8_t* predEnd = reinterpret_cast<uint8_t*>(predBlock) + predBlock->blockSize;
            if (predEnd == ptr) {
                predBlock->blockSize += block->blockSize;
                predBlock->next = block->next;
            }
        }
    }

    // Allocate from free list using best-fit. Returns nullptr if no fit found.
    JITMethod* allocateFromFreeList(size_t totalSize) {
        FreeBlock** bestPrev = nullptr;
        FreeBlock*  bestBlock = nullptr;
        size_t      bestSize = SIZE_MAX;

        // Best-fit search
        FreeBlock** prev = &freeList_;
        FreeBlock*  curr = freeList_;
        while (curr) {
            if (curr->blockSize >= totalSize && curr->blockSize < bestSize) {
                bestPrev = prev;
                bestBlock = curr;
                bestSize = curr->blockSize;
                if (bestSize == totalSize) break;  // Perfect fit
            }
            prev = &curr->next;
            curr = curr->next;
        }

        if (!bestBlock) return nullptr;

        size_t remainder = bestBlock->blockSize - totalSize;
        freeListBytes_ -= totalSize;

        if (remainder >= MethodAlignment) {
            // Split: create a smaller free block from the remainder
            FreeBlock* rest = reinterpret_cast<FreeBlock*>(
                reinterpret_cast<uint8_t*>(bestBlock) + totalSize);
            rest->blockSize = static_cast<uint32_t>(remainder);
            rest->next = bestBlock->next;
            *bestPrev = rest;
        } else {
            // Use the entire block (small waste is acceptable)
            *bestPrev = bestBlock->next;
            freeListBytes_ -= remainder;  // This waste is no longer free
            totalSize = bestBlock->blockSize;
        }

        return reinterpret_cast<JITMethod*>(bestBlock);
    }

    // Link a method into the doubly-linked list in address order.
    void linkMethod(JITMethod* method) {
        uint8_t* addr = reinterpret_cast<uint8_t*>(method);

        // Find insertion point (address order)
        JITMethod* prev = nullptr;
        JITMethod* curr = firstMethod_;
        while (curr && reinterpret_cast<uint8_t*>(curr) < addr) {
            prev = curr;
            curr = curr->nextInZone;
        }

        method->prevInZone = prev;
        method->nextInZone = curr;
        if (prev) prev->nextInZone = method;
        else firstMethod_ = method;
        if (curr) curr->prevInZone = method;
        else lastMethod_ = method;
    }
};

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_CODE_ZONE_HPP
