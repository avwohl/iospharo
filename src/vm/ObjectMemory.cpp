/*
 * ObjectMemory.cpp - Heap Management Implementation
 */

#include "ObjectMemory.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <functional>

namespace pharo {

// ===== CONSTRUCTION / INITIALIZATION =====

ObjectMemory::ObjectMemory() = default;

ObjectMemory::~ObjectMemory() {
    // Free allocated memory regions
    if (permSpaceStart_) {
        std::free(permSpaceStart_);
    }
    if (oldSpaceStart_) {
        std::free(oldSpaceStart_);
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

    oldSpaceStart_ = static_cast<uint8_t*>(
        std::aligned_alloc(8, config.oldSpaceSize));
    if (!oldSpaceStart_) {
        std::free(permSpaceStart_);
        permSpaceStart_ = nullptr;
        return false;
    }
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

    // Split new space into eden and survivor
    size_t edenSize = (config.newSpaceSize * config.edenRatio) / 100;
    edenStart_ = newSpaceStart_;
    edenFree_ = edenStart_;
    survivorStart_ = newSpaceStart_ + edenSize;

    // Initialize class table
    classTable_.resize(config.classTableSize, Oop::nil());

    // Zero all memory (objects expect zero-initialized slots)
    std::memset(permSpaceStart_, 0, config.permSpaceSize);
    std::memset(oldSpaceStart_, 0, config.oldSpaceSize);
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
        if (!dictAllocLog) dictAllocLog = fopen("/tmp/dict_alloc.log", "w");
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
        if (!ctxAllocLog) ctxAllocLog = fopen("/tmp/context_alloc.log", "w");
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
        // Old space is full - try a full GC and retry once
        static int allocFailCount = 0;
        static int gcRetryCount = 0;
        allocFailCount++;

        if (gcRetryCount < 10) {
            gcRetryCount++;
            if (gcRetryCount <= 3) {
                std::cerr << "[ALLOC-GC #" << gcRetryCount << "] Old space full, triggering GC and retry...\n";
            }
            fullGC();
            obj = allocateRaw(totalSize, Space::Old);
            if (obj) {
                if (gcRetryCount <= 3) {
                    std::cerr << "[ALLOC-GC #" << gcRetryCount << "] GC freed space, allocation succeeded\n";
                }
                // Fall through to continue with allocation
            } else {
                // GC didn't help
                if (gcRetryCount <= 3) {
                    std::cerr << "[ALLOC-GC #" << gcRetryCount << "] GC didn't free enough space\n";
                }
            }
        }

        if (!obj) {
            if (allocFailCount <= 10) {
                std::cerr << "[ALLOC-FAIL #" << allocFailCount << "] allocateSlots failed in old space! classIdx="
                          << classIndex << " slots=" << slotCount << " totalSize=" << totalSize << "\n";
                std::cerr << "  oldSpaceFree_=" << std::hex << (uintptr_t)oldSpaceFree_
                          << " oldSpaceEnd_=" << (uintptr_t)oldSpaceEnd_ << std::dec << "\n";
            }
            return nilObject_;
        }
    }

    // Set up overflow word if needed
    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(obj);
        *overflow = slotCount;
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

    // Handle overflow
    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(obj);
        *overflow = slotCount;
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

    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(obj);
        *overflow = slotCount;
        obj = reinterpret_cast<ObjectHeader*>(overflow + 1);
    }

    initializeHeader(obj, classIndex, slotCount, ObjectFormat::Indexable64);
    std::memset(obj->bytes(), 0, slotCount * 8);

    bytesAllocated_ += totalSize;
    return oopFromPointer(obj);
}

Oop ObjectMemory::shallowCopy(Oop original) {
    if (!original.isObject()) {
        return original;  // Immediates are their own copies
    }

    ObjectHeader* src = original.asObjectPtr();
    size_t size = src->totalSize();

    // TEMPORARY FIX: Allocate directly in old space (see allocateSlots comment)
    ObjectHeader* copy = allocateRaw(size, Space::Old);
    if (!copy) return nilObject_;

    // Copy all bytes including header
    std::memcpy(copy, src, size);

    // Generate new identity hash
    copy->setIdentityHash(generateHash());

    // Clear GC flags
    copy->setMarked(false);
    copy->setRemembered(false);

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
        return specialObject(SpecialObjectIndex::ClassFloat);
    }
    if (!obj.isObject()) {
        return nilObject_;  // Return proper nil, not raw 0
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
                // We're stuck in a loop - log once
                if (sameClassIdxCount == 101) {
                    std::cerr << "[CLASSOF] STUCK IN LOOP on classIdx=" << classIdx
                              << " - loop detected, returning nil\n";
                }
                // Just return nil - caller should handle
                // Reset count periodically to allow recovery
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
    }
    return cls;
}

uint32_t ObjectMemory::registerClass(Oop classOop) {
    uint32_t index = nextClassIndex_++;
    if (index < classTable_.size()) {
        classTable_[index] = classOop;
    }
    return index;
}

uint32_t ObjectMemory::indexOfClass(Oop classOop) const {
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
    trueObject_ = specialObject(SpecialObjectIndex::TrueObject);
    falseObject_ = specialObject(SpecialObjectIndex::FalseObject);
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
    int numTemps = (headerBits >> 16) & 0xFF;
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
    return obj.space() == Space::New;
}

bool ObjectMemory::isOld(Oop obj) const {
    if (!obj.isObject()) return false;
    return obj.space() == Space::Old;
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
           (p >= oldSpaceStart_ && p < oldSpaceEnd_) ||
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
        size_t slots = header->slotCount();
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
    // For now, just do a scavenge
    return scavenge();
}

// Helper to iterate over all objects in old space
void ObjectMemory::forEachObjectInOldSpace(std::function<void(ObjectHeader*)> callback) {
    uint8_t* scan = oldSpaceStart_;
    size_t objCount = 0;
    while (scan < oldSpaceFree_) {
        // Skip zero padding
        while (scan < oldSpaceFree_) {
            uint64_t word = *reinterpret_cast<uint64_t*>(scan);
            if (word != 0) break;
            scan += 8;
        }
        if (scan >= oldSpaceFree_) break;

        ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(scan);
        size_t size = obj->totalSize();

        // Sanity check to avoid infinite loops
        if (size < 8 || size > 100 * 1024 * 1024) {
            std::cerr << "[GC-ERROR] Invalid object size " << size << " at " << (void*)scan << "\n";
            break;
        }

        callback(obj);
        objCount++;
        if (objCount % 500000 == 0) {
            std::cerr << "[GC-SCAN] Scanned " << objCount << " objects, at offset "
                      << (scan - oldSpaceStart_) / (1024*1024) << "MB\n";
            std::cerr.flush();
        }
        scan += size;
    }
}

GCResult ObjectMemory::fullGC() {
    auto start = std::chrono::steady_clock::now();
    GCResult result{0, 0, 0};

    static int gcCallCount = 0;
    gcCallCount++;

    // GC is too slow currently (minutes for 3M objects with std::function overhead)
    // TODO: Optimize GC with inline iteration instead of callbacks
    if (gcCallCount <= 3) {
        std::cerr << "[FULL-GC #" << gcCallCount << "] GC disabled (too slow), using 1GB heap instead\n";
    }

    auto end = std::chrono::steady_clock::now();
    result.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();
    gcCount_++;
    totalGCTime_ += result.milliseconds;
    return result;

#if 0  // Disabled until optimized
    std::cerr << "[FULL-GC #" << gcCallCount << "] Starting GC...\n";
    std::cerr.flush();

    // === PHASE 1: Clear all marks ===
    size_t totalObjects = 0;
    forEachObjectInOldSpace([&](ObjectHeader* obj) {
        obj->setMarked(false);
        totalObjects++;
    });
    std::cerr << "[FULL-GC #" << gcCallCount << "] Phase 1 done: cleared " << totalObjects << " objects\n";
    std::cerr.flush();

    // === PHASE 2: Mark from roots ===
    std::vector<Oop> worklist;

    // Add special objects to worklist
    if (!specialObjectsArray_.isNil() && specialObjectsArray_.isObject()) {
        worklist.push_back(specialObjectsArray_);
    }

    // Add class table entries (only non-nil ones)
    size_t classCount = 0;
    for (size_t i = 1; i < classTable_.size(); i++) {
        Oop cls = classTable_[i];
        if (!cls.isNil() && cls.isObject()) {
            worklist.push_back(cls);
            classCount++;
        }
    }
    std::cerr << "[FULL-GC] Added " << classCount << " classes to worklist\n";
    std::cerr.flush();

    // Add nil, true, false
    if (nilObject_.isObject()) worklist.push_back(nilObject_);
    if (trueObject_.isObject()) worklist.push_back(trueObject_);
    if (falseObject_.isObject()) worklist.push_back(falseObject_);

    // Add explicit roots (interpreter stack, etc.)
    for (Oop* root : roots_) {
        if (root && root->isObject()) {
            worklist.push_back(*root);
        }
    }
    std::cerr << "[FULL-GC] Worklist size: " << worklist.size() << "\n";
    std::cerr.flush();

    // Process worklist (mark and trace)
    size_t markedCount = 0;
    size_t iterations = 0;
    std::cerr << "[FULL-GC] Starting mark loop...\n";
    std::cerr.flush();
    while (!worklist.empty()) {
        iterations++;
        if (iterations % 500000 == 0) {
            std::cerr << "[FULL-GC] Mark: " << iterations << " marked: " << markedCount << " wl: " << worklist.size() << "\n";
            std::cerr.flush();
        }
        Oop current = worklist.back();
        worklist.pop_back();

        if (!current.isObject()) continue;

        ObjectHeader* obj = current.asObjectPtr();
        if (!obj) continue;
        if (obj->isMarked()) continue;  // Already processed

        obj->setMarked(true);
        markedCount++;

        // Trace pointer slots
        ObjectFormat fmt = obj->format();
        if (fmt <= ObjectFormat::WeakWithFixed) {
            // Has pointer slots
            size_t slots = obj->slotCount();
            Oop* slotPtr = reinterpret_cast<Oop*>(obj->bytes());
            for (size_t i = 0; i < slots; i++) {
                Oop slot = slotPtr[i];
                if (slot.isObject()) {
                    ObjectHeader* slotObj = slot.asObjectPtr();
                    if (slotObj && !slotObj->isMarked()) {
                        worklist.push_back(slot);
                    }
                }
            }
        }
    }

    std::cerr << "[FULL-GC] Phase 2 done: marked " << markedCount << " objects\n";
    std::cerr.flush();

    // === PHASE 3: Compute forwarding addresses ===
    // We'll compact live objects toward the start of old space
    uint8_t* writePtr = oldSpaceStart_;
    std::unordered_map<ObjectHeader*, ObjectHeader*> forwarding;

    forEachObjectInOldSpace([&](ObjectHeader* obj) {
        if (obj->isMarked()) {
            size_t size = obj->totalSize();
            ObjectHeader* newAddr = reinterpret_cast<ObjectHeader*>(writePtr);
            if (newAddr != obj) {
                forwarding[obj] = newAddr;
            }
            writePtr += size;
        }
    });

    size_t garbageObjects = totalObjects - markedCount;
    size_t reclaimedBytes = (oldSpaceFree_ - oldSpaceStart_) - (writePtr - oldSpaceStart_);

    if (gcCallCount <= 3) {
        std::cerr << "[FULL-GC #" << gcCallCount << "] objects=" << totalObjects
                  << " marked=" << markedCount << " garbage=" << garbageObjects
                  << " reclaimed=" << (reclaimedBytes / 1024) << "KB\n";
    }

    // If nothing to reclaim, skip expensive pointer updates and moves
    if (forwarding.empty()) {
        result.bytesReclaimed = 0;
        result.objectsMoved = 0;
        goto finish;
    }

    // === PHASE 4: Update pointers in all marked objects ===
    forEachObjectInOldSpace([&](ObjectHeader* obj) {
        if (!obj->isMarked()) return;

        ObjectFormat fmt = obj->format();
        if (fmt <= ObjectFormat::WeakWithFixed) {
            size_t slots = obj->slotCount();
            Oop* slotPtr = reinterpret_cast<Oop*>(obj->bytes());
            for (size_t i = 0; i < slots; i++) {
                if (slotPtr[i].isObject()) {
                    ObjectHeader* oldPtr = slotPtr[i].asObjectPtr();
                    auto it = forwarding.find(oldPtr);
                    if (it != forwarding.end()) {
                        slotPtr[i] = Oop::fromObject(it->second);
                    }
                }
            }
        }
    });

    // Update special objects and class table
    if (specialObjectsArray_.isObject()) {
        auto it = forwarding.find(specialObjectsArray_.asObjectPtr());
        if (it != forwarding.end()) {
            specialObjectsArray_ = Oop::fromObject(it->second);
        }
    }
    for (size_t i = 1; i < classTable_.size(); i++) {
        if (classTable_[i].isObject()) {
            auto it = forwarding.find(classTable_[i].asObjectPtr());
            if (it != forwarding.end()) {
                classTable_[i] = Oop::fromObject(it->second);
            }
        }
    }
    if (nilObject_.isObject()) {
        auto it = forwarding.find(nilObject_.asObjectPtr());
        if (it != forwarding.end()) nilObject_ = Oop::fromObject(it->second);
    }
    if (trueObject_.isObject()) {
        auto it = forwarding.find(trueObject_.asObjectPtr());
        if (it != forwarding.end()) trueObject_ = Oop::fromObject(it->second);
    }
    if (falseObject_.isObject()) {
        auto it = forwarding.find(falseObject_.asObjectPtr());
        if (it != forwarding.end()) falseObject_ = Oop::fromObject(it->second);
    }

    // Update registered roots
    for (Oop* root : roots_) {
        if (root && root->isObject()) {
            auto it = forwarding.find(root->asObjectPtr());
            if (it != forwarding.end()) {
                *root = Oop::fromObject(it->second);
            }
        }
    }

    // === PHASE 5: Compact - slide objects to their forwarding addresses ===
    // Must iterate in address order and copy carefully to avoid overwriting
    writePtr = oldSpaceStart_;
    forEachObjectInOldSpace([&](ObjectHeader* obj) {
        if (obj->isMarked()) {
            size_t size = obj->totalSize();
            ObjectHeader* newAddr = reinterpret_cast<ObjectHeader*>(writePtr);
            if (newAddr != obj) {
                std::memmove(newAddr, obj, size);
            }
            newAddr->setMarked(false);  // Clear mark for next GC
            writePtr += size;
        }
    });

    result.bytesReclaimed = reclaimedBytes;
    result.objectsMoved = forwarding.size();
    oldSpaceFree_ = writePtr;

finish:
    auto end = std::chrono::steady_clock::now();
    result.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    gcCount_++;
    totalGCTime_ += result.milliseconds;

    return result;
#endif  // Disabled GC code
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

            // Check for overflow header (next word has numSlots=255)
            uint64_t* headerPtr = wordPtr;
            bool hasOverflow = false;
            if (scan + 8 < end) {
                uint64_t nextWord = *(wordPtr + 1);
                if ((nextWord & 0xFF) == 255) {
                    uint64_t overflowCount = word;
                    size_t remaining = end - scan;
                    size_t neededSize = 8 + overflowCount * 8 + 8;

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

    switch (space) {
        case Space::Perm:
            // Permanent space not supported for new allocations
            return nullptr;

        case Space::Old:
            if (oldSpaceFree_ + size > oldSpaceEnd_) {
                static int oldSpaceFailCount = 0;
                oldSpaceFailCount++;
                if (oldSpaceFailCount <= 5) {
                    std::cerr << "[OLD-SPACE-FAIL #" << oldSpaceFailCount << "] oldSpaceFree_=" << std::hex
                              << (uintptr_t)oldSpaceFree_ << " oldSpaceEnd_=" << (uintptr_t)oldSpaceEnd_
                              << " size=" << std::dec << size << "\n";
                }
                return nullptr;
            }
            {
                ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(oldSpaceFree_);
                oldSpaceFree_ += size;
                return obj;
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
        obj.asObjectPtr()->setRemembered(true);
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

            // Check for overflow header
            uint64_t* headerPtr = wordPtr;
            if (scan + 8 < end) {
                uint64_t nextWord = *(wordPtr + 1);
                if ((nextWord & 0xFF) == 255) {
                    uint64_t overflowCount = word;
                    size_t remaining = end - scan;
                    size_t neededSize = 8 + overflowCount * 8 + 8;

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

            // Check for overflow header
            uint64_t* headerPtr = wordPtr;
            if (scan + 8 < end) {
                uint64_t nextWord = *(wordPtr + 1);
                if ((nextWord & 0xFF) == 255) {
                    uint64_t overflowCount = word;
                    size_t remaining = end - scan;
                    size_t neededSize = 8 + overflowCount * 8 + 8;

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

} // namespace pharo
