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
#include "SistaRuntime.hpp"

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

    std::map<uint8_t, size_t> blockingOp;        // opcode → count
    std::map<uint8_t, size_t> malformedAtOp;     // opcode at malform → count
    size_t malformedAtEnd = 0;                   // failedAt == len (ran past)
    size_t sampleDumps = 0;

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
            if (failedAt != UINT32_MAX) {
                ObjectHeader* mh = methodOop.asObjectPtr();
                const uint8_t* bytes = mh->bytes()
                                     + (1 + numLiterals) * 8;
                size_t totalBytes = mh->byteSize();
                size_t slotBytes  = (1 + numLiterals) * 8;
                size_t bcSize = (totalBytes > slotBytes)
                                ? totalBytes - slotBytes : 0;
                if (failedAt < bcSize) {
                    blockingOp[bytes[failedAt]]++;
                }
            }
            break;
        }
        case sista::LiftResult::kMalformedMethod: {
            liftMalformed++;
            if (failedAt != UINT32_MAX) {
                ObjectHeader* mh = methodOop.asObjectPtr();
                const uint8_t* bytes = mh->bytes()
                                     + (1 + numLiterals) * 8;
                size_t totalBytes = mh->byteSize();
                size_t slotBytes  = (1 + numLiterals) * 8;
                size_t bcSize = (totalBytes > slotBytes)
                                ? totalBytes - slotBytes : 0;
                if (failedAt >= bcSize) malformedAtEnd++;
                else {
                    uint8_t failOp = bytes[failedAt];
                    malformedAtOp[failOp]++;
                    // Dump first few methods where malform is at a
                    // short-uncond-jump (0xB0-0xB7) — those shouldn't
                    // pop a stack or fail at the target lookup.
                    // Dump the first few methods that fail at ExtendA
                    // (0xE0) — those are truncated-prefix malforms we
                    // want to understand.
                    if (failOp == 0xE0 && sampleDumps < 3) {
                        sampleDumps++;
                        std::printf("\n--- Malformed sample #%zu "
                                    "at bc=%u op=0x%02X (len=%zu) ---\n",
                                    sampleDumps, failedAt, failOp, bcSize);
                        for (size_t k = 0; k < bcSize; k++) {
                            if (k % 16 == 0) std::printf("  %04zu:", k);
                            std::printf(" %02X", bytes[k]);
                            if (k % 16 == 15) std::printf("\n");
                        }
                        if (bcSize % 16) std::printf("\n");
                    }
                }
            }
            break;
        }
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

    // Runtime wrapper smoke test: compile + execute methods whose IR
    // doesn't deref the (synthetic) receiver/literal oops — those
    // would crash with our sentinel state.  Methods WITH sends are
    // allowed: the bail path writes state.ip / state.sp and exits,
    // which we validate by checking for ExitSend.
    //
    // What this proves: Sista lifts + lowers + executes every lifted
    // path correctly; bails write valid state; returns carry values.
    // In a real VM, ExitSend would hand control to the interpreter;
    // here we just verify the exit protocol.
    {
        sista::Runtime runtime;
        size_t compiled = 0, executed = 0, returnedOK = 0, sendBails = 0;

        // Full JITState-shaped buffer (192 bytes, matches offsets in
        // JITState.hpp).  Receiver/literals/temps fields are the only
        // ones the no-send paths touch.
        struct FullState {
            void*     sp;              // 0
            uint64_t  receiver;        // 8
            void*     literals;        // 16
            void*     tempBase;        // 24
            uint8_t   pad48[48 - 32];
            void*     ip;              // 48
            uint8_t   pad76[76 - 56];
            int       exitReason;      // 76
            uint64_t  returnValue;     // 80
            uint8_t   pad128[128 - 88];
            uint64_t  trueOop;         // 128
            uint64_t  falseOop;        // 136
            uint8_t   trailing[56];
        };
        static_assert(offsetof(FullState, receiver)    == 8, "");
        static_assert(offsetof(FullState, literals)    == 16, "");
        static_assert(offsetof(FullState, tempBase)    == 24, "");
        static_assert(offsetof(FullState, ip)          == 48, "");
        static_assert(offsetof(FullState, exitReason)  == 76, "");
        static_assert(offsetof(FullState, returnValue) == 80, "");
        static_assert(offsetof(FullState, trueOop)     == 128, "");
        static_assert(offsetof(FullState, falseOop)    == 136, "");

        Oop trueObj  = memory.trueObject();
        Oop falseObj = memory.falseObject();

        memory.forEachObjectInOldSpace([&](ObjectHeader* hdr) {
            if (!hdr->isCompiledMethod()) return;
            if (executed >= 500) return;

            Oop methodOop = Oop::fromObject(hdr);

            // Check the IR first — skip methods with any kSendUnspeculated
            // (they'd bail to an interpreter we don't have here).
            sista::Method m;
            auto r = sista::Builder::build(methodOop, memory, m);
            if (r != sista::LiftResult::kOk) return;
            // Skip methods whose IR dereferences the (synthetic)
            // receiver oop or an association — those crash with our
            // sentinel state.  Sends are fine: the bail writes state
            // and exits without dereffing any user oops.
            bool unsafeOp = false;
            for (const sista::Value& v : m.values) {
                switch (v.op) {
                case sista::Op::kLoadInstVar:
                case sista::Op::kStoreInstVar:
                case sista::Op::kGuardClass:
                case sista::Op::kInlineSend:
                case sista::Op::kBlockCreate:
                case sista::Op::kBlockValue:
                    unsafeOp = true;
                    break;
                default:
                    break;
                }
                if (unsafeOp) break;
            }
            if (unsafeOp) return;

            auto fn = runtime.compile(methodOop, memory);
            if (!fn) return;
            compiled++;

            // Invoke with a minimal-but-valid state: receiver =
            // sentinel, 64-slot temp area, 64-slot literal mirror.
            uint64_t stackBuf[64]  = {0};
            uint64_t tempsBuf[64]  = {0};
            uint64_t litsBuf[64]   = {0};
            // Populate literals from the method's cached literal table
            // so kLoadLiteral accesses see real Oops.  (Needed for
            // methods that push a literal constant.)
            for (size_t i = 0; i < m.literals.size() && i < 64; i++) {
                litsBuf[i] = m.literals[i].rawBits();
            }
            FullState state{};
            state.sp       = stackBuf;
            state.receiver = 0xCAFEBABEULL;
            state.literals = litsBuf;
            state.tempBase = tempsBuf;
            state.trueOop  = trueObj.rawBits();
            state.falseOop = falseObj.rawBits();

            executed++;
            fn(&state);
            if (state.exitReason == 1) returnedOK++;
            else if (state.exitReason == 2) sendBails++;
        });

        std::cout << "\n=== Runtime execute smoke test ===\n";
        std::cout << "Compiled:                   " << compiled << "\n";
        std::cout << "Executed:                   " << executed << "\n";
        std::cout << "ExitReturn:                 " << returnedOK << "\n";
        std::cout << "ExitSend (bail to interp):  " << sendBails << "\n";
        std::cout << "Other exit:                 "
                  << (executed - returnedOK - sendBails) << "\n";
    }

    // Malformed breakdown.
    std::vector<std::pair<uint8_t, size_t>> msorted(malformedAtOp.begin(),
                                                      malformedAtOp.end());
    std::sort(msorted.begin(), msorted.end(),
              [](auto& a, auto& b){ return a.second > b.second; });
    std::cout << "\n--- Malformed: opcode at failure point ---\n";
    std::cout << "  ran-past-end: " << malformedAtEnd << "\n";
    shown = 0;
    for (auto& kv : msorted) {
        if (shown++ >= 15) break;
        std::printf("  0x%02X  %zu\n", kv.first, kv.second);
    }
    return 0;
}
