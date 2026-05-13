/*
 * JITCompiler.cpp - Copy-and-patch JIT compiler implementation
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 */

#include "JITCompiler.hpp"
#include "PlatformJIT.hpp"
#include "SistaV1.hpp"
#include "sista/SistaRuntime.hpp"
#include "asmjit/AsmjitT1.hpp"
#include "../DebugSettings.hpp"
#include "../ObjectMemory.hpp"
#include "../Interpreter.hpp"
#include <cstring>
#include <cstdio>
#include <pthread.h>

namespace pharo {
namespace sista { class Runtime; }
extern sista::Runtime* sistaRuntimeForGCHook_;
}

#if PHARO_JIT_ENABLED

namespace pharo {
namespace jit {

using namespace generated;

// Sista V1 bytecode opcodes live in src/vm/jit/SistaV1.hpp (shared with
// Tier2Compiler.cpp). `using namespace SistaV1` below brings the names into
// this translation unit without the `SistaV1::` prefix.
using namespace SistaV1;


JITCompiler::JITCompiler(CodeZone& zone, MethodMap& methodMap,
                         ObjectMemory& memory, Interpreter& interp)
    : zone_(zone), methodMap_(methodMap), memory_(memory), interp_(interp)
{
    std::memset(&helpers_, 0, sizeof(helpers_));
}

// ===== BYTECODE DECODING =====

uint16_t JITCompiler::selectStencil(uint8_t opcode, int operand) const {
    // Map Sista V1 bytecodes to stencils.
    // Range bytecodes are handled with if-chains (compiler optimizes to range checks).
    // Individual bytecodes use a switch.
    if (opcode <= 0x0F) return static_cast<uint16_t>(StencilID::stencil_pushRecvVar);
    if (opcode <= 0x1F) return static_cast<uint16_t>(StencilID::stencil_pushLitVar);
    if (opcode <= 0x3F) return static_cast<uint16_t>(StencilID::stencil_pushLitConst);
    if (opcode <= 0x4B) return static_cast<uint16_t>(StencilID::stencil_pushTemp);

    switch (opcode) {
    case SistaV1::PushReceiver:  return static_cast<uint16_t>(StencilID::stencil_pushReceiver);
    case SistaV1::PushTrue:      return static_cast<uint16_t>(StencilID::stencil_pushTrue);
    case SistaV1::PushFalse:     return static_cast<uint16_t>(StencilID::stencil_pushFalse);
    case SistaV1::PushNil:       return static_cast<uint16_t>(StencilID::stencil_pushNil);
    case SistaV1::PushZero:      return static_cast<uint16_t>(StencilID::stencil_pushZero);
    case SistaV1::PushOne:       return static_cast<uint16_t>(StencilID::stencil_pushOne);
    case SistaV1::Dup:           return static_cast<uint16_t>(StencilID::stencil_dup);
    case SistaV1::ReturnReceiver:return static_cast<uint16_t>(StencilID::stencil_returnReceiver);
    case SistaV1::ReturnTrue:    return static_cast<uint16_t>(StencilID::stencil_returnTrue);
    case SistaV1::ReturnFalse:   return static_cast<uint16_t>(StencilID::stencil_returnFalse);
    case SistaV1::ReturnNil:     return static_cast<uint16_t>(StencilID::stencil_returnNil);
    case SistaV1::ReturnTop:     return static_cast<uint16_t>(StencilID::stencil_returnTop);
    case SistaV1::Pop:           return static_cast<uint16_t>(StencilID::stencil_pop);
    default: break;
    }

    // Arithmetic (0x60-0x6F): fast-path SmallInteger ops
    if (opcode >= SistaV1::ArithBase && opcode <= 0x6F) {
        switch (opcode & 0x0F) {
        case 0:  return static_cast<uint16_t>(StencilID::stencil_addSmallInt);
        case 1:  return static_cast<uint16_t>(StencilID::stencil_subSmallInt);
        case 2:  return static_cast<uint16_t>(StencilID::stencil_lessThanSmallInt);
        case 3:  return static_cast<uint16_t>(StencilID::stencil_greaterThanSmallInt);
        case 4:  return static_cast<uint16_t>(StencilID::stencil_lessEqualSmallInt);
        case 5:  return static_cast<uint16_t>(StencilID::stencil_greaterEqualSmallInt);
        case 6:  return static_cast<uint16_t>(StencilID::stencil_equalSmallInt);
        case 7:  return static_cast<uint16_t>(StencilID::stencil_notEqualSmallInt);
        case 8:  return static_cast<uint16_t>(StencilID::stencil_mulSmallInt);
        case 10: return static_cast<uint16_t>(StencilID::stencil_modSmallInt);
        case 12: return static_cast<uint16_t>(StencilID::stencil_bitShiftSmallInt);
        case 13: return static_cast<uint16_t>(StencilID::stencil_divSmallInt);
        case 14: return static_cast<uint16_t>(StencilID::stencil_bitAndSmallInt);
        case 15: return static_cast<uint16_t>(StencilID::stencil_bitOrSmallInt);
        default: return static_cast<uint16_t>(StencilID::stencil_send);
        }
    }

    // Send special selectors 16-31 (0x70-0x7F)
    if (opcode >= 0x70 && opcode <= 0x7F) {
        int selectorIdx = opcode - 0x70;
        switch (selectorIdx) {
        case 6:  return static_cast<uint16_t>(StencilID::stencil_identicalTo);     // ==
        case 8:  return static_cast<uint16_t>(StencilID::stencil_notIdenticalTo);  // ~~
        default: return static_cast<uint16_t>(StencilID::stencil_send);
        }
    }

    // Sends (0x80-0xAF)
    if (opcode >= SistaV1::Send0Base && opcode <= 0xAF)
        return static_cast<uint16_t>(StencilID::stencil_send);

    // Jumps
    if (opcode >= SistaV1::ShortJumpBase && opcode <= 0xB7)
        return static_cast<uint16_t>(StencilID::stencil_jump);
    if (opcode >= SistaV1::ShortJumpTrueBase && opcode <= 0xBF)
        return static_cast<uint16_t>(StencilID::stencil_jumpTrue);
    if (opcode >= SistaV1::ShortJumpFalseBase && opcode <= 0xC7)
        return static_cast<uint16_t>(StencilID::stencil_jumpFalse);

    // Pop-and-store
    if (opcode >= SistaV1::PopStoreRecvBase && opcode <= 0xCF)
        return static_cast<uint16_t>(StencilID::stencil_popStoreRecvVar);
    if (opcode >= SistaV1::PopStoreTempBase && opcode <= 0xD7)
        return static_cast<uint16_t>(StencilID::stencil_popStoreTemp);

    // Extended bytecodes that use the same stencils as their short counterparts
    switch (opcode) {
    case SistaV1::ExtPushRecvVar:  return static_cast<uint16_t>(StencilID::stencil_pushRecvVar);
    case SistaV1::ExtPushLitVar:   return static_cast<uint16_t>(StencilID::stencil_pushLitVar);
    case SistaV1::ExtPushLitConst: return static_cast<uint16_t>(StencilID::stencil_pushLitConst);
    case SistaV1::ExtPushTemp:     return static_cast<uint16_t>(StencilID::stencil_pushTemp);
    case SistaV1::ExtSend:         return static_cast<uint16_t>(StencilID::stencil_send);
    case SistaV1::ExtJump:         return static_cast<uint16_t>(StencilID::stencil_jump);
    case SistaV1::ExtJumpTrue:     return static_cast<uint16_t>(StencilID::stencil_jumpTrue);
    case SistaV1::ExtJumpFalse:    return static_cast<uint16_t>(StencilID::stencil_jumpFalse);
    // Note: ExtPopStoreRecv/Temp/LitVar, ExtStore* are handled in decodeBytecodes
    // by remapping to short opcodes, so they never reach selectStencil.
    default: break;
    }

    // Fallback: unknown extended opcodes deopt to interpreter
    return static_cast<uint16_t>(StencilID::stencil_send);
}

bool JITCompiler::decodeBytecodes(const uint8_t* bytecodes, size_t length,
                                   std::vector<DecodedBC>& decoded,
                                   uint8_t& failedOpcode,
                                   bool isFullBlock) {
    failedOpcode = 0;
    decoded.clear();
    decoded.reserve(length);  // Upper bound: one DecodedBC per byte

    int extA = 0;  // Extension A accumulator (Sista V1 prefix 0xE0)
    int extB = 0;  // Extension B accumulator (Sista V1 prefix 0xE1)
    int firstExtBCOffset = -1;  // Position of first extension byte for current ext group

    int maxBranchTarget = -1;  // Track furthest forward jump to detect dead code
    size_t i = 0;
    while (i < length) {
        DecodedBC bc;
        bc.opcode = bytecodes[i];
        bc.operand = -1;
        bc.operand2 = -1;
        bc.operand2Ptr = 0;
        bc.branchTarget = -1;
        bc.bcOffset = static_cast<int>(i);
        bc.bcLength = 1;

        uint8_t op = bytecodes[i];

        // Decode operand from bytecode encoding.
        // Short (1-byte) bytecodes encode the operand in the opcode itself.
        // Extended (2-3 byte) bytecodes use separate bytes + extension prefixes.
        //
        // The ranges are tested in bytecode order (0x00 → 0xFF).
        if (SistaV1::isPushRecvVar(op)) {
            bc.operand = op & 0x0F;                            // pushRecvVar 0..15
        } else if (SistaV1::isPushLitVar(op)) {
            bc.operand = op & 0x0F;                            // pushLitVar 0..15
        } else if (SistaV1::isPushLitConst(op)) {
            bc.operand = op & 0x1F;                            // pushLitConst 0..31
        } else if (SistaV1::isPushTemp(op)) {
            bc.operand = op - SistaV1::PushTempBase;           // pushTemp 0..11
        } else if (op >= SistaV1::PushReceiver && op <= SistaV1::Dup) {
            // 0x4C-0x53: individual push bytecodes, no operand
            if (op == SistaV1::PushThisContext) {
                // pushThisContext (0x52) — deopt to interpreter.
                // When preceded by `ExtB 1` (0xE1 0x01), the interpreter must
                // see the ExtB to push thisProcess instead of thisContext, so
                // resume at the first extension byte, not the 0x52 itself.
                bc.operand = (firstExtBCOffset >= 0) ? firstExtBCOffset : bc.bcOffset;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
        } else if (op >= 0x54 && op <= 0x57) {
            // Unused in Sista V1 — interpreter treats as nop
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0; firstExtBCOffset = -1;
            continue;
        } else if (op >= SistaV1::ReturnReceiver && op <= SistaV1::ReturnTop) {
            // 0x58-0x5C: method return bytecodes (return receiver/true/false/nil/top).
            // In a FullBlock these are Non-Local Returns (return from the enclosing
            // method, not just the block).  NLR is complex — deopt to interpreter.
            if (isFullBlock) {
                bc.operand = bc.bcOffset;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            // In a method (not a block): simple return — fall through to selectStencil.
        } else if (SistaV1::isBlockReturn(op)) {
            if (isFullBlock && extA == 0) {
                // In a FullBlock with no enclosing levels: these are simple returns.
                if (op == SistaV1::BlockReturnNil) {
                    bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnNil);
                } else {
                    bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnTop);
                }
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            // Non-FullBlock or non-local return (extA > 0): complex semantics. Deopt.
            // 0x5E (BlockReturnTop) reads extA_ (enclosing levels) and extB_
            // (jump distance) in the interpreter, so resume at the first
            // extension byte when present, not the 0x5D/0x5E itself.
            bc.operand = (firstExtBCOffset >= 0) ? firstExtBCOffset : bc.bcOffset;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0; firstExtBCOffset = -1;
            continue;
        } else if (op == 0x5F) {
            // Nop (per Sista V1 spec)
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0; firstExtBCOffset = -1;
            continue;
        } else if (SistaV1::isArithSelector(op)) {
            bc.operand = bc.bcOffset;                          // bytecode offset (for ArithOverflow ip)
            bc.operand2 = 1;                                   // all arith selectors are 1-arg
        } else if (SistaV1::isSpecialSelector(op)) {
            bc.operand = op - SistaV1::SpecialSendBase;        // send special 16-31
            // nArgs per selector: at: at:put: size next nextPut: atEnd == class ~~ value value: do: new new: x y
            static const uint8_t specialNArgs[16] = {1,2,0,0,1,0,1,0,1,0,1,1,0,1,0,0};
            bc.operand2 = specialNArgs[bc.operand];
        } else if (SistaV1::isSend0(op)) {
            bc.operand = op & 0x0F;
            bc.operand2 = 0;
        } else if (SistaV1::isSend1(op)) {
            bc.operand = op & 0x0F;
            bc.operand2 = 1;
        } else if (SistaV1::isSend2(op)) {
            bc.operand = op & 0x0F;
            bc.operand2 = 2;
        } else if (SistaV1::isShortJump(op)) {
            bc.branchTarget = static_cast<int>(i) + 1 + (op & 0x07) + 1;
        } else if (SistaV1::isShortJumpTrue(op)) {
            bc.branchTarget = static_cast<int>(i) + 1 + (op & 0x07) + 1;
        } else if (SistaV1::isShortJumpFalse(op)) {
            bc.branchTarget = static_cast<int>(i) + 1 + (op & 0x07) + 1;
        } else if (SistaV1::isPopStoreRecv(op)) {
            bc.operand = op & 0x07;                            // popStoreRecvVar 0..7
        } else if (SistaV1::isPopStoreTemp(op)) {
            bc.operand = op - SistaV1::PopStoreTempBase;       // popStoreTemp 0..7
        } else if (op == SistaV1::Pop) {
            // No operand
        } else if (op == 0xD9) {
            // Unconditional trap — deopt to interpreter (stopVM)
            bc.operand = bc.bcOffset;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0; firstExtBCOffset = -1;
            continue;
        } else if (op >= 0xDA && op <= 0xDF) {
            // Reserved bytecodes — interpreter treats as nop
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0; firstExtBCOffset = -1;
            continue;
        } else {
            // Extended bytecodes (0xE0+) — switch on exact opcode
            // If a multi-byte bytecode is truncated (runs past end of bytecodes),
            // it's dead code after a return — stop decoding rather than bail out.
            switch (op) {

            case SistaV1::ExtendA: {
                if (i + 1 >= length) goto done;
                if (firstExtBCOffset < 0) firstExtBCOffset = static_cast<int>(i);
                extA = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
                decoded.push_back(bc);
                i += bc.bcLength;
                continue;  // Don't reset ext — carries to next bytecode
            }
            case SistaV1::ExtendB: {
                if (i + 1 >= length) goto done;
                if (firstExtBCOffset < 0) firstExtBCOffset = static_cast<int>(i);
                uint8_t extByte = bytecodes[i + 1];
                extB = (extByte >= 128)
                    ? (extB << 8) | extByte | static_cast<int>(0xFFFFFF00u)
                    : (extB << 8) | extByte;
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
                decoded.push_back(bc);
                i += bc.bcLength;
                continue;
            }
            case SistaV1::ExtPushRecvVar: {
                if (i + 1 >= length) goto done;
                bc.operand = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                break;
            }
            case SistaV1::ExtPushLitVar: {
                if (i + 1 >= length) goto done;
                bc.operand = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                break;
            }
            case SistaV1::ExtPushLitConst: {
                if (i + 1 >= length) goto done;
                bc.operand = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                break;
            }
            case SistaV1::ExtPushTemp: {
                if (i + 1 >= length) goto done;
                bc.operand = bytecodes[i + 1];
                bc.bcLength = 2;
                break;
            }
            case SistaV1::PushArray: {
                // Exit to interpreter for array allocation, then resume JIT.
                // OPERAND = desc byte (bits 0-6 = arraySize, bit 7 = popIntoArray)
                if (i + 1 >= length) goto done;
                bc.operand = bytecodes[i + 1];
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushArray);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::PushInteger: {
                if (i + 1 >= length) goto done;
                int value = (extB << 8) | bytecodes[i + 1];
                bc.operand = static_cast<int>((static_cast<int64_t>(value) << 3) | 1);
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushInteger);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::PushCharacter: {
                if (i + 1 >= length) goto done;
                int codePoint = bytecodes[i + 1] + (extB << 8);
                bc.operand = static_cast<int>((static_cast<int64_t>(codePoint) << 3) | 3);
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushInteger);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::ExtSend: {
                if (i + 1 >= length) goto done;
                uint8_t desc = bytecodes[i + 1];
                bc.operand = ((extA << 5) | (desc >> 3)) & 0xFFFF;
                bc.operand2 = ((extB << 3) | (desc & 0x07)) & 0xFF;
                bc.bcLength = 2;
                break;
            }
            case SistaV1::ExtSuperSend: {
                // Super send — same encoding as ExtSend but lookup starts at superclass.
                // IC caching does NOT work for super sends: the megacache conflates
                // normal and super sends (both use the same selectorBits), so a
                // normal send's cached method would be returned for a super send.
                // Super sends stay as stencil_send (deopt) for correct lookup.
                if (i + 1 >= length) goto done;
                uint8_t desc = bytecodes[i + 1];
                bc.operand = ((extA << 5) | (desc >> 3)) & 0xFFFF;
                bc.operand2 = ((extB << 3) | (desc & 0x07)) & 0xFF;
                bc.bcLength = 2;
                break;  // Fall through to selectStencil → stencil_send (NOT upgraded to sendPoly)
            }
            case SistaV1::InlinedPrimitive: {
                // Sista inlined primitive — interpreter treats as nop (skip operand)
                if (i + 1 >= length) goto done;
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::ExtJump: {
                if (i + 1 >= length) goto done;
                int offset = bytecodes[i + 1] + (extB << 8);
                bc.branchTarget = static_cast<int>(i) + 2 + offset;
                bc.bcLength = 2;
                bc.opcode = SistaV1::ShortJumpBase;
                break;
            }
            case SistaV1::ExtJumpTrue: {
                if (i + 1 >= length) goto done;
                int offset = bytecodes[i + 1] + (extB << 8);
                bc.branchTarget = static_cast<int>(i) + 2 + offset;
                bc.bcLength = 2;
                bc.opcode = SistaV1::ShortJumpTrueBase;
                break;
            }
            case SistaV1::ExtJumpFalse: {
                if (i + 1 >= length) goto done;
                int offset = bytecodes[i + 1] + (extB << 8);
                bc.branchTarget = static_cast<int>(i) + 2 + offset;
                bc.bcLength = 2;
                bc.opcode = SistaV1::ShortJumpFalseBase;
                break;
            }
            case SistaV1::ExtPopStoreRecv: {
                if (i + 1 >= length) goto done;
                bc.operand = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                bc.opcode = SistaV1::PopStoreRecvBase;
                break;
            }
            case SistaV1::ExtPopStoreLitVar: {
                if (i + 1 >= length) goto done;
                bc.operand = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreLitVar);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::ExtPopStoreTemp: {
                if (i + 1 >= length) goto done;
                bc.operand = bytecodes[i + 1];
                bc.bcLength = 2;
                bc.opcode = SistaV1::PopStoreTempBase;
                break;
            }
            case SistaV1::ExtStoreRecv: {
                if (i + 1 >= length) goto done;
                bc.operand = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeRecvVar);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::ExtStoreLitVar: {
                if (i + 1 >= length) goto done;
                bc.operand = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeLitVar);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::ExtStoreTemp: {
                if (i + 1 >= length) goto done;
                bc.operand = bytecodes[i + 1];
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeTemp);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::CallPrimitive: {
                // 3 bytes — skip, already handled by activateMethod
                if (i + 2 >= length) goto done;
                bc.bcLength = 3;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::PushFullBlock: {
                // Block creation — exit to interpreter, create closure, resume JIT
                if (i + 2 >= length) goto done;
                int litIndex = (extA << 8) | bytecodes[i + 1];
                int flags = bytecodes[i + 2];
                // Pack: bcOffset in high 16, litIndex in low 16
                bc.operand = (bc.bcOffset << 16) | (litIndex & 0xFFFF);
                bc.operand2 = flags;
                bc.bcLength = 3;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushBlock);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::PushClosure: {
                // Old-style closure — deopt to interpreter (3-byte).
                // Interpreter reads both extA_ (numCopied/numArgs upper bits)
                // and extB_ (blockSize upper bits), so resume at the first
                // extension byte when present.
                if (i + 2 >= length) goto done;
                bc.operand = (firstExtBCOffset >= 0) ? firstExtBCOffset : bc.bcOffset;
                bc.bcLength = 3;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::PushRemoteTemp: {
                // Push Temp At k In Temp Vector At j (3-byte)
                if (i + 2 >= length) goto done;
                int tempIndex = bytecodes[i + 1];
                int vectorIndex = bytecodes[i + 2];
                bc.operand = (vectorIndex << 8) | tempIndex;
                bc.bcLength = 3;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushRemoteTemp);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::StoreRemoteTemp: {
                // Store Temp At k In Temp Vector At j (3-byte, no pop)
                if (i + 2 >= length) goto done;
                int tempIndex = bytecodes[i + 1];
                int vectorIndex = bytecodes[i + 2];
                bc.operand = (vectorIndex << 8) | tempIndex;
                bc.bcLength = 3;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeRemoteTemp);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case SistaV1::PopStoreRemoteTemp: {
                // Pop and Store Temp At k In Temp Vector At j (3-byte)
                if (i + 2 >= length) goto done;
                int tempIndex = bytecodes[i + 1];
                int vectorIndex = bytecodes[i + 2];
                bc.operand = (vectorIndex << 8) | tempIndex;
                bc.bcLength = 3;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreRemoteTemp);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            case 0xFE:
            case 0xFF: {
                // UNASSIGNED 3-byte bytecodes — skip as nop
                if (i + 2 >= length) goto done;
                bc.bcLength = 3;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            default: {
                // Unknown extended bytecode — deopt to interpreter (2-byte)
                if (i + 1 >= length) goto done;
                bc.operand = bc.bcOffset;
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0; firstExtBCOffset = -1;
                continue;
            }
            } // end switch
        }

        bc.stencilIdx = selectStencil(bc.opcode, bc.operand);
        // For send and arithmetic stencils, operand = bytecode offset for
        // precise deopt. On overflow or unhandled send, the stencil sets
        // state.ip = state.ip + bcOffset so the interpreter resumes there.
        {
            auto sid = static_cast<StencilID>(bc.stencilIdx);
            if (sid == StencilID::stencil_send) {
                // Check if this is a real send (has argCount in operand2)
                // vs a bail-out (operand2 == -1, was forced to stencil_send)
                if (bc.operand2 >= 0 &&
                    ((bc.opcode >= 0x60 && bc.opcode <= 0x6F) ||
                     (bc.opcode >= 0x70 && bc.opcode <= 0x7F) ||
                     (bc.opcode >= 0x80 && bc.opcode <= 0xAF) ||
                     bc.opcode == SistaV1::ExtSend)) {
                    // Real send: upgrade to polymorphic IC stencil
                    // Note: ExtSuperSend (0xEB) is excluded — the megacache
                    // conflates normal and super sends, so super sends must
                    // deopt to the interpreter for correct lookup.
                    int argCount = bc.operand2;
                    bc.branchTarget = bc.operand;  // Save literal/selector index for mega cache
                    // Tried swapping to stencil_sendPoly (696 bytes,
                    // vs sendJ2J's 2092) behind PHARO_JIT_POLY=1 for
                    // todo.md §2.5: caused a tryResume/chain-loop
                    // crash (sendPoly's operand layout / save-stack
                    // expectations differ from sendJ2J's despite
                    // sharing the first two packed fields).  Not a
                    // drop-in replacement.  Reverted.
                    bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendJ2J);
                    bc.operand = (bc.bcLength << 24) | (argCount << 16) | (bc.bcOffset & 0xFFFF);
                    // operand2Ptr will be set after code zone allocation
                } else {
                    // Bail-out or special send: keep stencil_send with bcOffset.
                    // Use the first extension byte offset if present, so the
                    // interpreter processes extension prefixes before the send.
                    bc.operand = (firstExtBCOffset >= 0) ? firstExtBCOffset : bc.bcOffset;
                }
            } else if (sid == StencilID::stencil_addSmallInt ||
                sid == StencilID::stencil_subSmallInt ||
                sid == StencilID::stencil_mulSmallInt ||
                sid == StencilID::stencil_lessThanSmallInt ||
                sid == StencilID::stencil_greaterThanSmallInt ||
                sid == StencilID::stencil_lessEqualSmallInt ||
                sid == StencilID::stencil_greaterEqualSmallInt ||
                sid == StencilID::stencil_equalSmallInt ||
                sid == StencilID::stencil_notEqualSmallInt ||
                sid == StencilID::stencil_divSmallInt ||
                sid == StencilID::stencil_modSmallInt ||
                sid == StencilID::stencil_bitAndSmallInt ||
                sid == StencilID::stencil_bitOrSmallInt ||
                sid == StencilID::stencil_bitShiftSmallInt) {
                bc.operand = bc.bcOffset;
            }
        }
        // Track branch targets and detect dead code after returns
        if (bc.branchTarget > maxBranchTarget)
            maxBranchTarget = bc.branchTarget;
        decoded.push_back(bc);
        i += bc.bcLength;
        extA = 0;
        extB = 0;
        firstExtBCOffset = -1;

        // Stop decoding after an unconditional return if no branch targets
        // point past this position. Everything beyond is dead code.
        {
            auto sid = static_cast<StencilID>(bc.stencilIdx);
            if ((sid == StencilID::stencil_returnTop ||
                 sid == StencilID::stencil_returnReceiver ||
                 sid == StencilID::stencil_returnTrue ||
                 sid == StencilID::stencil_returnFalse ||
                 sid == StencilID::stencil_returnNil) &&
                static_cast<int>(i) > maxBranchTarget) {
                break;  // Dead code follows — stop decoding
            }
        }
    }

done:
    return true;
}

// ===== RELOCATION PATCHING =====

bool JITCompiler::patchStencilInstance(
    uint8_t* codeBase, uint32_t stencilOffset,
    const StencilDef& stencil,
    const DecodedBC& bc,
    uint8_t* methodCode, uint32_t totalCodeSize,
    const std::vector<uint32_t>& bcToCodeOffset,
    const std::vector<uint32_t>& bcToBranchOffset,
    uint64_t* literalPool, uint32_t literalPoolOffset,
    uint32_t& nextLiteralSlot)
{
    uint8_t* stencilCode = codeBase + stencilOffset;

    // Architecture-specific patch dispatcher
    auto patchOne = [&](const Relocation& r, uint64_t value) -> bool {
        if constexpr (HostArch == Arch::ARM64) {
            return patchARM64(stencilCode, r, value);
        } else {
            return patchX86_64(stencilCode, r, value);
        }
    };

    // Allocate a literal pool slot and store a value. Returns the address
    // of the pool entry (which is what gets patched into the instruction).
    auto allocPoolSlot = [&](uint64_t value) -> uint64_t {
        uint32_t slot = nextLiteralSlot++;
        literalPool[slot] = value;
        return reinterpret_cast<uint64_t>(
            reinterpret_cast<uint8_t*>(literalPool) + slot * 8);
    };

    // ARM64 GOT pairs: PAGEOFF12 allocates a slot, PAGE21 reuses it.
    // x86_64: every GOT reloc independently allocates a slot.
    uint64_t lastGotSlotAddr = 0;

    auto allocOrReuseSlot = [&](const Relocation& reloc, uint64_t value) -> uint64_t {
        if constexpr (HostArch == Arch::ARM64) {
            if (reloc.type == RelocType::ARM64_GOT_LOAD_PAGEOFF12) {
                lastGotSlotAddr = allocPoolSlot(value);
                return lastGotSlotAddr;
            } else {
                // PAGE21: reuse the slot from the preceding PAGEOFF12
                return lastGotSlotAddr;
            }
        } else {
            // x86_64: each GOT reloc gets its own slot
            return allocPoolSlot(value);
        }
    };

    for (uint16_t r = 0; r < stencil.numRelocs; r++) {
        const Relocation& reloc = stencil.relocs[r];

        switch (reloc.hole) {

        case HoleKind::Continue: {
            // Patch branch to the next stencil (stencilOffset + stencil.codeSize)
            uint32_t nextOffset = stencilOffset + stencil.codeSize;
            uint64_t target = reinterpret_cast<uint64_t>(codeBase + nextOffset);
            if (!patchOne(reloc, target)) return false;
            break;
        }

        case HoleKind::BranchTarget: {
            // Patch branch to the target bytecode's stencil
            if (bc.branchTarget < 0) {
                fprintf(stderr, "[JIT] Invalid branch target %d\n", bc.branchTarget);
                return false;
            }
            // bcToBranchOffset.size() == bcLen + 1 (sentinel at bcLen
            // holds codeSize == end of machine code).  A branch target
            // of bcLen would jump to codeSize, which is the FIRST byte
            // of the literal pool — UDF on the zero word.  Bug 11b
            // layer 4b: previously clamped to bcLen, which produced
            // exactly that crash.  Reject any branch >= bcLen so the
            // method falls back to interpreter rather than compile to
            // unreachable code.
            int target = bc.branchTarget;
            if (target >= (int)bcToBranchOffset.size() - 1) {
                static int rejectCount = 0;
                if (++rejectCount <= 5) {
                    fprintf(stderr, "[JIT] Branch target %d past last bytecode "
                            "(bcLen=%zu) — refusing to compile this method\n",
                            target, bcToBranchOffset.size() - 1);
                }
                return false;
            }
            // Both tables are last-write-wins and point to the real stencil
            // at each bcOffset. Jumps bypass any SimStack fallthrough-flush
            // inserted before the real stencil (see bcToBranchOffset init).
            uint32_t targetOff = bcToBranchOffset[target];
            // Defense in depth: targetOff must point INTO machine code,
            // not into literal pool / IC area.  bcToBranchOffset[bcLen]
            // = codeSize (machine-code end) is the sentinel.
            const uint32_t mcEnd = bcToBranchOffset.back();
            if (targetOff >= mcEnd) {
                static int rejectCount2 = 0;
                if (++rejectCount2 <= 5) {
                    fprintf(stderr, "[JIT] Branch offset %u >= machine-code end %u "
                            "(target bc %d) — refusing to compile\n",
                            targetOff, mcEnd, target);
                }
                return false;
            }
            uint64_t targetAddr = reinterpret_cast<uint64_t>(codeBase + targetOff);
            if (!patchOne(reloc, targetAddr)) return false;
            break;
        }

        case HoleKind::Operand: {
            // Load operand from literal pool via GOT-style relocation.
            // ARM64: adrp+ldr pair (PAGEOFF12 allocates, PAGE21 reuses).
            // x86_64: single RIP-relative instruction (always allocates).
            uint64_t operandVal = static_cast<uint64_t>(bc.operand >= 0 ? bc.operand : 0);
            uint64_t poolEntryAddr = allocOrReuseSlot(reloc, operandVal);
            if (!patchOne(reloc, poolEntryAddr)) return false;
            break;
        }

        case HoleKind::Operand2: {
            uint64_t op2Val = bc.operand2Ptr != 0
                ? bc.operand2Ptr
                : static_cast<uint64_t>(bc.operand2 >= 0 ? bc.operand2 : 0);
            uint64_t poolEntryAddr = allocOrReuseSlot(reloc, op2Val);
            if (!patchOne(reloc, poolEntryAddr)) return false;
            break;
        }

        case HoleKind::ResumeAddr: {
            // Same target as Continue (next stencil address), but used as a
            // DATA value (stored via ADRP+ADD or GOT load) rather than a
            // branch target.  Stencils store this in J2JSave.resumeAddr
            // so the J2J return path can tail-call to the next stencil.
            uint32_t nextOffset = stencilOffset + stencil.codeSize;
            uint64_t target = reinterpret_cast<uint64_t>(codeBase + nextOffset);
            uint64_t poolEntryAddr = allocOrReuseSlot(reloc, target);
            if (!patchOne(reloc, poolEntryAddr)) return false;
            break;
        }

        case HoleKind::RuntimeHelper: {
            // The addend field encodes which helper
            int helperId = reloc.addend;
            void* helperAddr = nullptr;

            switch (helperId) {
            case 1: helperAddr = helpers_.sendSlow; break;
            case 2: helperAddr = helpers_.returnToInterp; break;
            case 3: helperAddr = helpers_.arithOverflow; break;
            case 4: helperAddr = helpers_.nilOopAddr; break;
            case 5: helperAddr = helpers_.trueOopAddr; break;
            case 6: helperAddr = helpers_.falseOopAddr; break;
            case 7: helperAddr = helpers_.megaCacheAddr; break;
            case 8: helperAddr = helpers_.pushFrame; break;
            case 9: helperAddr = helpers_.popFrame; break;
            case 10: helperAddr = helpers_.j2jCall; break;
            case 11: helperAddr = helpers_.arrayPrim; break;
            case 12: helperAddr = helpers_.newPrim; break;
            case 13: helperAddr = helpers_.icMiss; break;
            case 14: helperAddr = helpers_.j2jTrace; break;
            case 15: helperAddr = helpers_.primAtPtr; break;
            case 16: helperAddr = helpers_.primAtPutPtr; break;
            case 17: helperAddr = helpers_.recompileQueue; break;
            case 18: helperAddr = helpers_.fillIC; break;
            default:
                fprintf(stderr, "[JIT] Unknown runtime helper ID %d\n", helperId);
                return false;
            }

            if (!helperAddr) {
                fprintf(stderr, "[JIT] Runtime helper %d not set\n", helperId);
                return false;
            }

            if constexpr (HostArch == Arch::ARM64) {
                if (reloc.type == RelocType::ARM64_BRANCH26) {
                    // Direct branch to helper function
                    uint64_t target = reinterpret_cast<uint64_t>(helperAddr);
                    if (!patchARM64(stencilCode, reloc, target)) return false;
                } else {
                    // GOT load: allocate/reuse literal pool slot.
                    // For function pointer helpers (1-3, 8-9): store address of the
                    // helpers_ struct field for double indirection (±4GB range).
                    // For data helpers (4-7): store data address directly.
                    uint64_t poolValue;
                    switch (helperId) {
                    case 1: poolValue = reinterpret_cast<uint64_t>(&helpers_.sendSlow); break;
                    case 2: poolValue = reinterpret_cast<uint64_t>(&helpers_.returnToInterp); break;
                    case 3: poolValue = reinterpret_cast<uint64_t>(&helpers_.arithOverflow); break;
                    case 8: poolValue = reinterpret_cast<uint64_t>(&helpers_.pushFrame); break;
                    case 9: poolValue = reinterpret_cast<uint64_t>(&helpers_.popFrame); break;
                    case 10: poolValue = reinterpret_cast<uint64_t>(&helpers_.j2jCall); break;
                    case 11: poolValue = reinterpret_cast<uint64_t>(&helpers_.arrayPrim); break;
                    case 12: poolValue = reinterpret_cast<uint64_t>(&helpers_.newPrim); break;
                    case 13: poolValue = reinterpret_cast<uint64_t>(&helpers_.icMiss); break;
                    case 14: poolValue = reinterpret_cast<uint64_t>(&helpers_.j2jTrace); break;
                    case 15: poolValue = reinterpret_cast<uint64_t>(&helpers_.primAtPtr); break;
                    case 16: poolValue = reinterpret_cast<uint64_t>(&helpers_.primAtPutPtr); break;
                    case 17: poolValue = reinterpret_cast<uint64_t>(&helpers_.recompileQueue); break;
                    case 18: poolValue = reinterpret_cast<uint64_t>(&helpers_.fillIC); break;
                    default: poolValue = reinterpret_cast<uint64_t>(helperAddr); break;
                    }
                    uint64_t poolAddr = allocOrReuseSlot(reloc, poolValue);
                    if (!patchARM64(stencilCode, reloc, poolAddr)) return false;
                }
            } else {
                // x86_64: all runtime helpers use GOT-style literal pool.
                // Same double-indirection scheme as ARM64 GOT loads.
                uint64_t poolValue;
                switch (helperId) {
                case 1: poolValue = reinterpret_cast<uint64_t>(&helpers_.sendSlow); break;
                case 2: poolValue = reinterpret_cast<uint64_t>(&helpers_.returnToInterp); break;
                case 3: poolValue = reinterpret_cast<uint64_t>(&helpers_.arithOverflow); break;
                case 8: poolValue = reinterpret_cast<uint64_t>(&helpers_.pushFrame); break;
                case 9: poolValue = reinterpret_cast<uint64_t>(&helpers_.popFrame); break;
                case 11: poolValue = reinterpret_cast<uint64_t>(&helpers_.arrayPrim); break;
                case 12: poolValue = reinterpret_cast<uint64_t>(&helpers_.newPrim); break;
                case 13: poolValue = reinterpret_cast<uint64_t>(&helpers_.icMiss); break;
                case 14: poolValue = reinterpret_cast<uint64_t>(&helpers_.j2jTrace); break;
                case 15: poolValue = reinterpret_cast<uint64_t>(&helpers_.primAtPtr); break;
                case 16: poolValue = reinterpret_cast<uint64_t>(&helpers_.primAtPutPtr); break;
                case 17: poolValue = reinterpret_cast<uint64_t>(&helpers_.recompileQueue); break;
                case 18: poolValue = reinterpret_cast<uint64_t>(&helpers_.fillIC); break;
                default: poolValue = reinterpret_cast<uint64_t>(helperAddr); break;
                }
                uint64_t poolAddr = allocPoolSlot(poolValue);
                if (!patchX86_64(stencilCode, reloc, poolAddr)) return false;
            }
            break;
        }

        default:
            fprintf(stderr, "[JIT] Unhandled hole kind %d\n", static_cast<int>(reloc.hole));
            return false;
        }
    }

    return true;
}

// Map primitive index to prologue stencil. Returns StencilID(-1) for unsupported.
static uint16_t primitivePrologueStencil(int primIndex) {
    switch (primIndex) {
    case 1:   return static_cast<uint16_t>(StencilID::stencil_primAdd);
    case 2:   return static_cast<uint16_t>(StencilID::stencil_primSub);
    case 3:   return static_cast<uint16_t>(StencilID::stencil_primLessThan);
    case 4:   return static_cast<uint16_t>(StencilID::stencil_primGreaterThan);
    case 5:   return static_cast<uint16_t>(StencilID::stencil_primLessEqual);
    case 6:   return static_cast<uint16_t>(StencilID::stencil_primGreaterEqual);
    case 7:   return static_cast<uint16_t>(StencilID::stencil_primEqual);
    case 8:   return static_cast<uint16_t>(StencilID::stencil_primNotEqual);
    case 9:   return static_cast<uint16_t>(StencilID::stencil_primMul);
    case 10:  return static_cast<uint16_t>(StencilID::stencil_primQuo);
    case 11:  return static_cast<uint16_t>(StencilID::stencil_primMod);
    case 12:  return static_cast<uint16_t>(StencilID::stencil_primDiv);
    case 14:  return static_cast<uint16_t>(StencilID::stencil_primBitAnd);
    case 15:  return static_cast<uint16_t>(StencilID::stencil_primBitOr);
    case 17:  return static_cast<uint16_t>(StencilID::stencil_primBitShift);
    case 60:  return static_cast<uint16_t>(StencilID::stencil_primAt);
    case 61:  return static_cast<uint16_t>(StencilID::stencil_primAtPut);
    case 62:  return static_cast<uint16_t>(StencilID::stencil_primSize);
    case 110: return static_cast<uint16_t>(StencilID::stencil_primIdentical);
    // case 111: stencil_primClass is a no-op stub (needs ObjectMemory for class table
    // lookup which isn't available in stencils). Without a real prologue, J2J is
    // correctly blocked for class methods by the unsafePrim guard.
    default:  return static_cast<uint16_t>(-1);
    }
}

// ===== SIMSTACK REGISTER CACHING =====
//
// Walk the decoded bytecode list and replace base stencil IDs with SimStack
// variants where the simulated stack state allows.
//
// State: E=Empty (x19/x20 unused), 1=One (TOS in x19), 2=Two (TOS in x19, NOS in x20)
//
// At branch targets and backward jumps, state is forced to Empty (flush first).
// Sends and returns also flush to Empty.

#ifdef __aarch64__

void JITCompiler::applySimStack(std::vector<DecodedBC>& decoded,
                                std::vector<int>& entryState) {
    entryState.clear();
    if (decoded.empty()) return;
    entryState.assign(decoded.size(), 0);

    // Identify which bytecode offsets are branch targets (need Empty state)
    std::vector<bool> isBranchTarget(decoded.size(), false);
    for (size_t i = 0; i < decoded.size(); i++) {
        int target = decoded[i].branchTarget;
        if (target >= 0) {
            for (size_t j = 0; j < decoded.size(); j++) {
                if (decoded[j].bcOffset == target) {
                    isBranchTarget[j] = true;
                    break;
                }
            }
        }
    }

    // Select flush stencil for current state (1-4)
    auto flushForState = [](int st) -> uint16_t {
        switch (st) {
        case 1: return static_cast<uint16_t>(StencilID::stencil_flush1);
        case 2: return static_cast<uint16_t>(StencilID::stencil_flush2);
        case 3: return static_cast<uint16_t>(StencilID::stencil_flush3);
        default: return static_cast<uint16_t>(StencilID::stencil_flush4);
        }
    };

    // Insert a flush before decoded[i], advance i past it, set state=0.
    // The flush's entry state = pre-flush state (it reads x19..xN from regs).
    auto insertFlush = [&](size_t& i, int& st) {
        DecodedBC flush;
        flush.opcode = 0;
        flush.stencilIdx = flushForState(st);
        flush.operand = -1;
        flush.operand2 = -1;
        flush.operand2Ptr = 0;
        flush.branchTarget = -1;
        flush.bcOffset = decoded[i].bcOffset;
        flush.bcLength = 0;
        decoded.insert(decoded.begin() + i, flush);
        isBranchTarget.insert(isBranchTarget.begin() + i, false);
        entryState.insert(entryState.begin() + i, st);
        i++;
        st = 0;
    };

    // SimStack state: 0=Empty, 1=One(x19), 2=Two(x19,x20), 3=Three(x19-x21), 4=Four(x19-x22)
    int state = 0;

    for (size_t i = 0; i < decoded.size(); i++) {
        // Branch targets must enter as Empty
        if (isBranchTarget[i] && state != 0)
            insertFlush(i, state);

        // Determine if this instruction is a "barrier" that requires Empty state
        auto sid = static_cast<StencilID>(decoded[i].stencilIdx);
        bool isBarrier = false;
        switch (sid) {
        case StencilID::stencil_send:
        case StencilID::stencil_sendPoly:
        case StencilID::stencil_sendJ2J:
        case StencilID::stencil_sendInlineGetter:
        case StencilID::stencil_sendInlineSetter:
        case StencilID::stencil_sendInlineReturnsSelf:
        case StencilID::stencil_sendInlineReturnsLiteral:
        case StencilID::stencil_sendBlockValue1Arg:
        case StencilID::stencil_sendBlockValue0Arg:
        case StencilID::stencil_sendBlockValue2Arg:
        case StencilID::stencil_sendInlineMonoJ2J:
        case StencilID::stencil_pushBlock:
        case StencilID::stencil_pushArray:
        case StencilID::stencil_pushRemoteTemp:
        case StencilID::stencil_storeRemoteTemp:
        case StencilID::stencil_popStoreRemoteTemp:
        case StencilID::stencil_popStoreLitVar:
        case StencilID::stencil_storeLitVar:
        case StencilID::stencil_ltJumpFalse:
        case StencilID::stencil_ltJumpTrue:
        case StencilID::stencil_gtJumpFalse:
        case StencilID::stencil_gtJumpTrue:
        case StencilID::stencil_leJumpFalse:
        case StencilID::stencil_leJumpTrue:
        case StencilID::stencil_geJumpFalse:
        case StencilID::stencil_geJumpTrue:
        case StencilID::stencil_eqJumpFalse:
        case StencilID::stencil_eqJumpTrue:
        case StencilID::stencil_neqJumpFalse:
        case StencilID::stencil_neqJumpTrue:
        case StencilID::stencil_identJumpFalse:
        case StencilID::stencil_identJumpTrue:
        case StencilID::stencil_notIdentJumpFalse:
        case StencilID::stencil_notIdentJumpTrue:
            isBarrier = true;
            break;
        default:
            if (sid == StencilID::stencil_returnTrue ||
                sid == StencilID::stencil_returnFalse ||
                sid == StencilID::stencil_returnNil) {
                isBarrier = true;
            }
            break;
        }

        if (isBarrier && state != 0)
            insertFlush(i, state);

        // Record entry state for decoded[i] AFTER all flush insertions — this is
        // the SimStack state the stencil at decoded[i] expects at entry.
        // state != 0 here means the stencil will read x19..xN from registers;
        // tryResume from the interpreter leaves those registers undefined, so
        // these bytecode offsets must NOT be valid resume targets.
        entryState[i] = state;

        // Capture reference AFTER all potential insertions above
        auto& bc = decoded[i];
        sid = static_cast<StencilID>(bc.stencilIdx);

        // Now select SimStack variant based on current state
        switch (sid) {
        // --- PUSH instructions: E→1, 1→2, 2→3, 3→4, 4→4(spill) ---
#define PUSH_SIMSTACK_CASE(baseName) \
        case StencilID::baseName: \
            if      (state == 0) { bc.stencilIdx = static_cast<uint16_t>(StencilID::baseName##_E); state = 1; } \
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::baseName##_1); state = 2; } \
            else if (state == 2) { bc.stencilIdx = static_cast<uint16_t>(StencilID::baseName##_2); state = 3; } \
            else if (state == 3) { bc.stencilIdx = static_cast<uint16_t>(StencilID::baseName##_3); state = 4; } \
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::baseName##_4); } \
            break;
        PUSH_SIMSTACK_CASE(stencil_pushTemp)
        PUSH_SIMSTACK_CASE(stencil_pushRecvVar)
        PUSH_SIMSTACK_CASE(stencil_pushLitConst)
        PUSH_SIMSTACK_CASE(stencil_pushLitVar)
        PUSH_SIMSTACK_CASE(stencil_pushReceiver)
        PUSH_SIMSTACK_CASE(stencil_pushTrue)
        PUSH_SIMSTACK_CASE(stencil_pushFalse)
        PUSH_SIMSTACK_CASE(stencil_pushNil)
#undef PUSH_SIMSTACK_CASE

        // --- POP: 4→3, 3→2, 2→1, 1→E, E→E(mem) ---
        case StencilID::stencil_pop:
            if      (state == 4) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pop_4); state = 3; }
            else if (state == 3) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pop_3); state = 2; }
            else if (state == 2) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pop_2); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pop_1); state = 0; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pop_E); }
            break;

        // --- DUP: E→1, 1→2, 2→3, 3→4, 4→4(spill) ---
        case StencilID::stencil_dup:
            if      (state == 0) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_dup_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_dup_1); state = 2; }
            else if (state == 2) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_dup_2); state = 3; }
            else if (state == 3) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_dup_3); state = 4; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_dup_4); }
            break;

        // --- STORE TEMP (no pop): TOS in x19 for all states >= 1 ---
        case StencilID::stencil_storeTemp:
            if (state >= 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeTemp_1); }
            break;

        // --- POP+STORE TEMP: 4→3, 3→2, 2→1, 1→E ---
        case StencilID::stencil_popStoreTemp:
            if      (state == 4) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreTemp_4); state = 3; }
            else if (state == 3) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreTemp_3); state = 2; }
            else if (state == 2) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreTemp_2); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreTemp_1); state = 0; }
            break;

        // --- STORE RECV VAR (no pop): TOS in x19 for all states >= 1 ---
        case StencilID::stencil_storeRecvVar:
            if (state >= 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeRecvVar_1); }
            break;

        // --- POP+STORE RECV VAR: 4→3, 3→2, 2→1, 1→E ---
        case StencilID::stencil_popStoreRecvVar:
            if      (state == 4) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreRecvVar_4); state = 3; }
            else if (state == 3) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreRecvVar_3); state = 2; }
            else if (state == 2) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreRecvVar_2); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreRecvVar_1); state = 0; }
            break;

        // --- BINARY ARITHMETIC/COMPARISON: _4(4→3), _3(3→2), _2(2→1), state 1→flush ---
#define BINARY_SIMSTACK_CASE(baseName) \
        case StencilID::baseName: \
            if      (state == 4) { bc.stencilIdx = static_cast<uint16_t>(StencilID::baseName##_4); state = 3; } \
            else if (state == 3) { bc.stencilIdx = static_cast<uint16_t>(StencilID::baseName##_3); state = 2; } \
            else if (state == 2) { bc.stencilIdx = static_cast<uint16_t>(StencilID::baseName##_2); state = 1; } \
            else if (state == 1) { insertFlush(i, state); } \
            break;
        BINARY_SIMSTACK_CASE(stencil_addSmallInt)
        BINARY_SIMSTACK_CASE(stencil_subSmallInt)
        BINARY_SIMSTACK_CASE(stencil_mulSmallInt)
        BINARY_SIMSTACK_CASE(stencil_lessThanSmallInt)
        BINARY_SIMSTACK_CASE(stencil_greaterThanSmallInt)
        BINARY_SIMSTACK_CASE(stencil_lessEqualSmallInt)
        BINARY_SIMSTACK_CASE(stencil_greaterEqualSmallInt)
        BINARY_SIMSTACK_CASE(stencil_equalSmallInt)
        BINARY_SIMSTACK_CASE(stencil_notEqualSmallInt)
#undef BINARY_SIMSTACK_CASE

        // --- CONDITIONAL JUMPS: state 1 → _1 variant, state > 1 → flush then base ---
        case StencilID::stencil_jumpTrue:
            if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_jumpTrue_1); state = 0; }
            else if (state > 1) { insertFlush(i, state); }
            break;
        case StencilID::stencil_jumpFalse:
            if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_jumpFalse_1); state = 0; }
            else if (state > 1) { insertFlush(i, state); }
            break;
        case StencilID::stencil_jumpTrueBack:
            if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_jumpTrueBack_1); state = 0; }
            else if (state > 1) { insertFlush(i, state); }
            break;
        case StencilID::stencil_jumpFalseBack:
            if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_jumpFalseBack_1); state = 0; }
            else if (state > 1) { insertFlush(i, state); }
            break;

        // --- RETURNS ---
        case StencilID::stencil_returnTop:
            // returnTop_1 reads x19=TOS; the frame is discarded so cached
            // NOS/3rd/4th in x20-x22 are dead — no need to flush them.
            if (state >= 1) {
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnTop_1);
            } else {
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnTop_E);
            }
            state = 0;
            break;
        case StencilID::stencil_returnReceiver:
            // returnReceiver doesn't read stack; cached values are dead.
            if (state >= 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnReceiver_1); }
            state = 0;
            break;

        // --- UNCONDITIONAL JUMP: flush before jumping ---
        case StencilID::stencil_jump:
        case StencilID::stencil_jumpBack:
            if (state != 0)
                insertFlush(i, state);
            break;

        // --- SUPERINSTRUCTIONS: handled as barriers above ---
        case StencilID::stencil_ltJumpFalse:
        case StencilID::stencil_ltJumpTrue:
        case StencilID::stencil_gtJumpFalse:
        case StencilID::stencil_gtJumpTrue:
        case StencilID::stencil_leJumpFalse:
        case StencilID::stencil_leJumpTrue:
        case StencilID::stencil_geJumpFalse:
        case StencilID::stencil_geJumpTrue:
        case StencilID::stencil_eqJumpFalse:
        case StencilID::stencil_eqJumpTrue:
        case StencilID::stencil_neqJumpFalse:
        case StencilID::stencil_neqJumpTrue:
        case StencilID::stencil_identJumpFalse:
        case StencilID::stencil_identJumpTrue:
        case StencilID::stencil_notIdentJumpFalse:
        case StencilID::stencil_notIdentJumpTrue:
            state = 0;
            break;

        // --- pushZero, pushOne, pushInteger: not optimized ---
        case StencilID::stencil_pushZero:
        case StencilID::stencil_pushOne:
        case StencilID::stencil_pushInteger:
            if (state != 0)
                insertFlush(i, state);
            break;

        // --- Everything else: flush to Empty, use base stencil ---
        default:
            if (state != 0)
                insertFlush(i, state);
            break;
        }
    }
}

#endif // __aarch64__

// ===== SHARED-IC SEND-SITE MAP (§1.3a) =====

const std::vector<uint16_t>*
JITCompiler::getSendSiteBCOffsets(uint64_t compiledMethodBits) const {
    auto it = sendSiteMap_.find(compiledMethodBits);
    return it != sendSiteMap_.end() ? &it->second : nullptr;
}

// ===== IC-GUIDED SPECIALIZATION =====

void JITCompiler::applyICSpecialization(std::vector<DecodedBC>& decoded,
                                          JITMethod* oldVersion,
                                          ObjectHeader* methObj,
                                          int numLiterals) {
    // 2026-05-03: IC data lives in heap-side icBuffer, not in MAP_JIT.
    // No offset arithmetic needed — read directly from oldVersion->
    // icBuffer.

    uint16_t sendIdx = 0;
    uint16_t specialized = 0;
    uint16_t specGetter = 0, specSetter = 0, specReturnsSelf = 0;
    uint16_t specReturnsLit = 0, specBlockValue1 = 0, specMonoJ2J = 0;
    uint16_t specMultiSlot = 0;

    for (auto& bc : decoded) {
        if (static_cast<StencilID>(bc.stencilIdx) != StencilID::stencil_sendJ2J)
            continue;

        if (sendIdx >= oldVersion->numICEntries)
            break;  // more sends than IC slots (shouldn't happen)

        uint8_t* icBase = oldVersion->icSiteAt(sendIdx);
        uint64_t* ic = reinterpret_cast<uint64_t*>(icBase);

        // Check monomorphic: slot 0 populated, slot 1 empty
        uint64_t classKey0 = ic[0];
        uint64_t extra0 = ic[2];
        uint64_t classKey1 = ic[3];

        if (classKey0 != 0 && classKey1 == 0) {
            // Monomorphic site — check for trivial method patterns
            // Pack literal index (for selector recovery on bail) into bits 48-63
            uint64_t litBits = (uint64_t)(bc.branchTarget & 0xFFFF) << 48;
            // Splice gate: if this site's callee is a Sista splice target,
            // skip MonoJ2J (and block-value) specialization.  Those stencils
            // bypass tryJITActivation, where Sista's lowered fn(&sstate) is
            // invoked.  Without this gate, recompiling any caller whose IC
            // points at a splice callee would rewire dispatch to direct T1
            // entry — causing the 150× regression seen in prior J2J
            // caller-bump experiments (see project_jit_recompile_gap.md).
            // Cheap check: ic[1] is the callee CompiledMethod oop.
            uint64_t calleeMethBits = reinterpret_cast<uint64_t*>(icBase)[1];
            bool calleeIsSplice = false;
            if (calleeMethBits != 0 && sistaRuntimeForGCHook_) {
                calleeIsSplice = sistaRuntimeForGCHook_->hasSplice(
                    Oop::fromRawBits(calleeMethBits));
            }
            if (extra0 & (1ULL << 63)) {
                // Getter: inline slot read
                uint16_t slotIdx = (uint16_t)(extra0 & 0xFFFF);
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendInlineGetter);
                bc.operand2Ptr = litBits | (classKey0 << 16) | slotIdx;
                specialized++; specGetter++;
            } else if (extra0 & (1ULL << 62)) {
                // Setter: inline slot write
                uint16_t slotIdx = (uint16_t)(extra0 & 0xFFFF);
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendInlineSetter);
                bc.operand2Ptr = litBits | (classKey0 << 16) | slotIdx;
                specialized++; specSetter++;
            } else if (extra0 & (1ULL << 61)) {
                // ReturnsSelf: just pop args
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendInlineReturnsSelf);
                bc.operand2Ptr = litBits | (classKey0 << 16);
                specialized++; specReturnsSelf++;
            } else if (extra0 & (1ULL << 58)) {
                // ReturnsLiteral (Phase 4-style port).  IC patcher
                // only sets bit 58 when PHARO_RETLIT=1 — the IC
                // entry is rare under default settings, so this
                // branch is dead-by-default.  When it does fire,
                // the specialized stencil reads `icData[2]` for the
                // literal at runtime (we can't pack 32 bits of class
                // + 48 bits of literal into a single 64-bit operand).
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendInlineReturnsLiteral);
                // operand2Ptr is set to the IC base in the loop at
                // line ~2129; it gets patched alongside sendJ2J.
                specialized++; specReturnsLit++;
            } else if (extra0 & (1ULL << 57)) {
                // Multi-slot getter (^ self[A] op self[B] op const).
                // Set in IC by patchJITICAfterSend when the callee's
                // bytecodes match the 6-byte multi-slot pattern.  At
                // recompile, swap in the specialized stencil that
                // does class-check + 2 slot reads + scalar SmI arith
                // inline (vs IC_HIT macro's chained bit-tests).
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendInlineMultiSlot);
                // operand2Ptr stays as IC base — the stencil reads
                // class from icData[0] and packed bits from icData[2].
                specialized++; specMultiSlot++;
            } else if (!g_debug.noBlock1Spec && !calleeIsSplice &&
                       (extra0 & (1ULL << 59)) /* BLOCK_VALUE_BIT */ &&
                       ((bc.operand >> 16) & 0xFF) == 1) {
                // value: send to a FullBlockClosure receiver — specialize
                // for nArgs=1.  Stencil hardcodes nArgs and folds the
                // captured-temp loop into a straight-line nil fill when
                // numCopied==0.  Default-on after correctness validation
                // (2026-04-26).  Set PHARO_NO_BLOCK1_SPEC=1 to disable.
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendBlockValue1Arg);
                bc.operand2Ptr = litBits | (classKey0 << 16);
                specialized++; specBlockValue1++;
            } else if (!g_debug.noBlock1Spec && !calleeIsSplice &&
                       (extra0 & (1ULL << 59)) /* BLOCK_VALUE_BIT */ &&
                       ((bc.operand >> 16) & 0xFF) == 0) {
                // value send (0-arg) — same shape as 1-arg specialization
                // but receiver at sp[-1] and no arg slot to skip.  Helps the
                // `N timesRepeat: [block]` pattern where each iter sends
                // 0-arg `value` to a captured FullBlockClosure.
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendBlockValue0Arg);
                bc.operand2Ptr = litBits | (classKey0 << 16);
                specialized++; specBlockValue1++;
            } else if (!g_debug.noBlock1Spec && !calleeIsSplice &&
                       (extra0 & (1ULL << 59)) /* BLOCK_VALUE_BIT */ &&
                       ((bc.operand >> 16) & 0xFF) == 2) {
                // value:value: send (2-arg) to a FullBlockClosure receiver.
                // Phase 6 first piece (2026-05-02): closes a chunk of the
                // sort/dict/select bench gap where comparator blocks are
                // invoked via `block value: a value: b` per iter.
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendBlockValue2Arg);
                bc.operand2Ptr = litBits | (classKey0 << 16);
                specialized++; specBlockValue1++;
            } else if (g_debug.monoJ2JSpec && !calleeIsSplice &&
                       (extra0 & (1ULL << 60)) /* J2J_ENTRY_BIT */ &&
                       (extra0 & (0x1FULL << 48)) == 0 /* no inline prim */ &&
                       (extra0 & (1ULL << 59)) == 0 /* not block value */) {
                // Monomorphic send to a JIT-compiled method.  Replace
                // the 6-way IC probe in sendJ2J with a single class
                // check + direct J2J save+tail-call.  Uses the existing
                // IC table for entry address — same operand2Ptr layout
                // as sendJ2J.  OPT-IN (PHARO_MONOJ2J_SPEC=1) — default-on
                // attempt 2026-04-26 caused SIGSEGV on long fib bench;
                // suspected stale icData[1] (method oop) after GC.
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendInlineMonoJ2J);
                // Keep operand2Ptr unchanged (already points at IC base)
                specialized++; specMonoJ2J++;
            }
        } else if (classKey0 == 0) {
            // Cold IC site at recompile.  Selector-based block-value
            // spec (default-on 2026-05-03 — sort 100K -30%): when
            // this Send2 sends `value:value:`, apply
            // sendBlockValue2Arg with FullBlockClosure's classIndex
            // baked in.  The spec stencil's slow path probes the
            // mega-cache like sendJ2J does and bails to
            // ExitSendCached on hit (avoids the 3000× slowdown that
            // would otherwise occur from full method lookup on
            // every call).  Catches the canonical sort 100K
            // bottleneck — see docs/jit-multiweek-work.md.
            // PHARO_NO_BLOCK_VALUE_SPEC=1 to opt out.
            static const bool blockValueSpec =
                std::getenv("PHARO_NO_BLOCK_VALUE_SPEC") == nullptr;
            if (blockValueSpec) {
                int argCount = (bc.operand >> 16) & 0xFF;
                if (argCount <= 2 && methObj
                        && (bc.opcode >= 0x80 && bc.opcode <= 0xAF
                            || bc.opcode == SistaV1::ExtSend)) {
                    int litIndex = bc.branchTarget;
                    if (litIndex >= 0 && litIndex < numLiterals) {
                        Oop selOop = methObj->slotAt(1 + litIndex);
                        uint64_t selBits = selOop.rawBits();
                        if (selBits != 0 && (selBits & 0x7) == 0
                                && selBits > 0x10000) {
                            ObjectHeader* selObj = selOop.asObjectPtr();
                            // Match selector by argCount: #value (0),
                            // #value: (1), #value:value: (2)
                            bool selMatch = false;
                            if (selObj->isBytesObject()) {
                                size_t bsz = selObj->byteSize();
                                const char* sb = (const char*)selObj->bytes();
                                if (argCount == 0 && bsz == 5
                                        && std::memcmp(sb, "value", 5) == 0) {
                                    selMatch = true;
                                } else if (argCount == 1 && bsz == 6
                                        && std::memcmp(sb, "value:", 6) == 0) {
                                    selMatch = true;
                                } else if (argCount == 2 && bsz == 12
                                        && std::memcmp(sb, "value:value:", 12)
                                            == 0) {
                                    selMatch = true;
                                }
                            }
                            if (selMatch) {
                                uint32_t fbcIdx =
                                    interp_.jitRuntime().resolveFullBlockClosureClassIndex();
                                if (fbcIdx != 0) {
                                    StencilID specId = (argCount == 0)
                                        ? StencilID::stencil_sendBlockValue0Arg
                                        : (argCount == 1)
                                            ? StencilID::stencil_sendBlockValue1Arg
                                            : StencilID::stencil_sendBlockValue2Arg;
                                    uint64_t litBitsPacked =
                                        (uint64_t)(litIndex & 0xFFFF) << 48;
                                    bc.stencilIdx =
                                        static_cast<uint16_t>(specId);
                                    bc.operand2Ptr =
                                        litBitsPacked
                                        | ((uint64_t)fbcIdx << 16);
                                    specialized++; specBlockValue1++;
                                }
                            }
                        }
                    }
                }
            }
        }

        sendIdx++;
    }

    if (specialized > 0) {
        fprintf(stderr,
                "[JIT] IC specialization: %u/%u sites "
                "(get=%u set=%u retSelf=%u retLit=%u block1=%u monoJ2J=%u multi=%u)\n",
                specialized, sendIdx,
                specGetter, specSetter, specReturnsSelf,
                specReturnsLit, specBlockValue1, specMonoJ2J, specMultiSlot);
    }
}

// ===== RECOMPILATION =====

JITMethod* JITCompiler::recompile(Oop compiledMethod) {
    JITMethod* old = methodMap_.lookup(compiledMethod.rawBits());
    if (!old || old->numICEntries == 0)
        return nullptr;

    static const bool trace =
        std::getenv("PHARO_TRACE_RECOMPILE_FLOW") != nullptr;
    std::string sel;
    int oldTier = old->tier;
    uint8_t* oldCode = old->codeStart();
    if (trace) {
        sel = interp_.memory().selectorOf(compiledMethod);
        fprintf(stderr,
                "[RECOMP-IN] sel=#%s methOop=0x%llx oldTier=%d oldCode=%p "
                "oldNumIC=%u oldExecCount=%u\n",
                sel.c_str(),
                (unsigned long long)compiledMethod.rawBits(),
                oldTier, (void*)oldCode, old->numICEntries,
                old->stats ? old->stats->executionCount : 0);
    }

    // Temporarily remove from map so compile() doesn't short-circuit
    methodMap_.remove(compiledMethod.rawBits());

    JITMethod* newMethod = compile(compiledMethod, old);

    if (newMethod) {
        // tier is in MAP_JIT — open a tight W window for this single
        // store.  W^X audit 2026-04-26.
        makeWritable(newMethod, newMethod->totalSize);
        newMethod->tier = 2;  // Mark as recompiled
        makeExecutable(newMethod, newMethod->totalSize);
        recompilations_++;
        if (trace) {
            fprintf(stderr,
                    "[RECOMP-OUT] sel=#%s newCode=%p newCanBail=%d "
                    "newNumIC=%u\n",
                    sel.c_str(), (void*)newMethod->codeStart(),
                    (int)newMethod->canBailMidMethod,
                    newMethod->numICEntries);
        }
    } else {
        // Recompilation failed — restore old version
        methodMap_.insert(compiledMethod.rawBits(), old);
        if (trace) {
            fprintf(stderr, "[RECOMP-FAIL] sel=#%s — restored old\n",
                    sel.c_str());
        }
    }

    return newMethod;
}

// ===== MAIN COMPILATION =====

JITMethod* JITCompiler::compile(Oop compiledMethod, JITMethod* oldVersion) {
    // JIT_MAX_COMPILE: hard limit on total compilations + recompilations.
    // The noteMethodEntry path also has this check but it doesn't catch
    // the recompile path (maybeRecompileForOSR → recompile → compile).
    // For bisection at low PHARO_JIT_DEFER (deferred.md A1 P0), enforce
    // the limit here too.
    {
        static const int maxCompile = pharo::g_debug.jitMaxCompile;
        if (maxCompile >= 0 && (int)methodsCompiled_ >= maxCompile) {
            compilationsFailed_++;
            return nullptr;
        }
    }

    // Check if already compiled (skip check during recompilation)
    if (!oldVersion && methodMap_.lookup(compiledMethod.rawBits())) {
        return methodMap_.lookup(compiledMethod.rawBits());
    }

    // Validate the method oop is in heap (deferred.md A1 P0).
    // At low PHARO_JIT_DEFER, the queue can hand us methods whose
    // oops have been freed/moved by the time the safe-point drain
    // runs.  Without this guard, asObjectPtr() crashes on a stale
    // pointer.
    if (!compiledMethod.isObject() || compiledMethod.rawBits() < 0x10000
        || !interp_.memory().isValidPointer(compiledMethod)) {
        compilationsFailed_++;
        return nullptr;
    }
    // Reject nil/true/false specials (deferred.md A1 P0,
    // lldb-confirmed 2026-05-06): if the queue has a stale oop that
    // happens to be nil = 0x300000000, the early checks above pass
    // (nil IS a valid object pointer in heap range), then the IC
    // setup loop later writes through icBase = NULL+offset → crash
    // at JITCompiler.cpp:2402.  Catch it here.  Also validate that
    // the method's header is a SmallInteger — every CompiledMethod
    // has a SmI header, so this filters non-method oops cheaply.
    {
        Oop nilObj = interp_.memory().nil();
        if (compiledMethod.rawBits() == nilObj.rawBits()) {
            compilationsFailed_++;
            return nullptr;
        }
        ObjectHeader* h = compiledMethod.asObjectPtr();
        if (h->slotCount() < 1 || !h->slotAt(0).isSmallInteger()) {
            compilationsFailed_++;
            return nullptr;
        }
    }

    // PHARO_USE_ASMJIT_T1=1: route through the asmjit-emitted Tier-1
    // path instead of the stencil pipeline.  Phase 1 of the JIT
    // rebuild (see scripts/jit-diff/plan_asmjit_replacement.md).
    // Every method compiles to a tiny "set ExitSend; ret" trampoline,
    // so the runtime sees a JIT-compiled method but execution
    // immediately bails to the interpreter on entry.  Goal: prove
    // the integration plumbing works end-to-end before adding real
    // bytecode emit functions in Phase 2+.
    {
        static const bool useAsmjitT1 =
            std::getenv("PHARO_USE_ASMJIT_T1") != nullptr;
        if (useAsmjitT1) {
            JITMethod* jm = compileViaAsmjit(zone_, methodMap_, memory_,
                                              interp_, compiledMethod);
            if (jm) {
                methodsCompiled_++;
            } else {
                compilationsFailed_++;
            }
            return jm;
        }
    }

    // Stencil compile path follows.
    // Get bytecode range from CompiledMethod
    ObjectHeader* methObj = compiledMethod.asObjectPtr();
    Oop headerOop = methObj->slotAt(0);
    if (!headerOop.isSmallInteger()) {
        compilationsFailed_++;
        fprintf(stderr, "[JIT] FAIL: bad header for 0x%llx\n", (unsigned long long)compiledMethod.rawBits());
        return nullptr;
    }

    int64_t headerBits = headerOop.asSmallInteger();
    int numLiterals = static_cast<int>(headerBits & 0x7FFF);

    uint8_t* bytes = methObj->bytes();
    size_t bcStart = (1 + numLiterals) * 8;
    size_t totalBytes = methObj->slotCount() * 8;
    // Adjust for format-encoded unused bytes
    uint8_t fmt = static_cast<uint8_t>(methObj->format());
    int unusedBytes = (fmt >= 24) ? (fmt - 24) : 0;
    size_t bcLen = totalBytes - bcStart - unusedBytes;

    if (bcLen == 0 || bcLen > MaxCompilableBytecodes) {
        compilationsFailed_++;
        fprintf(stderr, "[JIT] FAIL: bcLen=%zu (max=%zu)\n", bcLen, (size_t)MaxCompilableBytecodes);
        return nullptr;
    }

    // Skip methods that send #ffiCall: — JIT codegen for FFI-using
    // methods produces code that crashes at offset 2296 when called
    // via chain loop (consistent across SDL_Renderer, SDL_Surface, etc).
    // Cheap detection: scan literals for the #ffiCall: symbol.  Real
    // fix is JIT-aware FFI codegen.  See project_fib_hang_chainloop.md.
    {
        for (int li = 1; li <= numLiterals; li++) {
            Oop lit = methObj->slotAt(li);
            if (!lit.isObject() || lit.rawBits() < 0x10000) continue;
            ObjectHeader* litHdr = lit.asObjectPtr();
            if (!litHdr->isBytesObject()) continue;
            if (litHdr->byteSize() != 8) continue;  // "ffiCall:" is 8 bytes
            const char* litBytes = (const char*)litHdr->bytes();
            if (std::memcmp(litBytes, "ffiCall:", 8) == 0) {
                compilationsFailed_++;
                fprintf(stderr, "[JIT] FFI-SKIP method 0x%llx (literal #ffiCall:)\n",
                        (unsigned long long)compiledMethod.rawBits());
                return nullptr;
            }
        }
    }

    const uint8_t* bytecodes = bytes + bcStart;

    // Method-level opt-in heuristic (task #2.6): skip JIT compilation
    // for methods with fewer than PHARO_JIT_MIN_SENDS sends.
    //
    // Default: 0 (no filtering).  Benchmark measurement (2026-04-18):
    // setting this to 3 on the array-fill workload produced a 13-25×
    // slowdown (4902ms vs ~205-370ms).  The docs/jit-todo.md
    // hypothesis that "interpreter beats JIT on arith loops" doesn't
    // hold for THIS workload — the hot path relies on
    // JIT-compiled `to:do:` / `timesRepeat:` bodies running in native
    // code.  Leaving the knob in for future bisection but it should
    // never be enabled by default.
    {
        const int minSends = (g_debug.jitMinSends >= 0) ? g_debug.jitMinSends : 0;
        if (minSends > 0) {
            int sendCount = 0;
            for (size_t i = 0; i < bcLen; i++) {
                uint8_t op = bytecodes[i];
                // All send-like bytecodes (arith + special + Send{0,1,2} + ExtSend + ExtSuperSend).
                if ((op >= 0x60 && op <= 0xAF) || op == 0xEA || op == 0xEB) {
                    sendCount++;
                    if (sendCount >= minSends) break;
                }
            }
            if (sendCount < minSends) {
                compilationsFailed_++;
                return nullptr;
            }
        }
    }

    // Check if this is a CompiledBlock (FullBlock) — block returns are simple returns
    uint32_t methodClassIndex = methObj->classIndex();
    bool isFullBlock = (methodClassIndex == interp_.compiledBlockClassIndex());

    // Bisection: PHARO_JIT_NO_BLOCKS=1 skips JIT compilation for CompiledBlocks.
    // Used to check whether the JIT bug is in block compilation specifically.
    if (isFullBlock) {
        static bool noBlocks = g_debug.noBlocks;
        if (noBlocks) {
            compilationsFailed_++;
            return nullptr;
        }
    }

    // Decode bytecodes
    std::vector<DecodedBC> decoded;
    uint8_t failedOpcode = 0;
    if (!decodeBytecodes(bytecodes, bcLen, decoded, failedOpcode, isFullBlock)) {
        compilationsFailed_++;
        if (failedOpcode) bailoutCounts_[failedOpcode]++;
        fprintf(stderr, "[JIT] Bail-out #%zu: opcode 0x%02X bcLen=%zu\n",
                compilationsFailed_, failedOpcode, bcLen);
        return nullptr;
    }

    if (decoded.empty()) {
        compilationsFailed_++;
        return nullptr;
    }

    // (Removed 2026-05-09: NLR-fallthru pop+returnSelf rewrite.  This was
    // a workaround for the depth-≥36 sender-chain corruption that turned
    // out to be A4 — savedActiveContext propagating nil through J2J-push
    // sites.  With A4 fixed at root cause (commit 16bc4ff7), the rewrite
    // is unnecessary and was changing semantics of all methods ending
    // in `<expr>. ^ self` (returning expr's value instead of self).
    // Validated 2026-05-09: full eval suite + 14-class SUnit pass with
    // BOTH JIT and INTERP NLR-fixes disabled.)

    // Bytecode dump for bisection (JIT_DUMP_BC env var)
    {
        static bool dumpBC = g_debug.jitDumpBC;
        static const char* dumpBCPre = g_debug.jitDumpBCPre;
        std::string sel;
        bool doDump = dumpBC;
        if (!doDump && dumpBCPre && *dumpBCPre) {
            sel = interp_.memory().selectorOf(compiledMethod);
            doDump = (sel == dumpBCPre);
        }
        if (doDump) {
            if (sel.empty()) sel = interp_.memory().selectorOf(compiledMethod);
            fprintf(stderr, "[JIT-BC] #%s bcLen=%zu bytes:", sel.c_str(), bcLen);
            for (size_t b = 0; b < bcLen; b++)
                fprintf(stderr, " %02X", bytecodes[b]);
            fprintf(stderr, "\n");
            for (size_t d = 0; d < decoded.size(); d++) {
                fprintf(stderr, "[JIT-BC]   [%zu] op=0x%02X stencil=%u operand=%d bc=%d br=%d\n",
                        d, decoded[d].opcode, decoded[d].stencilIdx,
                        decoded[d].operand, decoded[d].bcOffset, decoded[d].branchTarget);
            }
        }
    }

    // Selector-based JIT skip for bisection: PHARO_JIT_SKIP_SELECTORS=sel1,sel2,...
    // Skips JIT compilation for methods with these selectors. Useful for narrowing
    // down which compiled method causes a regression.
    {
        static const char* skipEnv = g_debug.jitSkipSelectors;
        if (skipEnv && *skipEnv) {
            std::string sel = interp_.memory().selectorOf(compiledMethod);
            // Match selector against comma-separated list (exact match per token).
            const char* p = skipEnv;
            while (*p) {
                const char* end = p;
                while (*end && *end != ',') end++;
                if ((size_t)(end - p) == sel.size() &&
                    std::memcmp(p, sel.data(), sel.size()) == 0) {
                    static int skipCount = 0;
                    if (++skipCount <= 10) {
                        fprintf(stderr, "[JIT] Skipping #%s (PHARO_JIT_SKIP_SELECTORS)\n",
                                sel.c_str());
                    }
                    compilationsFailed_++;
                    return nullptr;
                }
                p = (*end == ',') ? end + 1 : end;
            }
        }
    }

    // Prepend primitive prologue stencil if method has a supported primitive.
    // The prologue runs the fast path (type check + inline op); on failure
    // it falls through via _HOLE_CONTINUE to the normal bytecodes.
    bool hasPrimPrologue = false;
    if ((headerBits >> 16) & 1) {  // hasPrimitive flag
        // Extract primitive index from the CallPrimitive bytecode (248 lowByte highByte)
        if (bcLen >= 3 && bytecodes[0] == 0xF8) {
            int primIndex = bytecodes[1] | ((bytecodes[2] & 0x1F) << 8);
            uint16_t prologueStencil = primitivePrologueStencil(primIndex);
            if (prologueStencil != static_cast<uint16_t>(-1)) {
                // Insert prologue as the first stencil (bcOffset -1 = synthetic)
                DecodedBC prologue = {};
                prologue.opcode = 0;       // synthetic
                prologue.stencilIdx = prologueStencil;
                prologue.operand = -1;
                prologue.operand2 = -1;
                prologue.branchTarget = -1;
                prologue.bcOffset = 0;     // same offset as first BC
                prologue.bcLength = 0;     // doesn't consume any bytecodes
                decoded.insert(decoded.begin(), prologue);
                hasPrimPrologue = true;
            } else {
                // Method has a primitive we can't inline. The CallPrimitive
                // bytecode becomes NOP in JIT code. This is safe because:
                // 1. The interpreter always tries the primitive before entering JIT
                //    (tryExecute only fires AFTER primitive failure)
                // 2. J2J is blocked for these methods by the unsafePrim guard
                //    in IC patching (hasPrimPrologue == false)
                // So we compile the fallback bytecodes without a prologue.
            }
        }
    }

    // Peephole: fuse comparison + conditional jump into superinstructions.
    // This eliminates the boolean Oop creation/stack-roundtrip between them.
    // DISABLED: causes +2 sp leak per loop iteration in methods like scanFor:.
    // The fused stencil binaries are correct (verified via disassembly), so the
    // bug is likely in how fused stencils interact with the JIT resume path or
    // in one of the arithmetic fused stencils (88-byte lt/gt/eq/ne variants).
    for (size_t pi = 0; false && pi + 1 < decoded.size(); pi++) {
        auto& cmp = decoded[pi];
        auto& jmp = decoded[pi + 1];
        auto cmpSid = static_cast<StencilID>(cmp.stencilIdx);
        auto jmpSid = static_cast<StencilID>(jmp.stencilIdx);

        // Fuse comparison (arithmetic or identity) followed by jumpTrue/jumpFalse
        if (jmpSid != StencilID::stencil_jumpFalse &&
            jmpSid != StencilID::stencil_jumpTrue) continue;
        bool jumpOnFalse = (jmpSid == StencilID::stencil_jumpFalse);

        StencilID fused = StencilID::stencil_nop;
        switch (cmpSid) {
        case StencilID::stencil_lessThanSmallInt:
            fused = jumpOnFalse ? StencilID::stencil_ltJumpFalse : StencilID::stencil_ltJumpTrue;
            break;
        case StencilID::stencil_greaterThanSmallInt:
            fused = jumpOnFalse ? StencilID::stencil_gtJumpFalse : StencilID::stencil_gtJumpTrue;
            break;
        case StencilID::stencil_lessEqualSmallInt:
            fused = jumpOnFalse ? StencilID::stencil_leJumpFalse : StencilID::stencil_leJumpTrue;
            break;
        case StencilID::stencil_greaterEqualSmallInt:
            fused = jumpOnFalse ? StencilID::stencil_geJumpFalse : StencilID::stencil_geJumpTrue;
            break;
        case StencilID::stencil_equalSmallInt:
            fused = jumpOnFalse ? StencilID::stencil_eqJumpFalse : StencilID::stencil_eqJumpTrue;
            break;
        case StencilID::stencil_notEqualSmallInt:
            fused = jumpOnFalse ? StencilID::stencil_neqJumpFalse : StencilID::stencil_neqJumpTrue;
            break;
        case StencilID::stencil_identicalTo:
            fused = jumpOnFalse ? StencilID::stencil_identJumpFalse : StencilID::stencil_identJumpTrue;
            break;
        case StencilID::stencil_notIdenticalTo:
            fused = jumpOnFalse ? StencilID::stencil_notIdentJumpFalse : StencilID::stencil_notIdentJumpTrue;
            break;
        default: continue;
        }

        // Replace comparison with fused stencil, keeping its bcOffset and operand (deopt offset)
        cmp.stencilIdx = static_cast<uint16_t>(fused);
        cmp.branchTarget = jmp.branchTarget;  // Take the jump's branch target
        // Replace jump with nop (its bytecode offset is preserved for the bcToCode map)
        jmp.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
        jmp.branchTarget = -1;
        pi++;  // Skip the consumed jump
    }

    // Backward-jump upgrade: replace forward-jump stencils with yield-checking
    // backward-jump stencils for branches targeting earlier bytecodes.
    // OPERAND = branch target bytecode offset (for ip update on ExitYield).
    for (size_t bi = 0; bi < decoded.size(); bi++) {
        auto& bc = decoded[bi];
        if (bc.branchTarget < 0) continue;
        if (bc.branchTarget >= bc.bcOffset) continue;  // Forward jump — no change
        auto sid = static_cast<StencilID>(bc.stencilIdx);
        if (sid == StencilID::stencil_jump) {
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_jumpBack);
            bc.operand = bc.branchTarget;  // bytecode offset of branch target
        } else if (sid == StencilID::stencil_jumpTrue) {
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_jumpTrueBack);
            bc.operand = bc.branchTarget;
            // OPERAND2 = bcOffset of this jumpBack (must-bool bail target).
            bc.operand2 = bc.bcOffset;
        } else if (sid == StencilID::stencil_jumpFalse) {
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_jumpFalseBack);
            bc.operand = bc.branchTarget;
            bc.operand2 = bc.bcOffset;
        }
    }

    // Forward conditional jumps: stencil_jumpFalse / stencil_jumpTrue use
    // OPERAND as the bcOffset of THIS conditional, so on a non-Boolean
    // condition the stencil can set s->ip = bcStart + bcOffset and exit
    // with ExitMustBool — the interpreter then re-executes the
    // conditional and fires sendMustBeBoolean per spec.  Backward
    // variants use OPERAND for the yield-resume target and OPERAND2
    // for the same must-bool bail bcOffset.
    for (size_t bi = 0; bi < decoded.size(); bi++) {
        auto& bc = decoded[bi];
        auto sid = static_cast<StencilID>(bc.stencilIdx);
        if (sid == StencilID::stencil_jumpTrue ||
            sid == StencilID::stencil_jumpFalse) {
            bc.operand = bc.bcOffset;
        }
    }

    // IC-guided specialization: if recompiling with IC data, replace
    // monomorphic sendJ2J sites with inline getter/setter/returnsSelf stencils.
    if (oldVersion) {
        applyICSpecialization(decoded, oldVersion, methObj, numLiterals);
    }

    // Selector-based block-value specialization (PHARO_BLOCK_VALUE_SPEC=1
    // opt-in, 2026-05-03): infra is in place via
    // JITRuntime::resolveFullBlockClosureClassIndex(); the actual
    // applyICSpecialization-equivalent branch was attempted on
    // 2026-05-03 PM but caused a Morphic-startup hang we couldn't
    // diagnose in-session.  Documented in docs/jit-multiweek-work.md
    // for future work.

    // Parallel to `decoded` after SimStack: entryState[i] is the SimStack
    // state at entry to decoded[i]. state != 0 means the stencil reads
    // cached operands from x19..x22 and is NOT safe as a tryResume target
    // (registers are undefined after an interpreter round-trip).
    std::vector<int> simStackEntryState;

    // SimStack: register-based TOS/NOS caching in x19/x20.
    // Stencils use inline asm to read/write x19/x20 without clobber lists.
    // The compiler doesn't touch callee-saved regs in these tail-call
    // functions. extract_stencils.py verifies this statically.
#ifdef __aarch64__
    {
        // SimStack caches stack TOS/NOS/3rd/4th in x19-x22 between bytecode
        // stencils.  Disabled by default in b9ab22e (2026-04-17) after
        // Integer>>benchmark hangs surfaced after ~9 compiles — arith
        // overflow bail spilled x19/x20 in a way that left nil where a
        // boolean was expected.
        //
        // Re-enabled 2026-04-18 (137e7d5) after simple-case retesting
        // (benchmark, benchFib, whileTrue:, ifTrue:ifFalse:) suggested
        // the bug had been closed by intervening IC/stencil fixes.
        // Array-fill bench went 33ms → 22ms (33% faster).
        //
        // RE-DISABLED same day: running the full IntegerTest suite
        // (80 tests) with SimStack on produces 7 fail + 5 err that
        // do NOT reproduce in interpreter mode (0 fail, 0 err) and
        // do NOT reproduce in isolation (each failing test passes
        // individually).  State-dependent corruption across
        // many-test accumulation — the original b9ab22e symptom
        // manifests differently but is not truly fixed.
        //
        // 2026-04-29 retest: bench panel is now NET REGRESSION under
        // PHARO_JIT_SIMSTACK=1.  3-run averages with INLINE_ARITH
        // default-on:
        //   tiny bc/s: 23.0M vs 25.0M (-8%)
        //   1M getter:  110ms vs 100ms (+10%)
        //   fib(28):     24ms vs  21ms (+14%)
        // The "33% bench win" was a single Array-fill bench that's
        // since been dominated by other optimizations.  Default-OFF
        // is now the right call for *both* correctness AND perf.
        // `PHARO_JIT_SIMSTACK=1` enables for bisection / targeted
        // benches where the IntegerTest accumulation doesn't apply.
        // `PHARO_JIT_NO_SIMSTACK=1` also disables (backwards compat).
        // Strict "=1" semantics: no plain presence here because setting to
        // "0" explicitly should *not* toggle.  Keep the raw getenv.
        static bool noSimStack = []() {
            const char* noEnv = std::getenv("PHARO_JIT_NO_SIMSTACK");
            if (noEnv && noEnv[0] == '1') return true;
            const char* yesEnv = std::getenv("PHARO_JIT_SIMSTACK");
            if (yesEnv && yesEnv[0] == '1') return false;
            return true;  // default: SimStack OFF (correctness-first)
        }();
        // Per-selector SimStack disable for bisection:
        // PHARO_JIT_NO_SIMSTACK_SELECTORS=sel1,sel2,...
        bool skipSimStackHere = noSimStack;
        if (!skipSimStackHere) {
            static const char* skipSimStackEnv = g_debug.jitNoSimStackSelectors;
            if (skipSimStackEnv && *skipSimStackEnv) {
                std::string sel = interp_.memory().selectorOf(compiledMethod);
                const char* p = skipSimStackEnv;
                while (*p) {
                    const char* end = p;
                    while (*end && *end != ',') end++;
                    if ((size_t)(end - p) == sel.size() &&
                        std::memcmp(p, sel.data(), sel.size()) == 0) {
                        skipSimStackHere = true;
                        break;
                    }
                    p = (*end == ',') ? end + 1 : end;
                }
            }
        }
        if (!skipSimStackHere) applySimStack(decoded, simStackEntryState);
        if (simStackEntryState.size() != decoded.size())
            simStackEntryState.assign(decoded.size(), 0);

        // Spill-warning: flag any method that emits a stencil_*_4 variant
        // (push/dup beyond 4 regs → memory spill) or any flush[1-4] stencil
        // (state→memory transitions at barriers).
        //
        // Enabled by PHARO_JIT_SPILL_WARN=1. push_4/dup_4 spills the SP
        // forward; flush1..flush4 always write 1-4 values to memory at the
        // current sp. Either path can leak/corrupt stack slots if the
        // compile-time SimStack state disagrees with runtime register
        // contents.
        {
            // Strict "=1" semantics preserved.
            static const bool spillEnv = []() {
                const char* v = std::getenv("PHARO_JIT_SPILL_WARN");
                return v && *v == '1';
            }();
            if (spillEnv) {
                int spillCount = 0;   // push_4/dup_4 → memory
                int flushCount = 0;   // flush[1-4] → memory
                for (auto& d : decoded) {
                    auto sid = static_cast<StencilID>(d.stencilIdx);
                    switch (sid) {
                    case StencilID::stencil_pushTemp_4:
                    case StencilID::stencil_pushRecvVar_4:
                    case StencilID::stencil_pushLitConst_4:
                    case StencilID::stencil_pushLitVar_4:
                    case StencilID::stencil_pushReceiver_4:
                    case StencilID::stencil_pushTrue_4:
                    case StencilID::stencil_pushFalse_4:
                    case StencilID::stencil_pushNil_4:
                    case StencilID::stencil_dup_4:
                        spillCount++;
                        break;
                    case StencilID::stencil_flush1:
                    case StencilID::stencil_flush2:
                    case StencilID::stencil_flush3:
                    case StencilID::stencil_flush4:
                        flushCount++;
                        break;
                    default:
                        break;
                    }
                }
                if (spillCount > 0 || flushCount > 0) {
                    std::string sel = interp_.memory().selectorOf(compiledMethod);
                    fprintf(stderr, "[JIT-SPILL] #%zu #%s: %d spill + %d flush\n",
                            methodsCompiled_, sel.c_str(), spillCount, flushCount);
                }
            }
        }

        // Post-SimStack dump (JIT_DUMP_BC_POST=selectorName)
        // Includes entry SimStack state + stencil name so we can eyeball the
        // state transitions for a suspect method (e.g., method #8 max:).
        {
            static const char* dumpSel = g_debug.jitDumpBCPost;
            if (dumpSel && *dumpSel) {
                std::string sel = interp_.memory().selectorOf(compiledMethod);
                if (sel == dumpSel) {
                    fprintf(stderr, "[JIT-BC-POST] #%s post-SimStack stencils (size=%zu):\n",
                            sel.c_str(), decoded.size());
                    for (size_t d = 0; d < decoded.size(); d++) {
                        int entrySt = (d < simStackEntryState.size()) ? simStackEntryState[d] : -1;
                        const char* name = (decoded[d].stencilIdx < NumStencils)
                                           ? stencilTable[decoded[d].stencilIdx].name
                                           : "???";
                        fprintf(stderr,
                                "[JIT-BC-POST]   [%zu] st=%d %s(id=%u) op=%d bc=%d br=%d\n",
                                d, entrySt, name, decoded[d].stencilIdx,
                                decoded[d].operand, decoded[d].bcOffset, decoded[d].branchTarget);
                    }
                }
            }
        }
    }
#endif

    // Cross-arch dump (x86_64 + aarch64) — useful for debugging miscompiles
    // on the Linux side.  No SimStack entry state on x86 (variable is empty),
    // so st=0 across the board.
    {
        static const char* dumpSel = g_debug.jitDumpBCPost;
        if (dumpSel && *dumpSel) {
            std::string sel = interp_.memory().selectorOf(compiledMethod);
            if (sel == dumpSel) {
                std::string cls = interp_.classNameOfMethod(compiledMethod);
                fprintf(stderr, "[JIT-BC-POST] #%s>>%s methodOop=0x%llx post-decode stencils (size=%zu):\n",
                        cls.c_str(), sel.c_str(),
                        (unsigned long long)compiledMethod.rawBits(),
                        decoded.size());
                // Dump the raw bytecode bytes too — needed to verify whether
                // the decoder is reading what we think it's reading.
                fprintf(stderr, "[JIT-BC-POST]   raw bytecodes (%zu bytes):", bcLen);
                for (size_t b = 0; b < bcLen; b++) {
                    fprintf(stderr, " %02x", bytecodes[b]);
                }
                fprintf(stderr, "\n");
                for (size_t d = 0; d < decoded.size(); d++) {
                    int entrySt = (d < simStackEntryState.size()) ? simStackEntryState[d] : -1;
                    const char* name = (decoded[d].stencilIdx < NumStencils)
                                       ? stencilTable[decoded[d].stencilIdx].name
                                       : "???";
                    fprintf(stderr,
                            "[JIT-BC-POST]   [%zu] st=%d %s(id=%u) op=%d op2=%d bc=%d br=%d\n",
                            d, entrySt, name, decoded[d].stencilIdx,
                            decoded[d].operand, decoded[d].operand2,
                            decoded[d].bcOffset, decoded[d].branchTarget);
                }
            }
        }
    }

    // Ensure entryState has a valid size even when SimStack was skipped or
    // compiled on a non-ARM64 host (no register-caching variants exist).
    if (simStackEntryState.size() != decoded.size())
        simStackEntryState.assign(decoded.size(), 0);

    // First pass: compute total code size and build bytecode->code offset map
    // We also need a literal pool for GOT-style patching
    uint32_t codeSize = 0;
    uint32_t maxLiteralSlots = 0;

    // Map from bytecode offset to machine code offset.
    // Two maps:
    //   bcToCodeOffset: last-write-wins — points to the "real" stencil at each bcOffset.
    //     Used by tryResume (re-entering JIT from interpreter with Empty SimStack state).
    //   bcToBranchOffset: ALSO last-write-wins — points to the real stencil at each
    //     bcOffset, bypassing any SimStack flushes inserted before it.
    //
    // Why last-write-wins for branches: a SimStack fallthrough-flush inserted
    // before a branch target assumes x19/x20 hold live cached values. But when
    // a jump-taken predecessor arrives, x19/x20 are stale (jumps don't touch
    // them), and running flush1/flush2 writes stale registers to the stack,
    // corrupting TOS. The only sound layout is: jumps land on the REAL stencil
    // (state 0 entry); fallthrough still runs through the inserted flush
    // because the flush is emitted earlier in linear code order. So both
    // tables use last-write-wins and, in fact, are identical.
    std::vector<uint32_t> bcToCodeOffset(bcLen + 1, 0);
    std::vector<uint32_t> bcToBranchOffset(bcLen + 1, 0);
    // Parallel to bcToCodeOffset: SimStack entry state of the stencil at each
    // bcOffset (last-write-wins, matching bcToCodeOffset). Diagnostic only;
    // shipped to JITRuntime when PHARO_RESUME_STATE_DEBUG=1.
    std::vector<uint8_t> bcToEntryState(bcLen + 1, 0);

    for (size_t di = 0; di < decoded.size(); di++) {
        auto& bc = decoded[di];
        // Fused bytecodes (nop placeholders from peephole fusion) must NOT have
        // valid re-entry points. Their bcToCode entries stay 0, so tryResume
        // rejects them and the interpreter handles the bytecode. Without this,
        // resuming at a fused jumpFalse enters the fused stencil's CONTINUE
        // path unconditionally, ignoring the actual comparison result.
        (void)simStackEntryState;  // plumbed through for future use
        if (static_cast<StencilID>(bc.stencilIdx) != StencilID::stencil_nop) {
            // Last-write-wins for code offsets: the real stencil at bcOffset X
            // overwrites any SimStack flush inserted before it, so jumps land
            // on the real stencil (post-flush, empty SimStack).
            bcToCodeOffset[bc.bcOffset] = codeSize;
            bcToBranchOffset[bc.bcOffset] = codeSize;
            // Max-wins for entry state: if a flush stencil at this bcOffset has
            // a non-zero entry state (registers pending), keep it. tryResume
            // must reject entry at offsets with pending register values — the
            // flush stencil pushes those registers and we'd skip it if we enter
            // at the post-flush real stencil. Without this, the skipped flush
            // leaks stack slots on every resume (session 20: pollEvent: leak).
            uint8_t st = (di < simStackEntryState.size())
                ? static_cast<uint8_t>(simStackEntryState[di]) : 0;
            bcToEntryState[bc.bcOffset] = std::max(bcToEntryState[bc.bcOffset], st);
            for (int b = 1; b < bc.bcLength && (bc.bcOffset + b) <= (int)bcLen; b++) {
                bcToCodeOffset[bc.bcOffset + b] = codeSize;
                bcToBranchOffset[bc.bcOffset + b] = codeSize;
                bcToEntryState[bc.bcOffset + b] = std::max(bcToEntryState[bc.bcOffset + b], st);
            }
        }
        // Even nop stencils contribute to code size (they emit a branch instruction)
        const StencilDef& stencil = stencilTable[bc.stencilIdx];
        codeSize += stencil.codeSize;
        // Count literal pool slots needed.
        // ARM64: one slot per GOT pair (counted via PAGE21, the second reloc).
        // x86_64: one slot per non-branch reloc (each GOT ref is a single instruction).
        for (uint16_t r = 0; r < stencil.numRelocs; r++) {
            const auto& rel = stencil.relocs[r];
            if constexpr (HostArch == Arch::ARM64) {
                if (rel.type == RelocType::ARM64_GOT_LOAD_PAGE21) {
                    maxLiteralSlots++;
                }
            } else {
                if (rel.hole != HoleKind::Continue && rel.hole != HoleKind::BranchTarget) {
                    maxLiteralSlots++;
                }
            }
        }
    }
    // Sentinel: code offset for "one past the last bytecode"
    bcToCodeOffset[bcLen] = codeSize;
    bcToBranchOffset[bcLen] = codeSize;

    // Clear bcToCodeOffset for bytecodes with non-zero entry state.
    // Flush stencils at a bytecode offset push register values to the stack;
    // the post-flush real stencil (where bcToCodeOffset points) assumes those
    // values are already there. Resume from interpreter skips the flush, so
    // entering at the real stencil would read stale stack data and leak slots.
    // Setting bcToCodeOffset to 0 makes codeOffsetForBC return 0, which ALL
    // callers (tryResume, J2J trampoline, chain loop, asm trampoline) treat
    // as "no valid entry point".
    for (size_t i = 0; i <= bcLen; i++) {
        if (bcToEntryState[i] != 0) {
            bcToCodeOffset[i] = 0;
        }
    }

    // Literal pool lives after the code, 8-byte aligned
    uint32_t literalPoolOffset = (codeSize + 7) & ~7u;
    uint32_t literalPoolSize = maxLiteralSlots * 8;
    // bcToCode re-entry table lives after the literal pool, 4-byte aligned
    uint32_t bcToCodeTableOffset = (literalPoolOffset + literalPoolSize + 3) & ~3u;
    uint32_t bcToCodeTableSize = (static_cast<uint32_t>(bcLen) + 1) * sizeof(uint32_t);

    // Count send sites for inline cache data allocation.
    // sendInlineMonoJ2J also reads icData[0/1/2] from operand2Ptr, so it
    // needs an IC slot allocated and operand2Ptr populated below.
    //
    // CRITICAL: this list MUST match the IC-setup loop below (line ~2386
    // currently).  When stencil_sendInlineMultiSlot was added to the
    // setup loop without updating this counter, methods with multi-slot
    // sends got numSendSites=0 → icBuffer not allocated (NULL) → setup
    // loop writes through NULL+offset → SIGSEGV at 0x90 in the recompile
    // path.  Root cause of deferred.md A1 P0, lldb-confirmed 2026-05-06.
    uint16_t numSendSites = 0;
    for (auto& bc : decoded) {
        auto sid = static_cast<StencilID>(bc.stencilIdx);
        if (sid == StencilID::stencil_sendJ2J ||
            sid == StencilID::stencil_sendInlineMonoJ2J ||
            sid == StencilID::stencil_sendInlineReturnsLiteral ||
            sid == StencilID::stencil_sendInlineMultiSlot)
            numSendSites++;
    }

    // IC data lives after bcToCode table, 8-byte aligned. Each send site's
    // size comes from IC_BYTES_PER_SITE in JITMethod.hpp. The 'extra' field
    // holds J2J info: high byte = kind (0x80=getter, 0x40=setter), low 32
    // bits = slot index. When kind != 0, the send stencil inlines the field
    // access directly, bypassing the C++ boundary crossing entirely.
    // Task #41: side-channel selBits array, one u64 per send site.
    // Placed BEFORE IC data (not after) so the invariant
    // `icStart = codeStart + codeSize - numICEntries*IC_BYTES_PER_SITE`
    // stays true for all callers.  Accessed via JITMethod::selBitsArray().
    uint32_t selBitsArrayOffset = (bcToCodeTableOffset + bcToCodeTableSize + 7) & ~7u;
    uint32_t selBitsArraySize = numSendSites * sizeof(uint64_t);
    // 2026-05-03: IC data moved out of MAP_JIT into a heap-side buffer
    // (JITMethod::icBuffer) so per-fill W^X flips disappear.  No more
    // in-zone icDataOffset / icDataSize — totalSize ends after
    // selBitsArray.  The pointer baked into operand2Ptr below points
    // at the heap buffer instead of the in-zone offset.
    uint32_t totalSize = selBitsArrayOffset + selBitsArraySize;

    // The code zone is kept in writable W^X mode by default (set during
    // initialize). We write freely here; tryExecute() toggles to executable
    // only around the actual machine code call.

    // Compute the actual allocation size (allocate() adds JITMethod header + alignment)
    size_t allocSize = sizeof(JITMethod) + totalSize;
    allocSize = (allocSize + MethodAlignment - 1) & ~(MethodAlignment - 1);

    // Allocate in code zone (tries bump pointer, then free list).
    // Pass numSendSites so CodeZone::allocate also allocates the
    // heap-side icBuffer (2026-05-03: ICs moved out of MAP_JIT).
    JITMethod* jitMethod = zone_.allocate(totalSize, numSendSites);
    if (!jitMethod) {
        // Incremental eviction: free cold methods into the free list.
        // allocate() will reuse freed space without moving methods
        // (ADRP+LDR relocations in stencils are not position-independent
        // across non-page-aligned moves).
        //
        // SAFETY: before evicting, pin any JIT method that is *live* on the
        // active stack.  Otherwise a native RET from a trampoline/primitive
        // back into JIT code lands in freelist memory — SIGSEGV with
        // "PC not in any active JIT method (evicted?)".  Two sources of live
        // methods:
        //   (1) J2JSave pool entries — chain-loop return frames.
        //   (2) Native frame-pointer chain — LRs may be PCs inside JIT code.
        // Unpinned on exit via the Unpin RAII below.
        auto pinLiveMethods = [&]() {
            // (1) J2JSave pool — chain-loop return frames for ongoing J2J
            // calls.  Every live entry names a jitMethod that we must not
            // evict (otherwise the chain-loop's resume into it blows up).
            int live = interp_.j2jPoolLiveCount();
            const auto* base = interp_.j2jPoolBase();
            for (int i = 0; i < live; i++) {
                if (base[i].jitMethod && zone_.contains(base[i].jitMethod)) {
                    base[i].jitMethod->pinned = true;
                }
            }
            // (2) Native frame-pointer chain — LRs may be PCs inside JIT
            // code (trampoline return into caller's stencil).  Bounded by
            // pthread stack, plus a depth cap.  Apple and Linux report
            // stack info via different APIs; the platform layer exposes
            // a uniform getStackBounds(top, bot) so this code stays
            // ifdef-free.
            uint8_t* stackTop = nullptr;
            uint8_t* stackBot = nullptr;
            if (!pharo::platform::getStackBounds(&stackTop, &stackBot)) {
                return;  // can't walk; pin nothing
            }
            uint64_t* fp = static_cast<uint64_t*>(__builtin_frame_address(0));
            for (int depth = 0; depth < 256; depth++) {
                uint8_t* fpB = reinterpret_cast<uint8_t*>(fp);
                if (fpB + 16 > stackTop || fpB < stackBot) break;
                // fp[0] = saved caller fp, fp[1] = saved LR (arm64 prologue)
                uint64_t lr = fp[1];
                if (lr) {
                    auto* m = zone_.findMethodByPC(lr);
                    if (m) m->pinned = true;
                }
                uint64_t* next = reinterpret_cast<uint64_t*>(fp[0]);
                // Caller frame is at a HIGHER address than callee on a
                // downward-growing stack.  Bail on any inversion or reset.
                if (!next || next <= fp) break;
                fp = next;
            }
        };
        struct UnpinAll {
            CodeZone& z;
            ~UnpinAll() {
                for (auto* m = z.firstMethod(); m; m = m->nextInZone) m->pinned = false;
            }
        } unpinAll{zone_};
        pinLiveMethods();

        // Collect evicted code ranges during eviction via pre-eviction callback,
        // so we capture ALL evicted methods (both first-pass and second-pass).
        struct EvictedRange { uint64_t start; uint64_t end; };
        std::vector<EvictedRange> evictedRanges;
        evictedRanges.reserve(32);

        auto evictCallback = [](uint64_t methodOop, void* ctx) {
            auto* map = static_cast<MethodMap*>(ctx);
            map->remove(methodOop);
        };
        auto preEvictCallback = [](JITMethod* m, void* ctx) {
            auto* ranges = static_cast<std::vector<EvictedRange>*>(ctx);
            uint64_t s = reinterpret_cast<uint64_t>(m->codeStart());
            ranges->push_back({s, s + m->codeSize});
        };
        // Evict at least 2x what we need (amortize eviction cost)
        size_t evictTarget = allocSize * 2;
        size_t freed = zone_.evictLRU(evictTarget, evictCallback, &methodMap_,
                                       preEvictCallback, &evictedRanges);
        if (freed > 0) {
            static int evictCount = 0;
            if (++evictCount <= 3 || (evictCount % 500 == 0)) {
                fprintf(stderr, "[JIT] Incremental evict #%d: freed %zu bytes for %zu needed, "
                        "%zu methods remain, freeList=%zu\n",
                        evictCount, freed, allocSize,
                        zone_.methodCount(), zone_.freeListFreeBytes());
            }
            // Clear only J2J IC entries (bit 60) pointing to evicted code ranges.
            // This preserves classKey/methodBits/getter/setter IC data for surviving
            // methods, avoiding the massive re-patching overhead of a full flush.
            static constexpr uint64_t J2J_BIT = 1ULL << 60;
            static constexpr uint64_t ADDR_MASK = 0x0000FFFFFFFFFFFFULL;
            JITMethod* im = zone_.firstMethod();
            while (im) {
                if (im->numICEntries > 0 && im->icBuffer) {
                    uint8_t* icStart = im->icZoneStart();
                    for (uint32_t i = 0; i < im->numICEntries; i++) {
                        uint64_t* slots = reinterpret_cast<uint64_t*>(
                            icStart + i * IC_BYTES_PER_SITE);
                        for (uint32_t e = 0; e < IC_ENTRIES_PER_SITE; e++) {
                            uint64_t extra = slots[e * 3 + 2];
                            if (!(extra & J2J_BIT)) continue;
                            uint64_t addr = extra & ADDR_MASK;
                            for (auto& r : evictedRanges) {
                                if (addr >= r.start && addr < r.end) {
                                    slots[e * 3 + 2] = 0;
                                    break;
                                }
                            }
                        }
                    }
                }
                im = im->nextInZone;
            }
            jitMethod = zone_.allocate(totalSize, numSendSites);
        }

        // If incremental eviction wasn't enough, full flush as last resort.
        // Must still respect pinned methods — skip them and retry allocate.
        if (!jitMethod) {
            static int fullFlushCount = 0;
            if (++fullFlushCount <= 5) {
                fprintf(stderr, "[JIT] Full zone flush #%d (needed %zu bytes, "
                        "evicted %zu, freeList=%zu, bump=%zu)\n",
                        fullFlushCount, allocSize, freed,
                        zone_.freeListFreeBytes(), zone_.bumpFreeBytes());
            }
            JITMethod* m = zone_.firstMethod();
            while (m) {
                JITMethod* next = m->nextInZone;
                if (!m->pinned) {
                    uint64_t oop = m->compiledMethodOop;
                    zone_.freeMethod(m);
                    methodMap_.remove(oop);
                }
                m = next;
            }
            // compact() only resets bump pointer when zone is empty of live
            // methods.  With pinned survivors present, skip compact and let
            // allocate() use the free list.
            if (zone_.firstMethod() == nullptr) zone_.compact();

            jitMethod = zone_.allocate(totalSize, numSendSites);
            if (!jitMethod) {
                compilationsFailed_++;
                return nullptr;
            }
        }
    }

    // Fill in method header
    jitMethod->compiledMethodOop = compiledMethod.rawBits();
    jitMethod->methodHeader = static_cast<uint64_t>(headerBits);
    jitMethod->codeSize = totalSize;
    jitMethod->numBytecodes = static_cast<uint16_t>(bcLen);
    jitMethod->numICEntries = numSendSites;
    // Task #41: side-channel selBits array offset (0 if no send sites).
    jitMethod->selBitsArrayOffset = numSendSites > 0 ? selBitsArrayOffset : 0;
    jitMethod->tier = 1;

    // Extract arg/temp counts from header
    jitMethod->argCount = static_cast<uint8_t>((headerBits >> 24) & 0x0F);
    jitMethod->tempCount = static_cast<uint8_t>((headerBits >> 18) & 0x3F);
    jitMethod->hasPrimPrologue = hasPrimPrologue;
    jitMethod->isBlock = isFullBlock;
    jitMethod->pinned = false;  // Eviction safety — pinned transiently during evictLRU
    // isSpliceTarget: check SistaRuntime's set immediately so the flag is
    // reliable from first activation (lazy set in tryExecute /
    // noteMethodEntry would miss J2J-only callees that never enter
    // tryExecute).
    jitMethod->isSpliceTarget =
        sistaRuntimeForGCHook_
        && sistaRuntimeForGCHook_->hasSplice(compiledMethod);
    // Initial j2jDepthLimit lives in the heap-side stats struct (W^X
    // audit 2026-04-26).  CodeZone::allocate has already alloc'd
    // jitMethod->stats and zeroed it; bump the depth limit here.
    if (jitMethod->stats) {
        jitMethod->stats->j2jDepthLimit = 2;  // Start conservative; adapts up on clean runs
    }

    // Set up IC data pointers for send sites. The IC data lives at the end
    // of the allocation. Each send site gets 152 bytes initialized to zero
    // (empty IC — will be patched on first miss): 6 x [key, method, extra]
    // + selectorBits. The extra word encodes inline getter/setter info for
    // J2J dispatch (bit 63=getter, bit 62=setter, bit 61=returnsSelf).
    uint8_t* codeBase_pre = jitMethod->codeStart();
    uint8_t* icBufferBase = jitMethod->icZoneStart();  // heap, not in MAP_JIT
    uint32_t icDataSize = numSendSites * IC_BYTES_PER_SITE;
    // Bail out if IC buffer allocation failed but the method has send
    // sites — without this, the loop below writes to icSlots[18] at
    // offset 0x90 from NULL → SIGSEGV (deferred.md A1 P0 root cause
    // observed at low PHARO_JIT_DEFER, lldb-confirmed 2026-05-06 at
    // JITCompiler.cpp:2386).  Free the partially-set-up jitMethod
    // and return nullptr so caller falls back to interp.
    if (numSendSites > 0 && !icBufferBase) {
        compilationsFailed_++;
        fprintf(stderr,
                "[JIT] FAIL: icBuffer NULL but numSendSites=%u for "
                "method 0x%llx — bailing\n",
                numSendSites,
                (unsigned long long)compiledMethod.rawBits());
        zone_.freeMethod(jitMethod);
        return nullptr;
    }
    {
        // Zero IC data area first (icBuffer was zero-init by calloc, but
        // recompile path memcpys over it; the explicit memset keeps the
        // semantics symmetric with the prior in-zone layout).
        if (icBufferBase) std::memset(icBufferBase, 0, icDataSize);

        // On recompile: copy IC entries from old to new.  Without this,
        // applyICSpecialization emits stencil_sendInlineMonoJ2J for sites
        // that were monomorphic in the old IC, but the new IC starts
        // empty — so MonoJ2J's `lookupKey == icData[0]` check fails on
        // every call and bails to ExitSend (slow path).  The bench-suite
        // 1M getter regression under PHARO_OSR_RECOMPILE=1 (100→374 ms)
        // and the bench-panel 7→11 ms regression both stem from this
        // empty-IC-after-recompile bug.  Copying preserves IC state so
        // MonoJ2J hits on the first call.
        //
        // Safety: classKey is classIndex (GC-stable); methodBits and
        // selectorBits are Oops that GC's recoverAfterGC zeroes —
        // identical staleness profile to a non-recompile-time IC fill.
        // J2J entry addresses in `extra` point into the JIT code zone
        // (not heap), and the old method's code stays valid until
        // eviction.  numICEntries matches between old and new (same
        // bytecode → same send sites).
        if (oldVersion && oldVersion->numICEntries == numSendSites
                && oldVersion->icBuffer && icBufferBase) {
            std::memcpy(icBufferBase,
                        oldVersion->icZoneStart(),
                        icDataSize);
        }

        // Get special selectors array for 0x70-0x7F sends
        Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
        ObjectHeader* ssArray = (specialSelectors.isObject() && specialSelectors.rawBits() > 0x10000)
            ? specialSelectors.asObjectPtr() : nullptr;

        // §1.3a: populate the shared-IC send-site map so Tier2Compiler
        // can look up sendIdx for a given bytecode offset and share
        // this method's IC table.  Replaces any prior entry (e.g.,
        // after recompile).
        std::vector<uint16_t>& siteOffsets =
            sendSiteMap_[compiledMethod.rawBits()];
        siteOffsets.clear();
        siteOffsets.reserve(numSendSites);

        uint16_t sendIdx = 0;
        for (auto& bc : decoded) {
            auto sid = static_cast<StencilID>(bc.stencilIdx);
            if (sid == StencilID::stencil_sendJ2J ||
                sid == StencilID::stencil_sendInlineMonoJ2J ||
                sid == StencilID::stencil_sendInlineReturnsLiteral ||
                sid == StencilID::stencil_sendInlineMultiSlot) {
                // Critical defensive check (deferred.md A1 P0): icBufferBase
                // can be NULL if calloc failed at allocate() time.  Without
                // this guard, icBase = NULL+offset and the slot writes
                // below SIGSEGV at address 0x90.
                if (!icBufferBase) {
                    static int n = 0;
                    if (++n <= 5) {
                        fprintf(stderr,
                                "[JIT-FATAL-NULL-IC] compile: icBufferBase NULL "
                                "but sendJ2J site present (numSendSites=%u "
                                "method=0x%llx is_recompile=%d)\n",
                                numSendSites,
                                (unsigned long long)compiledMethod.rawBits(),
                                oldVersion != nullptr);
                    }
                    compilationsFailed_++;
                    zone_.freeMethod(jitMethod);
                    return nullptr;
                }
                uint8_t* icBase = icBufferBase + sendIdx * IC_BYTES_PER_SITE;
                bc.operand2Ptr = reinterpret_cast<uint64_t>(icBase);
                siteOffsets.push_back(static_cast<uint16_t>(bc.bcOffset));

                // Store selectorBits at offset 144 (icData[18]) for mega cache probe
                uint64_t selectorBits = 0;
                if (bc.opcode >= 0x60 && bc.opcode <= 0x6F) {
                    // Arithmetic special selector (index 0-15): from special objects array
                    int selectorIndex = bc.opcode - 0x60;
                    if (ssArray) {
                        size_t selectorSlot = selectorIndex * 2;
                        if (selectorSlot < ssArray->slotCount()) {
                            selectorBits = ssArray->slotAt(selectorSlot).rawBits();
                        }
                    }
                } else if (bc.opcode >= 0x70 && bc.opcode <= 0x7F) {
                    // Special selector 16-31: from special objects array
                    int selectorIndex = (bc.opcode - 0x70) + 16;
                    if (ssArray) {
                        size_t selectorSlot = selectorIndex * 2;
                        if (selectorSlot < ssArray->slotCount()) {
                            selectorBits = ssArray->slotAt(selectorSlot).rawBits();
                        }
                    }
                } else {
                    // Literal selector: from method's literal frame
                    int litIndex = bc.branchTarget;
                    if (litIndex >= 0 && litIndex < numLiterals) {
                        selectorBits = methObj->slotAt(1 + litIndex).rawBits();
                    }
                }
                uint64_t* icSlots = reinterpret_cast<uint64_t*>(icBase);
                icSlots[18] = selectorBits;
                // Task #41: also store in side-channel array.  Survives GC
                // memset of IC slots, so the megacache miss path has a
                // stable selector even when icSlots[18] has been zeroed.
                if (uint64_t* sba = jitMethod->selBitsArray()) {
                    sba[sendIdx] = selectorBits;
                }

                // A3 DIAG: count compile-time selBits=0 vs non-zero.
                static size_t compileSelZero = 0, compileSelNonZero = 0;
                if (selectorBits == 0) compileSelZero++;
                else compileSelNonZero++;
                if (g_debug.icHitDbg) {
                    fprintf(stderr, "[IC-COMPILE-SEL] %s opcode=0x%02X litIdx=%d numLits=%d icBase=%p selBits=0x%llx\n",
                            selectorBits == 0 ? "ZERO" : "OK",
                            bc.opcode, bc.branchTarget, numLiterals, (void*)icBase,
                            (unsigned long long)selectorBits);
                }

                sendIdx++;
            }
        }
    }

    // Classify method executability based on stencil content.
    // Three categories:
    //   1. hasSends: contains actual send stencils — can't execute (needs deopt)
    //   2. hasHeapWrites: writes receiver ivars or litvar — can't execute (needs write barrier)
    //   3. Neither: safe to execute. Arithmetic stencils may exit with ExitSend
    //      on non-SmallInteger inputs, which is handled by restoring SP.
    jitMethod->hasSends = false;
    jitMethod->hasHeapWrites = false;
    jitMethod->hasRecvFieldAccess = false;
    jitMethod->hasRecvFieldWrite = false;
    jitMethod->hasLitVarWrite = false;
    jitMethod->maxRecvFieldIndex = 0;
    for (auto& d : decoded) {
        auto sid = static_cast<StencilID>(d.stencilIdx);
        switch (sid) {
        // Receiver field access (read) — safe only for object receivers
        case StencilID::stencil_pushRecvVar:
            jitMethod->hasRecvFieldAccess = true;
            if (d.operand >= 0 && (uint8_t)d.operand > jitMethod->maxRecvFieldIndex)
                jitMethod->maxRecvFieldIndex = (uint8_t)d.operand;
            break;

        // Pure reads/stack ops — always safe
        case StencilID::stencil_pushTemp:
        case StencilID::stencil_pushLitConst:
        case StencilID::stencil_pushLitVar:
        case StencilID::stencil_pushReceiver:
        case StencilID::stencil_pushNil:
        case StencilID::stencil_pushTrue:
        case StencilID::stencil_pushFalse:
        case StencilID::stencil_pushZero:
        case StencilID::stencil_pushOne:
        case StencilID::stencil_pushInteger:
        case StencilID::stencil_dup:
        case StencilID::stencil_pop:
        case StencilID::stencil_nop:
        // Stack-only stores (write to tempBase, not heap)
        case StencilID::stencil_popStoreTemp:
        case StencilID::stencil_storeTemp:
        // Returns
        case StencilID::stencil_returnReceiver:
        case StencilID::stencil_returnTop:
        case StencilID::stencil_returnTrue:
        case StencilID::stencil_returnFalse:
        case StencilID::stencil_returnNil:
        // Control flow
        case StencilID::stencil_jump:
        case StencilID::stencil_jumpFalse:
        case StencilID::stencil_jumpTrue:
        case StencilID::stencil_jumpBack:
        case StencilID::stencil_jumpTrueBack:
        case StencilID::stencil_jumpFalseBack:
            break;  // safe

        // Arithmetic — may exit with ExitArithOverflow on non-SmallInteger,
        // handled by precise deopt in tryJITActivation
        case StencilID::stencil_addSmallInt:
        case StencilID::stencil_subSmallInt:
        case StencilID::stencil_mulSmallInt:
        case StencilID::stencil_lessThanSmallInt:
        case StencilID::stencil_greaterThanSmallInt:
        case StencilID::stencil_lessEqualSmallInt:
        case StencilID::stencil_greaterEqualSmallInt:
        case StencilID::stencil_equalSmallInt:
        case StencilID::stencil_notEqualSmallInt:
        case StencilID::stencil_divSmallInt:
        case StencilID::stencil_modSmallInt:
        case StencilID::stencil_bitAndSmallInt:
        case StencilID::stencil_bitOrSmallInt:
        case StencilID::stencil_bitShiftSmallInt:
            break;  // safe (ExitArithOverflow handled by precise deopt)

        // Heap writes — need write barrier (when gen GC is added)
        case StencilID::stencil_popStoreRecvVar:
        case StencilID::stencil_storeRecvVar:
            jitMethod->hasHeapWrites = true;
            jitMethod->hasRecvFieldAccess = true;
            jitMethod->hasRecvFieldWrite = true;
            if (d.operand >= 0 && (uint8_t)d.operand > jitMethod->maxRecvFieldIndex)
                jitMethod->maxRecvFieldIndex = (uint8_t)d.operand;
            break;
        case StencilID::stencil_popStoreLitVar:
        case StencilID::stencil_storeLitVar:
            jitMethod->hasHeapWrites = true;
            jitMethod->hasLitVarWrite = true;
            break;

        // Inlined special selectors — no deopt, pure computation
        case StencilID::stencil_identicalTo:
        case StencilID::stencil_notIdenticalTo:
            break;  // safe (identity compare, no side effects)

        // Remote temp access — reads/writes through temp vector, no heap alloc
        case StencilID::stencil_pushRemoteTemp:
            break;  // safe (read through temp vector)
        case StencilID::stencil_storeRemoteTemp:
        case StencilID::stencil_popStoreRemoteTemp:
            jitMethod->hasHeapWrites = true;  // writes to temp vector object
            break;

        // Block creation — exits to interpreter to allocate, then resumes
        case StencilID::stencil_pushBlock:
            jitMethod->hasSends = true;  // exits to interpreter
            break;

        // Sends — handled via deopt (stencil sets ip, exits to interpreter)
        case StencilID::stencil_send:
        case StencilID::stencil_sendJ2J:
            jitMethod->hasSends = true;  // Track for stats, but doesn't block execution
            break;

        // Inline monomorphic sends — getter reads heap, setter writes heap
        case StencilID::stencil_sendInlineGetter:
        case StencilID::stencil_sendInlineReturnsSelf:
        case StencilID::stencil_sendInlineReturnsLiteral:
        case StencilID::stencil_sendBlockValue1Arg:
        case StencilID::stencil_sendBlockValue0Arg:
        case StencilID::stencil_sendBlockValue2Arg:
        case StencilID::stencil_sendInlineMonoJ2J:
            jitMethod->hasSends = true;  // can deopt on class mismatch
            break;
        case StencilID::stencil_sendInlineSetter:
            jitMethod->hasSends = true;
            jitMethod->hasHeapWrites = true;
            break;

        default:
            jitMethod->hasSends = true;
            break;
        }
    }

    uint8_t* codeBase = jitMethod->codeStart();
    uint64_t* literalPool = reinterpret_cast<uint64_t*>(codeBase + literalPoolOffset);
    uint32_t nextLiteralSlot = 0;

    // Second pass: copy and patch stencils
    uint32_t offset = 0;
    for (size_t i = 0; i < decoded.size(); i++) {
        const DecodedBC& bc = decoded[i];
        const StencilDef& stencil = stencilTable[bc.stencilIdx];

        // Copy stencil bytes
        std::memcpy(codeBase + offset, stencil.code, stencil.codeSize);

        // Patch relocations
        if (!patchStencilInstance(codeBase, offset, stencil, bc,
                                  codeBase, totalSize, bcToCodeOffset,
                                  bcToBranchOffset,
                                  literalPool, literalPoolOffset,
                                  nextLiteralSlot)) {
            compilationsFailed_++;
            // Mark as invalidated so forEachRoot won't scan its stale Oop
            // and compaction can reclaim the space.
            jitMethod->invalidate();
            jitMethod->compiledMethodOop = 0;
            return nullptr;
        }

        offset += stencil.codeSize;
    }

    // Copy bcToCode re-entry table into the allocation
    jitMethod->bcToCodeTableOffset = bcToCodeTableOffset;
    uint32_t* tableBase = reinterpret_cast<uint32_t*>(codeBase + bcToCodeTableOffset);
    for (size_t b = 0; b <= bcLen; b++) {
        tableBase[b] = bcToCodeOffset[b];
    }

    // Post-emit machine-code dump (JIT_DUMP_HEX=selectorName).
    // Dumps each stencil's emitted bytes + reloc sites so we can diff against
    // the stencil template in generated_stencils_arm64.hpp. Used for verifying
    // that no stencil-copy/patch bug corrupted the code for a suspect method.
    {
        static const char* dumpHexSel = g_debug.jitDumpHex;
        if (dumpHexSel && *dumpHexSel) {
            std::string sel = interp_.memory().selectorOf(compiledMethod);
            if (sel == dumpHexSel) {
                fprintf(stderr, "[JIT-DUMP-HEX] #%s codeBase=%p codeSize=%u:\n",
                        sel.c_str(), (void*)codeBase, codeSize);
                uint32_t off = 0;
                for (size_t i = 0; i < decoded.size(); i++) {
                    const DecodedBC& bc = decoded[i];
                    const StencilDef& st = stencilTable[bc.stencilIdx];
                    fprintf(stderr, "[JIT-DUMP-HEX]   [%zu] +%u %s (size=%u bc=%d):\n",
                            i, off, st.name, st.codeSize, bc.bcOffset);
                    fprintf(stderr, "[JIT-DUMP-HEX]    ");
                    for (uint32_t b = 0; b < st.codeSize; b++) {
                        fprintf(stderr, " %02X", codeBase[off + b]);
                        if ((b & 15) == 15 && b + 1 < st.codeSize)
                            fprintf(stderr, "\n[JIT-DUMP-HEX]    ");
                    }
                    fprintf(stderr, "\n");
                    off += st.codeSize;
                }
            }
        }
    }

    // Flush icache for the newly written code
    flushICache(codeBase, totalSize);

    // Mark as compiled
    jitMethod->state = MethodState::Compiled;

    // Restore MAP_JIT view to executable for this thread.  CodeZone::
    // allocate() flipped to W; everything above wrote to MAP_JIT memory;
    // now we close the W window so the codebase invariant (W^X audit
    // 2026-04-26: thread is in X mode by default outside narrow write
    // windows) is preserved.  Without this, the next JIT entry SIGBUSes
    // because the page is still in W mode.
    makeExecutable(jitMethod, jitMethod->totalSize);

    // Register in method map
    methodMap_.insert(compiledMethod.rawBits(), jitMethod);

    // Ship per-bcOffset entry-state vector to runtime so tryResume can reject
    // register-reading (_N) entry offsets. Keyed by JITMethod* (GC-stable).
    // Always populated — this is a correctness prerequisite, not a diagnostic.
    // See deferred.md A1.
    {
        static const bool resumeStateDebug = g_debug.resumeStateDebug;
        if (resumeStateDebug) {
            static uint64_t methodsWithUnsafe = 0;
            static uint64_t totalUnsafeBc = 0;
            static uint64_t totalMethods = 0;
            uint32_t methodUnsafe = 0;
            for (uint8_t s : bcToEntryState) if (s != 0) methodUnsafe++;
            totalMethods++;
            if (methodUnsafe > 0) {
                methodsWithUnsafe++;
                totalUnsafeBc += methodUnsafe;
            }
            if (totalMethods <= 20 || (totalMethods % 100) == 0) {
                std::string sel = interp_.memory().selectorOf(compiledMethod);
                fprintf(stderr, "[JIT-COMPILE-STATE] method #%llu sel=%s unsafeBc=%u "
                        "(cumulative methodsWithUnsafe=%llu totalUnsafeBc=%llu)\n",
                        (unsigned long long)totalMethods, sel.c_str(), methodUnsafe,
                        (unsigned long long)methodsWithUnsafe,
                        (unsigned long long)totalUnsafeBc);
            }
        }
        interp_.jitRuntime().setBcEntryStates(jitMethod, std::move(bcToEntryState));
    }

    methodsCompiled_++;

    // Track method executability breakdown
    static size_t pureCount = 0, sendDeoptCount = 0, heapWriteCount = 0;
    if (!jitMethod->hasSends && !jitMethod->hasHeapWrites) pureCount++;
    else if (jitMethod->hasHeapWrites) heapWriteCount++;
    else sendDeoptCount++;  // Has sends but no heap writes → executable with deopt
    if (methodsCompiled_ % 50 == 0) {
        fprintf(stderr, "[JIT] Methods: %zu pure, %zu send-deopt, %zu heap-write "
                "(of %zu compiled, all executable)\n",
                pureCount, sendDeoptCount, heapWriteCount, methodsCompiled_);
    }

    // Log first few compiled methods with stencil detail
    {
        std::string sel = interp_.memory().selectorOf(compiledMethod);
        bool isKeysDo = (sel == "keysDo:");
        bool isDebugTarget = (sel == "noCheckAt:" || sel == "at:" || sel == "pvtCheckIndex:" || sel == "hasChanged" || sel == "hasPrimitive" || sel == "primitive");
        // Env-based selector dump: PHARO_JIT_DUMP_SEL=do:,max:,...
        static const char* dumpSelEnv = g_debug.jitDumpSel;
        bool isEnvDump = false;
        if (dumpSelEnv) {
            std::string excl(dumpSelEnv);
            size_t pos = 0;
            while (pos < excl.size()) {
                size_t comma = excl.find(',', pos);
                if (comma == std::string::npos) comma = excl.size();
                if (sel == excl.substr(pos, comma - pos)) { isEnvDump = true; break; }
                pos = comma + 1;
            }
        }
        if (methodsCompiled_ <= 20 || isKeysDo || isDebugTarget || isEnvDump) {
            if (isEnvDump) {
                fprintf(stderr, "[JIT-DUMP] methodOop=0x%llx numLits=%d bcLen=%zu\n",
                        (unsigned long long)compiledMethod.rawBits(),
                        numLiterals, bcLen);
                for (int li = 1; li <= numLiterals; li++) {
                    Oop lit = methObj->slotAt(li);
                    fprintf(stderr, "[JIT-DUMP]   lit[%d]=0x%llx cls=%s\n", li,
                            (unsigned long long)lit.rawBits(),
                            interp_.memory().classNameOf(lit).c_str());
                }
            }
            fprintf(stderr, "[JIT] Method #%zu compiled (%u bytes, %zu bytecodes) #%s:\n",
                    methodsCompiled_, totalSize, decoded.size(), sel.c_str());
            if (isDebugTarget) {
                fprintf(stderr, "  methodOop=0x%llx numLits=%d fmt=%u bcLen=%zu header=0x%llx\n",
                        (unsigned long long)compiledMethod.rawBits(),
                        numLiterals, (unsigned)fmt, bcLen,
                        (unsigned long long)headerBits);
                for (int li = 1; li <= numLiterals; li++) {
                    Oop lit = methObj->slotAt(li);
                    fprintf(stderr, "    lit[%d]=0x%llx", li,
                            (unsigned long long)lit.rawBits());
                    if (lit.isObject() && lit.rawBits() >= 0x10000) {
                        ObjectHeader* lh = lit.asObjectPtr();
                        fprintf(stderr, " class=%u", lh->classIndex());
                        if (lh->isBytesObject() && lh->byteSize() < 80) {
                            fprintf(stderr, " bytes=\"%.*s\"",
                                    (int)lh->byteSize(),
                                    (const char*)lh->bytes());
                        }
                    }
                    fprintf(stderr, "\n");
                }
                fprintf(stderr, "  raw bytecodes:");
                for (size_t b = 0; b < bcLen && b < 40; b++)
                    fprintf(stderr, " %02x", bytecodes[b]);
                fprintf(stderr, "\n");
            }
            for (auto& d : decoded) {
                const StencilDef& st = stencilTable[d.stencilIdx];
                fprintf(stderr, "  bc[%d] op=0x%02X -> %s (operand=%d, branch=%d)\n",
                        d.bcOffset, d.opcode, st.name, d.operand, d.branchTarget);
            }
            if (isEnvDump) {
                const uint8_t* cs = jitMethod->codeStart();
                fprintf(stderr, "[JIT-DUMP]   codeStart=%p codeSize=%u numBytecodes=%u "
                                "bcToCodeTableOffset=%u\n",
                        cs, jitMethod->codeSize, jitMethod->numBytecodes,
                        jitMethod->bcToCodeTableOffset);
                const uint32_t* t = jitMethod->bcToCodeTable();
                for (uint32_t b = 0; b <= jitMethod->numBytecodes; b++) {
                    fprintf(stderr, "[JIT-DUMP]   bc[%u] -> code+%u = %p\n",
                            b, t[b], (const void*)(cs + t[b]));
                }
            }
        }
    }

    return jitMethod;
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
