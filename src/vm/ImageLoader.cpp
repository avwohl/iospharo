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

    // MULTI-SEGMENT IMAGES ARE NOT SUPPORTED — say so at load, not by
    // crashing in the GC 90 MB later (docs/vm-compat-bugs.md #4).
    //
    // Spur writes the heap as one or more SEGMENTS inside the image data,
    // each (but the last) terminated by a bridge — a double-word free chunk
    // whose slot count spans the gap to the next segment.  The header's
    // firstSegmentBytes gives the first one's extent; when it is smaller
    // than imageBytes there are more.  This loader walks the whole data
    // block as one contiguous object sequence and relocates every pointer by
    // a single (newBase - oldBase) delta, so on a multi-segment image it
    // mis-parses at a bridge, stops (see [IMGLOAD-WALK-TRUNC]) and leaves
    // everything past that point holding SAVED-IMAGE addresses.  The GC then
    // dereferences one.  A fresh `Metacello ... baseline: 'Ume'` image is
    // 142 MB with a 75.8 MB first segment and does exactly that.
    //
    // NOTE for whoever implements it: the earlier "multi-segment" root cause
    // was retracted on the evidence that the file is exactly
    // headerSize + imageBytes with nothing past it.  That test does not
    // disprove segments — segments live INSIDE imageBytes.  firstSegmentBytes
    // is the field that answers it.  Real support needs the bridge walk AND a
    // PER-SEGMENT relocation delta, since each segment carries its own saved
    // start address.

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

    // Multi-segment images: walk the bridge chain, pack, record per-segment
    // swizzles.  Single-segment images get one entry and behave as before.
    if (!readSegments(result)) return false;
    (void)loadEnd;

    // Update the free pointer
    memory.setOldSpaceFreePointer(loadedData_ + loadedSize_);

    return true;
}

// Spur writes the heap as one or more SEGMENTS inside imageBytes, each but the
// last ending in a 16-byte BRIDGE.  Reference: cointerp-cpp.c:14984
// (SIR_readSegmentsFromImageFile).  Two things matter, and both were missing:
//
//   1. the bridges are DROPPED — the reference advances its write pointer by
//      `segSize - 2*BaseHeaderSize`, so the next segment overwrites the bridge
//      and the heap ends up a clean contiguous object sequence.  We read the
//      file in one shot, so we pack afterwards with memmove.
//   2. each segment carries its OWN saved base, so relocation needs a
//      PER-SEGMENT delta.  One `newBase - oldBase` is correct only for the
//      first segment.
//
// Missing (1) is what made forEachObject mis-parse at a bridge and silently
// abandon 38% of the heap; missing (2) would relocate later segments' pointers
// by the wrong amount (docs/vm-compat-bugs.md #4).
bool ImageLoader::readSegments(LoadResult& result) {
    segments_.clear();
    constexpr uint64_t kBaseHeaderSize = 8;
    constexpr uint64_t kBridgeSize = 2 * kBaseHeaderSize;

    const uint64_t firstSegSize = header_.firstSegmentBytes;
    if (firstSegSize == 0 || firstSegSize >= loadedSize_) {
        segments_.push_back({oldBase_, oldBase_ + loadedSize_,
                             static_cast<int64_t>(newBase_ - oldBase_)});
        return true;
    }

    uint64_t oldBase = oldBase_;
    uint8_t* readCursor = loadedData_;
    uint8_t* writeCursor = loadedData_;
    uint64_t segSize = firstSegSize;
    size_t guard = 0;

    while (true) {
        if (segSize < kBridgeSize
                || readCursor + segSize > loadedData_ + loadedSize_) {
            result.error = "Multi-segment image: malformed segment size";
            return false;
        }
        const uint64_t payload = segSize - kBridgeSize;

        // Read the bridge BEFORE packing can overwrite it.
        uint8_t* bridgehead = readCursor + payload;
        const uint64_t headWord = *reinterpret_cast<uint64_t*>(bridgehead);
        const uint64_t nextSize =
            *reinterpret_cast<uint64_t*>(bridgehead + kBaseHeaderSize);
        // Byte 7 of the head word zero => no address gap; else the low 56 bits
        // are a SLOT count of skipped address space.
        const uint64_t bridgeSpan =
            ((headWord >> 56) == 0) ? 0 : (8ull * ((headWord << 8) >> 8));

        if (writeCursor != readCursor) {
            std::memmove(writeCursor, readCursor, payload);
        }
        segments_.push_back({oldBase, oldBase + payload,
                             static_cast<int64_t>(
                                 reinterpret_cast<uint64_t>(writeCursor) - oldBase)});

        writeCursor += payload;
        readCursor  += segSize;
        // The reference advances the SAVED base by the FULL segment size —
        // `oldBase := oldBase + nextSegmentSize + bridgeSpan`
        // (cointerp-cpp.c:15008) — i.e. including the 16-byte bridge, which
        // occupies saved address space even though it holds no object.  Using
        // the payload here instead put every segment after the first 16 bytes
        // (then 32, 48, ...) low, which showed up as 8 invalid pointers in
        // hiddenRoots on the Ume image while the class table itself still
        // matched.
        oldBase      = oldBase + segSize + bridgeSpan;

        if (nextSize == 0) break;
        segSize = nextSize;
        if (++guard > 4096) {
            result.error = "Multi-segment image: bridge chain did not terminate";
            return false;
        }
    }

    const size_t packed = static_cast<size_t>(writeCursor - loadedData_);
    fprintf(stderr,
        "[IMGLOAD-MULTISEG] %zu segments; packed %zu bytes from %zu "
        "(%zu bridge bytes dropped)\n",
        segments_.size(), packed, loadedSize_, loadedSize_ - packed);
    loadedSize_ = packed;
    return true;
}

// ===== POINTER RELOCATION =====

// Locate hiddenRootsObj — the ONE format-9 object whose slots really are Oops.
//
// Spur lays the heap out as nil(1), false(2), true(3), freeListsObj(4),
// hiddenRootsObj(5), so it is identifiable by position before anything has
// been relocated.  Everything else with format 9 is a 64-bit WORD ARRAY whose
// contents are DATA (see relocatePointers).  Same walk as setupClassTable's.
uint64_t* ImageLoader::findHiddenRootsHeader() const {
    uint8_t* heapStart = loadedData_;
    if (!heapStart || loadedSize_ < 16) return nullptr;
    auto objectAfter = [&](uint8_t* objPtr) -> uint8_t* {
        ObjectHeader* hdr = reinterpret_cast<ObjectHeader*>(objPtr);
        size_t sz = hdr->totalSize();
        if (sz < 16) sz = 16;
        uint8_t* next = objPtr + sz - (hdr->hasOverflowSlots() ? 8 : 0);
        if (next + 16 <= heapStart + loadedSize_) {
            uint64_t followingWord = *reinterpret_cast<uint64_t*>(next + 8);
            if (extractNumSlots(followingWord) == 255) next += 8;
        }
        return next;
    };
    uint8_t* obj = heapStart;
    for (int i = 0; i < 4; i++) {
        obj = objectAfter(obj);
        if (obj >= heapStart + loadedSize_) return nullptr;
    }
    return reinterpret_cast<uint64_t*>(obj);
}

bool ImageLoader::relocatePointers(ObjectMemory& memory, LoadResult& result) {
    // See findHiddenRootsHeader: format 9 is pointer-bearing for exactly ONE
    // object in the image, and treating every format-9 object as pointers
    // CORRUPTS 64-bit word-array data (docs/vm-compat-bugs.md #16).
    uint64_t* hiddenRootsHeader = findHiddenRootsHeader();
    forEachObject([this, hiddenRootsHeader](uint64_t* headerPtr, size_t size) {
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
            // SILENT TRUNCATION: the trailing slots are then never relocated and
            // keep SAVED-IMAGE addresses.  That is exactly how an image ends up
            // with an unrelocated activeProcess (docs/vm-compat-bugs.md #4), and
            // the clamp said nothing about it.  Report it.
            static int clampN = 0;
            if (++clampN <= 20) {
                fprintf(stderr,
                    "[IMGLOAD-CLAMP] obj=0x%llx format=%u slotCount=%zu > maxSlots=%zu"
                    " (size=%zu) — %zu trailing slots NOT relocated\n",
                    (unsigned long long)(uintptr_t)headerPtr, (unsigned)format,
                    slotCount, maxSlots, size, slotCount - maxSlots);
            }
            slotCount = maxSlots;
        }

        // Only pointer objects have pointer fields to relocate.
        // Formats 0-5 are pointer objects, 24-31 are compiled methods (mixed).
        //
        // Format 9 (Indexable64) is a 64-bit WORD ARRAY.  It is pointer-bearing
        // for exactly ONE object — hiddenRootsObj, which holds the class-table
        // pages.  Relocating EVERY format-9 object as pointers, which this did
        // until 2026-08-12, silently rewrites ordinary numeric data: a
        // Cog-written `DoubleWordArray` holding 16r10000000000 came back as
        // 16r300000000, our heap base, while stock Cog read it back unchanged
        // (docs/vm-compat-bugs.md #16).  Any element landing in
        // [oldBase, oldBase + imageBytes) was mangled.
        bool hasPointers = (format <= 5)
                        || (format == 9 && headerPtr == hiddenRootsHeader);
        bool isCompiledMethod = (format >= 24 && format <= 31);

        if (hasPointers) {
            // All slots are pointers or SmallIntegers - relocate all
            for (size_t i = 0; i < slotCount; ++i) {
                uint64_t before = firstSlot[i];
                firstSlot[i] = relocatePointer(before);
                // Attribute every DECLINED old-base pointer to its owner's
                // format.  Format 9 is a 64-bit WORD array relocated as if it
                // held pointers (a documented hack for hiddenRoots), so a
                // numeric datum that happens to look like a tag-0 old-base
                // address lands here as a FALSE POSITIVE — and one that lands
                // inside the loaded range gets silently MANGLED instead.
                // A decline attributed to a genuine pointer format (0-5) is a
                // different animal: a live slot left holding a saved-image
                // address, which the GC mark phase then dereferences
                // (docs/vm-compat-bugs.md #4).  Counting by format is what
                // tells the two apart; the previous session guessed.
                if (before == firstSlot[i] && (before & 7) == 0
                        && before >= oldBase_ + loadedSize_) {
                    declinedByFormat_[format]++;
                }
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

    {
        // Report ONLY when a genuine pointer format is implicated.  Format 9
        // declines are word-array data misread as pointers and are expected in
        // the tens of thousands on a healthy image; a decline in formats 0-5 is
        // a live slot left holding a saved-image address, which the GC will
        // dereference (docs/vm-compat-bugs.md #4).
        size_t pointerFormatDeclines = 0;
        for (size_t f = 0; f <= 5; ++f) pointerFormatDeclines += declinedByFormat_[f];
        if (pointerFormatDeclines > 0) {
            size_t total = 0;
            for (size_t f = 0; f < 32; ++f) total += declinedByFormat_[f];
            fprintf(stderr, "[IMGLOAD-DECLINE-BY-FORMAT] %zu declined old-base "
                            "pointers in POINTER formats (of %zu total):",
                    pointerFormatDeclines, total);
            for (size_t f = 0; f < 32; ++f) {
                if (declinedByFormat_[f])
                    fprintf(stderr, " fmt%zu=%zu", f, declinedByFormat_[f]);
            }
            fprintf(stderr, "  — a pointer-format decline is a live slot left "
                            "holding a saved-image address; fmt9 entries are "
                            "word-array data and expected\n");
        }
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
        // objPtr is a HEADER address, but totalSize() measures from the
        // allocation start — and for a >=255-slot object that start is the
        // overflow word, 8 bytes BEFORE the header.  Without this correction
        // `next` overshot the true end by 8 for any overflow object in the
        // first five, which would then desync the rest of the walk.  Masked
        // today only because nil/false/true/freeListsObj are all small.
        uint8_t* next = objPtr + size - (hdr->hasOverflowSlots() ? 8 : 0);
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
    // Multi-segment: each segment has its own saved base, so find the owning
    // segment and use ITS delta.  One segment => identical to the old path.
    if (segments_.size() > 1) {
        for (const Segment& seg : segments_) {
            if (oldOop >= seg.oldStart && oldOop < seg.oldEnd) {
                return static_cast<uint64_t>(
                    static_cast<int64_t>(oldOop) + seg.swizzle);
            }
        }
        return oldOop;  // in no segment — decline, as below
    }
    // Only relocate if it's within the old heap bounds
    if (oldOop < oldBase_ || oldOop >= oldBase_ + loadedSize_) {
        // Pointer outside old heap - could be special value, already relocated,
        // or from another segment. Don't relocate.
        //
        // A pointer that is ABOVE oldBase_ but beyond loadedSize_ is the
        // dangerous case: it looks exactly like a saved-image address we failed
        // to load (multi-segment image?), and declining it leaves a dangling
        // saved-image pointer in a live slot — docs/vm-compat-bugs.md #4.
        // NOT reported here.  This function has no idea WHO owns the slot, and
        // the answer turns out to be everything: on a healthy base image
        // 74,998 values land here and every one of them is inside a FORMAT 9
        // object — a 64-bit word array that `hasPointers` includes for
        // hiddenRoots' sake, so ordinary numeric data gets offered to this
        // function and correctly declined.  Printing them unconditionally
        // trained the eye to skip a line that is almost always benign, and one
        // session built a root-cause theory on them.  The caller knows the
        // owner's format and reports only the dangerous case — see
        // [IMGLOAD-DECLINE-BY-FORMAT] in relocatePointers().
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
            // 32 bits, matching Oop::CharacterMax — the old 30-bit mask
            // truncated high codepoints on LOAD as well.
            uint32_t codepoint = static_cast<uint32_t>((raw >> 3) & 0xFFFFFFFFu);
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
