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
    uint64_t screenSize;          // width << 32 | height
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
        // Typical images have < 100k classes
        if (classIndex > 0x100000) return false;

        // Reserved bits between format and hash (bits 29-31) should usually be 0
        // But this isn't a hard requirement, so we don't check

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
    uint8_t* scan = loadedData_;
    uint8_t* end = loadedData_ + loadedSize_;
    size_t objectNum = 0;

    // std::cerr << "[DEBUG] forEachObject: oldBase=0x" << std::hex << oldBase_
    //           << " size=" << std::dec << loadedSize_ << std::endl;

    while (scan < end) {
        uint64_t* headerPtr = reinterpret_cast<uint64_t*>(scan);
        uint64_t header = *headerPtr;

        // Skip zero words (free space / segment bridges)
        if (header == 0) {
            size_t currentOffset = scan - loadedData_;
            // if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
            //     std::cerr << "[SKIP-ZERO] @0x" << std::hex << currentOffset << std::dec << std::endl;
            // }
            scan += 8;
            continue;
        }

        // Check for overflow header: current word is slot count, next word has numSlots=255
        // Large objects (>254 slots) store the actual count in a preceding word
        bool isOverflowObject = false;
        uint64_t overflowSlotCount = 0;
        if (scan + 8 < end) {
            uint64_t nextWord = *(headerPtr + 1);
            // Check if next word looks like a valid overflow header
            // (numSlots byte = 255, and it's a valid header structure)
            if (spurNumSlots(nextWord) == 255 && looksLikeValidHeader(nextWord)) {
                // Current word is the overflow slot count
                // Check 1: High 32 bits = 0 means full 64-bit value is the count
                if (header >= 255 && header <= 1000000 && (header >> 32) == 0) {
                    overflowSlotCount = header;
                }
                // Check 2: Low 32 bits might contain the count (handles 0xff00000000000400)
                else {
                    uint32_t lowBits = static_cast<uint32_t>(header);
                    if (lowBits >= 255 && lowBits <= 65536) {
                        overflowSlotCount = lowBits;
                    }
                }

                if (overflowSlotCount > 0) {
                    // Calculate how much space the object would need
                    size_t objectBytes = 8 + 8 + overflowSlotCount * 8;  // overflow + header + slots
                    size_t remaining = end - scan;
                    // Validate: fits in remaining space
                    if (objectBytes <= remaining) {
                        isOverflowObject = true;
                        // Move to the actual header
                        headerPtr = headerPtr + 1;
                        header = *headerPtr;
                        scan += 8;  // Account for the overflow word
                    }
                }
            }
        }

        // Handle case where current header has numSlots=255
        // This could be: (a) object with exactly 255 slots, or (b) we ARE at an overflow header
        if (!isOverflowObject && spurNumSlots(header) == 255) {
            // Check if PREVIOUS word could be an overflow slot count
            if (scan > loadedData_) {
                uint64_t prevWord = *(headerPtr - 1);
                // Check 1: High 32 bits = 0 means it's definitely a raw count
                if (prevWord >= 255 && prevWord <= 1000000 && (prevWord >> 32) == 0) {
                    size_t objectBytes = 8 + 8 + prevWord * 8;
                    size_t remaining = end - scan + 8;
                    if (objectBytes <= remaining) {
                        isOverflowObject = true;
                        overflowSlotCount = prevWord;
                    }
                }
                // Check 2: Low 32 bits might contain valid count (handles 0xff00000000000400)
                else {
                    uint32_t lowBits = static_cast<uint32_t>(prevWord);
                    if (lowBits >= 255 && lowBits <= 65536) {
                        size_t objectBytes = 8 + 8 + lowBits * 8;
                        size_t remaining = end - scan + 8;
                        if (objectBytes <= remaining) {
                            isOverflowObject = true;
                            overflowSlotCount = lowBits;
                        }
                    }
                }
            }

            // If still not overflow, check if this looks like a valid header for a 255-slot object
            if (!isOverflowObject && looksLikeValidHeader(header)) {
                // Could be a real object with exactly 255 slots - let it through
                // The objectSize function will handle it correctly
            } else if (!isOverflowObject) {
                // Not a valid header, skip it
                scan += 8;
                continue;
            }
        }

        // Skip values that look like old-space pointers (not headers)
        // Old-space pointers are in the range [oldBase_, oldBase_ + loadedSize_)
        size_t currentOffset = scan - loadedData_;
        uint64_t aligned = header & ~7ULL;
        if (aligned >= oldBase_ && aligned < (oldBase_ + loadedSize_)) {
            if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
                std::cerr << "[SKIP-PTR] @0x" << std::hex << currentOffset
                          << " val=0x" << header << std::dec << std::endl;
            }
            scan += 8;
            continue;
        }

        // Extract and validate class index and format using CORRECT layout
        uint32_t classIndex = spurClassIndex(header);
        uint8_t format = spurFormat(header);
        uint32_t hash = spurHash(header);
        uint8_t numSlots = spurNumSlots(header);

        // VALIDATION: With correct Spur layout, validate the header
        // numSlots in bits 56-63, format in bits 24-28, classIndex in bits 0-21

        // Class index 0 is valid (free chunks), but classIndex > 1M is suspicious
        if (classIndex > 0x100000) {
            if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
                std::cerr << "[SKIP-CLSIDX] @0x" << std::hex << currentOffset
                          << " val=0x" << header << " cls=" << std::dec << classIndex << std::endl;
            }
            scan += 8;
            continue;
        }

        // Format must be 0-31
        if (format > 31) {
            if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
                std::cerr << "[SKIP-FMT] @0x" << std::hex << currentOffset
                          << " val=0x" << header << " fmt=" << std::dec << (int)format << std::endl;
            }
            scan += 8;
            continue;
        }

        // Reserved bits 22-23 and 29-31 should typically be 0
        uint64_t reservedBits = (header >> 22) & 0x3 | (header >> 29) & 0x7;
        if (reservedBits != 0) {
            // Some reserved bits set - could be invalid
            // But don't reject outright as some images may use these
        }

        // Additional check: ASCII text in bytes 0-6 often creates suspicious patterns
        // Check if ALL bytes 1-6 are printable ASCII - very likely string data, not a header
        bool looksLikeAscii = true;
        for (int i = 1; i <= 6; i++) {
            uint8_t b = (header >> (i * 8)) & 0xFF;
            if (b != 0 && (b < 0x20 || b > 0x7e)) {
                looksLikeAscii = false;
                break;
            }
        }

        // Skip if ALL of bytes 1-6 are printable ASCII and slot count byte is also ASCII
        uint8_t slotByte = header & 0xFF;
        bool slotByteIsAscii = (slotByte >= 0x20 && slotByte <= 0x7e);
        if (looksLikeAscii && slotByteIsAscii && classIndex > 0) {
            if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
                std::cerr << "[SKIP-ASCII] @0x" << std::hex << currentOffset
                          << " val=0x" << header << " fmt=" << std::dec << (int)format
                          << " cls=" << classIndex << std::endl;
            }
            scan += 8;
            continue;
        }

        // Check for free chunks: classIndex 0 with a valid structure
        // Free chunks in Spur have format 0 (pointer object) and reasonable slot count
        if (classIndex == 0 && format <= 5) {
            // Valid free chunk should have a reasonable header structure
            // In the correct layout, numSlots is in bits 56-63
            if (numSlots > 0) {
                // Additional check: verify next few words are NOT ASCII data
                bool nextIsAscii = true;
                uint64_t* nextPtr = headerPtr + 1;
                for (int i = 0; i < 2 && (scan + (i+1)*8 < end); i++) {
                    uint64_t nextVal = nextPtr[i];
                    for (int j = 0; j < 8; j++) {
                        uint8_t b = (nextVal >> (j * 8)) & 0xFF;
                        if (b != 0 && (b < 0x20 || b > 0x7e)) {
                            nextIsAscii = false;
                            break;
                        }
                    }
                    if (!nextIsAscii) break;
                }
                if (nextIsAscii) {
                    // Following data is ASCII, this isn't a real free chunk
                    if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
                        std::cerr << "[SKIP-FAKE-FREE] @0x" << std::hex << currentOffset
                                  << " val=0x" << header << std::dec << std::endl;
                    }
                    scan += 8;
                    continue;
                }
                // This looks like a genuine free chunk - skip the entire chunk
                size_t freeChunkSize = objectSize(headerPtr);
                if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
                    std::cerr << "[SKIP-FREE] @0x" << std::hex << currentOffset
                              << " val=0x" << header << " slots=" << std::dec << (int)numSlots
                              << " sz=" << freeChunkSize << std::endl;
                }
                if (freeChunkSize > 0 && freeChunkSize <= (end - scan)) {
                    scan += freeChunkSize;
                } else {
                    scan += 8;
                }
                continue;
            }
            // Not a valid free chunk, just skip 8 bytes
            if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
                std::cerr << "[SKIP-CLS0] @0x" << std::hex << currentOffset
                          << " val=0x" << header << std::dec << std::endl;
            }
            scan += 8;
            continue;
        }

        // Validate format
        // Formats 6, 7, 8 are reserved and shouldn't appear
        // Note: Format 0 CAN have slots (it means no fixed instance vars from class def)
        if (format >= 6 && format <= 8) {
            // Reserved format
            if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
                std::cerr << "[SKIP-RSVFMT] @0x" << std::hex << currentOffset
                          << " val=0x" << header << " fmt=" << std::dec << (int)format << std::endl;
            }
            scan += 8;
            continue;
        }

        // Class index 0 is reserved for free chunks (already handled above)
        // Valid class indices are 1 to 0x3FFFFF (about 4 million)
        // No upper limit check needed since we already validated flags and format
        if (classIndex == 0) {
            // if (currentOffset >= 0x6d600 && currentOffset < 0x6de00) {
            //     std::cerr << "[SKIP-CLS0] @0x" << std::hex << currentOffset
            //               << " val=0x" << header << " cls=" << std::dec << classIndex << std::endl;
            // }
            scan += 8;
            continue;
        }

        size_t size = objectSize(headerPtr);
        size_t offset = scan - loadedData_;

        // Debug: print objects near scheduler area
        // if (objectNum < 20 || (offset >= 0x6db00 && offset < 0x6de00)) {
        //     std::cerr << "[OBJ] #" << objectNum
        //               << " @0x" << std::hex << offset
        //               << " hdr=0x" << header
        //               << " slots=" << std::dec << (int)numSlots
        //               << " fmt=" << (int)format
        //               << " cls=" << classIndex
        //               << " sz=" << size
        //               << " end=0x" << std::hex << (offset + size) << std::dec << std::endl;
        // }

        // Sanity check size
        if (size == 0 || size > (end - scan)) {
            std::cerr << "[ERROR] Invalid size " << size << " at offset 0x"
                      << std::hex << offset << std::dec << ", header=0x"
                      << std::hex << header << std::dec << std::endl;
            break;
        }

        objectNum++;
        callback(headerPtr, size);
        scan += size;
    }

    // std::cerr << "[DEBUG] forEachObject done: " << objectNum << " objects" << std::endl;
}

} // namespace pharo

#endif // PHARO_IMAGE_LOADER_HPP
