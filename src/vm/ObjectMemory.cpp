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
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <sys/mman.h>

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
        munmap(oldSpaceStart_, config.oldSpaceSize);
        permSpaceStart_ = nullptr;
        oldSpaceStart_ = nullptr;
        return false;
    }
    newSpaceEnd_ = newSpaceStart_ + config.newSpaceSize;

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
    size_t headerSize = sizeof(ObjectHeader);
    bool hasOverflow = slotCount >= 255;
    if (hasOverflow) {
        headerSize += sizeof(uint64_t);
    }

    size_t bodySize = slotCount * sizeof(Oop);
    size_t totalSize = headerSize + bodySize;
    totalSize = (totalSize + 7) & ~7ULL;

    // Allocate in old space (no generational GC — eden is reserved for compacting GC scratch)
    ObjectHeader* obj = allocateRaw(totalSize, Space::Old);

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

    bytesAllocated_ += totalSize;
    return oopFromPointer(obj);
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

    // Allocate in old space (no generational GC — eden is reserved for compacting GC scratch)
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
    return classAtIndex(header->classIndex());
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

    Oop symbolObj = allocateBytes(symbolClassIdx, name.size());
    if (symbolObj.isNil()) {
        return false;
    }

    ObjectHeader* symHdr = symbolObj.asObjectPtr();
    std::memcpy(symHdr->bytes(), name.c_str(), name.size());

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

    if (index >= header->slotCount()) return nilObject_;

    return header->slotAt(index);
}

void ObjectMemory::storePointer(size_t index, Oop obj, Oop value) {
    if (!obj.isObject()) return;
    ObjectHeader* header = obj.asObjectPtr();
    if (index >= header->slotCount()) return;

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
    // Selector is the penultimate literal (slot numLiterals - 1, 0-based from slot 1)
    Oop sel = fetchPointer(numLits - 1, method);
    std::string name = oopToString(sel);
    return name.empty() ? "?" : name;
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
    ObjectHeader* clsHdr = classObj.asObjectPtr();
    size_t clsSlots = clsHdr->slotCount();

    Oop nameOop;
    if (clsSlots == 6) {
        // Metaclass — get thisClass from slot 5, then name from slot 6
        Oop thisClass = clsHdr->slotAt(5);
        if (!thisClass.isObject()) return "?";
        ObjectHeader* tcHdr = thisClass.asObjectPtr();
        if (tcHdr->slotCount() < 7) return "?";
        nameOop = tcHdr->slotAt(6);
    } else if (clsSlots >= 7) {
        nameOop = clsHdr->slotAt(6);
    } else {
        return "?";
    }

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
    // Generational GC not implemented. All allocations go to old space.
    // Eden is reserved as scratch space for compacting GC's saved-first-fields.
    return fullGC();
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

    size_t freeAfter = oldSpaceEnd_ - oldSpaceFree_;

    // If less than 25% free after sweep, request compacting GC at next safe point
    size_t totalSpace = oldSpaceEnd_ - oldSpaceStart_;
    if (freeAfter < totalSpace / 4) {
        needsCompactGC_ = true;
    }

    gcCount_++;
    totalGCTime_ += ms;
}

GCResult ObjectMemory::fullGC(bool skipEphemerons) {
    auto start = std::chrono::steady_clock::now();
    GCResult result{0, 0, 0};

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
    size_t markedCount = markPhase(skipEphemerons);

    // Symbol class corruption check and stale pointer check disabled (verified clean)

    // 4. Plan + update + copy (compact)
    planCompactSavingForwarders();
    updatePointersAfterCompact();
    copyAndUnmark();

    // 5. Rebuild free list from gap
    rebuildFreeListAfterCompact();

    // Post-compaction stale pointer check (disabled — verified clean, too expensive for production)

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

    // Record compacted size for threshold-based GC triggering
    lastCompactedSize_ = oldSpaceFree_ - oldSpaceStart_;

    gcCount_++;
    totalGCTime_ += result.milliseconds;
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
            // Generational GC not implemented — eden is scratch for compacting GC.
            // Fall through to return nullptr.
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
            classTable_[i] = nilObject_;
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
    for (size_t i = 1; i < classTable_.size(); ++i) {
        size_t pageNum = i / PageSize;
        size_t slotNum = i % PageSize;

        if (pageNum >= classTablePages_.size()) break;
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

    // 1. Mark from memory roots (special objects — NOT class table entries).
    // In Spur, class table entries are NOT strong roots. Anonymous/transient
    // classes can be collected when no live object references them.
    forEachMemoryRoot([this](Oop& oop) {
        markAndTrace(oop);
    }, /* includeClassTable */ false);

    // 2. Mark from interpreter roots
    if (interpreter_) {
        interpreter_->forEachRoot([this](Oop& oop) {
            markAndTrace(oop);
        });
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
        if (!ephemeronList_.empty()) {
            while (markInactiveEphemerons()) {}
            fired = ephemeronList_.size();
            fireAllEphemerons();
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

    // 5b. Sweep the class table: nil entries for classes that were not marked.
    // This allows anonymous/transient classes to be collected.
    sweepClassTable();

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

    // Update pointers in NEW SPACE objects that reference old space.
    // Without this, young objects holding old space pointers become stale
    // after compaction moves old space objects.
    {
        int newSpaceUpdated = 0;
        auto scanNewSpaceRegion = [&](uint8_t* start, uint8_t* end, const char* label) {
            int objCount = 0;
            int updatedCount = 0;
            uint8_t* scan = start;
            while (scan + 8 <= end) {  // Need at least 8 bytes for header
                ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(scan);
                size_t ts = obj->totalSize();
                if (ts == 0 || ts > 0x10000000) break;  // Invalid — stop
                if (obj->classIndex() != 0) {  // Not free
                    objCount++;
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
        // Scan eden (edenStart_ to edenFree_)
        scanNewSpaceRegion(edenStart_, edenFree_, "eden");
        // Scan survivor space (survivorStart_ to newSpaceEnd_)
        scanNewSpaceRegion(survivorStart_, newSpaceEnd_, "survivor");
        (void)newSpaceUpdated;  // suppress unused warning
    }

    // Update memory roots
    auto updateOop = [&resolveForward](Oop& oop) {
        oop = resolveForward(oop);
    };

    forEachMemoryRoot(updateOop);

    if (interpreter_) {
        interpreter_->forEachRoot(updateOop);
    }

    // Note: hiddenRootsObj page pointer slots are NOT updated here.
    // They are format-9 slots (not traced by pointerSlotsOf), and at this point
    // hiddenRootsObj_ may point to its destination address (data not yet moved).
    // Instead, classTablePages_ (in forEachMemoryRoot) tracks page Oops and
    // syncClassTableToHeap writes them back to hiddenRoots before save.
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

} // namespace pharo
