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

JITCompiler::JITCompiler(CodeZone& zone, MethodMap& methodMap,
                         ObjectMemory& memory, Interpreter& interp)
    : zone_(zone), methodMap_(methodMap), memory_(memory), interp_(interp)
{
    std::memset(&helpers_, 0, sizeof(helpers_));
}

// ===== BYTECODE DECODING =====

uint16_t JITCompiler::selectStencil(uint8_t opcode, int operand) const {
    // Map Sista V1 bytecodes to stencils
    if (opcode <= 0x0F) return static_cast<uint16_t>(StencilID::stencil_pushRecvVar);
    if (opcode <= 0x1F) return static_cast<uint16_t>(StencilID::stencil_pushLitVar);
    if (opcode <= 0x3F) return static_cast<uint16_t>(StencilID::stencil_pushLitConst);
    if (opcode <= 0x4B) return static_cast<uint16_t>(StencilID::stencil_pushTemp);

    switch (opcode) {
    case 0x4C: return static_cast<uint16_t>(StencilID::stencil_pushReceiver);
    case 0x4D: return static_cast<uint16_t>(StencilID::stencil_pushTrue);
    case 0x4E: return static_cast<uint16_t>(StencilID::stencil_pushFalse);
    case 0x4F: return static_cast<uint16_t>(StencilID::stencil_pushNil);
    case 0x50: return static_cast<uint16_t>(StencilID::stencil_pushZero);
    case 0x51: return static_cast<uint16_t>(StencilID::stencil_pushOne);
    case 0x53: return static_cast<uint16_t>(StencilID::stencil_dup);

    // Returns
    case 0x58: return static_cast<uint16_t>(StencilID::stencil_returnReceiver);
    case 0x59: return static_cast<uint16_t>(StencilID::stencil_returnTrue);
    case 0x5A: return static_cast<uint16_t>(StencilID::stencil_returnFalse);
    case 0x5B: return static_cast<uint16_t>(StencilID::stencil_returnNil);
    case 0x5C: return static_cast<uint16_t>(StencilID::stencil_returnTop);

    // Pop
    case 0xD8: return static_cast<uint16_t>(StencilID::stencil_pop);

    default: break;
    }

    // Arithmetic (0x60-0x6F)
    if (opcode >= 0x60 && opcode <= 0x6F) {
        int which = opcode & 0x0F;
        switch (which) {
        case 0:  return static_cast<uint16_t>(StencilID::stencil_addSmallInt);
        case 1:  return static_cast<uint16_t>(StencilID::stencil_subSmallInt);
        case 2:  return static_cast<uint16_t>(StencilID::stencil_lessThanSmallInt);
        case 3:  return static_cast<uint16_t>(StencilID::stencil_greaterThanSmallInt);
        case 6:  return static_cast<uint16_t>(StencilID::stencil_equalSmallInt);
        case 7:  return static_cast<uint16_t>(StencilID::stencil_notEqualSmallInt);
        case 8:  return static_cast<uint16_t>(StencilID::stencil_mulSmallInt);
        default: return static_cast<uint16_t>(StencilID::stencil_send);  // fallback to send
        }
    }

    // Sends (0x80-0xAF)
    if (opcode >= 0x80 && opcode <= 0xAF) {
        return static_cast<uint16_t>(StencilID::stencil_send);
    }

    // Short jumps (0xB0-0xB7)
    if (opcode >= 0xB0 && opcode <= 0xB7) {
        return static_cast<uint16_t>(StencilID::stencil_jump);
    }

    // Short jump true (0xB8-0xBF)
    if (opcode >= 0xB8 && opcode <= 0xBF) {
        return static_cast<uint16_t>(StencilID::stencil_jumpTrue);
    }

    // Short jump false (0xC0-0xC7)
    if (opcode >= 0xC0 && opcode <= 0xC7) {
        return static_cast<uint16_t>(StencilID::stencil_jumpFalse);
    }

    // Pop and store receiver var (0xC8-0xCF)
    if (opcode >= 0xC8 && opcode <= 0xCF) {
        return static_cast<uint16_t>(StencilID::stencil_popStoreRecvVar);
    }

    // Pop and store temp (0xD0-0xD7)
    if (opcode >= 0xD0 && opcode <= 0xD7) {
        return static_cast<uint16_t>(StencilID::stencil_popStoreTemp);
    }

    // Extended bytecodes (0xE0+) — bail out for now
    return static_cast<uint16_t>(StencilID::stencil_send);
}

bool JITCompiler::decodeBytecodes(const uint8_t* bytecodes, size_t length,
                                   std::vector<DecodedBC>& decoded) {
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
        bc.branchTarget = -1;
        bc.bcOffset = static_cast<int>(i);
        bc.bcLength = 1;

        uint8_t op = bytecodes[i];

        // Decode operand from bytecode encoding
        if (op <= 0x0F) {
            bc.operand = op & 0x0F;  // pushRecvVar index
        } else if (op <= 0x1F) {
            bc.operand = op & 0x0F;  // pushLitVar index
        } else if (op <= 0x3F) {
            bc.operand = op & 0x1F;  // pushLitConst index
        } else if (op <= 0x4B) {
            bc.operand = op - 0x40;  // pushTemp index
        } else if (op >= 0x60 && op <= 0x6F) {
            bc.operand = op & 0x0F;  // arithmetic selector index
        } else if (op >= 0x80 && op <= 0x8F) {
            bc.operand = op & 0x0F;  // send0: literal selector index
            bc.operand2 = 0;         // 0 args
        } else if (op >= 0x90 && op <= 0x9F) {
            bc.operand = op & 0x0F;
            bc.operand2 = 1;         // 1 arg
        } else if (op >= 0xA0 && op <= 0xAF) {
            bc.operand = op & 0x0F;
            bc.operand2 = 2;         // 2 args
        } else if (op >= 0xB0 && op <= 0xB7) {
            // Short unconditional jump: target = pc + (op & 7) + 1
            int delta = (op & 0x07) + 1;
            bc.branchTarget = static_cast<int>(i) + 1 + delta;
        } else if (op >= 0xB8 && op <= 0xBF) {
            // Short jump if true
            int delta = (op & 0x07) + 1;
            bc.branchTarget = static_cast<int>(i) + 1 + delta;
        } else if (op >= 0xC0 && op <= 0xC7) {
            // Short jump if false
            int delta = (op & 0x07) + 1;
            bc.branchTarget = static_cast<int>(i) + 1 + delta;
        } else if (op >= 0xC8 && op <= 0xCF) {
            bc.operand = op & 0x07;  // popStoreRecvVar index
        } else if (op >= 0xD0 && op <= 0xD7) {
            bc.operand = op - 0xD0;  // popStoreTemp index
        } else if (op == 0xD8) {
            // Pop (handled by selectStencil)
        } else if (op == 0xE0) {
            // extA prefix: accumulate into extension A value
            // E0 consumes 2 bytes total (opcode + extension byte)
            if (i + 1 >= length) return false;
            uint8_t extByte = bytecodes[i + 1];
            extA = (extA << 8) | extByte;
            bc.bcLength = 2;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            continue;  // Don't reset extA — it carries to the next bytecode
        } else if (op == 0xE1) {
            // extB prefix: signed extension
            if (i + 1 >= length) return false;
            uint8_t extByte = bytecodes[i + 1];
            if (extByte >= 128)
                extB = (extB << 8) | extByte | static_cast<int>(0xFFFFFF00u);
            else
                extB = (extB << 8) | extByte;
            bc.bcLength = 2;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            continue;
        } else if (op == 0xE2) {
            // Extended push receiver variable
            if (i + 1 >= length) return false;
            bc.operand = (extA << 8) | bytecodes[i + 1];
            bc.bcLength = 2;
        } else if (op == 0xE3) {
            // Extended push literal variable
            if (i + 1 >= length) return false;
            bc.operand = (extA << 8) | bytecodes[i + 1];
            bc.bcLength = 2;
        } else if (op == 0xE4) {
            // Extended push literal constant
            if (i + 1 >= length) return false;
            bc.operand = (extA << 8) | bytecodes[i + 1];
            bc.bcLength = 2;
        } else if (op == 0xE5) {
            // Extended push temp
            if (i + 1 >= length) return false;
            bc.operand = bytecodes[i + 1];
            bc.bcLength = 2;
        } else if (op == 0xE8) {
            // Push integer (signed, extB extends)
            if (i + 1 >= length) return false;
            bc.bcLength = 2;
            // Bail out: pushInteger requires a stencil that creates SmallIntegers
            // from arbitrary values, which we don't have yet. Fall back to send/nop.
            return false;
        } else if (op == 0xEA) {
            // Extended send: selector = (extA<<5 | byte>>3), args = (extB<<3 | byte&7)
            if (i + 1 >= length) return false;
            uint8_t desc = bytecodes[i + 1];
            bc.operand = ((extA << 5) | (desc >> 3)) & 0xFFFF;
            bc.operand2 = ((extB << 3) | (desc & 0x07)) & 0xFF;
            bc.bcLength = 2;
        } else if (op == 0xEB) {
            // Super send — bail, we don't have a super send stencil
            return false;
        } else if (op == 0xEF) {
            // Extended pop store
            if (i + 1 >= length) return false;
            uint8_t desc = bytecodes[i + 1];
            int kind = (desc >> 5) & 0x07;
            int index = ((extA << 5) | (desc & 0x1F));
            bc.operand = index;
            bc.bcLength = 2;
            if (kind == 0) {
                // Store into receiver variable
                bc.opcode = 0xC8;  // Pretend it's popStoreRecvVar
            } else if (kind == 1) {
                // Store into temp
                bc.opcode = 0xD0;  // Pretend it's popStoreTemp
            } else {
                return false;  // Other store kinds not supported
            }
        } else if (op == 0xF0 || op == 0xF1 || op == 0xF2) {
            // Extended jumps (3 bytes: opcode + 2 byte offset)
            if (i + 2 >= length) return false;
            uint8_t b1 = bytecodes[i + 1];
            uint8_t b2 = bytecodes[i + 2];
            int offset = (extB << 16) | (b1 << 8) | b2;
            bc.branchTarget = static_cast<int>(i) + 3 + offset;
            bc.bcLength = 3;
            if (op == 0xF0) bc.opcode = 0xB0;       // unconditional jump
            else if (op == 0xF1) bc.opcode = 0xB8;   // jump true
            else bc.opcode = 0xC0;                    // jump false
        } else if (op == 0xF5) {
            // Store temp (after primitive): just skip
            if (i + 1 >= length) return false;
            bc.bcLength = 2;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0;
            continue;
        } else if (op == 0xF8) {
            // callPrimitive (3 bytes) — skip, already handled by activateMethod
            if (i + 2 >= length) return false;
            bc.bcLength = 3;
            bc.stencilIdx = static_cast<uint16_t>(StencilID::stencil_nop);
            decoded.push_back(bc);
            i += bc.bcLength;
            extA = 0; extB = 0;
            continue;
        } else if (op >= 0xE0) {
            // Other unhandled extended bytecodes — bail
            return false;
        }

        bc.stencilIdx = selectStencil(bc.opcode, bc.operand);
        decoded.push_back(bc);
        i += bc.bcLength;
        extA = 0;
        extB = 0;
    }

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
            // GOT-style: adrp+ldr pair loads from a literal pool slot
            // We put the operand value in a literal pool entry and patch
            // the adrp+ldr to load from it.
            uint32_t slot = nextLiteralSlot++;
            literalPool[slot] = static_cast<uint64_t>(bc.operand >= 0 ? bc.operand : 0);

            uint64_t poolEntryAddr = reinterpret_cast<uint64_t>(
                reinterpret_cast<uint8_t*>(literalPool) + slot * 8);

            if (reloc.type == RelocType::ARM64_GOT_LOAD_PAGE21) {
                if (!patchARM64(stencilCode, reloc, poolEntryAddr)) return false;
            } else if (reloc.type == RelocType::ARM64_GOT_LOAD_PAGEOFF12) {
                if (!patchARM64(stencilCode, reloc, poolEntryAddr)) return false;
            }
            break;
        }

        case HoleKind::Operand2: {
            uint32_t slot = nextLiteralSlot++;
            literalPool[slot] = static_cast<uint64_t>(bc.operand2 >= 0 ? bc.operand2 : 0);
            uint64_t poolEntryAddr = reinterpret_cast<uint64_t>(
                reinterpret_cast<uint8_t*>(literalPool) + slot * 8);

            if (reloc.type == RelocType::ARM64_GOT_LOAD_PAGE21 ||
                reloc.type == RelocType::ARM64_GOT_LOAD_PAGEOFF12) {
                if (!patchARM64(stencilCode, reloc, poolEntryAddr)) return false;
            }
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
            } else if (reloc.type == RelocType::ARM64_GOT_LOAD_PAGE21 ||
                       reloc.type == RelocType::ARM64_GOT_LOAD_PAGEOFF12) {
                // GOT load: put the value (Oop) in a literal pool slot
                // and patch to load from it
                uint32_t slot = nextLiteralSlot++;
                // For Oop helpers (nil/true/false), helperAddr points to the Oop value
                uint64_t oopBits = *reinterpret_cast<uint64_t*>(helperAddr);
                literalPool[slot] = oopBits;
                uint64_t poolEntryAddr = reinterpret_cast<uint64_t>(
                    reinterpret_cast<uint8_t*>(literalPool) + slot * 8);
                if (!patchARM64(stencilCode, reloc, poolEntryAddr)) return false;
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
    if (!decodeBytecodes(bytecodes, bcLen, decoded)) {
        compilationsFailed_++;
        return nullptr;  // Contains unsupported extended bytecodes
    }

    if (decoded.empty()) {
        compilationsFailed_++;
        return nullptr;
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
    uint32_t totalSize = literalPoolOffset + literalPoolSize;

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
    jitMethod->tier = 1;

    // Extract arg/temp counts from header
    jitMethod->argCount = static_cast<uint8_t>((headerBits >> 24) & 0x0F);
    jitMethod->tempCount = static_cast<uint8_t>((headerBits >> 18) & 0x3F);

    uint8_t* codeBase = jitMethod->codeStart();
    uint64_t* literalPool = reinterpret_cast<uint64_t*>(codeBase + literalPoolOffset);
    uint32_t nextLiteralSlot = 0;

    // Make writable for code generation
    {
        ScopedWriteAccess guard(codeBase, totalSize);

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
                fprintf(stderr, "[JIT] Patch failed at bytecode offset %d\n", bc.bcOffset);
                compilationsFailed_++;
                // The allocation is wasted but the zone will reclaim it later
                return nullptr;
            }

            offset += stencil.codeSize;
        }
    }
    // ScopedWriteAccess flushes icache and marks executable

    // Mark as compiled
    jitMethod->state = MethodState::Compiled;
    zone_.finalize(jitMethod);

    // Register in method map
    methodMap_.insert(compiledMethod.rawBits(), jitMethod);

    methodsCompiled_++;
    return jitMethod;
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
