/*
 * ObjectMemory.cpp - Heap Management Implementation
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 *
 * This is a clean C++ reimplementation of the Spur 64-bit object memory,
 * based on the architecture and algorithms defined by the Pharo project
 * (https://pharo.org) and OpenSmalltalk-VM. The Spur memory manager
 * design by Eliot Miranda served as the authoritative reference.
 * See THIRD_PARTY_LICENSES for upstream license details.
 */

#include "ObjectMemory.hpp"
#include "Interpreter.hpp"
#include "DebugVars.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <dlfcn.h>
#include <functional>
#ifdef _WIN32
#include "../platform/win_mman.h"  // mmap/munmap/madvise over VirtualAlloc
#else
#include <sys/mman.h>
#endif

namespace pharo {

extern uint64_t g_stepNum;

// Eden bump-cell addresses for the JIT inline-alloc emit (PHARO_T1_INLINE_NEW_ASM);
// set in initializeHeap once the (single) ObjectMemory instance exists.
extern "C" uint8_t** g_jitEdenFreeCell = nullptr;
extern "C" uint8_t** g_jitSurvivorStartCell = nullptr;

// Base address of the (resize-once, never-reallocated) class table, captured at
// init so the JIT `class` inline-prim can index it directly. See
// AsmjitT1.cpp tryPrimClass and memory/vm-speed-lever-dispatch.
uint64_t g_classTableBase = 0;

// ===== CONSTRUCTION / INITIALIZATION =====

ObjectMemory::ObjectMemory() = default;

ObjectMemory::~ObjectMemory() {
    // Free allocated memory regions
    if (permSpaceStart_) {
        std::free(permSpaceStart_);
    }
    if (oldSpaceStart_) {
        if (oldSpaceUseMmap_) {
            munmap(oldSpaceStart_, oldSpaceMmapSize_);
        } else {
            std::free(oldSpaceStart_);
        }
    }
    if (newSpaceStart_) {
        std::free(newSpaceStart_);
    }
}

bool ObjectMemory::initialize(const MemoryConfig& config) {
    // Allocate memory regions
    // Use aligned allocation for 8-byte alignment requirement

    permSpaceStart_ = static_cast<uint8_t*>(
#ifdef _WIN32
        // MinGW/UCRT has no C11 aligned_alloc; malloc is 16-byte aligned on
        // x64 (>= the 8-byte need) and pairs with the std::free below.
        std::malloc(config.permSpaceSize));
#else
        std::aligned_alloc(8, config.permSpaceSize));
#endif
    if (!permSpaceStart_) return false;
    permSpaceEnd_ = permSpaceStart_ + config.permSpaceSize;

    // Old-space: reserve `oldSpaceMaxSize` virtual bytes up-front.
    // mmap(MAP_ANONYMOUS) reserves address space without committing pages;
    // the kernel only allocates physical memory when the VM writes to a
    // page.  So a 4 GB reservation on an iPhone 8 with 1 GB of physical
    // RAM costs ~0 bytes of physical memory; physical use is capped by
    // the OS's OOM kill / jetsam policy.
    //
    // Back-compat: legacy callers still set `config.oldSpaceSize` — treat
    // it as a max if set.  `oldSpaceInitialSize` is a GC-tuning hint
    // (soft threshold); actual allocation grows lazily up to max.
    size_t maxSize = config.oldSpaceMaxSize;
    if (config.oldSpaceSize != 0) {
        maxSize = config.oldSpaceSize;  // legacy single-knob path
    }
    oldSpaceStart_ = static_cast<uint8_t*>(
        mmap(nullptr, maxSize, PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
    if (oldSpaceStart_ == MAP_FAILED) {
        oldSpaceStart_ = nullptr;
        std::free(permSpaceStart_);
        permSpaceStart_ = nullptr;
        return false;
    }
    oldSpaceUseMmap_ = true;
    oldSpaceMmapSize_ = maxSize;
    oldSpaceEnd_ = oldSpaceStart_ + maxSize;
    oldSpaceFree_ = oldSpaceStart_;
    oldSpaceInitialTarget_ = oldSpaceStart_ + config.oldSpaceInitialSize;
    newSpaceStart_ = static_cast<uint8_t*>(
#ifdef _WIN32
        std::malloc(config.newSpaceSize));   // see permSpace note above
#else
        std::aligned_alloc(8, config.newSpaceSize));
#endif
    if (!newSpaceStart_) {
        std::free(permSpaceStart_);
        munmap(oldSpaceStart_, oldSpaceMmapSize_);
        permSpaceStart_ = nullptr;
        oldSpaceStart_ = nullptr;
        return false;
    }
    newSpaceEnd_ = newSpaceStart_ + config.newSpaceSize;

    // Split new space into eden and survivor
    size_t edenSize = (config.newSpaceSize * config.edenRatio) / 100;
    edenStart_ = newSpaceStart_;
    survivorStart_ = newSpaceStart_ + edenSize;
    edenAllocBase_ = edenStart_;
    edenAllocLimit_ = survivorStart_;
    if (GET_DEBUG_BOOL(PHARO_EDEN_ROTATE)) {
        size_t half = (static_cast<size_t>(survivorStart_ - edenStart_) / 2) & ~4095ULL;
        edenAllocLimit_ = edenStart_ + half;
    }
    edenFree_ = edenAllocBase_;

    // Publish the eden bump-cell addresses for the JIT inline-alloc emit
    // (PHARO_T1_INLINE_NEW_ASM). One ObjectMemory instance per VM, and these
    // fields' addresses are stable after construction, so the emit bakes them.
    g_jitEdenFreeCell = &edenFree_;
    g_jitSurvivorStartCell = &edenAllocLimit_;

    // Initialize class table
    classTable_.resize(config.classTableSize, Oop::nil());
    // Capture the stable base for the JIT `class` inline-prim. classTable_ is
    // resized exactly once here and never reallocated (setClassAtIndex only
    // writes within bounds), so this pointer is valid for the VM's lifetime.
    g_classTableBase = reinterpret_cast<uint64_t>(classTable_.data());

    // Zero perm space and new space (old space is mmap'd with MAP_ANONYMOUS,
    // which provides zero-filled pages lazily — no memset needed)
    std::memset(permSpaceStart_, 0, config.permSpaceSize);
    std::memset(newSpaceStart_, 0, config.newSpaceSize);

    // PHARO_GC_HEADROOM_MB override (default 0 = keep compile-time 512 MB).
    // See ObjectMemory.hpp gcHeadroom_ comment for the perf-vs-RSS table.
    if (g_debug.gcHeadroomMB > 0) {
        gcHeadroom_ = static_cast<size_t>(g_debug.gcHeadroomMB) * 1024ULL * 1024ULL;
    }

    return true;
}

// ===== OBJECT ALLOCATION =====

Oop ObjectMemory::allocateSlots(uint32_t classIndex, size_t slotCount,
                                 ObjectFormat format) {
    size_t headerSize = sizeof(ObjectHeader);
    bool hasOverflow = slotCount >= 255;
    if (hasOverflow) {
        headerSize += sizeof(uint64_t);
    }

    size_t bodySize = slotCount * sizeof(Oop);
    size_t totalSize = headerSize + bodySize;
    totalSize = (totalSize + 7) & ~7ULL;
    // Spur minimum object size: 16 bytes.  ObjectHeader::totalSize() and
    // every linear heap walker (ObjectScanner, allObjectsDo) advance 16 for
    // a 0-slot object; allocating it as 8 packed the NEXT object 8 bytes
    // earlier than walkers expect, desyncing every linear walk downstream
    // (become's write-through, the scavenger's old-space root scan via the
    // eden-full old-space fallback) — the OpalCompiler large-method
    // corruption root cause (2026-07-02).
    if (totalSize < 16) totalSize = 16;

    // Allocate in eden (young) for pointer-slot objects except
    // overflow-slot (>= 255 slots, large/stable objects).  Overflow
    // variants go to old because scavenge copy hasn't been tested
    // against the overflow-word layout yet.  Fall back to old on
    // eden-full.
    Space targetSpace = (hasOverflow || enableYoungGen_ == false)
                        ? Space::Old : Space::New;
    ObjectHeader* obj = allocateRaw(totalSize, targetSpace);
    if (!obj && targetSpace == Space::New) {
        // Eden full — retry in old space.  A scavenge will run at
        // the next safe point (needsScavenge_ flag set by
        // allocateRaw).
        obj = allocateRaw(totalSize, Space::Old);
    }

    if (!obj) {
        return nilObject_;
    }

    // Set up overflow word if needed.
    // Spur convention: byte 7 of the overflow word must be 0xFF so the
    // ObjectScanner recognises it as an overflow word and skips to the real header.
    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(obj);
        *overflow = slotCount | (0xFFULL << 56);
        obj = reinterpret_cast<ObjectHeader*>(overflow + 1);
    }

    initializeHeader(obj, classIndex, slotCount, format);

    Oop* slots = obj->slots();
    // During image load nilObject_ may not be set yet; raw 0 is fine since
    // image loading will overwrite these slots.
    Oop nilValue = (nilObject_.rawBits() != 0) ? nilObject_ : Oop::nil();
    for (size_t i = 0; i < slotCount; ++i) {
        slots[i] = nilValue;
    }
    // Zero the min-16 pad word of a 0-slot object so it never carries
    // stale bytes (kept tenure-memcpy'd garbage out of diagnostics).
    if (slotCount == 0) {
        reinterpret_cast<uint64_t*>(slots)[0] = 0;
    }

    bytesAllocated_ += totalSize;
    Oop result = oopFromPointer(obj);

    // PHARO_MNU_ALLOC_DBG=1: print every allocation of a
    // MessageNotUnderstood (classIndex == 4307 in the Pharo 13 image).
    // Used to find which call site leaks an empty MNU through to the
    // Set>>fullCheck JIT bug.
    if (GET_DEBUG_BOOL(PHARO_MNU_ALLOC_DBG) && classIndex == 4307) {
        static int n = 0;
        n++;
        if (n <= 30) {
            void* ra1 = __builtin_return_address(0);
            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Wframe-address"
            void* ra2 = __builtin_extract_return_addr(__builtin_return_address(1));
            #pragma GCC diagnostic pop
            Dl_info i1{}, i2{};
            int got1 = dladdr(ra1, &i1);
            int got2 = dladdr(ra2, &i2);
            fprintf(stderr,
                "[MNU-ALLOC] #%d oop=0x%llx slotCount=%zu callers: %s+%lld <- %s+%lld\n",
                n, (unsigned long long)result.rawBits(), slotCount,
                got1 && i1.dli_sname ? i1.dli_sname : "?",
                got1 ? (long long)((uint8_t*)ra1 - (uint8_t*)i1.dli_saddr) : 0LL,
                got2 && i2.dli_sname ? i2.dli_sname : "?",
                got2 ? (long long)((uint8_t*)ra2 - (uint8_t*)i2.dli_saddr) : 0LL);
        }
    }
    // Corpse forensics (PHARO_CORPSE_PUSH_TRAP): remember the last object
    // allocated and the scavenge count at that instant.  If a corpse pushed
    // later IS this address and the scavenge count has since advanced, then a
    // scavenge ran between the allocation and the push and tenured the object,
    // leaving this Oop aimed at a scrubbed eden slot -- a leaked GC deferral.
    // If the counts match, nothing collected it and the allocator returned it
    // uninitialised. The two cases need opposite fixes, so measure, don't guess.
    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_CORPSE_PUSH_TRAP), 0)) {
        extern uint64_t g_scavengeCount;
        extern uint64_t g_lastAllocAddr;
        extern uint64_t g_lastAllocScav;
        extern uint32_t g_lastAllocClassIdx;
        g_lastAllocAddr = result.rawBits();
        g_lastAllocScav = g_scavengeCount;
        g_lastAllocClassIdx = classIndex;
    }
    return result;
}

Oop ObjectMemory::allocateBytes(uint32_t classIndex, size_t byteCount) {
    size_t slotCount = (byteCount + 7) / 8;

    size_t padding = (slotCount * 8) - byteCount;
    ObjectFormat format = static_cast<ObjectFormat>(
        static_cast<int>(ObjectFormat::Indexable8) + padding);

    size_t headerSize = sizeof(ObjectHeader);
    bool hasOverflow = slotCount >= 255;
    if (hasOverflow) {
        headerSize += sizeof(uint64_t);
    }

    size_t totalSize = headerSize + slotCount * 8;
    totalSize = (totalSize + 7) & ~7ULL;
    if (totalSize < 16) totalSize = 16;  // Spur minimum (see allocateSlots)

    // Allocate in old space (no generational GC — eden is reserved for compacting GC scratch)
    ObjectHeader* obj = allocateRaw(totalSize, Space::Old);

    if (!obj) return nilObject_;

    // Handle overflow (byte 7 must be 0xFF for scanner recognition)
    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(obj);
        *overflow = slotCount | (0xFFULL << 56);
        obj = reinterpret_cast<ObjectHeader*>(overflow + 1);
    }

    initializeHeader(obj, classIndex, slotCount, format);

    std::memset(obj->bytes(), 0, slotCount * 8);
    if (slotCount == 0) {
        reinterpret_cast<uint64_t*>(obj->slots())[0] = 0;  // min-16 pad word
    }

    bytesAllocated_ += totalSize;
    return oopFromPointer(obj);
}

Oop ObjectMemory::createString(const std::string& str) {
    Oop stringClass = specialObject(SpecialObjectIndex::ClassByteString);
    if (stringClass.isNil() || !stringClass.isObject()) {
        return nilObject_;
    }

    // In Spur, a class's identity hash IS the class index for its instances
    ObjectHeader* classHdr = stringClass.asObjectPtr();
    uint32_t classIndex = classHdr->identityHash();
    if (classIndex == 0) {
        classIndex = classHdr->classIndex();
    }

    Oop strObj = allocateBytes(classIndex, str.size());
    if (strObj.isNil()) {
        return nilObject_;
    }

    ObjectHeader* strHdr = strObj.asObjectPtr();
    std::memcpy(strHdr->bytes(), str.c_str(), str.size());

    return strObj;
}

Oop ObjectMemory::allocateWords(uint32_t classIndex, size_t wordCount) {
    size_t slotCount = wordCount;

    size_t headerSize = sizeof(ObjectHeader);
    bool hasOverflow = slotCount >= 255;
    if (hasOverflow) {
        headerSize += sizeof(uint64_t);
    }

    size_t totalSize = headerSize + slotCount * 8;
    totalSize = (totalSize + 7) & ~7ULL;
    if (totalSize < 16) totalSize = 16;  // Spur minimum (see allocateSlots)

    // Allocate in old space (no generational GC — eden is reserved for compacting GC scratch)
    ObjectHeader* obj = allocateRaw(totalSize, Space::Old);

    if (!obj) return nilObject_;

    // Handle overflow (byte 7 must be 0xFF for scanner recognition)
    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(obj);
        *overflow = slotCount | (0xFFULL << 56);
        obj = reinterpret_cast<ObjectHeader*>(overflow + 1);
    }

    initializeHeader(obj, classIndex, slotCount, ObjectFormat::Indexable64);
    std::memset(obj->bytes(), 0, slotCount * 8);
    if (slotCount == 0) {
        reinterpret_cast<uint64_t*>(obj->slots())[0] = 0;  // min-16 pad word
    }

    bytesAllocated_ += totalSize;
    return oopFromPointer(obj);
}

Oop ObjectMemory::allocateCompiledMethod(uint32_t classIndex, size_t numSlots, size_t bytecodeSize) {
    // CompiledMethod: numSlots pointer slots followed by bytecodeSize bytes
    // The slot area holds header (slot 0) and literals
    // The byte area holds the bytecodes
    // Calculate total byte size
    size_t slotBytes = numSlots * 8;
    size_t totalBytes = slotBytes + bytecodeSize;

    size_t totalSlots = (totalBytes + 7) / 8;

    // Format 24+padding = CompiledMethod with 0-3 unused trailing bytes
    size_t padding = (totalSlots * 8) - totalBytes;
    ObjectFormat format = static_cast<ObjectFormat>(24 + padding);

    size_t headerSize = sizeof(ObjectHeader);
    bool hasOverflow = totalSlots >= 255;
    if (hasOverflow) {
        headerSize += sizeof(uint64_t);
    }

    size_t totalSize = headerSize + totalSlots * 8;
    totalSize = (totalSize + 7) & ~7ULL;
    if (totalSize < 16) totalSize = 16;  // Spur minimum (see allocateSlots)

    ObjectHeader* obj = allocateRaw(totalSize, Space::Old);
    if (!obj) return nilObject_;

    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(obj);
        *overflow = totalSlots | (0xFFULL << 56);
        obj = reinterpret_cast<ObjectHeader*>(overflow + 1);
    }

    initializeHeader(obj, classIndex, totalSlots, format);

    std::memset(obj->slots(), 0, totalSlots * 8);

    bytesAllocated_ += totalSize;
    return oopFromPointer(obj);
}

Oop ObjectMemory::shallowCopy(Oop original) {
    if (!original.isObject()) {
        return original;  // Immediates are their own copies
    }

    ObjectHeader* src = original.asObjectPtr();
    size_t size = src->totalSize();
    bool hasOverflow = src->hasOverflowSlots();

    // Generational clones (2026-06-13): most clones are short-lived
    // (ThirtyTwoBitRegister in SHA256 = 3.2M/hash, Point/Rectangle in
    // graphics) and piling them into old space forces periodic fullGC.
    // Route small non-overflow clones through eden like allocateSlots
    // does — a scavenge reclaims the dead ones cheaply.  (The old "eden
    // reserved for compacting GC scratch" comment was stale:
    // allocateSlots has used eden for normal pointer objects all along.)
    // Overflow (>=255 slots) and young-gen-off fall to old, matching
    // allocateSlots' caveat (scavenge copy untested vs the overflow-word
    // layout).  Opt-out PHARO_NO_GEN_CLONE.
    bool cloneToEden = !hasOverflow && enableYoungGen_
        && !GET_DEBUG_BOOL(PHARO_NO_GEN_CLONE);
    ObjectHeader* copy = allocateRaw(size,
        cloneToEden ? Space::New : Space::Old);
    if (!copy && cloneToEden) copy = allocateRaw(size, Space::Old);
    if (!copy) return nilObject_;

    // For objects with overflow slot count, the memory layout is:
    // [overflow_word][header][data...]
    // We need to copy the overflow word too, not just from the header
    if (hasOverflow) {
        // Source starts at overflow word (8 bytes before header)
        const uint8_t* srcStart = reinterpret_cast<const uint8_t*>(src) - 8;
        // Copy the whole object including overflow word
        std::memcpy(copy, srcStart, size);
        // The actual header is 8 bytes into the allocated block
        copy = reinterpret_cast<ObjectHeader*>(reinterpret_cast<uint8_t*>(copy) + 8);
    } else {
        // Copy all bytes including header
        std::memcpy(copy, src, size);
    }

    // Generate new identity hash
    copy->setIdentityHash(generateHash());

    // Clear GC and protection flags - clones start clean and mutable.
    // Standard Cog VM's clone (cloneshouldAllocateInPermSpace) creates a fresh
    // header that does NOT inherit immutable/pinned/marked/remembered bits.
    copy->setMarked(false);
    copy->setRemembered(false);
    copy->setImmutable(false);
    copy->setPinned(false);

    bytesAllocated_ += size;
    Oop copyResult = oopFromPointer(copy);

    // Write-barrier fixup: shallowCopy allocates in old space and
    // memcpy's the source's slots verbatim.  If the source had any
    // young refs (and our `setRemembered(false)` above just cleared
    // the bit), the copy now contains an unbarried old→young ref.
    // Scan pointer slots and rememberObject if any young found —
    // matches what storePointer would have done if we'd reached
    // here via normal slot writes.  This was the single largest
    // source of scavenge audit-gap misses (memory/
    // jit_remembered_set_dead.md).
    if (isOldObject(copy) && src->isPointersObject()) {
        size_t np = pointerSlotsOf(src);
        Oop* slots = copy->slots();
        for (size_t i = 0; i < np; ++i) {
            Oop v = slots[i];
            if (v.isObject() && isYoung(v)) {
                rememberObject(copyResult);
                break;
            }
        }
    }
    if (GET_DEBUG_BOOL(PHARO_MNU_ALLOC_DBG) && src->classIndex() == 4307) {
        static int n = 0;
        n++;
        if (n <= 30) {
            void* ra1 = __builtin_return_address(0);
            Dl_info i1{};
            int got1 = dladdr(ra1, &i1);
            fprintf(stderr,
                "[MNU-COPY] #%d new=0x%llx src=0x%llx caller=%s+%lld\n",
                n, (unsigned long long)copyResult.rawBits(),
                (unsigned long long)original.rawBits(),
                got1 && i1.dli_sname ? i1.dli_sname : "?",
                got1 ? (long long)((uint8_t*)ra1 - (uint8_t*)i1.dli_saddr) : 0LL);
        }
    }
    return copyResult;
}

// ===== CLASS TABLE =====


Oop ObjectMemory::classOf(Oop obj) const {
    if (obj.isSmallInteger()) {
        return specialObject(SpecialObjectIndex::ClassSmallInteger);
    }
    if (obj.isCharacter()) {
        return specialObject(SpecialObjectIndex::ClassCharacter);
    }
    if (obj.isSmallFloat()) {
        return classAtIndex(4);
    }
    if (!obj.isObject()) {
        return nilObject_;
    }

    if (obj.isNil()) {
        ObjectHeader* header = obj.asObjectPtr();
        return classAtIndex(header->classIndex());
    }

    if (!isValidPointer(obj)) {
        return nilObject_;
    }

    ObjectHeader* header = obj.asObjectPtr();
    uint32_t clsIdx = header->classIndex();
    // Spur forwarder transparency: becomeForward (2026-07-07) leaves obj1 as a
    // forwarder to obj2 so that a reference the become scan-and-replace MISSED
    // (an untracked JIT operand under materialization — the SlotIntegration
    // root) still resolves.  If such a forwarder reaches dispatch as the
    // receiver, its class MUST be the TARGET's class, not the Forwarded class
    // (index 8) — otherwise method lookup lands in the wrong class and the send
    // spuriously does-not-understand.  That MNU-on-a-forwarded-object during
    // Context>>copyTo: is the documented ARM context-storm TRIGGER (the census's
    // MNU+PrimitiveFailed triples): the debugger's handler freezes on it and
    // re-copies the stack, re-hitting the same forwarder forever.  Following the
    // forwarder here (one predicted-not-taken compare on the already-loaded
    // classIndex) makes forwarders transparent to dispatch, per Spur semantics.
    if (__builtin_expect(clsIdx == ObjectHeader::ForwardedClassIndex, 0)) {
        // Evidence counter: each increment is a send/dispatch to a forwarded
        // object that, WITHOUT this fix, would have looked up methods in the
        // Forwarded class (idx 8) and spuriously MNU'd — a potential ARM-storm
        // trigger the fix silently prevents. Non-zero during a real run proves
        // the fix is doing work. (Counter only touched in this rare branch.)
        extern uint64_t g_classOfForwarderFollows;
        ++g_classOfForwarderFollows;
        // A/B knob (PHARO_NO_CLASSOF_FWD): revert to pre-296bba26 behavior —
        // return the Forwarded class (idx 8) so the send mis-dispatches / MNUs.
        // Used to DEMONSTRATE that these same sites break without the fix.
        if (__builtin_expect(GET_DEBUG_BOOL(PHARO_NO_CLASSOF_FWD), 0))
            return classAtIndex(clsIdx);
        Oop target = followForwarded(obj);
        if (!target.isObject()) return classOf(target);       // followed to an immediate
        if (!isValidPointer(target)) return nilObject_;
        return classAtIndex(target.asObjectPtr()->classIndex());
    }
    return classAtIndex(clsIdx);
}

uint64_t g_classOfForwarderFollows = 0;

uint32_t ObjectMemory::registerClass(Oop classOop) {
    // In Spur, if the class already has an identity hash, that IS its class
    // table index.  Use it rather than assigning a new sequential index.
    //
    // Also track classOf(classOop)'s index in knownMetaclassIndices_ so
    // identityHashOf() can later detect class instances by checking their
    // class field — letting us route class hashing through registerClass
    // (sequential) instead of generateHash() (random).
    uint32_t assignedIdx = 0;
    if (classOop.isObject()) {
        ObjectHeader* hdr = classOop.asObjectPtr();
        uint32_t hash = hdr->identityHash();
        if (hash != 0 && hash < classTable_.size()) {
            if (GET_DEBUG_BOOL(PHARO_CTCHECK)) {
                Oop prev = classTable_[hash];
                if (prev.isObject() && !prev.isNil() && prev != classOop) {
                    fprintf(stderr, "[CTOVERWRITE-reuse] idx=%u old=0x%llx new=0x%llx (class reused its hash, displacing a live entry)\n",
                            hash, (unsigned long long)prev.rawBits(), (unsigned long long)classOop.rawBits());
                }
            }
            classTable_[hash] = classOop;
            assignedIdx = hash;
        } else {
            // No hash yet — assign new index and set the hash to match
            uint32_t index = nextClassIndex_++;
            if (index < classTable_.size()) {
                if (GET_DEBUG_BOOL(PHARO_CTCHECK)) {
                    Oop prev = classTable_[index];
                    if (prev.isObject() && !prev.isNil() && prev != classOop) {
                        fprintf(stderr, "[CTOVERWRITE-seq] idx=%u old=0x%llx new=0x%llx nextClassIndex_now=%u (sequential alloc hit a LIVE slot!)\n",
                                index, (unsigned long long)prev.rawBits(), (unsigned long long)classOop.rawBits(), nextClassIndex_);
                    } else {
                        fprintf(stderr, "[CTASSIGN] idx=%u new=0x%llx\n", index, (unsigned long long)classOop.rawBits());
                    }
                }
                classTable_[index] = classOop;
                hdr->setIdentityHash(index);
            }
            assignedIdx = index;
        }
        // Track metaclass: classOf(classOop) is the class of this class,
        // i.e., its metaclass.  Future identityHashOf() calls on instances
        // of this metaclass will know they're class instances.
        uint32_t metaIdx = hdr->classIndex();
        if (metaIdx != 0) {
            knownMetaclassIndices_.insert(metaIdx);
        }
    } else {
        // Non-object passed (shouldn't happen in normal flow) — fall through
        uint32_t index = nextClassIndex_++;
        if (index < classTable_.size()) {
            classTable_[index] = classOop;
        }
        assignedIdx = index;
    }
    return assignedIdx;
}

uint32_t ObjectMemory::indexOfClass(Oop classOop) const {
    // In Spur, a class's identity hash IS its class table index.
    // The Cog VM's classTagForClass: extracts the hash directly:
    //   classIndex = (uint32AtPointer(classObj + 4)) & identityHashHalfWordMask
    // Using the identity hash is O(1) and always returns the canonical index.
    // A linear scan can return a wrong index if a class appears at multiple
    // positions (e.g., metaclass circularity), causing primitiveAllInstances
    // to count wrong objects.
    if (classOop.isObject()) {
        ObjectHeader* hdr = classOop.asObjectPtr();
        uint32_t hash = hdr->identityHash();
        if (hash != 0 && hash < classTable_.size() && classTable_[hash] == classOop) {
            return hash;
        }
    }
    // Fallback: linear scan for classes without a hash or with stale hash
    for (uint32_t i = 0; i < classTable_.size(); ++i) {
        if (classTable_[i] == classOop) {
            return i;
        }
    }
    return 0;  // Not found
}

// ===== SPECIAL OBJECTS =====

Oop ObjectMemory::specialObject(SpecialObjectIndex index) const {
    if (specialObjectsArray_.isNil() || !specialObjectsArray_.isObject()) {
        return nilObject_;
    }

    ObjectHeader* array = specialObjectsArray_.asObjectPtr();
    size_t idx = static_cast<size_t>(index);
    if (idx >= array->slotCount()) {
        return nilObject_;
    }

    return array->slotAt(idx);
}

void ObjectMemory::setSpecialObject(SpecialObjectIndex index, Oop value) {
    if (specialObjectsArray_.isNil() || !specialObjectsArray_.isObject()) {
        return;
    }

    ObjectHeader* array = specialObjectsArray_.asObjectPtr();
    size_t idx = static_cast<size_t>(index);
    if (idx < array->slotCount()) {
        array->slotAtPut(idx, value);
    }
}

void ObjectMemory::cacheSpecialObjects() {
    nilObject_ = specialObject(SpecialObjectIndex::NilObject);
    uint64_t oldNilBits = Oop::getNilBits();
    Oop::setNilBits(nilObject_.rawBits());
    trueObject_ = specialObject(SpecialObjectIndex::TrueObject);
    falseObject_ = specialObject(SpecialObjectIndex::FalseObject);

    // Update class table: replace old nil (Oop(0)) entries with real nil
    if (oldNilBits != nilObject_.rawBits()) {
        for (size_t i = 0; i < classTable_.size(); i++) {
            if (classTable_[i].rawBits() == oldNilBits) {
                classTable_[i] = nilObject_;
            }
        }
    }
}

void ObjectMemory::cacheGCClassIndices() {
    // Cache Context class index for GC. Context objects need special handling:
    // only trace slots up to the stack pointer, not all slots, because slots
    // beyond the stack pointer contain garbage from previous activations.
    Oop contextClass = specialObject(SpecialObjectIndex::ClassMethodContext);
    contextClassIndex_ = 0;
    if (contextClass.isObject()) {
        ObjectHeader* ctxPtr = contextClass.asObjectPtr();
        // Look up by matching the special object pointer against class table entries
        for (uint32_t i = 1; i < classTable_.size() && i < 20000; ++i) {
            if (classTable_[i].isObject() &&
                classTable_[i].asObjectPtr() == ctxPtr) {
                contextClassIndex_ = i;
                break;
            }
        }
        // Also populate the class table entry if it was empty
        if (contextClassIndex_ != 0 && contextClassIndex_ < classTable_.size() &&
            !classTable_[contextClassIndex_].isObject()) {
            classTable_[contextClassIndex_] = contextClass;
        }
    }
}

// ===== SYMBOL AND GLOBAL LOOKUP =====

bool ObjectMemory::symbolEquals(Oop symbol, const char* str) const {
    if (!symbol.isObject()) return false;

    ObjectHeader* header = symbol.asObjectPtr();
    if (!header->isBytesObject()) return false;

    size_t symbolLen = header->byteSize();
    size_t strLen = std::strlen(str);

    if (symbolLen != strLen) return false;

    const uint8_t* symbolBytes = header->bytes();
    return std::memcmp(symbolBytes, str, strLen) == 0;
}

Oop ObjectMemory::findGlobal(const std::string& name) const {
    Oop smalltalkDict = specialObject(SpecialObjectIndex::SmalltalkDictionary);
    if (smalltalkDict.isNil() || !smalltalkDict.isObject()) {
        return nilObject_;
    }

    // Navigate to the actual SystemDictionary (may be wrapped in Environment)
    ObjectHeader* envHeader = smalltalkDict.asObjectPtr();
    Oop sysDict = smalltalkDict;

    if (envHeader->slotCount() >= 1) {
        Oop slot0 = fetchPointer(0, smalltalkDict);
        if (slot0.isObject() && !slot0.isNil()) {
            ObjectHeader* slot0Header = slot0.asObjectPtr();
            if (slot0Header->slotCount() >= 2) {
                Oop innerSlot0 = fetchPointer(0, slot0);
                if (innerSlot0.isSmallInteger()) {
                    sysDict = slot0;  // Use the inner dictionary
                }
            }
        }
    }

    ObjectHeader* dictHeader = sysDict.asObjectPtr();
    Oop arraySlot = fetchPointer(1, sysDict);

    if (!arraySlot.isObject() || arraySlot.isNil()) {
        return nilObject_;
    }

    ObjectHeader* arrayHeader = arraySlot.asObjectPtr();
    size_t arraySize = arrayHeader->slotCount();

    // Check for overflow header
    uint64_t headerRaw = arrayHeader->rawHeader();
    uint64_t slotCountByte = (headerRaw >> 56) & 0xFF;
    if (slotCountByte == 255) {
        const uint64_t* overflowPtr = reinterpret_cast<const uint64_t*>(arrayHeader) - 1;
        uint64_t overflowVal = *overflowPtr;
        if (overflowVal >= 255 && overflowVal <= 1000000 && (overflowVal >> 32) == 0) {
            arraySize = static_cast<size_t>(overflowVal);
        }
    }

    for (size_t i = 0; i < arraySize; ++i) {
        Oop item = arrayHeader->slotAt(i);
        if (item.isNil() || !item.isObject()) continue;
        if (!isValidPointer(item)) continue;

        ObjectHeader* itemHeader = item.asObjectPtr();
        size_t slotCount = itemHeader->slotCount();
        if (slotCount < 2 || slotCount > 100) continue;

        Oop key = fetchPointer(0, item);
        if (!key.isObject() || key.isNil()) continue;
        if (!isValidPointer(key)) continue;

        ObjectHeader* keyHeader = key.asObjectPtr();
        if (!keyHeader->isBytesObject()) continue;

        size_t keySize = keyHeader->byteSize();
        if (keySize > 1000) continue;

        if (symbolEquals(key, name.c_str())) {
            return fetchPointer(1, item);
        }
    }

    // Modern Pharo might store additional entries in overflow structures at slots 2-5
    // Let me search those too (with defensive pointer validation)
    size_t dictSlots = dictHeader->slotCount();
    if (dictSlots > 100) dictSlots = 10;  // Sanity limit

    for (size_t overflowIdx = 2; overflowIdx < dictSlots; ++overflowIdx) {
        Oop overflowSlot = fetchPointer(overflowIdx, sysDict);
        if (!overflowSlot.isObject() || overflowSlot.isNil()) continue;

        // Validate pointer is within heap
        if (!isValidPointer(overflowSlot)) continue;

        ObjectHeader* overflowHeader = overflowSlot.asObjectPtr();
        size_t overflowSlots = overflowHeader->slotCount();
        if (overflowSlots > 100000) continue;  // Sanity limit

        // Check if this object contains associations or arrays of associations
        for (size_t i = 0; i < overflowSlots; ++i) {
            Oop item = fetchPointer(i, overflowSlot);
            if (!item.isObject() || item.isNil()) continue;
            if (!isValidPointer(item)) continue;

            ObjectHeader* itemHeader = item.asObjectPtr();
            size_t itemSlots = itemHeader->slotCount();
            if (itemSlots > 100000) continue;  // Sanity limit

            // Check if it's an array that might contain more associations
            if (itemHeader->format() == ObjectFormat::Indexable && itemSlots < 10000) {
                // Search this array for associations
                for (size_t j = 0; j < itemSlots; ++j) {
                    Oop assoc = fetchPointer(j, item);
                    if (!assoc.isObject() || assoc.isNil()) continue;
                    if (!isValidPointer(assoc)) continue;

                    ObjectHeader* assocHeader = assoc.asObjectPtr();
                    if (assocHeader->slotCount() >= 2 && assocHeader->slotCount() < 100) {
                        Oop key = fetchPointer(0, assoc);
                        if (key.isObject() && !key.isNil() && isValidPointer(key)) {
                            if (symbolEquals(key, name.c_str())) {
                                return fetchPointer(1, assoc);
                            }
                        }
                    }
                }
            }

            // Check if item itself is an association
            if (itemSlots >= 2 && itemSlots < 100) {
                Oop key = fetchPointer(0, item);
                if (key.isObject() && !key.isNil() && isValidPointer(key)) {
                    if (symbolEquals(key, name.c_str())) {
                        return fetchPointer(1, item);
                    }
                }
            }
        }
    }

    // Last resort for 'Smalltalk': try special object index 8 directly
    // In some images, special object 8 IS the Smalltalk/Environment
    if (name == "Smalltalk") {
        return smalltalkDict;
    }

    return nilObject_;
}

Oop ObjectMemory::lookupSymbol(const std::string& name) {
    Oop symbolClass = findGlobal("Symbol");
    Oop byteSymbolClass = findGlobal("ByteSymbol");

    Oop targetClass = byteSymbolClass.isNil() ? symbolClass : byteSymbolClass;
    if (targetClass.isNil() || !targetClass.isObject()) {
        return nilObject_;
    }

    uint32_t symbolClassIdx = identityHashOf(targetClass);
    if (symbolClassIdx == 0) {
        symbolClassIdx = indexOfClass(targetClass);
    }
    if (symbolClassIdx == 0) {
        return nilObject_;
    }

    auto scanRegion = [&](const uint8_t* start, const uint8_t* end) -> Oop {
        if (start == nullptr || end == nullptr || start >= end) {
            return nilObject_;
        }
        const uint8_t* scan = start;
        while (scan < end) {
            ObjectHeader* header = reinterpret_cast<ObjectHeader*>(const_cast<uint8_t*>(scan));

            uint32_t clsIdx = header->classIndex();
            if (clsIdx == 0) {
                scan += 8;
                continue;
            }

            if (clsIdx == symbolClassIdx) {
                if (header->isBytesObject()) {
                    size_t byteSize = header->byteSize();
                    if (byteSize == name.size()) {
                        const uint8_t* bytes = header->bytes();
                        if (memcmp(bytes, name.c_str(), byteSize) == 0) {
                            return Oop::fromObject(header);
                        }
                    }
                }
            }

            size_t objSize = header->totalSize();
            if (objSize == 0 || objSize > static_cast<size_t>(end - scan)) {
                scan += 8;
                continue;
            }
            scan += objSize;
        }
        return nilObject_;
    };

    Oop result = scanRegion(permSpaceStart_, permSpaceEnd_);
    if (result.rawBits() != nilObject_.rawBits()) return result;

    result = scanRegion(oldSpaceStart_, oldSpaceFree_);
    if (result.rawBits() != nilObject_.rawBits()) return result;

    return nilObject_;
}

bool ObjectMemory::setGlobal(const std::string& name, Oop value) {
    Oop smalltalkDict = specialObject(SpecialObjectIndex::SmalltalkDictionary);
    if (smalltalkDict.isNil() || !smalltalkDict.isObject()) {
        return false;
    }

    // Navigate to the actual SystemDictionary (may be wrapped in Environment)
    ObjectHeader* envHeader = smalltalkDict.asObjectPtr();
    Oop sysDict = smalltalkDict;

    if (envHeader->slotCount() >= 1) {
        Oop slot0 = fetchPointer(0, smalltalkDict);
        if (slot0.isObject() && !slot0.isNil()) {
            ObjectHeader* slot0Header = slot0.asObjectPtr();
            if (slot0Header->slotCount() >= 2) {
                Oop innerSlot0 = fetchPointer(0, slot0);
                if (innerSlot0.isSmallInteger()) {
                    sysDict = slot0;  // Use the inner dictionary
                }
            }
        }
    }

    Oop arraySlot = fetchPointer(1, sysDict);
    if (!arraySlot.isObject() || arraySlot.isNil()) {
        return false;
    }

    ObjectHeader* arrayHeader = arraySlot.asObjectPtr();
    size_t arraySize = arrayHeader->slotCount();

    // Check for overflow header
    uint64_t headerRaw = arrayHeader->rawHeader();
    uint64_t slotCountByte = (headerRaw >> 56) & 0xFF;
    if (slotCountByte == 255) {
        const uint64_t* overflowPtr = reinterpret_cast<const uint64_t*>(arrayHeader) - 1;
        uint64_t overflowVal = *overflowPtr;
        if (overflowVal >= 255 && overflowVal <= 1000000 && (overflowVal >> 32) == 0) {
            arraySize = static_cast<size_t>(overflowVal);
        }
    }

    for (size_t i = 0; i < arraySize; ++i) {
        Oop item = arrayHeader->slotAt(i);
        if (item.isNil() || !item.isObject()) continue;
        if (!isValidPointer(item)) continue;

        ObjectHeader* itemHeader = item.asObjectPtr();
        size_t slotCount = itemHeader->slotCount();
        if (slotCount < 2 || slotCount > 100) continue;

        Oop key = fetchPointer(0, item);
        if (!key.isObject() || key.isNil()) continue;
        if (!isValidPointer(key)) continue;

        ObjectHeader* keyHeader = key.asObjectPtr();
        if (!keyHeader->isBytesObject()) continue;

        if (symbolEquals(key, name.c_str())) {
            storePointer(1, item, value);
            return true;
        }
    }

    // Globals in Pharo are GlobalVariable bindings, not plain Associations.
    // GlobalVariable is a LiteralVariable with inst vars #(name value) --
    // instSize 2, the same slot layout as Association's #(key value) -- so
    // the storePointer(0/1) below is correct for either.  Using Association
    // here is what made ReleaseTest>>testAllGlobalBindingAreGlobalVariables
    // report `an Array(#Display->Form(1024x768x32)) should have been empty`.
    // Fall back to Association only for images with no GlobalVariable.
    Oop assocClass = findGlobal("GlobalVariable");
    if (assocClass.isNil() || !assocClass.isObject()) {
        assocClass = findGlobal("Association");
    }
    if (assocClass.isNil()) {
        return false;
    }

    uint32_t assocClassIdx = indexOfClass(assocClass);
    if (assocClassIdx == 0) {
        assocClassIdx = registerClass(assocClass);
    }
    if (assocClassIdx == 0) {
        return false;
    }

    // Reuse the image's interned symbol when there is one.  Symbols are
    // unique, so a freshly allocated #name would compare ~= to every #name
    // already compiled into the image.
    Oop symbolObj = lookupSymbol(name);
    if (!symbolObj.isObject() || symbolObj.isNil()) {
        // Nothing interned — allocate.  The class MUST be ByteSymbol, not
        // Symbol: `Symbol` is abstract, with instSpec format 0 (no indexable
        // fields), so an instance of it is indexed as a non-indexable object
        // and `at:` answers a SmallInteger where the image expects a
        // Character.  That is how the #Display globals key installed here
        // came to raise `68 doesNotUnderstand: #asciiValue` inside
        // Symbol>>isLiteralSymbol.  lookupSymbol() above already picks
        // ByteSymbol in preference to Symbol; keep the two consistent.
        Oop symbolClass = findGlobal("ByteSymbol");
        if (symbolClass.isNil() || !symbolClass.isObject()) {
            symbolClass = findGlobal("Symbol");
        }
        if (symbolClass.isNil()) {
            return false;
        }
        uint32_t symbolClassIdx = indexOfClass(symbolClass);
        if (symbolClassIdx == 0) {
            return false;
        }
        symbolObj = allocateBytes(symbolClassIdx, name.size());
        if (symbolObj.isNil()) {
            return false;
        }
        ObjectHeader* symHdr = symbolObj.asObjectPtr();
        std::memcpy(symHdr->bytes(), name.c_str(), name.size());
    }

    Oop assocObj = allocateSlots(assocClassIdx, 2);
    if (assocObj.isNil()) {
        return false;
    }

    // Set association key and value
    storePointer(0, assocObj, symbolObj);
    storePointer(1, assocObj, value);

    // Find empty slot in dictionary array and add the association
    Oop nilObj = specialObject(SpecialObjectIndex::NilObject);

    for (size_t i = 0; i < arraySize; ++i) {
        Oop item = arrayHeader->slotAt(i);
        if (item.rawBits() == 0 || item.rawBits() == nilObj.rawBits()) {
            storePointer(i, arraySlot, assocObj);
            return true;
        }
    }

    return false;
}

Oop ObjectMemory::createStartupContext(Oop method, Oop receiver) {
    Oop contextClass = specialObject(SpecialObjectIndex::ClassMethodContext);
    if (contextClass.isNil()) {
        return nilObject_;
    }

    Oop methodHeader = fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) {
        return nilObject_;
    }

    int64_t headerBits = methodHeader.asSmallInteger();
    int numTemps = (headerBits >> 18) & 0x3F;
    int numLiterals = headerBits & 0x7FFF;

    size_t contextSize = 6 + numTemps + 32;  // 6 fixed + temps + stack

    uint32_t classIndex = indexOfClass(contextClass);
    if (classIndex == 0) {
        classIndex = const_cast<ObjectMemory*>(this)->registerClass(contextClass);
    }

    Oop context = allocateSlots(classIndex, contextSize, ObjectFormat::IndexableWithFixed);
    if (context.isNil()) {
        return nilObject_;
    }

    int initialPC = (1 + numLiterals) * 8 + 1;  // 1-based byte offset past header+literals

    storePointer(0, context, nil());                              // sender
    storePointer(1, context, Oop::fromSmallInteger(initialPC));   // pc
    storePointer(2, context, Oop::fromSmallInteger(numTemps)); // stackp (1-based index of top temp)
    storePointer(3, context, method);                              // method
    storePointer(4, context, nil());                               // closureOrNil
    storePointer(5, context, receiver);                            // receiver

    for (int i = 0; i < numTemps; ++i) {
        storePointer(6 + i, context, nil());
    }

    return context;
}

Oop ObjectMemory::createStartupContextWithArg(Oop method, Oop receiver, Oop arg) {
    Oop contextClass = specialObject(SpecialObjectIndex::ClassMethodContext);
    if (contextClass.isNil()) {
        return nilObject_;
    }

    Oop methodHeader = fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) {
        return nilObject_;
    }

    int64_t headerBits = methodHeader.asSmallInteger();
    int numTemps = (headerBits >> 18) & 0x3F;  // VMMaker: MethodHeaderTempCountShift=21, untagged=18, 6 bits
    int numArgs = (headerBits >> 24) & 0xF;  // Arguments are in bits 24-27
    int numLiterals = headerBits & 0x7FFF;

    (void)numArgs;  // May differ from 1 for some methods; proceed regardless

    // Context needs: 6 fixed slots + 1 arg + temps + some stack space
    size_t contextSize = 6 + numArgs + numTemps + 32;

    uint32_t classIndex = indexOfClass(contextClass);
    if (classIndex == 0) {
        classIndex = const_cast<ObjectMemory*>(this)->registerClass(contextClass);
    }

    Oop context = allocateSlots(classIndex, contextSize, ObjectFormat::IndexableWithFixed);
    if (context.isNil()) {
        return nilObject_;
    }

    int initialPC = (1 + numLiterals) * 8 + 1;

    storePointer(0, context, nil());                                      // sender
    storePointer(1, context, Oop::fromSmallInteger(initialPC));           // pc
    storePointer(2, context, Oop::fromSmallInteger(numTemps)); // stackp (1-based index of top temp)
    storePointer(3, context, method);                                      // method
    storePointer(4, context, nil());                                       // closureOrNil
    storePointer(5, context, receiver);                                    // receiver
    storePointer(6, context, arg);                                         // argument

    for (int i = 0; i < numTemps; ++i) {
        storePointer(6 + numArgs + i, context, nil());
    }

    return context;
}

// ===== OBJECT ACCESS =====

Oop ObjectMemory::fetchPointer(size_t index, Oop obj) const {
    if (!obj.isObject()) return nilObject_;

    if (!isValidPointer(obj)) {
        return nilObject_;
    }

    ObjectHeader* header = obj.asObjectPtr();

    // Out-of-bounds fetch answers nil BY DESIGN — VM code deliberately
    // probes optional slots past shorter shapes (e.g. classNameOf reading
    // the name slot on a 6-slot Metaclass).  Do NOT add a tripwire here:
    // a 2026-07-04 attempt logged 50 false positives per run from those
    // legitimate probes.  Stores are different — see storePointer below.
    if (index >= header->slotCount()) return nilObject_;

    return header->slotAt(index);
}

void ObjectMemory::storePointer(size_t index, Oop obj, Oop value) {
    if (!obj.isObject()) return;
    ObjectHeader* header = obj.asObjectPtr();
    if (index >= header->slotCount()) {
        // Out-of-bounds store is DROPPED — loud, never silent (see
        // fetchPointer above; a dropped store corrupts image state).
        static int storeOobLog = 0;
        if (storeOobLog++ < 50) {
            fprintf(stderr, "[STORE-OOB] storePointer idx=%zu >= slots=%zu "
                    "obj=0x%llx cls=%u val=0x%llx — store DROPPED\n",
                    index, (size_t)header->slotCount(),
                    (unsigned long long)obj.rawBits(), header->classIndex(),
                    (unsigned long long)value.rawBits());
        }
        return;
    }

    if (isOld(obj) && value.isObject() && isYoung(value)) {
        rememberObject(obj);
    }

    // PHARO_SENDER_TRIPWIRE=1: diagnose NLR-over-walk under JIT (bug 14 family).
    // Logs every write that nils a context's sender slot (slot 0) when the
    // object currently has a non-nil slot 0 and context-shaped slot 3.
    // Normal NLR walks 2-5 contexts; walks of 20+ indicate the NLR escaped
    // its home context and is running into scheduler / sibling-process
    // contexts, which causes the cascade where 6 processes all end up with
    // sender=nil and then terminate.  See docs/jit-uncovered-bugs.md bug 14.
    if (GET_DEBUG_BOOL(PHARO_SENDER_TRIPWIRE) && index == 0 && value.isNil() && header->slotCount() >= 6) {
        Oop oldSender = header->slots()[0];
        Oop maybeMethod = header->slots()[3];
        if (!oldSender.isNil() && oldSender.rawBits() != 0 &&
            maybeMethod.isObject() && maybeMethod.rawBits() > 0x10000) {
            fprintf(stderr, "[SENDER-NIL] ctx=0x%llx oldSender=0x%llx "
                            "method=0x%llx slots=%zu\n",
                    (unsigned long long)obj.rawBits(),
                    (unsigned long long)oldSender.rawBits(),
                    (unsigned long long)maybeMethod.rawBits(),
                    (size_t)header->slotCount());
        }
    }

    // Run-formation forensics (PHARO_WATCH_OLDOFF armed): an object ref
    // written into an Array slot whose two predecessors already hold the
    // SAME ref — the smear signature — with a native backtrace naming the
    // writer.  storePointer is the choke point for all C++ writers.
    if (__builtin_expect(GET_DEBUG_INT(PHARO_WATCH_OLDOFF) >= 0, 0)) {
        extern uint64_t g_scavengeCount;
        if (g_scavengeCount >= 10 && index >= 2 &&
            value.isObject() && value.rawBits() > 0x10000 && !value.isNil() &&
            header->format() == ObjectFormat::Indexable) {
            Oop* sl = header->slots();
            if (sl[index - 1].rawBits() == value.rawBits() &&
                sl[index - 2].rawBits() == value.rawBits()) {
                static int runFormN = 0;
                if (++runFormN <= 8) {
                    fprintf(stderr,
                        "[RUN-FORM] arr=%p idx=%zu val=%llx scav=%llu\n",
                        (void*)header, index,
                        (unsigned long long)value.rawBits(),
                        (unsigned long long)g_scavengeCount);
                    void dumpCxxBacktrace(const char* tag);
                    dumpCxxBacktrace("RUN-FORM");
                }
            }
        }
    }

    header->slotAtPut(index, value);
    // Shadow-slot detector (PHARO_SHADOW_SLOTS): track the write.
    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS), 0)) {
        shadowStore(obj.rawBits(), index, value.rawBits(), 2);
    }
}

uint8_t ObjectMemory::fetchByte(size_t index, Oop obj) const {
    if (!obj.isObject()) return 0;
    ObjectHeader* header = obj.asObjectPtr();
    // Byte access is valid for ANY non-pointer indexable object (bytes, words,
    // shorts) — byteAt() only requires !isPointersObject() && index<byteSize().
    // The old !isBytesObject() guard (Indexable8 only) made byte-level file I/O
    // silently READ 0 into WordArray/Bitmap/FloatArray/ShortArray buffers:
    // primitiveFileRead reported a full read but stored nothing -> buffer left
    // zeroed -> Fuel read encoded-reference index 0 -> SubscriptOutOfBounds
    // (FLBinaryFileStream testWordArray/testBitmap).  byteSize() bounds words/shorts
    // correctly (8 for a 2-element WordArray).  2026-06-20 fix.
    if (header->isPointersObject()) return 0;
    if (index >= header->byteSize()) return 0;
    return header->byteAt(index);
}

void ObjectMemory::storeByte(size_t index, Oop obj, uint8_t value) {
    if (!obj.isObject()) return;
    ObjectHeader* header = obj.asObjectPtr();
    // See fetchByte: accept any non-pointer indexable object (bytes/words/shorts);
    // the old !isBytesObject() guard made primitiveFileWrite silently WRITE nothing
    // from a WordArray/Bitmap/FloatArray buffer.  2026-06-20 fix.
    if (header->isPointersObject()) return;
    if (index >= header->byteSize()) return;
    header->byteAtPut(index, value);
}

uint32_t ObjectMemory::fetchWord32(size_t index, Oop obj) const {
    if (!obj.isObject()) return 0;
    ObjectHeader* header = obj.asObjectPtr();
    uint32_t* words = reinterpret_cast<uint32_t*>(header->bytes());
    return words[index];
}

void ObjectMemory::storeWord32(size_t index, Oop obj, uint32_t value) {
    if (!obj.isObject()) return;
    ObjectHeader* header = obj.asObjectPtr();
    uint32_t* words = reinterpret_cast<uint32_t*>(header->bytes());
    words[index] = value;
}

uint64_t ObjectMemory::fetchWord64(size_t index, Oop obj) const {
    if (!obj.isObject()) return 0;
    ObjectHeader* header = obj.asObjectPtr();
    uint64_t* words = reinterpret_cast<uint64_t*>(header->bytes());
    return words[index];
}

void ObjectMemory::storeWord64(size_t index, Oop obj, uint64_t value) {
    if (!obj.isObject()) return;
    ObjectHeader* header = obj.asObjectPtr();
    uint64_t* words = reinterpret_cast<uint64_t*>(header->bytes());
    words[index] = value;
}

size_t ObjectMemory::slotCountOf(Oop obj) const {
    if (!obj.isObject()) return 0;
    return obj.asObjectPtr()->slotCount();
}

size_t ObjectMemory::fixedFieldCountOf(ObjectHeader* obj) const {
    uint32_t classIdx = obj->classIndex();
    if (classIdx == 0 || classIdx >= classTable_.size()) return 0;
    Oop classOop = classTable_[classIdx];
    if (!classOop.isObject()) return 0;
    ObjectHeader* classHdr = classOop.asObjectPtr();
    if (classHdr->slotCount() < 3) return 0;
    Oop instSpec = classHdr->slotAt(2);  // slot 2 = instance specification
    if (instSpec.isSmallInteger()) {
        return static_cast<size_t>(instSpec.asSmallInteger() & 0xFFFF);
    }
    return 0;
}

size_t ObjectMemory::fixedFieldCountOf(Oop obj) const {
    if (!obj.isObject()) return 0;
    return fixedFieldCountOf(obj.asObjectPtr());
}

std::string ObjectMemory::oopToString(Oop obj) const {
    if (!obj.isObject() || obj.rawBits() < 0x10000) return "";
    ObjectHeader* hdr = obj.asObjectPtr();
    if (!hdr->isBytesObject()) return "";
    size_t sz = hdr->byteSize();
    if (sz > 4096) return "";  // Guard against corrupted headers
    return std::string(reinterpret_cast<const char*>(hdr->bytes()), sz);
}

size_t ObjectMemory::numLiteralsOf(Oop method) const {
    if (!method.isObject()) return 0;
    ObjectHeader* hdr = method.asObjectPtr();
    if (hdr->slotCount() < 1) return 0;
    Oop header = hdr->slotAt(0);
    if (!header.isSmallInteger()) return 0;
    return static_cast<size_t>(header.asSmallInteger() & 0x7FFF);
}

std::string ObjectMemory::selectorOf(Oop method) const {
    size_t numLits = numLiteralsOf(method);
    if (numLits < 2) return "?";
    Oop sel = fetchPointer(numLits - 1, method);
    std::string name = oopToString(sel);
    if (!name.empty()) return name;
    // Penultimate is not a Symbol — likely an AdditionalMethodState whose
    // slot 1 holds the real selector.
    //
    // isValidPointer, not just isObject: selectorOf is a DIAGNOSTIC helper
    // called from compile paths, trace gates and crash handlers, i.e. from
    // exactly the places that already suspect something is wrong.  It has
    // to be total.  fetchPointer above bounds-checks the slot read, but
    // this branch dereferences whatever it answered, and a method whose
    // literal happens to be a bogus pointer would fault here instead of
    // answering "?".
    if (sel.isObject() && sel.rawBits() >= 0x10000 && isValidPointer(sel)) {
        ObjectHeader* amsHdr = sel.asObjectPtr();
        if (amsHdr->slotCount() >= 2) {
            Oop amsSel = amsHdr->slotAt(1);
            std::string amsName = oopToString(amsSel);
            if (!amsName.empty()) return amsName;
        }
    }
    return "?";
}

bool ObjectMemory::patchClassMethodToReturnSelf(Oop classObj, const char* selectorName) {
    if (!classObj.isObject()) return false;

    // Get the metaclass (class of the class object)
    Oop metaclass = classOf(classObj);
    if (!metaclass.isObject()) return false;

    // Get method dictionary (slot 1 of metaclass)
    Oop methodDict = fetchPointer(1, metaclass);
    if (!methodDict.isObject()) return false;

    ObjectHeader* mdHeader = methodDict.asObjectPtr();
    size_t mdSlots = mdHeader->slotCount();

    // Scan keys (at slots 2..mdSlots-1 of the MethodDictionary)
    size_t selectorLen = strlen(selectorName);
    for (size_t i = 2; i < mdSlots; i++) {
        Oop key = mdHeader->slotAt(i);
        if (!key.isObject() || key == nil()) continue;

        ObjectHeader* keyHdr = key.asObjectPtr();
        if (!keyHdr->isBytesObject()) continue;
        if (keyHdr->byteSize() != selectorLen) continue;
        if (memcmp(keyHdr->bytes(), selectorName, selectorLen) != 0) continue;

        // Found the selector — get the method from values array (slot 1)
        Oop values = fetchPointer(1, methodDict);
        if (!values.isObject()) return false;

        ObjectHeader* valHdr = values.asObjectPtr();
        size_t valueIdx = i - 2;
        if (valueIdx >= valHdr->slotCount()) return false;

        Oop method = valHdr->slotAt(valueIdx);
        if (!method.isObject()) return false;

        // Patch first bytecode to 0x58 (Sista V1 "return receiver")
        size_t numLits = numLiteralsOf(method);
        size_t bytecodeOffset = (1 + numLits) * sizeof(Oop);  // byte offset in object
        ObjectHeader* methodHdr = method.asObjectPtr();
        uint8_t* bytes = methodHdr->bytes();
        if (bytecodeOffset < methodHdr->byteSize()) {
            bytes[bytecodeOffset] = 0x58;  // return receiver (self)
            return true;
        }
        return false;
    }
    return false;
}

std::string ObjectMemory::nameOfClass(Oop classObj) const {
    if (!classObj.isObject()) return "?";
    if (!isValidPointer(classObj)) return "?(invalid)";
    ObjectHeader* clsHdr = classObj.asObjectPtr();
    size_t clsSlots = clsHdr->slotCount();

    Oop nameOop;
    if (clsSlots == 6) {
        // Metaclass — get thisClass from slot 5, then name from slot 6
        Oop thisClass = clsHdr->slotAt(5);
        if (!thisClass.isObject()) return "?";
        if (!isValidPointer(thisClass)) return "?(invalid-tc)";
        ObjectHeader* tcHdr = thisClass.asObjectPtr();
        if (tcHdr->slotCount() < 7) return "?";
        nameOop = tcHdr->slotAt(6);
    } else if (clsSlots >= 7) {
        nameOop = clsHdr->slotAt(6);
    } else {
        return "?";
    }

    // Validate name oop before dereferencing — the field may hold a stale/
    // bogus pointer, especially for classes that have been replaced or
    // compacted.  oopToString will dereference unconditionally.
    if (!nameOop.isObject()) return "?";
    if (!isValidPointer(nameOop)) return "?(invalid-name)";

    std::string name = oopToString(nameOop);
    if (name.empty()) return "?";
    if (clsSlots == 6) return name + " class";
    return name;
}

std::string ObjectMemory::classNameOf(Oop obj) const {
    if (obj.isNil()) return "nil";
    if (!obj.isObject()) {
        if (obj.isSmallInteger()) return "SmallInteger";
        if (obj.isCharacter()) return "Character";
        if (obj.isSmallFloat()) return "SmallFloat64";
        return "?";
    }
    return nameOfClass(classOf(obj));
}

size_t ObjectMemory::byteSizeOf(Oop obj) const {
    if (!obj.isObject()) return 0;
    return obj.asObjectPtr()->byteSize();
}

size_t ObjectMemory::totalSizeOf(Oop obj) const {
    if (!obj.isObject()) return 0;
    return obj.asObjectPtr()->totalSize();
}

// ===== OBJECT QUERIES =====

bool ObjectMemory::isYoung(Oop obj) const {
    if (!obj.isObject()) return false;
    auto p = reinterpret_cast<const uint8_t*>(obj.asObjectPtr());
    return p >= newSpaceStart_ && p < newSpaceEnd_;
}

bool ObjectMemory::isOld(Oop obj) const {
    if (!obj.isObject()) return false;
    auto p = reinterpret_cast<const uint8_t*>(obj.asObjectPtr());
    return p >= oldSpaceStart_ && p < oldSpaceEnd_;
}

bool ObjectMemory::isPerm(Oop obj) const {
    if (!obj.isObject()) return false;
    auto p = reinterpret_cast<const uint8_t*>(obj.asObjectPtr());
    return p >= permSpaceStart_ && p < permSpaceEnd_;
}

bool ObjectMemory::isPinned(Oop obj) const {
    if (!obj.isObject()) return false;
    return obj.asObjectPtr()->isPinned();
}

bool ObjectMemory::isImmutable(Oop obj) const {
    if (!obj.isObject()) return true;  // Immediates are immutable
    return obj.asObjectPtr()->isImmutable();
}

bool ObjectMemory::isRemembered(Oop obj) const {
    if (!obj.isObject()) return false;
    return obj.asObjectPtr()->isRemembered();
}

bool ObjectMemory::isValidHeapAddress(void* addr) const {
    uint8_t* p = static_cast<uint8_t*>(addr);
    return (p >= permSpaceStart_ && p < permSpaceEnd_) ||
           (p >= oldSpaceStart_ && p < oldSpaceFree_) ||
           (p >= newSpaceStart_ && p < newSpaceEnd_);
}

bool ObjectMemory::isValidObject(Oop obj) const {
    if (!obj.isObject()) return true;  // Immediates are valid
    void* ptr = obj.asObjectPtr();
    return isValidHeapAddress(ptr);
}

// ===== OBJECT MODIFICATION =====

void ObjectMemory::ensurePinArena() {
    if (pinArenaStart_) return;
    if (!GET_DEBUG_BOOL(PHARO_PIN_RELOCATE)) return;
    if (!oldSpaceStart_ || !oldSpaceFree_ || !oldSpaceEnd_) return;
    constexpr size_t kPinArenaBytes = 256 * 1024;
    if (oldSpaceFree_ + kPinArenaBytes > oldSpaceEnd_) return;
    pinArenaStart_ = oldSpaceFree_;
    pinArenaFree_  = oldSpaceFree_;
    pinArenaEnd_   = oldSpaceFree_ + kPinArenaBytes;
    oldSpaceFree_ += kPinArenaBytes;
    std::memset(pinArenaStart_, 0, kPinArenaBytes);
    if (GET_DEBUG_BOOL(PHARO_GC_LOG) || GET_DEBUG_BOOL(PHARO_PIN_STATS)) {
        fprintf(stderr, "[PIN-ARENA] carved %zu KB at +%td KB\n",
                kPinArenaBytes / 1024,
                (ptrdiff_t)((pinArenaStart_ - oldSpaceStart_) / 1024));
        fflush(stderr);
    }
}

ObjectHeader* ObjectMemory::allocatePinnedLow(size_t size) {
    // Pinned objects are immovable, so WHERE they land decides how much of old
    // space sliding compaction can ever reclaim: a pin high in the heap makes
    // the destination finger jump to it and abandon everything below.  Six
    // 32-byte FFI buffers cost 146 MB on a NeoJSON image, 12x on XMLParser.
    //
    // The free list cannot supply a low home during a load -- it is built by
    // rebuildFreeListAfterCompact, which only runs after a fullGC, and the
    // pins are placed long before that (measured: tenuredLow=0, noChunk=186).
    // So carve a small arena ONCE, the first time a pin needs a home, and put
    // every later pin in it.  It sits at whatever oldSpaceFree_ was then --
    // early in the run, hence low -- and everything allocated afterwards is
    // above it, so the finger only ever abandons the arena itself.
    if (size == 0) return nullptr;
    if (!pinArenaStart_) {
        ensurePinArena();                 // normally already done at image load
        if (!pinArenaStart_) return nullptr;
    }
    if (pinArenaFree_ + size > pinArenaEnd_) return nullptr;  // full: caller bumps
    ObjectHeader* out = reinterpret_cast<ObjectHeader*>(pinArenaFree_);
    pinArenaFree_ += size;
    return out;
}

extern "C" size_t g_pinSkipNotObj, g_pinSkipYoung, g_pinSkipPinned,
                  g_pinSkipNoChunk, g_pinSkipNotLower;
extern "C" size_t g_pinTenuredLow;

Oop ObjectMemory::relocateToLowSpace(Oop original) {
    // Spur does not pin an object where it happens to sit: pinObject COPIES it
    // and become:s it, because at pin time nothing holds the address yet --
    // taking the address is what the caller is about to do.  We pinned in
    // place, so six 32-byte FFI buffers scattered between +166 MB and +208 MB
    // stranded 146 MB that sliding compaction could never reclaim (12x on the
    // XMLParser image).  Moving the object low first keeps the pins out of the
    // compactor's way.
    if (!original.isObject()) { g_pinSkipNotObj++; return original; }
    ObjectHeader* src = original.asObjectPtr();
    if (!isOldObject(src)) { g_pinSkipYoung++; return original; }
    if (src->isPinned()) { g_pinSkipPinned++; return original; }

    const bool isOverflow = src->hasOverflowSlots();
    uint8_t* srcBase = reinterpret_cast<uint8_t*>(src) - (isOverflow ? 8 : 0);
    const size_t size = src->totalSize();

    // Deliberately NOT the pin arena.  Feeding this path arena memory made the
    // VM crash in 3 s (rc=133) on a NeoJSON load: relocateToLowSpace runs
    // inside primitivePin and finishes with becomeForward, a full-heap walk
    // that rewrites references while the interpreter still holds oops for the
    // primitive in flight.  The tenure-time placement below is safe because it
    // runs inside scavenge, where that is already the contract.  Leave this on
    // the free list only -- it is a no-op during a load, which is harmless.
    ObjectHeader* dstBase = allocateFromFreeList(size);
    if (!dstBase) { g_pinSkipNoChunk++; return original; }
    uint8_t* dstBytes = reinterpret_cast<uint8_t*>(dstBase);
    if (dstBytes >= srcBase) {
        // Only a DOWNWARD move helps; anything else just churns.  The chunk is
        // already unlinked, so hand it back rather than leaking it.
        makeFreeChunk(dstBytes, size, /*zeroBody=*/false);
        addToFreeList(reinterpret_cast<ObjectHeader*>(dstBytes), size);
        g_pinSkipNotLower++;
        return original;
    }

    std::memcpy(dstBytes, srcBase, size);
    ObjectHeader* copy = reinterpret_cast<ObjectHeader*>(
        dstBytes + (isOverflow ? 8 : 0));
    copy->setPinned(true);
    copy->setMarked(src->isMarked());
    copy->setRemembered(false);

    Oop copyOop = oopFromPointer(copy);

    // memcpy duplicated the slots verbatim; if any point into new space the
    // copy needs the old->young barrier that storePointer would have applied.
    if (src->isPointersObject()) {
        size_t np = pointerSlotsOf(copy);
        Oop* slots = copy->slots();
        for (size_t i = 0; i < np; ++i) {
            if (slots[i].isObject() && isYoung(slots[i])) {
                rememberObject(copyOop);
                break;
            }
        }
    }

    becomeForward(original, copyOop);
    return copyOop;
}

void ObjectMemory::pinObject(Oop obj) {
    if (obj.isObject()) {
        obj.asObjectPtr()->setPinned(true);
    }
}

void ObjectMemory::makeImmutable(Oop obj) {
    if (obj.isObject()) {
        obj.asObjectPtr()->setImmutable(true);
    }
}

bool ObjectMemory::become(Oop obj1, Oop obj2) {
    if (!obj1.isObject() || !obj2.isObject()) {
        return false;  // Can only become: heap objects
    }

    // This is a costly operation - must scan entire heap
    allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;
        ObjectHeader* header = obj.asObjectPtr();
        ObjectFormat format = header->format();

        // Skip non-pointer objects (byte/word/short arrays) - their slots are raw data, not Oops
        if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) return;
        if (format >= ObjectFormat::Indexable64 && format <= ObjectFormat::Indexable32Odd) return;
        if (format >= ObjectFormat::Indexable16 && format <= ObjectFormat::Indexable16_3) return;
        if (format >= ObjectFormat::Reserved6 && format <= ObjectFormat::Reserved8) return;

        size_t slots = header->slotCount();

        // For CompiledMethods, only scan the literal frame (pointer part)
        if (header->isCompiledMethod() && slots > 0) {
            Oop methodHeader = header->slotAt(0);
            if (methodHeader.isSmallInteger()) {
                size_t numLits = methodHeader.asSmallInteger() & 0x7FFF;
                slots = std::min(slots, numLits + 1);
            }
        }

        for (size_t i = 0; i < slots; ++i) {
            Oop slot = header->slotAt(i);
            if (slot == obj1) {
                header->slotAtPut(i, obj2);
            } else if (slot == obj2) {
                header->slotAtPut(i, obj1);
            }
        }
    });

    // CLASSES: instance dispatch resolves header classIndex through
    // classTable_ — a C++ vector the heap scan above cannot see.  Swap any
    // table entries so instances whose headers carry those class indices
    // dispatch through the exchanged objects (Spur parity).
    for (size_t i = 0; i < classTable_.size(); ++i) {
        if (classTable_[i] == obj1) classTable_[i] = obj2;
        else if (classTable_[i] == obj2) classTable_[i] = obj1;
    }

    return true;
}

bool ObjectMemory::becomeForward(Oop obj1, Oop obj2) {
    // Shadow-slot detector: identities are about to swap arbitrarily —
    // the (object, slot) keys cannot be remapped, so drop the table.
    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS), 0)) {
        shadowClear();
    }
    if (!obj1.isObject() || !obj2.isObject()) {
        return false;
    }

    extern uint64_t g_scavengeCount;
    size_t becomeReplacedCount = 0;
    const bool becomeTrace =
        GET_DEBUG_INT(PHARO_WATCH_OLDOFF) >= 0 && g_scavengeCount >= 10;

    allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;
        ObjectHeader* header = obj.asObjectPtr();
        ObjectFormat format = header->format();

        // Skip non-pointer objects (byte/word/short arrays) - their slots are raw data, not Oops
        if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) return;
        if (format >= ObjectFormat::Indexable64 && format <= ObjectFormat::Indexable32Odd) return; // 9, 10, 11
        if (format >= ObjectFormat::Indexable16 && format <= ObjectFormat::Indexable16_3) return;
        // Skip reserved formats
        if (format >= ObjectFormat::Reserved6 && format <= ObjectFormat::Reserved8) return;

        size_t slots = header->slotCount();

        // For CompiledMethods (format 24-31), only scan the literal frame (pointer part).
        // Slot 0 is the method header (SmallInteger), slots 1..numLiterals are literal pointers,
        // remaining slots contain raw bytecodes that should not be scanned.
        if (header->isCompiledMethod() && slots > 0) {
            Oop methodHeader = header->slotAt(0);
            if (methodHeader.isSmallInteger()) {
                size_t numLits = methodHeader.asSmallInteger() & 0x7FFF;
                // Scan header + literals only (slots 0..numLits)
                slots = std::min(slots, numLits + 1);
            }
        }

        for (size_t i = 0; i < slots; ++i) {
            if (header->slotAt(i) == obj1) {
                header->slotAtPut(i, obj2);
                becomeReplacedCount++;
                if (becomeTrace && becomeReplacedCount <= 12) {
                    fprintf(stderr,
                        "[BECOME-SLOT] holder=%p holderCls=%u slot=%zu\n",
                        (void*)header, header->classIndex(), i);
                }
            }
        }
    });

    if (becomeTrace) {
        static int becomeN = 0;
        if (++becomeN <= 60) {
            fprintf(stderr,
                "[BECOME] #%d scav=%llu obj1=%llx obj2=%llx replaced=%zu\n",
                becomeN, (unsigned long long)g_scavengeCount,
                (unsigned long long)obj1.rawBits(),
                (unsigned long long)obj2.rawBits(), becomeReplacedCount);
        }
    }

    // CLASSES: instance dispatch resolves header classIndex through
    // classTable_ — a C++ vector the heap scan above cannot see.  If obj1
    // occupies class-table slots, redirect them so instances whose headers
    // carry that classIndex dispatch through obj2.  Spur parity: a class's
    // identityHash IS its table index and the become prims copy the hash,
    // so indexOfClass(obj2) stays coherent.  GHost/Mocketry real-object
    // stubs depend on this exact dance — they becomeForward: an anonymous
    // helper class into a mutation object; without the redirect the victim
    // keeps dispatching into the empty helper and stubs silently never
    // intercept (gitlab/bitbucket API suites, 90 tests, jitpkg 2026-07-06).
    size_t classTableHits = 0;
    for (size_t i = 0; i < classTable_.size(); ++i) {
        if (classTable_[i] == obj1) { classTable_[i] = obj2; classTableHits++; }
    }

    // SAFETY NET (PHARO_BECOME_FORWARDER): the scan-and-replace above finds
    // refs to obj1 in the heap + classTable, and scanStackReplace/forEachRoot
    // covers VM roots.  But a ref that GC's mark-phase keeps alive yet the
    // become-scan misses (e.g. a JIT operand under materialization) leaves
    // obj1 as a STALE VALID object — a reshaped-away instance the next
    // allInstances re-finds at its old (smaller) size (SlotIntegration
    // trait ivar-add: z-rebuild reads a 1-slot husk whose class is instSize
    // 2 -> SubscriptOutOfBounds -> rebuild aborts).  Turning obj1 into a
    // forwarder to obj2 makes any missed ref resolve via followForwarded and
    // makes allInstances (which skips isForwarded) stop re-finding the husk.
    if (!GET_DEBUG_BOOL(PHARO_NO_BECOME_FORWARDER)
            && obj1.isObject() && obj1.rawBits() != obj2.rawBits()
            && obj1.asObjectPtr()->slotCount() >= 1
            && !obj1.asObjectPtr()->isForwarded()) {
        ObjectHeader* h1 = obj1.asObjectPtr();
        h1->slotAtPut(0, obj2);
        h1->setClassIndex(ObjectHeader::ForwardedClassIndex);
    }

    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_TRACE_BECOME), 0)) {
        auto sz = [&](Oop o){ return o.isObject() ? o.asObjectPtr()->slotCount() : 0; };
        extern uint64_t g_stepNum;
        fprintf(stderr, "[BECOME-FWD] step=%llu %s(%zu slots)@%llx -> %s(%zu slots)@%llx  heapRefs=%zu ctHits=%zu\n",
                (unsigned long long)g_stepNum,
                classNameOf(obj1).c_str(), sz(obj1), (unsigned long long)obj1.rawBits(),
                classNameOf(obj2).c_str(), sz(obj2), (unsigned long long)obj2.rawBits(),
                becomeReplacedCount, classTableHits);
    }

    return true;
}

bool ObjectMemory::becomeForwardAll(const std::unordered_map<uint64_t, Oop>& map) {
    if (map.empty()) return true;

    // Shadow-slot detector: identities are about to change arbitrarily — the
    // (object, slot) keys cannot be remapped, so drop the table. Once for the
    // whole batch rather than once per pair.
    if (__builtin_expect(GET_DEBUG_BOOL(PHARO_SHADOW_SLOTS), 0)) {
        shadowClear();
    }

    extern uint64_t g_scavengeCount;
    size_t becomeReplacedCount = 0;
    const bool becomeTrace =
        GET_DEBUG_INT(PHARO_WATCH_OLDOFF) >= 0 && g_scavengeCount >= 10;

    // ONE heap scan covering every pair. Same traversal and the same format
    // guards as becomeForward above; only the test changes, from a comparison
    // against a single oop to a lookup in the map.
    allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;
        ObjectHeader* header = obj.asObjectPtr();
        ObjectFormat format = header->format();

        // Skip non-pointer objects (byte/word/short arrays) - their slots are raw data, not Oops
        if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) return;
        if (format >= ObjectFormat::Indexable64 && format <= ObjectFormat::Indexable32Odd) return; // 9, 10, 11
        if (format >= ObjectFormat::Indexable16 && format <= ObjectFormat::Indexable16_3) return;
        // Skip reserved formats
        if (format >= ObjectFormat::Reserved6 && format <= ObjectFormat::Reserved8) return;

        size_t slots = header->slotCount();

        // For CompiledMethods (format 24-31), only scan the literal frame.
        if (header->isCompiledMethod() && slots > 0) {
            Oop methodHeader = header->slotAt(0);
            if (methodHeader.isSmallInteger()) {
                size_t numLits = methodHeader.asSmallInteger() & 0x7FFF;
                slots = std::min(slots, numLits + 1);
            }
        }

        for (size_t i = 0; i < slots; ++i) {
            Oop slot = header->slotAt(i);
            if (!slot.isObject()) continue;
            auto it = map.find(slot.rawBits());
            if (it == map.end()) continue;
            header->slotAtPut(i, it->second);
            becomeReplacedCount++;
        }
    });

    // CLASSES: the same classTable_ redirect becomeForward performs, and for
    // the same reason — instance dispatch resolves header classIndex through
    // classTable_, a C++ vector the heap scan cannot see. One pass over the
    // table serves the whole batch.
    size_t classTableHits = 0;
    for (size_t i = 0; i < classTable_.size(); ++i) {
        if (!classTable_[i].isObject()) continue;
        auto it = map.find(classTable_[i].rawBits());
        if (it != map.end()) { classTable_[i] = it->second; classTableHits++; }
    }

    // SAFETY NET (PHARO_BECOME_FORWARDER): the same forwarder becomeForward
    // installs, so a reference the scan missed still resolves via
    // followForwarded and allInstances stops re-finding the husk.
    //
    // Installed only after the scan and the class-table pass, so every pair
    // sees the pre-become heap. Doing it as becomeForward does, interleaved
    // per pair, would let pair k's forwarder overwrite slot 0 of an object
    // that pair k+1 still has to scan.
    if (!GET_DEBUG_BOOL(PHARO_NO_BECOME_FORWARDER)) {
        for (const auto& entry : map) {
            Oop obj1 = Oop::fromRawBits(entry.first);
            Oop obj2 = entry.second;
            if (!obj1.isObject() || obj1.rawBits() == obj2.rawBits()) continue;
            ObjectHeader* h1 = obj1.asObjectPtr();
            if (h1->slotCount() < 1 || h1->isForwarded()) continue;
            h1->slotAtPut(0, obj2);
            h1->setClassIndex(ObjectHeader::ForwardedClassIndex);
        }
    }

    if (becomeTrace || __builtin_expect(GET_DEBUG_BOOL(PHARO_TRACE_BECOME), 0)) {
        extern uint64_t g_stepNum;
        fprintf(stderr,
                "[BECOME-FWD-ALL] step=%llu pairs=%zu heapRefs=%zu ctHits=%zu\n",
                (unsigned long long)g_stepNum, map.size(),
                becomeReplacedCount, classTableHits);
    }

    return true;
}

// ===== IDENTITY HASH =====

uint32_t ObjectMemory::identityHashOf(Oop obj) {
    if (obj.isSmallInteger()) {
        // SmallIntegers use their value as hash
        return static_cast<uint32_t>(obj.asSmallInteger() & 0x3FFFFF);
    }
    if (obj.isCharacter()) {
        return obj.asCharacter() & 0x3FFFFF;
    }
    if (obj.isSmallFloat()) {
        // SmallFloat identity hash: hash the raw bits
        uint64_t bits = obj.rawBits();
        return static_cast<uint32_t>((bits >> 32) ^ bits) & 0x3FFFFF;
    }
    if (!obj.isObject()) {
        return 0;
    }

    ObjectHeader* header = obj.asObjectPtr();
    uint32_t hash = header->identityHash();
    if (hash != 0) return hash;

    // Class-instance detection: if obj's class is a known metaclass,
    // obj is itself a class.  Route through registerClass() for
    // sequential index allocation (Spur convention) rather than
    // generating a random hash that could collide with an existing
    // class's table index.
    //
    // knownMetaclassIndices_ is populated by registerClass() each
    // time a class is registered — it adds classOf(thatClass)'s
    // index, building up the set of "things whose instances are
    // classes".  Bootstrapped during image load.
    uint32_t objClassIdx = header->classIndex();
    if (objClassIdx != 0
        && knownMetaclassIndices_.count(objClassIdx) > 0) {
        return registerClass(obj);
    }

    // Non-class object — assign a random hash via LCG.
    hash = generateHash();
    header->setIdentityHash(hash);
    // Blocker #4: detect REGENERATION of a symbol's identityHash (its header
    // hash was 0 — either never set, or ZEROED by a wild write).  Log per-symbol
    // counts; a test-key symbol regenerating >1 time = its header was corrupted.
    if (GET_DEBUG_BOOL(PHARO_T1_TRACE_MOD) && header->isBytesObject()) {
        size_t n = header->byteSize();
        const uint8_t* by = header->bytes();
        std::string s((const char*)by, n < 40 ? n : 40);
        if (s.find("DefinedIn") != std::string::npos
                || s == "SystemOrganization") {
            fprintf(stderr, "[IDHASH-GEN] sym=%s obj=0x%llx newHash=%u\n",
                    s.c_str(), (unsigned long long)obj.rawBits(), hash);
        }
    }
    return hash;
}

void ObjectMemory::ensureIdentityHash(Oop obj) {
    identityHashOf(obj);  // Side effect: generates hash if needed
}

uint32_t ObjectMemory::generateHash() {
    // Use D.H. Lehmer's linear congruential generator (same as official Pharo VM)
    // lastHash = lastHash * 16807 (which is 7^5)
    // Adding top bits gives better spread.
    //
    // Class detection in identityHashOf() (knownMetaclassIndices_)
    // routes MOST class objects to registerClass() for sequential
    // index allocation.  But some metaclass-of-metaclass corner
    // cases (Trait class, ClassTrait, etc.) may slip through if
    // their metaclass isn't yet in the set.  Keep the
    // collision-avoidance dance as defence in depth — at ~0.12 %
    // skip rate it's negligible perf cost and catches anything
    // identityHashOf misses.
    uint32_t hash;
    do {
        lastHash_ = (lastHash_ * 16807) & 0xFFFFFFFF;
        hash = (lastHash_ + (lastHash_ >> 4)) & 0x3FFFFF;  // 22-bit hash
    } while (hash == 0
             || (hash < classTable_.size()
                 && classTable_[hash].isObject()
                 && !classTable_[hash].isNil()));
    return hash;
}

// ===== GARBAGE COLLECTION =====

uint64_t g_lastAllocAddr = 0;      // corpse forensics: last allocateSlots result
uint64_t g_lastAllocScav = 0;      // g_scavengeCount at that allocation
uint32_t g_lastAllocClassIdx = 0;  // its class index
uint64_t g_scavengeCount = 0;  // total scavenges (A/B diagnostic)
GCResult ObjectMemory::scavenge() {
    // PHARO_SCAV_QUARANTINE_AT: once eden is quarantined nothing young
    // exists anymore (allocation falls back to old space) — later scavenge
    // requests are no-ops, and running one would touch protected pages.
    static bool scavQuarantineActive = false;
    if (scavQuarantineActive) return GCResult{0, 0, 0};
    g_scavengeCount++;
    // Copying scavenge: tenure all reachable young objects to old
    // space, reset eden.  Unreachable young objects vanish when
    // eden is reset.
    //
    // Roots:
    //   1. Interpreter roots (via forEachRoot)
    //   2. Remembered set (old→young references)
    //   3. Class table + special objects
    //   4. Mourn queue, ephemeron list (carried across GCs)
    //
    // Forwarding: an old→new address map is kept in a side table
    // rather than overwriting headers, so young objects that get
    // skipped (unreachable) retain valid scanners-can-read state
    // until eden reset.
    auto start = std::chrono::steady_clock::now();
    GCResult result{0, 0, 0};

    // If eden is empty, nothing to do.
    if (edenFree_ == edenAllocBase_) {
        result.milliseconds = 0;
        return result;
    }

    size_t edenUsedBefore = static_cast<size_t>(edenFree_ - edenAllocBase_);
    static int scavCount = 0;
    int myScavId = ++scavCount;

    // IP ROUND-TRIP (2026-06-10): a scavenge MOVES young CompiledMethods
    // — including one the interpreter is CURRENTLY EXECUTING (a fresh
    // eval DoIt / freshly-compiled test method).  forEachRoot updates
    // method_/savedMethod oops, but instructionPointer_, the saved
    // frames' savedIP, and the live JITState ip are raw pointers into
    // the OLD eden copy.  Execution continues on the stale copy —
    // correct until eden refills and overwrites those bytes, then the
    // interpreter executes whatever the new objects put there (the
    // 'EVAL-RESULT=' silent-eval-loss / wrong-receiver-DNU family;
    // ~10% per layout, scavengesSoFar=3 signature).  fullGC has always
    // wrapped object motion in prepareForGC/afterGC; do the same here.
    // gcPrepared() guards the scavenge fullGC runs internally (ips are
    // already offsets then — re-preparing would destroy them).
    bool ipWrapped = interpreter_ && !interpreter_->gcPrepared();
    if (ipWrapped) interpreter_->prepareForGC();
    if (g_debug.gcEphDebug && myScavId <= 20) {
        fprintf(stderr, "[SCAV-%d] eden used=%zu KB old=%zu KB\n",
            myScavId, edenUsedBefore / 1024,
            (size_t)(oldSpaceFree_ - oldSpaceStart_) / 1024);
    }

    // Forwarding map: young header address → new (old-space) header address.
    std::unordered_map<ObjectHeader*, ObjectHeader*> forward;
    forward.reserve(1024);

    // Helper: if oop points to a young object, copy to old space and
    // return the new Oop.  Otherwise, return unchanged.
    auto tenureIfYoung = [&](Oop oop) -> Oop {
        if (!oop.isObject()) return oop;
        ObjectHeader* obj = oop.asObjectPtr();
        uint8_t* p = reinterpret_cast<uint8_t*>(obj);
        if (p < edenAllocBase_ || p >= edenFree_) return oop;
        auto it = forward.find(obj);
        if (it != forward.end()) return Oop::fromObject(it->second);

        // Copy to old space.  Use raw bytes so overflow-slot
        // prefix (if any) and header are preserved.
        size_t size = obj->totalSize();
        // totalSize already includes overflow prefix; obj-start is
        // obj minus 8 if hasOverflowSlots else obj.
        bool overflow = obj->hasOverflowSlots();
        uint8_t* srcStart = p - (overflow ? 8 : 0);
        size_t copySize = size;  // totalSize includes overflow word

        // A PINNED object tenured by bumping oldSpaceFree_ lands at the TOP
        // of old space, and a pin there strands everything below it from
        // sliding compaction permanently -- six 32-byte FFI buffers cost
        // 146 MB on a NeoJSON image and 12x on XMLParser
        // (docs/gc-oldspace-fragmentation-2026-08-22.md).  Tenuring is the
        // event that actually PLACES these buffers: measured, 439 of 625
        // newly-pinned objects are still in eden when pinned, so pin-time
        // relocation never sees them.  Give a pinned promotion a low
        // free-list chunk instead, so the pin lands out of the compactor's
        // way in the first place.
        uint8_t* destStart = nullptr;
        if (obj->isPinned()
                && GET_DEBUG_BOOL(PHARO_PIN_RELOCATE)
                && GET_DEBUG_BOOL(PHARO_OLDSPACE_FREELIST)) {
            ObjectHeader* low = allocatePinnedLow(copySize);
            if (!low) low = allocateFromFreeList(copySize);
            if (low) {
                destStart = reinterpret_cast<uint8_t*>(low);
                g_pinTenuredLow++;
            }
        }
        if (!destStart && oldSpaceFree_ + copySize > oldSpaceEnd_) {
            // Old-space OOM during tenure.  The old "bail — caller retries"
            // contract was FICTION (silent-cap audit 2026-07-03): every call
            // site detects tenure only via oop-changed, which is identical
            // for "wasn't young" and "OOM bail", and Phase 3 unconditionally
            // resets eden afterwards — so a silently-bailed live object was
            // WIPED and every reference to it dangled into reused eden
            // (delayed use-after-free instead of an OutOfMemory).  Until a
            // grow-old-space / low-space-signal path exists, fail LOUDLY at
            // the exact exhaustion point: a clean abort with a diagnosis
            // beats silent heap corruption in every scenario.
            fprintf(stderr,
                "[VM] FATAL: old space exhausted during scavenge tenure "
                "(need %zu bytes, %td free of %td). Live heap has outgrown "
                "the old-space reservation (MemoryConfig::oldSpaceMaxSize).\n",
                copySize,
                (ptrdiff_t)(oldSpaceEnd_ - oldSpaceFree_),
                (ptrdiff_t)(oldSpaceEnd_ - oldSpaceStart_));
            // Say whether the circuit breaker was even in play.  Working this
            // out after the fact cost an hour on 2026-09-02: the absence of a
            // [LOW-SPACE] line has three different meanings -- the image never
            // armed prim 125, the crossing was never observed, or it was
            // latched and never delivered -- and the log could not tell them
            // apart.  Now it can.
            fprintf(stderr,
                "[VM] low-space breaker at abort: threshold=%zu bytes (%s), "
                "crossing latched=%s\n",
                lowSpaceThresholdBytes_,
                lowSpaceThresholdBytes_ ? "armed by the image via prim 125"
                                        : "DISARMED — the image never installed "
                                          "a LowSpaceWatcher (bare eval mode does not)",
                lowSpaceCrossed_ ? "yes, but never delivered" : "no");
            dumpHeapCensus(25);   // say WHAT filled the heap before dying
            fflush(stderr);
            std::abort();
        }
        if (!destStart) {
            destStart = oldSpaceFree_;
            oldSpaceFree_ += copySize;
            noteOldSpaceAdvance();   // low-space breaker; see ObjectMemory.hpp
        }
        std::memcpy(destStart, srcStart, copySize);
        ObjectHeader* newHdr = reinterpret_cast<ObjectHeader*>(
            destStart + (overflow ? 8 : 0));
        forward[obj] = newHdr;
        // Smear forensics (PHARO_WATCH_OLDOFF armed): if a young pointer
        // array carried a >=4 run of one object ref INTO tenure, the run was
        // formed in eden before this scavenge.
        if (__builtin_expect(GET_DEBUG_INT(PHARO_WATCH_OLDOFF) >= 0, 0) &&
            newHdr->format() == ObjectFormat::Indexable) {
            Oop* sl = newHdr->slots();
            size_t n = newHdr->slotCount();
            size_t run = 1;
            static int tenureSmearN = 0;
            for (size_t i = 1; i < n; i++) {
                if (sl[i].rawBits() == sl[i - 1].rawBits() &&
                    sl[i].isObject() && sl[i].rawBits() > 0x10000 &&
                    !sl[i].isNil()) {
                    if (++run == 4 && tenureSmearN < 20) {
                        tenureSmearN++;
                        fprintf(stderr,
                            "[SMEAR-TENURE] scav=%llu youngArr=%p oldArr=%p slots=%zu "
                            "runVal=%llx at=[%zu]\n",
                            (unsigned long long)g_scavengeCount, (void*)obj,
                            (void*)newHdr, n,
                            (unsigned long long)sl[i].rawBits(), i);
                    }
                } else run = 1;
            }
        }
        return Oop::fromObject(newHdr);
    };

    // Phase 1: tenure all root-reachable young objects.  Collect
    // newly-tenured objects so we can scan their fields (which may
    // reference more young objects).
    std::vector<ObjectHeader*> scanQueue;
    scanQueue.reserve(1024);

    auto visitRoot = [&](Oop& oopRef) {
        Oop newOop = tenureIfYoung(oopRef);
        if (newOop.rawBits() != oopRef.rawBits()) {
            oopRef = newOop;
            // Newly tenured — its new address is newOop.asObjectPtr().
            scanQueue.push_back(newOop.asObjectPtr());
        } else if (newOop.isObject()) {
            // Already tenured (forwarded) — still scan if we haven't.
            // For simplicity, tenureIfYoung already returns new addr;
            // skip duplicate enqueues via forward map.
        }
    };

    // Roots: interpreter
    if (interpreter_) {
        interpreter_->forEachRoot(visitRoot);
        // jit-may22b Step 1: rekey Sista's cache for new→old tenure
        // forwarding too.  Without this, methods compiled while in
        // new space have cache keys that go stale post-scavenge
        // (their new-space oops are reclaimed when scavenge ends).
        // Opt-in via PHARO_SISTA_REKEY_AFTER_GC=1.
        // The Sista rekey machinery is JIT-only; compiled out when JIT is off.
#if PHARO_JIT_ENABLED
        if (GET_DEBUG_BOOL(PHARO_SISTA_REKEY_AFTER_GC)) {
            interpreter_->rekeySistaCacheViaForwarders(
                [&forward](uint64_t oldBits) -> uint64_t {
                    Oop o = Oop::fromRawBits(oldBits);
                    if (!o.isObject() || o.rawBits() <= 0x10000) return 0;
                    ObjectHeader* obj = o.asObjectPtr();
                    auto it = forward.find(obj);
                    if (it != forward.end()) {
                        return Oop::fromObject(it->second).rawBits();
                    }
                    // Not in the forward map.  If it's still a
                    // valid old-space pointer, keep it.  Otherwise
                    // (new-space oop with no forward = unreachable,
                    // or some weird state), keep it anyway — the
                    // compact-time rekey will sort it out if needed.
                    // Avoiding the drop here prevents losing entries
                    // that are still reachable via other paths.
                    return oldBits;
                });
        }
#endif  // PHARO_JIT_ENABLED — Sista cache rekey (scavenge)
    }

    // Roots: memory (class table, special objects, etc.)
    forEachMemoryRoot(visitRoot, /*includeClassTable*/ true);

    // Roots: full old-space scan for old→young pointers.  This is
    // O(oldSpace) per scavenge — slower than a maintained remembered
    // set, but immune to missed write barriers in primitives /
    // bytecodes that use direct slotAtPut.  Trade correctness for
    // perf until every write site is audited.
    //
    // Snapshot oldSpaceFree_ so we don't re-scan freshly-tenured
    // objects appended during this loop (Phase 2 drains those).
    // PHARO_SCAV_BIT_AUDIT: count whether RememberedBit accurately tracks
    // old→young refs.  Low overhead — just bookkeeping during the existing
    // full scan.  Used to validate write-barrier coverage before a
    // future session flips scavenge to skip un-remembered objects.
    const bool bitAudit = GET_DEBUG_BOOL(PHARO_SCAV_BIT_AUDIT);
    size_t auditBitSetHit = 0;     // bit set + ≥1 young ref (true positive)
    size_t auditBitSetMiss = 0;    // bit set + no young ref (false positive, harmless)
    size_t auditBitUnsetHit = 0;   // bit unset + ≥1 young ref (audit-gap MISS!)
    size_t auditBitUnsetSafe = 0;  // bit unset + no young ref (true negative)

    // For audit-gap miss logging (first N per scavenge).
    static thread_local size_t auditMissLogCount = 0;
    constexpr size_t kAuditMissLogMax = 10;

    auto scanRegionForYoung = [&](uint8_t* regionStart, uint8_t* regionEnd) {
        ObjectScanner scan(regionStart, regionEnd);
        while (ObjectHeader* obj = scan.next()) {
            size_t np = pointerSlotsOf(obj);
            Oop* slots = obj->slots();
            size_t cnt = obj->slotCount();
            bool foundYoungRef = false;
            int firstYoungSlot = -1;
            for (size_t i = 0; i < np && i < cnt; ++i) {
                Oop s = slots[i];
                if (!s.isObject()) continue;
                uint8_t* hp = reinterpret_cast<uint8_t*>(s.asObjectPtr());
                if (hp >= edenAllocBase_ && hp < edenFree_) {
                    if (bitAudit && !foundYoungRef) firstYoungSlot = (int)i;
                    if (bitAudit) foundYoungRef = true;
                    Oop newS = tenureIfYoung(s);
                    slots[i] = newS;
                    if (newS.rawBits() != s.rawBits()) {
                        scanQueue.push_back(newS.asObjectPtr());
                    }
                }
            }
            if (bitAudit) {
                bool wasRemembered = obj->isRemembered();
                if (wasRemembered && foundYoungRef) auditBitSetHit++;
                else if (wasRemembered && !foundYoungRef) auditBitSetMiss++;
                else if (!wasRemembered && foundYoungRef) {
                    auditBitUnsetHit++;
                    if (auditMissLogCount < kAuditMissLogMax) {
                        auditMissLogCount++;
                        uint32_t cls = obj->classIndex();
                        Oop clsOop = classAtIndex(cls);
                        std::string cn = clsOop.isObject()
                            ? nameOfClass(clsOop) : std::string("?");
                        std::string vcn = "?";
                        if (firstYoungSlot >= 0 && (size_t)firstYoungSlot < cnt) {
                            Oop v = slots[firstYoungSlot];
                            if (v.isObject() && v.rawBits() > 0x10000) {
                                uint32_t vc = v.asObjectPtr()->classIndex();
                                Oop vcl = classAtIndex(vc);
                                if (vcl.isObject()) vcn = nameOfClass(vcl);
                            }
                        }
                        // Tag the space: perm objects never barrier so
                        // they're a categorical exemption.
                        uint8_t* hp = reinterpret_cast<uint8_t*>(obj);
                        const char* space =
                            (hp >= permSpaceStart_ && hp < permSpaceEnd_)
                                ? "perm"
                                : (hp >= oldSpaceStart_ && hp < oldSpaceEnd_)
                                    ? "old" : "?";
                        fprintf(stderr,
                            "[SCAV-MISS #%zu] obj=%p[%s] cls=%s(idx=%u) "
                            "slotCount=%zu firstYoungSlot=%d val.cls=%s\n",
                            auditMissLogCount, (void*)obj, space, cn.c_str(),
                            cls, cnt, firstYoungSlot, vcn.c_str());
                    }
                }
                else auditBitUnsetSafe++;
            }
        }
    };
    // Snapshot oldSpaceFree_ so we don't re-scan freshly-tenured
    // objects appended during this loop (Phase 2 drains those).
    scanRegionForYoung(oldSpaceStart_, oldSpaceFree_);
    // Permanent space holds the special-objects array, and setSpecialObject
    // does direct slotAtPut bypassing the write barrier.
    if (permSpaceStart_ && permSpaceEnd_ > permSpaceStart_) {
        scanRegionForYoung(permSpaceStart_, permSpaceEnd_);
    }
    if (bitAudit) {
        fprintf(stderr,
            "[SCAV-AUDIT] bit accuracy: setHit=%zu setMiss=%zu "
            "unsetHit=%zu (gap!) unsetSafe=%zu\n",
            auditBitSetHit, auditBitSetMiss,
            auditBitUnsetHit, auditBitUnsetSafe);
    }
    rememberedSet_.clear();

    // Roots: mourn queue (ephemerons pending finalization hold
    // their fields alive).
    for (Oop& m : mournQueue_) {
        visitRoot(m);
    }

    // Phase 2: drain scanQueue.  For each newly-tenured object,
    // scan its pointer slots and tenure any young targets.
    while (!scanQueue.empty()) {
        ObjectHeader* obj = scanQueue.back();
        scanQueue.pop_back();
        size_t np = pointerSlotsOf(obj);
        Oop* slots = obj->slots();
        for (size_t i = 0; i < np && i < obj->slotCount(); ++i) {
            Oop s = slots[i];
            if (!s.isObject()) continue;
            uint8_t* hp = reinterpret_cast<uint8_t*>(s.asObjectPtr());
            if (hp >= edenAllocBase_ && hp < edenFree_) {
                Oop newS = tenureIfYoung(s);
                if (newS.rawBits() != s.rawBits()) {
                    slots[i] = newS;
                    scanQueue.push_back(newS.asObjectPtr());
                }
            }
        }
    }

    // Update nil/true/false bits if they moved (they shouldn't —
    // permanent specials are in old/perm space — but defensive).
    if (nilObject_.isObject()) {
        auto it = forward.find(nilObject_.asObjectPtr());
        if (it != forward.end()) {
            nilObject_ = Oop::fromObject(it->second);
            Oop::setNilBits(nilObject_.rawBits());
        }
    }

    // DIAGNOSTIC (PHARO_SCAV_DANGLE_CHECK): before eden is reset, scan all
    // live memory for any Oop still pointing into eden.  Each such pointer is
    // a MISSED ROOT — either it holds a now-stale pointer to a tenured object
    // (in `forward`) or a use-after-free to an object about to vanish.  This
    // pinpoints the scavenge root-set gap behind the cumulative-state
    // corruption (see docs/vm-compat-bugs.md).
    if (GET_DEBUG_BOOL(PHARO_SCAV_DANGLE_CHECK)) {
        static int dangScav = 0;
        int sid = ++dangScav;
        size_t dangCount = 0;
        auto reportSlot = [&](const char* where, ObjectHeader* holder, Oop v) {
            uint8_t* hp = reinterpret_cast<uint8_t*>(v.asObjectPtr());
            if (hp < edenAllocBase_ || hp >= edenFree_) return;
            dangCount++;
            if (dangCount > 15) return;
            bool tenured = forward.find(v.asObjectPtr()) != forward.end();
            std::string hcls = "?";
            if (holder) {
                Oop hc = classAtIndex(holder->classIndex());
                if (hc.isObject()) hcls = nameOfClass(hc);
            }
            std::string vcls = "?";
            if (v.rawBits() > 0x10000) {
                Oop vc = classAtIndex(v.asObjectPtr()->classIndex());
                if (vc.isObject()) vcls = nameOfClass(vc);
            }
            fprintf(stderr,
                "[SCAV-DANGLE-%d] %s holder=%s val.cls=%s %s\n",
                sid, where, hcls.c_str(), vcls.c_str(),
                tenured ? "(TENURED-but-not-updated)" : "(USE-AFTER-FREE)");
        };
        // Scan old + perm space objects.
        auto scanLive = [&](uint8_t* a, uint8_t* b, const char* tag) {
            ObjectScanner sc(a, b);
            while (ObjectHeader* obj = sc.next()) {
                size_t np = pointerSlotsOf(obj);
                Oop* sl = obj->slots();
                size_t cnt = obj->slotCount();
                for (size_t i = 0; i < np && i < cnt; ++i)
                    if (sl[i].isObject()) reportSlot(tag, obj, sl[i]);
            }
        };
        scanLive(oldSpaceStart_, oldSpaceFree_, "old");
        if (permSpaceStart_ && permSpaceEnd_ > permSpaceStart_)
            scanLive(permSpaceStart_, permSpaceEnd_, "perm");
        // Format-9 (Indexable64) roots whose slots pointerSlotsOf() treats as
        // NON-pointers and thus the scans above (and scavenge itself) skip:
        // hiddenRoots, freeLists, and the class-table pages.  Scan ALL their
        // slots as raw Oops for eden pointers — this is the prime suspect for
        // the scavenge missed-root.
        auto scanAllSlots = [&](Oop holderOop, const char* tag) {
            if (!holderOop.isObject() || holderOop.rawBits() <= 0x10000) return;
            ObjectHeader* h = holderOop.asObjectPtr();
            size_t cnt = h->slotCount();
            Oop* sl = h->slots();
            for (size_t i = 0; i < cnt; ++i) reportSlot(tag, h, sl[i]);
        };
        scanAllSlots(hiddenRootsObj_, "hiddenRoots(fmt9)");
        scanAllSlots(freeListsObj_, "freeLists(fmt9)");
        for (size_t i = 0; i < classTablePages_.size(); ++i)
            scanAllSlots(classTablePages_[i], "classTablePage(fmt9)");
        // Scan VM register/stack/frame roots (the forEachRoot set).
        if (interpreter_) {
            interpreter_->forEachRoot([&](Oop& o) {
                if (o.isObject()) reportSlot("vm-root", nullptr, o);
            });
        }
        if (dangCount > 0) {
            fprintf(stderr,
                "[SCAV-DANGLE-%d] TOTAL %zu dangling young pointer(s) after scavenge\n",
                sid, dangCount);
        }
    }

    // DIAGNOSTIC (PHARO_SCAV_RAWSCAN): brute-force superset of the dangle
    // check above — scan EVERY aligned word of old+perm space for a value
    // that lands in the eden range, with no object-format assumptions.  The
    // object-aware dangle check shares pointerSlotsOf/ObjectScanner with the
    // scavenge itself, so a systematic blind spot (format misclassification,
    // scanner misparse) is invisible to it; this scan is not.
    if (GET_DEBUG_BOOL(PHARO_SCAV_RAWSCAN)) {
        static int rawScav = 0;
        int sid = ++rawScav;
        std::vector<uint8_t*> hitAddrs;
        size_t hits = 0;
        auto rawScan = [&](uint8_t* a, uint8_t* b) {
            for (uint8_t* p = a; p + 8 <= b; p += 8) {
                uint64_t w = *reinterpret_cast<uint64_t*>(p);
                if ((w & 7) != 0) continue;  // heap oops are 8-aligned, tag 0
                uint8_t* t = reinterpret_cast<uint8_t*>(w);
                if (t < edenAllocBase_ || t >= edenFree_) continue;
                hits++;
                if (hitAddrs.size() < 25) hitAddrs.push_back(p);
            }
        };
        rawScan(oldSpaceStart_, oldSpaceFree_);
        if (permSpaceStart_ && permSpaceEnd_ > permSpaceStart_)
            rawScan(permSpaceStart_, permSpaceEnd_);
        if (hits) {
            // Attribute each hit to the nearest object at-or-before it (a hit
            // beyond that object's extent = dead space between objects, i.e.
            // likely a harmless stale word; a hit INSIDE an object beyond its
            // pointerSlotsOf() = the scavenge blind spot).
            for (uint8_t* p : hitAddrs) {
                uint64_t w = *reinterpret_cast<uint64_t*>(p);
                bool tenured =
                    forward.find(reinterpret_cast<ObjectHeader*>(w)) != forward.end();
                uint8_t* base = (p >= permSpaceStart_ && p < permSpaceEnd_)
                    ? permSpaceStart_ : oldSpaceStart_;
                uint8_t* limit = (base == permSpaceStart_) ? permSpaceEnd_ : oldSpaceFree_;
                ObjectHeader* prevObj = nullptr;
                ObjectScanner attr(base, limit);
                while (ObjectHeader* obj = attr.next()) {
                    if (reinterpret_cast<uint8_t*>(obj) > p) break;
                    prevObj = obj;
                }
                std::string hcls = "?";
                size_t hslots = 0, hptrs = 0;
                int hfmt = -1;
                bool inside = false;
                ptrdiff_t off = -1;
                if (prevObj) {
                    uint8_t* os = reinterpret_cast<uint8_t*>(prevObj);
                    uint8_t* oe = os - (prevObj->hasOverflowSlots() ? 8 : 0)
                        + prevObj->totalSize();
                    inside = p < oe;
                    off = p - os;
                    Oop hc = classAtIndex(prevObj->classIndex());
                    if (hc.isObject()) hcls = nameOfClass(hc);
                    hslots = prevObj->slotCount();
                    hptrs = pointerSlotsOf(prevObj);
                    hfmt = static_cast<int>(prevObj->format());
                }
                // The eden target is still intact (pre-reset) — identify it.
                std::string vcls = "?";
                size_t vslots = 0;
                ObjectHeader* vh = reinterpret_cast<ObjectHeader*>(w);
                Oop vc = classAtIndex(vh->classIndex());
                if (vc.isObject()) vcls = nameOfClass(vc);
                vslots = vh->slotCount();
                fprintf(stderr,
                    "[SCAV-RAWSCAN-%d] addr=%p val=%p %s nearest=%s(clsIdx=%u fmt=%d slots=%zu ptrSlots=%zu) off=%td %s target=%s(slots=%zu)\n",
                    sid, (void*)p, (void*)w,
                    tenured ? "TENURED-NOT-UPDATED" : "NOT-IN-FORWARD",
                    hcls.c_str(), prevObj ? prevObj->classIndex() : 0,
                    hfmt, hslots, hptrs, off,
                    inside ? "INSIDE" : "IN-GAP",
                    vcls.c_str(), vslots);
            }
            fprintf(stderr, "[SCAV-RAWSCAN-%d] TOTAL %zu eden-range word(s) after scavenge\n",
                    sid, hits);
            // Scanner-coverage report: does ObjectScanner actually reach
            // oldSpaceFree_?  A premature stop truncates the old->young root
            // scan the same way, which IS the missed-root mechanism.
            {
                ObjectScanner cov(oldSpaceStart_, oldSpaceFree_);
                size_t objCount = 0;
                uint8_t* lastEnd = oldSpaceStart_;
                while (ObjectHeader* obj = cov.next()) {
                    objCount++;
                    uint8_t* os = reinterpret_cast<uint8_t*>(obj);
                    lastEnd = os + obj->totalSize() - (obj->hasOverflowSlots() ? 8 : 0);
                }
                fprintf(stderr,
                    "[SCAV-RAWSCAN-%d] scanner coverage: %zu objs, stopped at %p, oldSpaceFree=%p, uncovered=%zd bytes\n",
                    sid, objCount, (void*)lastEnd, (void*)oldSpaceFree_,
                    (ptrdiff_t)(oldSpaceFree_ - lastEnd));
                if (oldSpaceFree_ - lastEnd >= 8) {
                    fprintf(stderr, "[SCAV-RAWSCAN-%d] words at stop:", sid);
                    for (int k = 0; k < 8 && lastEnd + (k + 1) * 8 <= oldSpaceFree_; ++k)
                        fprintf(stderr, " %016llx",
                            (unsigned long long)*reinterpret_cast<uint64_t*>(lastEnd + k * 8));
                    fprintf(stderr, "\n");
                }
            }
        }
    }

    // DIAGNOSTIC (PHARO_SCAV_RAWSCAN): tenure range-overlap check.  If two
    // forward-map SOURCES overlap in eden, an allocation path handed out
    // overlapping young memory (mis-sized allocation) — two "objects" share
    // bytes and tenure splits them into diverging copies: the identity-
    // substitution mechanism behind the convertStorePop failure.  Dest
    // overlap would mean the tenure bump-allocator itself is broken.
    if (GET_DEBUG_BOOL(PHARO_SCAV_RAWSCAN) && !forward.empty()) {
        std::vector<std::pair<uint8_t*, uint8_t*>> ranges;  // [start, end)
        ranges.reserve(forward.size());
        auto rangeOf = [](ObjectHeader* h) -> std::pair<uint8_t*, uint8_t*> {
            uint8_t* s = reinterpret_cast<uint8_t*>(h) -
                (h->hasOverflowSlots() ? 8 : 0);
            return {s, s + h->totalSize()};
        };
        for (auto& kv : forward) ranges.push_back(rangeOf(kv.first));
        std::sort(ranges.begin(), ranges.end());
        static int ovN = 0;
        for (size_t i = 1; i < ranges.size() && ovN < 10; i++) {
            if (ranges[i - 1].second > ranges[i].first) {
                ovN++;
                fprintf(stderr,
                    "[EDEN-OVERLAP] scav=%d src ranges [%p..%p) and [%p..%p) overlap!\n",
                    myScavId, (void*)ranges[i - 1].first, (void*)ranges[i - 1].second,
                    (void*)ranges[i].first, (void*)ranges[i].second);
            }
        }
        ranges.clear();
        for (auto& kv : forward) ranges.push_back(rangeOf(kv.second));
        std::sort(ranges.begin(), ranges.end());
        for (size_t i = 1; i < ranges.size() && ovN < 10; i++) {
            if (ranges[i - 1].second > ranges[i].first) {
                ovN++;
                fprintf(stderr,
                    "[TENURE-DEST-OVERLAP] scav=%d dest ranges [%p..%p) and [%p..%p) overlap!\n",
                    myScavId, (void*)ranges[i - 1].first, (void*)ranges[i - 1].second,
                    (void*)ranges[i].first, (void*)ranges[i].second);
            }
        }
    }

    // PHARO_SCAV_DUMP_FORWARD: persist tenure maps for post-mortem
    // identity-mismatch forensics.  N >= 0: that scavenge only; -2: every
    // scavenge to fwdmap-<n>.txt.
    if (myScavId == GET_DEBUG_INT(PHARO_SCAV_DUMP_FORWARD) ||
        GET_DEBUG_INT(PHARO_SCAV_DUMP_FORWARD) == -2) {
        char fn[64];
        snprintf(fn, sizeof fn, "C:/tmp/fwdmap-%d.txt", myScavId);
        FILE* ff = fopen(fn, "w");
        if (ff) {
            for (auto& kv : forward)
                fprintf(ff, "%llx %llx\n",
                        (unsigned long long)reinterpret_cast<uintptr_t>(kv.first),
                        (unsigned long long)reinterpret_cast<uintptr_t>(kv.second));
            fclose(ff);
            fprintf(stderr, "[FWD-DUMP] scavenge #%d: %zu entries -> %s\n",
                    myScavId, forward.size(), fn);
        }
    }

    // Phase 3: reset eden.  All unreferenced young objects vanish.
    if (myScavId == GET_DEBUG_INT(PHARO_SCAV_QUARANTINE_AT)) {
        // Quarantine: keep eden contents but make every later access fault.
        // edenFree_ pinned at the limit = eden permanently "full", so
        // allocateRawYoung always takes the old-space fallback and the
        // protected pages are never legitimately touched again.  Any fault
        // in this range afterwards IS the corruption path, caught live.
        scavQuarantineActive = true;
        edenFree_ = edenAllocLimit_;
#ifdef _WIN32
        uintptr_t pgLo = (reinterpret_cast<uintptr_t>(edenStart_) + 4095) & ~4095ULL;
        uintptr_t pgHi = reinterpret_cast<uintptr_t>(survivorStart_) & ~4095ULL;
        unsigned long oldProt = 0;
        int ok = VirtualProtect(reinterpret_cast<void*>(pgLo), pgHi - pgLo,
                                PHARO_PAGE_NOACCESS, &oldProt);
        fprintf(stderr,
            "[SCAV-QUARANTINE] scavenge #%d: eden [%p..%p) protected (ok=%d) — "
            "any further access to this range is a stale young reference\n",
            myScavId, (void*)pgLo, (void*)pgHi, ok);
#else
        fprintf(stderr, "[SCAV-QUARANTINE] not implemented on this platform\n");
#endif
    } else if (GET_DEBUG_BOOL(PHARO_EDEN_ROTATE)) {
#ifdef _WIN32
        // Retire the just-scavenged half (page-protect it) and activate
        // the other half.  A stale pointer into the retired generation now
        // faults at first dereference instead of silently aliasing a
        // reused-eden object.
        unsigned long rotProt = 0;
        uintptr_t rLo = (reinterpret_cast<uintptr_t>(edenAllocBase_) + 4095) & ~4095ULL;
        uintptr_t rHi = reinterpret_cast<uintptr_t>(edenAllocLimit_) & ~4095ULL;
        if (rHi > rLo)
            VirtualProtect(reinterpret_cast<void*>(rLo), rHi - rLo,
                           PHARO_PAGE_NOACCESS, &rotProt);
        size_t rotHalf = static_cast<size_t>(edenAllocLimit_ - edenAllocBase_);
        uint8_t* otherBase = (edenAllocBase_ == edenStart_)
            ? edenAllocLimit_ : edenStart_;
        edenAllocBase_ = otherBase;
        edenAllocLimit_ = otherBase + rotHalf;
        uintptr_t aLo = (reinterpret_cast<uintptr_t>(edenAllocBase_) + 4095) & ~4095ULL;
        uintptr_t aHi = reinterpret_cast<uintptr_t>(edenAllocLimit_) & ~4095ULL;
        if (aHi > aLo)
            VirtualProtect(reinterpret_cast<void*>(aLo), aHi - aLo,
                           PHARO_PAGE_READWRITE, &rotProt);
        edenFree_ = edenAllocBase_;
        fprintf(stderr, "[EDEN-ROTATE] scavenge #%d: active [%p..%p), retired half protected\n",
                myScavId, (void*)edenAllocBase_, (void*)edenAllocLimit_);
#else
        edenFree_ = edenAllocBase_;
#endif
    } else {
        // Cascade hunt (2026-07-05): poison the retired eden window so a
        // read-through-recycled-storage fabricates a RECOGNIZABLE value.
        if (GET_DEBUG_BOOL(PHARO_EDEN_POISON)) {
            uint64_t* w = reinterpret_cast<uint64_t*>(edenAllocBase_);
            uint64_t* wEnd = reinterpret_cast<uint64_t*>(edenFree_);
            while (w < wEnd) *w++ = 0x5CAFED0000000000ULL;
        }
        edenFree_ = edenAllocBase_;
    }

    size_t edenUsedAfter = 0;
    (void)edenUsedBefore;
    result.bytesReclaimed = edenUsedBefore - edenUsedAfter;
    // Objects relocated this cycle = the survivors tenured out of eden.
    // `forward` holds exactly one entry per copied object.  This field was
    // declared and printed by five diagnostics but never assigned, so every
    // GC report claimed "moved 0 objects".
    result.objectsMoved = forward.size();

    // Rebuild ips from the offsets stashed at scavenge entry (skips
    // the full-GC-only methodCache/IC flush tail).
    if (ipWrapped) interpreter_->afterGC(false);

    auto end = std::chrono::steady_clock::now();
    result.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();
    gcCount_++;
    totalGCTime_ += result.milliseconds;
    scavCount_++;
    scavTime_ += result.milliseconds;
    return result;
}

GCResult ObjectMemory::incrementalGC() {
    // Without a proper generational GC, incremental GC must do a full GC
    // to process weak references (which tests like WeakMessageSendTest need).
    return fullGC();
}

// Helper to iterate over all objects in old space
void ObjectMemory::forEachObjectInOldSpace(std::function<void(ObjectHeader*)> callback) {
    ObjectScanner scanner(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = scanner.next()) {
        callback(obj);
    }
}

void ObjectMemory::sweepGC() {
    // Non-compacting mark-sweep GC. Safe to call from within allocations
    // because no objects are moved — only dead objects become free chunks.
    auto start = std::chrono::steady_clock::now();

    // 1. Clear all marks
    ObjectScanner clearScanner(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = clearScanner.next()) {
        obj->setMarked(false);
    }

    // 2. Mark phase (same as fullGC)
    markPhase();

    // 3. Sweep: convert dead objects to free chunks, coalesce adjacent ones,
    //    and shrink oldSpaceFree_ if tail is dead.
    clearFreeLists();

    uint8_t* lastLiveEnd = oldSpaceStart_;

    // We need to coalesce adjacent dead objects into single free chunks.
    // Track start of current dead run.
    uint8_t* deadRunStart = nullptr;

    ObjectScanner sweepScanner(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = sweepScanner.next()) {
        uint8_t* objAddr = reinterpret_cast<uint8_t*>(obj);
        // totalSize() ALREADY includes the 8-byte overflow word (see
        // ObjectHeader::totalSize and the note in objectAfter) — it is the
        // size of the whole allocation, measured from objStart.  Only the
        // START needs backing up; adding 8 to the size double-counted the
        // overflow word and pushed lastLiveEnd 8 bytes past the true end of
        // every large (>254-slot) live object.  When such an object was the
        // last live one, the trailing-dead-run branch below set
        // oldSpaceFree_ = lastLiveEnd, leaving an 8-byte remnant of the dead
        // object above it; the next ObjectScanner walk then tried to parse a
        // header out of those stale bytes.
        size_t objSize = obj->totalSize();
        uint8_t* objStart = obj->hasOverflowSlots() ? (objAddr - 8) : objAddr;

        if (obj->isMarked()) {
            // Live object — clear mark
            obj->setMarked(false);
            lastLiveEnd = objStart + objSize;

            // End any dead run
            if (deadRunStart) {
                size_t runSize = objStart - deadRunStart;
                if (runSize >= 16) {
                    ObjectHeader* freeChunk = makeFreeChunk(deadRunStart, runSize);
                    if (freeChunk) {
                        addToFreeList(freeChunk, runSize);
                    }
                }
                deadRunStart = nullptr;
            }
        } else {
            // Dead object — start or extend dead run
            if (!deadRunStart) {
                deadRunStart = objStart;
            }
        }
    }

    // Handle trailing dead run — shrink oldSpaceFree_
    if (deadRunStart && deadRunStart >= lastLiveEnd) {
        oldSpaceFree_ = lastLiveEnd;
    } else if (deadRunStart) {
        // Dead run at end but mixed with live
        size_t runSize = oldSpaceFree_ - deadRunStart;
        if (runSize >= 16) {
            ObjectHeader* freeChunk = makeFreeChunk(deadRunStart, runSize);
            if (freeChunk) {
                addToFreeList(freeChunk, runSize);
            }
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    size_t freeAfter = oldSpaceEnd_ - oldSpaceFree_;

    // If less than 25% free after sweep, request compacting GC at next safe point
    size_t totalSpace = oldSpaceEnd_ - oldSpaceStart_;
    if (freeAfter < totalSpace / 4) {
        needsCompactGC_ = true;
    }

    gcCount_++;
    totalGCTime_ += ms;
    sweepCount_++;
    sweepTime_ += ms;
}

bool ObjectMemory::checkHeapIntegrity(const char* when) {
    size_t scanned = 0, badSlots = 0, badObjs = 0;
    const size_t maxClass = classTable_.size();
    Oop o = firstObject();
    while (o.isObject() && o.rawBits() != 0) {
        ObjectHeader* h = o.asObjectPtr();
        // Validate the holder's own class first.
        uint32_t hci = h->classIndex();
        bool holderBad = false;
        size_t nptr = 0;
        if (hci == 0 || hci >= maxClass) {
            holderBad = true;  // free chunk / garbage header — skip slot scan
        } else {
            nptr = pointerSlotsOf(h);
            for (size_t i = 0; i < nptr; ++i) {
                Oop v = h->slotAt(i);
                if (v.isNil()) continue;              // nil is always a legal slot value
                uint64_t b = v.rawBits();
                uint64_t tag = b & 0x7;
                bool ok = true;
                const char* why = "";
                // A well-formed oop is: nil (handled above), a tag-0 heap pointer,
                // or an immediate with tag 1 (SmallInt) / 3 (Character) / 5 (SmallFloat).
                // Tags 2/4/6/7 are MALFORMED — no valid oop has them.  This is the
                // fingerprint of the cold-startup wild write (raw C++ ints 0x4/0x6/0xc
                // = tag 4/6/4 leaking into oop slots).  The old check skipped these as
                // "immediates" (if (!v.isObject()) continue), which is why it never
                // caught the corruption.
                if (tag == 1 || tag == 3 || tag == 5) {
                    // legit immediate (SmallInteger / Character / SmallFloat)
                } else if (tag == 0) {
                    ok = isValidPointer(v);
                    why = "out-of-heap(dangling)";
                    if (ok) {
                        uint32_t tci = v.asObjectPtr()->classIndex();
                        if (tci == 0 || tci >= maxClass) { ok = false; why = "invalid-class(free/garbage)"; }
                    }
                } else {
                    ok = false;
                    why = "malformed-tag(raw-int?)";   // tag 2/4/6/7 — the wild-write fingerprint
                }
                if (!ok) {
                    if (badSlots < 12) {
                        fprintf(stderr, "[HEAPCHECK %s] CORRUPT holder=0x%llx cls=%u fmt=%u slot[%zu]=0x%llx -> %s\n",
                                when, (unsigned long long)o.rawBits(), hci, (unsigned)h->format(),
                                i, (unsigned long long)v.rawBits(), why);
                    }
                    ++badSlots;
                }
            }
        }
        if (holderBad) ++badObjs;
        ++scanned;
        Oop next = objectAfter(o);
        if (!next.isObject() || next.rawBits() == o.rawBits()) break;
        o = next;
        if (scanned > 5000000) { fprintf(stderr, "[HEAPCHECK %s] scan cap hit\n", when); break; }
    }
    fprintf(stderr, "[HEAPCHECK %s] scanned=%zu corruptSlots=%zu badHeaderObjs=%zu => %s\n",
            when, scanned, badSlots, badObjs, (badSlots == 0) ? "CLEAN" : "CORRUPT");
    return badSlots == 0;
}

GCResult ObjectMemory::fullGC(bool skipEphemerons) {
    auto start = std::chrono::steady_clock::now();
    GCResult result{0, 0, 0};


    size_t usedBefore = oldSpaceFree_ - oldSpaceStart_;

    // Diagnostic: PHARO_GC_LOG=1 logs every fullGC call.  On Pi 5 perf
    // showed 51% of bench CPU in fullGC + page-fault overhead from the
    // mark-bit clear's __memset_zva64.  Need to identify what's
    // triggering GC during the bench loop.
    if (GET_DEBUG_BOOL(PHARO_GC_LOG)) {
        static int count = 0;
        ++count;
        if (count <= 50 || (count & 0x1F) == 0) {
            fprintf(stderr, "[GC-LOG] fullGC #%d (used=%zu MB, threshold=%zu MB, skipEph=%d)\n",
                    count,
                    (oldSpaceFree_ - oldSpaceStart_) / (1024 * 1024),
                    (lastCompactedSize_ + std::max(gcHeadroom_, lastCompactedSize_))
                        / (1024 * 1024),
                    (int)skipEphemerons);
        }
    }

    // jit-may23b R71: phase timing.
    const bool timeGCPhases = GET_DEBUG_BOOL(PHARO_TIME_GC_PHASES);
    auto readTSC = []() -> uint64_t {
#if defined(__aarch64__)
        uint64_t t;
        asm volatile("mrs %0, cntvct_el0" : "=r"(t));   // ARM virtual cycle counter
        return t;
#elif defined(__x86_64__) || defined(__i386__)
        return __builtin_ia32_rdtsc();                  // x86 time-stamp counter
#else
        return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
#endif
    };
    uint64_t tPrepGCStart = timeGCPhases ? readTSC() : 0;

    // 1. Convert interpreter IPs to offsets (methods may move)
    if (interpreter_) {
        interpreter_->prepareForGC();
    }

    // 1.5. Pre-compact scavenge when young-gen is active — BEFORE the mark
    // phase.  Compact repurposes new-space as scratch for forwarding-field
    // storage, so eden must be empty going into plan/compact.  This used to
    // run AFTER markPhase (between mark and plan): the scavenge treats weak
    // slots as strong and tenures every root-reachable young object, so a
    // young object that was scavenge-reachable but NOT mark-reachable (e.g.
    // a fresh Symbol held only by the SymbolTable WeakSet) arrived in old
    // space UNMARKED — planCompact then reclaimed it while the live weak
    // slots (retargeted by the scavenge) still pointed at it.  Result:
    // dangling refs into compacted-over memory, surfacing as heap-phase-
    // dependent corruption in symbol/literal machinery (the OpalCompiler
    // large-method fileIn failure, root-caused 2026-07-02).  Scavenging
    // FIRST lets markPhase see the tenured copies and apply true weak
    // semantics to them.
    //
    // NOTE for anyone bisecting a young-space corruption: this call is NOT
    // gated by PHARO_YG_NO_SCAVENGE, PHARO_YG_SKIP_SCAV_FROM or
    // PHARO_GC_ROUNDTRIP_ONLY -- those all gate only the interpreter-side
    // scavenge in Interpreter.cpp. Every fullGC therefore still drives one
    // scavenge, so those knobs floor the scavenge count at 1 rather than 0 and
    // a bisect that assumes 0 draws the wrong conclusion (measured 2026-08-18:
    // both knobs took a repro from scavenges=2 to scavenges=1, which reads as
    // "still reproduces with scavenging off" and is not what happened).
    // PHARO_YG_NO_SCAVENGE now gates this site too, so "disable scavenging"
    // means it. Skipping this scavenge is NOT safe as a production setting --
    // it reinstates the 2026-07-02 defect described above, where a young object
    // that is scavenge-reachable but not mark-reachable lands in old space
    // unmarked and planCompact reclaims it under live weak slots. It is a
    // BISECT AXIS ONLY, and it is what pinned the 2026-08-18 PMVector>>cos
    // corpse to this call rather than the interpreter-side one.
    if (enableYoungGen_ && edenFree_ > edenAllocBase_
            && !GET_DEBUG_BOOL(PHARO_YG_NO_SCAVENGE)) {
        scavenge();
    }
#ifdef _WIN32
    // EDEN_ROTATE detector: compact legitimately scribbles scratch over all
    // of new space — unprotect for the GC's duration (re-protected at the
    // end) so the detector doesn't false-fault here.
    if (GET_DEBUG_BOOL(PHARO_EDEN_ROTATE)) {
        unsigned long rotP = 0;
        uintptr_t uLo = (reinterpret_cast<uintptr_t>(edenStart_) + 4095) & ~4095ULL;
        uintptr_t uHi = reinterpret_cast<uintptr_t>(survivorStart_) & ~4095ULL;
        if (uHi > uLo)
            VirtualProtect(reinterpret_cast<void*>(uLo), uHi - uLo,
                           PHARO_PAGE_READWRITE, &rotP);
    }
#endif
    uint64_t tClearStart = timeGCPhases ? readTSC() : 0;

    // 2. Clear all marks AND grey bits
    // Grey bits must be cleared to prevent stale grey bits (from the image
    // or from a previous interrupted GC) from desyncing the savedFieldPtr
    // in updatePointersAfterCompact. A stale grey on a pinned object would
    // cause it to be treated as mobile, advancing the saved fields pointer
    // and corrupting every subsequent object's first field.
    {
        ObjectScanner scanner(oldSpaceStart_, oldSpaceFree_);
        while (ObjectHeader* obj = scanner.next()) {
            obj->setMarked(false);
            obj->setGrey(false);
        }
    }
    uint64_t tMarkStart = timeGCPhases ? readTSC() : 0;

    // 3. Mark phase
    ephemeronRescuedKeys_.clear();  // per-cycle set (fired-key weak nilling)
    size_t markedCount = markPhase(skipEphemerons);
    uint64_t tCompactStart = timeGCPhases ? readTSC() : 0;

    // Symbol class corruption check and stale pointer check disabled (verified clean)

    // (Pre-compact scavenge moved to step 1.5 — it must precede markPhase;
    // see the comment there.)

    // 4. Plan + update + copy (compact).
    //    The saved-first-fields scratch space is finite (one Oop per moving
    //    object, carved out of new space), so a heap with more movers than it
    //    holds is compacted in several passes.  Each pass relocates the prefix
    //    it managed to plan; the objects it moved keep their MARK bits so the
    //    next pass still walks them and updates their pointers to objects that
    //    have not moved yet.  A single pass covering the whole heap takes
    //    exactly the old path.
    //
    //    This replaces the roll-back-and-skip workaround: planning used to undo
    //    its partial plan and fullGC degraded to mark-only, which is safe but
    //    permanent — a live set that overflows the scratch once overflows it
    //    every cycle, so old space never de-fragmented again.
    {
        CompactPass pass;
        pass.srcStart = oldSpaceStart_;
        pass.dstStart = oldSpaceStart_;
        bool firstPass = true;
        bool marksClearedInline = false;

        for (;;) {
            planCompactSavingForwarders(pass);

            // updatePointersAfterCompact runs INSIDE the loop, once per pass,
            // while this pass's forwarders are installed.  Under
            // PHARO_JIT_ENABLED it also rekeys the Sista method->fn cache
            // through those forwarders (PHARO_SISTA_REKEY_AFTER_GC), so it
            // cannot be hoisted after the loop: by then every grey bit is clear
            // and follow() would be the identity, leaving every cache key
            // pointed at where its method used to be.
            //
            // Running it once per pass is correct because the per-pass
            // translations COMPOSE.  An object an earlier pass relocated is
            // marked but no longer grey (copyAndUnmark clears grey on every
            // object it touches), so resolveForward is the identity on it; and
            // an earlier pass's destinations lie strictly below the current
            // pass's source range (the destination finger never overtakes the
            // source finger), so a key already rewritten can never alias an
            // object still to be planned.
            updatePointersAfterCompact();

            // Only a single pass covering the whole heap may clear marks as it
            // copies — the common case, and the one that avoids the extra heap
            // walk below.
            marksClearedInline = firstPass && pass.complete;
            firstPass = false;
            result.compactPasses++;
            result.objectsMoved += copyAndUnmark(pass, marksClearedInline);

            if (pass.complete) {
                oldSpaceFree_ = pass.dstEnd;
                break;
            }

            // Forward-progress guard: a pass that plans nothing and consumes no
            // source would loop forever.  Only reachable with a zero-capacity
            // scratch space, but a hung GC is worse than a partly compacted
            // heap.  Leave oldSpaceFree_ alone — everything from pass.srcEnd on
            // is still live and still at its original address.
            if (pass.srcEnd == pass.srcStart) {
                fprintf(stderr,
                    "[GC-COMPACT-STALL] compaction pass %zu planned nothing "
                    "(scratch = %zu Oop slots); stopping with old space partly "
                    "compacted.\n",
                    result.compactPasses,
                    (size_t)(reinterpret_cast<Oop*>(newSpaceEnd_)
                             - reinterpret_cast<Oop*>(newSpaceStart_)));
                fflush(stderr);
                break;
            }

            pass.srcStart = pass.srcEnd;
            pass.dstStart = pass.dstEnd;
        }

        // Multi-pass runs deliberately left mark bits set so later passes would
        // still scan the already-relocated objects.  Clear them now.
        if (!marksClearedInline) {
            ObjectScanner markClearScanner(oldSpaceStart_, oldSpaceFree_);
            while (ObjectHeader* obj = markClearScanner.next()) {
                obj->setMarked(false);
            }
        }

        // Multi-pass is no longer an error, but every extra pass costs a full
        // updatePointersAfterCompact walk of old space + perm + eden + every
        // root, so it is worth seeing when it starts happening.  First five.
        if (result.compactPasses > 1) {
            static int multipassLogged = 0;
            if (multipassLogged++ < 5) {
                fprintf(stderr,
                    "[GC-MULTIPASS] compaction needed %zu passes (scratch = %zu "
                    "Oop slots, %zu objects moved) — raise new space to cut the "
                    "repeated pointer-update walks.\n",
                    result.compactPasses,
                    (size_t)(reinterpret_cast<Oop*>(newSpaceEnd_)
                             - reinterpret_cast<Oop*>(newSpaceStart_)),
                    result.objectsMoved);
                fflush(stderr);
            }
        }
    }
    if (timeGCPhases) {
        uint64_t tDone = readTSC();
        std::fprintf(stderr,
            "[GC-PHASES] prep=%llu clear=%llu mark=%llu compact=%llu (ns each)\n",
            (unsigned long long)(tClearStart - tPrepGCStart),
            (unsigned long long)(tMarkStart - tClearStart),
            (unsigned long long)(tCompactStart - tMarkStart),
            (unsigned long long)(tDone - tCompactStart));
    }

    // 5. Rebuild free list from the gap at the end of old space.  Once, after
    //    the last pass: it reads oldSpaceFree_ and madvise(MADV_DONTNEED)s
    //    everything above it, which would hand live pages back to the kernel if
    //    it ran between passes, while oldSpaceFree_ still held its
    //    pre-compaction value.
    rebuildFreeListAfterCompact();

    // Post-compaction stale pointer check (disabled — verified clean, too expensive for production)
    if (GET_DEBUG_BOOL(PHARO_HEAP_CHECK)) {
        checkHeapIntegrity("post-fullGC");
    }

    // 6. Update nil bits if nil moved
    if (nilObject_.isObject()) {
        Oop::setNilBits(nilObject_.rawBits());
    }

    // 7. Restore interpreter IPs from offsets
    if (interpreter_) {
        interpreter_->afterGC();
    }

    forceGCFlag_ = false;

#ifdef _WIN32
    // EDEN_ROTATE detector: re-protect the retired half (its content is now
    // compact-scratch trash, so a stale pointer still faults on touch).
    if (GET_DEBUG_BOOL(PHARO_EDEN_ROTATE)) {
        size_t rotHalf = static_cast<size_t>(edenAllocLimit_ - edenAllocBase_);
        uint8_t* retBase = (edenAllocBase_ == edenStart_)
            ? edenAllocLimit_ : edenStart_;
        unsigned long rotP = 0;
        uintptr_t rLo = (reinterpret_cast<uintptr_t>(retBase) + 4095) & ~4095ULL;
        uintptr_t rHi = reinterpret_cast<uintptr_t>(retBase + rotHalf) & ~4095ULL;
        if (rHi > rLo)
            VirtualProtect(reinterpret_cast<void*>(rLo), rHi - rLo,
                           PHARO_PAGE_NOACCESS, &rotP);
    }
#endif

    size_t usedAfter = oldSpaceFree_ - oldSpaceStart_;
    result.bytesReclaimed = (usedBefore > usedAfter) ? (usedBefore - usedAfter) : 0;

    auto end = std::chrono::steady_clock::now();
    result.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    // Record compacted size for threshold-based GC triggering
    lastCompactedSize_ = oldSpaceFree_ - oldSpaceStart_;

    gcCount_++;
    totalGCTime_ += result.milliseconds;
    fullGCCount_++;
    fullGCTime_ += result.milliseconds;

    // PHARO_HEAP_CENSUS: histogram the survivors when the live heap is
    // suspiciously large — the explosion diagnosis without waiting for
    // the fatal.
    if (GET_DEBUG_BOOL(PHARO_HEAP_CENSUS) && usedAfter > (1ULL << 30)) {
        dumpHeapCensus(25);
        // The census says WHAT filled the heap; the process dump says WHO —
        // the storm process's current frames name the loop.
        if (interpreter_) interpreter_->dumpProcessQueues();
    }

    // PHARO_GC_LOG completion line: used-after tracks LIVE heap across the
    // run (the entry line shows pressure; this shows what survived) — the
    // leak-vs-borderline discriminator for old-space exhaustion deaths.
    if (GET_DEBUG_BOOL(PHARO_GC_LOG)) {
        static int count = 0;
        ++count;
        if (count <= 50 || (count & 0x1F) == 0) {
            // usedAfter is the BUMP POINTER, not the live set.  Walk what
            // is actually in old space so the two can be compared: a
            // package-loaded image measured 235 MB used against 91 MB of
            // real objects, and only the second number is "live".
            size_t objBytes = 0, freeBytes = 0, objCount = 0, freeCount = 0;
            uint8_t* walkEnd = oldSpaceStart_;
            {
                ObjectScanner walk(oldSpaceStart_, oldSpaceFree_);
                while (ObjectHeader* o = walk.next()) {
                    size_t sz = o->totalSize();
                    walkEnd = reinterpret_cast<uint8_t*>(o) + sz;
                    if (o->classIndex() == 0) { freeBytes += sz; freeCount++; }
                    else { objBytes += sz; objCount++; }
                }
            }
            fprintf(stderr, "[GC-LOG] fullGC #%d done: used=%zu MB (objects=%zu MB "
                    "in %zu, free-chunks=%zu MB in %zu) reclaimed=%zu MB marked=%zu %lldms\n",
                    count, usedAfter / (1024 * 1024),
                    objBytes / (1024 * 1024), objCount,
                    freeBytes / (1024 * 1024), freeCount,
                    result.bytesReclaimed / (1024 * 1024),
                    markedCount,
                    (long long)result.milliseconds);
            fprintf(stderr, "[GC-LOG]   walkEnd=+%zu MB  passes=%zu moved=%zu\n",
                    (size_t)(walkEnd - oldSpaceStart_) / (1024 * 1024),
                    result.compactPasses, result.objectsMoved);
            {
                // Where do the pinned objects sit?  A pinned object high in
                // old space is a floor under oldSpaceFree_ that sliding
                // compaction cannot cross.
                ObjectScanner pw(oldSpaceStart_, oldSpaceFree_);
                int shown = 0;
                uint8_t* lastObj = oldSpaceStart_;
                while (ObjectHeader* o = pw.next()) {
                    lastObj = reinterpret_cast<uint8_t*>(o) + o->totalSize();
                    if (o->isPinned() && shown < 12) {
                        fprintf(stderr, "[GC-LOG]   pinned @+%zu KB size=%zu cls=%u\n",
                                (size_t)((uint8_t*)o - oldSpaceStart_) / 1024,
                                o->totalSize(), o->classIndex());
                        shown++;
                    }
                }
                fprintf(stderr, "[GC-LOG]   lastObjectEnd=+%zu KB  oldSpaceFree=+%zu KB\n",
                        (size_t)(lastObj - oldSpaceStart_) / 1024,
                        (size_t)(oldSpaceFree_ - oldSpaceStart_) / 1024);
            }
        }
    }

    // PHARO_DELAY_DEBUG: log any GC over 10ms. Long GCs stall the main
    // interpreter loop, which is what blocks checkTimerSemaphore and
    // causes delay-fire latency spikes seen in timing tests.
    if (g_debug.delayDebug) {
        if (result.milliseconds >= 10) {
            fprintf(stderr, "[GC-LONG] took=%lldms marked=%zu bytesReclaimed=%zu usedBefore=%zu usedAfter=%zu\n",
                    (long long)result.milliseconds, markedCount,
                    result.bytesReclaimed, usedBefore, usedAfter);
        }
    }
    return result;
}

bool ObjectMemory::needsGC() const {
    return forceGCFlag_;
}

void ObjectMemory::addRoot(Oop* root) {
    roots_.push_back(root);
}

void ObjectMemory::removeRoot(Oop* root) {
    roots_.erase(std::remove(roots_.begin(), roots_.end(), root), roots_.end());
}

void ObjectMemory::allObjectsDo(std::function<void(Oop)> callback) {
    // Helper to scan a memory region
    auto scanRegion = [&](uint8_t* start, uint8_t* end) {
        uint8_t* scan = start;
        while (scan < end) {
            uint64_t* wordPtr = reinterpret_cast<uint64_t*>(scan);
            uint64_t word = *wordPtr;

            // Skip zero headers (free space / padding)
            if (word == 0) {
                scan += 8;
                while (scan < end) {
                    wordPtr = reinterpret_cast<uint64_t*>(scan);
                    if (*wordPtr != 0) break;
                    scan += 8;
                }
                if (scan >= end) break;
                word = *wordPtr;
            }

            // Check for overflow header.
            // In Spur, overflow objects have: [overflow_count_word][header_word][slots...]
            // Both the overflow count word and the header word have 0xFF in bits 56-63.
            // CRITICAL: Only check for overflow when the CURRENT word has 0xFF in its
            // top byte. Otherwise, wordPtr+1 is a slot value, not a header!
            uint64_t* headerPtr = wordPtr;
            uint8_t topByte = static_cast<uint8_t>((word >> 56) & 0xFF);
            if (topByte == 255 && scan + 8 < end) {
                uint64_t nextWord = *(wordPtr + 1);
                uint8_t nextNumSlots = static_cast<uint8_t>((nextWord >> 56) & 0xFF);
                if (nextNumSlots == 255) {
                    // Current word is overflow count, next word is the actual header
                    // The overflow count is in the low 56 bits of the first word
                    uint64_t overflowCount = (word << 8) >> 8;  // mask off top byte
                    size_t remaining = end - scan;
                    size_t neededSize = 8 + 8 + overflowCount * 8;  // overflow word + header + slots

                    if (overflowCount >= 255 && neededSize <= remaining) {
                        headerPtr = wordPtr + 1;
                    } else {
                        // Invalid overflow - skip past both words
                        scan += 16;
                        continue;
                    }
                }
            }

            ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(headerPtr);
            size_t size = obj->totalSize();

            // Bounds check: size must fit within remaining heap
            size_t remaining = end - scan;
            if (size == 0 || size > remaining) {
                // Invalid size - skip 8 bytes and resync
                scan += 8;
                continue;
            }

            // Free chunks are NOT Smalltalk objects: they carry classIndex 0
            // (see makeFreeChunk) and have no class, so handing one to the
            // image is fatal.  Measured 2026-08-23 with
            // PHARO_OLDSPACE_FREELIST=1: class-shape migration
            // (Metaclass>>addSlot: -> copyObject:to:) enumerated the heap,
            // reached free chunk 0x700008ba20, sent it #isReadOnlyObject and
            // took a doesNotUnderstand with classIdx=0 -- killing the process
            // and wedging the run.  This never bit before because without the
            // knob the post-compaction gaps stay zero-filled and are skipped
            // by the zero-word path above; the knob writes real headers there.
            if (obj->classIndex() != 0) {
                callback(oopFromPointer(obj));
            }

            // Advance past the object
            scan += size;
        }
    };

    // Scan permanent space
    scanRegion(permSpaceStart_, permSpaceEnd_);

    // Scan old space
    scanRegion(oldSpaceStart_, oldSpaceFree_);

    // Scan eden
    scanRegion(edenAllocBase_, edenFree_);
}

void ObjectMemory::collectInstancesOfClass(uint32_t classIndex,
                                            std::vector<Oop>& out) {
    // classIndex 0 is what makeFreeChunk stamps into every FREE CHUNK, and it
    // is also what indexOfClass answers as its "not found" sentinel
    // (ObjectMemory.cpp, `return 0;  // Not found`).  Those two meanings
    // collide: asking for the instances of a class that has no class-table
    // entry would otherwise collect every free chunk in the heap and hand them
    // to the image as objects.  Nothing is ever an instance of class 0.
    //
    // Measured 2026-08-23: with PHARO_OLDSPACE_FREELIST=1 this is exactly how
    // class-shape migration died -- Pharo's migrateInstancesTo: asks
    // `oldClass allInstances`, the freshly created class has no table entry
    // (it was never instantiated -- see the classTable_ note in Primitives.cpp),
    // so targetClassIndex was 0 and the image got an Array of free chunks.  The
    // first one reached #isReadOnlyObject and took a classIndex-0 DNU.
    if (classIndex == 0) return;

    auto scanRegion = [&](uint8_t* start, uint8_t* end) {
        uint8_t* scan = start;
        while (scan < end) {
            uint64_t* wordPtr = reinterpret_cast<uint64_t*>(scan);
            uint64_t word = *wordPtr;

            if (word == 0) {
                scan += 8;
                while (scan < end) {
                    wordPtr = reinterpret_cast<uint64_t*>(scan);
                    if (*wordPtr != 0) break;
                    scan += 8;
                }
                if (scan >= end) break;
                word = *wordPtr;
            }

            uint64_t* headerPtr = wordPtr;
            uint8_t topByte = static_cast<uint8_t>((word >> 56) & 0xFF);
            if (topByte == 255 && scan + 8 < end) {
                uint64_t nextWord = *(wordPtr + 1);
                uint8_t nextNumSlots = static_cast<uint8_t>((nextWord >> 56) & 0xFF);
                if (nextNumSlots == 255) {
                    uint64_t overflowCount = (word << 8) >> 8;
                    size_t remaining = end - scan;
                    size_t neededSize = 8 + 8 + overflowCount * 8;
                    if (overflowCount >= 255 && neededSize <= remaining) {
                        headerPtr = wordPtr + 1;
                    } else {
                        scan += 16;
                        continue;
                    }
                }
            }

            ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(headerPtr);
            size_t size = obj->totalSize();
            size_t remaining = end - scan;
            if (size == 0 || size > remaining) {
                scan += 8;
                continue;
            }

            // Inlined class-index check — no std::function dispatch
            if (!obj->isForwarded() && obj->classIndex() == classIndex) {
                out.push_back(oopFromPointer(obj));
            }

            scan += size;
        }
    };

    scanRegion(permSpaceStart_, permSpaceEnd_);
    scanRegion(oldSpaceStart_, oldSpaceFree_);
    scanRegion(edenAllocBase_, edenFree_);
}

void ObjectMemory::collectInstancesOfClassInEden(uint32_t classIndex,
                                                 std::vector<Oop>& out) {
    // classIndex 0 is what makeFreeChunk stamps into every FREE CHUNK, and it
    // is also what indexOfClass answers as its "not found" sentinel
    // (ObjectMemory.cpp, `return 0;  // Not found`).  Those two meanings
    // collide: asking for the instances of a class that has no class-table
    // entry would otherwise collect every free chunk in the heap and hand them
    // to the image as objects.  Nothing is ever an instance of class 0.
    //
    // Measured 2026-08-23: with PHARO_OLDSPACE_FREELIST=1 this is exactly how
    // class-shape migration died -- Pharo's migrateInstancesTo: asks
    // `oldClass allInstances`, the freshly created class has no table entry
    // (it was never instantiated -- see the classTable_ note in Primitives.cpp),
    // so targetClassIndex was 0 and the image got an Array of free chunks.  The
    // first one reached #isReadOnlyObject and took a classIndex-0 DNU.
    if (classIndex == 0) return;

    // Same scan, young allocation area only.  Its caller (the code-zone
    // eviction pin scan) caches the whole-heap result per GC and re-walks
    // ONLY this region on every round, because eden is the only place an
    // object can appear without a GC having happened in between.  Eden is a
    // few MB; the whole heap was 826,734 objects per round, 194 million per
    // 9-second run, which is what made eviction dominate a package load.
    uint8_t* scan = edenAllocBase_;
    uint8_t* end = edenFree_;
    while (scan && scan < end) {
        uint64_t* wordPtr = reinterpret_cast<uint64_t*>(scan);
        uint64_t word = *wordPtr;
        if (word == 0) {
            scan += 8;
            while (scan < end) {
                wordPtr = reinterpret_cast<uint64_t*>(scan);
                if (*wordPtr != 0) break;
                scan += 8;
            }
            if (scan >= end) break;
            word = *wordPtr;
        }
        uint64_t* headerPtr = wordPtr;
        uint8_t topByte = static_cast<uint8_t>((word >> 56) & 0xFF);
        if (topByte == 255 && scan + 8 < end) {
            uint64_t nextWord = *(wordPtr + 1);
            uint8_t nextNumSlots = static_cast<uint8_t>((nextWord >> 56) & 0xFF);
            if (nextNumSlots == 255) {
                uint64_t overflowCount = (word << 8) >> 8;
                size_t remaining = static_cast<size_t>(end - scan);
                size_t neededSize = 8 + 8 + overflowCount * 8;
                if (overflowCount >= 255 && neededSize <= remaining) {
                    headerPtr = wordPtr + 1;
                } else {
                    scan += 16;
                    continue;
                }
            }
        }
        ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(headerPtr);
        size_t size = obj->totalSize();
        size_t remaining = static_cast<size_t>(end - scan);
        if (size == 0 || size > remaining) { scan += 8; continue; }
        if (!obj->isForwarded() && obj->classIndex() == classIndex) {
            out.push_back(oopFromPointer(obj));
        }
        scan += size;
    }
}

// ===== OBJECT ITERATION (for primitives 138/139) =====

// Helper: find the first accessible object starting at 'scan' within [scan, end).
// Returns nullptr if none found.
static ObjectHeader* findAccessibleObjectIn(uint8_t* scan, uint8_t* end, ObjectMemory& mem) {
    while (scan < end) {
        uint64_t* wordPtr = reinterpret_cast<uint64_t*>(scan);
        uint64_t word = *wordPtr;

        // Skip zero headers (free space / padding)
        if (word == 0) {
            scan += 8;
            while (scan < end) {
                wordPtr = reinterpret_cast<uint64_t*>(scan);
                if (*wordPtr != 0) break;
                scan += 8;
            }
            if (scan >= end) return nullptr;
            word = *wordPtr;
        }

        // Check for overflow header
        uint64_t* headerPtr = wordPtr;
        uint8_t topByte = static_cast<uint8_t>((word >> 56) & 0xFF);
        if (topByte == 255 && scan + 8 < end) {
            uint64_t nextWord = *(wordPtr + 1);
            uint8_t nextNumSlots = static_cast<uint8_t>((nextWord >> 56) & 0xFF);
            if (nextNumSlots == 255) {
                uint64_t overflowCount = (word << 8) >> 8;
                size_t remaining = end - scan;
                size_t neededSize = 8 + 8 + overflowCount * 8;
                if (overflowCount >= 255 && neededSize <= remaining) {
                    headerPtr = wordPtr + 1;
                } else {
                    scan += 16;
                    continue;
                }
            }
        }

        ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(headerPtr);
        size_t size = obj->totalSize();
        size_t remaining = end - scan;
        if (size == 0 || size > remaining) {
            scan += 8;
            continue;
        }

        // Check if this is a valid accessible object (classIndex != 0, valid class)
        uint32_t cls = obj->classIndex();
        if (cls != 0) {
            Oop classOop = mem.classAtIndex(cls);
            if (classOop.isObject() && !classOop.isNil()) {
                return obj;
            }
        }

        scan += size;
    }
    return nullptr;
}

Oop ObjectMemory::firstObject() {
    // Scan perm space first, then old space, then eden
    struct Region { uint8_t* start; uint8_t* end; };
    Region regions[] = {
        { permSpaceStart_, permSpaceEnd_ },
        { oldSpaceStart_, oldSpaceFree_ },
        { edenAllocBase_, edenFree_ },
    };
    for (auto& r : regions) {
        if (r.start && r.start < r.end) {
            ObjectHeader* obj = findAccessibleObjectIn(r.start, r.end, *this);
            if (obj) return oopFromPointer(obj);
        }
    }
    return Oop::fromSmallInteger(0);
}

Oop ObjectMemory::objectAfter(Oop current) {
    if (!current.isObject()) return Oop::fromSmallInteger(0);

    ObjectHeader* header = current.asObjectPtr();
    uint8_t* ptr = reinterpret_cast<uint8_t*>(header);
    // For overflow objects, the allocation starts 8 bytes before the header
    // (the overflow word precedes the main header). totalSize() includes the
    // overflow word, so we must back up ptr to the allocation start.
    if (header->hasOverflowSlots()) {
        ptr -= sizeof(uint64_t);
    }
    size_t size = header->totalSize();
    uint8_t* next = ptr + size;

    // Determine which region this object is in, then continue scanning
    struct Region { uint8_t* start; uint8_t* end; };
    Region regions[] = {
        { permSpaceStart_, permSpaceEnd_ },
        { oldSpaceStart_, oldSpaceFree_ },
        { edenAllocBase_, edenFree_ },
    };

    bool foundRegion = false;
    for (int i = 0; i < 3; i++) {
        auto& r = regions[i];
        if (!r.start || r.start >= r.end) continue;

        if (!foundRegion) {
            // Check if object is in this region
            if (ptr >= r.start && ptr < r.end) {
                foundRegion = true;
                // Try to find next object in remaining part of this region
                ObjectHeader* obj = findAccessibleObjectIn(next, r.end, *this);
                if (obj) return oopFromPointer(obj);
                // Fall through to check subsequent regions
            }
        } else {
            // Check subsequent regions
            ObjectHeader* obj = findAccessibleObjectIn(r.start, r.end, *this);
            if (obj) return oopFromPointer(obj);
        }
    }

    return Oop::fromSmallInteger(0);
}

// ===== MEMORY STATISTICS =====

ObjectMemory::Statistics ObjectMemory::statistics() const {
    Statistics stats;
    stats.bytesAllocated = bytesAllocated_;
    stats.bytesFree = (oldSpaceEnd_ - oldSpaceFree_);
    stats.objectCount = 0;  // Would need to count
    stats.gcCount = gcCount_;
    stats.totalGCTime = totalGCTime_;
    return stats;
}

// ===== PRIVATE HELPERS =====

ObjectHeader* ObjectMemory::allocateRaw(size_t size, Space space) {
    size = (size + 7) & ~7ULL;  // Align to 8 bytes
    // Spur invariant: every object occupies at least 16 bytes (2 words)
    // to guarantee space for a forwarding pointer during GC.
    if (size < 16) size = 16;

    switch (space) {
        case Space::Perm:
            // Permanent space not supported for new allocations
            return nullptr;

        case Space::Old: {
            // Reuse a post-compaction gap before extending the heap.  See
            // rebuildFreeListAfterCompact for why the gaps exist and why this
            // is opt-in.
            if (GET_DEBUG_BOOL(PHARO_OLDSPACE_FREELIST) && freeListsMask_) {
                if (ObjectHeader* reused = allocateFromFreeList(size)) {
                    bytesAllocated_ += size;
                    return reused;
                }
            }
            if (oldSpaceFree_ + size <= oldSpaceEnd_) {
                // Fast path: bump pointer allocation
                ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(oldSpaceFree_);
                oldSpaceFree_ += size;
                noteOldSpaceAdvance();   // low-space breaker; see ObjectMemory.hpp

                // Threshold-based GC trigger: request compacting GC at next safe point
                // when heap usage exceeds last compacted size + headroom.
                // This avoids running GC from allocation where C++ locals hold Oops.
                // Headroom scales with the live set.  A fixed headroom means a
                // full GC every gcHeadroom_ bytes allocated however large the
                // heap is, and there is no generational collector here — every
                // full GC marks and compacts the whole heap.  Letting the heap
                // grow by its own size before collecting bounds the share of
                // runtime spent collecting instead of letting it rise with heap
                // size, and costs only memory the collection would have
                // reclaimed anyway.
                //
                // Below gcHeadroom_ live (512 MB by default, PHARO_GC_HEADROOM_MB)
                // nothing changes: max() picks the fixed value, so the tuning
                // table in ObjectMemory.hpp still describes this VM.
                size_t used = oldSpaceFree_ - oldSpaceStart_;
                size_t headroom = std::max(gcHeadroom_, lastCompactedSize_);
                size_t gcThreshold = lastCompactedSize_ + headroom;
                if (used > gcThreshold && !needsCompactGC_) {
                    needsCompactGC_ = true;
                    // GC threshold crossed — compaction will run at next safe point
                }
                return obj;
            }
            // Bump pointer full — OOM
            return nullptr;
        }

        case Space::New: {
            // Young-gen bump-pointer allocation in eden.
            if (edenFree_ + size <= edenAllocLimit_) {
                ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(edenFree_);
                edenFree_ += size;
                return obj;
            }
            // Eden full — signal scavenge at next safe point.
            needsScavenge_ = true;
            return nullptr;
        }
        default:
            return nullptr;
    }
}

void ObjectMemory::initializeHeader(ObjectHeader* obj, uint32_t classIndex,
                                     size_t slotCount, ObjectFormat format) {
    uint8_t slots = (slotCount >= 255) ? 255 : static_cast<uint8_t>(slotCount);
    uint64_t header = ObjectHeader::makeHeader(
        slots,
        0,  // No hash initially
        format,
        classIndex
    );
    obj->setRawHeader(header);
}

Space ObjectMemory::spaceForPointer(void* ptr) const {
    uint8_t* p = static_cast<uint8_t*>(ptr);

    if (p >= permSpaceStart_ && p < permSpaceEnd_) {
        return Space::Perm;
    }
    if (p >= oldSpaceStart_ && p < oldSpaceEnd_) {
        return Space::Old;
    }
    if (p >= newSpaceStart_ && p < newSpaceEnd_) {
        return Space::New;
    }

    // Unknown - default to old
    return Space::Old;
}

Oop ObjectMemory::oopFromPointer(ObjectHeader* ptr) const {
    if (!ptr) return nilObject_;
    Space space = spaceForPointer(ptr);
    return Oop::fromObject(ptr, space);
}

// NOTE: As of 2026-05-27 `rememberedSet_` is populated here but never
// iterated (only `.clear()`-ed by scavenge at line 1599).  Scavenge
// instead does an O(oldSpace) full scan for old→young pointers (line
// 1563) to tolerate missed barriers in JIT-emitted stores.  Wiring up
// this set for use in scavenge would let us drop the full scan — see
// `jit_remembered_set_dead.md` and the AsmjitT1.cpp:1925-1928 design
// comment for the audit gap that needs closing first.
void ObjectMemory::rememberObject(Oop obj) {
    if (obj.isObject()) {
        ObjectHeader* hdr = obj.asObjectPtr();
        if (!hdr->isRemembered()) {
            hdr->setRemembered(true);
            rememberedSet_.push_back(hdr);
        }
    }
}

// ===== OBJECT ENUMERATION =====

Oop ObjectMemory::firstInstanceOf(uint32_t targetClassIndex) {
    Oop found = Oop::nil();

    // Helper to scan a memory region for the first instance
    auto scanRegion = [&](uint8_t* start, uint8_t* end) -> bool {
        uint8_t* scan = start;
        while (scan < end) {
            uint64_t* wordPtr = reinterpret_cast<uint64_t*>(scan);
            uint64_t word = *wordPtr;

            // Skip zero headers (free space / padding)
            if (word == 0) {
                scan += 8;
                while (scan < end) {
                    wordPtr = reinterpret_cast<uint64_t*>(scan);
                    if (*wordPtr != 0) break;
                    scan += 8;
                }
                if (scan >= end) break;
                word = *wordPtr;
            }

            // Check for overflow header.
            // Only when current word has 0xFF in top byte (overflow count words
            // always do). Otherwise wordPtr+1 would be a slot value, not a header.
            uint64_t* headerPtr = wordPtr;
            uint8_t topByte = static_cast<uint8_t>((word >> 56) & 0xFF);
            if (topByte == 255 && scan + 8 < end) {
                uint64_t nextWord = *(wordPtr + 1);
                uint8_t nextNumSlots = static_cast<uint8_t>((nextWord >> 56) & 0xFF);
                if (nextNumSlots == 255) {
                    uint64_t overflowCount = (word << 8) >> 8;  // mask off top byte
                    size_t remaining = end - scan;
                    size_t neededSize = 8 + 8 + overflowCount * 8;  // overflow word + header + slots

                    if (overflowCount >= 255 && neededSize <= remaining) {
                        headerPtr = wordPtr + 1;
                    } else {
                        scan += 16;
                        continue;
                    }
                }
            }

            ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(headerPtr);
            size_t size = obj->totalSize();

            size_t remaining = end - scan;
            if (size == 0 || size > remaining) {
                scan += 8;
                continue;
            }

            // Check if this object matches the target class
            if (obj->classIndex() == targetClassIndex) {
                found = oopFromPointer(obj);
                return true;  // Found
            }

            scan += size;
        }
        return false;  // Not found in this region
    };

    // Search permanent space first
    if (scanRegion(permSpaceStart_, permSpaceEnd_)) return found;

    // Search old space
    if (scanRegion(oldSpaceStart_, oldSpaceFree_)) return found;

    // Search eden
    if (scanRegion(edenAllocBase_, edenFree_)) return found;

    return nilObject_;  // Not found
}

Oop ObjectMemory::nextInstanceAfter(Oop afterObject, uint32_t targetClassIndex) {
    if (!afterObject.isObject()) return nilObject_;

    // Get the address of the starting object
    ObjectHeader* startPtr = afterObject.asObjectPtr();
    uint8_t* startAddr = reinterpret_cast<uint8_t*>(startPtr);
    // totalSize() counts the 8-byte overflow word, which sits BEFORE the
    // header — so the size must be measured from the allocation start, not
    // from the header.  Without this back-up, searchFrom landed 8 bytes past
    // the true end of any >=255-slot receiver, i.e. on the first slot of the
    // next object; the walk below then read that data word as a header and
    // stayed desynced for the rest of the region.  Same idiom as objectAfter.
    if (startPtr->hasOverflowSlots()) startAddr -= sizeof(uint64_t);
    size_t startSize = startPtr->totalSize();
    uint8_t* searchFrom = startAddr + startSize;  // Start searching AFTER this object

    Oop found = Oop::nil();
    bool foundStart = false;

    // Helper to scan a memory region
    auto scanRegion = [&](uint8_t* start, uint8_t* end) -> bool {
        // If we haven't passed the starting object yet, adjust start
        uint8_t* scan = start;
        if (!foundStart) {
            if (searchFrom >= start && searchFrom < end) {
                scan = searchFrom;
                foundStart = true;
            } else if (searchFrom >= end) {
                return false;  // Starting object is after this region
            } else {
                foundStart = true;  // Starting object was in previous region
            }
        }

        while (scan < end) {
            uint64_t* wordPtr = reinterpret_cast<uint64_t*>(scan);
            uint64_t word = *wordPtr;

            // Skip zero headers
            if (word == 0) {
                scan += 8;
                while (scan < end) {
                    wordPtr = reinterpret_cast<uint64_t*>(scan);
                    if (*wordPtr != 0) break;
                    scan += 8;
                }
                if (scan >= end) break;
                word = *wordPtr;
            }

            // Check for overflow header.
            // Only when current word has 0xFF in top byte (overflow count words
            // always do). Otherwise wordPtr+1 would be a slot value, not a header.
            uint64_t* headerPtr = wordPtr;
            uint8_t topByte = static_cast<uint8_t>((word >> 56) & 0xFF);
            if (topByte == 255 && scan + 8 < end) {
                uint64_t nextWord = *(wordPtr + 1);
                uint8_t nextNumSlots = static_cast<uint8_t>((nextWord >> 56) & 0xFF);
                if (nextNumSlots == 255) {
                    uint64_t overflowCount = (word << 8) >> 8;  // mask off top byte
                    size_t remaining = end - scan;
                    size_t neededSize = 8 + 8 + overflowCount * 8;  // overflow word + header + slots

                    if (overflowCount >= 255 && neededSize <= remaining) {
                        headerPtr = wordPtr + 1;
                    } else {
                        scan += 16;
                        continue;
                    }
                }
            }

            ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(headerPtr);
            size_t size = obj->totalSize();

            size_t remaining = end - scan;
            if (size == 0 || size > remaining) {
                scan += 8;
                continue;
            }

            // Check if this object matches the target class
            if (obj->classIndex() == targetClassIndex) {
                found = oopFromPointer(obj);
                return true;  // Found
            }

            scan += size;
        }
        return false;  // Not found in this region
    };

    // Determine which space the starting object is in and search from there
    Space startSpace = spaceForPointer(startPtr);

    switch (startSpace) {
        case Space::Perm:
            if (scanRegion(permSpaceStart_, permSpaceEnd_)) return found;
            if (scanRegion(oldSpaceStart_, oldSpaceFree_)) return found;
            if (scanRegion(edenAllocBase_, edenFree_)) return found;
            break;
        case Space::Old:
            if (scanRegion(oldSpaceStart_, oldSpaceFree_)) return found;
            if (scanRegion(edenAllocBase_, edenFree_)) return found;
            break;
        case Space::New:
            if (scanRegion(edenAllocBase_, edenFree_)) return found;
            break;
        case Space::Reserved:
            // Reserved space - search all spaces
            if (scanRegion(permSpaceStart_, permSpaceEnd_)) return found;
            if (scanRegion(oldSpaceStart_, oldSpaceFree_)) return found;
            if (scanRegion(edenAllocBase_, edenFree_)) return found;
            break;
    }

    return nilObject_;  // Not found
}

// ===== FREE LIST HELPERS =====

// A free-list link is terminated with Oop::nil(), and **nil IS an object**:
// `next.isObject()` answers true for it, so decoding the terminator as a chunk
// pointer yields heap base (0x7000000000), whose slot 0 is also nil -- a
// self-loop.  Measured 2026-08-23: the list was cyclic immediately after
// rebuild, with the LAST of 8 correctly-linked gaps pointing at heap base.
// Every reader of a link must go through here.
static inline ObjectHeader* freeListLinkToChunk(Oop link) {
    if (!link.isObject() || link.isNil()) return nullptr;
    return link.asObjectPtr();
}

ObjectHeader* ObjectMemory::makeFreeChunk(uint8_t* addr, size_t size, bool zeroBody) {
    // A free chunk has classIndex=0 and stores its size in slots.
    // Minimum free chunk is 16 bytes (8-byte header + 8-byte next pointer).
    if (size < 16) {
        // Too small for a free chunk — just zero it
        std::memset(addr, 0, size);
        return nullptr;
    }

    // A chunk big enough to need an overflow count occupies TWO header words
    // -- the count at addr, the header at addr+8 -- so its slot count is
    // (size - 16) / 8, not (size - 8) / 8.  Computing it the short way made
    // every large free chunk describe 8 bytes MORE than it owns, i.e. claim
    // the first word of whatever follows it.  Nothing consumed free chunks
    // before 2026-08-22 so it never bit, but sweepGC has been building them
    // that way, and `allocateFromFreeList` computing `remainderAddr` from
    // the shifted `chunk` is what made the opt-in free-list path stall (see
    // docs/gc-oldspace-fragmentation-2026-08-22.md).
    const bool needsOverflow = ((size - sizeof(ObjectHeader)) / 8) >= 255;
    const size_t headerBytes = needsOverflow ? 16 : 8;
    if (size < headerBytes + 8) {          // cannot hold header + one slot
        std::memset(addr, 0, size);
        return nullptr;
    }
    size_t slotCount = (size - headerBytes) / 8;

    ObjectHeader* chunk = reinterpret_cast<ObjectHeader*>(addr);
    if (needsOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(addr);
        *overflow = slotCount | (0xFFULL << 56);
        chunk = reinterpret_cast<ObjectHeader*>(addr + 8);
        chunk->setRawHeader(
            ObjectHeader::makeHeader(255, 0, ObjectFormat::ZeroSized, 0));
    } else {
        chunk->setRawHeader(ObjectHeader::makeHeader(
            static_cast<uint8_t>(slotCount), 0, ObjectFormat::ZeroSized, 0));
    }

    // Zero the body (next pointer in slot 0 will be set by addToFreeList).
    // Skipped when splitting an existing free chunk: that body is already
    // free-chunk memory, and zeroing it costs O(chunk size) on every single
    // allocation once the list holds one big post-compaction gap.
    if (zeroBody) std::memset(chunk->bytes(), 0, slotCount * 8);

    return chunk;
}

void ObjectMemory::addToFreeList(ObjectHeader* chunk, size_t size) {
    // Size in 8-byte units (including header)
    size_t sizeInSlots = size / 8;

    if (sizeInSlots > 0 && sizeInSlots < NumFreeLists) {
        // Exact-size list: singly linked via slot 0
        Oop next = (freeLists_[sizeInSlots] != nullptr)
            ? Oop::fromObject(freeLists_[sizeInSlots])
            : Oop::nil();
        if (chunk->slotCount() > 0) {
            chunk->slotAtPut(0, next);
        }
        freeLists_[sizeInSlots] = chunk;
        freeListsMask_ |= (1ULL << sizeInSlots);
    } else {
        // Large chunk list (index 0): singly linked via slot 0
        Oop next = (freeLists_[0] != nullptr)
            ? Oop::fromObject(freeLists_[0])
            : Oop::nil();
        if (chunk->slotCount() > 0) {
            chunk->slotAtPut(0, next);
        }
        freeLists_[0] = chunk;
        freeListsMask_ |= 1ULL;
    }
}

ObjectHeader* ObjectMemory::allocateFromFreeList(size_t size) {
    size_t sizeInSlots = size / 8;

    // Try exact-size list first
    if (sizeInSlots > 0 && sizeInSlots < NumFreeLists) {
        if (freeListsMask_ & (1ULL << sizeInSlots)) {
            ObjectHeader* chunk = freeLists_[sizeInSlots];
            if (chunk) {
                // Pop from list
                Oop next = chunk->slotCount() > 0 ? chunk->slotAt(0) : Oop::nil();
                freeLists_[sizeInSlots] = freeListLinkToChunk(next);
                if (!freeLists_[sizeInSlots]) {
                    freeListsMask_ &= ~(1ULL << sizeInSlots);
                }
                if (GET_DEBUG_BOOL(PHARO_FREECHUNK_REFS)) {
                    fprintf(stderr, "[FREELIST-ALLOC-EXACT] %p size=%zu\n",
                            (void*)chunk, size);
                }
                return chunk;
            }
        }
    }

    // Try large chunk list (first fit)
    if (freeListsMask_ & 1ULL) {
        // `prevChunk == nullptr` means the head pointer itself is the link to
        // update; otherwise the link lives in prevChunk's slot 0 and MUST be
        // written as an encoded Oop.  The old code kept an ObjectHeader** into
        // the heap and stored a RAW pointer through it, so unlinking a mid-list
        // chunk wrote 0 (from `nullptr`) into the previous chunk's slot 0.
        // Read back through slotAt(0) that 0 decodes to heap base
        // (0x7000000000), whose own slot 0 is also 0 -- a self-loop, which is
        // exactly the cycle the guard below reports.
        ObjectHeader* prevChunk = nullptr;
        ObjectHeader* chunk = freeLists_[0];
        // Floyd cycle guard.  A cycle in the large-chunk list makes this walk
        // spin forever with the interpreter holding the CPU: measured
        // 2026-08-23 with PHARO_OLDSPACE_FREELIST=1, the VM froze at exactly
        // 17,520 bytecode steps at 99% CPU, and every sample was inside this
        // function (via primitiveNewWithArg -> allocateBytes).  Detect it and
        // fall back to bump allocation rather than hang; the same list is
        // walked again on the next allocation, so a one-shot report is enough
        // to know the list is corrupt without drowning the log.
        ObjectHeader* slow = chunk;
        bool advanceSlow = false;
        while (chunk) {
            size_t chunkSize = chunk->totalSize();
            // Require an exact fit or a leftover big enough to become its
            // own free chunk.  Accepting a 1-15 byte leftover left it neither
            // split off nor zeroed, and the caller then writes a header
            // claiming only `size` -- the next scanner walk steps straight
            // into the orphaned tail.
            if (chunkSize == size || (chunkSize > size && chunkSize - size >= 16)) {
                // Unlink
                Oop next = chunk->slotCount() > 0 ? chunk->slotAt(0) : Oop::nil();
                if (prevChunk) {
                    if (prevChunk->slotCount() > 0) prevChunk->slotAtPut(0, next);
                } else {
                    freeLists_[0] = freeListLinkToChunk(next);
                }
                if (!freeLists_[0]) {
                    freeListsMask_ &= ~1ULL;
                }

                // If leftover is big enough, put remainder back.  Measure
                // from the chunk's TRUE start: for an overflow chunk `chunk`
                // points one word past it, and using `chunk` as the base
                // shifted every split by 8 bytes, compounding until the heap
                // stopped parsing.
                size_t remainder = chunkSize - size;
                uint8_t* chunkBase = reinterpret_cast<uint8_t*>(chunk);
                if (chunk->hasOverflowSlots()) chunkBase -= 8;
                if (remainder >= 16) {
                    uint8_t* remainderAddr = chunkBase + size;
                    ObjectHeader* remChunk =
                        makeFreeChunk(remainderAddr, remainder, /*zeroBody=*/false);
                    if (remChunk) {
                        addToFreeList(remChunk, remainder);
                    }
                }

                if (GET_DEBUG_BOOL(PHARO_FREECHUNK_REFS)) {
                    fprintf(stderr, "[FREELIST-ALLOC] %p size=%zu (chunk was %zu)\n",
                            (void*)chunkBase, size, chunkSize);
                }
                // Return the chunk's BASE, not `chunk`.  makeFreeChunk answers
                // addr+8 for an overflow chunk (the count word sits at addr),
                // and the caller treats this result exactly like the bump
                // path's `oldSpaceFree_` -- the START of the allocation.
                // Returning the shifted pointer laid the new object at
                // chunkBase+8 while `remainder` above was measured from
                // chunkBase, so the object's last 8 bytes overlapped the
                // remainder chunk's header: heap corruption that shows up
                // later as a garbage link in the free list.
                return reinterpret_cast<ObjectHeader*>(chunkBase);
            }
            // Advance
            Oop next = chunk->slotCount() > 0 ? chunk->slotAt(0) : Oop::nil();
            prevChunk = chunk;
            chunk = freeListLinkToChunk(next);

            // Floyd: advance the slow pointer every other step.  If it ever
            // meets the fast one the list loops back on itself.
            if (advanceSlow && slow) {
                Oop sNext = slow->slotCount() > 0 ? slow->slotAt(0) : Oop::nil();
                slow = sNext.isObject() ? sNext.asObjectPtr() : nullptr;
            }
            advanceSlow = !advanceSlow;
            if (chunk && chunk == slow) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    fprintf(stderr,
                            "[FREELIST-CYCLE] large-chunk free list loops at %p "
                            "(request %zu bytes) -- DROPPING the large-chunk "
                            "list and falling back to bump allocation\n",
                            (void*)chunk, size);
                    fflush(stderr);
                }
                // DROP the list rather than merely abandoning this walk.
                // Returning nullptr alone leaves the cycle in place, so every
                // later allocation re-walks it to detection: measured
                // 2026-08-23, a NeoJSON load with the knob on still ran but at
                // ~240k bytecodes/s (213 M steps in 890 s) instead of
                // finishing in 23 s.  A list known to be corrupt is worth
                // nothing; forgetting it costs only the reuse it was going to
                // provide, which is the pre-knob behaviour.
                freeLists_[0] = nullptr;
                freeListsMask_ &= ~1ULL;
                return nullptr;
            }
        }
    }

    return nullptr;  // No suitable free chunk found
}

void ObjectMemory::clearFreeLists() {
    freeLists_.fill(nullptr);
    freeListsMask_ = 0;
}

// ===== MARK PHASE =====

void ObjectMemory::markAndTrace(Oop oop) {
    if (!oop.isObject()) return;

    ObjectHeader* obj = oop.asObjectPtr();

    // Don't mark permanent space objects (they never move/die)
    if (isPermObject(obj)) return;

    // Must be within USED old space bounds (not just allocated range).
    // Pointers beyond oldSpaceFree_ point to unallocated space; treating
    // the data there as headers would read garbage and corrupt mark state.
    // Eden objects also need to be traced so old objects referenced
    // only through eden don't get swept.  Eden doesn't get swept itself,
    // but we still use mark bits to avoid re-visiting during trace; a
    // post-compact pass clears them.
    auto p = reinterpret_cast<uint8_t*>(obj);
    bool inOld = (p >= oldSpaceStart_ && p < oldSpaceFree_);
    bool inEden = (p >= edenAllocBase_ && p < edenFree_);
    if (!inOld && !inEden) {
        return;
    }

    // Already marked?
    if (obj->isMarked()) return;

    // Validate this is a real object header, not an interior pointer.
    // Interior pointers (pointing into the middle of another object) would have
    // random slot data at their "header" position. Calling setMarked() on such
    // data corrupts the containing object by flipping bit 30 (MarkedBit) on
    // arbitrary data, which then cascades through scanPointerFields reading
    // garbage as format/slots.
    uint32_t classIdx = obj->classIndex();
    if (classIdx == 0 || classIdx >= classTable_.size() ||
        !classTable_[classIdx].isObject()) {
        return;
    }

    // Definitive interior pointer check: verify this address is at a real object
    // start. The classIndex check above can pass for interior pointers if the
    // slot data happens to have bits 0-21 matching a valid class table entry.
    // Eden objects aren't in validObjectStarts_ (built from old-space scan),
    // so skip the check for eden — its range is always valid live objects.
    if (inOld && !validObjectStarts_.empty()) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
        if (validObjectStarts_.find(addr) == validObjectStarts_.end()) {
            return;
        }
    }

    // Mark it
    obj->setMarked(true);

    // Classify by format
    ObjectFormat fmt = obj->format();
    if (fmt == ObjectFormat::WeakWithFixed) {
        // Format 5 = Ephemeron. Key is ALWAYS the first inst var (slot 0).
        // In Spur, ephemeron key is slot[0], values are all remaining slots.
        // If key is immediate or already marked → "inactive" ephemeron, treat as strong.
        // Otherwise → "active" ephemeron, defer to ephemeronList_.
        size_t total = obj->slotCount();
        constexpr size_t keyIndex = 0;  // Key is always slot 0 for ephemerons
        bool keyAlive = true;
        Oop keyForLog = Oop::nil();
        if (total > 0) {
            Oop key = obj->slotAt(keyIndex);
            keyForLog = key;
            // Same bound as the three sites hardened in 248ceff8 — this fourth
            // sibling was missed there, and it is the one that runs FIRST:
            // markAndTrace decides which list the ephemeron lands on, so it
            // faults before markInactiveEphemerons/fireAllEphemerons get the
            // chance to guard.  `obj` is bounds-checked above; `key` is raw
            // image data out of slot 0 and is not.
            if (key.isObject() && !isReadableHeapObject(key)) {
                keyAlive = false;  // outside every heap region — never deref
            } else if (key.isObject() && !isPermObject(key.asObjectPtr())) {
                keyAlive = key.asObjectPtr()->isMarked();
            }
            // Immediates and perm objects are always "alive"
        }

        ephemeronEncounterCount_++;
        if (keyAlive) ephemeronInactiveCount_++;
        else ephemeronActiveCount_++;
        (void)keyForLog;

        if (keyAlive) {
            // Inactive ephemeron: treat as fully strong (mark all pointer fields)
            markStack_.push_back(obj);
        } else {
            // Active ephemeron: defer ALL tracing. Do NOT mark any fields.
            // Matching Cog VM behavior: active ephemerons are added to the list
            // and no fields are traced until the key's fate is determined.
            // This is critical: if value == key (e.g., Object>>finalizer returns
            // self), marking the value would mark the key, preventing firing.
            // Fields are traced later by markInactiveEphemerons (if key becomes
            // reachable) or fireAllEphemerons (if key remains unreachable).
            ephemeronList_.push_back(obj);
        }
    } else if (fmt == ObjectFormat::Weak) {
        // Format 4 = Weak array. Add to weakList_, mark only fixed fields.
        weakList_.push_back(obj);
        size_t fixedFields = fixedFieldCountOf(obj);
        if (fixedFields > 0) {
            Oop* slots = obj->slots();
            size_t total = obj->slotCount();
            for (size_t i = 0; i < fixedFields && i < total; ++i) {
                markAndTrace(slots[i]);
            }
        }
    } else {
        markStack_.push_back(obj);
    }
}

void ObjectMemory::processMarkStack() {
    while (!markStack_.empty()) {
        ObjectHeader* obj = markStack_.back();
        markStack_.pop_back();
        scanPointerFields(obj);
    }
}

void ObjectMemory::printMarkParentChain(uint64_t oop) {
    uint64_t cur = oop;
    std::unordered_set<uint64_t> seenHops;
    for (int hop = 0; hop < 24; hop++) {
        auto it = markParent_.find(cur);
        if (it == markParent_.end()) {
            auto rt = markRootTag_.find(cur);
            fprintf(stderr, " <- ROOT(%s)",
                    (rt != markRootTag_.end() && rt->second) ? rt->second : "?");
            break;
        }
        uint32_t viaSlot = it->second.slot;
        cur = it->second.parent;
        Oop parentO = Oop::fromRawBits(cur);
        fprintf(stderr, " <- %s(0x%llx)[%u]", classNameOf(parentO).c_str(),
                (unsigned long long)cur, viaSlot);
        // A Context is the interesting case: say whether the holding slot is
        // LIVE (below stackp) or dead residue above it, and name the activation.
        fprintf(stderr, "%s", describeContextSlot(parentO, viaSlot).c_str());
        if (!seenHops.insert(cur).second) { fprintf(stderr, " <- (cycle)"); break; }
    }
    fprintf(stderr, "\n");
}

void ObjectMemory::reportWatchedChains() {
    if (heapWatchHits_.empty()) return;
    std::unordered_set<uint64_t> seen;
    for (uint64_t oop : heapWatchHits_) {
        if (!seen.insert(oop).second) continue;
        fprintf(stderr, "[HEAP-CHAIN] 0x%llx", (unsigned long long)oop);
        printMarkParentChain(oop);
    }
    heapWatchHits_.clear();
}

std::string ObjectMemory::describeContextSlot(Oop parent, uint32_t slot) const {
    if (!parent.isObject() || classNameOf(parent) != "Context") return std::string();
    ObjectHeader* pc = parent.asObjectPtr();
    Oop stackpO = pc->slotCount() > 2 ? pc->slotAt(2) : Oop::nil();
    long long stackp = stackpO.isSmallInteger() ? stackpO.asSmallInteger() : -1;
    long long liveEnd = 6 + stackp;   // slots 0-5 are the fixed fields
    char buf[192];
    snprintf(buf, sizeof buf, "{stackp=%lld liveSlots=6..%lld slot%s method=%s}",
             stackp, liveEnd - 1,
             (stackp >= 0 && (long long)slot >= liveEnd) ? "=DEAD-RESIDUE" : "=live",
             pc->slotCount() > 3 ? selectorOf(pc->slotAt(3)).c_str() : "?");
    return std::string(buf);
}

void ObjectMemory::scanPointerFields(ObjectHeader* obj) {
    // Mark the class of this object via classIndex.
    // In Spur, the class is NOT a pointer slot — it's encoded as an index in
    // the object header.  The standard VM's scanPointerFieldsOfMaybeFiredEphemeron:
    // calls markAndTraceClassOf: here to keep metaclasses (and any class only
    // reachable via classIndex) alive while instances exist.
    uint32_t classIdx = obj->classIndex();
    if (classIdx > 0 && classIdx < classTable_.size()) {
        markAndTrace(classTable_[classIdx]);
    }

    size_t numPointers = pointerSlotsOf(obj);
    Oop* slots = obj->slots();
#if PHARO_HOT_PATH_DIAG
    currentScanParent_ = obj;
#endif
    // PHARO_WATCH_HEAP_CLASSIDX: report the PARENT that reaches each instance
    // of the watched class.  PHARO_WATCH_ROOT_CLASS answers "is it a direct
    // root?"; when the answer is no, this answers "then who holds it?".
    // Split into two loops so the non-watching path keeps the bare
    // markAndTrace call — this is the hottest loop in the collector.
    // Resolve the watched class NAME to its classIndex once per process by
    // scanning classTable_ — comparing names per slot would swamp the mark.
    static const int watchIdx = [this]() -> int {
        if (int idx = GET_DEBUG_INT(PHARO_WATCH_HEAP_CLASSIDX)) return idx;
        const char* nm = GET_DEBUG_STR(PHARO_WATCH_HEAP_CLASS);
        if (!nm) return 0;
        for (size_t i = 1; i < classTable_.size(); i++) {
            if (!classTable_[i].isObject()) continue;
            if (nameOfClass(classTable_[i]) == nm) {
                fprintf(stderr, "[HEAP-WATCH] %s => classIndex %zu\n", nm, i);
                return (int)i;
            }
        }
        fprintf(stderr, "[HEAP-WATCH] class %s not found in classTable\n", nm);
        return 0;
    }();
    // PHARO_WEAK_SURVIVOR_PATHS: remember, for every object, the FIRST parent
    // that reached it during this mark.  processWeaklings then walks that chain
    // back for each weak referent that survived, which is the direct answer to
    // "why wasn't this collected?".  One map entry per marked object, so only
    // for a deliberate diagnostic run.
    if (__builtin_expect(recordMarkParents_, 0)) {
        Oop parentOop = Oop::fromObject(obj);
        for (size_t i = 0; i < numPointers; ++i) {
            Oop child = slots[i];
            if (child.isObject() && child.rawBits() > 0x10000) {
                markParent_.emplace(child.rawBits(),
                                    MarkParent{parentOop.rawBits(), (uint32_t)i});
                // Collect watched-class instances for the post-fixpoint chain
                // report.  Logging here (as the plain watch does) would print
                // the immediate parent only, and the parent map is not
                // complete until the mark reaches its fixpoint.
                if (watchIdx != 0
                        && child.asObjectPtr()->classIndex() == (uint32_t)watchIdx)
                    heapWatchHits_.push_back(child.rawBits());
            }
            markAndTrace(child);
        }
#if PHARO_HOT_PATH_DIAG
        currentScanParent_ = nullptr;
#endif
        return;
    }
    if (__builtin_expect(watchIdx != 0, 0)) {
        for (size_t i = 0; i < numPointers; ++i) {
            Oop child = slots[i];
            if (child.isObject() && child.rawBits() > 0x10000
                    && child.asObjectPtr()->classIndex() == (uint32_t)watchIdx
                    && heapWatchLogged_ < GET_DEBUG_INT(PHARO_WATCH_HEAP_MAXLOG)) {
                heapWatchLogged_++;
                Oop parentOop = Oop::fromObject(obj);
                fprintf(stderr,
                    "[HEAP-WATCH] 0x%llx <- parent 0x%llx cls=%s slot=%zu/%zu %s\n",
                    (unsigned long long)child.rawBits(),
                    (unsigned long long)parentOop.rawBits(),
                    classNameOf(parentOop).c_str(),
                    i, numPointers,
                    describeContextSlot(parentOop, (uint32_t)i).c_str());
            }
#if PHARO_HOT_PATH_DIAG
            currentScanSlot_ = i;
#endif
            markAndTrace(child);
        }
    } else
    for (size_t i = 0; i < numPointers; ++i) {
#if PHARO_HOT_PATH_DIAG
        currentScanSlot_ = i;
#endif
        markAndTrace(slots[i]);
    }
#if PHARO_HOT_PATH_DIAG
    currentScanParent_ = nullptr;
#endif
}

size_t ObjectMemory::pointerSlotsOf(ObjectHeader* obj) const {
    ObjectFormat fmt = obj->format();

    // Pointer objects (formats 0-5): all slots are pointers
    if (fmt <= ObjectFormat::WeakWithFixed) {
        size_t totalSlots = obj->slotCount();

        // A Context's live extent is its stackp, exactly as in Spur, whose
        // numPointerSlotsOf: returns CtxtTempFrameStart + stackp for a
        // MethodContext.  Scanning the WHOLE slot array instead — which this
        // did until 2026-08-11 — makes dead expression-stack residue a GC
        // root: a returned activation's context still held a
        // PropertyManagerTestObject at slot 8 with stackp=2, so the object
        // outlived a collection it should not have and
        // PropertyManagerTest>>testPropertyManagerValueWeakness failed here
        // and passed on Cog.  Residue above stackp is unreachable by the
        // language's own rules, so not tracing it is not a heuristic.
        //
        // The full scan was introduced because prepareForGC could sync temps
        // into a Context without raising stackp to cover them, and a stackp
        // too LOW makes both the mark and the post-compaction pointer update
        // skip live slots (stale pointers -> classIdx=0 crashes).  Both temp
        // syncs now raise stackp themselves (Interpreter::prepareForGC), and
        // Interpreter::storeContextStackp nils the tail whenever it lowers
        // stackp, so nothing meaningful is left above it either way.
        // PHARO_CTX_TRACE_ALL_SLOTS=1 restores the old scan for bisecting.
        if (totalSlots > 2 && contextClassIndex_ != 0
                && obj->classIndex() == contextClassIndex_
                && !GET_DEBUG_BOOL(PHARO_CTX_TRACE_ALL_SLOTS)) {
            Oop sp = obj->slotAt(2);
            if (sp.isSmallInteger()) {
                int64_t stackp = sp.asSmallInteger();
                if (stackp >= 0 && (size_t)(6 + stackp) <= totalSlots)
                    return (size_t)(6 + stackp);
            }
        }
        return totalSlots;
    }

    // CompiledMethods (formats 24-31): only literal frame is pointers
    if (obj->isCompiledMethod()) {
        size_t totalSlots = obj->slotCount();
        if (totalSlots == 0) return 0;

        // Slot 0 is the method header (SmallInteger)
        Oop methodHeader = obj->slotAt(0);
        if (methodHeader.isSmallInteger()) {
            size_t numLiterals = methodHeader.asSmallInteger() & 0x7FFF;
            // Pointer slots = header + literals = numLiterals + 1
            return std::min(numLiterals + 1, totalSlots);
        }
        return 1;  // At least the header slot
    }

    // Byte/word/short objects: no pointer slots
    return 0;
}

void ObjectMemory::processWeaklings() {
    size_t weakCount = 0, nilledCount = 0, queuedCount = 0;
    for (ObjectHeader* obj : weakList_) {
        weakCount++;
        size_t slots = obj->slotCount();
        Oop* slotPtr = obj->slots();
        size_t startSlot = fixedFieldCountOf(obj);
        bool anyNilled = false;
        for (size_t i = startSlot; i < slots; ++i) {
            Oop ref = slotPtr[i];
            // A weak slot holding something that is not in any heap region
            // cannot be alive, and must never be dereferenced: reading
            // ->isMarked() on it is how an unrelocated saved-image address
            // (0x10000000000, the old image base) killed the VM inside
            // markPhase on a freshly loaded Ume image
            // (docs/vm-compat-bugs.md #4).  Report it — the value is a
            // symptom of an ImageLoader/relocation defect, not something to
            // swallow — then nil the slot, which is what a weak reference to
            // a non-object means.
            if (ref.isObject() && !isReadableHeapObject(ref)) {
                static int badWeakN = 0;
                if (++badWeakN <= 20) {
                    fprintf(stderr,
                        "[WEAK-BAD-REF] %s(0x%llx) slot %zu holds 0x%llx — not "
                        "in old space, eden or perm space.  Nilled instead of "
                        "dereferenced; the value itself is an unrelocated or "
                        "corrupt pointer and wants root-causing.\n",
                        classNameOf(Oop::fromObject(obj)).c_str(),
                        (unsigned long long)Oop::fromObject(obj).rawBits(), i,
                        (unsigned long long)ref.rawBits());
                }
                slotPtr[i] = nilObject_;
                anyNilled = true;
                nilledCount++;
                continue;
            }
            if (ref.isObject() && !isPermObject(ref.asObjectPtr())) {
                // Nil if dead, OR if only alive because a fired ephemeron
                // rescued it for the mourn queue (not strongly reachable —
                // stock Spur nils weaklings before retracing fired keys).
                if (!ref.asObjectPtr()->isMarked()
                        || ephemeronRescuedKeys_.count(ref.asObjectPtr())) {
                    slotPtr[i] = nilObject_;
                    anyNilled = true;
                    nilledCount++;
                } else if (__builtin_expect(logWeakSurvivorClasses_, 0)
                           && !recordMarkParents_
                           && heapWatchLogged_++ < GET_DEBUG_INT(PHARO_WATCH_HEAP_MAXLOG)) {
                    // Cheap sibling of PHARO_WEAK_SURVIVOR_PATHS: names the
                    // surviving referent WITHOUT the per-object parent map, so
                    // it can be left on while chasing a timing-sensitive
                    // survivor that the full tracer suppresses.
                    fprintf(stderr, "[WEAK-ALIVE-CLS] %s in %s slot %zu\n",
                            classNameOf(ref).c_str(),
                            classNameOf(Oop::fromObject(obj)).c_str(), i);
                } else if (__builtin_expect(recordMarkParents_, 0)
                           && (weakPathFilter_ == nullptr
                               || classNameOf(ref) == weakPathFilter_)
                           && heapWatchLogged_++ < GET_DEBUG_INT(PHARO_WATCH_HEAP_MAXLOG)) {
                    // Survivor: walk the recorded parent chain back to a root
                    // and print it.  This names the object that is keeping the
                    // weak referent alive.
                    fprintf(stderr, "[WEAK-ALIVE] %s in %s slot %zu:",
                            classNameOf(ref).c_str(),
                            classNameOf(Oop::fromObject(obj)).c_str(), i);
                    printMarkParentChain(ref.rawBits());
                }
            }
        }
        if (anyNilled) {
            // Queue weak object as mourner (matches Spur behavior).
            // WeakFinalizationList detects collected entries this way.
            mournQueue_.push_back(Oop::fromObject(obj));
            pendingFinalizationSignals_++;
            queuedCount++;
        }
    }
    if (g_debug.gcEphDebug) {
        fprintf(stderr, "[GC-WEAK] weakArrays=%zu slotsNilled=%zu arraysQueued=%zu\n",
                weakCount, nilledCount, queuedCount);
    }
}

bool ObjectMemory::markInactiveEphemerons() {
    bool foundInactive = false;
    size_t writeIdx = 0;
    size_t transitioned = 0;

    for (size_t i = 0; i < ephemeronList_.size(); ++i) {
        ObjectHeader* obj = ephemeronList_[i];
        size_t total = obj->slotCount();
        constexpr size_t keyIndex = 0;  // Key is always slot 0

        bool keyAlive = true;
        if (total > 0) {
            Oop key = obj->slotAt(keyIndex);
            if (key.isObject() && !isReadableHeapObject(key)) {
                keyAlive = false;  // see [WEAK-BAD-REF] above — never deref
            } else if (key.isObject() && !isPermObject(key.asObjectPtr())) {
                keyAlive = key.asObjectPtr()->isMarked();
            }
        }

        if (keyAlive) {
            // Key became reachable — mark all fields as strong
            scanPointerFields(obj);
            processMarkStack();
            foundInactive = true;
            transitioned++;
            // Don't keep in list (removed by not copying to writeIdx)
        } else {
            // Still active — keep in list
            ephemeronList_[writeIdx++] = obj;
        }
    }
    ephemeronList_.resize(writeIdx);
    if (g_debug.gcEphDebug && transitioned > 0) {
        fprintf(stderr, "  [MIE] transitioned=%zu remaining=%zu\n",
                transitioned, writeIdx);
    }
    return foundInactive;
}

void ObjectMemory::fireAllEphemerons() {
    // Fire a snapshot of currently-active ephemerons. Marking their contents
    // (scanPointerFields) may discover new ephemerons and append them to
    // ephemeronList_; those must be left for a follow-up markInactive/fire
    // fixed-point pass — if we missed them here, the finalization signal
    // count undercounts by ~1 per WeakKeyDictionary entry chain.
    size_t count = ephemeronList_.size();
    for (size_t i = 0; i < count; ++i) {
        ObjectHeader* obj = ephemeronList_[i];
        obj->setFormat(ObjectFormat::FixedSize);

        Oop objOop = Oop::fromObject(obj);
        mournQueue_.push_back(objOop);
        pendingFinalizationSignals_++;

        // The key is about to be RESCUED (marked) so it survives for the
        // mourn queue — but it was not strongly reachable, so weak slots
        // referencing it must still read nil this cycle.  Record it for
        // processWeaklings (which runs after the ephemeron fixed point).
        if (obj->slotCount() > 0) {
            Oop key = obj->slotAt(0);
            if (key.isObject() && isReadableHeapObject(key)
                    && !isPermObject(key.asObjectPtr())
                    && !key.asObjectPtr()->isMarked()) {
                ephemeronRescuedKeys_.insert(key.asObjectPtr());
            }
        }

        scanPointerFields(obj);
        processMarkStack();
    }
    // Remove only the entries we fired. Preserve any newly-appended ephemerons
    // discovered during the marking above.
    ephemeronList_.erase(ephemeronList_.begin(), ephemeronList_.begin() + count);
}

void ObjectMemory::markClassTablePages() {
    // Equivalent of Spur's markAndTraceHiddenRoots.
    // Class table page objects live in the heap but are only referenced from
    // hiddenRootsObj (format 9 = Indexable64, whose slots are NOT traced by
    // scanPointerFields). Without explicit marking, compaction treats them
    // as dead and overwrites them.
    //
    // We use classTablePages_ (the C++ side structure populated at load time
    // and kept current by forEachMemoryRoot) rather than reading from
    // hiddenRootsObj directly, since hiddenRoots is format 9 and its slots
    // aren't managed by the GC's pointer update machinery.

    // Mark hiddenRootsObj itself
    if (hiddenRootsObj_.isObject()) {
        hiddenRootsObj_.asObjectPtr()->setMarked(true);
    }

    // Mark freeListsObj (it's also format 9 and must survive)
    if (freeListsObj_.isObject()) {
        freeListsObj_.asObjectPtr()->setMarked(true);
    }

    // Mark class table page objects
    for (size_t i = 0; i < classTablePages_.size(); ++i) {
        Oop pageOop = classTablePages_[i];
        if (!pageOop.isObject() || pageOop.rawBits() == 0) continue;
        if (pageOop == nilObject_) continue;

        ObjectHeader* page = pageOop.asObjectPtr();
        auto p = reinterpret_cast<uint8_t*>(page);
        if (p < oldSpaceStart_ || p >= oldSpaceFree_) continue;

        if (i == 0) {
            // Page 0 contains classes for immediate types (SmallInteger at 1,
            // Character at 2, SmallFloat at 4). Fully trace it so those
            // classes and everything they reference stays alive.
            if (!page->isMarked()) {
                page->setMarked(true);
                markStack_.push_back(page);
            }
        } else {
            // Pages 1..N: just set the mark bit. The class objects themselves
            // are already marked via classTable_ in forEachMemoryRoot.
            page->setMarked(true);
        }
    }
}

void ObjectMemory::dumpClassTableConsistency(const char* when) {
    // 1) Class-table self-consistency: every live entry should be an object,
    //    and (Spur invariant) its identityHash should equal its index.
    size_t liveEntries = 0, hashMismatch = 0, badEntry = 0, maxUsed = 0;
    for (size_t i = 8; i < classTable_.size(); ++i) {
        Oop e = classTable_[i];
        if (!e.isObject() || e.isNil()) continue;
        liveEntries++;
        maxUsed = i;
        if (!isValidPointer(e)) { badEntry++; continue; }
        uint32_t h = e.asObjectPtr()->identityHash();
        if (h != i) hashMismatch++;
    }
    // 2) Orphaned instances: walk the heap; for each object, classOf requires
    //    classTable_[classIndex] to be a live class.  Count objects whose
    //    class slot is nil/invalid (classOf would return garbage → corruption).
    size_t scanned = 0, orphan = 0;
    uint32_t firstOrphanIdx = 0;
    Oop o = firstObject();
    Oop prevObj = Oop::nil();
    const char* stopReason = "reached-free-ptr";
    uint8_t* freePtr = oldSpaceFree_;
    while (o.isObject() && scanned < 50000000) {
        scanned++;
        uint8_t* op = reinterpret_cast<uint8_t*>(o.rawBits());
        if (op >= freePtr) { stopReason = "reached-free-ptr"; break; }
        uint32_t ci = o.asObjectPtr()->classIndex();
        if (ci != 0) {
            Oop cls = (ci < classTable_.size()) ? classTable_[ci] : Oop::nil();
            if (!cls.isObject() || cls.isNil() || !isValidPointer(cls)) {
                if (orphan == 0) firstOrphanIdx = ci;
                orphan++;
            }
        }
        Oop nxt = objectAfter(o);
        if (!nxt.isObject()) { stopReason = "objectAfter-not-object"; break; }
        if (nxt.rawBits() == o.rawBits()) { stopReason = "objectAfter-same-addr"; break; }
        uint8_t* np = reinterpret_cast<uint8_t*>(nxt.rawBits());
        if (np < op) { stopReason = "objectAfter-went-backward"; break; }
        prevObj = o;
        o = nxt;
    }
    // If we stopped before the free pointer, the last object we were ON (o) is
    // suspect: its header (size/format) likely derailed objectAfter.
    uint8_t* stopAddr = reinterpret_cast<uint8_t*>(o.rawBits());
    bool earlyStop = (stopAddr < freePtr);
    fprintf(stderr, "[CTCHECK %s] nextClassIndex_=%u liveEntries=%zu maxUsedIdx=%zu "
                    "hashMismatch=%zu badEntry=%zu | heapScanned=%zu orphanInstances=%zu firstOrphanClassIdx=%u"
                    " | stop=%s earlyStop=%d\n",
            when, nextClassIndex_, liveEntries, maxUsed, hashMismatch, badEntry,
            scanned, orphan, firstOrphanIdx, stopReason, (int)earlyStop);
    if (earlyStop && o.isObject()) {
        ObjectHeader* h = o.asObjectPtr();
        uint64_t raw = *reinterpret_cast<uint64_t*>(h);
        fprintf(stderr, "[CTCHECK %s]   DERAIL at obj=0x%llx hdrRaw=0x%016llx classIdx=%u fmt=%d slotCount=%zu marked=%d"
                        " | prevObj=0x%llx prevClassIdx=%u\n",
                when, (unsigned long long)o.rawBits(), (unsigned long long)raw,
                h->classIndex(), (int)h->format(), h->slotCount(), (int)h->isMarked(),
                (unsigned long long)prevObj.rawBits(),
                prevObj.isObject() ? prevObj.asObjectPtr()->classIndex() : 0);
    }
}

void ObjectMemory::sweepClassTable() {
    // After mark phase: clear class table entries whose class objects were not
    // marked (unreachable). This allows anonymous/transient classes to be GC'd.
    // Matches standard Spur behavior where class table pages 1+ are only marked
    // (kept alive as containers) but their entries are not strong roots.
    //
    // Skip indices 0-7: these are reserved for immediate types and special
    // class index puns (free chunks, forwarding pointers, etc.).
    for (size_t i = 8; i < classTable_.size(); ++i) {
        Oop entry = classTable_[i];
        if (!entry.isObject()) continue;
        ObjectHeader* obj = entry.asObjectPtr();
        auto p = reinterpret_cast<uint8_t*>(obj);
        if (p < oldSpaceStart_ || p >= oldSpaceFree_) continue;
        if (!obj->isMarked()) {
            if (g_debug.gcEphDebug) {  // reuse PHARO_GC_EPH_DEBUG for sweep forensics
                static int sweepLog = 0;
                if (sweepLog++ < 40)
                    fprintf(stderr, "[CLASS-SWEEP] idx=%zu cls=%s oop=0x%llx\n",
                            i, classNameOf(entry).c_str(),
                            (unsigned long long)entry.rawBits());
            }
            classTable_[i] = nilObject_;
            // Also nil the in-heap page slot (same addressing as
            // syncClassTableToHeap): a swept class must not leave a dangling
            // pointer in the page Array for the compactor or image writer.
            {
                constexpr size_t PageSize = 1024;
                size_t pageNum = i / PageSize;
                size_t slotNum = i % PageSize;
                if (pageNum < classTablePages_.size()) {
                    Oop pageOop = classTablePages_[pageNum];
                    if (pageOop.isObject() && pageOop.rawBits() != 0
                        && pageOop != nilObject_) {
                        ObjectHeader* page = pageOop.asObjectPtr();
                        auto pb = reinterpret_cast<uint8_t*>(page);
                        if (pb >= oldSpaceStart_ && pb < oldSpaceFree_
                            && slotNum < page->slotCount()) {
                            page->slotAtPut(slotNum, nilObject_);
                        }
                    }
                }
            }
        }
    }
}

void ObjectMemory::syncClassTableToHeap() {
    // The C++ classTable_ vector is the runtime source of truth, but the image
    // is saved from the in-heap class table pages inside hiddenRootsObj.
    // When registerClass() adds a new class, it only updates the C++ vector.
    // This method writes the vector back to the heap pages before save.
    // It also updates hiddenRootsObj's page pointer slots, since GC compaction
    // may have moved the page objects (classTablePages_ tracks their current
    // addresses but hiddenRoots slots may be stale).

    if (!hiddenRootsObj_.isObject()) return;
    ObjectHeader* hr = hiddenRootsObj_.asObjectPtr();

    constexpr size_t PageSize = 1024;

    // Step 1: Update hiddenRoots page pointer slots from classTablePages_
    for (size_t p = 0; p < classTablePages_.size(); ++p) {
        if (p < hr->slotCount()) {
            hr->slotAtPut(p, classTablePages_[p]);
        }
    }

    // Step 2: Write class entries from C++ vector into heap pages
    //
    // The `break` that used to guard this loop SILENTLY DROPPED every class
    // whose index fell beyond the pages the image happened to ship with.
    // classTable_ is sized 1<<22 at startup and registerClass() writes to it
    // freely, but nothing ever grew classTablePages_ -- there is no push_back
    // or resize on it anywhere except setClassTablePage(), which only the image
    // loader calls for pages that already exist.  So a session that created
    // enough classes to spill past the last loaded page saved an image whose
    // objects referenced class indices with no class table entry.
    //
    // Measured 2026-08-15 with avwohl/validate_smalltalk_image: a clean image
    // validates PASS; after an 896-class suite run the saved image had 150
    // objects across 10 indices (25778-25836, i.e. page 25) reporting
    // "references class index N which is not in the class table".  Repeat runs
    // of the same input reproduce identically (11 errors) whenever the run
    // completes.
    //
    // Grow the heap side to match rather than truncating.  A save that cannot
    // represent the live class table must say so, not quietly emit a corrupt
    // image -- silently dropping state is exactly what CLAUDE.md prohibits.
    size_t highestUsed = 0;
    for (size_t i = 1; i < classTable_.size(); ++i)
        if (classTable_[i].isObject() && classTable_[i] != nilObject_) highestUsed = i;
    const size_t pagesNeeded = (highestUsed / PageSize) + 1;
    if (pagesNeeded > classTablePages_.size()) {
        const uint32_t arrayClassIdx = classTablePages_.empty()
            ? 0u : classTablePages_[0].asObjectPtr()->classIndex();
        for (size_t pg = classTablePages_.size(); pg < pagesNeeded; ++pg) {
            Oop newPage = arrayClassIdx
                ? allocateSlots(arrayClassIdx, PageSize, ObjectFormat::Indexable)
                : Oop::nil();
            if (!newPage.isObject() || newPage == nilObject_) {
                fprintf(stderr,
                        "[CLASSTABLE] FATAL: cannot allocate class-table page %zu "
                        "(need %zu pages for highest class index %zu); saving now "
                        "would drop those classes and corrupt the image\n",
                        pg, pagesNeeded, highestUsed);
                fflush(stderr);
                break;
            }
            for (size_t sl = 0; sl < PageSize; ++sl)
                newPage.asObjectPtr()->slotAtPut(sl, nilObject_);
            setClassTablePage(pg, newPage);
            if (pg < hr->slotCount()) {
                hr->slotAtPut(pg, newPage);
            } else {
                fprintf(stderr,
                        "[CLASSTABLE] hiddenRoots has only %zu slots but page %zu "
                        "is needed; the page is live but will not be reachable "
                        "from the saved image\n", hr->slotCount(), pg);
                fflush(stderr);
            }
        }
    }

    for (size_t i = 1; i < classTable_.size(); ++i) {
        size_t pageNum = i / PageSize;
        size_t slotNum = i % PageSize;

        if (pageNum >= classTablePages_.size()) {
            if (classTable_[i].isObject() && classTable_[i] != nilObject_) {
                static int dropLog = 0;
                if (dropLog++ < 5)
                    fprintf(stderr,
                            "[CLASSTABLE] dropping class at index %zu: page %zu "
                            "does not exist (have %zu)\n",
                            i, pageNum, classTablePages_.size());
            }
            break;
        }
        Oop pageOop = classTablePages_[pageNum];
        if (!pageOop.isObject() || pageOop.rawBits() == 0 || pageOop == nilObject_) continue;

        ObjectHeader* page = pageOop.asObjectPtr();
        auto p = reinterpret_cast<uint8_t*>(page);
        if (p < oldSpaceStart_ || p >= oldSpaceFree_) continue;

        if (slotNum < page->slotCount()) {
            page->slotAtPut(slotNum, classTable_[i]);
        }
    }
}

size_t ObjectMemory::markPhase(bool skipEphemerons) {
    heapWatchLogged_ = 0;
    recordMarkParents_ = GET_DEBUG_BOOL(PHARO_WEAK_SURVIVOR_PATHS);
    if (recordMarkParents_) { markParent_.clear(); markRootTag_.clear(); }
    // pointerSlotsOf bounds a Context's trace at its stackp and needs the
    // cached class index; cacheGCClassIndices runs at image load, but a
    // become: on Context class would leave it stale/zero.
    if (contextClassIndex_ == 0) cacheGCClassIndices();
    weakPathFilter_ = GET_DEBUG_STR(PHARO_WATCH_HEAP_CLASS);
    logWeakSurvivorClasses_ = GET_DEBUG_BOOL(PHARO_WEAK_SURVIVOR_CLASSES);
    // Reserve space for mark stack to avoid frequent reallocations
    markStack_.clear();
    markStack_.reserve(100000);
    weakList_.clear();
    ephemeronList_.clear();
    ephemeronEncounterCount_ = 0;
    ephemeronInactiveCount_ = 0;
    ephemeronActiveCount_ = 0;

    // Build valid object start set for interior pointer detection.
    // This prevents markAndTrace from calling setMarked() on interior pointers
    // whose random data happens to have a valid classIndex, which would corrupt
    // slot values by ORing the MarkedBit (0x40000000) into them.
    validObjectStarts_.clear();
    validObjectStarts_.reserve(800000);
    size_t preEphCount = 0;
    {
        ObjectScanner buildScan(oldSpaceStart_, oldSpaceFree_);
        while (ObjectHeader* obj = buildScan.next()) {
            validObjectStarts_.insert(reinterpret_cast<uintptr_t>(obj));
            if (obj->format() == ObjectFormat::WeakWithFixed) preEphCount++;
        }
    }
    if (g_debug.gcEphDebug) {
        fprintf(stderr, "[GC-PRE] heap-scan format-5 count=%zu\n", preEphCount);
    }

    size_t markedCount = 0;

    // 1. Mark from memory roots (special objects — NOT class table entries).
    // In Spur, class table entries are NOT strong roots. Anonymous/transient
    // classes can be collected when no live object references them.
    forEachMemoryRoot([this](Oop& oop) {
        markAndTrace(oop);
    }, /* includeClassTable */ false);

    // 2. Mark from interpreter roots.
    // True fullGC/sweep (ephemerons processed) marks StrongOnly: pure VM
    // caches (method cache, JIT headers/ICs, count-map keys) are WEAK
    // roots so they can't pin dead classes/methods for an extra cycle
    // (ObsoleteTest one-cycle pin).  Dead cache entries are voided by
    // purgeDeadCacheRoots() below, after the mark fixpoint.  The
    // scavenge-emulating skipEphemerons path keeps every root strong —
    // young objects referenced only by caches must tenure, not dangle.
    if (interpreter_) {
        interpreter_->forEachRoot([this](Oop& oop) {
            if (__builtin_expect(recordMarkParents_, 0)
                    && oop.isObject() && oop.rawBits() > 0x10000)
                markRootTag_.emplace(oop.rawBits(), interpreter_->rootTag_);
            markAndTrace(oop);
        }, skipEphemerons ? Interpreter::RootScope::All
                          : Interpreter::RootScope::StrongOnly);
    }

    // 2b. Mark in-heap class table pages (hiddenRoots + page Arrays).
    // These are format 9 objects not traced by scanPointerFields, so without
    // this they'd be treated as dead and destroyed by compaction.
    markClassTablePages();

    // 3. Drain mark stack
    processMarkStack();

    // 4. Ephemeron fixed-point iteration
    // Some ephemerons' keys may have become reachable through other marking.
    // Iterate until no more ephemerons become inactive, then fire the rest.
    // Skip during auto-compact GC to emulate scavenge behavior — a real
    // generational GC scavenge wouldn't fire old-space ephemerons.
    if (!skipEphemerons) {
        size_t fired = 0;
        // Fixed-point outer loop: each fireAllEphemerons pass may mark fields
        // that reach new ephemerons (appended to ephemeronList_). Those must
        // be retested against markInactive, and any with dead keys fired too.
        while (!ephemeronList_.empty()) {
            while (markInactiveEphemerons()) {}
            if (ephemeronList_.empty()) break;
            size_t thisRound = ephemeronList_.size();
            fireAllEphemerons();
            fired += thisRound;
        }
        if (g_debug.gcEphDebug) {
            fprintf(stderr, "[GC-EPH] encountered=%zu inactive=%zu active=%zu fired=%zu weakList=%zu\n",
                    ephemeronEncounterCount_, ephemeronInactiveCount_,
                    ephemeronActiveCount_, fired, weakList_.size());
        }

        // 5. Process weak objects (nil dead references, queue mourners)
        processWeaklings();
    } else {
        // Still need to mark ephemeron contents so they survive compaction,
        // but don't fire them or process weak nilling.
        for (ObjectHeader* eph : ephemeronList_) {
            // Mark all slots (key + values) to keep them alive
            for (size_t i = 0; i < eph->slotCount(); i++) {
                Oop slot = Oop::fromRawBits(reinterpret_cast<uintptr_t*>(eph + 1)[i]);
                if (slot.isObject() && !slot.isNil()) {
                    markAndTrace(slot);
                }
            }
        }
        processMarkStack();
        // Don't nil weak refs — those objects are alive, just uncollectable this cycle
        // Mark weak object contents to keep them alive
        for (ObjectHeader* weak : weakList_) {
            for (size_t i = 0; i < weak->slotCount(); i++) {
                Oop slot = Oop::fromRawBits(reinterpret_cast<uintptr_t*>(weak + 1)[i]);
                if (slot.isObject() && !slot.isNil()) {
                    markAndTrace(slot);
                }
            }
        }
        processMarkStack();
    }

    // 5a. Mark fixpoint reached and markParent_ is complete — report the full
    // retention chain for every watched-class instance that survived.
    reportWatchedChains();

    // 5b. Sweep the class table: nil entries for classes that were not marked.
    // This allows anonymous/transient classes to be collected.
    sweepClassTable();

    // 5c. Mark bits are final — void every VM cache slot whose target
    // died this cycle.  MANDATORY whenever roots were marked StrongOnly
    // (step 2): compaction/sweep is about to reclaim those objects, and
    // the pointer-update pass (or a later dispatch through a stale cache
    // entry after sweepGC) would otherwise walk dead oops.
    if (interpreter_ && !skipEphemerons) {
        interpreter_->purgeDeadCacheRoots();
    }

    // 6. Count marked objects
    ObjectScanner scanner(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = scanner.next()) {
        if (obj->isMarked()) markedCount++;
    }

    return markedCount;
}

// ===== COMPACT PHASE =====

void ObjectMemory::planCompactSavingForwarders(CompactPass& pass) {
    // Use the whole new space as scratch space for saved first
    // fields during compacting GC.  Any live young-space objects
    // have already been tenured by a pre-compact scavenge (or
    // start empty in pure mark-sweep-compact mode), so eden is
    // safe to reuse here.  Compacting GC and scavenge are never
    // concurrent.
    //
    // Reset per pass: each pass plans from scratch into the same area, and
    // copyAndUnmark consumes exactly what this pass saved.
    savedFirstFieldsSpace_.start = reinterpret_cast<Oop*>(newSpaceStart_);
    savedFirstFieldsSpace_.limit = reinterpret_cast<Oop*>(newSpaceEnd_);
    savedFirstFieldsSpace_.top = savedFirstFieldsSpace_.start;

    uint8_t* toFinger = pass.dstStart;  // Destination for next live object
    planSeen_ = planMarked_ = planUnmarked_ = planPinned_ = 0;
    planInPlace_ = planToMove_ = planMarkedBytes_ = 0;

    // Assume the rest of the heap fits; the scratch-overflow branch below
    // overwrites these when it does not.
    pass.srcEnd = oldSpaceFree_;
    pass.complete = true;

    ObjectScanner scanner(pass.srcStart, oldSpaceFree_);
    while (ObjectHeader* obj = scanner.next()) {
        planSeen_++;
        if (!obj->isMarked()) {
            planUnmarked_++;
            continue;  // Dead — skip
        }
        planMarked_++;

        size_t objSize = obj->totalSize();
        planMarkedBytes_ += objSize;
        uint8_t* objAddr = reinterpret_cast<uint8_t*>(obj);
        bool isOverflow = obj->hasOverflowSlots();
        // Object start in memory (includes overflow word if present)
        uint8_t* objStart = isOverflow ? (objAddr - 8) : objAddr;
        // Where the header will be at the destination
        uint8_t* destHeaderPos = isOverflow ? (toFinger + 8) : toFinger;

        // Pinned objects don't move
        if (obj->isPinned()) {
            planPinned_++;
            if (toFinger < objStart) {
                // Gap before pinned object — skip over it
                toFinger = objStart;
            }
            obj->setGrey(false);  // Ensure no stale grey — critical for savedFieldPtr sync
            toFinger += objSize;
            continue;
        }

        // Does this object actually need to move?
        if (destHeaderPos == objAddr) {
            planInPlace_++;
            // Already in place — no forwarding needed. Clear grey bit.
            obj->setGrey(false);
            toFinger += objSize;
            continue;
        }

        // Mobile object that needs to move: save first field, store forwarding address.
        // Every Spur object has at least 16 bytes (8-byte header + 8 bytes padding/data),
        // so we can always use the first word after the header for forwarding.
        {
            // Check if we have scratch space.  If not, this object and
            // everything after it belong to the NEXT pass: stop here, so the
            // update and copy phases see exactly the prefix that was planned.
            //
            // History (this is the third shape of this code).  Originally the
            // overflow returned false and the CALLER IGNORED IT — updatePointers
            // and copyAndUnmark then ran against a half-planned heap and every
            // inbound reference to an unplanned mobile object dangled after the
            // memmove (silent-cap audit 2026-07-03, wholesale corruption at
            // >~2-4M movers).  The interim fix rolled the partial plan back and
            // let fullGC skip compaction for the cycle: correct, but a live set
            // that overflows the scratch once overflows it every cycle, so old
            // space stayed fragmented for the life of the process.  Cog does a
            // multi-pass compact; fullGC now does too, so neither the corruption
            // nor the give-up is needed.
            if (savedFirstFieldsSpace_.top >= savedFirstFieldsSpace_.limit) {
                pass.srcEnd = objStart;
                pass.dstEnd = toFinger;
                pass.complete = false;
                return;
            }
            // Save first field (word right after header, always exists in Spur)
            Oop* firstField = reinterpret_cast<Oop*>(obj + 1);
            *savedFirstFieldsSpace_.top = *firstField;
            savedFirstFieldsSpace_.top++;

            // Store forwarding address in first field.
            // For overflow objects, forwarding points past the overflow word
            // to where the header will be at the destination (per Spur spec).
            ObjectHeader* dest = reinterpret_cast<ObjectHeader*>(destHeaderPos);
            *firstField = Oop::fromObject(dest);

            // Mark this object as having a forwarding address (grey bit)
            obj->setGrey(true);
            planToMove_++;
        }

        toFinger += objSize;
    }

    // Plan summary logged by fullGC caller

    if (GET_DEBUG_BOOL(PHARO_GC_LOG)) {
        fprintf(stderr, "[GC-PLAN] seen=%zu marked=%zu unmarked=%zu pinned=%zu "
                "inplace=%zu tomove=%zu markedBytes=%zu toFinger=+%zu KB "
                "src=[+%zu,+%zu] KB\n",
                planSeen_, planMarked_, planUnmarked_, planPinned_,
                planInPlace_, planToMove_, planMarkedBytes_,
                (size_t)(toFinger - oldSpaceStart_) / 1024,
                (size_t)(pass.srcStart - oldSpaceStart_) / 1024,
                (size_t)(pass.srcEnd - oldSpaceStart_) / 1024);
    }
    // Everything from pass.srcStart on was planned.  pass.srcEnd and
    // pass.complete were set to say so before the walk started.
    pass.dstEnd = toFinger;
}

void ObjectMemory::updatePointersAfterCompact() {
    // Helper to resolve a forwarding address: if the referenced object has the
    // grey bit set (meaning it's mobile and has a forwarding address in slot 0),
    // return the forwarding Oop. Otherwise return the original Oop unchanged.
    auto resolveForward = [this](Oop ref) -> Oop {
        if (!ref.isObject()) return ref;
        ObjectHeader* refObj = ref.asObjectPtr();
        if (isPermObject(refObj)) return ref;
        if (!isOldObject(refObj)) return ref;  // Outside heap — leave as is
        // Validate this points to a real object header, not an interior pointer.
        // Interior pointers have random data; reading mark/grey bits from random
        // data could cause us to read a "forwarding address" that doesn't exist.
        auto p = reinterpret_cast<uint8_t*>(refObj);
        if (p >= oldSpaceFree_) return ref;  // Beyond used heap
        uint32_t cIdx = refObj->classIndex();
        if (cIdx == 0 || cIdx >= classTable_.size() ||
            !classTable_[cIdx].isObject()) {
            return ref;  // Not a valid object — leave pointer unchanged
        }
        if (!refObj->isMarked()) return ref;  // Dead — leave as is
        if (refObj->isGrey()) {
            // Grey = has forwarding address in first field (word after header)
            Oop* firstField = reinterpret_cast<Oop*>(refObj + 1);
            return *firstField;
        }
        // Not grey = didn't move, pointer is already correct
        return ref;
    };

    // Update pointers in all marked objects, maintaining a parallel pointer
    // into savedFirstFieldsSpace for grey (mobile) objects.  This is critical
    // because grey compiled methods have their slot 0 (method header) overwritten
    // with a forwarding address; we must read the real header from the saved
    // copy to know how many literal slots to scan.
    {
        Oop* savedFieldPtr = savedFirstFieldsSpace_.start;
        ObjectScanner scanner(oldSpaceStart_, oldSpaceFree_);
        while (ObjectHeader* obj = scanner.next()) {
            if (!obj->isMarked()) continue;

            Oop* slots = obj->slots();

            if (obj->isGrey()) {
                // Mobile object: slot 0 has been overwritten with forwarding addr.
                // The real slot 0 is in savedFirstFieldsSpace.

                // Compute numPointers using saved first field for compiled methods
                size_t numPointers;
                if (obj->isCompiledMethod()) {
                    size_t totalSlots = obj->slotCount();
                    if (totalSlots == 0) {
                        numPointers = 0;
                    } else {
                        Oop savedField = (savedFieldPtr < savedFirstFieldsSpace_.top)
                            ? *savedFieldPtr : Oop::fromSmallInteger(0);
                        if (savedField.isSmallInteger()) {
                            size_t numLiterals = savedField.asSmallInteger() & 0x7FFF;
                            numPointers = std::min(numLiterals + 1, totalSlots);
                        } else {
                            numPointers = 1;
                        }
                    }
                } else {
                    numPointers = pointerSlotsOf(obj);
                }

                // Update slots 1..numPointers (skip slot 0 which is forwarding addr)
                for (size_t i = 1; i < numPointers; ++i) {
                    slots[i] = resolveForward(slots[i]);
                }

                // Update the saved first field itself (it may point to a mobile object)
                if (savedFieldPtr < savedFirstFieldsSpace_.top) {
                    if (numPointers > 0) {
                        *savedFieldPtr = resolveForward(*savedFieldPtr);
                    }
                    savedFieldPtr++;
                }
            } else {
                // Non-mobile (pinned or in-place): slot 0 is valid, scan all slots
                size_t numPointers = pointerSlotsOf(obj);
                for (size_t i = 0; i < numPointers; ++i) {
                    slots[i] = resolveForward(slots[i]);
                }
            }
        }
    }

    // Also update pointers in permanent space objects that reference old space
    {
        ObjectScanner permScanner(permSpaceStart_, permSpaceEnd_);
        while (ObjectHeader* obj = permScanner.next()) {
            size_t numPointers = pointerSlotsOf(obj);
            Oop* slots = obj->slots();
            for (size_t i = 0; i < numPointers; ++i) {
                slots[i] = resolveForward(slots[i]);
            }
        }
    }

    // Update pointers in NEW SPACE objects that reference old space.
    // Without this, young objects holding old space pointers become stale
    // after compaction moves old space objects.
    {
        int newSpaceUpdated = 0;
        auto scanNewSpaceRegion = [&](uint8_t* start, uint8_t* end, const char* label) {
            int updatedCount = 0;
            uint8_t* scan = start;
            while (scan + 8 <= end) {  // Need at least 8 bytes for header
                ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(scan);
                size_t ts = obj->totalSize();
                if (ts == 0 || ts > 0x10000000) {
                    // Implausible header terminates the region walk — any
                    // young objects BEYOND this point keep stale old-space
                    // pointers after compaction.  Loud, never silent
                    // (silent-cap audit residue, closed 2026-07-04).
                    static int nsTermLog = 0;
                    if (nsTermLog++ < 10) {
                        fprintf(stderr, "[NS-SCAN-TERM] %s: implausible "
                                "totalSize=%zu at %p (%zd bytes before end) — "
                                "region walk stopped\n",
                                label, ts, (void*)scan, (ssize_t)(end - scan));
                    }
                    break;
                }
                if (obj->classIndex() != 0) {  // Not free
                    size_t numPointers = pointerSlotsOf(obj);
                    Oop* slots = obj->slots();
                    for (size_t i = 0; i < numPointers; ++i) {
                        Oop old = slots[i];
                        slots[i] = resolveForward(slots[i]);
                        if (slots[i].rawBits() != old.rawBits()) updatedCount++;
                    }
                }
                scan += ts;
            }
            (void)label;  // suppress unused warning
            newSpaceUpdated += updatedCount;
        };
        // Scan eden (edenStart_ to edenFree_) — edenFree_ is the live
        // watermark, so this walks only objects that were actually allocated.
        scanNewSpaceRegion(edenAllocBase_, edenFree_, "eden");
        // NO survivor scan.  `[survivorStart_, newSpaceEnd_)` has no
        // allocations at all: this VM's scavenge TENURES every reachable
        // young object straight to old space (see ObjectMemory::scavenge)
        // and resets eden — nothing is ever copied into survivor space, and
        // no code writes there (grep survivorStart_: it is only ever read as
        // the eden limit).  Walking it to `newSpaceEnd_` therefore parsed
        // never-initialised pages as object headers, relying on the
        // implausible-totalSize guard to stop, and printed
        // "[NS-SCAN-TERM] survivor ... region walk stopped" — a warning about
        // young objects losing old-space updates when there are no young
        // objects there to lose them.  If a copying survivor is ever
        // implemented, restore the scan with that collector's live
        // watermark as the end bound, never newSpaceEnd_.
        (void)newSpaceUpdated;  // suppress unused warning
    }

    // Update memory roots
    auto updateOop = [&resolveForward](Oop& oop) {
        oop = resolveForward(oop);
    };

    forEachMemoryRoot(updateOop);

    if (interpreter_) {
        interpreter_->forEachRoot(updateOop);
        // jit-may22b Step 1: Sista cache GC integration.
        // While forwarders are still installed (we're between the plan
        // phase and the move phase), rekey Sista's method→fn cache so
        // hints-bearing compiles survive compaction.  After this point
        // the old oop bits become invalid; doing the rekey AFTER
        // compact (in recoverSistaAfterGC) is too late.
        //
        // OPT-IN ONLY via PHARO_SISTA_REKEY_AFTER_GC=1.  Empirical
        // testing showed PHARO_SISTA_COMPILE_BAIL_ONLY=1 + rekey
        // breaks with "Message not understood: ByteString >>
        // #encodeString:" — investigation deferred.  The default
        // path falls back to reset() in recoverSistaAfterGC below.
        // The Sista rekey machinery is JIT-only; compiled out when JIT is off.
#if PHARO_JIT_ENABLED
        if (GET_DEBUG_BOOL(PHARO_SISTA_REKEY_AFTER_GC)) {
            interpreter_->rekeySistaCacheViaForwarders(
                [&resolveForward](uint64_t oldBits) -> uint64_t {
                    Oop o = Oop::fromRawBits(oldBits);
                    if (!o.isObject() || o.rawBits() <= 0x10000) return 0;
                    Oop n = resolveForward(o);
                    if (!n.isObject() || n.rawBits() <= 0x10000) return 0;
                    return n.rawBits();
                });
        }
#endif  // PHARO_JIT_ENABLED — Sista cache rekey (compact)
    }

    // Note: hiddenRootsObj page pointer slots are NOT updated here.
    // They are format-9 slots (not traced by pointerSlotsOf), and at this point
    // hiddenRootsObj_ may point to its destination address (data not yet moved).
    // Instead, classTablePages_ (in forEachMemoryRoot) tracks page Oops and
    // syncClassTableToHeap writes them back to hiddenRoots before save.
}

size_t ObjectMemory::copyAndUnmark(const CompactPass& pass, bool clearMarks) {
    Oop* savedFieldPtr = savedFirstFieldsSpace_.start;
    uint8_t* toFinger = pass.dstStart;
    size_t movedCount = 0;

    // Exactly the prefix planCompactSavingForwarders planned — no further, or
    // objects with no forwarding address would be slid anyway.  That was the
    // original defect.
    ObjectScanner scanner(pass.srcStart, pass.srcEnd);
    while (ObjectHeader* obj = scanner.next()) {
        if (!obj->isMarked()) continue;

        size_t objSize = obj->totalSize();
        uint8_t* objAddr = reinterpret_cast<uint8_t*>(obj);
        bool isOverflow = obj->hasOverflowSlots();
        // Object start in memory (includes overflow word if present)
        uint8_t* objStart = isOverflow ? (objAddr - 8) : objAddr;
        // Where the header will be at the destination
        uint8_t* destHeaderPos = isOverflow ? (toFinger + 8) : toFinger;

        if (obj->isPinned()) {
            // Pinned: don't move, just clear mark and grey
            // Zero the gap before pinned object so scanner skips it
            if (toFinger < objStart) {
                std::memset(toFinger, 0, objStart - toFinger);
                toFinger = objStart;
            }
            if (clearMarks) obj->setMarked(false);
            obj->setGrey(false);
            toFinger += objSize;
            continue;
        }

        // Restore first field from saved space (only for grey objects = actually moved)
        if (obj->isGrey() && savedFieldPtr < savedFirstFieldsSpace_.top) {
            // Write directly to the first word after header (works for zero-slot objects too)
            Oop* firstField = reinterpret_cast<Oop*>(obj + 1);
            *firstField = *savedFieldPtr;
            savedFieldPtr++;
        }

        // Slide object to destination.
        // For overflow objects, copy from the overflow word (before header).
        // memmove handles overlapping regions correctly.
        if (toFinger != objStart) {
            std::memmove(toFinger, objStart, objSize);
            movedCount++;
        }

        // Clear mark and grey on the (possibly moved) copy
        ObjectHeader* movedObj = reinterpret_cast<ObjectHeader*>(destHeaderPos);
        if (clearMarks) movedObj->setMarked(false);
        movedObj->setGrey(false);

        toFinger += objSize;
    }
    // gcCopyGeneration was already incremented at function start

    // Parallel-walk integrity: every saved first field planted by
    // planCompactSavingForwarders must be consumed here, in order.  A
    // mismatch means some object's slot 0 was restored from the WRONG
    // saved entry (and every later restore is off by the same amount) —
    // silent wholesale slot-0 corruption.
    if (savedFieldPtr != savedFirstFieldsSpace_.top) {
        fprintf(stderr,
            "[GC-COMPACT-DESYNC] copyAndUnmark consumed %td saved fields but "
            "planCompact saved %td — slot-0 restores are misaligned!\n",
            (ptrdiff_t)(savedFieldPtr - savedFirstFieldsSpace_.start),
            (ptrdiff_t)(savedFirstFieldsSpace_.top - savedFirstFieldsSpace_.start));
    }

    // Plan/copy agreement.  planCompactSavingForwarders and copyAndUnmark must
    // walk the same objects in the same order and finish on the same finger.
    // If they disagree, the saved first fields are misaligned and the heap is
    // already destroyed — there is nothing to recover to.  This covers a
    // DIFFERENT failure class from the [GC-COMPACT-DESYNC] check above: that
    // one catches the wrong NUMBER of saved fields being consumed, this one
    // catches the same number consumed over a different set of objects.
    //
    // main writes this as assert().  This VM is measured in Release builds
    // (CLAUDE.md), where assert() compiles to nothing, so report and die
    // instead — same shape as the old-space-exhaustion FATAL above.
    if (toFinger != pass.dstEnd) {
        fprintf(stderr,
            "[GC-COMPACT-MISMATCH] copyAndUnmark finished at %p, plan said %p "
            "(srcStart=%p srcEnd=%p dstStart=%p complete=%d) — plan and copy "
            "disagree; the heap is unrecoverable.\n",
            (void*)toFinger, (void*)pass.dstEnd, (void*)pass.srcStart,
            (void*)pass.srcEnd, (void*)pass.dstStart, (int)pass.complete);
        fflush(stderr);
        std::abort();
    }

    // A partial pass opens a gap between the compacted prefix and the first
    // object of the next pass.  memmove leaves stale (still marked) copies
    // behind there; ObjectScanner skips zero words, so zeroing the gap is what
    // keeps the heap walkable between passes.
    if (!pass.complete && toFinger < pass.srcEnd) {
        std::memset(toFinger, 0, pass.srcEnd - toFinger);
    }

    // oldSpaceFree_ is deliberately NOT updated here.  It bounds the plan's
    // scan and resolveForward's "beyond the used heap" test, so it must keep
    // its pre-compaction value for the whole multi-pass loop.  fullGC lowers it
    // once, after the final pass.
    return movedCount;
}

// Low-level tripwire for bug-14 diagnosis.  Declared in ObjectHeader.hpp,
// defined here so the header stays free of <cstdio>.  Fires for every
// slotAtPut(0, nil) on a context-shaped object, including fast-path writes
// that bypass ObjectMemory::storePointer.  Enabled by PHARO_SLOT_TRIPWIRE=1.
bool ObjectHeader::slot_tripwire_enabled() {
    return GET_DEBUG_BOOL(PHARO_SLOT_TRIPWIRE);
}

void ObjectHeader::slot_tripwire_fire(const ObjectHeader* hdr, Oop oldSender, Oop method) {
    fprintf(stderr, "[SLOT-TRIPWIRE] ctx=0x%llx oldSender=0x%llx method=0x%llx slots=%zu\n",
            (unsigned long long)reinterpret_cast<uintptr_t>(hdr),
            (unsigned long long)oldSender.rawBits(),
            (unsigned long long)method.rawBits(),
            (size_t)hdr->slotCount());
}

bool ObjectHeader::slot_run_tripwire_enabled() {
    return GET_DEBUG_BOOL(PHARO_SLOT_RUN_TRIPWIRE);
}

void ObjectHeader::slot_run_tripwire_fire(const ObjectHeader* hdr, size_t index, Oop value) {
    extern uint64_t g_scavengeCount;
    if (value.isNil()) return;         // nil runs are legit (OC removals)
    // Dedupe consecutive fills of the same (array, value) — a legit
    // atAllPut:-style fill fires once, not once per slot.
    static uintptr_t lastArr = 0;
    static uint64_t lastVal = 0;
    if (reinterpret_cast<uintptr_t>(hdr) == lastArr &&
        value.rawBits() == lastVal) return;
    lastArr = reinterpret_cast<uintptr_t>(hdr);
    lastVal = value.rawBits();
    static int n = 0;
    if (++n > 60) return;
    fprintf(stderr, "[SLOT-RUN-TRIPWIRE] arr=0x%llx idx=%zu val=%llx scav=%llu\n",
            (unsigned long long)reinterpret_cast<uintptr_t>(hdr), index,
            (unsigned long long)value.rawBits(),
            (unsigned long long)g_scavengeCount);
    if (g_scavengeCount >= 12) {
        void dumpCxxBacktrace(const char* tag);
        dumpCxxBacktrace("SLOT-RUN");
    }
}

void ObjectMemory::rebuildFreeListAfterCompact() {
    clearFreeLists();

    // PHARO_OLDSPACE_FREELIST: put the post-compaction GAPS on the free list
    // so allocation can reuse them.
    //
    // Sliding compaction cannot move a pinned object, so `toFinger` jumps to
    // it and abandons everything below.  Measured 2026-08-22: a NeoJSON
    // package image ends up with 235 MB of old space holding 89 MB of
    // objects, and the XMLParser one with 1.15 GB holding 94 MB -- 12x --
    // because six 32-byte pinned objects sit between +166 MB and +208 MB and
    // nothing below them is ever reused.  Full analysis in
    // docs/gc-oldspace-fragmentation-2026-08-22.md.
    //
    // Opt-in while it is new: this is the first code path that hands the
    // old-space allocator a chunk it did not bump-allocate, and a mistake
    // here corrupts the heap rather than failing a test.  allocateRaw's Old
    // case reads the same knob.
    if (GET_DEBUG_BOOL(PHARO_OLDSPACE_FREELIST)) {
        size_t gaps = 0, gapBytes = 0;
        uint8_t* prevEnd = oldSpaceStart_;
        ObjectScanner sc(oldSpaceStart_, oldSpaceFree_);
        while (ObjectHeader* o = sc.next()) {
            uint8_t* objStart = reinterpret_cast<uint8_t*>(o);
            if (o->hasOverflowSlots()) objStart -= 8;
            if (objStart > prevEnd) {
                size_t gap = static_cast<size_t>(objStart - prevEnd);
                if (gap >= 16) {
                    // zeroBody=false: the gap is already zeros (that is why
                    // ObjectScanner walked over it) or an older free chunk;
                    // memsetting 148 MB on every fullGC buys nothing.
                    // NOTE 2026-08-23: these gaps were verified to be
                    // genuinely all-zero.  A temporary check that scanned each
                    // gap for non-zero words before creating the chunk fired
                    // ZERO times across a full run, so rebuild is NOT writing
                    // free-chunk headers over live memory -- one more theory
                    // for the PHARO_OLDSPACE_FREELIST wedge, refuted.  The
                    // check was removed because it costs an O(148 MB) scan per
                    // fullGC.
                    if (ObjectHeader* c = makeFreeChunk(prevEnd, gap, false)) {
                        addToFreeList(c, gap);
                        gaps++; gapBytes += gap;
                        if (GET_DEBUG_BOOL(PHARO_GC_LOG)) {
                            fprintf(stderr, "[FREELIST-GAP] %p .. %p (%zu)\n",
                                    (void*)prevEnd, (void*)(prevEnd + gap), gap);
                        }
                    }
                }
            }
            // Advance from the object's BASE, not its header.  For an
            // overflow object `o` is the header at base+8 while totalSize()
            // already counts the overflow word, so using `o` here overshot the
            // true end by 8 -- while objStart above DOES apply the correction.
            // The same base-vs-header inconsistency produced two of the bugs
            // already fixed in this path.
            prevEnd = objStart + o->totalSize();
        }
        // Validate what we just built.  This distinguishes the two possible
        // cycle sources: present HERE means rebuild itself linked a chunk into
        // its own list twice (or added overlapping chunks); absent here but
        // reported later by allocateFromFreeList's Floyd guard means the cycle
        // is created by the split/unlink path during allocation.
        {
            ObjectHeader* fast = freeLists_[0];
            ObjectHeader* slow2 = fast;
            bool adv = false, cyclic = false;
            size_t len = 0;
            while (fast) {
                Oop nx = fast->slotCount() > 0 ? fast->slotAt(0) : Oop::nil();
                fast = freeListLinkToChunk(nx);
                len++;
                if (adv && slow2) {
                    Oop sn = slow2->slotCount() > 0 ? slow2->slotAt(0) : Oop::nil();
                    slow2 = freeListLinkToChunk(sn);
                }
                adv = !adv;
                if (fast && fast == slow2) { cyclic = true; break; }
            }
            if (cyclic) {
                fprintf(stderr, "[FREELIST-BUILD] list is ALREADY cyclic right "
                        "after rebuild (%zu links walked, %zu gaps added)\n",
                        len, gaps);
                // Dump the first nodes with their links so the aliasing is
                // visible rather than guessed at.
                ObjectHeader* n = freeLists_[0];
                for (size_t i = 0; i < 12 && n; i++) {
                    Oop nx = n->slotCount() > 0 ? n->slotAt(0) : Oop::nil();
                    fprintf(stderr,
                            "[FREELIST-NODE] %2zu chunk=%p slots=%u total=%zu "
                            "overflow=%d -> %p\n",
                            i, (void*)n, (unsigned)n->slotCount(),
                            (size_t)n->totalSize(), (int)n->hasOverflowSlots(),
                            nx.isObject() ? (void*)nx.asObjectPtr() : nullptr);
                    n = freeListLinkToChunk(nx);
                }
                fflush(stderr);
            } else if (GET_DEBUG_BOOL(PHARO_GC_LOG)) {
                fprintf(stderr, "[FREELIST-BUILD] acyclic, %zu links, %zu gaps\n",
                        len, gaps);
                fflush(stderr);
            }
        }
        if (gaps && GET_DEBUG_BOOL(PHARO_GC_LOG)) {
            fprintf(stderr, "[GC-FREELIST] %zu gaps, %zu MB reclaimed for reuse "
                    "(used=%zu MB)\n", gaps, gapBytes / (1024 * 1024),
                    (size_t)(oldSpaceFree_ - oldSpaceStart_) / (1024 * 1024));
        }
    }

    // The gap between oldSpaceFree_ and oldSpaceEnd_ is one big free chunk.
    // No scanner walks past oldSpaceFree_, so the gap doesn't need to be
    // zeroed.  Asking the kernel to drop physical pages keeps RSS bounded
    // by live-heap size instead of the 4 GB mmap reservation, and avoids
    // multi-second memset-the-whole-tail cost on every fullGC.
    size_t freeBytes = oldSpaceEnd_ - oldSpaceFree_;
    if (oldSpaceUseMmap_ && freeBytes >= 4096) {
        constexpr uintptr_t kPageSize = 4096;
        uintptr_t pageStart = (reinterpret_cast<uintptr_t>(oldSpaceFree_)
                               + (kPageSize - 1)) & ~(kPageSize - 1);
        uintptr_t pageEnd = reinterpret_cast<uintptr_t>(oldSpaceEnd_)
                            & ~(kPageSize - 1);
        if (pageEnd > pageStart) {
            ::madvise(reinterpret_cast<void*>(pageStart),
                      pageEnd - pageStart, MADV_DONTNEED);
        }
    }
    // We don't need to create a free list entry for the trailing gap —
    // the bump pointer allocator already handles this via oldSpaceFree_.
    // Free lists will be populated when we switch to free-list-based allocation.
}


// Class-name histogram of the whole heap, largest byte-footprint first.
// Called on the old-space-exhaustion FATAL path and per-fullGC under
// PHARO_HEAP_CENSUS=1 — turns "live heap exploded" into "3.4 GB of X".
void ObjectMemory::dumpHeapCensus(int topN) {
    struct Bucket { size_t count = 0; size_t bytes = 0; };
    std::unordered_map<uint32_t, Bucket> byClass;
    size_t totalBytes = 0, totalObjs = 0;
    allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;
        ObjectHeader* h = obj.asObjectPtr();
        size_t sz = h->slotCount() * 8 + sizeof(ObjectHeader);
        Bucket& b = byClass[h->classIndex()];
        b.count++;
        b.bytes += sz;
        totalBytes += sz;
        totalObjs++;
    });
    std::vector<std::pair<uint32_t, Bucket>> v(byClass.begin(), byClass.end());
    std::sort(v.begin(), v.end(),
              [](auto& a, auto& b) { return a.second.bytes > b.second.bytes; });
    fprintf(stderr, "[HEAP-CENSUS] %zu objects, %zu MB total; top %d classes by bytes:\n",
            totalObjs, totalBytes / (1024 * 1024), topN);
    for (int i = 0; i < topN && i < (int)v.size(); i++) {
        uint32_t idx = v[i].first;
        std::string name = (idx < classTable_.size() && classTable_[idx].isObject())
            ? nameOfClass(classTable_[idx])
            : std::string("?");
        fprintf(stderr, "[HEAP-CENSUS]  %8zu objs %8zu KB  %s (idx %u)\n",
                v[i].second.count, v[i].second.bytes / 1024, name.c_str(), idx);
    }
    fflush(stderr);
}

} // namespace pharo
