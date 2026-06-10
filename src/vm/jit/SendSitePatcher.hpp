/*
 * SendSitePatcher.hpp - instruction encoders for patched monomorphic
 * send sites (PMS).  docs/patched-ic-design.md §4.
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Hand-rolled encoders for exactly the four instruction forms PMS
 * patches.  asmjit is never used at patch time — that sidesteps its
 * documented silent-drop hazard (memory: asmjit-arm64-invalid-logical-imm)
 * — but every encoder is cross-validated against asmjit output once at
 * startup via sendSitePatcherSelfCheck() (hard-fail on mismatch).
 *
 * Patch words (site layout, design §2):
 *   W0  movz w6, #keyLo16            head: patched class key, low half
 *   W1  movk w6, #keyHi16, lsl 16    head: key high half — THE commit/unlink word
 *   W2  b    <target>                head: b Lprobe (unlinked) / b T (linked)
 *   W3  movz x10, #jmLo16            tail: calleeJM immediate
 *   W4  movk x10, #jmMid16, lsl 16
 *   W5  movk x10, #jmHi16,  lsl 32
 *   W6  b    <calleeEntry>           tail: patched direct call
 */
#ifndef PHARO_SEND_SITE_PATCHER_HPP
#define PHARO_SEND_SITE_PATCHER_HPP

#include <cstdint>

#if PHARO_JIT_ENABLED || !defined(PHARO_JIT_ENABLED)

namespace pharo {
namespace jit {

// An unlinked site's key-hi half-word.  0x0040____ exceeds the 22-bit
// classIndex range and lacks bit 31 (the immediate-receiver marker), so
// it can never match a real lookup key: unlink = ONE aligned store of
// W1.  Design §2.1.  Forbidden forever: any change that makes W2
// reachable without a key match.
static constexpr uint16_t kImpossibleKeyHi16 = 0x0040;

// ===== Encoders (verified against ARM ARM C6.2; cross-checked vs
// asmjit at startup by sendSitePatcherSelfCheck) =====

// movz w6, #imm16        (MOVZ 32-bit, hw=0, Rd=6)
constexpr uint32_t encodeMovzW6(uint16_t imm16) {
    return 0x52800006u | (uint32_t(imm16) << 5);
}
// movk w6, #imm16, lsl 16  (MOVK 32-bit, hw=1, Rd=6)
constexpr uint32_t encodeMovkW6Hi(uint16_t imm16) {
    return 0x72A00006u | (uint32_t(imm16) << 5);
}
// movz x10, #imm16       (MOVZ 64-bit, hw=0, Rd=10)
constexpr uint32_t encodeMovzX10(uint16_t imm16) {
    return 0xD280000Au | (uint32_t(imm16) << 5);
}
// movk x10, #imm16, lsl 16
constexpr uint32_t encodeMovkX10Mid(uint16_t imm16) {
    return 0xF2A0000Au | (uint32_t(imm16) << 5);
}
// movk x10, #imm16, lsl 32
constexpr uint32_t encodeMovkX10Hi(uint16_t imm16) {
    return 0xF2C0000Au | (uint32_t(imm16) << 5);
}
// b <target>   (B, imm26 = (target - pc) >> 2).  Caller must verify
// reach: |target - pc| < 128 MB (design §5: refuse-to-link on failure,
// NEVER assert-and-continue).
constexpr bool branchInRange(int64_t byteDelta) {
    return byteDelta >= -(1ll << 27) && byteDelta < (1ll << 27)
        && (byteDelta & 3) == 0;
}
constexpr uint32_t encodeB(int64_t byteDelta) {
    return 0x14000000u | (uint32_t(byteDelta >> 2) & 0x03FFFFFFu);
}

// ===== Decoders / opcode-class checks (for the pre-patch assert,
// design §4 step 3: verify every word about to be overwritten) =====

inline bool isMovzW6(uint32_t insn)    { return (insn & 0xFFE0001Fu) == 0x52800006u; }
// movk x14, #imm16, lsl 48 — the tail's V2 packed-resume word at T+28;
// linkSendSite decodes the site's nArgs from it for the arity gate.
inline bool isMovkX14Pack(uint32_t insn) { return (insn & 0xFFE0001Fu) == 0xF2E0000Eu; }
inline bool isMovkW6Hi(uint32_t insn)  { return (insn & 0xFFE0001Fu) == 0x72A00006u; }
inline bool isMovzX10(uint32_t insn)   { return (insn & 0xFFE0001Fu) == 0xD280000Au; }
inline bool isMovkX10Mid(uint32_t insn){ return (insn & 0xFFE0001Fu) == 0xF2A0000Au; }
inline bool isMovkX10Hi(uint32_t insn) { return (insn & 0xFFE0001Fu) == 0xF2C0000Au; }
inline bool isB(uint32_t insn)         { return (insn & 0xFC000000u) == 0x14000000u; }

inline uint16_t decodeMovImm16(uint32_t insn) {
    return uint16_t((insn >> 5) & 0xFFFF);
}
// Byte delta encoded in a B instruction (sign-extended imm26 << 2).
inline int64_t decodeBDelta(uint32_t insn) {
    int32_t imm26 = int32_t(insn << 6) >> 6;   // sign-extend 26 bits
    return int64_t(imm26) << 2;
}

// Cross-validate every encoder against asmjit-emitted bytes.  Called
// once from JITRuntime::initialize; aborts on mismatch (a wrong patch
// encoder is silent code corruption — fail loudly at startup instead).
void sendSitePatcherSelfCheck();

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_SEND_SITE_PATCHER_HPP
