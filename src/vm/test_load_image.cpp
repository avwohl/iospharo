/*
 * test_load_image.cpp - Test loading a Pharo image
 *
 * Usage: ./test_load_image <path-to-image>
 */

#include "ObjectMemory.hpp"
#include "ImageLoader.hpp"
#include "Interpreter.hpp"
#include <iostream>
#include <iomanip>

using namespace pharo;

void printHeader(const SpurImageHeader& header) {
    std::cout << "=== Image Header ===" << std::endl;
    std::cout << "Format:            " << header.imageFormat;
    if (header.imageFormat == 68021) std::cout << " (Spur 64-bit)";
    else if (header.imageFormat == 68533) std::cout << " (Spur 64-bit Sista)";
    std::cout << std::endl;

    std::cout << "Header size:       " << header.headerSize << " bytes" << std::endl;
    std::cout << "Image size:        " << header.imageBytes << " bytes ("
              << (header.imageBytes / 1024 / 1024) << " MB)" << std::endl;
    std::cout << "Old base address:  0x" << std::hex << header.startOfMemory
              << std::dec << std::endl;
    std::cout << "Special objects:   0x" << std::hex << header.specialObjectsOop
              << std::dec << std::endl;
    std::cout << "Last hash:         " << header.lastHash << std::endl;

    uint32_t width = static_cast<uint32_t>(header.screenSize >> 32);
    uint32_t height = static_cast<uint32_t>(header.screenSize & 0xFFFFFFFF);
    std::cout << "Screen size:       " << width << " x " << height << std::endl;

    std::cout << "Flags:             0x" << std::hex << header.imageHeaderFlags
              << std::dec << std::endl;
    if (header.imageHeaderFlags & 1) std::cout << "  - Full block closures" << std::endl;
    if (header.imageHeaderFlags & 8) std::cout << "  - Sista V1 bytecodes" << std::endl;

    // Segment info
    std::cout << "First segment:     " << header.firstSegmentBytes << " bytes" << std::endl;
    std::cout << "Free old space:    " << header.freeOldSpaceInImage << " bytes" << std::endl;
    std::cout << "Eden bytes:        " << header.edenBytes << std::endl;
}

void printSpecialObjects(ObjectMemory& memory) {
    std::cout << "\n=== Special Objects ===" << std::endl;

    // Note: In Smalltalk, nil/true/false are actual singleton objects,
    // not special pointer values. So nil.isNil() checks for null pointer (0),
    // but the nil singleton has a real address.

    Oop specialArray = memory.specialObjectsArray();
    std::cout << "Special objects array: 0x" << std::hex << specialArray.rawBits()
              << std::dec << std::endl;

    if (!specialArray.isObject()) {
        std::cout << "ERROR: Special objects array is not valid" << std::endl;
        return;
    }

    ObjectHeader* arrayHeader = specialArray.asObjectPtr();
    uint64_t rawHeader = arrayHeader->rawHeader();
    std::cout << "  Raw header: 0x" << std::hex << rawHeader << std::dec << std::endl;
    std::cout << "  Slot count byte: " << (rawHeader & 0xFF) << std::endl;
    std::cout << "  Hash: " << ((rawHeader >> 8) & 0x3FFFFF) << std::endl;
    std::cout << "  Format: " << ((rawHeader >> 30) & 0x1F) << std::endl;
    std::cout << "  Class index: " << ((rawHeader >> 35) & 0x3FFFFF) << std::endl;
    std::cout << "  Flags: " << ((rawHeader >> 57) & 0x7F) << std::endl;

    size_t slotCount = arrayHeader->slotCount();
    std::cout << "  Computed slot count: " << slotCount << std::endl;

    // Sanity check
    if (slotCount > 1000) {
        std::cout << "ERROR: Slot count too large, header may be corrupt" << std::endl;
        return;
    }

    // Print first few special objects
    std::cout << "\nFirst 10 special objects:" << std::endl;
    for (size_t i = 0; i < 10 && i < slotCount; i++) {
        Oop obj = arrayHeader->slotAt(i);
        std::cout << "  [" << i << "]: 0x" << std::hex << obj.rawBits() << std::dec;
        if (obj.isSmallInteger()) {
            std::cout << " (SmallInteger: " << obj.asSmallInteger() << ")";
        } else if (obj.rawBits() == 0) {
            std::cout << " (nil/zero)";
        } else if (obj.isObject()) {
            // Check if address looks valid
            uint64_t addr = obj.rawBits() & ~7ULL;
            if (memory.isValidHeapAddress(reinterpret_cast<void*>(addr))) {
                std::cout << " (object, class: " << obj.asObjectPtr()->classIndex() << ")";
            } else {
                std::cout << " (INVALID ADDRESS)";
            }
        }
        std::cout << std::endl;
    }
}

void printMemoryStats(ObjectMemory& memory) {
    std::cout << "\n=== Memory Layout ===" << std::endl;
    std::cout << "Old space:  0x" << std::hex
              << reinterpret_cast<uint64_t>(memory.oldSpaceStart())
              << " - 0x"
              << reinterpret_cast<uint64_t>(memory.oldSpaceEnd())
              << std::dec << std::endl;

    auto stats = memory.statistics();
    std::cout << "Allocated:  " << (stats.bytesAllocated / 1024 / 1024) << " MB" << std::endl;
    std::cout << "Free:       " << (stats.bytesFree / 1024 / 1024) << " MB" << std::endl;

    // Debug: show first few bytes at start of old space
    std::cout << "\nFirst 128 bytes of old space (raw):" << std::endl;
    uint64_t* ptr = reinterpret_cast<uint64_t*>(memory.oldSpaceStart());
    for (int i = 0; i < 16; i++) {
        std::cout << "  [" << (i * 8) << "]: 0x" << std::hex << ptr[i] << std::dec << std::endl;
    }

    // Try to interpret first object
    ObjectHeader* firstObj = reinterpret_cast<ObjectHeader*>(memory.oldSpaceStart());
    uint64_t header = firstObj->rawHeader();
    std::cout << "\nFirst object header analysis:" << std::endl;
    std::cout << "  Raw: 0x" << std::hex << header << std::dec << std::endl;
    if (header != 0) {
        std::cout << "  Slot count: " << (header & 0xFF) << std::endl;
        std::cout << "  Format: " << ((header >> 30) & 0x1F) << std::endl;
        std::cout << "  Class index: " << ((header >> 35) & 0x3FFFFF) << std::endl;
    }
}

void countObjects(ObjectMemory& memory) {
    std::cout << "\n=== Object Statistics ===" << std::endl;

    size_t totalObjects = 0;
    size_t pointerObjects = 0;
    size_t byteObjects = 0;
    size_t methodObjects = 0;
    size_t totalBytes = 0;

    memory.allObjectsDo([&](Oop obj) {
        if (!obj.isObject()) return;

        ObjectHeader* header = obj.asObjectPtr();
        totalObjects++;
        size_t objSize = header->totalSize();

        // Debug: catch objects with suspicious sizes
        if (objSize > 100 * 1024 * 1024) {  // > 100 MB
            std::cout << "WARNING: Huge object at 0x" << std::hex
                      << reinterpret_cast<uint64_t>(header) << std::dec
                      << " size=" << objSize
                      << " slots=" << header->slotCount()
                      << " format=" << static_cast<int>(header->format())
                      << " raw=0x" << std::hex << header->rawHeader() << std::dec
                      << std::endl;
        }

        totalBytes += objSize;

        if (header->isBytesObject()) {
            byteObjects++;
        } else if (header->isCompiledMethod()) {
            methodObjects++;
        } else if (header->isPointersObject()) {
            pointerObjects++;
        }
    });

    std::cout << "Total objects:    " << totalObjects << std::endl;
    std::cout << "Pointer objects:  " << pointerObjects << std::endl;
    std::cout << "Byte objects:     " << byteObjects << std::endl;
    std::cout << "Compiled methods: " << methodObjects << std::endl;
    std::cout << "Total size:       " << (totalBytes / 1024 / 1024) << " MB" << std::endl;
}

void testOopTagging() {
    std::cout << "\n=== Oop Tagging Tests ===" << std::endl;

    // Test SmallInteger
    Oop si = Oop::fromSmallInteger(42);
    std::cout << "SmallInteger 42:  0x" << std::hex << si.rawBits() << std::dec;
    std::cout << " isSmallInteger=" << si.isSmallInteger();
    std::cout << " value=" << si.asSmallInteger() << std::endl;

    // Test negative SmallInteger
    Oop neg = Oop::fromSmallInteger(-100);
    std::cout << "SmallInteger -100: 0x" << std::hex << neg.rawBits() << std::dec;
    std::cout << " value=" << neg.asSmallInteger() << std::endl;

    // Test Character
    Oop ch = Oop::fromCharacter('A');
    std::cout << "Character 'A':    0x" << std::hex << ch.rawBits() << std::dec;
    std::cout << " isCharacter=" << ch.isCharacter();
    std::cout << " codepoint=" << ch.asCharacter() << std::endl;

    // Test nil
    Oop nil = Oop::nil();
    std::cout << "nil:              0x" << std::hex << nil.rawBits() << std::dec;
    std::cout << " isNil=" << nil.isNil() << std::endl;

    // Verify tagging uses low bits (iOS ASLR compatible)
    std::cout << "\nTag verification (should use low 3 bits only):" << std::endl;
    std::cout << "  SmallInteger tag: " << (si.rawBits() & 7) << " (expected 1)" << std::endl;
    std::cout << "  Character tag:    " << (ch.rawBits() & 7) << " (expected 3)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "Pharo Clean VM - Image Loader Test" << std::endl;
    std::cout << "===================================" << std::endl;

    // Run tagging tests first
    testOopTagging();

    if (argc < 2) {
        std::cout << "\nUsage: " << argv[0] << " <path-to-image>" << std::endl;
        std::cout << "\nNo image specified, skipping image load test." << std::endl;
        return 0;
    }

    const char* imagePath = argv[1];
    std::cout << "\nLoading image: " << imagePath << std::endl;

    // Initialize memory (256 MB should be enough for most images)
    ObjectMemory memory;
    MemoryConfig config;
    config.oldSpaceSize = 256 * 1024 * 1024;  // 256 MB
    config.newSpaceSize = 32 * 1024 * 1024;   // 32 MB
    config.permSpaceSize = 8 * 1024 * 1024;   // 8 MB

    if (!memory.initialize(config)) {
        std::cerr << "ERROR: Failed to initialize memory" << std::endl;
        return 1;
    }
    std::cout << "Memory initialized successfully" << std::endl;

    // Load the image
    ImageLoader loader;
    LoadResult result = loader.load(imagePath, memory);

    if (!result.success) {
        std::cerr << "ERROR: Failed to load image: " << result.error << std::endl;
        return 1;
    }

    std::cout << "Image loaded successfully!" << std::endl;

    // Print information
    printHeader(loader.header());
    printSpecialObjects(memory);
    printMemoryStats(memory);
    countObjects(memory);

    // Try to initialize interpreter
    std::cout << "\n=== Interpreter Initialization ===" << std::endl;
    Interpreter interpreter(memory);
    if (interpreter.initialize()) {
        std::cout << "Interpreter initialized successfully!" << std::endl;
        std::cout << "Active method: 0x" << std::hex
                  << interpreter.activeMethod().rawBits() << std::dec << std::endl;

        // Run bytecode steps for testing
        std::cout << "\n=== Execution Test ===" << std::endl;
        std::cout << "Running 10000 bytecode steps..." << std::endl;
        int activeSteps = 0;
        int idleSteps = 0;
        for (int i = 0; i < 10000; i++) {
            if (i % 1000 == 0) {
                std::cout << "[Step " << (i+1) << "] active=" << activeSteps << " idle=" << idleSteps << std::endl;
            }
            if (interpreter.step()) {
                activeSteps++;
            } else {
                idleSteps++;
            }
        }
        std::cout << "Execution completed: " << activeSteps << " active steps, " << idleSteps << " idle steps" << std::endl;
    } else {
        std::cout << "Interpreter initialization failed (may need process setup)" << std::endl;
    }

    std::cout << "\n=== Test Complete ===" << std::endl;
    return 0;
}
