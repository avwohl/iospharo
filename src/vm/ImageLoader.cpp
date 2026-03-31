/*
 * ImageLoader.cpp - Spur 64-bit Image File Loader Implementation
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Loads standard Pharo/Squeak Spur images. The image format is defined by
 * the Pharo project (https://pharo.org) and OpenSmalltalk-VM.
 * See THIRD_PARTY_LICENSES for upstream license details.
 */

#include "ImageLoader.hpp"
#include <cstring>
#include <algorithm>
#include <iostream>

namespace pharo {

// Spur 64-bit header field extraction
inline uint8_t extractFormat(uint64_t header) {
    return static_cast<uint8_t>((header >> 24) & 0x1F);
}

inline uint8_t extractNumSlots(uint64_t header) {
    return static_cast<uint8_t>((header >> 56) & 0xFF);
}

// ===== MAIN LOAD FUNCTION =====

LoadResult ImageLoader::load(const std::string& path, ObjectMemory& memory) {
    LoadResult result;

    // Open the image file
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        result.error = "Cannot open image file: " + path;
        return result;
    }

    // Step 1: Read and validate header
    if (!readHeader(file, result)) {
        return result;
    }

    // Step 2: Load heap data
    if (!loadHeapData(file, memory, result)) {
        return result;
    }

    // Step 3: Relocate pointers
    if (!relocatePointers(memory, result)) {
        return result;
    }

    // Step 4: Set up special objects
    if (!setupSpecialObjects(memory, result)) {
        return result;
    }

    // Step 5: Build class table
    if (!buildClassTable(memory, result)) {
        return result;
    }

    // Step 6: Cache class indices that need special GC handling
    memory.cacheGCClassIndices();

    result.success = true;
    return result;
}

// ===== HEADER READING =====

bool ImageLoader::readHeader(std::ifstream& file, LoadResult& result) {
    // Read the minimum header (first 64 bytes)
    file.read(reinterpret_cast<char*>(&header_), sizeof(SpurImageHeader));

    if (!file.good()) {
        result.error = "Failed to read image header";
        return false;
    }

    // Validate format
    switch (header_.imageFormat) {
        case static_cast<uint32_t>(ImageFormat::Spur64):
            result.format = ImageFormat::Spur64;
            break;
        case static_cast<uint32_t>(ImageFormat::Spur64Sista):
            result.format = ImageFormat::Spur64Sista;
            result.sistaV1 = true;
            break;
        default:
            result.error = "Unsupported image format: " +
                          std::to_string(header_.imageFormat);
            return false;
    }

    // Check for byte-swapped image (would need to swap all data)
    // For now, we only support native byte order
    if (header_.headerSize > 1024 || header_.headerSize < 64) {
        result.error = "Image may be byte-swapped (unsupported)";
        return false;
    }

    // Parse flags
    result.sistaV1 = (header_.imageHeaderFlags &
                      static_cast<uint64_t>(ImageFlags::SistaV1)) != 0;
    result.fullBlockClosures = (header_.imageHeaderFlags &
                                static_cast<uint64_t>(ImageFlags::FullBlockClosures)) != 0;

    // Parse screen size (packed as width << 16 | height, per Spur format)
    result.screenWidth = static_cast<uint32_t>((header_.screenSize >> 16) & 0xFFFF);
    result.screenHeight = static_cast<uint32_t>(header_.screenSize & 0xFFFF);

    result.heapSize = header_.imageBytes;

    // Seek to start of heap data
    file.seekg(header_.headerSize, std::ios::beg);
    if (!file.good()) {
        result.error = "Failed to seek to heap data";
        return false;
    }

    return true;
}

// ===== HEAP LOADING =====

bool ImageLoader::loadHeapData(std::ifstream& file, ObjectMemory& memory,
                               LoadResult& result) {
    loadedSize_ = header_.imageBytes;

    // Get the destination in old space
    loadedData_ = memory.oldSpaceStart();
    uint8_t* loadEnd = loadedData_ + loadedSize_;

    // Verify we have enough space
    if (loadEnd > memory.oldSpaceEnd()) {
        result.error = "Image too large for allocated memory";
        return false;
    }

    // Read the heap data directly into old space
    file.read(reinterpret_cast<char*>(loadedData_), loadedSize_);

    if (!file.good() && !file.eof()) {
        result.error = "Failed to read heap data";
        return false;
    }

    // Calculate relocation offset
    oldBase_ = header_.startOfMemory;
    newBase_ = reinterpret_cast<uint64_t>(loadedData_);

    // Update the free pointer
    memory.setOldSpaceFreePointer(loadEnd);

    return true;
}

// ===== POINTER RELOCATION =====

bool ImageLoader::relocatePointers(ObjectMemory& memory, LoadResult& result) {
    forEachObject([this](uint64_t* headerPtr, size_t size) {
        // The header itself doesn't contain pointers (it has class index, not oop)

        // Get object format from header
        uint64_t header = *headerPtr;
        uint8_t slotCountByte = extractNumSlots(header);
        uint8_t format = extractFormat(header);

        // Check for overflow header - mask top byte per standard Spur
        size_t slotCount;
        uint64_t* firstSlot;
        size_t headerSize = 8;  // Base header size
        if (slotCountByte == 255) {
            // Overflow: previous word has count in low 56 bits (top byte is 0xFF marker)
            uint64_t prevWord = *(headerPtr - 1);
            slotCount = static_cast<size_t>((prevWord << 8) >> 8);
            firstSlot = headerPtr + 1;
        } else {
            slotCount = slotCountByte;
            firstSlot = headerPtr + 1;
        }

        // Sanity check: slot count must fit within the object size
        if (size <= headerSize) {
            // Object too small for any slots
            return;  // Skip this object entirely
        }
        size_t maxSlots = (size - headerSize) / 8;
        if (slotCount > maxSlots) {
            slotCount = maxSlots;
        }

        // Only pointer objects have pointer fields to relocate
        // Formats 0-5 are pointer objects, 24-31 are compiled methods (mixed)
        // Format 9 (Indexable64) - hiddenRoots uses this and contains pointers!
        bool hasPointers = (format <= 5) || (format == 9);
        bool isCompiledMethod = (format >= 24 && format <= 31);

        if (hasPointers) {
            // All slots are pointers or SmallIntegers - relocate all
            for (size_t i = 0; i < slotCount; ++i) {
                firstSlot[i] = relocatePointer(firstSlot[i]);
            }
        } else if (isCompiledMethod) {
            // Compiled methods have header + literals followed by bytecodes
            // First convert the header SmallInteger to get correct numLiterals
            if (slotCount > 0) {
                // First, convert the method header (slot 0) from Spur format
                uint64_t oldHeader = firstSlot[0];
                firstSlot[0] = relocatePointer(oldHeader);  // Convert SmallInteger

                // Now decode numLiterals using OUR format (3-bit tag)
                int64_t headerValue = static_cast<int64_t>(firstSlot[0]) >> 3;
                size_t numLiterals = headerValue & 0x7FFF;

                // Relocate the literals (slots 1..numLiterals inclusive)
                size_t pointerSlots = std::min(numLiterals + 1, slotCount);
                for (size_t i = 1; i < pointerSlots; ++i) {
                    firstSlot[i] = relocatePointer(firstSlot[i]);
                }
            }
        }
        // Byte/word objects don't have pointers to relocate
    });

    return true;
}

// ===== SPECIAL OBJECTS =====

// Helper to relocate an object's slots given its oop (pointer to header)
// In this image, oops point to the HEADER; slots start 8 bytes after.
void ImageLoader::relocateObjectSlots(uint64_t* headerPtr) {
    uint64_t header = *headerPtr;
    uint8_t format = extractFormat(header);

    // Only relocate pointer objects (format 0-5)
    if (format > 5) return;

    uint8_t numSlots = extractNumSlots(header);
    size_t slotCount = numSlots;
    if (numSlots == 255) {
        // Overflow: previous word contains count. Mask off top byte (standard Spur).
        uint64_t rawWord = *(headerPtr - 1);
        slotCount = static_cast<size_t>((rawWord << 8) >> 8);
    }

    uint64_t* slots = headerPtr + 1;
    for (size_t i = 0; i < slotCount; ++i) {
        slots[i] = relocatePointer(slots[i]);
    }
}

bool ImageLoader::setupSpecialObjects(ObjectMemory& memory, LoadResult& result) {
    // The special objects array oop needs relocation too
    uint64_t relocatedSpecialOop = relocatePointer(header_.specialObjectsOop);

    // Convert to our Oop type
    Oop specialObjects = rawToOop(relocatedSpecialOop, memory);

    if (specialObjects.isNil() || !specialObjects.isObject()) {
        result.error = "Invalid special objects array";
        return false;
    }

    // NOTE: The special objects array slots were ALREADY relocated during step 3 (relocatePointers)
    // since the array is part of the heap. We should NOT relocate them again here.
    // The slots now contain addresses relative to newBase_, not oldBase_.

    // NOTE: All objects in the heap (including those pointed to by the special objects array)
    // were ALREADY relocated during step 3 (relocatePointers). We should NOT relocate them again.
    memory.setSpecialObjectsArray(specialObjects);
    memory.cacheSpecialObjects();

    return true;
}

// ===== CLASS TABLE =====

bool ImageLoader::buildClassTable(ObjectMemory& memory, LoadResult& result) {
    // In Spur 64-bit, the first five objects in old space are:
    //   1. nil (format 0, 0 slots)
    //   2. false (format 0, 0 slots)
    //   3. true (format 0, 0 slots)
    //   4. freeListsObj (format 9, 64 slots — free list heads)
    //   5. hiddenRootsObj / classTableRootObj (4096 page slots + 8 extra roots)
    //
    // hiddenRootsObj slots 0..4095 are class table page pointers (nil if unused).
    // Each page is an Array of 1024 class object pointers.
    // Class index N = page[N / 1024][N % 1024].
    // Slots 4096..4103 are extra roots (special objects, not class pages).

    uint8_t* heapStart = memory.oldSpaceStart();
    Oop nilObj = memory.specialObject(SpecialObjectIndex::NilObject);
    constexpr size_t PageSize = 1024;
    constexpr size_t MaxClassTablePages = 4096;
    size_t totalClasses = 0;

    // Walk objects from start of old space to find the 5th object (hiddenRootsObj).
    // In Spur 64-bit, minimum object size is 16 bytes (8 header + 8 body).
    // Objects with >254 slots have an overflow word before the header.
    auto objectAfter = [&](uint8_t* objPtr) -> uint8_t* {
        ObjectHeader* hdr = reinterpret_cast<ObjectHeader*>(objPtr);
        size_t size = hdr->totalSize();
        // Spur minimum object size is 16 bytes (header + at least 1 slot for forwarding)
        if (size < 16) size = 16;
        uint8_t* next = objPtr + size;
        // Check if the next position is an overflow word (the word after it has numSlots=255)
        if (next + 16 <= heapStart + loadedSize_) {
            uint64_t followingWord = *reinterpret_cast<uint64_t*>(next + 8);
            if (extractNumSlots(followingWord) == 255) {
                // next points to the overflow word; the real header is at next+8
                next += 8;
            }
        }
        return next;
    };

    // Find freeListsObj (4th) and hiddenRootsObj (5th) from start of old space
    // Objects: nil(1), false(2), true(3), freeListsObj(4), hiddenRootsObj(5)
    uint8_t* obj = heapStart;  // starts at nil
    uint8_t* freeListsPtr = nullptr;
    for (int i = 0; i < 4; i++) {
        obj = objectAfter(obj);
        if (obj >= heapStart + loadedSize_) {
            result.error = "Ran off end of heap walking to object " + std::to_string(i + 2);
            return false;
        }
        if (i == 2) freeListsPtr = obj;  // After 3 advances = 4th object = freeListsObj
    }
    // After 4 advances: obj = 5th object = hiddenRootsObj

    // Store freeListsObj and hiddenRootsObj as GC roots for image saving
    if (freeListsPtr) {
        memory.setFreeListsObj(memory.oopFromPointer(
            reinterpret_cast<ObjectHeader*>(freeListsPtr)));
    }
    memory.setHiddenRootsObj(memory.oopFromPointer(
        reinterpret_cast<ObjectHeader*>(obj)));

    ObjectHeader* hiddenRoots = reinterpret_cast<ObjectHeader*>(obj);
    size_t hrSlots = hiddenRoots->slotCount();

    if (hrSlots < MaxClassTablePages) {
        result.error = "hiddenRoots has only " + std::to_string(hrSlots)
                     + " slots, expected >= " + std::to_string(MaxClassTablePages);
        return false;
    }

    // Read class table pages from hiddenRoots slots 0..4095
    size_t numPages = 0;
    for (size_t pageNum = 0; pageNum < MaxClassTablePages; pageNum++) {
        Oop pageOop = hiddenRoots->slotAt(pageNum);

        // Skip nil page entries
        if (pageOop.rawBits() == 0 || pageOop.rawBits() == nilObj.rawBits() || !pageOop.isObject()) {
            continue;
        }

        // Validate the pointer is within our heap
        uint64_t pageAddr = pageOop.rawBits();
        if (pageAddr < newBase_ || pageAddr >= newBase_ + loadedSize_) {
            continue;
        }

        ObjectHeader* pageHdr = pageOop.asObjectPtr();
        size_t pageSlots = pageHdr->slotCount();
        numPages++;

        // Register page in C++ side structure so GC keeps it updated
        memory.setClassTablePage(pageNum, pageOop);

        // Each slot in the page is a class object pointer
        for (size_t i = 0; i < pageSlots && i < PageSize; i++) {
            Oop classOop = pageHdr->slotAt(i);

            if (classOop.rawBits() == 0 || classOop.rawBits() == nilObj.rawBits()) {
                continue;
            }
            if (!classOop.isObject()) {
                continue;
            }

            // Validate class entry points within the heap.
            // After relocation, all valid class pointers must be within
            // [newBase, newBase+loadedSize). Values outside this range
            // are raw data that happens to have tag bits 000 — not class pointers.
            uint64_t classAddr = classOop.rawBits();
            if (classAddr < newBase_ || classAddr >= newBase_ + loadedSize_) {
                // Nil out bad pointer so GC won't try to mark it
                pageHdr->slotAtPut(i, nilObj);
                continue;
            }

            uint32_t classIndex = static_cast<uint32_t>(pageNum * PageSize + i);
            memory.setClassAtIndex(classIndex, classOop);
            totalClasses++;
        }
    }

    return totalClasses > 0;
}

// ===== POINTER UTILITIES =====

bool ImageLoader::isObjectPointer(uint64_t bits) const {
    // Check if this looks like an object pointer:
    // - Bit 0 must be 0 (not an immediate)
    // - Must be 8-byte aligned (bits 0-2 in original are tags/space)
    // - For Spur, the pointer range is within the heap

    if (bits == 0) return false;  // nil
    if (bits & 1) return false;   // Immediate (SmallInteger, etc.)

    // In the saved image, pointers are relative to startOfMemory
    // and are 8-byte aligned within the heap
    uint64_t aligned = bits & ~7ULL;
    return aligned >= oldBase_ && aligned < (oldBase_ + loadedSize_);
}

uint64_t ImageLoader::relocatePointer(uint64_t oldOop) const {
    if (oldOop == 0) return 0;  // nil stays nil

    // Spur 64-bit immediate encoding: ALL immediates use 3-bit tags.
    //   SmallInteger: tag = 001, encoding = (value << 3) | 1
    //                 Same as our encoding - pass through unchanged.
    //   Character:    tag = 010, encoding = (codepoint << 3) | 2
    //                 Must convert to our encoding: (codepoint << 3) | 3
    //   SmallFloat:   tag = 100, encoding = (bits << 3) | 4
    //                 Must change tag from 100 to our 101.
    //   Object ptr:   tag = 000, 8-byte aligned address
    if (oldOop & 1) {
        // SmallInteger: Spur 64-bit uses (value << 3) | 1, same as our encoding
        return oldOop;
    }
    if ((oldOop & 7) == 2) {
        // Spur Character (tag 010): codepoint = raw >> 3 (3-bit tag)
        uint64_t codepoint = oldOop >> 3;
        return (codepoint << 3) | 0x3;  // Our CharacterTag = 011
    }
    if ((oldOop & 7) == 4) {
        // Spur SmallFloat (tag 100): change tag to our 101
        return (oldOop & ~7ULL) | 0x5;  // Our SmallFloatTag = 101
    }
    if ((oldOop & 7) != 0) {
        // Unknown immediate tag (6 = unused in Spur 64-bit)
        return oldOop;
    }

    // It's an object pointer (tag = 000, 8-byte aligned address)
    // Only relocate if it's within the old heap bounds
    if (oldOop < oldBase_ || oldOop >= oldBase_ + loadedSize_) {
        // Pointer outside old heap - could be special value, already relocated,
        // or from another segment. Don't relocate.
        return oldOop;
    }

    // Calculate new address
    uint64_t newAddr = oldOop - oldBase_ + newBase_;
    return newAddr;
}

Oop ImageLoader::rawToOop(uint64_t raw, ObjectMemory& memory) const {
    if (raw == 0) return Oop::nil();

    // After image loading, values should be in OUR encoding format
    // (relocatePointer already converted from Spur to ours).
    // Our encoding: SmallInteger = (value << 3) | 1, Character = (cp << 3) | 3,
    //               SmallFloat = (...) | 5, Object pointer = ... | 0
    if (raw & 1) {
        // SmallInteger (tag 001) or other odd immediate
        uint64_t tag3 = raw & 7;
        if (tag3 == 1) {
            int64_t value = static_cast<int64_t>(raw) >> 3;
            return Oop::fromSmallInteger(value);
        } else if (tag3 == 3) {
            // Our Character encoding (011)
            uint32_t codepoint = static_cast<uint32_t>((raw >> 3) & 0x3FFFFFFF);
            return Oop::fromCharacter(codepoint);
        } else if (tag3 == 5) {
            // Our SmallFloat encoding (101) - already correct after relocation
            // Payload is identical to Spur's, just different tag bits.
            return Oop::fromRawBits(raw);
        }
        // tag 7: treat as SmallInteger
        int64_t value = static_cast<int64_t>(raw) >> 3;
        return Oop::fromSmallInteger(value);
    }

    if ((raw & 7) != 0) {
        // Even non-zero tag that's not an object pointer
        // Could be an unrelocated Spur Character (tag 010) or SmallFloat (tag 100)
        if ((raw & 7) == 2) {
            // Spur Character (tag 010) - extract codepoint with 3-bit shift
            uint32_t codepoint = static_cast<uint32_t>(raw >> 3);
            return Oop::fromCharacter(codepoint);
        } else if ((raw & 7) == 4) {
            // Unrelocated Spur SmallFloat (tag 100) - just change tag to ours (101)
            // Payload encoding is identical between Spur and our format.
            return Oop::fromRawBits((raw & ~7ULL) | 5);
        }
        return Oop::fromSmallInteger(0);  // Unknown
    }

    // Object pointer (tag = 000)
    ObjectHeader* ptr = reinterpret_cast<ObjectHeader*>(raw);
    return memory.oopFromPointer(ptr);
}

// ===== OBJECT SIZE CALCULATION =====

size_t ImageLoader::objectSize(uint64_t* headerPtr) const {
    uint64_t header = *headerPtr;

    // Extract fields using CORRECT Spur layout
    uint8_t numSlots = extractNumSlots(header);
    uint8_t format = extractFormat(header);

    size_t slotCount;
    size_t headerSize = 8;  // Base header

    if (numSlots == 255) {
        // Overflow: previous word contains count. Mask off top byte (standard Spur).
        uint64_t rawWord = *(headerPtr - 1);
        slotCount = static_cast<size_t>((rawWord << 8) >> 8);
    } else {
        slotCount = numSlots;
    }

    // Calculate body size based on format
    size_t bodySize;

    if (format <= 5) {
        // Pointer objects: slotCount * 8 bytes
        bodySize = slotCount * 8;
    } else if (format == 9) {
        // 64-bit indexable
        bodySize = slotCount * 8;
    } else if (format >= 10 && format <= 11) {
        // 32-bit indexable (format 11 = odd count)
        bodySize = slotCount * 8;  // Storage is slot-aligned
    } else if (format >= 12 && format <= 15) {
        // 16-bit indexable
        bodySize = slotCount * 8;
    } else if (format >= 16 && format <= 23) {
        // 8-bit indexable (bytes)
        bodySize = slotCount * 8;
    } else if (format >= 24 && format <= 31) {
        // Compiled methods
        bodySize = slotCount * 8;
    } else {
        // Unknown format
        bodySize = slotCount * 8;
    }

    // Total size: minimum 16 bytes (Spur requires 2 words minimum for forwarding pointer)
    // Then align to 8 bytes
    size_t totalSize = headerSize + bodySize;
    if (totalSize < 16) totalSize = 16;
    return (totalSize + 7) & ~7ULL;
}

} // namespace pharo
