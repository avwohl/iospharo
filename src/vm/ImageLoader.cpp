/*
 * ImageLoader.cpp - Spur 64-bit Image File Loader Implementation
 */

#include "ImageLoader.hpp"
#include <cstring>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <map>
#include <set>

namespace pharo {

// ===== SPUR 64-BIT HEADER FIELD EXTRACTION =====
// These must match the layout in ObjectHeader.hpp
// classIndex: bits 0-21, format: bits 24-28, hash: bits 32-53, numSlots: bits 56-63

inline uint32_t extractClassIndex(uint64_t header) {
    return static_cast<uint32_t>(header & 0x3FFFFF);
}

inline uint8_t extractFormat(uint64_t header) {
    return static_cast<uint8_t>((header >> 24) & 0x1F);
}

inline uint32_t extractHash(uint64_t header) {
    return static_cast<uint32_t>((header >> 32) & 0x3FFFFF);
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
        std::cerr << "[ImageLoader] " << result.error << "\n";
        return result;
    }

    // Step 1: Read and validate header
    if (!readHeader(file, result)) {
        std::cerr << "[ImageLoader] Header read failed: " << result.error << "\n";
        return result;
    }

    // Step 2: Load heap data
    if (!loadHeapData(file, memory, result)) {
        std::cerr << "[ImageLoader] Heap load failed: " << result.error << "\n";
        return result;
    }

    // Step 3: Relocate pointers
    if (!relocatePointers(memory, result)) {
        std::cerr << "[ImageLoader] Relocate failed: " << result.error << "\n";
        return result;
    }

    // Step 4: Set up special objects
    if (!setupSpecialObjects(memory, result)) {
        std::cerr << "[ImageLoader] Special objects failed: " << result.error << "\n";
        return result;
    }

    // Step 5: Build class table
    if (!buildClassTable(memory, result)) {
        std::cerr << "[ImageLoader] Class table failed: " << result.error << "\n";
        return result;
    }

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

    // Parse screen size
    result.screenWidth = static_cast<uint32_t>(header_.screenSize >> 32);
    result.screenHeight = static_cast<uint32_t>(header_.screenSize & 0xFFFFFFFF);

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

    // DEBUG: Check specific method header at known offset
    // File offset 0x29760 = heap offset 0x296e0 (after 128-byte header)
    size_t testOff = 0x296e0;
    if (testOff + 8 <= loadedSize_) {
        uint64_t val = *reinterpret_cast<uint64_t*>(loadedData_ + testOff);
        // std::cerr << "[DEBUG] After file.read: offset 0x" << std::hex << testOff
                  // << " value=0x" << val << std::dec << std::endl;
        // Also print the raw bytes
        uint8_t* bytes = loadedData_ + testOff;
        // std::cerr << "[DEBUG] Raw bytes: ";
        for (int i = 0; i < 8; i++) {
            // if (bytes[i] < 16) std::cerr << "0";
            // std::cerr << std::hex << (int)bytes[i] << " ";
        }
        // std::cerr << std::dec << std::endl;
    }

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
    // We need to adjust every object pointer in the heap.
    // An object pointer has bit 0 = 0 and points within the heap.

    size_t objectCount = 0;
    size_t pointerCount = 0;
    size_t relocatedCount = 0;

    // First, verify the special objects array structure directly
    // Special objects array header should be at specArrayOffset
    size_t specArrayOff = header_.specialObjectsOop - header_.startOfMemory;
    // std::cerr << "[DEBUG] Verifying special objects at raw offset 0x" << std::hex << specArrayOff << std::dec << std::endl;
    uint64_t* specRaw = reinterpret_cast<uint64_t*>(loadedData_ + specArrayOff);
    uint64_t specHdr = specRaw[0];
    // std::cerr << "  Header: 0x" << std::hex << specHdr << std::dec << std::endl;
    // std::cerr << "  slot count = " << (int)extractNumSlots(specHdr) << std::endl;
    // std::cerr << "  format = " << (int)extractFormat(specHdr) << std::endl;
    // std::cerr << "  classIndex = " << extractClassIndex(specHdr) << std::endl;

    // Dump scheduler association location
    uint64_t schedAssocOop = specRaw[4];  // slot 3 = index 4 (after header at [0])
    size_t schedAssocOff = (schedAssocOop & ~7ULL) - header_.startOfMemory;
    // std::cerr << "  Slot 3 (scheduler): 0x" << std::hex << schedAssocOop
              // << " -> raw offset 0x" << schedAssocOff << std::dec << std::endl;

    // Now dump the scheduler association object directly
    uint64_t* schedRaw = reinterpret_cast<uint64_t*>(loadedData_ + schedAssocOff);
    // std::cerr << "[DEBUG] Scheduler association at offset 0x" << std::hex << schedAssocOff << std::dec << ":" << std::endl;
    for (int i = 0; i < 5; i++) {
        uint64_t val = schedRaw[i];
        // std::cerr << "  [" << i << "]: 0x" << std::hex << val;
        if (i == 0) {
            // std::cerr << " (header: slots=" << std::dec << (int)extractNumSlots(val)
                      // << " fmt=" << (int)extractFormat(val)
                      // << " cls=" << extractClassIndex(val) << ")";
        }
        // std::cerr << std::dec << std::endl;
    }

    // Also check what's at the ProcessScheduler location (slot 1 of association = slot 2 after header)
    uint64_t psOop = schedRaw[2];  // header at [0], key at [1], value at [2]
    // std::cerr << "[DEBUG] ProcessScheduler oop from association: 0x" << std::hex << psOop << std::endl;
    if ((psOop & 1) == 0 && psOop != 0) {
        size_t psOff = (psOop & ~7ULL) - header_.startOfMemory;
        // std::cerr << "  -> raw offset 0x" << psOff << std::dec << std::endl;
        uint64_t* psRaw = reinterpret_cast<uint64_t*>(loadedData_ + psOff);
        // std::cerr << "  Header: 0x" << std::hex << psRaw[0] << std::dec << std::endl;
        // std::cerr << "  (slots=" << (int)extractNumSlots(psRaw[0])
                  // << " fmt=" << (int)extractFormat(psRaw[0])
                  // << " cls=" << extractClassIndex(psRaw[0]) << ")" << std::endl;
    }

    // Also dump where forEachObject lands on bad header
    // std::cerr << "[DEBUG] Raw bytes at 0x6da80-0x6db20 (where bad object starts):" << std::endl;
    uint64_t* rawPtr = reinterpret_cast<uint64_t*>(loadedData_ + 0x6da80);
    for (int i = 0; i < 20; i++) {
        uint64_t val = rawPtr[i];
        size_t off = 0x6da80 + i*8;
        // std::cerr << "  [0x" << std::hex << off << "]: 0x" << val;
        // Try to print as ASCII if it looks like text
        char buf[9] = {0};
        for (int j = 0; j < 8; j++) buf[j] = ((val >> (j*8)) & 0xFF);
        bool isAscii = true;
        for (int j = 0; j < 8; j++) {
            if (buf[j] != 0 && (buf[j] < 32 || buf[j] > 126)) isAscii = false;
        }
        // if (isAscii && buf[0] != 0) std::cerr << " \"" << buf << "\"";
        // if (looksLikeValidHeader(val)) std::cerr << " [HEADER?]";
        // std::cerr << std::dec << std::endl;
    }

    // Debug: dump bytes at scheduler association key/value locations
    // Key (Symbol #Processor) should be at 0x82760, value (ProcessScheduler) at 0x82778
    // std::cerr << "[DEBUG] Scheduler association key area (0x82750-0x82790):" << std::endl;
    rawPtr = reinterpret_cast<uint64_t*>(loadedData_ + 0x82750);
    for (int i = 0; i < 8; i++) {
        uint64_t val = rawPtr[i];
        size_t off = 0x82750 + i*8;
        // std::cerr << "  [0x" << std::hex << off << "]: 0x" << val;
        // Check if it looks like a pointer to old space
        if ((val & ~7ULL) >= header_.startOfMemory &&
            (val & ~7ULL) < header_.startOfMemory + loadedSize_ &&
            (val & 1) == 0) {
            // std::cerr << " (oop off=" << ((val & ~7ULL) - header_.startOfMemory) << ")";
        }
        // Try to print as ASCII if it looks like text
        char buf[9] = {0};
        for (int j = 0; j < 8; j++) buf[j] = ((val >> (j*8)) & 0xFF);
        bool isAscii = true;
        for (int j = 0; j < 8; j++) {
            if (buf[j] != 0 && (buf[j] < 32 || buf[j] > 126)) isAscii = false;
        }
        // if (isAscii && buf[0] != 0) std::cerr << " \"" << buf << "\"";
        // std::cerr << std::dec << std::endl;
    }

    // Scheduler association calculation
    // Note: In this image, oops point to header, slots are 8 bytes after
    size_t specialArrayOffset = header_.specialObjectsOop - header_.startOfMemory;
    uint64_t* specObjArray = reinterpret_cast<uint64_t*>(loadedData_ + specialArrayOffset);
    uint64_t schedulerAssocPtr = specObjArray[4];  // Slot 3 (oop points to header)
    size_t schedulerAssocOffset = schedulerAssocPtr - header_.startOfMemory;
    // std::cerr << "[DEBUG] Scheduler header at offset 0x" << std::hex << schedulerAssocOffset << std::dec << std::endl;

    // Track if we visit the scheduler association
    bool visitedSchedulerAssoc = false;
    size_t schedulerOffset = schedulerAssocOffset;  // forEachObject visits by header offset

    // Track if we visit the Float values array (at offset 0x30971e8)
    static bool visitedFloatValuesArray = false;
    static size_t maxOffsetSeen = 0;

    forEachObject([this, &objectCount, &pointerCount, &relocatedCount, &visitedSchedulerAssoc, schedulerOffset](uint64_t* headerPtr, size_t size) {
        objectCount++;
        size_t offset = reinterpret_cast<uint8_t*>(headerPtr) - loadedData_;

        // Track maximum offset reached
        if (offset > maxOffsetSeen) maxOffsetSeen = offset;

        // Track Float values array visit (for debugging)
        if (offset == 0x30971e8) {
            visitedFloatValuesArray = true;
        }

        // Debug tracing for objects near Float values array (disabled - keep for future debugging)
        // if (offset >= 0x3095000 && offset <= 0x3098000) { ... }

        // Check if we visit the scheduler association
        if (offset == schedulerOffset) {
            visitedSchedulerAssoc = true;
            uint64_t header = *headerPtr;
            // std::cerr << "[DEBUG] Visiting scheduler at offset 0x" << std::hex << offset
                      // << " header=0x" << header << " format=" << std::dec << (int)extractFormat(header)
                      // << " size=" << size << std::endl;
        }

        // Also check if we SKIP OVER the scheduler
        if (offset < schedulerOffset && offset + size > schedulerOffset) {
            // std::cerr << "[DEBUG] SKIPPING OVER scheduler! Object at 0x" << std::hex << offset
                      // << " size=" << std::dec << size << " would end at 0x" << std::hex << (offset + size)
                      // << " schedulerOffset=0x" << schedulerOffset
                      // << std::dec << std::endl;
        }

        // Print objects from 0x6c000 to 0x6e000 to trace what's happening
        if (offset >= 0x6c000 && offset < 0x6e000) {
            uint64_t header = *headerPtr;
            // std::cerr << "[TRACE] off=0x" << std::hex << offset
                      // << " hdr=0x" << header
                      // << " slots=" << std::dec << (int)extractNumSlots(header)
                      // << " fmt=" << (int)extractFormat(header)
                      // << " cls=" << extractClassIndex(header)
                      // << " sz=" << size
                      // << " end=0x" << std::hex << (offset + size) << std::dec << std::endl;
        }

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
                pointerCount++;
                uint64_t oldValue = firstSlot[i];
                // relocatePointer handles both object pointers AND SmallIntegers
                uint64_t newValue = relocatePointer(oldValue);
                // Debug tracing for specific pointers (disabled)
                // if (oldValue == 0x1000044c070ULL) { ... }
                firstSlot[i] = newValue;
                if (oldValue != newValue) relocatedCount++;
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
                    pointerCount++;
                    uint64_t oldValue = firstSlot[i];
                    firstSlot[i] = relocatePointer(oldValue);
                    if (oldValue != firstSlot[i]) relocatedCount++;
                }
            }
        }
        // Byte/word objects don't have pointers to relocate
    });

    // Relocation summary (reduced verbosity)
    // std::cerr << "[DEBUG] Relocation: " << objectCount << " objects, "
    //           << relocatedCount << " pointers relocated" << std::endl;

    // DEBUG: Check the same offset after relocation
    size_t testOff = 0x296e0;
    if (testOff + 8 <= loadedSize_) {
        uint64_t val = *reinterpret_cast<uint64_t*>(loadedData_ + testOff);
        // std::cerr << "[DEBUG] After relocation: offset 0x" << std::hex << testOff
                  // << " value=0x" << val << std::dec << std::endl;
    }

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

    // Slots are after the header
    uint64_t* slots = headerPtr + 1;
    for (size_t i = 0; i < slotCount; ++i) {
        uint64_t oldValue = slots[i];
        // relocatePointer handles both object pointers and SmallIntegers
        slots[i] = relocatePointer(oldValue);
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

    // DEBUG: Check the test offset after special objects setup
    size_t testOff = 0x296e0;
    if (testOff + 8 <= loadedSize_) {
        uint64_t val = *reinterpret_cast<uint64_t*>(loadedData_ + testOff);
        // std::cerr << "[DEBUG] After setupSpecialObjects: offset 0x" << std::hex << testOff
                  // << " value=0x" << val << std::dec << std::endl;
    }

    return true;
}

// ===== CLASS TABLE =====

bool ImageLoader::buildClassTable(ObjectMemory& memory, LoadResult& result) {
    // In Pharo 12 Spur 64-bit, the class table is stored as follows:
    //
    // Layout at start of old space:
    //   offset 0x0:   nil object (format 0, 0 slots, 16 bytes)
    //   offset 0x10:  false object (format 0, 0 slots, 16 bytes)
    //   offset 0x20:  true object (format 0, 0 slots, 16 bytes)
    //   offset 0x30:  hiddenRoots (format 9, 64 slots with nil, 520 bytes)
    //   offset 0x238: classTablePages array (format 2, holds page pointers)
    //
    // NOTE: In Pharo 12, hiddenRoots slots are all nil. The class table
    // pages array is stored RIGHT AFTER hiddenRoots, not pointed to by slot 0.
    //
    // Each class table page is an Array of up to 1024 class object pointers.
    // Class index N is found at: classTablePages[N / 1024][N % 1024]

    uint8_t* heapStart = memory.oldSpaceStart();
    Oop nilObj = memory.specialObject(SpecialObjectIndex::NilObject);
    constexpr size_t PageSize = 1024;
    size_t totalClasses = 0;

    // In Pharo 12, hiddenRoots is at offset 0x30 with 64 slots (all nil)
    // hiddenRoots size = 8 (header) + 64*8 (slots) = 520 bytes
    // So class table pages array starts at 0x30 + 520 = 0x238
    constexpr size_t hiddenRootsOffset = 0x30;
    constexpr size_t hiddenRootsSize = 8 + 64 * 8;  // header + slots
    constexpr size_t classTablePagesOffset = hiddenRootsOffset + hiddenRootsSize;  // 0x238

    // The structure at 0x238 has two 8-byte headers, then the page pointers start at 0x248
    // Skip the two headers and read page pointers directly
    constexpr size_t pagePointersOffset = classTablePagesOffset + 16;  // 0x248

    // Read page pointers (they are old-space addresses that need relocation)
    uint64_t* pagePointers = reinterpret_cast<uint64_t*>(heapStart + pagePointersOffset);

    // Count how many non-zero page pointers we have
    size_t numPages = 0;
    for (size_t i = 0; i < 100; i++) {  // Max 100 pages
        uint64_t ptr = pagePointers[i];
        if (ptr == 0 || (ptr & 7) != 0) {  // Zero or not aligned = end of pages
            break;
        }
        numPages++;
    }
    std::cerr << "[CLASS-TABLE] Found " << numPages << " class table pages\n";

    // Iterate through each page
    for (size_t pageNum = 0; pageNum < numPages; pageNum++) {
        uint64_t pageAddr = pagePointers[pageNum];

        // Validate the pointer is within our heap
        if (pageAddr < newBase_ || pageAddr >= newBase_ + loadedSize_) {
            continue;
        }

        // Get the page array header
        ObjectHeader* pageHdr = reinterpret_cast<ObjectHeader*>(pageAddr);
        size_t pageSlots = pageHdr->slotCount();

        // Each slot in the page is a class object pointer
        for (size_t i = 0; i < pageSlots && i < PageSize; i++) {
            Oop classOop = pageHdr->slotAt(i);

            // Skip nil entries (both raw 0 and the actual nil object)
            if (classOop.rawBits() == 0 || classOop.rawBits() == nilObj.rawBits()) {
                continue;
            }

            // Skip non-object entries (SmallIntegers shouldn't be in class table)
            if (!classOop.isObject()) {
                continue;
            }

            uint32_t classIndex = static_cast<uint32_t>(pageNum * PageSize + i);
            memory.setClassAtIndex(classIndex, classOop);
            totalClasses++;

            // Debug: log class 3075 (UndefinedObject) registration
            if (classIndex == 3075) {
                std::cerr << "[CLASS-TABLE] Registered UndefinedObject at index 3075: 0x"
                          << std::hex << classOop.rawBits() << std::dec << "\n";
            }
        }
    }

    std::cerr << "[CLASS-TABLE] Registered " << totalClasses << " classes from pages\n";

    // Debug: check if key classes are registered
    Oop class3075 = memory.classAtIndex(3075);
    std::cerr << "[CLASS-TABLE] Class 3075 (UndefinedObject): "
              << (class3075.isNil() ? "NIL" : "registered")
              << " 0x" << std::hex << class3075.rawBits() << std::dec << "\n";

    // Check class 1 (Association)
    Oop class1 = memory.classAtIndex(1);
    std::cerr << "[CLASS-TABLE] Class 1: "
              << (class1.isNil() ? "NIL" : (class1.rawBits() == memory.nil().rawBits() ? "NIL-OBJ" : "registered"))
              << " 0x" << std::hex << class1.rawBits() << std::dec << "\n";

    if (totalClasses > 0) {
        return true;
    }

fallback_scan:
    // In Spur, the layout at start of old space is:
    //   offset 0x0:  nil object (format 0, 0 slots)
    //   offset 0x10: false object (format 0, 0 slots)
    //   offset 0x20: true object (format 0, 0 slots)
    //   offset 0x30: hiddenRoots array (format 9 = Indexable64)
    //
    // hiddenRoots slot 0 -> classTableFirstPage array
    // classTableFirstPage[N] -> page N (array of ~1024 class pointers)

    // Find the hiddenRoots object at offset 0x30
    {
        ObjectHeader* hrHdr = reinterpret_cast<ObjectHeader*>(heapStart + 0x30);
        auto hrFmt = hrHdr->format();
        size_t hrSlots = hrHdr->slotCount();

        // hiddenRoots should be format 9 (Indexable64) with slots
        if (hrFmt != ObjectFormat::Indexable64 || hrSlots < 1) {
            goto direct_scan;
        }

        Oop classTableFirstPageOop = hrHdr->slotAt(0);

        // Check for both raw 0 and the actual nil object
        if (classTableFirstPageOop.rawBits() == 0 || classTableFirstPageOop.rawBits() == nilObj.rawBits() || !classTableFirstPageOop.isObject()) {
            // Try using hiddenRoots itself as the class table first page
            for (size_t i = 0; i < hrSlots; i++) {
                Oop classOop = hrHdr->slotAt(i);
                // Skip both raw 0 and the actual nil object
                if (classOop.rawBits() != 0 && classOop.rawBits() != nilObj.rawBits() && classOop.isObject()) {
                    memory.setClassAtIndex(static_cast<uint32_t>(i), classOop);
                    totalClasses++;
                }
            }

            if (totalClasses > 0) {
                return true;
            }

            goto direct_scan;
        }

        ObjectHeader* classTableFirstPageHdr = classTableFirstPageOop.asObjectPtr();
        size_t numPages = classTableFirstPageHdr->slotCount();

        // Iterate through each page pointer
        for (size_t pageNum = 0; pageNum < numPages && pageNum < 20; pageNum++) {
            Oop pageOop = classTableFirstPageHdr->slotAt(pageNum);

            // Skip both raw 0 and the actual nil object
            if (pageOop.rawBits() == 0 || pageOop.rawBits() == nilObj.rawBits() || !pageOop.isObject()) {
                continue;
            }

            ObjectHeader* pageHdr = pageOop.asObjectPtr();
            size_t pageSlots = pageHdr->slotCount();

            // Each slot in the page is a class object pointer
            for (size_t i = 0; i < pageSlots; i++) {
                Oop classOop = pageHdr->slotAt(i);

                if (classOop.isNil() || classOop.rawBits() == nilObj.rawBits()) {
                    continue;
                }

                if (classOop.isObject()) {
                    uint32_t classIndex = static_cast<uint32_t>(pageNum * PageSize + i);
                    memory.setClassAtIndex(classIndex, classOop);
                    totalClasses++;
                }
            }
        }

        return true;
    }

direct_scan:

    // Alternative approach: For each object in the heap, register its class
    // if we can find the actual class object. This builds the class table
    // incrementally by finding all class objects.
    //
    // In Spur, classes are objects with:
    //   - Format 1 (FixedSize)
    //   - 12-16 slots typically
    //   - Their metaclass index is in range 3000-5000

    // First, let's find all objects that LOOK like classes
    std::map<uint32_t, Oop> foundClasses;

    uint8_t* scanPtr = heapStart;
    uint8_t* scanEnd = memory.oldSpaceEnd();
    size_t scanned = 0;
    size_t classesFound = 0;

    // std::cerr << "[DEBUG] Scanning heap for class objects..." << std::endl;

    while (scanPtr < scanEnd && scanned < 2000000) {
        ObjectHeader* hdr = reinterpret_cast<ObjectHeader*>(scanPtr);
        uint64_t rawHeader = hdr->rawHeader();

        // Skip zeros
        if (rawHeader == 0) {
            scanPtr += 8;
            continue;
        }

        auto fmt = hdr->format();
        size_t slots = hdr->slotCount();
        uint32_t classIdx = hdr->classIndex();
        size_t objSize = hdr->totalSize();

        // Skip invalid sizes
        if (objSize == 0 || objSize > 100 * 1024 * 1024) {
            scanPtr += 8;
            continue;
        }

        // Classes in Pharo have:
        // - Format 1 (FixedSize) with 12-16 slots
        // - Metaclass index in range 3000-5000
        bool looksLikeClass = false;
        if (fmt == ObjectFormat::FixedSize && slots >= 10 && slots <= 20) {
            // Check metaclass range
            if (classIdx >= 3000 && classIdx < 6000) {
                looksLikeClass = true;
            }
        }

        if (looksLikeClass) {
            // This looks like a class object!
            // Now we need to find what classIndex this class represents
            // In Spur, each class stores its class index in a special field
            //
            // Typically, slot 0 is superclass, slot 1 is methodDict,
            // slot 2 is format (which encodes instance format + class index hint)

            // For now, we'll use a different approach:
            // Find objects that reference this class and see what classIndex they use
            // Or, find the class in the special objects array

            classesFound++;
            if (classesFound <= 10) {
                Oop classOop = memory.oopFromPointer(hdr);
                // std::cerr << "[DEBUG] Found class-like object at 0x" << std::hex
                          // << (scanPtr - heapStart) << std::dec
                          // << " metaclass=" << classIdx << " slots=" << slots << std::endl;
            }
        }

        scanPtr += objSize;
        scanned++;
    }

    // std::cerr << "[DEBUG] Found " << classesFound << " class-like objects in "
              // << scanned << " scanned objects" << std::endl;

    // Build class table from special objects and heap scan
    // Many special objects ARE classes (like SmallInteger class at SO 5)
    // We need to find what classIndex instances of each class use

    // std::cerr << "[DEBUG] Building class table from special objects and heap..." << std::endl;

    // In Spur, each class object has a "format" field that encodes the instance format
    // AND the class index for instances of this class.
    //
    // Class layout in Pharo:
    //   slot 0: superclass
    //   slot 1: methodDict
    //   slot 2: format (SmallInteger encoding instSpec + class index)
    //
    // The class index is stored in the class's identityHash (hash field in header)!
    // This is how Spur implements the class table - each class's hash = its classIndex

    // Scan all class-like objects and register them using their identity hash
    // In Spur, each class's identityHash field IS its class table index
    scanPtr = heapStart;
    scanned = 0;
    size_t registeredClasses = 0;

    // Classes in Pharo/Spur typically have:
    // - Format 0-1 (Fixed size pointer objects)
    // - At least 8 slots (superclass, methodDict, format, etc.)
    // - A metaclass as their classIndex (also a class, so typically > 1000)
    // - An identityHash that becomes their class table index
    //
    // We cast a wide net and register anything that looks like a class

    while (scanPtr < scanEnd && scanned < 2000000) {
        ObjectHeader* hdr = reinterpret_cast<ObjectHeader*>(scanPtr);
        uint64_t rawHeader = hdr->rawHeader();

        if (rawHeader == 0) {
            scanPtr += 8;
            continue;
        }

        auto fmt = hdr->format();
        size_t slots = hdr->slotCount();
        uint32_t metaclassIdx = hdr->classIndex();
        size_t objSize = hdr->totalSize();

        if (objSize == 0 || objSize > 100 * 1024 * 1024) {
            scanPtr += 8;
            continue;
        }

        // Get identity hash - this is the class table index for class objects
        uint32_t identHash = hdr->identityHash();

        // Very inclusive class detection:
        // - Format 0-5 (all pointer object types)
        // - At least 6 slots (metaclasses have ~6 slots, regular classes have 12+)
        // - identityHash > 0 and < 100000 (valid class index range)
        // - classIndex (metaclass) > 0 (non-nil class)
        bool looksLikeClass = (fmt <= ObjectFormat::WeakWithFixed);  // Format 0-5
        looksLikeClass = looksLikeClass && (slots >= 6);  // No upper limit
        looksLikeClass = looksLikeClass && (identHash > 0 && identHash < 100000);
        looksLikeClass = looksLikeClass && (metaclassIdx > 0);

        if (looksLikeClass) {
            // Only register if not already registered (first occurrence wins)
            if (memory.classAtIndex(identHash).isNil()) {
                Oop classOop = memory.oopFromPointer(hdr);
                memory.setClassAtIndex(identHash, classOop);
                registeredClasses++;
            }
        }

        scanPtr += objSize;
        scanned++;
    }

    // Classes registered via heap scan: registeredClasses

    // Try special approach: check special objects that ARE classes
    // SO 5, 6, 7, 9, etc. should be class objects
    // std::cerr << "[DEBUG] Checking special objects for classes..." << std::endl;
    for (int soIdx = 5; soIdx < 20; soIdx++) {
        Oop specialObj = memory.specialObject(static_cast<SpecialObjectIndex>(soIdx));
        // Skip both raw 0 and the actual nil object
        if (specialObj.rawBits() == 0 || specialObj.rawBits() == nilObj.rawBits() || !specialObj.isObject()) continue;

        ObjectHeader* objHdr = specialObj.asObjectPtr();
        auto soFmt = objHdr->format();
        size_t soSlots = objHdr->slotCount();
        uint32_t soMetaclass = objHdr->classIndex();
        uint32_t soHash = objHdr->identityHash();

        // std::cerr << "[DEBUG] SO " << soIdx << ": fmt=" << static_cast<int>(soFmt)
                  // << " slots=" << soSlots << " metaclass=" << soMetaclass
                  // << " identHash=" << soHash << std::endl;

        // If this looks like a class with a reasonable identHash, register it
        if (soFmt == ObjectFormat::FixedSize && soSlots >= 10 && soHash > 0 && soHash < 10000) {
            if (memory.classAtIndex(soHash).isNil()) {
                memory.setClassAtIndex(soHash, specialObj);
                registeredClasses++;
                // std::cerr << "[DEBUG] Registered SO " << soIdx << " class at index " << soHash << std::endl;
            }
        }
    }

    // Final fallback: just scan heap and register ALL objects by their classIndex
    // This gives us at least some class resolution capability
    // std::cerr << "[DEBUG] Registering class indices from heap objects..." << std::endl;

    scanPtr = heapStart;
    scanned = 0;
    std::set<uint32_t> seenClassIndices;

    while (scanPtr < scanEnd && scanned < 100000) {
        ObjectHeader* hdr = reinterpret_cast<ObjectHeader*>(scanPtr);
        uint64_t rawHeader = hdr->rawHeader();

        if (rawHeader == 0) {
            scanPtr += 8;
            continue;
        }

        size_t objSize = hdr->totalSize();
        if (objSize == 0 || objSize > 100 * 1024 * 1024) {
            scanPtr += 8;
            continue;
        }

        uint32_t classIdx = hdr->classIndex();
        auto fmt = hdr->format();
        size_t slots = hdr->slotCount();

        // If this object looks like a class, use IT as the class for its metaclass index
        if (fmt == ObjectFormat::FixedSize && slots >= 10 && slots <= 20 &&
            classIdx >= 3000 && classIdx < 6000) {
            // This is a class object. Its classIndex is the METACLASS index.
            // We need to find what regular class index this class represents.

            // In Spur, each class has a field that stores its hash/index
            // Slot 5 or so might contain the class identity hash which maps to class index
            // For now, just record that this classIdx (metaclass) exists
            seenClassIndices.insert(classIdx);
        }

        seenClassIndices.insert(classIdx);
        scanPtr += objSize;
        scanned++;
    }

    // std::cerr << "[DEBUG] Found " << seenClassIndices.size() << " unique class indices" << std::endl;

    // Report some statistics about class indices
    uint32_t minIdx = *seenClassIndices.begin();
    uint32_t maxIdx = *seenClassIndices.rbegin();
    // std::cerr << "[DEBUG] Class index range: " << minIdx << " - " << maxIdx << std::endl;

    // DEBUG: Check the test offset after class table setup
    (void)registeredClasses;  // Suppress unused warning
    return true;
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

    // Check for immediate values using SPUR's encoding (the image format)
    // Standard Spur 64-bit immediate tags:
    // - Object pointer: tag 000 (8-byte aligned address)
    // - SmallInteger: bit 0 = 1 (any odd number)
    // - Character: tag 010 (bit 0 = 0, but bits 2:1 = 01)
    // - SmallFloat: tag 100 (bit 0 = 0, but bits 2:1 = 10)
    //
    // So immediates are: any value with (tag & 7) != 0
    // Only tag == 0 (all low 3 bits zero) is an object pointer
    uint64_t tag = oldOop & 7;
    if (tag != 0) {
        // This is an immediate value (SmallInteger, Character, or SmallFloat)
        // Convert from Spur encoding to our encoding if needed
        if (tag == 1 || tag == 3 || tag == 5 || tag == 7) {
            // SmallInteger (odd): already compatible with our tag 001
            return oldOop;
        } else if (tag == 2) {
            // Spur Character (010) -> our Character (011)
            // Value is in bits 63:3, shift and re-tag
            uint64_t value = oldOop >> 3;
            return (value << 3) | 0x3;  // Our CharacterTag
        } else if (tag == 4) {
            // Spur SmallFloat (100) -> our SmallFloat (101)
            uint64_t value = oldOop >> 3;
            return (value << 3) | 0x5;  // Our SmallFloatTag
        } else if (tag == 6) {
            // Tag 110 - reserved/unused in Spur, treat as immediate
            return oldOop;
        }
        // Any other tag - don't relocate
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

    // Check tag - handles both Spur and our encoding
    uint64_t tag = raw & 7;
    if (tag != 0) {
        // Immediate value
        if (tag == 1 || tag == 3 || tag == 5 || tag == 7) {
            // Odd tags (SmallInteger variants or our Character/SmallFloat)
            if (tag == 1) {
                // SmallInteger (both Spur and ours use tag 001)
                int64_t value = static_cast<int64_t>(raw) >> 3;
                return Oop::fromSmallInteger(value);
            } else if (tag == 3) {
                // Our Character encoding (011)
                uint32_t codepoint = static_cast<uint32_t>((raw >> 3) & 0x1FFFFFFF);
                return Oop::fromCharacter(codepoint);
            } else if (tag == 5) {
                // Our SmallFloat encoding (101)
                Oop result;
                uint64_t rotated = raw >> 3;
                uint64_t doubleBits = (rotated >> 61) | (rotated << 3);
                double value;
                std::memcpy(&value, &doubleBits, sizeof(double));
                if (!Oop::tryFromSmallFloat(value, result)) {
                    return Oop::fromSmallInteger(0);
                }
                return result;
            }
            // tag == 7: treat as SmallInteger
            int64_t value = static_cast<int64_t>(raw) >> 3;
            return Oop::fromSmallInteger(value);
        } else if (tag == 2) {
            // Spur Character (010) - convert to our format
            uint32_t codepoint = static_cast<uint32_t>((raw >> 3) & 0x1FFFFFFF);
            return Oop::fromCharacter(codepoint);
        } else if (tag == 4) {
            // Spur SmallFloat (100) - convert to our format
            Oop result;
            uint64_t rotated = raw >> 3;
            uint64_t doubleBits = (rotated >> 61) | (rotated << 3);
            double value;
            std::memcpy(&value, &doubleBits, sizeof(double));
            if (!Oop::tryFromSmallFloat(value, result)) {
                return Oop::fromSmallInteger(0);
            }
            return result;
        }
        // tag == 6: reserved, treat as SmallInteger (best effort)
        return Oop::fromSmallInteger(static_cast<int64_t>(raw) >> 3);
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
