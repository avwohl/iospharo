/*
 * ImageWriter.cpp - Spur 64-bit Image File Writer
 *
 * Writes the heap to disk in standard Spur format by converting iOS
 * immediate tags back to standard Spur encoding. Uses atomic write
 * (write to .tmp, then rename) to avoid corrupting the image on crash.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 *
 * The Spur image format is defined by the Pharo project (https://pharo.org)
 * and OpenSmalltalk-VM. See THIRD_PARTY_LICENSES for upstream license details.
 */

#include "ImageWriter.hpp"
#include "ObjectHeader.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace pharo {

// ===== TAG CONVERSION =====

uint64_t ImageWriter::iosToSpurTag(uint64_t bits) {
    if (bits == 0) return 0;  // nil/zero passes through

    uint64_t tag = bits & 7;
    switch (tag) {
        case 1:  // SmallInteger (001) — same in both encodings
            return bits;
        case 3:  // iOS Character (011) → Spur Character (010)
            return (bits & ~7ULL) | 2;
        case 5:  // iOS SmallFloat (101) → Spur SmallFloat (100)
            return (bits & ~7ULL) | 4;
        case 0:  // Object pointer (000) — pass through
            return bits;
        default: // Unknown tag — pass through unchanged
            return bits;
    }
}

// ===== HEADER WRITING =====

bool ImageWriter::writeHeader(std::ofstream& file, const SpurImageHeader& header) {
    // Write the struct directly — its binary layout matches the Spur image format
    // (same struct used by ImageLoader::readHeader for reading).
    // Then pad to 128 bytes (the standard padded header size).
    static_assert(sizeof(SpurImageHeader) <= 128, "Header struct exceeds 128 bytes");

    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &header, sizeof(SpurImageHeader));

    file.write(reinterpret_cast<const char*>(buf), 128);
    return file.good();
}

// ===== HEAP DATA WRITING =====

bool ImageWriter::writeHeapData(std::ofstream& file, ObjectMemory& memory) {
    uint8_t* heapStart = memory.oldSpaceStart();
    uint8_t* heapEnd = memory.oldSpaceFree();
    size_t heapBytes = static_cast<size_t>(heapEnd - heapStart);

    if (heapBytes == 0) return true;

    // Phase 1: Build bitmap of which 64-bit words are pointer slots.
    // We mark slots that may contain Oops (pointer fields, method literals)
    // so we know which words need tag conversion when writing.
    size_t wordCount = heapBytes / 8;
    std::vector<bool> isPointerSlot(wordCount, false);

    ObjectScanner scanner(heapStart, heapEnd);
    ObjectHeader* obj;
    while ((obj = scanner.next()) != nullptr) {
        uint8_t* objAddr = reinterpret_cast<uint8_t*>(obj);
        if (objAddr < heapStart || objAddr >= heapEnd) break;

        uint32_t classIdx = obj->classIndex();
        if (classIdx == 0) continue;  // Free chunk — skip

        ObjectFormat fmt = obj->format();
        size_t slotCount = obj->slotCount();

        // Pointer to first slot (right after header)
        uint8_t* slotsStart = objAddr + 8;
        size_t firstSlotWord = static_cast<size_t>(slotsStart - heapStart) / 8;

        if (firstSlotWord + slotCount > wordCount) {
            // Object extends past heapEnd — truncate
            slotCount = wordCount - firstSlotWord;
        }

        uint8_t fmtVal = static_cast<uint8_t>(fmt);

        if (fmtVal <= 5 || fmtVal == 9) {
            // Pointer objects (formats 0-5) and Indexable64 (format 9, hiddenRoots)
            // All slots contain Oops
            for (size_t i = 0; i < slotCount; i++) {
                isPointerSlot[firstSlotWord + i] = true;
            }
        } else if (fmtVal >= 24 && fmtVal <= 31) {
            // CompiledMethod: slot 0 = method header (SmallInteger),
            // slots 1..numLiterals = literal Oops,
            // rest = bytecodes (raw bytes, not Oops)
            if (slotCount > 0) {
                // Slot 0 is the method header SmallInteger — it's an immediate, mark it
                isPointerSlot[firstSlotWord] = true;

                // Read numLiterals from method header
                uint64_t* slots = reinterpret_cast<uint64_t*>(slotsStart);
                uint64_t headerWord = slots[0];
                // Method header is a SmallInteger: (value << 3) | 1
                // numLiterals = value & 0x7FFF (bits 0-14 of the integer value)
                if (headerWord & 1) {  // Is SmallInteger
                    int64_t headerValue = static_cast<int64_t>(headerWord) >> 3;
                    size_t numLiterals = static_cast<size_t>(headerValue & 0x7FFF);
                    // Mark literal slots (1..numLiterals)
                    size_t pointerSlots = std::min(numLiterals + 1, slotCount);
                    for (size_t i = 1; i < pointerSlots; i++) {
                        isPointerSlot[firstSlotWord + i] = true;
                    }
                }
            }
        }
        // Formats 10-23 (byte/word/short indexable) — no pointer slots, skip
    }

    // Phase 2: Write heap word-by-word, applying tag conversion to pointer slots
    const uint64_t* heapWords = reinterpret_cast<const uint64_t*>(heapStart);

    // Use a write buffer for efficiency
    static constexpr size_t BufWords = 8192;
    uint64_t writeBuf[BufWords];
    size_t bufIdx = 0;

    for (size_t i = 0; i < wordCount; i++) {
        uint64_t word = heapWords[i];
        if (isPointerSlot[i]) {
            word = iosToSpurTag(word);
        }
        writeBuf[bufIdx++] = word;
        if (bufIdx == BufWords) {
            file.write(reinterpret_cast<const char*>(writeBuf), BufWords * 8);
            if (!file.good()) return false;
            bufIdx = 0;
        }
    }

    // Flush remaining
    if (bufIdx > 0) {
        file.write(reinterpret_cast<const char*>(writeBuf), bufIdx * 8);
        if (!file.good()) return false;
    }

    return true;
}

// ===== MAIN SAVE FUNCTION =====

SaveResult ImageWriter::save(const std::string& path, ObjectMemory& memory,
                             const SpurImageHeader& originalHeader,
                             uint32_t lastHash, int screenWidth, int screenHeight) {
    SaveResult result{false, ""};

    // Check for perm space usage — fail if non-empty (matches reference VM)
    if (memory.permSpaceStart() != memory.permSpaceEnd()) {
        // Check if there are actually objects in perm space
        ObjectScanner permScanner(memory.permSpaceStart(), memory.permSpaceEnd());
        if (permScanner.next() != nullptr) {
            result.error = "Permanent space is non-empty; snapshot not supported";
            return result;
        }
    }

    // Build the header
    SpurImageHeader header{};
    uint8_t* heapStart = memory.oldSpaceStart();
    uint8_t* heapFree = memory.oldSpaceFree();
    size_t heapBytes = static_cast<size_t>(heapFree - heapStart);

    header.imageFormat = originalHeader.imageFormat;
    header.headerSize = 128;  // Standard padded size
    header.imageBytes = heapBytes;
    header.startOfMemory = reinterpret_cast<uint64_t>(heapStart);
    // specialObjectsOop: the raw pointer bits (no tag conversion needed —
    // it's an object pointer with tag 000)
    header.specialObjectsOop = memory.specialObjectsArray().rawBits();
    header.lastHash = lastHash;
    header.screenSize = (static_cast<uint64_t>(screenWidth) << 16) | static_cast<uint64_t>(screenHeight);
    header.imageHeaderFlags = originalHeader.imageHeaderFlags;
    header.extraVMMemory = originalHeader.extraVMMemory;
    header.numStackPages = originalHeader.numStackPages;
    header.cogCodeSize = originalHeader.cogCodeSize;
    header.edenBytes = originalHeader.edenBytes;
    header.maxExtSemTabSize = originalHeader.maxExtSemTabSize;
    header.unused1 = 0;
    header.firstSegmentBytes = heapBytes;  // Single segment
    header.freeOldSpaceInImage = 0;

    // Write to temporary file
    std::string tmpPath = path + ".tmp";
    std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        result.error = "Cannot open " + tmpPath + " for writing";
        return result;
    }

    if (!writeHeader(file, header)) {
        result.error = "Failed to write image header";
        file.close();
        std::remove(tmpPath.c_str());
        return result;
    }

    if (!writeHeapData(file, memory)) {
        result.error = "Failed to write heap data";
        file.close();
        std::remove(tmpPath.c_str());
        return result;
    }

    file.close();
    if (!file.good()) {
        result.error = "Error closing temporary file";
        std::remove(tmpPath.c_str());
        return result;
    }

    // Atomic rename
    if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
        result.error = "Failed to rename " + tmpPath + " to " + path + ": " + strerror(errno);
        std::remove(tmpPath.c_str());
        return result;
    }

#ifdef DEBUG
    fprintf(stderr, "[ImageWriter] Saved %zu bytes to %s\n", heapBytes, path.c_str());
#endif
    result.success = true;
    return result;
}

} // namespace pharo
