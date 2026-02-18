/*
 * ObjectMemory.cpp - Heap Management Implementation
 */

#include "ObjectMemory.hpp"
#include "Interpreter.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <sys/mman.h>
#include <execinfo.h>

extern int g_symbolMetaclassIdx;

namespace pharo {

extern uint64_t g_stepNum;

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
        std::aligned_alloc(8, config.permSpaceSize));
    if (!permSpaceStart_) return false;
    permSpaceEnd_ = permSpaceStart_ + config.permSpaceSize;

    // Use mmap for old space to get lazy-committed pages.
    // The OS only allocates physical memory when pages are written to,
    // so reserving a large virtual range is cheap.
    oldSpaceStart_ = static_cast<uint8_t*>(
        mmap(nullptr, config.oldSpaceSize, PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
    if (oldSpaceStart_ == MAP_FAILED) {
        oldSpaceStart_ = nullptr;
        std::free(permSpaceStart_);
        permSpaceStart_ = nullptr;
        return false;
    }
    oldSpaceUseMmap_ = true;
    oldSpaceMmapSize_ = config.oldSpaceSize;
    oldSpaceEnd_ = oldSpaceStart_ + config.oldSpaceSize;
    oldSpaceFree_ = oldSpaceStart_;
    newSpaceStart_ = static_cast<uint8_t*>(
        std::aligned_alloc(8, config.newSpaceSize));
    if (!newSpaceStart_) {
        std::free(permSpaceStart_);
        std::free(oldSpaceStart_);
        permSpaceStart_ = nullptr;
        oldSpaceStart_ = nullptr;
        return false;
    }
    newSpaceEnd_ = newSpaceStart_ + config.newSpaceSize;

    fprintf(stderr, "[HEAP] permSpace: %p - %p\n", permSpaceStart_, permSpaceEnd_);
    fprintf(stderr, "[HEAP] oldSpace: %p - %p (%.1f GB)\n",
            oldSpaceStart_, oldSpaceEnd_, config.oldSpaceSize / (1024.0*1024.0*1024.0));
    fprintf(stderr, "[HEAP] newSpace: %p - %p (%zu MB)\n", newSpaceStart_, newSpaceEnd_,
            config.newSpaceSize / (1024*1024));

    // Split new space into eden and survivor
    size_t edenSize = (config.newSpaceSize * config.edenRatio) / 100;
    edenStart_ = newSpaceStart_;
    edenFree_ = edenStart_;
    survivorStart_ = newSpaceStart_ + edenSize;

    // Initialize class table
    classTable_.resize(config.classTableSize, Oop::nil());

    // Zero perm space and new space (old space is mmap'd with MAP_ANONYMOUS,
    // which provides zero-filled pages lazily — no memset needed)
    std::memset(permSpaceStart_, 0, config.permSpaceSize);
    std::memset(newSpaceStart_, 0, config.newSpaceSize);

    return true;
}

// ===== OBJECT ALLOCATION =====

Oop ObjectMemory::allocateSlots(uint32_t classIndex, size_t slotCount,
                                 ObjectFormat format) {
    // DEBUG: Catch allocations with classIndex=0
    if (classIndex == 0) {
        static int zeroClassCount = 0;
        if (++zeroClassCount <= 10) {
            std::cerr << "[ALLOC-WARNING] allocateSlots called with classIndex=0! slots="
                      << slotCount << " format=" << static_cast<int>(format) << "\n";
        }
    }

    // Calculate size: header + slots
    size_t headerSize = sizeof(ObjectHeader);
    bool hasOverflow = slotCount >= 255;
    if (hasOverflow) {
        headerSize += sizeof(uint64_t);  // Overflow word
    }

    size_t bodySize = slotCount * sizeof(Oop);
    size_t totalSize = headerSize + bodySize;

    // Align to 8 bytes
    totalSize = (totalSize + 7) & ~7ULL;

    // TEMPORARY FIX: Allocate directly in old space to bypass broken scavenge
    // The scavenge doesn't reset eden (to avoid corruption from missing pointer forwarding)
    // which causes eden to fill up. Until proper GC is implemented, allocate in old space.
    ObjectHeader* obj = allocateRaw(totalSize, Space::Old);

    // TRACE: Log Dictionary allocations (classIndex 3143)
    if (classIndex == 3143) {
        static FILE* dictAllocLog = nullptr;
        static int dictAllocLogCount = 0;
        if (!dictAllocLog) dictAllocLog = nullptr;
        if (dictAllocLog && dictAllocLogCount < 20) {
            dictAllocLogCount++;
            fprintf(dictAllocLog, "[DICT-ALLOC #%d] slots=%zu totalSize=%zu\n",
                    dictAllocLogCount, slotCount, totalSize);
            fprintf(dictAllocLog, "  edenFree_ before allocation: 0x%llx\n",
                    (unsigned long long)edenFree_);
            fflush(dictAllocLog);
        }
    }

    // TRACE: Log Context allocations
    if (classIndex == 36) {  // Context class
        static FILE* ctxAllocLog = nullptr;
        static int ctxAllocLogCount = 0;
        if (!ctxAllocLog) ctxAllocLog = nullptr;
        if (ctxAllocLog && ctxAllocLogCount < 20) {
            ctxAllocLogCount++;
            fprintf(ctxAllocLog, "[CTX-ALLOC #%d] slots=%zu totalSize=%zu\n",
                    ctxAllocLogCount, slotCount, totalSize);
            fprintf(ctxAllocLog, "  Eden allocation result: %p (edenFree_=%p, edenStart=%p, survivorStart=%p)\n",
                    (void*)obj, (void*)edenFree_, (void*)edenStart_, (void*)survivorStart_);
            fflush(ctxAllocLog);
        }
    }

    if (!obj) {
        static int allocFailCount = 0;
        allocFailCount++;
        if (allocFailCount <= 10) {
            fprintf(stderr, "[ALLOC-FAIL #%d] allocateSlots: old space OOM! classIdx=%u slots=%zu totalSize=%zu used=%zuMB\n",
                    allocFailCount, classIndex, slotCount, totalSize,
                    (size_t)(oldSpaceFree_ - oldSpaceStart_) / (1024*1024));
            fflush(stderr);
        }
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

    // Initialize slots to nil
    Oop* slots = obj->slots();
    // DEFENSIVE: If nilObject_ is not yet set (during image load), use Oop::nil() (raw 0)
    // This is okay because these objects will be overwritten during image loading
    Oop nilValue = (nilObject_.rawBits() != 0) ? nilObject_ : Oop::nil();
    static bool warnedNilNotSet = false;
    if (nilObject_.rawBits() == 0 && slotCount > 0 && !warnedNilNotSet) {
        std::cerr << "[ALLOC-WARNING] nilObject_ not set during allocation, using raw 0\n";
        warnedNilNotSet = true;
    }
    for (size_t i = 0; i < slotCount; ++i) {
        slots[i] = nilValue;
    }

    bytesAllocated_ += totalSize;
    return oopFromPointer(obj);
}

Oop ObjectMemory::allocateBytes(uint32_t classIndex, size_t byteCount) {
    // Calculate number of 64-bit slots needed
    size_t slotCount = (byteCount + 7) / 8;

    // Determine the correct format based on padding
    size_t padding = (slotCount * 8) - byteCount;
    ObjectFormat format = static_cast<ObjectFormat>(
        static_cast<int>(ObjectFormat::Indexable8) + padding);

    // Calculate total size
    size_t headerSize = sizeof(ObjectHeader);
    bool hasOverflow = slotCount >= 255;
    if (hasOverflow) {
        headerSize += sizeof(uint64_t);
    }

    size_t totalSize = headerSize + slotCount * 8;
    totalSize = (totalSize + 7) & ~7ULL;

    // TEMPORARY FIX: Allocate directly in old space (see allocateSlots comment)
    ObjectHeader* obj = allocateRaw(totalSize, Space::Old);

    if (!obj) return nilObject_;

    // Handle overflow (byte 7 must be 0xFF for scanner recognition)
    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(obj);
        *overflow = slotCount | (0xFFULL << 56);
        obj = reinterpret_cast<ObjectHeader*>(overflow + 1);
    }

    initializeHeader(obj, classIndex, slotCount, format);

    // Zero all bytes
    std::memset(obj->bytes(), 0, slotCount * 8);

    bytesAllocated_ += totalSize;
    return oopFromPointer(obj);
}

Oop ObjectMemory::createString(const std::string& str) {
    // Get ByteString class
    Oop stringClass = specialObject(SpecialObjectIndex::ClassByteString);
    if (stringClass.isNil() || !stringClass.isObject()) {
        return nilObject_;
    }

    // Get class index for ByteString instances
    // The class's identity hash IS the class index for its instances in Spur
    ObjectHeader* classHdr = stringClass.asObjectPtr();
    uint32_t classIndex = classHdr->identityHash();

    // If the class doesn't have an identity hash, fall back to classIndex (wrong but might work)
    if (classIndex == 0) {
        classIndex = classHdr->classIndex();
    }

    // Allocate the string
    Oop strObj = allocateBytes(classIndex, str.size());
    if (strObj.isNil()) {
        return nilObject_;
    }

    // Copy string content
    ObjectHeader* strHdr = strObj.asObjectPtr();
    std::memcpy(strHdr->bytes(), str.c_str(), str.size());

    return strObj;
}

Oop ObjectMemory::allocateWords(uint32_t classIndex, size_t wordCount) {
    // Each word is 64 bits = 1 slot
    size_t slotCount = wordCount;

    size_t headerSize = sizeof(ObjectHeader);
    bool hasOverflow = slotCount >= 255;
    if (hasOverflow) {
        headerSize += sizeof(uint64_t);
    }

    size_t totalSize = headerSize + slotCount * 8;
    totalSize = (totalSize + 7) & ~7ULL;

    // TEMPORARY FIX: Allocate directly in old space (see allocateSlots comment)
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

    // Round up to slot alignment for the slot count field
    size_t totalSlots = (totalBytes + 7) / 8;

    // Determine padding for the byte portion
    size_t padding = (totalSlots * 8) - totalBytes;
    // Format 24-27 = CompiledMethod (bytes with 0-3 odd bytes unused)
    // This maps to ObjectFormat enum values for CompiledMethod
    ObjectFormat format = static_cast<ObjectFormat>(24 + padding);

    size_t headerSize = sizeof(ObjectHeader);
    bool hasOverflow = totalSlots >= 255;
    if (hasOverflow) {
        headerSize += sizeof(uint64_t);
    }

    size_t totalSize = headerSize + totalSlots * 8;
    totalSize = (totalSize + 7) & ~7ULL;

    // Allocate in old space
    ObjectHeader* obj = allocateRaw(totalSize, Space::Old);
    if (!obj) return nilObject_;

    // Handle overflow (byte 7 must be 0xFF for scanner recognition)
    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(obj);
        *overflow = totalSlots | (0xFFULL << 56);
        obj = reinterpret_cast<ObjectHeader*>(overflow + 1);
    }

    initializeHeader(obj, classIndex, totalSlots, format);

    // Zero all content (slots will be nil, bytecodes will be 0)
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

    // TEMPORARY FIX: Allocate directly in old space (see allocateSlots comment)
    ObjectHeader* copy = allocateRaw(size, Space::Old);
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
    return oopFromPointer(copy);
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
        // SmallFloat64's class is at class table index 4 (Cog VM's smallFloatTag)
        // NOT ClassFloat (special object index 9) which is BoxedFloat64
        return classAtIndex(4);
    }
    if (!obj.isObject()) {
        return nilObject_;  // Return proper nil, not raw 0
    }

    // nil, true, false are valid heap objects - look up their class normally
    // (isValidPointer rejects nil, so handle it before the check)
    if (obj.isNil()) {
        // nil's class is UndefinedObject - read classIndex from nil's header
        ObjectHeader* header = obj.asObjectPtr();
        return classAtIndex(header->classIndex());
    }

    // Validate pointer before dereferencing to catch corruption
    if (!isValidPointer(obj)) {
        static int corruptCount = 0;
        corruptCount++;
        if (corruptCount <= 20) {
            uint64_t bits = obj.rawBits();
            std::cerr << "[CLASSOF] INVALID POINTER #" << corruptCount
                      << ": 0x" << std::hex << bits << std::dec;
            // Try to decode as ASCII to help identify corruption source
            char ascii[9];
            for (int i = 0; i < 8; i++) {
                uint8_t byte = (bits >> (i * 8)) & 0xFF;
                ascii[i] = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
            }
            ascii[8] = '\0';
            std::cerr << " ASCII(LE)=\"" << ascii << "\"";
            // Also show reversed (big endian)
            for (int i = 0; i < 4; i++) {
                std::swap(ascii[i], ascii[7-i]);
            }
            std::cerr << " ASCII(BE)=\"" << ascii << "\"\n";
            std::cerr.flush();
        }
        return nilObject_;  // Return proper nil instead of raw 0
    }

    ObjectHeader* header = obj.asObjectPtr();
    uint32_t classIdx = header->classIndex();
    Oop cls = classAtIndex(classIdx);

    // Debug: trace classOf for classIndex 3156 (OrderedCollection's metaclass)
    static int oc3156Count = 0;
    if (classIdx == 3156 && oc3156Count++ < 10) {
        static FILE* ocLog = nullptr;
        if (ocLog) {
            fprintf(ocLog, "[CLASSOF-3156 #%d] obj=0x%llx classIdx=%u -> cls=0x%llx\n",
                    oc3156Count, (unsigned long long)obj.rawBits(), classIdx,
                    (unsigned long long)cls.rawBits());
            // Check if obj and cls are the same
            if (obj.rawBits() == cls.rawBits()) {
                fprintf(ocLog, "  WARNING: obj == cls! This is wrong for class objects.\n");
            }
            fflush(ocLog);
        }
    }

    // Debug: trace classOf for nil object (classIdx 3075)
    static int nilClassOfCount = 0;
    if (classIdx == 3075 && nilClassOfCount++ < 5) {
        std::cerr << "[CLASSOF-NIL #" << nilClassOfCount << "] obj=0x" << std::hex << obj.rawBits()
                  << " classIdx=" << classIdx << " -> cls=0x" << cls.rawBits()
                  << " isNil=" << std::dec << cls.isNil() << "\n";
    }

    if (cls.isNil() || cls.rawBits() == 0) {
        static int nilClassCount = 0;
        static uint32_t lastBadClassIdx = 0;
        static int sameClassIdxCount = 0;

        nilClassCount++;

        // Detect infinite loop on same bad classIdx
        if (classIdx == lastBadClassIdx) {
            sameClassIdxCount++;
            if (sameClassIdxCount > 100) {
                if (sameClassIdxCount == 101) {
                    std::cerr << "[CLASSOF] STUCK IN LOOP on classIdx=" << classIdx
                              << " - loop detected, returning nil\n";
                }
                if (sameClassIdxCount > 1000) {
                    sameClassIdxCount = 0;
                }
            }
        } else {
            lastBadClassIdx = classIdx;
            sameClassIdxCount = 1;
        }

        if (nilClassCount <= 20 || classIdx == 3156) {
            std::cerr << "[CLASSOF] obj=0x" << std::hex << obj.rawBits()
                      << " classIdx=" << std::dec << classIdx
                      << " NOT FOUND in class table (tableSize=" << classTable_.size() << ")\n";
        }

        // Enhanced diagnostics on FIRST invalid classIdx after GC
        if (nilClassCount == 1) {
            uint64_t rawHeader = header->rawHeader();
            std::cerr << "[CLASSOF-DIAG] FIRST classIdx=0!\n"
                      << "  obj=0x" << std::hex << obj.rawBits() << std::dec << "\n"
                      << "  rawHeader=0x" << std::hex << rawHeader << std::dec << "\n"
                      << "  format=" << (int)header->format()
                      << " hash=" << header->identityHash()
                      << " slotCount=" << header->slotCount() << "\n"
                      << "  isMarked=" << header->isMarked()
                      << " isGrey=" << header->isGrey()
                      << " isPinned=" << header->isPinned() << "\n";

            // Check heap bounds
            uintptr_t objAddr = (uintptr_t)header;
            uintptr_t heapStart = (uintptr_t)oldSpaceStart_;
            uintptr_t heapEnd = (uintptr_t)oldSpaceFree_;
            std::cerr << "  heapStart=0x" << std::hex << heapStart
                      << " heapEnd=0x" << heapEnd
                      << " offset=" << std::dec << (objAddr - heapStart) << " bytes"
                      << " (" << (objAddr - heapStart) / (1024*1024) << "MB)\n";
            std::cerr << "  inHeap=" << (objAddr >= heapStart && objAddr < heapEnd) << "\n";

            // Dump 8 words around the address
            uint64_t* wordPtr = reinterpret_cast<uint64_t*>(header);
            std::cerr << "  MEMORY DUMP around obj:\n";
            for (int i = -2; i < 6; ++i) {
                uint64_t* p = wordPtr + i;
                if ((uintptr_t)p >= heapStart && (uintptr_t)p < heapEnd + 64) {
                    std::cerr << "    [" << (i >= 0 ? "+" : "") << i << "] 0x"
                              << std::hex << *p << std::dec;
                    if (i == 0) std::cerr << "  <-- HEADER";
                    std::cerr << "\n";
                }
            }

            // Try to find the enclosing object by scanning backwards
            // to find the previous valid-looking header
            std::cerr << "  SCANNING BACKWARDS for enclosing object...\n";
            uint64_t* scan = wordPtr - 1;
            int maxBack = 200;
            while (scan >= reinterpret_cast<uint64_t*>(oldSpaceStart_) && maxBack-- > 0) {
                uint64_t w = *scan;
                uint8_t numSlots = (w >> 56) & 0xFF;
                uint32_t ci = w & 0x3FFFFF;
                uint8_t fmt = (w >> 24) & 0x1F;
                if (ci > 0 && ci < 22000 && fmt <= 31 && numSlots < 255) {
                    // Could be a valid header
                    ObjectHeader* candidate = reinterpret_cast<ObjectHeader*>(scan);
                    size_t candSlots = candidate->slotCount();
                    size_t bodyBytes = candSlots * 8;
                    uintptr_t candEnd = (uintptr_t)scan + 8 + bodyBytes;
                    std::cerr << "    PREV obj=0x" << std::hex << (uintptr_t)scan
                              << std::dec
                              << " classIdx=" << ci << " fmt=" << (int)fmt
                              << " slots=" << candSlots
                              << " bodyEnd=0x" << std::hex << candEnd << std::dec
                              << " containsCorrupt=" << (candEnd > objAddr) << "\n";
                    if (candEnd > objAddr) {
                        // This object CONTAINS the corrupt address
                        size_t offsetInObj = objAddr - (uintptr_t)scan - 8;
                        std::cerr << "    CORRUPT ADDR IS INSIDE THIS OBJECT at byte offset "
                                  << offsetInObj << " (slot " << offsetInObj/8 << ")\n";
                    }
                    break;
                }
                scan--;
            }
            std::cerr.flush();

            // Log interpreter state to identify what code triggered this
            if (interpreter_) {
                interpreter_->logCurrentMethod(stderr);
            }
        }
    }
    return cls;
}

uint32_t ObjectMemory::registerClass(Oop classOop) {
    // In Spur, if the class already has an identity hash, that IS its class
    // table index. Use it rather than assigning a new sequential index.
    if (classOop.isObject()) {
        ObjectHeader* hdr = classOop.asObjectPtr();
        uint32_t hash = hdr->identityHash();
        if (hash != 0 && hash < classTable_.size()) {
            classTable_[hash] = classOop;
            return hash;
        }
    }
    // No hash yet — assign new index and set the hash to match
    uint32_t index = nextClassIndex_++;
    if (index < classTable_.size()) {
        classTable_[index] = classOop;
        if (classOop.isObject()) {
            classOop.asObjectPtr()->setIdentityHash(index);
        }
    }
    return index;
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
    std::cerr << "[CACHE] Context classIndex=" << contextClassIndex_ << "\n";
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
    // Get SmalltalkDictionary from special objects
    Oop smalltalkDict = specialObject(SpecialObjectIndex::SmalltalkDictionary);
    if (smalltalkDict.isNil() || !smalltalkDict.isObject()) {
        // std::cerr << "[DEBUG] findGlobal: SmalltalkDictionary is nil" << std::endl;
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
        // std::cerr << "[DEBUG] findGlobal: array slot is nil" << std::endl;
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

    // Search the array for the named global
    int totalAssocs = 0;
    for (size_t i = 0; i < arraySize; ++i) {
        Oop item = arrayHeader->slotAt(i);
        if (item.isNil() || !item.isObject()) continue;
        if (!isValidPointer(item)) continue;  // Validate pointer

        ObjectHeader* itemHeader = item.asObjectPtr();
        size_t slotCount = itemHeader->slotCount();
        if (slotCount < 2 || slotCount > 100) continue;  // Sanity check

        Oop key = fetchPointer(0, item);
        if (!key.isObject() || key.isNil()) continue;
        if (!isValidPointer(key)) continue;  // Validate key pointer

        ObjectHeader* keyHeader = key.asObjectPtr();
        if (!keyHeader->isBytesObject()) continue;

        size_t keySize = keyHeader->byteSize();
        if (keySize > 1000) continue;  // Sanity check

        totalAssocs++;
        if (symbolEquals(key, name.c_str())) {
            return fetchPointer(1, item);
        }
    }
    (void)totalAssocs;  // Suppress unused warning

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
    // Symbols are interned in Pharo. We need to find the Symbol instance
    // with the given content.

    static FILE* symLog = nullptr;
    static bool logInit = false;
    if (!logInit) {
        logInit = true;
        symLog = nullptr;
    }

    // Also find ByteSymbol class - Pharo uses ByteSymbol for most symbols
    Oop symbolClass = findGlobal("Symbol");
    Oop byteSymbolClass = findGlobal("ByteSymbol");

    // Debug logging removed for cleaner output

    // Use ByteSymbol if available (more common in Pharo), otherwise Symbol
    Oop targetClass = byteSymbolClass.isNil() ? symbolClass : byteSymbolClass;
    if (targetClass.isNil() || !targetClass.isObject()) {
        if (symLog) { fprintf(symLog, "[SYM] Symbol/ByteSymbol class not found for '%s'\n", name.c_str()); fflush(symLog); }
        return nilObject_;
    }

    // The class index that Symbol INSTANCES have is the identity hash of the Symbol class
    uint32_t symbolClassIdx = identityHashOf(targetClass);
    if (symbolClassIdx == 0) {
        symbolClassIdx = indexOfClass(targetClass);
    }
    if (symbolClassIdx == 0) {
        if (symLog) { fprintf(symLog, "[SYM] Symbol/ByteSymbol class has no valid index for '%s'\n", name.c_str()); fflush(symLog); }
        return nilObject_;
    }
    if (symLog) { fprintf(symLog, "[SYM] Looking for '%s', target classIdx=%u\n", name.c_str(), symbolClassIdx); fflush(symLog); }
    if (symLog) { fprintf(symLog, "[SYM] permSpace=%p-%p oldSpace=%p-%p\n",
                          (void*)permSpaceStart_, (void*)permSpaceEnd_,
                          (void*)oldSpaceStart_, (void*)oldSpaceEnd_); fflush(symLog); }

    // Helper lambda to scan a memory region for a symbol
    auto scanRegion = [&](const uint8_t* start, const uint8_t* end, const char* regionName) -> Oop {
        if (symLog) { fprintf(symLog, "[SYM] Scanning %s: %p-%p (%zu bytes)\n", regionName, (void*)start, (void*)end, (size_t)(end-start)); fflush(symLog); }
        if (start == nullptr || end == nullptr || start >= end) {
            if (symLog) { fprintf(symLog, "[SYM] %s is empty or invalid, skipping\n", regionName); fflush(symLog); }
            return nilObject_;
        }
        const uint8_t* scan = start;
        size_t objectCount = 0;
        while (scan < end) {
            ObjectHeader* header = reinterpret_cast<ObjectHeader*>(const_cast<uint8_t*>(scan));
            uint64_t rawHeader = header->rawHeader();

            // Debug disabled to reduce log size

            // Free chunks have classIndex == 0 (Spur format)
            uint32_t clsIdx = header->classIndex();
            if (clsIdx == 0) {
                // Skip this word and continue - classIndex 0 is reserved for free space
                scan += 8;
                continue;
            }

            // Debug bytes objects disabled

            // Check if this is a Symbol (ByteSymbol)
            if (clsIdx == symbolClassIdx) {
                // Check if content matches
                if (header->isBytesObject()) {
                    size_t byteSize = header->byteSize();
                    if (byteSize == name.size()) {
                        const uint8_t* bytes = header->bytes();
                        if (memcmp(bytes, name.c_str(), byteSize) == 0) {
                            // Found the symbol!
                            Oop result = Oop::fromObject(header);
                            if (symLog) { fprintf(symLog, "[SYM] FOUND '%s' in %s at 0x%llx\n", name.c_str(), regionName, (unsigned long long)result.rawBits()); fflush(symLog); }
                            return result;
                        }
                    }
                }
            }

            // Move to next object
            size_t objSize = header->totalSize();
            if (objSize == 0 || objSize > static_cast<size_t>(end - scan)) {
                // Invalid object size - skip 8 bytes and keep trying
                scan += 8;
                continue;
            }
            scan += objSize;
            objectCount++;
            if (objectCount % 200000 == 0 && symLog) {
                fprintf(symLog, "[SYM] %s: scanned %zu objects, at %p\n", regionName, objectCount, (void*)scan); fflush(symLog);
            }
        }
        if (symLog) { fprintf(symLog, "[SYM] %s: finished, scanned %zu objects\n", regionName, objectCount); fflush(symLog); }
        return nilObject_;
    };

    // Scan permanent space first (symbols are often there)
    if (symLog) { fprintf(symLog, "[SYM] Starting permSpace scan...\n"); fflush(symLog); }
    Oop result = scanRegion(permSpaceStart_, permSpaceEnd_, "permSpace");
    if (symLog) { fprintf(symLog, "[SYM] permSpace result: bits=0x%llx nilObj=0x%llx isFound=%d\n",
                          (unsigned long long)result.rawBits(), (unsigned long long)nilObject_.rawBits(),
                          result.rawBits() != nilObject_.rawBits()); fflush(symLog); }
    if (result.rawBits() != nilObject_.rawBits()) return result;

    // Scan old space (use oldSpaceFree_ which is the end of actual data, not oldSpaceEnd_ which is end of buffer)
    if (symLog) { fprintf(symLog, "[SYM] Starting oldSpace scan (using oldSpaceFree_=%p)...\n", (void*)oldSpaceFree_); fflush(symLog); }
    result = scanRegion(oldSpaceStart_, oldSpaceFree_, "oldSpace");
    if (symLog) { fprintf(symLog, "[SYM] oldSpace result: bits=0x%llx isFound=%d\n",
                          (unsigned long long)result.rawBits(),
                          result.rawBits() != nilObject_.rawBits()); fflush(symLog); }
    if (result.rawBits() != nilObject_.rawBits()) return result;

    if (symLog) { fprintf(symLog, "[SYM] NOT FOUND '%s' after scanning all spaces\n", name.c_str()); fflush(symLog); }
    return nilObject_;
}

bool ObjectMemory::setGlobal(const std::string& name, Oop value) {
    // Get SmalltalkDictionary from special objects
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

    // Search for existing binding
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
            // Found existing binding - update its value (slot 1)
            storePointer(1, item, value);
            return true;
        }
    }

    // Not found - create new binding
    // Find Symbol and Association classes
    Oop symbolClass = findGlobal("Symbol");
    Oop assocClass = findGlobal("Association");
    if (symbolClass.isNil() || assocClass.isNil()) {
        return false;
    }

    uint32_t assocClassIdx = indexOfClass(assocClass);
    uint32_t symbolClassIdx = indexOfClass(symbolClass);
    if (assocClassIdx == 0 || symbolClassIdx == 0) {
        return false;
    }

    // Allocate symbol (byte object)
    Oop symbolObj = allocateBytes(symbolClassIdx, name.size());
    if (symbolObj.isNil()) {
        return false;
    }

    // Copy name into symbol
    ObjectHeader* symHdr = symbolObj.asObjectPtr();
    std::memcpy(symHdr->bytes(), name.c_str(), name.size());

    // Allocate Association (2 pointer slots)
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
    // Get MethodContext class
    Oop contextClass = specialObject(SpecialObjectIndex::ClassMethodContext);
    if (contextClass.isNil()) {
        return nilObject_;
    }

    // Debug: Log Context class info once
    static bool loggedContextClass = false;
    if (!loggedContextClass) {
        loggedContextClass = true;
        if (contextClass.isObject()) {
            ObjectHeader* ctxClsHdr = contextClass.asObjectPtr();
            std::cerr << "[CONTEXT-CLASS] ClassMethodContext at 0x" << std::hex << contextClass.rawBits()
                      << " classIndex=" << std::dec << ctxClsHdr->classIndex()
                      << " format=" << (int)ctxClsHdr->format()
                      << " slots=" << ctxClsHdr->slotCount() << "\n";
            // Try to get class name
            if (ctxClsHdr->slotCount() > 6) {
                Oop nameOop = fetchPointer(6, contextClass);
                if (nameOop.isObject()) {
                    ObjectHeader* nameHdr = nameOop.asObjectPtr();
                    if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                        std::string name((char*)nameHdr->bytes(), nameHdr->byteSize());
                        std::cerr << "[CONTEXT-CLASS] Name: '" << name << "'\n";
                    }
                }
            }
            // Also check what class 3104 is
            Oop class3104 = classAtIndex(3104);
            if (class3104.isObject() && !class3104.isNil()) {
                ObjectHeader* c3104Hdr = class3104.asObjectPtr();
                std::cerr << "[CLASS-3104] Class at index 3104: 0x" << std::hex << class3104.rawBits()
                          << " format=" << std::dec << (int)c3104Hdr->format()
                          << " slots=" << c3104Hdr->slotCount() << "\n";
                if (c3104Hdr->slotCount() > 6) {
                    Oop name3104 = fetchPointer(6, class3104);
                    if (name3104.isObject()) {
                        ObjectHeader* n3104Hdr = name3104.asObjectPtr();
                        if (n3104Hdr->isBytesObject() && n3104Hdr->byteSize() < 100) {
                            std::string name((char*)n3104Hdr->bytes(), n3104Hdr->byteSize());
                            std::cerr << "[CLASS-3104] Name: '" << name << "'\n";
                        }
                    }
                }
            } else {
                std::cerr << "[CLASS-3104] Class at index 3104 is nil or not object\n";
            }
        }
    }

    // Get method header to determine temp count
    Oop methodHeader = fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) {
        return nilObject_;
    }

    int64_t headerBits = methodHeader.asSmallInteger();
    int numTemps = (headerBits >> 18) & 0x3F;  // VMMaker: MethodHeaderTempCountShift=21, untagged=18, 6 bits
    // In Spur 64-bit, numLiterals is in bits 0-14 (no shift needed)
    int numLiterals = headerBits & 0x7FFF;

    // MethodContext layout:
    // slot 0: sender
    // slot 1: pc (instruction pointer as SmallInteger offset, 1-based)
    // slot 2: stackp (stack pointer within context)
    // slot 3: method
    // slot 4: closureOrNil
    // slot 5: receiver
    // slots 6+: arguments, temporaries, stack

    // Context needs: 6 fixed slots + temps + some stack space
    size_t contextSize = 6 + numTemps + 32;  // 32 extra for stack

    uint32_t classIndex = indexOfClass(contextClass);
    if (classIndex == 0) {
        // Context class not in table, try to register it
        classIndex = const_cast<ObjectMemory*>(this)->registerClass(contextClass);
    }

    // Debug: Log Context class index once
    static bool loggedClassIndex = false;
    if (!loggedClassIndex) {
        loggedClassIndex = true;
        std::cerr << "[CONTEXT-ALLOC] Using classIndex=" << classIndex
                  << " for Context instances\n";
        // Verify round-trip
        Oop verifyClass = classAtIndex(classIndex);
        std::cerr << "[CONTEXT-ALLOC] classAtIndex(" << classIndex << ") returns 0x"
                  << std::hex << verifyClass.rawBits() << std::dec;
        if (verifyClass.rawBits() == contextClass.rawBits()) {
            std::cerr << " (MATCHES contextClass)\n";
        } else {
            std::cerr << " (MISMATCH! contextClass=0x" << std::hex << contextClass.rawBits()
                      << std::dec << ")\n";
        }
    }

    // Use IndexableWithFixed for contexts (format 3) - they have fixed + indexed fields
    Oop context = allocateSlots(classIndex, contextSize, ObjectFormat::IndexableWithFixed);
    if (context.isNil()) {
        return nilObject_;
    }

    // Calculate initial PC (after header + literals)
    // PC is 1-based byte offset from start of method
    int initialPC = (1 + numLiterals) * 8 + 1;  // +1 for 1-based

    // Initialize context
    storePointer(0, context, nil());                              // sender (nil = bottom of stack)
    storePointer(1, context, Oop::fromSmallInteger(initialPC));   // pc
    storePointer(2, context, Oop::fromSmallInteger(numTemps + 5)); // stackp
    storePointer(3, context, method);                              // method
    storePointer(4, context, nil());                               // closureOrNil
    storePointer(5, context, receiver);                            // receiver

    // Initialize temporaries to nil
    for (int i = 0; i < numTemps; ++i) {
        storePointer(6 + i, context, nil());
    }

    return context;
}

Oop ObjectMemory::createStartupContextWithArg(Oop method, Oop receiver, Oop arg) {
    // Get MethodContext class
    Oop contextClass = specialObject(SpecialObjectIndex::ClassMethodContext);
    if (contextClass.isNil()) {
        return nilObject_;
    }

    // Get method header to determine temp count and arg count
    Oop methodHeader = fetchPointer(0, method);
    if (!methodHeader.isSmallInteger()) {
        return nilObject_;
    }

    int64_t headerBits = methodHeader.asSmallInteger();
    int numTemps = (headerBits >> 18) & 0x3F;  // VMMaker: MethodHeaderTempCountShift=21, untagged=18, 6 bits
    int numArgs = (headerBits >> 24) & 0xF;  // Arguments are in bits 24-27
    int numLiterals = headerBits & 0x7FFF;

    // Sanity check: we expect 1 argument for this method
    if (numArgs != 1) {
        static FILE* uiLog = nullptr;
        if (!uiLog) uiLog = nullptr;
        if (uiLog) {
            fprintf(uiLog, "[UI] Warning: method has %d args, expected 1\n", numArgs);
            fflush(uiLog);
        }
        // Still try to create context with the provided arg
    }

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

    // Calculate initial PC (after header + literals)
    int initialPC = (1 + numLiterals) * 8 + 1;

    // Initialize context
    storePointer(0, context, nil());                                      // sender
    storePointer(1, context, Oop::fromSmallInteger(initialPC));           // pc
    storePointer(2, context, Oop::fromSmallInteger(numArgs + numTemps + 5)); // stackp
    storePointer(3, context, method);                                      // method
    storePointer(4, context, nil());                                       // closureOrNil
    storePointer(5, context, receiver);                                    // receiver

    // Store the argument
    storePointer(6, context, arg);

    // Initialize temporaries to nil (after args)
    for (int i = 0; i < numTemps; ++i) {
        storePointer(6 + numArgs + i, context, nil());
    }

    return context;
}

// ===== OBJECT ACCESS =====

Oop ObjectMemory::fetchPointer(size_t index, Oop obj) const {
    // Return proper nil object instead of raw 0 for error cases
    // This prevents corruption when the result is used as a message receiver
    if (!obj.isObject()) return nilObject_;

    // Validate pointer before dereferencing
    if (!isValidPointer(obj)) {
        return nilObject_;
    }

    ObjectHeader* header = obj.asObjectPtr();

    if (index >= header->slotCount()) return nilObject_;

    return header->slotAt(index);
}

void ObjectMemory::storePointer(size_t index, Oop obj, Oop value) {
    if (!obj.isObject()) return;
    ObjectHeader* header = obj.asObjectPtr();

    // Bounds check
    if (index >= header->slotCount()) return;

    // Diagnostic: detect when Symbol class (classIndex 3094) is stored ANYWHERE
    if (value.isObject() && value.rawBits() > 0x10000) {
        ObjectHeader* valHdr = value.asObjectPtr();
        if (valHdr->classIndex() == g_symbolMetaclassIdx) {
            uint32_t objCI = header->classIndex();
            // Skip Context (ci=36) and BlockClosure/FullBlockClosure (ci=38)
            // These legitimately store Symbol class as receiver
            if (objCI != 36 && objCI != 38) {
                static int symClsStoreCount = 0;
                if (++symClsStoreCount <= 50) {
                    uint32_t objSlots = header->slotCount();
                    Oop oldVal = header->slotAt(index);
                    fprintf(stderr, "[STORE-SYMCLS #%d] step=%llu obj=0x%llx(ci%u,slots%u) idx=%zu "
                            "val=0x%llx oldVal=0x%llx\n",
                            symClsStoreCount, (unsigned long long)g_stepNum,
                            (unsigned long long)obj.rawBits(), objCI, objSlots, index,
                            (unsigned long long)value.rawBits(),
                            (unsigned long long)oldVal.rawBits());
                    // C++ backtrace to find caller
                    void* bt[20];
                    int depth = backtrace(bt, 20);
                    char** syms = backtrace_symbols(bt, depth);
                    if (syms) {
                        for (int i = 0; i < depth && i < 10; i++)
                            fprintf(stderr, "  bt[%d]: %s\n", i, syms[i]);
                        free(syms);
                    }
                    fflush(stderr);
                }
            }
        }
    }

    // Check for old->young pointer (needs remembered set)
    if (isOld(obj) && value.isObject() && isYoung(value)) {
        rememberObject(obj);
    }

    header->slotAtPut(index, value);
}

uint8_t ObjectMemory::fetchByte(size_t index, Oop obj) const {
    if (!obj.isObject()) return 0;
    ObjectHeader* header = obj.asObjectPtr();
    if (!header->isBytesObject()) return 0;  // Must be bytes object for byteAt
    if (index >= header->byteSize()) return 0;
    return header->byteAt(index);
}

void ObjectMemory::storeByte(size_t index, Oop obj, uint8_t value) {
    if (!obj.isObject()) return;
    ObjectHeader* header = obj.asObjectPtr();
    if (!header->isBytesObject()) return;  // Must be bytes object for byteAtPut
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
        size_t slots = header->slotCount();
        for (size_t i = 0; i < slots; ++i) {
            Oop slot = header->slotAt(i);
            if (slot == obj1) {
                header->slotAtPut(i, obj2);
            } else if (slot == obj2) {
                header->slotAtPut(i, obj1);
            }
        }
    });

    return true;
}

bool ObjectMemory::becomeForward(Oop obj1, Oop obj2) {
    if (!obj1.isObject() || !obj2.isObject()) {
        return false;
    }

    allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;
        ObjectHeader* header = obj.asObjectPtr();
        ObjectFormat format = header->format();

        // Skip non-pointer objects (byte/word/short arrays) - their slots are raw data, not Oops
        if (format >= ObjectFormat::Indexable8 && format <= ObjectFormat::Indexable8_7) return;
        if (format >= ObjectFormat::Indexable32 && format <= ObjectFormat::Indexable64) return;
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
            }
        }
    });

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
    if (hash == 0) {
        hash = generateHash();
        header->setIdentityHash(hash);
    }
    return hash;
}

void ObjectMemory::ensureIdentityHash(Oop obj) {
    identityHashOf(obj);  // Side effect: generates hash if needed
}

uint32_t ObjectMemory::generateHash() {
    // Use D.H. Lehmer's linear congruential generator (same as official Pharo VM)
    // lastHash = lastHash * 16807 (which is 7^5)
    // Adding top bits gives better spread
    uint32_t hash;
    do {
        lastHash_ = (lastHash_ * 16807) & 0xFFFFFFFF;
        hash = (lastHash_ + (lastHash_ >> 4)) & 0x3FFFFF;  // 22-bit hash
    } while (hash == 0);  // 0 means unhashed, so regenerate
    return hash;
}

// ===== GARBAGE COLLECTION =====

GCResult ObjectMemory::scavenge() {
    auto start = std::chrono::steady_clock::now();
    GCResult result{0, 0, 0};

    // CRITICAL BUG FIX: The previous scavenge implementation copied objects from
    // eden to old space but DID NOT UPDATE POINTERS. This caused memory corruption
    // because:
    // 1. Objects were copied to old space
    // 2. Eden was reset (edenFree_ = edenStart_)
    // 3. But pointers to eden objects were NOT updated
    // 4. New allocations reused eden addresses, overwriting "live" objects
    //
    // Proper scavenging requires forwarding pointers or a Cheney-style two-finger
    // algorithm with pointer updating. Until that's implemented, we MUST NOT
    // reset eden after copying.
    //
    // For now, just promote everything to old space WITHOUT resetting eden.
    // This wastes eden space but prevents corruption.

    uint8_t* scan = edenStart_;
    while (scan < edenFree_) {
        ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(scan);
        size_t size = obj->totalSize();

        // Copy to old space
        if (oldSpaceFree_ + size <= oldSpaceEnd_) {
            std::memcpy(oldSpaceFree_, obj, size);
            oldSpaceFree_ += size;
            result.objectsMoved++;
            result.bytesReclaimed += size;
        }

        scan += size;
    }

    // DO NOT reset eden - pointers are not updated!
    // edenFree_ = edenStart_;  // DISABLED: causes memory corruption

    auto end = std::chrono::steady_clock::now();
    result.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    gcCount_++;
    totalGCTime_ += result.milliseconds;

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
    static int sweepCount = 0;
    sweepCount++;

    auto start = std::chrono::steady_clock::now();

    // 1. Clear all marks
    ObjectScanner clearScanner(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = clearScanner.next()) {
        obj->setMarked(false);
    }

    // 2. Mark phase (same as fullGC)
    size_t markedCount = markPhase();

    // 3. Sweep: convert dead objects to free chunks, coalesce adjacent ones,
    //    and shrink oldSpaceFree_ if tail is dead.
    clearFreeLists();

    uint8_t* lastLiveEnd = oldSpaceStart_;
    size_t deadCount = 0;
    size_t deadBytes = 0;

    // We need to coalesce adjacent dead objects into single free chunks.
    // Track start of current dead run.
    uint8_t* deadRunStart = nullptr;

    ObjectScanner sweepScanner(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = sweepScanner.next()) {
        uint8_t* objAddr = reinterpret_cast<uint8_t*>(obj);
        size_t objSize = obj->totalSize();
        bool hasOverflow = obj->hasOverflowSlots();
        uint8_t* objStart = hasOverflow ? (objAddr - 8) : objAddr;
        size_t fullSize = hasOverflow ? (objSize + 8) : objSize;

        if (obj->isMarked()) {
            // Live object — clear mark
            obj->setMarked(false);
            lastLiveEnd = objStart + fullSize;

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
            // Dead object
            deadCount++;
            deadBytes += fullSize;

            // Start or extend dead run
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

    size_t usedAfter = oldSpaceFree_ - oldSpaceStart_;
    size_t freeAfter = oldSpaceEnd_ - oldSpaceFree_;

    if (sweepCount <= 10) {
        fprintf(stderr, "[SWEEP-GC #%d] marked=%zu dead=%zu (%zuKB) time=%lldms used=%zuMB free=%zuMB\n",
                sweepCount, markedCount, deadCount, deadBytes / 1024,
                (long long)ms, usedAfter / (1024*1024), freeAfter / (1024*1024));
        fflush(stderr);
    }

    // If less than 25% free after sweep, request compacting GC at next safe point
    size_t totalSpace = oldSpaceEnd_ - oldSpaceStart_;
    if (freeAfter < totalSpace / 4) {
        needsCompactGC_ = true;
        if (sweepCount <= 5) {
            fprintf(stderr, "[SWEEP-GC] Set needsCompactGC_ (free=%zuMB < %zuMB threshold)\n",
                    freeAfter / (1024*1024), totalSpace / 4 / (1024*1024));
            fflush(stderr);
        }
    }

    gcCount_++;
    totalGCTime_ += ms;
}

GCResult ObjectMemory::fullGC() {
    auto start = std::chrono::steady_clock::now();
    GCResult result{0, 0, 0};

    static int gcCallCount = 0;
    gcCallCount++;

    size_t usedBefore = oldSpaceFree_ - oldSpaceStart_;

    // 1. Convert interpreter IPs to offsets (methods may move)
    if (interpreter_) {
        interpreter_->prepareForGC();
    }

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

    // 3. Mark phase
    size_t markedCount = markPhase();

    // DIAGNOSTIC: Lightweight Symbol class corruption check.
    // Compare slot values against the known Oop for Symbol class (classIdx 3094).
    // Uses simple 64-bit bit comparison — no pointer following.
    Oop symClassOop;
    int preCompCount = 0;
    bool symCheckEnabled = (classTable_.size() > 3094 && classTable_[3094].isObject());
    if (symCheckEnabled) {
        symClassOop = classTable_[3094];
        uint64_t symBits = symClassOop.rawBits();
        ObjectScanner scanner(oldSpaceStart_, oldSpaceFree_);
        while (ObjectHeader* obj = scanner.next()) {
            if (!obj->isMarked()) continue;
            uint32_t ci = obj->classIndex();
            if (ci == 36 || ci == 38) continue;  // Context, FullBlockClosure — legitimate
            if (obj->isCompiledMethod()) continue;
            ObjectFormat fmt = obj->format();
            if (fmt > ObjectFormat::WeakWithFixed) continue;
            size_t numSlots = obj->slotCount();
            Oop* slots = obj->slots();
            for (size_t i = 0; i < numSlots; i++) {
                if (slots[i].rawBits() == symBits) preCompCount++;
            }
        }
    }

    // 4. Plan + update + copy (compact)
    planCompactSavingForwarders();
    updatePointersAfterCompact();
    copyAndUnmark();

    // 5. Rebuild free list from gap
    rebuildFreeListAfterCompact();

    // 6. Update nil bits if nil moved
    if (nilObject_.isObject()) {
        Oop::setNilBits(nilObject_.rawBits());
    }

    // 7. Restore interpreter IPs from offsets
    if (interpreter_) {
        interpreter_->afterGC();
    }

    forceGCFlag_ = false;

    size_t usedAfter = oldSpaceFree_ - oldSpaceStart_;
    result.bytesReclaimed = (usedBefore > usedAfter) ? (usedBefore - usedAfter) : 0;

    auto end = std::chrono::steady_clock::now();
    result.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    if (gcCallCount <= 10) {
        fprintf(stderr, "[FULL-GC #%d] step=%llu marked=%zu reclaimed=%zuKB time=%lldms used=%zuMB\n",
                gcCallCount, (unsigned long long)g_stepNum, markedCount,
                result.bytesReclaimed / 1024, (long long)result.milliseconds,
                usedAfter / (1024*1024));
        fflush(stderr);
    }
    // Log GC to file (Mac Catalyst stderr is lost)
    {
        FILE* gcLog = fopen("/tmp/iospharo-gc.log", "a");
        if (gcLog) {
            fprintf(gcLog, "[FULL-GC #%d] marked=%zu reclaimed=%zuKB time=%zums used=%zuMB\n",
                    gcCallCount, markedCount, result.bytesReclaimed / 1024,
                    (size_t)result.milliseconds, usedAfter / (1024*1024));
            fflush(gcLog);
            fclose(gcLog);
        }
    }

    // POST-GC VERIFICATION: Compare Symbol class ref count before vs after compaction.
    // After compaction, Symbol class may have moved — use updated classTable entry.
    // If count INCREASED, compaction introduced corruption.
    if (symCheckEnabled) {
        Oop newSymClassOop = classTable_[3094];
        uint64_t newSymBits = newSymClassOop.rawBits();
        int postCompCount = 0;
        int firstBadCi = 0; uint64_t firstBadObj = 0; int firstBadSlot = -1;
        ObjectScanner scanner(oldSpaceStart_, oldSpaceFree_);
        while (ObjectHeader* obj = scanner.next()) {
            uint32_t ci = obj->classIndex();
            if (ci == 36 || ci == 38) continue;
            if (obj->isCompiledMethod()) continue;
            ObjectFormat fmt = obj->format();
            if (fmt > ObjectFormat::WeakWithFixed) continue;
            size_t numSlots = obj->slotCount();
            Oop* slots = obj->slots();
            for (size_t i = 0; i < numSlots; i++) {
                if (slots[i].rawBits() == newSymBits) {
                    postCompCount++;
                    if (firstBadSlot < 0 && postCompCount > preCompCount) {
                        firstBadCi = ci;
                        firstBadObj = reinterpret_cast<uint64_t>(obj);
                        firstBadSlot = (int)i;
                    }
                }
            }
        }
        if (postCompCount > preCompCount) {
            fprintf(stderr, "[GC-SYM-CORRUPT #%d] Compaction introduced Symbol class refs! "
                    "%d -> %d (+%d) firstNew: ci=%d obj=0x%llx slot=%d symOop=0x%llx->0x%llx\n",
                    gcCallCount, preCompCount, postCompCount, postCompCount - preCompCount,
                    firstBadCi, (unsigned long long)firstBadObj, firstBadSlot,
                    (unsigned long long)symClassOop.rawBits(),
                    (unsigned long long)newSymClassOop.rawBits());
        }
    }

    // Record compacted size for threshold-based GC triggering
    lastCompactedSize_ = oldSpaceFree_ - oldSpaceStart_;

    gcCount_++;
    totalGCTime_ += result.milliseconds;
    return result;
}

bool ObjectMemory::needsGC() const {
    if (forceGCFlag_) return true;

    // Check if eden is nearly full
    size_t edenSize = survivorStart_ - edenStart_;
    size_t edenUsed = edenFree_ - edenStart_;
    return edenUsed > (edenSize * 90 / 100);  // 90% full
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
            bool hasOverflow = false;
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
                        hasOverflow = true;
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

            callback(oopFromPointer(obj));

            // Advance past the object
            scan += size;
        }
    };

    // Scan permanent space
    scanRegion(permSpaceStart_, permSpaceEnd_);

    // Scan old space
    scanRegion(oldSpaceStart_, oldSpaceFree_);

    // Scan eden
    scanRegion(edenStart_, edenFree_);
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
        { edenStart_, edenFree_ },
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
        { edenStart_, edenFree_ },
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
    stats.bytesFree = (oldSpaceEnd_ - oldSpaceFree_) +
                      (survivorStart_ - edenFree_);
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
            if (oldSpaceFree_ + size <= oldSpaceEnd_) {
                // Fast path: bump pointer allocation
                ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(oldSpaceFree_);
                oldSpaceFree_ += size;

                // Threshold-based GC trigger: request compacting GC at next safe point
                // when heap usage exceeds last compacted size + headroom.
                // This avoids running GC from allocation where C++ locals hold Oops.
                size_t used = oldSpaceFree_ - oldSpaceStart_;
                size_t gcThreshold = lastCompactedSize_ + gcHeadroom_;
                if (used > gcThreshold && !needsCompactGC_) {
                    needsCompactGC_ = true;
                    // GC threshold crossed — compaction will run at next safe point
                }
                return obj;
            }
            // Bump pointer full — OOM
            return nullptr;
        }

        case Space::New:
            return allocateInEden(size);

        default:
            return nullptr;
    }
}

ObjectHeader* ObjectMemory::allocateInEden(size_t size) {
    size = (size + 7) & ~7ULL;

    if (edenFree_ + size > survivorStart_) {
        return nullptr;  // Eden is full
    }

    ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(edenFree_);
    edenFree_ += size;
    return obj;
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

void ObjectMemory::rememberObject(Oop obj) {
    if (obj.isObject()) {
        ObjectHeader* hdr = obj.asObjectPtr();
        if (!hdr->isRemembered()) {
            hdr->setRemembered(true);
            rememberedSet_.push_back(hdr);
        }
    }
}

Oop ObjectMemory::promoteObject(Oop obj) {
    if (!obj.isObject() || !isYoung(obj)) {
        return obj;
    }

    ObjectHeader* src = obj.asObjectPtr();
    size_t size = src->totalSize();

    ObjectHeader* dst = allocateRaw(size, Space::Old);
    if (!dst) {
        return nilObject_;  // Out of memory
    }

    std::memcpy(dst, src, size);
    return Oop::fromObject(dst, Space::Old);
}

void ObjectMemory::copyObjectBytes(ObjectHeader* from, ObjectHeader* to) {
    std::memcpy(to, from, from->totalSize());
}

void ObjectMemory::updatePointer(Oop& ptr) {
    // Used during GC to update forwarded pointers
    // TODO: Implement with forwarding pointers
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
    if (scanRegion(edenStart_, edenFree_)) return found;

    return nilObject_;  // Not found
}

Oop ObjectMemory::nextInstanceAfter(Oop afterObject, uint32_t targetClassIndex) {
    if (!afterObject.isObject()) return nilObject_;

    // Get the address of the starting object
    ObjectHeader* startPtr = afterObject.asObjectPtr();
    uint8_t* startAddr = reinterpret_cast<uint8_t*>(startPtr);
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
            if (scanRegion(edenStart_, edenFree_)) return found;
            break;
        case Space::Old:
            if (scanRegion(oldSpaceStart_, oldSpaceFree_)) return found;
            if (scanRegion(edenStart_, edenFree_)) return found;
            break;
        case Space::New:
            if (scanRegion(edenStart_, edenFree_)) return found;
            break;
        case Space::Reserved:
            // Reserved space - search all spaces
            if (scanRegion(permSpaceStart_, permSpaceEnd_)) return found;
            if (scanRegion(oldSpaceStart_, oldSpaceFree_)) return found;
            if (scanRegion(edenStart_, edenFree_)) return found;
            break;
    }

    return nilObject_;  // Not found
}

// ===== FREE LIST HELPERS =====

ObjectHeader* ObjectMemory::makeFreeChunk(uint8_t* addr, size_t size) {
    // A free chunk has classIndex=0 and stores its size in slots.
    // Minimum free chunk is 16 bytes (8-byte header + 8-byte next pointer).
    if (size < 16) {
        // Too small for a free chunk — just zero it
        std::memset(addr, 0, size);
        return nullptr;
    }

    ObjectHeader* chunk = reinterpret_cast<ObjectHeader*>(addr);
    size_t slotCount = (size - sizeof(ObjectHeader)) / 8;

    // Build header: classIndex=0, format=0, slotCount
    uint8_t slots = (slotCount >= 255) ? 255 : static_cast<uint8_t>(slotCount);
    uint64_t header = ObjectHeader::makeHeader(slots, 0, ObjectFormat::ZeroSized, 0);
    chunk->setRawHeader(header);

    // For overflow, write the overflow word before the header
    if (slotCount >= 255) {
        // This is more complex — for now, free chunks > 255 slots go to the
        // large list at index 0. We'll set up the overflow word.
        uint64_t* overflow = reinterpret_cast<uint64_t*>(addr);
        *overflow = slotCount | (0xFFULL << 56);
        chunk = reinterpret_cast<ObjectHeader*>(addr + 8);
        header = ObjectHeader::makeHeader(255, 0, ObjectFormat::ZeroSized, 0);
        chunk->setRawHeader(header);
    }

    // Zero the body (next pointer in slot 0 will be set by addToFreeList)
    std::memset(chunk->bytes(), 0, slotCount * 8);

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
                freeLists_[sizeInSlots] = next.isObject() ? next.asObjectPtr() : nullptr;
                if (!freeLists_[sizeInSlots]) {
                    freeListsMask_ &= ~(1ULL << sizeInSlots);
                }
                return chunk;
            }
        }
    }

    // Try large chunk list (first fit)
    if (freeListsMask_ & 1ULL) {
        ObjectHeader** prev = &freeLists_[0];
        ObjectHeader* chunk = freeLists_[0];
        while (chunk) {
            size_t chunkSize = chunk->totalSize();
            if (chunkSize >= size) {
                // Unlink
                Oop next = chunk->slotCount() > 0 ? chunk->slotAt(0) : Oop::nil();
                *prev = next.isObject() ? next.asObjectPtr() : nullptr;
                if (!freeLists_[0]) {
                    freeListsMask_ &= ~1ULL;
                }

                // If leftover is big enough, put remainder back
                size_t remainder = chunkSize - size;
                if (remainder >= 16) {
                    uint8_t* remainderAddr = reinterpret_cast<uint8_t*>(chunk) + size;
                    ObjectHeader* remChunk = makeFreeChunk(remainderAddr, remainder);
                    if (remChunk) {
                        addToFreeList(remChunk, remainder);
                    }
                }

                return chunk;
            }
            // Advance
            Oop next = chunk->slotCount() > 0 ? chunk->slotAt(0) : Oop::nil();
            prev = reinterpret_cast<ObjectHeader**>(&chunk->slots()[0]);
            chunk = next.isObject() ? next.asObjectPtr() : nullptr;
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
    auto p = reinterpret_cast<uint8_t*>(obj);
    if (p < oldSpaceStart_ || p >= oldSpaceFree_) {
        static int badPtrCount = 0;
        if (badPtrCount++ < 20) {
            std::cerr << "[GC-MARK] BAD pointer 0x" << std::hex
                      << oop.rawBits() << " at obj " << (uintptr_t)obj
                      << " (outside used old space 0x" << (uintptr_t)oldSpaceStart_
                      << "-0x" << (uintptr_t)oldSpaceFree_ << ")" << std::dec;
            if (currentScanParent_) {
                std::cerr << " parent=0x" << std::hex << (uintptr_t)currentScanParent_
                          << " cls=" << std::dec << currentScanParent_->classIndex()
                          << " fmt=" << (int)currentScanParent_->format()
                          << " slots=" << currentScanParent_->slotCount()
                          << " slot#=" << currentScanSlot_;
            }
            std::cerr << "\n";
        }
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
        static int interiorPtrCount = 0;
        if (interiorPtrCount++ < 20) {
            std::cerr << "[GC-MARK] INTERIOR pointer 0x" << std::hex
                      << oop.rawBits() << " at obj " << (uintptr_t)obj
                      << " classIdx=" << std::dec << classIdx;
            if (currentScanParent_) {
                std::cerr << " parent=0x" << std::hex << (uintptr_t)currentScanParent_
                          << " cls=" << std::dec << currentScanParent_->classIndex()
                          << " slot#=" << currentScanSlot_;
            }
            std::cerr << "\n";
        }
        return;
    }

    // Definitive interior pointer check: verify this address is at a real object
    // start. The classIndex check above can pass for interior pointers if the
    // slot data happens to have bits 0-21 matching a valid class table entry.
    if (!validObjectStarts_.empty()) {
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
        if (total > 0) {
            Oop key = obj->slotAt(keyIndex);
            if (key.isObject() && !isPermObject(key.asObjectPtr())) {
                keyAlive = key.asObjectPtr()->isMarked();
            }
            // Immediates and perm objects are always "alive"
        }

        ephemeronEncounterCount_++;
        if (keyAlive) ephemeronInactiveCount_++;
        else ephemeronActiveCount_++;

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

void ObjectMemory::scanPointerFields(ObjectHeader* obj) {
    size_t numPointers = pointerSlotsOf(obj);
    Oop* slots = obj->slots();
    currentScanParent_ = obj;
    for (size_t i = 0; i < numPointers; ++i) {
        currentScanSlot_ = i;
        markAndTrace(slots[i]);
    }
    currentScanParent_ = nullptr;
}

size_t ObjectMemory::pointerSlotsOf(ObjectHeader* obj) const {
    ObjectFormat fmt = obj->format();

    // Pointer objects (formats 0-5): all slots are pointers
    if (fmt <= ObjectFormat::WeakWithFixed) {
        size_t totalSlots = obj->slotCount();

        // Context objects: scan ALL slots, not just up to stackp.
        // The stackp optimization is unsafe because prepareForGC syncs temps
        // to the Context without always updating stackp. A stale stackp causes
        // the GC to skip valid pointer slots during both marking and compaction
        // reference updating, leading to classIdx=0 crashes (stale pointers to
        // memory freed by compaction). Scanning nil slots beyond stackp is cheap.

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
    for (ObjectHeader* obj : weakList_) {
        size_t slots = obj->slotCount();
        Oop* slotPtr = obj->slots();
        size_t startSlot = fixedFieldCountOf(obj);
        bool anyNilled = false;
        for (size_t i = startSlot; i < slots; ++i) {
            Oop ref = slotPtr[i];
            if (ref.isObject() && !isPermObject(ref.asObjectPtr())) {
                if (!ref.asObjectPtr()->isMarked()) {
                    slotPtr[i] = nilObject_;
                    anyNilled = true;
                }
            }
        }
        if (anyNilled) {
            // Queue weak object as mourner (matches Spur behavior).
            // WeakFinalizationList detects collected entries this way.
            mournQueue_.push_back(Oop::fromObject(obj));
            pendingFinalizationSignals_++;
        }
    }
}

bool ObjectMemory::markInactiveEphemerons() {
    bool foundInactive = false;
    size_t writeIdx = 0;

    for (size_t i = 0; i < ephemeronList_.size(); ++i) {
        ObjectHeader* obj = ephemeronList_[i];
        size_t total = obj->slotCount();
        constexpr size_t keyIndex = 0;  // Key is always slot 0

        bool keyAlive = true;
        if (total > 0) {
            Oop key = obj->slotAt(keyIndex);
            if (key.isObject() && !isPermObject(key.asObjectPtr())) {
                keyAlive = key.asObjectPtr()->isMarked();
            }
        }

        if (keyAlive) {
            // Key became reachable — mark all fields as strong
            scanPointerFields(obj);
            processMarkStack();
            foundInactive = true;
            // Don't keep in list (removed by not copying to writeIdx)
        } else {
            // Still active — keep in list
            ephemeronList_[writeIdx++] = obj;
        }
    }
    ephemeronList_.resize(writeIdx);
    return foundInactive;
}

void ObjectMemory::fireAllEphemerons() {
    for (ObjectHeader* obj : ephemeronList_) {
        // Fire: change format from 5 (WeakWithFixed/Ephemeron) to 1 (FixedSize)
        // so it's no longer treated as an ephemeron in subsequent GCs.
        obj->setFormat(ObjectFormat::FixedSize);

        // Queue as mourner
        Oop objOop = Oop::fromObject(obj);
        mournQueue_.push_back(objOop);
        pendingFinalizationSignals_++;

        // Mark ALL fields including the key. The Spur VM marks everything
        // when firing — the key stays alive so the finalization process can
        // read it. Weak references to the key are NOT nilled in this GC cycle.
        // Instead, the finalization process (signaled via TheFinalizationSemaphore)
        // runs cleanup actions (e.g., removing subscriptions from registries).
        // The key becomes truly unreachable on the next GC cycle.
        scanPointerFields(obj);
        processMarkStack();
    }
    ephemeronList_.clear();
}

size_t ObjectMemory::markPhase() {
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
    {
        ObjectScanner buildScan(oldSpaceStart_, oldSpaceFree_);
        while (ObjectHeader* obj = buildScan.next()) {
            validObjectStarts_.insert(reinterpret_cast<uintptr_t>(obj));
        }
    }

    size_t markedCount = 0;

    // 1. Mark from memory roots (special objects, class table)
    forEachMemoryRoot([this](Oop& oop) {
        markAndTrace(oop);
    });

    // 2. Mark from interpreter roots
    if (interpreter_) {
        interpreter_->forEachRoot([this](Oop& oop) {
            markAndTrace(oop);
        });
    }

    // 3. Drain mark stack
    processMarkStack();

    // 4. Ephemeron fixed-point iteration
    // Some ephemerons' keys may have become reachable through other marking.
    // Iterate until no more ephemerons become inactive, then fire the rest.
    {
        size_t fired = 0;
        if (!ephemeronList_.empty()) {
            while (markInactiveEphemerons()) {}
            fired = ephemeronList_.size();
            fireAllEphemerons();
        }
        if constexpr (false) {
            fprintf(stderr, "[EPHEMERON] encountered=%zu aliveKey=%zu deadKey=%zu fired=%zu mourners=%zu pending=%d\n",
                    ephemeronEncounterCount_, ephemeronInactiveCount_, ephemeronActiveCount_,
                    fired, mournQueue_.size(), pendingFinalizationSignals_);
        }
    }

    // 5. Process weak objects (nil dead references, queue mourners)
    // DIAGNOSTIC: scan WeakArrays for Symbol class BEFORE processWeaklings
    {
        static int gcDiagCount = 0;
        gcDiagCount++;
        int symClsInWeak = 0;
        for (ObjectHeader* obj : weakList_) {
            size_t slots = obj->slotCount();
            size_t startSlot = fixedFieldCountOf(obj);
            for (size_t i = startSlot; i < slots; ++i) {
                Oop ref = obj->slotAt(i);
                if (ref.isObject() && ref.rawBits() > 0x10000 &&
                    ref.asObjectPtr()->classIndex() == g_symbolMetaclassIdx) {
                    symClsInWeak++;
                    if (symClsInWeak <= 5) {
                        fprintf(stderr, "[GC-DIAG #%d BEFORE] Symbol class in WeakArray! "
                                "obj=0x%llx(ci%u,slots%zu) slot=%zu marked=%d\n",
                                gcDiagCount,
                                (unsigned long long)Oop::fromObject(obj).rawBits(),
                                obj->classIndex(), slots, i,
                                ref.asObjectPtr()->isMarked() ? 1 : 0);
                        fflush(stderr);
                    }
                }
            }
        }
        if (symClsInWeak > 0 && gcDiagCount <= 20) {
            fprintf(stderr, "[GC-DIAG #%d] Found %d Symbol class refs in WeakArrays BEFORE processWeaklings\n",
                    gcDiagCount, symClsInWeak);
            fflush(stderr);
        }
    }
    processWeaklings();
    // DIAGNOSTIC: scan WeakArrays for Symbol class AFTER processWeaklings
    {
        static int gcDiagCountPost = 0;
        gcDiagCountPost++;
        int symClsInWeak = 0;
        for (ObjectHeader* obj : weakList_) {
            size_t slots = obj->slotCount();
            size_t startSlot = fixedFieldCountOf(obj);
            for (size_t i = startSlot; i < slots; ++i) {
                Oop ref = obj->slotAt(i);
                if (ref.isObject() && ref.rawBits() > 0x10000 &&
                    ref.asObjectPtr()->classIndex() == g_symbolMetaclassIdx) {
                    symClsInWeak++;
                    if (symClsInWeak <= 5) {
                        fprintf(stderr, "[GC-DIAG #%d AFTER] Symbol class STILL in WeakArray! "
                                "obj=0x%llx(ci%u,slots%zu) slot=%zu marked=%d\n",
                                gcDiagCountPost,
                                (unsigned long long)Oop::fromObject(obj).rawBits(),
                                obj->classIndex(), slots, i,
                                ref.asObjectPtr()->isMarked() ? 1 : 0);
                        fflush(stderr);
                    }
                }
            }
        }
        if (symClsInWeak > 0 && gcDiagCountPost <= 20) {
            fprintf(stderr, "[GC-DIAG #%d] Found %d Symbol class refs in WeakArrays AFTER processWeaklings\n",
                    gcDiagCountPost, symClsInWeak);
            fflush(stderr);
        }
    }

    // 6. Count marked objects
    ObjectScanner scanner(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = scanner.next()) {
        if (obj->isMarked()) markedCount++;
    }

    return markedCount;
}

// ===== COMPACT PHASE =====

bool ObjectMemory::planCompactSavingForwarders() {
    // Use eden as scratch space for saved first fields.
    // Eden is unused during full GC.
    savedFirstFieldsSpace_.start = reinterpret_cast<Oop*>(edenStart_);
    savedFirstFieldsSpace_.limit = reinterpret_cast<Oop*>(edenStart_ +
        (survivorStart_ - edenStart_));
    savedFirstFieldsSpace_.top = savedFirstFieldsSpace_.start;

    uint8_t* toFinger = oldSpaceStart_;  // Destination for next live object

    size_t deadCount = 0;
    size_t deadBytes = 0;
    size_t moveCount = 0;
    size_t stayCount = 0;

    ObjectScanner scanner(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = scanner.next()) {
        if (!obj->isMarked()) {
            deadCount++;
            deadBytes += obj->totalSize();
            continue;  // Dead — skip
        }

        size_t objSize = obj->totalSize();
        uint8_t* objAddr = reinterpret_cast<uint8_t*>(obj);
        bool isOverflow = obj->hasOverflowSlots();
        // Object start in memory (includes overflow word if present)
        uint8_t* objStart = isOverflow ? (objAddr - 8) : objAddr;
        // Where the header will be at the destination
        uint8_t* destHeaderPos = isOverflow ? (toFinger + 8) : toFinger;

        // Pinned objects don't move
        if (obj->isPinned()) {
            if (toFinger < objStart) {
                // Gap before pinned object — skip over it
                toFinger = objStart;
            }
            obj->setGrey(false);  // Ensure no stale grey — critical for savedFieldPtr sync
            toFinger += objSize;
            stayCount++;
            continue;
        }

        // Does this object actually need to move?
        if (destHeaderPos == objAddr) {
            // Already in place — no forwarding needed. Clear grey bit.
            obj->setGrey(false);
            toFinger += objSize;
            stayCount++;
            continue;
        }
        moveCount++;

        // Mobile object that needs to move: save first field, store forwarding address.
        // Every Spur object has at least 16 bytes (8-byte header + 8 bytes padding/data),
        // so we can always use the first word after the header for forwarding.
        {
            // Check if we have scratch space
            if (savedFirstFieldsSpace_.top >= savedFirstFieldsSpace_.limit) {
                return false;  // Overflow — need another pass
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
        }

        toFinger += objSize;
    }

    // Plan summary logged by fullGC caller

    return true;  // All objects planned in one pass
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

    // Update memory roots
    auto updateOop = [&resolveForward](Oop& oop) {
        oop = resolveForward(oop);
    };

    forEachMemoryRoot(updateOop);

    if (interpreter_) {
        interpreter_->forEachRoot(updateOop);
    }
}

void ObjectMemory::copyAndUnmark() {
    Oop* savedFieldPtr = savedFirstFieldsSpace_.start;
    uint8_t* toFinger = oldSpaceStart_;

    ObjectScanner scanner(oldSpaceStart_, oldSpaceFree_);
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
            obj->setMarked(false);
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
        }

        // Clear mark and grey on the (possibly moved) copy
        ObjectHeader* movedObj = reinterpret_cast<ObjectHeader*>(destHeaderPos);
        movedObj->setMarked(false);
        movedObj->setGrey(false);

        toFinger += objSize;
    }
    // gcCopyGeneration was already incremented at function start

    // Update oldSpaceFree_ to after the last live object
    oldSpaceFree_ = toFinger;
}

void ObjectMemory::rebuildFreeListAfterCompact() {
    clearFreeLists();

    // The gap between oldSpaceFree_ and oldSpaceEnd_ is one big free chunk
    size_t freeBytes = oldSpaceEnd_ - oldSpaceFree_;
    if (freeBytes >= 16) {
        // Zero the free area first (for clean scanning later)
        std::memset(oldSpaceFree_, 0, freeBytes);
    }
    // We don't need to create a free list entry for the trailing gap —
    // the bump pointer allocator already handles this via oldSpaceFree_.
    // Free lists will be populated when we switch to free-list-based allocation.
}

// ===== HEAP POINTER VERIFICATION =====

size_t ObjectMemory::verifyHeapPointers() {
    FILE* log = fopen("/tmp/iospharo-verify.log", "w");
    if (!log) return 0;

    // Pass 1: Build set of all valid object start addresses (both old space and perm space)
    std::unordered_set<uintptr_t> validAddrs;
    validAddrs.reserve(800000);
    ObjectScanner pass1(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = pass1.next()) {
        validAddrs.insert(reinterpret_cast<uintptr_t>(obj));
    }
    if (permSpaceStart_ && permSpaceEnd_ > permSpaceStart_) {
        ObjectScanner permScan(permSpaceStart_, permSpaceEnd_);
        while (ObjectHeader* obj = permScan.next()) {
            validAddrs.insert(reinterpret_cast<uintptr_t>(obj));
        }
    }
    fprintf(log, "[VERIFY] %zu valid objects in heap\n", validAddrs.size());

    // Identify classIndex 36: print the class name
    if (classTable_.size() > 36 && classTable_[36].isObject()) {
        Oop cls36 = classTable_[36];
        // Class name is typically in slot 5 or 6 (Symbol)
        ObjectHeader* clsHdr = cls36.asObjectPtr();
        if (clsHdr->slotCount() > 6) {
            Oop name = clsHdr->slotAt(5);  // Name slot in standard Pharo class layout
            if (name.isObject() && name.rawBits() > 0x10000) {
                ObjectHeader* nameHdr = name.asObjectPtr();
                if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                    std::string className((char*)nameHdr->bytes(), nameHdr->byteSize());
                    fprintf(log, "[VERIFY] classIndex 36 = '%s'\n", className.c_str());
                } else {
                    // Try slot 6
                    name = clsHdr->slotAt(6);
                    if (name.isObject() && name.rawBits() > 0x10000) {
                        nameHdr = name.asObjectPtr();
                        if (nameHdr->isBytesObject() && nameHdr->byteSize() < 100) {
                            std::string className((char*)nameHdr->bytes(), nameHdr->byteSize());
                            fprintf(log, "[VERIFY] classIndex 36 = '%s' (slot 6)\n", className.c_str());
                        }
                    }
                }
            }
        }
    }

    // Pass 2: Check every pointer slot in every pointer-format object
    size_t badPtrs = 0;
    size_t checkedPtrs = 0;
    ObjectScanner pass2(oldSpaceStart_, oldSpaceFree_);
    while (ObjectHeader* obj = pass2.next()) {
        uint8_t fmt = static_cast<uint8_t>(obj->format());
        size_t nSlots = obj->slotCount();

        // Determine which slots contain pointers
        size_t ptrSlots = 0;
        if (fmt <= 5) {
            ptrSlots = nSlots;
        } else if (fmt >= 24 && fmt <= 31) {
            if (nSlots > 0) {
                Oop hdr0 = obj->slotAt(0);
                if (hdr0.isSmallInteger()) {
                    size_t numLits = hdr0.asSmallInteger() & 0x7FFF;
                    ptrSlots = std::min(numLits + 1, nSlots);
                }
            }
        }

        for (size_t i = 0; i < ptrSlots; ++i) {
            Oop val = obj->slotAt(i);
            if (!val.isObject()) continue;
            if (val.rawBits() < 0x10000) continue;
            if (val.isNil()) continue;

            checkedPtrs++;
            uintptr_t addr = val.rawBits();

            if (validAddrs.find(addr) == validAddrs.end()) {
                badPtrs++;
                uintptr_t objAddr = reinterpret_cast<uintptr_t>(obj);

                // Check if the target address is past the heap boundary
                bool pastHeap = (addr >= reinterpret_cast<uintptr_t>(oldSpaceFree_));

                // Look for nearby valid object
                bool foundNear = false;
                for (int delta = -8; delta <= 8; delta++) {
                    if (delta == 0) continue;
                    uintptr_t nearAddr = addr + delta * 8;
                    if (validAddrs.find(nearAddr) != validAddrs.end()) {
                        int offsetBytes = (int)(addr - nearAddr);
                        ObjectHeader* nearObj = reinterpret_cast<ObjectHeader*>(nearAddr);
                        fprintf(log, "[BAD-PTR #%zu] obj@0x%llx cls=%u fmt=%d slot[%zu]=0x%llx "
                                "-> nearest valid @0x%llx (off by %+d) nearCls=%u nearFmt=%d nearSlots=%zu%s\n",
                                badPtrs, (unsigned long long)objAddr, obj->classIndex(),
                                fmt, i, (unsigned long long)addr,
                                (unsigned long long)nearAddr, offsetBytes,
                                nearObj->classIndex(), (int)nearObj->format(), nearObj->slotCount(),
                                pastHeap ? " [PAST-HEAP]" : "");
                        foundNear = true;
                        break;
                    }
                }
                if (!foundNear && badPtrs <= 100) {
                    // Read what's at the target address for diagnostic
                    uint64_t targetWord = 0;
                    if (addr >= reinterpret_cast<uintptr_t>(oldSpaceStart_) &&
                        addr + 8 <= reinterpret_cast<uintptr_t>(oldSpaceEnd_)) {
                        targetWord = *reinterpret_cast<uint64_t*>(addr);
                    }
                    fprintf(log, "[BAD-PTR #%zu] obj@0x%llx cls=%u fmt=%d slot[%zu]=0x%llx "
                            "-> NO nearby valid object targetWord=0x%llx%s\n",
                            badPtrs, (unsigned long long)objAddr, obj->classIndex(),
                            fmt, i, (unsigned long long)addr,
                            (unsigned long long)targetWord,
                            pastHeap ? " [PAST-HEAP]" : "");
                }
                // For the first 5 bad pointers, dump the containing object's context
                if (badPtrs <= 5) {
                    fprintf(log, "  container: cls=%u fmt=%d slots=%zu\n",
                            obj->classIndex(), fmt, nSlots);
                    // Dump first 6 slots (fixed fields for Context/MethodDict)
                    for (size_t s = 0; s < std::min(nSlots, (size_t)8); s++) {
                        Oop sv = obj->slotAt(s);
                        const char* desc = "";
                        if (sv.isSmallInteger()) desc = " (SmallInt)";
                        else if (sv.isNil()) desc = " (nil)";
                        else if (sv.isObject() && sv.rawBits() > 0x10000) {
                            ObjectHeader* svh = sv.asObjectPtr();
                            static char buf[128];
                            snprintf(buf, sizeof(buf), " (obj cls=%u fmt=%d slots=%zu)",
                                     svh->classIndex(), (int)svh->format(), svh->slotCount());
                            desc = buf;
                        }
                        fprintf(log, "  slot[%zu]=0x%llx%s\n", s,
                                (unsigned long long)sv.rawBits(), desc);
                    }
                }
            }
        }
    }

    fprintf(log, "[VERIFY] checked=%zu bad=%zu\n", checkedPtrs, badPtrs);
    fflush(log);
    fclose(log);

    fprintf(stderr, "[VERIFY] checked=%zu pointers, bad=%zu\n", checkedPtrs, badPtrs);
    return badPtrs;
}

} // namespace pharo
