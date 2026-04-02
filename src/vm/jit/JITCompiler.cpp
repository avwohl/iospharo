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
                                   uint8_t& failedOpcode) {
    failedOpcode = 0;
    decoded.clear();
    decoded.reserve(length);  // Upper bound: one DecodedBC per byte

    int extA = 0;  // Extension A accumulator (Sista V1 prefix 0xE0)
    int extB = 0;  // Extension B accumulator (Sista V1 prefix 0xE1)

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
            // Unused in Sista V1, but may appear — deopt to interpreter
            bc.operand = bc.bcOffset;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0;
            continue;
        } else if (op >= SistaV1::ReturnReceiver && op <= SistaV1::ReturnTop) {
            // 0x58-0x5C: return bytecodes, no operand
        } else if (op >= 0x5D && op <= 0x5F) {
            // 0x5D/0x5E: block return, 0x5F: reserved — deopt to interpreter
            bc.operand = bc.bcOffset;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0;
            continue;
        } else if (op >= SistaV1::ArithBase && op <= 0x6F) {
            bc.operand = op & 0x0F;                            // arithmetic selector
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
        } else if (op >= 0xD9 && op <= 0xDF) {
            // Trap bytecodes — deopt to interpreter
            bc.operand = bc.bcOffset;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
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
                // Allocates array — deopt to interpreter
                if (i + 1 >= length) goto done;
                bc.operand = bc.bcOffset;
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
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
                // Super send — deopt to interpreter (handles super lookup)
                if (i + 1 >= length) goto done;
                bc.operand = bc.bcOffset;
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
                decoded.push_back(bc);
                i += bc.bcLength;
                extA = 0; extB = 0;
                continue;
            }
            case SistaV1::InlinedPrimitive: {
                // Sista inlined primitive — deopt to interpreter
                if (i + 1 >= length) goto done;
                bc.operand = bc.bcOffset;
                bc.bcLength = 2;
                bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_send);
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
                    ((bc.opcode >= 0x80 && bc.opcode <= 0xAF) ||
                     (bc.opcode >= 0x70 && bc.opcode <= 0x7F) ||
                     bc.opcode == SistaV1::ExtSend)) {
                    // Real send: upgrade to polymorphic IC stencil
                    int argCount = bc.operand2;
                    bc.branchTarget = bc.operand;  // Save literal/selector index for mega cache
                    bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_sendPoly);
                    bc.operand = (argCount << 16) | (bc.bcOffset & 0xFFFF);
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
        decoded.push_back(bc);
        i += bc.bcLength;
        extA = 0;
        extB = 0;
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
    uint64_t* literalPool, uint32_t literalPoolOffset,
    uint32_t& nextLiteralSlot)
{
    uint8_t* stencilCode = codeBase + stencilOffset;
    // Track the last allocated GOT slot so adrp+ldr pairs share the same slot.
    // PAGEOFF12 allocates; PAGE21 reuses.
    uint64_t lastGotSlotAddr = 0;

    for (uint16_t r = 0; r < stencil.numRelocs; r++) {
        const Relocation& reloc = stencil.relocs[r];
        uint32_t* insn = reinterpret_cast<uint32_t*>(stencilCode + reloc.offset);
        uint64_t pc = reinterpret_cast<uint64_t>(insn);

        switch (reloc.hole) {

        case HoleKind::Continue: {
            // Patch branch to the next stencil (stencilOffset + stencil.codeSize)
            uint32_t nextOffset = stencilOffset + stencil.codeSize;
            uint64_t target = reinterpret_cast<uint64_t>(codeBase + nextOffset);
            if (!patchARM64(stencilCode, reloc, target)) return false;
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
            if (target >= (int)bcToCodeOffset.size()) {
                target = (int)bcToCodeOffset.size() - 1;
            }
            uint32_t targetOff = bcToCodeOffset[target];
            uint64_t targetAddr = reinterpret_cast<uint64_t>(codeBase + targetOff);
            if (!patchARM64(stencilCode, reloc, targetAddr)) return false;
            break;
        }

        case HoleKind::Operand: {
            // GOT-style: adrp+ldr pair loads from a literal pool slot.
            // The PAGEOFF12 reloc comes first (higher offset in generated relocs),
            // followed by PAGE21. Both must target the SAME slot.
            // Allocate on PAGEOFF12; reuse on PAGE21.
            uint64_t poolEntryAddr;
            if (reloc.type == RelocType::ARM64_GOT_LOAD_PAGEOFF12) {
                uint32_t slot = nextLiteralSlot++;
                literalPool[slot] = static_cast<uint64_t>(bc.operand >= 0 ? bc.operand : 0);
                lastGotSlotAddr = reinterpret_cast<uint64_t>(
                    reinterpret_cast<uint8_t*>(literalPool) + slot * 8);
                poolEntryAddr = lastGotSlotAddr;
            } else {
                // PAGE21: reuse the slot from the preceding PAGEOFF12
                poolEntryAddr = lastGotSlotAddr;
            }
            if (!patchARM64(stencilCode, reloc, poolEntryAddr)) return false;
            break;
        }

        case HoleKind::Operand2: {
            uint64_t poolEntryAddr;
            if (reloc.type == RelocType::ARM64_GOT_LOAD_PAGEOFF12) {
                uint32_t slot = nextLiteralSlot++;
                // Use operand2Ptr (64-bit pointer) if set, otherwise fall back to operand2 (int)
                literalPool[slot] = bc.operand2Ptr != 0
                    ? bc.operand2Ptr
                    : static_cast<uint64_t>(bc.operand2 >= 0 ? bc.operand2 : 0);
                lastGotSlotAddr = reinterpret_cast<uint64_t>(
                    reinterpret_cast<uint8_t*>(literalPool) + slot * 8);
                poolEntryAddr = lastGotSlotAddr;
            } else {
                poolEntryAddr = lastGotSlotAddr;
            }
            if (!patchARM64(stencilCode, reloc, poolEntryAddr)) return false;
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
            default:
                fprintf(stderr, "[JIT] Unknown runtime helper ID %d\n", helperId);
                return false;
            }

            if (!helperAddr) {
                fprintf(stderr, "[JIT] Runtime helper %d not set\n", helperId);
                return false;
            }

            if (reloc.type == RelocType::ARM64_BRANCH26) {
                // Direct branch to helper function
                uint64_t target = reinterpret_cast<uint64_t>(helperAddr);
                if (!patchARM64(stencilCode, reloc, target)) return false;
            } else if (reloc.type == RelocType::ARM64_GOT_LOAD_PAGEOFF12) {
                // GOT load (PAGEOFF12 comes first): allocate literal pool slot.
                // Store the ADDRESS, not the value. The stencil does a double
                // dereference: load address from literal pool, then load value
                // through that address. This keeps values in sync after GC
                // (e.g., nilOopBits/trueOopBits/falseOopBits are updated by
                // updateSpecialOops and the stencil reads the current value).
                uint32_t slot = nextLiteralSlot++;
                literalPool[slot] = reinterpret_cast<uint64_t>(helperAddr);
                lastGotSlotAddr = reinterpret_cast<uint64_t>(
                    reinterpret_cast<uint8_t*>(literalPool) + slot * 8);
                if (!patchARM64(stencilCode, reloc, lastGotSlotAddr)) return false;
            } else if (reloc.type == RelocType::ARM64_GOT_LOAD_PAGE21) {
                // PAGE21: reuse the slot from the preceding PAGEOFF12
                if (!patchARM64(stencilCode, reloc, lastGotSlotAddr)) return false;
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

    // Decode bytecodes
    std::vector<DecodedBC> decoded;
    uint8_t failedOpcode = 0;
    if (!decodeBytecodes(bytecodes, bcLen, decoded, failedOpcode)) {
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

    // Peephole: fuse comparison + conditional jump into superinstructions.
    // This eliminates the boolean Oop creation/stack-roundtrip between them.
    for (size_t pi = 0; pi + 1 < decoded.size(); pi++) {
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

    // First pass: compute total code size and build bytecode->code offset map
    // We also need a literal pool for GOT-style patching
    uint32_t codeSize = 0;
    uint32_t maxLiteralSlots = 0;

    // Map from bytecode offset to machine code offset
    std::vector<uint32_t> bcToCodeOffset(bcLen + 1, 0);

    for (auto& bc : decoded) {
        bcToCodeOffset[bc.bcOffset] = codeSize;
        // Fill intermediate bytes of multi-byte instructions with the same offset
        for (int b = 1; b < bc.bcLength && (bc.bcOffset + b) <= (int)bcLen; b++) {
            bcToCodeOffset[bc.bcOffset + b] = codeSize;
        }
        const StencilDef& stencil = stencilTable[bc.stencilIdx];
        codeSize += stencil.codeSize;
        // Count literal pool slots needed (one per GOT reloc pair)
        for (uint16_t r = 0; r < stencil.numRelocs; r++) {
            if (stencil.relocs[r].type == RelocType::ARM64_GOT_LOAD_PAGE21) {
                maxLiteralSlots++;
            }
        }
    }
    // Sentinel: code offset for "one past the last bytecode"
    bcToCodeOffset[bcLen] = codeSize;

    // Literal pool lives after the code, 8-byte aligned
    uint32_t literalPoolOffset = (codeSize + 7) & ~7u;
    uint32_t literalPoolSize = maxLiteralSlots * 8;
    // bcToCode re-entry table lives after the literal pool, 4-byte aligned
    uint32_t bcToCodeTableOffset = (literalPoolOffset + literalPoolSize + 3) & ~3u;
    uint32_t bcToCodeTableSize = (static_cast<uint32_t>(bcLen) + 1) * sizeof(uint32_t);

    // Count send sites for inline cache data allocation
    uint16_t numSendSites = 0;
    for (auto& bc : decoded) {
        if (static_cast<StencilID>(bc.stencilIdx) == StencilID::stencil_sendPoly)
            numSendSites++;
    }

    // IC data lives after bcToCode table, 8-byte aligned. Each send site gets
    // 72 bytes: 4 entries x [uint64_t cachedClassIndex, uint64_t cachedMethodBits]
    // + 8 bytes for selectorBits (used by mega cache probe)
    static constexpr uint32_t IC_ENTRIES_PER_SITE = 4;
    static constexpr uint32_t IC_BYTES_PER_SITE = IC_ENTRIES_PER_SITE * 16 + 8;
    uint32_t icDataOffset = (bcToCodeTableOffset + bcToCodeTableSize + 7) & ~7u;
    uint32_t icDataSize = numSendSites * IC_BYTES_PER_SITE;
    uint32_t totalSize = icDataOffset + icDataSize;

    // The code zone is kept in writable W^X mode by default (set during
    // initialize). We write freely here; tryExecute() toggles to executable
    // only around the actual machine code call.

    // Allocate in code zone
    JITMethod* jitMethod = zone_.allocate(totalSize, 0 /* no IC entries yet */);
    if (!jitMethod) {
        // Try eviction
        zone_.evictLRU(totalSize + 1024);
        zone_.compact();
        jitMethod = zone_.allocate(totalSize, 0);
        if (!jitMethod) {
            compilationsFailed_++;
            return nullptr;
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

    // Set up IC data pointers for send sites. The IC data lives at the end
    // of the allocation. Each send site gets 72 bytes initialized to zero
    // (empty IC — will be patched on first miss). The last 8 bytes of each
    // block store selectorBits for the megamorphic cache probe.
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
            if (static_cast<StencilID>(bc.stencilIdx) == StencilID::stencil_sendPoly) {
                uint8_t* icBase = codeBase_pre + icDataOffset + sendIdx * IC_BYTES_PER_SITE;
                bc.operand2Ptr = reinterpret_cast<uint64_t>(icBase);

                // Store selectorBits at offset 64 (icData[8]) for mega cache probe
                uint64_t selectorBits = 0;
                if (bc.opcode >= 0x70 && bc.opcode <= 0x7F) {
                    // Special selector: from special objects array
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
                icSlots[8] = selectorBits;

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
    for (auto& d : decoded) {
        auto sid = static_cast<StencilID>(d.stencilIdx);
        switch (sid) {
        // Pure reads/stack ops — always safe
        case StencilID::stencil_pushRecvVar:
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
        case StencilID::stencil_popStoreLitVar:
        case StencilID::stencil_storeLitVar:
            jitMethod->hasHeapWrites = true;
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
        case StencilID::stencil_sendPoly:
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
                                  literalPool, literalPoolOffset,
                                  nextLiteralSlot)) {
            compilationsFailed_++;
            // The allocation is wasted but the zone will reclaim it later
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
    if (methodsCompiled_ <= 5) {
        fprintf(stderr, "[JIT] Method #%zu compiled (%u bytes, %zu bytecodes):\n",
                methodsCompiled_, totalSize, decoded.size());
        for (auto& d : decoded) {
            const StencilDef& st = stencilTable[d.stencilIdx];
            fprintf(stderr, "  bc[%d] op=0x%02X -> %s (operand=%d, branch=%d)\n",
                    d.bcOffset, d.opcode, st.name, d.operand, d.branchTarget);
        }
    }

    return jitMethod;
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
