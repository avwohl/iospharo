/*
 * ImageLoader.hpp - Spur 64-bit Image File Loader
 *
 * This class loads Pharo/Squeak image files in Spur 64-bit format.
 *
 * SPUR IMAGE FORMAT (64-bit):
 *
 * The image file consists of:
 * 1. A fixed-size header (64 or 72 bytes depending on version)
 * 2. Raw object memory (heap snapshot)
 *
 * IMAGE HEADER FIELDS:
 *   - imageFormat: Magic number identifying format (68021, 68533, etc.)
 *   - headerSize: Bytes before first object
 *   - imageBytes: Total size of object data
 *   - startOfMemory: Base address when image was saved
 *   - specialObjectsOop: Raw oop of special objects array
 *   - lastHash: Last identity hash assigned
 *   - screenSize: Saved screen dimensions (packed)
 *   - imageHeaderFlags: Various flags
 *   - extraVMMemory: Additional memory requested
 *
 * LOADING PROCESS:
 * 1. Read and validate header
 * 2. Allocate memory for heap
 * 3. Read raw object data into memory
 * 4. Relocate all object pointers (adjust for new base address)
 * 5. Build class table from class objects
 * 6. Set up special objects
 *
 * POINTER RELOCATION:
 *   The image was saved at address `startOfMemory`. We load it at a
 *   different address. Every object pointer in the heap must be adjusted:
 *     newOop = oldOop - oldBase + newBase
 *
 *   With our iOS-compatible Oop class, we also need to update the
 *   space encoding in the low bits.
 */

#ifndef PHARO_IMAGE_LOADER_HPP
#define PHARO_IMAGE_LOADER_HPP

#include "ObjectMemory.hpp"
#include <string>
#include <cstdint>
#include <fstream>
#include <vector>
#include <iostream>

namespace pharo {

// ===== SPUR 64-BIT HEADER FIELD EXTRACTION =====
// classIndex: bits 0-21, format: bits 24-28, hash: bits 32-53, numSlots: bits 56-63

inline uint32_t spurClassIndex(uint64_t header) {
    return static_cast<uint32_t>(header & 0x3FFFFF);
}

inline uint8_t spurFormat(uint64_t header) {
    return static_cast<uint8_t>((header >> 24) & 0x1F);
}

inline uint32_t spurHash(uint64_t header) {
    return static_cast<uint32_t>((header >> 32) & 0x3FFFFF);
}

inline uint8_t spurNumSlots(uint64_t header) {
    return static_cast<uint8_t>((header >> 56) & 0xFF);
}

/// Image format versions we support
enum class ImageFormat : uint32_t {
    Spur64 = 68021,       // Basic Spur 64-bit
    Spur64Sista = 68533,  // Spur 64-bit with Sista bytecodes
};

/// Raw image header as stored in file
struct SpurImageHeader {
    uint32_t imageFormat;         // Magic number
    uint32_t headerSize;          // Bytes before first object
    uint64_t imageBytes;          // Size of heap data
    uint64_t startOfMemory;       // Base address when saved
    uint64_t specialObjectsOop;   // Oop of special objects array
    uint64_t lastHash;            // Last identity hash
    uint64_t screenSize;          // width << 16 | height
    uint64_t imageHeaderFlags;    // Various flags
    uint32_t extraVMMemory;       // Extra memory requested (KB)
    uint16_t numStackPages;       // Stack pages (if present)
    uint16_t cogCodeSize;         // JIT code size (KB, if present)
    uint32_t edenBytes;           // Eden size
    uint16_t maxExtSemTabSize;    // Max external semaphore table size
    uint16_t unused1;
    uint64_t firstSegmentBytes;   // Size of first segment
    uint64_t freeOldSpaceInImage; // Free space in old space
};

/// Flags in imageHeaderFlags
enum class ImageFlags : uint64_t {
    FullBlockClosures = 1 << 0,    // Uses full block closures
    PreemptionYields = 1 << 1,     // Preemption causes yield not switch
    DisableVMDisplay = 1 << 2,     // No VM-level display
    SistaV1 = 1 << 3,              // Sista V1 bytecode set
    ImageFloatsBigEndian = 1 << 4, // Floats stored big-endian
    PosixFlock = 1 << 5,           // Use POSIX file locking
};

/// Result of loading an image
struct LoadResult {
    bool success = false;
    std::string error;

    // Image metadata
    ImageFormat format = ImageFormat::Spur64;
    uint64_t heapSize = 0;
    uint32_t screenWidth = 0;
    uint32_t screenHeight = 0;
    bool sistaV1 = false;
    bool fullBlockClosures = false;
};

class ImageLoader {
public:
    ImageLoader() = default;

    /// Load an image file into the object memory.
    /// The ObjectMemory must already be initialized with sufficient space.
    LoadResult load(const std::string& path, ObjectMemory& memory);

    /// Get the image header (valid after successful load)
    const SpurImageHeader& header() const { return header_; }

private:
    SpurImageHeader header_{};

    // Loading state
    uint8_t* loadedData_ = nullptr;
    size_t loadedSize_ = 0;
    uint64_t oldBase_ = 0;
    uint64_t newBase_ = 0;

    // ===== LOADING STEPS =====

    /// Read and validate the image header
    bool readHeader(std::ifstream& file, LoadResult& result);

    /// Load raw heap data into memory
    bool loadHeapData(std::ifstream& file, ObjectMemory& memory,
                      LoadResult& result);

    /// Relocate all object pointers
    bool relocatePointers(ObjectMemory& memory, LoadResult& result);

    /// Find and set up the special objects array
    bool setupSpecialObjects(ObjectMemory& memory, LoadResult& result);

    /// Build the class table from loaded objects
    bool buildClassTable(ObjectMemory& memory, LoadResult& result);

    // ===== POINTER UTILITIES =====

    /// Check if a raw value looks like an object pointer (not immediate)
    bool isObjectPointer(uint64_t bits) const;

    /// Relocate a single pointer value
    uint64_t relocatePointer(uint64_t oldOop) const;

    /// Convert a raw pointer to an Oop with correct space encoding
    Oop rawToOop(uint64_t raw, ObjectMemory& memory) const;

    /// Relocate slots of an object given its oop (pointer to header)
    void relocateObjectSlots(uint64_t* headerPtr);

    // ===== OBJECT SCANNING =====

    /// Iterate over all objects in the loaded heap
    template<typename Func>
    void forEachObject(Func callback);

    /// Get the size of an object from its header
    size_t objectSize(uint64_t* headerPtr) const;

    /// Check if a word looks like a valid Spur object header
    static bool looksLikeValidHeader(uint64_t word) {
        // Extract fields using CORRECT Spur 64-bit layout
        uint8_t format = spurFormat(word);
        uint32_t classIndex = spurClassIndex(word);

        // Format must be 0-31 (valid for Spur)
        if (format > 31) return false;

        // Class index 0 is free chunk marker - valid but rare
        // Very large class indices are suspicious
        // Typical Pharo images have < 20k classes, be conservative with 50k limit
        if (classIndex > 50000) return false;

        // Also reject if high bits of header look like ASCII (indicates misaligned scan)
        uint8_t highByte = (word >> 56) & 0xFF;
        if (highByte >= 0x20 && highByte < 0x7F) {
            // Looks like ASCII - probably misaligned into string data
            return false;
        }

        return true;
    }

    /// Check if a word could be a valid overflow slot count
    bool looksLikeValidOverflowCount(uint64_t word, uint8_t* scanPos) const {
        // Overflow count should be >= 255 (otherwise wouldn't need overflow)
        if (word < 255) return false;

        // Overflow count shouldn't exceed remaining heap
        size_t remaining = (loadedData_ + loadedSize_) - scanPos;
        size_t objectSize = 8 + word * 8 + 8;  // header + slots + overflow word
        if (objectSize > remaining) return false;

        // Overflow count shouldn't be astronomically large
        // Even a 1GB object would only have ~128M slots
        if (word > 128 * 1024 * 1024) return false;

        return true;
    }
};

// ===== TEMPLATE IMPLEMENTATION =====

template<typename Func>
void ImageLoader::forEachObject(Func callback) {
    // Sequential Spur object scanner.
    // In Spur, objects are contiguous in memory. We step from object to object
    // using the size computed from each header. No heuristic detection needed.
    //
    // Overflow headers: Objects with >254 slots have a 2-word header:
    //   [overflow word: 0xFF in top byte, actual slot count in low 56 bits]
    //   [main header: numSlots=255, classIndex, format, hash]
    //   [slots...]
    // BOTH the overflow word and main header have 0xFF in byte 7 (numSlots position).
    // When we see numSlots=255, we peek at the next word. If it also has numSlots=255,
    // then current = overflow word, next = main header. Otherwise, current IS the
    // main header and the overflow word was at the previous position.
    uint8_t* scan = loadedData_;
    uint8_t* end = loadedData_ + loadedSize_;
    size_t objectNum = 0;

    while (scan < end) {
        uint64_t* wordPtr = reinterpret_cast<uint64_t*>(scan);
        uint64_t word = *wordPtr;

        // Skip zero words (padding / segment bridges)
        if (word == 0) {
            scan += 8;
            continue;
        }

        // Check for overflow header: in Spur, large objects (>254 slots) have:
        //   [overflow word] [header with numSlots=255] [slots...]
        // The overflow word has the actual slot count in the low 56 bits AND
        // 0xFF in its top byte (same as the main header's numSlots=255 marker).
        // We detect it by checking if the NEXT word has numSlots=255.
        uint64_t* headerPtr = wordPtr;
        uint64_t header = word;
        size_t slotCount;
        size_t totalHeaderSize = 8;  // Just the main header

        uint8_t numSlotsField = spurNumSlots(header);

        if (numSlotsField == 255) {
            // This could be an overflow word (the count) preceding a header.
            // Check the next word to see if IT also has numSlots=255 (the actual header).
            if (scan + 8 < end) {
                uint64_t nextWord = *(wordPtr + 1);
                if (spurNumSlots(nextWord) == 255) {
                    // Current word is the overflow count, next word is the real header
                    slotCount = static_cast<size_t>((word << 8) >> 8);  // Low 56 bits
                    headerPtr = wordPtr + 1;
                    header = nextWord;
                    totalHeaderSize = 16;  // overflow word + header
                } else {
                    // numSlots=255 but next word doesn't match — treat current as the header
                    // with the PREVIOUS word as overflow count (if available)
                    if (scan > loadedData_) {
                        uint64_t prevWord = *(wordPtr - 1);
                        slotCount = static_cast<size_t>((prevWord << 8) >> 8);
                    } else {
                        slotCount = 0;
                    }
                }
            } else {
                slotCount = 0;
            }
        } else {
            slotCount = numSlotsField;
        }

        // Compute object size: header(s) + slots * 8, minimum 16 bytes (Spur invariant)
        size_t bodySize = slotCount * 8;
        size_t objectBytes = totalHeaderSize + bodySize;
        if (objectBytes < 16) objectBytes = 16;
        objectBytes = (objectBytes + 7) & ~7ULL;

        // Sanity check: object must fit within the heap
        if (objectBytes > static_cast<size_t>(end - scan)) {
            // Possible corruption or end-of-heap alignment — stop scanning
            break;
        }

        // Skip free chunks (classIndex == 0) but still advance by their size
        uint32_t classIndex = spurClassIndex(header);
        if (classIndex == 0) {
            scan += objectBytes;
            continue;
        }

        objectNum++;
        callback(headerPtr, objectBytes);
        scan += objectBytes;
    }
}

} // namespace pharo

#endif // PHARO_IMAGE_LOADER_HPP
