/*
 * SendSitePatcher.cpp - startup cross-validation of the PMS patch
 * encoders against asmjit.  docs/patched-ic-design.md §4.
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 */
#include "JITConfig.hpp"

#if PHARO_JIT_ENABLED

#include "SendSitePatcher.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <asmjit/a64.h>

namespace pharo {
namespace jit {

namespace {

// Emit one instruction through asmjit and return its word.
template <typename EmitFn>
uint32_t asmjitWord(EmitFn&& emit) {
    asmjit::Environment env(asmjit::Arch::kAArch64);
    asmjit::CodeHolder code;
    code.init(env);
    asmjit::a64::Assembler a(&code);
    emit(a);
    code.flatten();
    uint32_t word = 0;
    code.copy_flattened_data(&word, sizeof(word),
                             asmjit::CopySectionFlags::kPadSectionBuffer);
    return word;
}

void check(const char* what, uint32_t ours, uint32_t theirs) {
    if (ours != theirs) {
        std::fprintf(stderr,
            "[PMS-SELFCHECK] FATAL: %s encoder mismatch: ours=0x%08X asmjit=0x%08X\n",
            what, ours, theirs);
        std::abort();
    }
}

} // namespace

void sendSitePatcherSelfCheck() {
    using namespace asmjit::a64;

    // The five mov forms, cross-validated against asmjit with
    // non-symmetric test immediates.
    check("movz w6",
          encodeMovzW6(0xABCD),
          asmjitWord([](Assembler& a) { a.mov(w6, asmjit::Imm(0xABCD)); }));
    check("movk w6 lsl16",
          encodeMovkW6Hi(0x1234),
          asmjitWord([](Assembler& a) { a.movk(w6, asmjit::Imm(0x1234), 16); }));
    check("movz x10",
          encodeMovzX10(0xBEEF),
          asmjitWord([](Assembler& a) { a.mov(x10, asmjit::Imm(0xBEEF)); }));
    check("movk x10 lsl16",
          encodeMovkX10Mid(0x5678),
          asmjitWord([](Assembler& a) { a.movk(x10, asmjit::Imm(0x5678), 16); }));
    check("movk x10 lsl32",
          encodeMovkX10Hi(0x9ABC),
          asmjitWord([](Assembler& a) { a.movk(x10, asmjit::Imm(0x9ABC), 32); }));

    check("movk x14 lsl48 (pack-word decoder)",
          0xF2E0000Eu | (uint32_t(0x100B) << 5),
          asmjitWord([](Assembler& a) { a.movk(x14, asmjit::Imm(0x100B), 48); }));
    if (!isMovkX14Pack(0xF2E0000Eu | (uint32_t(0x100B) << 5))
        || decodeMovImm16(0xF2E0000Eu | (uint32_t(0x100B) << 5)) != 0x100B) {
        std::fprintf(stderr, "[PMS-SELFCHECK] FATAL: pack-word decoder failed\n");
        std::abort();
    }

    // B: known-good vectors from the ARM ARM (imm26 two's complement).
    check("b +0", encodeB(0),  0x14000000u);
    check("b +4", encodeB(4),  0x14000001u);
    check("b -4", encodeB(-4), 0x17FFFFFFu);

    // Round-trip decoders.
    if (!isMovzW6(encodeMovzW6(7)) || decodeMovImm16(encodeMovzW6(7)) != 7
        || !isMovkW6Hi(encodeMovkW6Hi(kImpossibleKeyHi16))
        || !isMovzX10(encodeMovzX10(1)) || !isMovkX10Mid(encodeMovkX10Mid(2))
        || !isMovkX10Hi(encodeMovkX10Hi(3))
        || !isB(encodeB(-4)) || decodeBDelta(encodeB(-4)) != -4
        || decodeBDelta(encodeB(0x7FFFFFC)) != 0x7FFFFFC
        || branchInRange(1ll << 27) || !branchInRange((1ll << 27) - 4)) {
        std::fprintf(stderr, "[PMS-SELFCHECK] FATAL: decoder round-trip failed\n");
        std::abort();
    }
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
