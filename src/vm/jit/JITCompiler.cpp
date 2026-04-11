/*
 * JITCompiler.cpp - Copy-and-patch JIT compiler implementation
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 */

#include "JITCompiler.hpp"
#include "PlatformJIT.hpp"
#include "../ObjectMemory.hpp"
#include "../Interpreter.hpp"
#include <cstring>
#include <cstdio>

#if PHARO_JIT_ENABLED

namespace pharo {
namespace jit {

using namespace generated;

// Sista V1 bytecode opcodes (see docs/SistaV1-Bytecode-Spec.md)
namespace SistaV1 {
    // 1-byte ranges
    constexpr uint8_t PushRecvVarBase   = 0x00; // 0x00-0x0F
    constexpr uint8_t PushLitVarBase    = 0x10; // 0x10-0x1F
    constexpr uint8_t PushLitConstBase  = 0x20; // 0x20-0x3F
    constexpr uint8_t PushTempBase      = 0x40; // 0x40-0x4B
    constexpr uint8_t PushReceiver      = 0x4C;
    constexpr uint8_t PushTrue          = 0x4D;
    constexpr uint8_t PushFalse         = 0x4E;
    constexpr uint8_t PushNil           = 0x4F;
    constexpr uint8_t PushZero          = 0x50;
    constexpr uint8_t PushOne           = 0x51;
    constexpr uint8_t Dup               = 0x53;
    constexpr uint8_t ReturnReceiver    = 0x58;
    constexpr uint8_t ReturnTrue        = 0x59;
    constexpr uint8_t ReturnFalse       = 0x5A;
    constexpr uint8_t ReturnNil         = 0x5B;
    constexpr uint8_t ReturnTop         = 0x5C;
    constexpr uint8_t ArithBase         = 0x60; // 0x60-0x6F
    constexpr uint8_t Send0Base         = 0x80; // 0x80-0x8F
    constexpr uint8_t Send1Base         = 0x90; // 0x90-0x9F
    constexpr uint8_t Send2Base         = 0xA0; // 0xA0-0xAF
    constexpr uint8_t ShortJumpBase     = 0xB0; // 0xB0-0xB7
    constexpr uint8_t ShortJumpTrueBase = 0xB8; // 0xB8-0xBF
    constexpr uint8_t ShortJumpFalseBase= 0xC0; // 0xC0-0xC7
    constexpr uint8_t PopStoreRecvBase  = 0xC8; // 0xC8-0xCF
    constexpr uint8_t PopStoreTempBase  = 0xD0; // 0xD0-0xD7
    constexpr uint8_t Pop               = 0xD8;
    // 2-byte extended bytecodes
    constexpr uint8_t ExtendA           = 0xE0;
    constexpr uint8_t ExtendB           = 0xE1;
    constexpr uint8_t ExtPushRecvVar    = 0xE2;
    constexpr uint8_t ExtPushLitVar     = 0xE3;
    constexpr uint8_t ExtPushLitConst   = 0xE4;
    constexpr uint8_t ExtPushTemp       = 0xE5;
    constexpr uint8_t PushArray         = 0xE7;
    constexpr uint8_t PushInteger       = 0xE8;
    constexpr uint8_t PushCharacter     = 0xE9;
    constexpr uint8_t ExtSend           = 0xEA;
    constexpr uint8_t ExtSuperSend      = 0xEB;
    constexpr uint8_t InlinedPrimitive  = 0xEC;
    constexpr uint8_t ExtJump           = 0xED;
    constexpr uint8_t ExtJumpTrue       = 0xEE;
    constexpr uint8_t ExtJumpFalse      = 0xEF;
    constexpr uint8_t ExtPopStoreRecv   = 0xF0;
    constexpr uint8_t ExtPopStoreLitVar = 0xF1;
    constexpr uint8_t ExtPopStoreTemp   = 0xF2;
    constexpr uint8_t ExtStoreRecv      = 0xF3;
    constexpr uint8_t ExtStoreLitVar    = 0xF4;
    constexpr uint8_t ExtStoreTemp      = 0xF5;
    // 3-byte bytecodes
    constexpr uint8_t CallPrimitive     = 0xF8;
    constexpr uint8_t PushFullBlock     = 0xF9;
    constexpr uint8_t PushClosure       = 0xFA;
    constexpr uint8_t PushRemoteTemp    = 0xFB;
    constexpr uint8_t StoreRemoteTemp   = 0xFC;
    constexpr uint8_t PopStoreRemoteTemp= 0xFD;
}


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
        if (op <= 0x0F) {
            bc.operand = op & 0x0F;                            // pushRecvVar
        } else if (op <= 0x1F) {
            bc.operand = op & 0x0F;                            // pushLitVar
        } else if (op <= 0x3F) {
            bc.operand = op & 0x1F;                            // pushLitConst
        } else if (op <= 0x4B) {
            bc.operand = op - SistaV1::PushTempBase;           // pushTemp
        } else if (op >= SistaV1::PushReceiver && op <= SistaV1::Dup) {
            // 0x4C-0x53: individual push bytecodes, no operand
            if (op == 0x52) {
                // pushThisContext — deopt to interpreter
                bc.operand = bc.bcOffset;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0;
                continue;
            }
        } else if (op >= 0x54 && op <= 0x57) {
            // Unused in Sista V1 — interpreter treats as nop
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0;
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
                extA = 0; extB = 0;
                continue;
            }
            // In a method (not a block): simple return — fall through to selectStencil.
        } else if (op == 0x5D || op == 0x5E) {
            if (isFullBlock && extA == 0) {
                // In a FullBlock with no enclosing levels: these are simple returns.
                // 0x5D = return nil from block, 0x5E = return top from block.
                if (op == 0x5D) {
                    bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnNil);
                } else {
                    bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnTop);
                }
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0;
                continue;
            }
            // Non-FullBlock or non-local return (extA > 0): complex semantics. Deopt.
            bc.operand = bc.bcOffset;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0;
            continue;
        } else if (op == 0x5F) {
            // Nop (per Sista V1 spec)
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0;
            continue;
        } else if (op >= SistaV1::ArithBase && op <= 0x6F) {
            bc.operand = bc.bcOffset;                          // bytecode offset (for ArithOverflow ip)
            bc.operand2 = 1;                                   // all arith selectors are 1-arg
        } else if (op >= 0x70 && op <= 0x7F) {
            bc.operand = op - 0x70;                            // send special 16-31
            // nArgs per selector: at: at:put: size next nextPut: atEnd == class ~~ value value: do: new new: x y
            static const uint8_t specialNArgs[16] = {1,2,0,0,1,0,1,0,1,0,1,1,0,1,0,0};
            bc.operand2 = specialNArgs[bc.operand];
        } else if (op >= SistaV1::Send0Base && op <= 0x8F) {
            bc.operand = op & 0x0F;
            bc.operand2 = 0;                                   // send 0 args
        } else if (op >= SistaV1::Send1Base && op <= 0x9F) {
            bc.operand = op & 0x0F;
            bc.operand2 = 1;                                   // send 1 arg
        } else if (op >= SistaV1::Send2Base && op <= 0xAF) {
            bc.operand = op & 0x0F;
            bc.operand2 = 2;                                   // send 2 args
        } else if (op >= SistaV1::ShortJumpBase && op <= 0xB7) {
            bc.branchTarget = static_cast<int>(i) + 1 + (op & 0x07) + 1;
        } else if (op >= SistaV1::ShortJumpTrueBase && op <= 0xBF) {
            bc.branchTarget = static_cast<int>(i) + 1 + (op & 0x07) + 1;
        } else if (op >= SistaV1::ShortJumpFalseBase && op <= 0xC7) {
            bc.branchTarget = static_cast<int>(i) + 1 + (op & 0x07) + 1;
        } else if (op >= SistaV1::PopStoreRecvBase && op <= 0xCF) {
            bc.operand = op & 0x07;                            // popStoreRecvVar
        } else if (op >= SistaV1::PopStoreTempBase && op <= 0xD7) {
            bc.operand = op - SistaV1::PopStoreTempBase;       // popStoreTemp
        } else if (op == SistaV1::Pop) {
            // 0xD8: no operand
        } else if (op == 0xD9) {
            // Unconditional trap — deopt to interpreter (stopVM)
            bc.operand = bc.bcOffset;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0;
            continue;
        } else if (op >= 0xDA && op <= 0xDF) {
            // Reserved bytecodes — interpreter treats as nop
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0;
            continue;
        } else {
            // Extended bytecodes (0xE0+) — switch on exact opcode
            // If a multi-byte bytecode is truncated (runs past end of bytecodes),
            // it's dead code after a return — stop decoding rather than bail out.
            switch (op) {

            case SistaV1::ExtendA: {
                if (i + 1 >= length) goto done;
                extA = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
                decoded.push_back(bc);
                i += bc.bcLength;
                continue;  // Don't reset ext — carries to next bytecode
            }
            case SistaV1::ExtendB: {
                if (i + 1 >= length) goto done;
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
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
                continue;
            }
            case SistaV1::ExtStoreLitVar: {
                if (i + 1 >= length) goto done;
                bc.operand = (extA << 8) | bytecodes[i + 1];
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeLitVar);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0;
                continue;
            }
            case SistaV1::ExtStoreTemp: {
                if (i + 1 >= length) goto done;
                bc.operand = bytecodes[i + 1];
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeTemp);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0;
                continue;
            }
            case SistaV1::CallPrimitive: {
                // 3 bytes — skip, already handled by activateMethod
                if (i + 2 >= length) goto done;
                bc.bcLength = 3;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
                continue;
            }
            case SistaV1::PushClosure: {
                // Old-style closure — deopt to interpreter (3-byte)
                if (i + 2 >= length) goto done;
                bc.operand = bc.bcOffset;
                bc.bcLength = 3;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
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
                extA = 0; extB = 0;
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
                    bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendJ2J);
                    bc.operand = (bc.bcLength << 24) | (argCount << 16) | (bc.bcOffset & 0xFFFF);
                    // operand2Ptr will be set after code zone allocation
                } else {
                    // Bail-out or special send: keep stencil_send with bcOffset
                    bc.operand = bc.bcOffset;
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
            // Clamp to end-of-method if branch targets past the last bytecode
            int target = bc.branchTarget;
            if (target >= (int)bcToBranchOffset.size()) {
                target = (int)bcToBranchOffset.size() - 1;
            }
            // Both tables are last-write-wins and point to the real stencil
            // at each bcOffset. Jumps bypass any SimStack fallthrough-flush
            // inserted before the real stencil (see bcToBranchOffset init).
            uint32_t targetOff = bcToBranchOffset[target];
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

void JITCompiler::applySimStack(std::vector<DecodedBC>& decoded) {
    if (decoded.empty()) return;

    // Identify which bytecode offsets are branch targets (need Empty state)
    std::vector<bool> isBranchTarget(decoded.size(), false);
    for (size_t i = 0; i < decoded.size(); i++) {
        int target = decoded[i].branchTarget;
        if (target >= 0) {
            // Find the decoded instruction at this bytecode offset
            for (size_t j = 0; j < decoded.size(); j++) {
                if (decoded[j].bcOffset == target) {
                    isBranchTarget[j] = true;
                    break;
                }
            }
        }
    }

    // SimStack state: 0=Empty, 1=One, 2=Two
    int state = 0;  // Start Empty

    for (size_t i = 0; i < decoded.size(); i++) {
        // NOTE: We must NOT capture `auto& bc = decoded[i]` here because
        // insertions below invalidate references. Capture AFTER insertions.

        // Branch targets must enter as Empty
        if (isBranchTarget[i] && state != 0) {
            DecodedBC flush;
            flush.opcode = 0;
            flush.stencilIdx = (state == 2)
                ? static_cast<uint16_t>(StencilID::stencil_flush2)
                : static_cast<uint16_t>(StencilID::stencil_flush1);
            flush.operand = -1;
            flush.operand2 = -1;
            flush.operand2Ptr = 0;
            flush.branchTarget = -1;
            flush.bcOffset = decoded[i].bcOffset;
            flush.bcLength = 0;
            decoded.insert(decoded.begin() + i, flush);
            isBranchTarget.insert(isBranchTarget.begin() + i, false);
            i++;  // Skip past the flush we just inserted, process the original
            state = 0;
        }

        // Determine if this instruction is a "barrier" that requires Empty state
        auto sid = static_cast<StencilID>(decoded[i].stencilIdx);
        bool isBarrier = false;
        switch (sid) {
        case StencilID::stencil_send:
        case StencilID::stencil_sendPoly:
        case StencilID::stencil_sendJ2J:
        case StencilID::stencil_pushBlock:
        case StencilID::stencil_pushArray:
        case StencilID::stencil_pushRemoteTemp:
        case StencilID::stencil_storeRemoteTemp:
        case StencilID::stencil_popStoreRemoteTemp:
        case StencilID::stencil_popStoreLitVar:
        case StencilID::stencil_storeLitVar:
        // Superinstructions read operands from memory stack (sp[-1], sp[-2])
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
            // Return stencils are barriers too
            if (sid == StencilID::stencil_returnTrue ||
                sid == StencilID::stencil_returnFalse ||
                sid == StencilID::stencil_returnNil) {
                isBarrier = true;
            }
            break;
        }

        // If barrier and state != Empty, insert flush before this instruction
        if (isBarrier && state != 0) {
            DecodedBC flush;
            flush.opcode = 0;
            flush.stencilIdx = (state == 2)
                ? static_cast<uint16_t>(StencilID::stencil_flush2)
                : static_cast<uint16_t>(StencilID::stencil_flush1);
            flush.operand = -1;
            flush.operand2 = -1;
            flush.operand2Ptr = 0;
            flush.branchTarget = -1;
            flush.bcOffset = decoded[i].bcOffset;
            flush.bcLength = 0;
            decoded.insert(decoded.begin() + i, flush);
            isBranchTarget.insert(isBranchTarget.begin() + i, false);
            i++;  // Process the original after the flush
            state = 0;
        }

        // Capture reference AFTER all potential insertions above
        auto& bc = decoded[i];
        sid = static_cast<StencilID>(bc.stencilIdx);

        // Now select SimStack variant based on current state
        switch (sid) {
        // --- PUSH instructions: E→1, 1→2, 2→2(spill) ---
        case StencilID::stencil_pushTemp:
            if (state == 0)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushTemp_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushTemp_1); state = 2; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushTemp_2); }
            break;
        case StencilID::stencil_pushRecvVar:
            if (state == 0)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushRecvVar_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushRecvVar_1); state = 2; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushRecvVar_2); }
            break;
        case StencilID::stencil_pushLitConst:
            if (state == 0)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushLitConst_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushLitConst_1); state = 2; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushLitConst_2); }
            break;
        case StencilID::stencil_pushLitVar:
            if (state == 0)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushLitVar_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushLitVar_1); state = 2; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushLitVar_2); }
            break;
        case StencilID::stencil_pushReceiver:
            if (state == 0)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushReceiver_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushReceiver_1); state = 2; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushReceiver_2); }
            break;
        case StencilID::stencil_pushTrue:
            if (state == 0)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushTrue_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushTrue_1); state = 2; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushTrue_2); }
            break;
        case StencilID::stencil_pushFalse:
            if (state == 0)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushFalse_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushFalse_1); state = 2; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushFalse_2); }
            break;
        case StencilID::stencil_pushNil:
            if (state == 0)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushNil_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushNil_1); state = 2; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pushNil_2); }
            break;

        // --- POP: 2→1, 1→E, E→E(mem) ---
        case StencilID::stencil_pop:
            if (state == 2)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pop_2); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pop_1); state = 0; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_pop_E); }
            break;

        // --- DUP: E→1(from mem), 1→2, 2→2(spill) ---
        case StencilID::stencil_dup:
            if (state == 0)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_dup_E); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_dup_1); state = 2; }
            else                 { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_dup_2); }
            break;

        // --- STORE TEMP (no pop): keep state ---
        case StencilID::stencil_storeTemp:
            if (state == 1)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeTemp_1); }
            else if (state == 2) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeTemp_2); }
            // state 0: use base stencil (reads from memory)
            break;

        // --- POP+STORE TEMP: 2→1, 1→E ---
        case StencilID::stencil_popStoreTemp:
            if (state == 2)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreTemp_2); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreTemp_1); state = 0; }
            // state 0: use base stencil
            break;

        // --- STORE RECV VAR (no pop): keep state ---
        case StencilID::stencil_storeRecvVar:
            if (state >= 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_storeRecvVar_1); }
            break;

        // --- POP+STORE RECV VAR: 2→1, 1→E ---
        case StencilID::stencil_popStoreRecvVar:
            if (state == 2)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreRecvVar_2); state = 1; }
            else if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_popStoreRecvVar_1); state = 0; }
            break;

        // --- BINARY ARITHMETIC/COMPARISON: state 2 → _2 variant, state 1 → flush then base ---
        // These consume 2 operands. _2 variants read from simNOS/simTOS.
        // Base stencils read from sp[-1]/sp[-2], so state must be 0.
#define BINARY_SIMSTACK_CASE(baseName) \
        case StencilID::baseName: \
            if (state == 2) { bc.stencilIdx = static_cast<uint16_t>(StencilID::baseName##_2); state = 1; } \
            else if (state == 1) { \
                DecodedBC flush; flush.opcode = 0; \
                flush.stencilIdx = static_cast<uint16_t>(StencilID::stencil_flush1); \
                flush.operand = -1; flush.operand2 = -1; flush.operand2Ptr = 0; \
                flush.branchTarget = -1; flush.bcOffset = bc.bcOffset; flush.bcLength = 0; \
                decoded.insert(decoded.begin() + i, flush); \
                isBranchTarget.insert(isBranchTarget.begin() + i, false); \
                i++; state = 0; \
            } \
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

        // --- CONDITIONAL JUMPS: consume TOS (1→E), 2→flush then base ---
        case StencilID::stencil_jumpTrue:
            if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_jumpTrue_1); state = 0; }
            else if (state == 2) {
                // Flush both cached values, then use base stencil
                DecodedBC flush; flush.opcode = 0;
                flush.stencilIdx = static_cast<uint16_t>(StencilID::stencil_flush2);
                flush.operand = -1; flush.operand2 = -1; flush.operand2Ptr = 0;
                flush.branchTarget = -1; flush.bcOffset = bc.bcOffset; flush.bcLength = 0;
                decoded.insert(decoded.begin() + i, flush);
                isBranchTarget.insert(isBranchTarget.begin() + i, false);
                i++; state = 0;
                // base stencil reads from memory — correct now
            }
            break;
        case StencilID::stencil_jumpFalse:
            if (state == 1) { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_jumpFalse_1); state = 0; }
            else if (state == 2) {
                DecodedBC flush; flush.opcode = 0;
                flush.stencilIdx = static_cast<uint16_t>(StencilID::stencil_flush2);
                flush.operand = -1; flush.operand2 = -1; flush.operand2Ptr = 0;
                flush.branchTarget = -1; flush.bcOffset = bc.bcOffset; flush.bcLength = 0;
                decoded.insert(decoded.begin() + i, flush);
                isBranchTarget.insert(isBranchTarget.begin() + i, false);
                i++; state = 0;
            }
            break;

        // --- RETURNS ---
        case StencilID::stencil_returnTop:
            if (state == 2) {
                // Flush both, then use _E variant
                DecodedBC flush; flush.opcode = 0;
                flush.stencilIdx = static_cast<uint16_t>(StencilID::stencil_flush2);
                flush.operand = -1; flush.operand2 = -1; flush.operand2Ptr = 0;
                flush.branchTarget = -1; flush.bcOffset = decoded[i].bcOffset; flush.bcLength = 0;
                decoded.insert(decoded.begin() + i, flush);
                isBranchTarget.insert(isBranchTarget.begin() + i, false);
                i++; state = 0;
                decoded[i].stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnTop_E);
            } else if (state == 1) {
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnTop_1);
            } else {
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnTop_E);
            }
            state = 0;
            break;
        case StencilID::stencil_returnReceiver:
            if (state == 1)      { bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_returnReceiver_1); }
            // state==2: returnReceiver doesn't read stack, cached values are discarded with the frame
            state = 0;
            break;

        // --- UNCONDITIONAL JUMP: must flush cached values before jumping ---
        case StencilID::stencil_jump:
            // Branch targets enter in state 0 (memory-only stack), and jumps
            // land on the REAL stencil via last-write-wins bcToBranchOffset,
            // bypassing any flush inserted in front of the target. So if we
            // have values cached in x19/x20 at the jump site, we must flush
            // them to memory BEFORE jumping — otherwise those logical stack
            // slots are lost and the target reads the wrong values from
            // memory.
            if (state != 0) {
                DecodedBC flush;
                flush.opcode = 0;
                flush.stencilIdx = (state == 2)
                    ? static_cast<uint16_t>(StencilID::stencil_flush2)
                    : static_cast<uint16_t>(StencilID::stencil_flush1);
                flush.operand = -1;
                flush.operand2 = -1;
                flush.operand2Ptr = 0;
                flush.branchTarget = -1;
                flush.bcOffset = decoded[i].bcOffset;
                flush.bcLength = 0;
                decoded.insert(decoded.begin() + i, flush);
                isBranchTarget.insert(isBranchTarget.begin() + i, false);
                i++;  // Skip past the flush we just inserted
                state = 0;
            }
            break;

        // --- SUPERINSTRUCTIONS (comparison+jump fused): handled as barriers above ---
        // They read from memory stack (sp[-1], sp[-2]), so flush is inserted
        // by the barrier check before we reach here.
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

        // --- NOP, pushZero, pushOne, pushInteger: not optimized ---
        case StencilID::stencil_pushZero:
        case StencilID::stencil_pushOne:
        case StencilID::stencil_pushInteger:
            // These aren't common enough to warrant variants.
            // Flush if needed, then treat as pushing to memory.
            if (state != 0) {
                DecodedBC flush;
                flush.opcode = 0;
                flush.stencilIdx = (state == 2)
                    ? static_cast<uint16_t>(StencilID::stencil_flush2)
                    : static_cast<uint16_t>(StencilID::stencil_flush1);
                flush.operand = -1;
                flush.operand2 = -1;
                flush.operand2Ptr = 0;
                flush.branchTarget = -1;
                flush.bcOffset = bc.bcOffset;
                flush.bcLength = 0;
                decoded.insert(decoded.begin() + i, flush);
                isBranchTarget.insert(isBranchTarget.begin() + i, false);
                i++;
                state = 0;
            }
            break;

        // --- Everything else: flush to Empty, use base stencil ---
        default:
            if (state != 0) {
                // Unknown instruction — conservatively flush
                DecodedBC flush;
                flush.opcode = 0;
                flush.stencilIdx = (state == 2)
                    ? static_cast<uint16_t>(StencilID::stencil_flush2)
                    : static_cast<uint16_t>(StencilID::stencil_flush1);
                flush.operand = -1;
                flush.operand2 = -1;
                flush.operand2Ptr = 0;
                flush.branchTarget = -1;
                flush.bcOffset = bc.bcOffset;
                flush.bcLength = 0;
                decoded.insert(decoded.begin() + i, flush);
                isBranchTarget.insert(isBranchTarget.begin() + i, false);
                i++;
                state = 0;
            }
            break;
        }
    }
}

#endif // __aarch64__

// ===== MAIN COMPILATION =====

JITMethod* JITCompiler::compile(Oop compiledMethod) {
    // Check if already compiled
    if (methodMap_.lookup(compiledMethod.rawBits())) {
        return methodMap_.lookup(compiledMethod.rawBits());
    }

    // Get bytecode range from CompiledMethod
    ObjectHeader* methObj = compiledMethod.asObjectPtr();
    Oop headerOop = methObj->slotAt(0);
    if (!headerOop.isSmallInteger()) {
        compilationsFailed_++;
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
        return nullptr;
    }

    const uint8_t* bytecodes = bytes + bcStart;

    // Check if this is a CompiledBlock (FullBlock) — block returns are simple returns
    uint32_t methodClassIndex = methObj->classIndex();
    bool isFullBlock = (methodClassIndex == interp_.compiledBlockClassIndex());

    // Bisection: PHARO_JIT_NO_BLOCKS=1 skips JIT compilation for CompiledBlocks.
    // Used to check whether the JIT bug is in block compilation specifically.
    if (isFullBlock) {
        static bool noBlocks = getenv("PHARO_JIT_NO_BLOCKS") != nullptr;
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

    // Bytecode dump for bisection (JIT_DUMP_BC env var)
    {
        static bool dumpBC = !!getenv("JIT_DUMP_BC");
        static const char* dumpBCPre = getenv("JIT_DUMP_BC_PRE");
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
        static const char* skipEnv = getenv("PHARO_JIT_SKIP_SELECTORS");
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
                // CRITICAL: Don't JIT-compile methods with unsupported primitives.
                // The CallPrimitive bytecode becomes NOP in JIT code, so the fallback
                // bytecodes run unconditionally. Via J2J (direct call), activateMethod
                // is bypassed and the primitive is never tried. For methods like
                // Object>>at: whose fallback is errorSubscriptBounds:, this causes
                // false errors. Keep these methods interpreter-only so the primitive
                // is always tried first.
                compilationsFailed_++;
                return nullptr;
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

    // SimStack: register-based TOS/NOS caching in x19/x20.
    // Stencils use inline asm to read/write x19/x20 without clobber lists.
    // The compiler doesn't touch callee-saved regs in these tail-call
    // functions. extract_stencils.py verifies this statically.
#ifdef __aarch64__
    {
        static bool noSimStack = getenv("PHARO_JIT_NO_SIMSTACK") != nullptr;
        // Per-selector SimStack disable for bisection:
        // PHARO_JIT_NO_SIMSTACK_SELECTORS=sel1,sel2,...
        bool skipSimStackHere = noSimStack;
        if (!skipSimStackHere) {
            static const char* skipSimStackEnv = getenv("PHARO_JIT_NO_SIMSTACK_SELECTORS");
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
        if (!skipSimStackHere) applySimStack(decoded);

        // Post-SimStack dump (JIT_DUMP_BC_POST=selectorName)
        {
            static const char* dumpSel = getenv("JIT_DUMP_BC_POST");
            if (dumpSel && *dumpSel) {
                std::string sel = interp_.memory().selectorOf(compiledMethod);
                if (sel == dumpSel) {
                    fprintf(stderr, "[JIT-BC-POST] #%s post-SimStack stencils:\n", sel.c_str());
                    for (size_t d = 0; d < decoded.size(); d++) {
                        fprintf(stderr, "[JIT-BC-POST]   [%zu] stencil=%u operand=%d bc=%d br=%d\n",
                                d, decoded[d].stencilIdx, decoded[d].operand,
                                decoded[d].bcOffset, decoded[d].branchTarget);
                    }
                }
            }
        }
    }
#endif

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

    for (auto& bc : decoded) {
        // Fused bytecodes (nop placeholders from peephole fusion) must NOT have
        // valid re-entry points. Their bcToCode entries stay 0, so tryResume
        // rejects them and the interpreter handles the bytecode. Without this,
        // resuming at a fused jumpFalse enters the fused stencil's CONTINUE
        // path unconditionally, ignoring the actual comparison result.
        if (static_cast<StencilID>(bc.stencilIdx) != StencilID::stencil_nop) {
            // Last-write-wins: the real stencil at bcOffset X overwrites any
            // SimStack flush inserted before it, so jumps and tryResume both
            // land on the real stencil (Empty SimStack state at entry).
            bcToCodeOffset[bc.bcOffset] = codeSize;
            bcToBranchOffset[bc.bcOffset] = codeSize;
            for (int b = 1; b < bc.bcLength && (bc.bcOffset + b) <= (int)bcLen; b++) {
                bcToCodeOffset[bc.bcOffset + b] = codeSize;
                bcToBranchOffset[bc.bcOffset + b] = codeSize;
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

    // Literal pool lives after the code, 8-byte aligned
    uint32_t literalPoolOffset = (codeSize + 7) & ~7u;
    uint32_t literalPoolSize = maxLiteralSlots * 8;
    // bcToCode re-entry table lives after the literal pool, 4-byte aligned
    uint32_t bcToCodeTableOffset = (literalPoolOffset + literalPoolSize + 3) & ~3u;
    uint32_t bcToCodeTableSize = (static_cast<uint32_t>(bcLen) + 1) * sizeof(uint32_t);

    // Count send sites for inline cache data allocation
    uint16_t numSendSites = 0;
    for (auto& bc : decoded) {
        if (static_cast<StencilID>(bc.stencilIdx) == StencilID::stencil_sendJ2J)
            numSendSites++;
    }

    // IC data lives after bcToCode table, 8-byte aligned. Each send site gets
    // 104 bytes: 4 entries x [uint64_t classKey, uint64_t methodBits, uint64_t extra]
    // + 8 bytes for selectorBits (used by mega cache probe).
    // The 'extra' field holds J2J info: high byte = kind (0x80=getter, 0x40=setter),
    // low 32 bits = slot index. When kind != 0, the send stencil inlines the
    // field access directly, bypassing the C++ boundary crossing entirely.
    static constexpr uint32_t IC_ENTRIES_PER_SITE = 4;
    static constexpr uint32_t IC_BYTES_PER_SITE = IC_ENTRIES_PER_SITE * 24 + 8;
    uint32_t icDataOffset = (bcToCodeTableOffset + bcToCodeTableSize + 7) & ~7u;
    uint32_t icDataSize = numSendSites * IC_BYTES_PER_SITE;
    uint32_t totalSize = icDataOffset + icDataSize;

    // The code zone is kept in writable W^X mode by default (set during
    // initialize). We write freely here; tryExecute() toggles to executable
    // only around the actual machine code call.

    // Compute the actual allocation size (allocate() adds JITMethod header + alignment)
    size_t allocSize = sizeof(JITMethod) + totalSize;
    allocSize = (allocSize + MethodAlignment - 1) & ~(MethodAlignment - 1);

    // Allocate in code zone (tries bump pointer, then free list)
    JITMethod* jitMethod = zone_.allocate(totalSize, 0 /* no IC entries yet */);
    if (!jitMethod) {
        // Incremental eviction: free cold methods into the free list.
        // allocate() will reuse freed space without moving methods
        // (ADRP+LDR relocations in stencils are not position-independent
        // across non-page-aligned moves).

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
                if (im->numICEntries > 0) {
                    uint8_t* icStart = im->codeStart() + im->codeSize
                                     - im->numICEntries * 104;
                    for (uint32_t i = 0; i < im->numICEntries; i++) {
                        uint64_t* slots = reinterpret_cast<uint64_t*>(icStart + i * 104);
                        for (int e = 0; e < 4; e++) {
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
            jitMethod = zone_.allocate(totalSize, 0);
        }

        // If incremental eviction wasn't enough, full flush as last resort
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
                zone_.freeMethod(m);
                m = next;
            }
            methodMap_.clear();
            zone_.compact();

            jitMethod = zone_.allocate(totalSize, 0);
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
    jitMethod->tier = 1;

    // Extract arg/temp counts from header
    jitMethod->argCount = static_cast<uint8_t>((headerBits >> 24) & 0x0F);
    jitMethod->tempCount = static_cast<uint8_t>((headerBits >> 18) & 0x3F);
    jitMethod->hasPrimPrologue = hasPrimPrologue;

    // Set up IC data pointers for send sites. The IC data lives at the end
    // of the allocation. Each send site gets 104 bytes initialized to zero
    // (empty IC — will be patched on first miss): 4 x [key, method, extra]
    // + selectorBits. The extra word encodes inline getter/setter info for
    // J2J dispatch (bit 63=getter, bit 62=setter, bit 61=returnsSelf).
    uint8_t* codeBase_pre = jitMethod->codeStart();
    {
        // Zero IC data area first
        std::memset(codeBase_pre + icDataOffset, 0, icDataSize);

        // Get special selectors array for 0x70-0x7F sends
        Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
        ObjectHeader* ssArray = (specialSelectors.isObject() && specialSelectors.rawBits() > 0x10000)
            ? specialSelectors.asObjectPtr() : nullptr;

        uint16_t sendIdx = 0;
        for (auto& bc : decoded) {
            if (static_cast<StencilID>(bc.stencilIdx) == StencilID::stencil_sendJ2J) {
                uint8_t* icBase = codeBase_pre + icDataOffset + sendIdx * IC_BYTES_PER_SITE;
                bc.operand2Ptr = reinterpret_cast<uint64_t>(icBase);

                // Store selectorBits at offset 64 (icData[8]) for mega cache probe
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
                icSlots[12] = selectorBits;

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

    // Flush icache for the newly written code
    flushICache(codeBase, totalSize);

    // Mark as compiled
    jitMethod->state = MethodState::Compiled;

    // Register in method map
    methodMap_.insert(compiledMethod.rawBits(), jitMethod);

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
        if (methodsCompiled_ <= 5 || isKeysDo || isDebugTarget) {
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
        }
    }

    return jitMethod;
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
