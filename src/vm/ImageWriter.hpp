/*
 * ImageWriter.hpp - Spur 64-bit Image File Writer
 *
 * Writes the current heap to a Spur image file, converting iOS immediate
 * tags back to standard Spur format so the saved image is portable.
 *
 * WRITING PROCESS:
 * 1. Build header from current VM state
 * 2. Write to temporary file (atomic save)
 * 3. Scan heap objects, converting pointer slot immediates back to Spur tags
 * 4. Rename temp file to target path
 *
 * TAG CONVERSION (inverse of ImageLoader):
 *   iOS tag 001 (SmallInteger)  → Spur tag 001 (pass through)
 *   iOS tag 011 (Character)     → Spur tag 010
 *   iOS tag 101 (SmallFloat)    → Spur tag 100
 *   iOS tag 000 (Object ptr)    → absolute address (pass through)
 *   Zero (nil)                  → zero (pass through)
 */

#ifndef PHARO_IMAGE_WRITER_HPP
#define PHARO_IMAGE_WRITER_HPP

#include "ObjectMemory.hpp"
#include "ImageLoader.hpp"
#include <string>
#include <cstdint>
#include <fstream>
#include <vector>

namespace pharo {

struct SaveResult {
    bool success;
    std::string error;
};

class ImageWriter {
public:
    SaveResult save(const std::string& path, ObjectMemory& memory,
                    const SpurImageHeader& originalHeader,
                    uint32_t lastHash, int screenWidth, int screenHeight);

private:
    /// Convert a single 64-bit value from iOS tag format back to standard Spur.
    /// This is the exact inverse of ImageLoader::relocatePointer's tag conversion.
    static uint64_t iosToSpurTag(uint64_t bits);

    /// Write the 128-byte image header
    bool writeHeader(std::ofstream& file, const SpurImageHeader& header);

    /// Write heap data, converting pointer slot values back to Spur tags
    bool writeHeapData(std::ofstream& file, ObjectMemory& memory);
};

} // namespace pharo

#endif // PHARO_IMAGE_WRITER_HPP
