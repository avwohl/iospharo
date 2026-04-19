/*
 * test_sista_survey.cpp - Run Sista lifter against every CompiledMethod
 * in a loaded image and report coverage statistics.
 *
 * Usage: ./test_sista_survey <path-to-image>
 *
 * Output: counts of (ok, unsupported-bytecode, malformed) across every
 * method, plus the top blocking opcodes with occurrence counts.  Guides
 * where to invest lifter coverage work next.
 */
#include "../../ObjectMemory.hpp"
#include "../../ImageLoader.hpp"
#include "../SistaV1.hpp"
#include "SistaIR.hpp"
#include "SistaBuilder.hpp"
#include "SistaLowering.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

using namespace pharo;

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <image>\n", argv[0]);
        return 1;
    }
    const char* imagePath = argv[1];

    ObjectMemory memory;
    MemoryConfig config;
    config.oldSpaceSize  = 512ULL * 1024 * 1024;
    config.newSpaceSize  = 32 * 1024 * 1024;
    config.permSpaceSize = 8 * 1024 * 1024;
    if (!memory.initialize(config)) {
        std::fprintf(stderr, "init memory failed\n");
        return 1;
    }

    ImageLoader loader;
    LoadResult result = loader.load(imagePath, memory);
    if (!result.success) {
        std::fprintf(stderr, "load failed: %s\n", result.error.c_str());
        return 1;
    }
    std::cout << "Image loaded.\n";

    // Survey.
    size_t totalMethods    = 0;
    size_t liftOk          = 0;
    size_t liftUnsupported = 0;
    size_t liftMalformed   = 0;
    size_t lowerOk         = 0;
    size_t lowerFailed     = 0;

    std::map<uint8_t, size_t> blockingOp;  // opcode → count

    sista::Lowering lowering;

    memory.forEachObjectInOldSpace([&](ObjectHeader* hdr) {
        if (!hdr->isCompiledMethod()) return;
        totalMethods++;

        Oop methodOop = Oop::fromObject(hdr);
        // Header fetch copied from Builder::build — must read method
        // header via memory.fetchPointer to get the SmallInt oop bits.
        Oop hdrSlot = memory.fetchPointer(0, methodOop);
        if (!hdrSlot.isSmallInteger()) return;
        int64_t headerBits = hdrSlot.asSmallInteger();
        uint32_t numLiterals = (uint32_t)(headerBits & 0x7FFF);

        sista::Method m;
        uint32_t failedAt = UINT32_MAX;
        sista::LiftResult r = sista::Builder::build(methodOop, memory, m,
                                                     &failedAt);
        switch (r) {
        case sista::LiftResult::kOk: {
            liftOk++;
            // Try to lower.
            uint32_t failedVal = UINT32_MAX;
            auto fn = lowering.lower(m, &failedVal);
            if (fn) lowerOk++;
            else    lowerFailed++;
            break;
        }
        case sista::LiftResult::kUnsupportedBytecode: {
            liftUnsupported++;
            // Record the blocking opcode.  `failedAt` is the bytecode
            // offset within the method's bytecode stream.
            if (failedAt != UINT32_MAX) {
                ObjectHeader* mh = methodOop.asObjectPtr();
                const uint8_t* bytes = mh->bytes()
                                     + (1 + numLiterals) * 8;
                size_t bcSize = mh->byteSize();
                if (failedAt < bcSize) {
                    blockingOp[bytes[failedAt]]++;
                }
            }
            break;
        }
        case sista::LiftResult::kMalformedMethod:
            liftMalformed++;
            break;
        }
    });

    std::cout << "\n=== Sista Survey ===\n";
    std::cout << "Total CompiledMethods: " << totalMethods << "\n";
    std::cout << "Lift ok:                " << liftOk
              << " (" << (totalMethods ? (100.0 * liftOk / totalMethods) : 0)
              << "%)\n";
    std::cout << "Lift unsupported:       " << liftUnsupported << "\n";
    std::cout << "Lift malformed:         " << liftMalformed << "\n";
    std::cout << "Lower ok:               " << lowerOk << "\n";
    std::cout << "Lower failed:           " << lowerFailed << "\n";

    // Top blocking opcodes.
    std::vector<std::pair<uint8_t, size_t>> sorted(blockingOp.begin(),
                                                    blockingOp.end());
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b){ return a.second > b.second; });
    std::cout << "\n--- Top blocking opcodes (first-unsupported-byte) ---\n";
    size_t shown = 0;
    for (auto& kv : sorted) {
        if (shown++ >= 20) break;
        std::printf("  0x%02X  %zu\n", kv.first, kv.second);
    }
    return 0;
}
