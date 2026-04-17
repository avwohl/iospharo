// Sista V1 bytecode opcodes (Pharo 10+).
// See docs/SistaV1-Bytecode-Spec.md for the full spec.
//
// Previously duplicated in JITCompiler.cpp and Tier2Compiler.cpp; the two
// copies had drifted (0xFB/0xFC/0xFD had different names). This is now the
// single source of truth. Other code paths should `#include "SistaV1.hpp"`
// and use `SistaV1::PushReceiver` instead of `0x4C`.
//
// RANGE HELPERS: prefer `SistaV1::isSend(op)` over
// `op >= 0x80 && op <= 0xAF` so the reader sees the intent. See
// ~/fix_constants.txt for the convention.
#pragma once

#include <cstdint>

namespace pharo {
namespace jit {
namespace SistaV1 {

// --- 1-byte ranges -----------------------------------------------------------
constexpr uint8_t PushRecvVarBase       = 0x00; // 0x00-0x0F  pushRecvVar 0..15
constexpr uint8_t PushRecvVarLast       = 0x0F;
constexpr uint8_t PushLitVarBase        = 0x10; // 0x10-0x1F  pushLitVar 0..15
constexpr uint8_t PushLitVarLast        = 0x1F;
constexpr uint8_t PushLitConstBase      = 0x20; // 0x20-0x3F  pushLitConst 0..31
constexpr uint8_t PushLitConstLast      = 0x3F;
constexpr uint8_t PushTempBase          = 0x40; // 0x40-0x4B  pushTemp 0..11
constexpr uint8_t PushTempLast          = 0x4B;

// --- 0x4C-0x5F individual opcodes --------------------------------------------
constexpr uint8_t PushReceiver          = 0x4C;
constexpr uint8_t PushTrue              = 0x4D;
constexpr uint8_t PushFalse             = 0x4E;
constexpr uint8_t PushNil               = 0x4F;
constexpr uint8_t PushZero              = 0x50;
constexpr uint8_t PushOne               = 0x51;
constexpr uint8_t PushThisContext       = 0x52;
constexpr uint8_t Dup                   = 0x53;
// 0x54-0x57: unassigned
constexpr uint8_t ReturnReceiver        = 0x58;
constexpr uint8_t ReturnTrue            = 0x59;
constexpr uint8_t ReturnFalse           = 0x5A;
constexpr uint8_t ReturnNil             = 0x5B;
constexpr uint8_t ReturnTop             = 0x5C;
constexpr uint8_t BlockReturnNil        = 0x5D;
constexpr uint8_t BlockReturnTop        = 0x5E;
// 0x5F: unassigned

// --- Sends -------------------------------------------------------------------
constexpr uint8_t ArithBase             = 0x60; // 0x60-0x6F  arithmetic selector 0..15
constexpr uint8_t ArithLast             = 0x6F;
constexpr uint8_t SpecialSendBase       = 0x70; // 0x70-0x7F  special selector 16..31
constexpr uint8_t SpecialSendLast       = 0x7F;
constexpr uint8_t Send0Base             = 0x80; // 0x80-0x8F  send literal, 0 args
constexpr uint8_t Send0Last             = 0x8F;
constexpr uint8_t Send1Base             = 0x90; // 0x90-0x9F  send literal, 1 arg
constexpr uint8_t Send1Last             = 0x9F;
constexpr uint8_t Send2Base             = 0xA0; // 0xA0-0xAF  send literal, 2 args
constexpr uint8_t Send2Last             = 0xAF;

// --- Jumps -------------------------------------------------------------------
constexpr uint8_t ShortJumpBase         = 0xB0; // 0xB0-0xB7  unconditional, +1..+8
constexpr uint8_t ShortJumpLast         = 0xB7;
constexpr uint8_t ShortJumpTrueBase     = 0xB8; // 0xB8-0xBF  if true
constexpr uint8_t ShortJumpTrueLast     = 0xBF;
constexpr uint8_t ShortJumpFalseBase    = 0xC0; // 0xC0-0xC7  if false
constexpr uint8_t ShortJumpFalseLast    = 0xC7;

// --- Stores ------------------------------------------------------------------
constexpr uint8_t PopStoreRecvBase      = 0xC8; // 0xC8-0xCF  popIntoRecvVar 0..7
constexpr uint8_t PopStoreRecvLast      = 0xCF;
constexpr uint8_t PopStoreTempBase      = 0xD0; // 0xD0-0xD7  popIntoTemp 0..7
constexpr uint8_t PopStoreTempLast      = 0xD7;

// --- Stack ops ---------------------------------------------------------------
constexpr uint8_t Pop                   = 0xD8;
// 0xD9 unconditional trap; 0xDA-0xDF reserved

// --- 2-byte extended bytecodes (0xE0-0xF7) ----------------------------------
constexpr uint8_t ExtendA               = 0xE0;
constexpr uint8_t ExtendB               = 0xE1;
constexpr uint8_t ExtPushRecvVar        = 0xE2;
constexpr uint8_t ExtPushLitVar         = 0xE3;
constexpr uint8_t ExtPushLitConst       = 0xE4;
constexpr uint8_t ExtPushTemp           = 0xE5;
// 0xE6 unassigned
constexpr uint8_t PushArray             = 0xE7;
constexpr uint8_t PushInteger           = 0xE8;
constexpr uint8_t PushCharacter         = 0xE9;
constexpr uint8_t ExtSend               = 0xEA;
constexpr uint8_t ExtSuperSend          = 0xEB;
constexpr uint8_t InlinedPrimitive      = 0xEC;
constexpr uint8_t ExtJump               = 0xED;
constexpr uint8_t ExtJumpTrue           = 0xEE;
constexpr uint8_t ExtJumpFalse          = 0xEF;
constexpr uint8_t ExtPopStoreRecv       = 0xF0;
constexpr uint8_t ExtPopStoreLitVar     = 0xF1;
constexpr uint8_t ExtPopStoreTemp       = 0xF2;
constexpr uint8_t ExtStoreRecv          = 0xF3;
constexpr uint8_t ExtStoreLitVar        = 0xF4;
constexpr uint8_t ExtStoreTemp          = 0xF5;
// 0xF6, 0xF7 unassigned

// --- 3-byte bytecodes (0xF8-0xFD) -------------------------------------------
constexpr uint8_t CallPrimitive         = 0xF8;
constexpr uint8_t PushFullBlock         = 0xF9;
constexpr uint8_t PushClosure           = 0xFA;
constexpr uint8_t PushTempAtInVec       = 0xFB; // (alias: PushRemoteTemp)
constexpr uint8_t StoreTempAtInVec      = 0xFC; // (alias: StoreRemoteTemp)
constexpr uint8_t PopStoreTempAtInVec   = 0xFD; // (alias: PopStoreRemoteTemp)
// 0xFE, 0xFF unassigned

// Compatibility aliases (old names from the pre-merge duplicates):
constexpr uint8_t PushRemoteTemp        = PushTempAtInVec;
constexpr uint8_t StoreRemoteTemp       = StoreTempAtInVec;
constexpr uint8_t PopStoreRemoteTemp    = PopStoreTempAtInVec;

// ============================================================================
// RANGE HELPERS
// ============================================================================
// Prefer these over raw range comparisons. Reading
//   `if (isSend(op))`
// gives the reader the intent — reading
//   `if (op >= 0x80 && op <= 0xAF)`
// leaves them guessing what the group means.

constexpr bool isPushRecvVar(uint8_t op) {
    return op >= PushRecvVarBase && op <= PushRecvVarLast;
}
constexpr bool isPushLitVar(uint8_t op) {
    return op >= PushLitVarBase && op <= PushLitVarLast;
}
constexpr bool isPushLitConst(uint8_t op) {
    return op >= PushLitConstBase && op <= PushLitConstLast;
}
constexpr bool isPushTemp(uint8_t op) {
    return op >= PushTempBase && op <= PushTempLast;
}

// Arithmetic special selectors 0..15 (+, -, <, >, <=, >=, =, ~=, *, etc.)
constexpr bool isArithSelector(uint8_t op) {
    return op >= ArithBase && op <= ArithLast;
}
// Special selectors 16..31 (at:, at:put:, size, class, etc.)
constexpr bool isSpecialSelector(uint8_t op) {
    return op >= SpecialSendBase && op <= SpecialSendLast;
}
// Any send-type bytecode (arith + special + literal + extended).
constexpr bool isSendBytecode(uint8_t op) {
    return (op >= ArithBase && op <= Send2Last)
        || op == ExtSend
        || op == ExtSuperSend;
}
constexpr bool isLiteralSend(uint8_t op) {
    return op >= Send0Base && op <= Send2Last;
}
constexpr bool isSend0(uint8_t op) {
    return op >= Send0Base && op <= Send0Last;
}
constexpr bool isSend1(uint8_t op) {
    return op >= Send1Base && op <= Send1Last;
}
constexpr bool isSend2(uint8_t op) {
    return op >= Send2Base && op <= Send2Last;
}

constexpr bool isShortJump(uint8_t op) {
    return op >= ShortJumpBase && op <= ShortJumpLast;
}
constexpr bool isShortJumpTrue(uint8_t op) {
    return op >= ShortJumpTrueBase && op <= ShortJumpTrueLast;
}
constexpr bool isShortJumpFalse(uint8_t op) {
    return op >= ShortJumpFalseBase && op <= ShortJumpFalseLast;
}
constexpr bool isConditionalShortJump(uint8_t op) {
    return op >= ShortJumpTrueBase && op <= ShortJumpFalseLast;
}
constexpr bool isAnyShortJump(uint8_t op) {
    return op >= ShortJumpBase && op <= ShortJumpFalseLast;
}

constexpr bool isPopStoreRecv(uint8_t op) {
    return op >= PopStoreRecvBase && op <= PopStoreRecvLast;
}
constexpr bool isPopStoreTemp(uint8_t op) {
    return op >= PopStoreTempBase && op <= PopStoreTempLast;
}

constexpr bool isReturn(uint8_t op) {
    return op >= ReturnReceiver && op <= ReturnTop;
}
constexpr bool isBlockReturn(uint8_t op) {
    return op == BlockReturnNil || op == BlockReturnTop;
}

// Range of 2-byte extended bytecodes (0xE0-0xF5; 0xF6/0xF7 unassigned).
constexpr bool isExtended2Byte(uint8_t op) {
    return op >= ExtendA && op <= ExtStoreTemp;
}
// Range of 3-byte bytecodes (0xF8-0xFD; 0xFE/0xFF unassigned).
constexpr bool isThreeByteBytecode(uint8_t op) {
    return op >= CallPrimitive && op <= PopStoreTempAtInVec;
}

}  // namespace SistaV1
}  // namespace jit
}  // namespace pharo
