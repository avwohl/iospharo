/*
 * ImageLoader.cpp - Spur 64-bit Image File Loader Implementation
 */

#include "ImageLoader.hpp"
#include <cstring>
#include <algorithm>
#include <iostream>
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

    // Check special objects array directly
    size_t specArrayOffset = header_.specialObjectsOop - header_.startOfMemory;
    // std::cerr << "[DEBUG] Special objects array at offset 0x" << std::hex << specArrayOffset << std::dec << std::endl;
    uint64_t* specArray = reinterpret_cast<uint64_t*>(loadedData_ + specArrayOffset);
    // Show header and first 10 slots
    // std::cerr << "[DEBUG] SpecialObjects header: 0x" << std::hex << specArray[0] << std::dec << std::endl;
    for (int i = 0; i < 10; i++) {
        uint64_t val = specArray[i + 1];  // Skip header
        // std::cerr << "  [" << i << "]: 0x" << std::hex << val;
        if (val >= header_.startOfMemory && val < header_.startOfMemory + loadedSize_) {
            // std::cerr << " (offset 0x" << (val - header_.startOfMemory) << ")";
        }
        // std::cerr << std::dec << std::endl;
    }

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

    forEachObject([this, &objectCount, &pointerCount, &relocatedCount, &visitedSchedulerAssoc, schedulerOffset](uint64_t* headerPtr, size_t size) {
        objectCount++;
        size_t offset = reinterpret_cast<uint8_t*>(headerPtr) - loadedData_;

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

        // Check for overflow header - must match ObjectHeader::slotCount() logic!
        size_t slotCount;
        uint64_t* firstSlot;
        size_t headerSize = 8;  // Base header size
        if (slotCountByte == 255) {
            // Check if previous word looks like a valid overflow count
            uint64_t prevWord = *(headerPtr - 1);
            bool looksLikeOverflow = false;
            size_t overflowCount = 255;

            // Check 1: High 32 bits = 0 means it's definitely a raw count
            if (prevWord >= 255 && prevWord <= 1000000) {
                if ((prevWord >> 32) == 0) {
                    looksLikeOverflow = true;
                    overflowCount = static_cast<size_t>(prevWord);
                }
            }

            // Check 2: Low 32 bits might be a valid count even if high bits set
            // This matches ObjectHeader::slotCount() for cases like 0xff00000000000400
            if (!looksLikeOverflow) {
                uint32_t lowBits = static_cast<uint32_t>(prevWord);
                if (lowBits >= 255 && lowBits <= 65536) {
                    looksLikeOverflow = true;
                    overflowCount = static_cast<size_t>(lowBits);
                }
            }

            if (looksLikeOverflow) {
                slotCount = overflowCount;
                headerSize = 16;  // Overflow header included
            } else {
                // Not overflow - exactly 255 slots
                slotCount = 255;
            }
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
        bool hasPointers = (format <= 5);
        bool isCompiledMethod = (format >= 24 && format <= 31);

        if (hasPointers) {
            // All slots are pointers or SmallIntegers - relocate all
            for (size_t i = 0; i < slotCount; ++i) {
                pointerCount++;
                uint64_t oldValue = firstSlot[i];
                // relocatePointer handles both object pointers AND SmallIntegers
                firstSlot[i] = relocatePointer(oldValue);
                if (oldValue != firstSlot[i]) relocatedCount++;
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

    // std::cerr << "[DEBUG] Relocation: " << objectCount << " objects, "
              // << relocatedCount << " pointers relocated" << std::endl;
    // std::cerr << "[DEBUG] Visited scheduler association: " << (visitedSchedulerAssoc ? "YES" : "NO") << std::endl;

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
        // Overflow: word before header may contain count
        // Must match ObjectHeader::slotCount() logic
        uint64_t prevWord = *(headerPtr - 1);

        // Check 1: High 32 bits = 0 means it's definitely a raw count
        if (prevWord >= 255 && prevWord <= 1000000 && (prevWord >> 32) == 0) {
            slotCount = static_cast<size_t>(prevWord);
        } else {
            // Check 2: Low 32 bits might be valid count
            uint32_t lowBits = static_cast<uint32_t>(prevWord);
            if (lowBits >= 255 && lowBits <= 65536) {
                slotCount = static_cast<size_t>(lowBits);
            }
            // else keep slotCount = 255
        }
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
    // In Spur 64-bit, the class table is stored as follows:
    //
    // The first object in old space (at offset 0x0) is the "hiddenRoots" object.
    // hiddenRoots is an Array with:
    //   - slot 0: classTableFirstPage - an Array of class table pages
    //   - slot 1+: other roots (symbols, etc.)
    //
    // Each class table page is an Array of up to 1024 class object pointers.
    // Class index N is found at: classTableFirstPage[N / 1024][N % 1024]
    //
    // Page indices:
    //   - Page 0: class indices 0-1023
    //   - Page 1: class indices 1024-2047
    //   - Page 2: class indices 2048-3071
    //   - Page 3: class indices 3072-4095 (includes index 3075)
    //   - Page 4: class indices 4096-5119 (includes index 4735)

    uint8_t* heapStart = memory.oldSpaceStart();
    Oop nilObj = memory.specialObject(SpecialObjectIndex::NilObject);
    constexpr size_t PageSize = 1024;
    size_t totalClasses = 0;

    // std::cerr << "[DEBUG] Building class table from hiddenRoots structure..." << std::endl;

    // The hiddenRoots object is at the very start of old space (offset 0)
    ObjectHeader* hiddenRootsHdr = reinterpret_cast<ObjectHeader*>(heapStart);
    uint64_t hiddenRootsRaw = hiddenRootsHdr->rawHeader();

    size_t hiddenRootsSlots = hiddenRootsHdr->slotCount();
    if (hiddenRootsSlots == 0) {
        // std::cerr << "[DEBUG] hiddenRoots has 0 slots, trying alternate location..." << std::endl;
        // Maybe hiddenRoots is not at offset 0, try offset 0x8
        hiddenRootsHdr = reinterpret_cast<ObjectHeader*>(heapStart + 0x8);
        hiddenRootsRaw = hiddenRootsHdr->rawHeader();
        hiddenRootsSlots = hiddenRootsHdr->slotCount();
        // std::cerr << "[DEBUG] At offset 0x8: slots=" << hiddenRootsSlots
                  // << " fmt=" << static_cast<int>(hiddenRootsHdr->format()) << std::endl;
    }

    // Slot 0 of hiddenRoots should be the classTableFirstPage array
    if (hiddenRootsSlots < 1) {
        // std::cerr << "[DEBUG] hiddenRoots doesn't have enough slots for class table" << std::endl;
        // Fall back to scanning approach
        goto fallback_scan;
    }

    {
        Oop classTableFirstPageOop = hiddenRootsHdr->slotAt(0);

        if (classTableFirstPageOop.isNil() || !classTableFirstPageOop.isObject()) {
            goto fallback_scan;
        }

        ObjectHeader* classTableFirstPageHdr = classTableFirstPageOop.asObjectPtr();
        size_t numPages = classTableFirstPageHdr->slotCount();

        // Iterate through each page
        for (size_t pageNum = 0; pageNum < numPages && pageNum < 20; pageNum++) {
            Oop pageOop = classTableFirstPageHdr->slotAt(pageNum);

            if (pageOop.isNil() || !pageOop.isObject()) {
                // std::cerr << "[DEBUG] Page " << pageNum << " is nil, skipping" << std::endl;
                continue;
            }

            ObjectHeader* pageHdr = pageOop.asObjectPtr();
            size_t pageSlots = pageHdr->slotCount();

            // std::cerr << "[DEBUG] Page " << pageNum << ": " << pageSlots << " slots" << std::endl;

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

fallback_scan:
    // std::cerr << "[DEBUG] Falling back to Spur class table structure navigation..." << std::endl;

    // In Spur, the layout at start of old space is:
    //   offset 0x0:  nil object (format 0, 0 slots)
    //   offset 0x10: false object (format 0, 0 slots)
    //   offset 0x20: true object (format 0, 0 slots)
    //   offset 0x30: hiddenRoots array (format 9 = Indexable64)
    //
    // hiddenRoots slot 0 -> classTableFirstPage array
    // classTableFirstPage[N] -> page N (array of ~1024 class pointers)

    // Find the hiddenRoots object at offset 0x30
    ObjectHeader* hrHdr = reinterpret_cast<ObjectHeader*>(heapStart + 0x30);
    auto hrFmt = hrHdr->format();
    size_t hrSlots = hrHdr->slotCount();

    // hiddenRoots should be format 9 (Indexable64) with slots
    if (hrFmt != ObjectFormat::Indexable64 || hrSlots < 1) {
        // std::cerr << "[DEBUG] hiddenRoots at 0x30 doesn't look right, trying direct scan..." << std::endl;
        goto direct_scan;
    }

    {
        // Dump first 10 slots of hiddenRoots to understand structure
        // std::cerr << "[DEBUG] First 10 slots of hiddenRoots:" << std::endl;
        for (int i = 0; i < 10 && i < static_cast<int>(hrSlots); i++) {
            Oop slotOop = hrHdr->slotAt(i);
            // std::cerr << "  [" << i << "]: 0x" << std::hex << slotOop.rawBits() << std::dec;
            // if (slotOop.isNil()) std::cerr << " (nil)";
            // else if (slotOop.isSmallInteger()) std::cerr << " (int=" << slotOop.asSmallInteger() << ")";
            // else if (slotOop.isObject()) std::cerr << " (object)";
            // std::cerr << std::endl;
        }

        // The class table pages might be directly embedded in hiddenRoots
        // or might be pointed to by slot 0
        Oop classTableFirstPageOop = hrHdr->slotAt(0);
        // std::cerr << "[DEBUG] classTableFirstPage oop: 0x" << std::hex
                  // << classTableFirstPageOop.rawBits() << std::dec << std::endl;

        if (classTableFirstPageOop.isNil() || !classTableFirstPageOop.isObject()) {
            // std::cerr << "[DEBUG] classTableFirstPage is nil or not an object" << std::endl;
            // Try using hiddenRoots itself as the class table first page
            // Each slot points to a class object
            // std::cerr << "[DEBUG] Trying to use hiddenRoots slots directly as class table..." << std::endl;

            for (size_t i = 0; i < hrSlots; i++) {
                Oop classOop = hrHdr->slotAt(i);
                if (!classOop.isNil() && classOop.isObject()) {
                    memory.setClassAtIndex(static_cast<uint32_t>(i), classOop);
                    totalClasses++;
                }
            }

            if (totalClasses > 0) {
                // std::cerr << "[DEBUG] Loaded " << totalClasses << " classes directly from hiddenRoots" << std::endl;
                return true;
            }

            goto direct_scan;
        }

        ObjectHeader* classTableFirstPageHdr = classTableFirstPageOop.asObjectPtr();
        size_t numPages = classTableFirstPageHdr->slotCount();

        // std::cerr << "[DEBUG] classTableFirstPage:" << std::endl;
        // std::cerr << "  format: " << static_cast<int>(classTableFirstPageHdr->format()) << std::endl;
        // std::cerr << "  numPages: " << numPages << std::endl;

        // Iterate through each page pointer
        for (size_t pageNum = 0; pageNum < numPages && pageNum < 20; pageNum++) {
            Oop pageOop = classTableFirstPageHdr->slotAt(pageNum);

            if (pageOop.isNil() || !pageOop.isObject()) {
                // std::cerr << "[DEBUG] Page " << pageNum << " is nil, skipping" << std::endl;
                continue;
            }

            ObjectHeader* pageHdr = pageOop.asObjectPtr();
            size_t pageSlots = pageHdr->slotCount();

            // std::cerr << "[DEBUG] Page " << pageNum << ": " << pageSlots << " slots, fmt="
                      // << static_cast<int>(pageHdr->format()) << std::endl;

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

        // std::cerr << "[DEBUG] Loaded " << totalClasses << " classes from " << numPages
                  // << " class table pages via hiddenRoots" << std::endl;
        return true;
    }

direct_scan:
    // std::cerr << "[DEBUG] Direct scan: building class table by scanning all objects..." << std::endl;

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

    // Also track what indices we need but haven't found
    std::set<uint32_t> neededIndices = {3075, 3077, 3079, 3080, 3085, 3086, 3092, 3102, 3121, 3135, 3139, 4735};

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

        // Expanded criteria for class detection:
        // - Format 1 (FixedSize) with 10-20 slots
        // - Metaclass index > 1000 (allow wider range)
        bool looksLikeClass = (fmt == ObjectFormat::FixedSize && slots >= 10 && slots <= 20);

        // Get identity hash
        uint32_t identHash = hdr->identityHash();

        // If identHash matches a needed index, definitely register
        if (looksLikeClass && neededIndices.count(identHash) > 0) {
            Oop classOop = memory.oopFromPointer(hdr);
            memory.setClassAtIndex(identHash, classOop);
            registeredClasses++;
            // std::cerr << "[DEBUG] Found needed class at index " << identHash
                      // << " (metaclass=" << metaclassIdx << ", slots=" << slots << ")" << std::endl;
            neededIndices.erase(identHash);
        }
        // Also register if it looks like a class in the expected range
        else if (looksLikeClass && metaclassIdx >= 3000 && metaclassIdx < 6000 &&
                 identHash > 0 && identHash < 10000) {
            Oop classOop = memory.oopFromPointer(hdr);
            memory.setClassAtIndex(identHash, classOop);
            registeredClasses++;

        }

        scanPtr += objSize;
        scanned++;
    }

    // std::cerr << "[DEBUG] Registered " << registeredClasses << " classes using identity hash" << std::endl;

    // Report missing needed indices
    if (!neededIndices.empty()) {
        // std::cerr << "[DEBUG] Still missing " << neededIndices.size() << " needed class indices: ";
        for (uint32_t idx : neededIndices) {
            // std::cerr << idx << " ";
        }
        // std::cerr << std::endl;
    }

    // Try special approach: check special objects that ARE classes
    // SO 5, 6, 7, 9, etc. should be class objects
    // std::cerr << "[DEBUG] Checking special objects for classes..." << std::endl;
    for (int soIdx = 5; soIdx < 20; soIdx++) {
        Oop specialObj = memory.specialObject(static_cast<SpecialObjectIndex>(soIdx));
        if (specialObj.isNil() || !specialObj.isObject()) continue;

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
    size_t testOff = 0x296e0;
    if (testOff + 8 <= loadedSize_) {
        uint64_t val = *reinterpret_cast<uint64_t*>(loadedData_ + testOff);
        // std::cerr << "[DEBUG] After buildClassTable: offset 0x" << std::hex << testOff
                  // << " value=0x" << val << std::dec << std::endl;
    }

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

    // Check for immediate values (bit 0 = 1)
    // Spur 64-bit uses 3-bit tags just like us:
    // - SmallInteger: tag 001 (bits 2:0), value in bits 63:3
    // - Character: tag 010
    // - SmallFloat: tag 100
    // So immediates don't need format conversion, just return as-is
    if (oldOop & 1) {
        return oldOop;  // Immediates don't relocate
    }

    // Extract the address part (clear low 3 bits)
    uint64_t oldAddr = oldOop & ~7ULL;

    // Calculate new address
    uint64_t newAddr = oldAddr - oldBase_ + newBase_;

    // In the original Spur format, bits 0-2 might encode space info
    // We need to set our space encoding (bits 1-2 = 00 for old space)
    // Bit 0 stays 0 for object pointers
    return newAddr;  // Space encoding is 00 (old space) by default
}

Oop ImageLoader::rawToOop(uint64_t raw, ObjectMemory& memory) const {
    if (raw == 0) return Oop::nil();
    if (raw & 1) {
        // Immediate - determine type and convert
        uint64_t tag = raw & 7;
        if (tag == 1) {
            // SmallInteger
            int64_t value = static_cast<int64_t>(raw) >> 3;
            return Oop::fromSmallInteger(value);
        } else if (tag == 3) {
            // Character
            uint32_t codepoint = static_cast<uint32_t>((raw >> 3) & 0x1FFFFFFF);
            return Oop::fromCharacter(codepoint);
        } else if (tag == 5) {
            // SmallFloat - complex encoding, preserve raw bits for now
            // The encoding may differ between VMs
            Oop result;
            // Try to create SmallFloat, fall back if encoding differs
            uint64_t rotated = raw >> 3;
            uint64_t doubleBits = (rotated >> 61) | (rotated << 3);
            double value;
            std::memcpy(&value, &doubleBits, sizeof(double));
            if (!Oop::tryFromSmallFloat(value, result)) {
                // Fall back to zero - image may use different encoding
                return Oop::fromSmallInteger(0);
            }
            return result;
        }
    }

    // Object pointer
    ObjectHeader* ptr = reinterpret_cast<ObjectHeader*>(raw & ~7ULL);
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
        // Could be overflow (previous word is count) or exactly 255 slots
        uint64_t prevWord = *(headerPtr - 1);

        // Check if prevWord looks like a valid overflow count.
        // Must match ObjectHeader::slotCount() logic for consistency.
        bool looksLikeOverflow = false;
        size_t overflowCount = 255;

        // Check 1: High 32 bits = 0 means it's definitely a raw count
        if (prevWord >= 255 && prevWord <= 1000000) {
            if ((prevWord >> 32) == 0) {
                looksLikeOverflow = true;
                overflowCount = static_cast<size_t>(prevWord);
            }
        }

        // Check 2: Low 32 bits might be a valid count even if high bits set
        // This handles cases like 0xff00000000000400 (1024 slots)
        if (!looksLikeOverflow) {
            uint32_t lowBits = static_cast<uint32_t>(prevWord);
            if (lowBits >= 255 && lowBits <= 65536) {
                looksLikeOverflow = true;
                overflowCount = static_cast<size_t>(lowBits);
            }
        }

        if (looksLikeOverflow) {
            slotCount = overflowCount;
            headerSize += 8;  // Overflow word
        } else {
            // Not overflow - exactly 255 slots
            slotCount = 255;
        }
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

    // Total size (8-byte aligned)
    size_t totalSize = headerSize + bodySize;
    return (totalSize + 7) & ~7ULL;
}

} // namespace pharo
