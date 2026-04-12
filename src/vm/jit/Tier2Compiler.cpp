/*
 * Tier2Compiler.cpp - MIR-based optimizing JIT compiler
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Translates Sista V1 bytecodes into MIR IR, then uses MIR to generate
 * native ARM64 code with register allocation.
 *
 * STRATEGY:
 *
 * The Smalltalk expression stack is mapped to MIR virtual registers
 * (the "virtual stack" or vstack). Instead of load/store to JITState->sp
 * on every push/pop, values live in registers. The vstack is flushed to
 * memory only at sends, returns, and backward branch targets.
 *
 * Temps are mapped to MIR registers, loaded lazily from tempBase on first
 * access. Stores write both the register and memory (for GC visibility).
 *
 * SmallInteger arithmetic uses MIR's overflow-checked ops (ADDO/SUBO/MULO)
 * with BO (branch on overflow) to the slow path.
 */

#include "Tier2Compiler.hpp"
#include "CodeZone.hpp"
#include "JITMethod.hpp"
#include "../ObjectMemory.hpp"
#include "../Interpreter.hpp"
#include <cstring>
#include <cstdio>
#include <vector>

#include <setjmp.h>

#if PHARO_JIT_ENABLED

// MIR error recovery: longjmp back to compile() on MIR errors
static thread_local jmp_buf mir_error_jmp;
static thread_local bool mir_error_active = false;

[[noreturn]] static void mir_error_handler(MIR_error_type_t error_type, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    fprintf(stderr, "[T2] MIR error (type %d): ", error_type);
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    if (mir_error_active) {
        longjmp(mir_error_jmp, 1);
    }
    // If no jmp_buf is active, abort (shouldn't happen)
    abort();
}

namespace pharo {
namespace jit {

// Sista V1 bytecode constants (duplicated from JITCompiler.cpp to avoid header dependency)
namespace SistaV1 {
    constexpr uint8_t PushRecvVarBase   = 0x00;
    constexpr uint8_t PushLitVarBase    = 0x10;
    constexpr uint8_t PushLitConstBase  = 0x20;
    constexpr uint8_t PushTempBase      = 0x40;
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
    constexpr uint8_t ArithBase         = 0x60;
    constexpr uint8_t Send0Base         = 0x80;
    constexpr uint8_t Send1Base         = 0x90;
    constexpr uint8_t Send2Base         = 0xA0;
    constexpr uint8_t ShortJumpBase     = 0xB0;
    constexpr uint8_t ShortJumpTrueBase = 0xB8;
    constexpr uint8_t ShortJumpFalseBase= 0xC0;
    constexpr uint8_t PopStoreRecvBase  = 0xC8;
    constexpr uint8_t PopStoreTempBase  = 0xD0;
    constexpr uint8_t Pop               = 0xD8;
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
    constexpr uint8_t CallPrimitive     = 0xF8;
    constexpr uint8_t PushFullBlock     = 0xF9;
    constexpr uint8_t PushClosure       = 0xFA;
}

// JITState field offsets (must match JITState.hpp)
static constexpr int OFF_SP        = 0;
static constexpr int OFF_RECEIVER  = 8;
static constexpr int OFF_LITERALS  = 16;
static constexpr int OFF_TEMPBASE  = 24;
static constexpr int OFF_MEMORY    = 32;
static constexpr int OFF_INTERP    = 40;
static constexpr int OFF_IP        = 48;
static constexpr int OFF_JITMETHOD = 56;
static constexpr int OFF_METHOD    = 64;
static constexpr int OFF_ARGCOUNT  = 72;
static constexpr int OFF_EXIT      = 76;
static constexpr int OFF_RETVAL    = 80;
static constexpr int OFF_CACHED    = 88;
static constexpr int OFF_ICDATA    = 96;
static constexpr int OFF_SENDNARGS = 104;
static constexpr int OFF_TRUE      = 128;
static constexpr int OFF_FALSE     = 136;

// Exit reasons (must match JITState.hpp)
static constexpr int EXIT_RETURN       = 1;
static constexpr int EXIT_SEND         = 2;

// SmallInteger tag
static constexpr uint64_t SMALLINT_TAG = 1;
static constexpr uint64_t TAG_MASK     = 7;

// ===== DECODED BYTECODE (simplified for Tier 2) =====

struct T2BC {
    uint8_t opcode;
    int     operand;
    int     operand2;
    int     branchTarget;  // -1 if not a jump
    int     bcOffset;
    int     bcLength;
};

// ===== Tier2Compiler =====

Tier2Compiler::Tier2Compiler(CodeZone& zone, MethodMap& methodMap,
                             ObjectMemory& memory, Interpreter& interp)
    : zone_(zone), methodMap_(methodMap), memory_(memory), interp_(interp)
{
    std::memset(vstack_, 0, sizeof(vstack_));
    std::memset(tempRegs_, 0, sizeof(tempRegs_));
    std::memset(tempLoaded_, 0, sizeof(tempLoaded_));
    std::memset(bcLabels_, 0, sizeof(bcLabels_));
    std::memset(bcLabelUsed_, 0, sizeof(bcLabelUsed_));
}

Tier2Compiler::~Tier2Compiler() {
    for (auto ctx : liveContexts_) {
        MIR_gen_finish(ctx);
        MIR_finish(ctx);
    }
    liveContexts_.clear();
}

bool Tier2Compiler::initialize() {
    // MIR context is created per-compilation now (see compile())
    return true;
}

MIR_reg_t Tier2Compiler::newScratch() {
    char name[32];
    snprintf(name, sizeof(name), "_t%d", scratchCounter_++);
    return MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, name);
}

// Helper: emit MIR instruction (convenience wrappers)
#define EMIT(code, ...) \
    MIR_append_insn(mirCtx_, mirFunc_, MIR_new_insn(mirCtx_, code, __VA_ARGS__))

#define REG(r) MIR_new_reg_op(mirCtx_, r)
#define IMM(v) MIR_new_int_op(mirCtx_, (int64_t)(v))
#define MEM(type, base, disp) MIR_new_mem_op(mirCtx_, type, disp, base, 0, 0)
#define LABEL_OP(l) MIR_new_label_op(mirCtx_, l)

void Tier2Compiler::emitPrologue(int tempCount, int argCount) {
    // Load JITState fields into registers
    // sp = state->sp
    EMIT(MIR_MOV, REG(reg_sp_), MEM(MIR_T_I64, reg_statePtr_, OFF_SP));
    // receiver = state->receiver
    EMIT(MIR_MOV, REG(reg_receiver_), MEM(MIR_T_I64, reg_statePtr_, OFF_RECEIVER));
    // tempBase = state->tempBase
    EMIT(MIR_MOV, REG(reg_tempBase_), MEM(MIR_T_I64, reg_statePtr_, OFF_TEMPBASE));
    // literals = state->literals
    EMIT(MIR_MOV, REG(reg_literals_), MEM(MIR_T_I64, reg_statePtr_, OFF_LITERALS));
    // trueOop = state->trueOop
    EMIT(MIR_MOV, REG(reg_trueOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_TRUE));
    // falseOop = state->falseOop
    EMIT(MIR_MOV, REG(reg_falseOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_FALSE));

    // Create temp registers (loaded lazily)
    tempCount_ = tempCount;
    for (int i = 0; i < tempCount && i < MaxTemps; i++) {
        char name[32];
        snprintf(name, sizeof(name), "tmp%d", i);
        tempRegs_[i] = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, name);
        tempLoaded_[i] = false;
    }
}

void Tier2Compiler::flushVStack() {
    // Write all vstack values to JITState->sp in memory.
    // The Smalltalk stack grows upward: sp[0] = TOS, sp[-1] = NOS, etc.
    // After flush: sp points past the top element.
    //
    // We need to adjust sp based on how many vstack values there are vs
    // the original sp position. The vstack represents values that would
    // have been pushed on sp.
    //
    // Strategy: write vstack[0] to *(sp), vstack[1] to *(sp+8), etc.
    // Then advance sp by vstackDepth_.
    for (int i = 0; i < vstackDepth_; i++) {
        // *(sp + i*8) = vstack[i]
        // sp is a pointer, so offset is i * sizeof(Oop) = i * 8
        MIR_reg_t addr = newScratch();
        EMIT(MIR_ADD, REG(addr), REG(reg_sp_), IMM(i * 8));
        EMIT(MIR_MOV, MEM(MIR_T_I64, addr, 0), REG(vstack_[i]));
    }
    if (vstackDepth_ > 0) {
        EMIT(MIR_ADD, REG(reg_sp_), REG(reg_sp_), IMM(vstackDepth_ * 8));
    }
    vstackDepth_ = 0;
}

void Tier2Compiler::emitReturn() {
    // return TOS: set state->returnValue = TOS, state->exitReason = EXIT_RETURN
    MIR_reg_t retVal;
    if (vstackDepth_ > 0) {
        retVal = vpop();
    } else {
        // TOS is in memory at sp[-1] (sp points past TOS in our convention)
        // Actually in stencils sp[0] = TOS (sp points at TOS)
        // Load from sp[0]
        retVal = newScratch();
        EMIT(MIR_MOV, REG(retVal), MEM(MIR_T_I64, reg_sp_, 0));
    }
    // state->returnValue = retVal
    EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_RETVAL), REG(retVal));
    // state->exitReason = EXIT_RETURN
    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_RETURN));
    // Return from the generated function
    MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
}

void Tier2Compiler::emitSendExit(int nArgs, int bcOffset, bool cached, MIR_reg_t cachedMethod) {
    // Flush vstack to memory so interpreter can see the stack
    flushVStack();

    // state->sp = sp (updated after flush)
    EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));

    // state->sendArgCount = nArgs
    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_SENDNARGS), IMM(nArgs));

    // state->ip = bytecode address for this send
    // We store the bytecode offset; the interpreter converts to actual IP
    // Actually: state->ip is the raw bytecode pointer. We need to compute it.
    // For now, store bcOffset in ip field — the caller (JITRuntime) will
    // need to reconstruct. Actually let's load state->ip first, then add offset.
    // Hmm, ip points into the bytecodes of the CompiledMethod.
    // We'll store the bytecodes base + bcOffset.
    // Load ip base from the method:
    MIR_reg_t ipReg = newScratch();
    EMIT(MIR_MOV, REG(ipReg), MEM(MIR_T_I64, reg_statePtr_, OFF_IP));
    EMIT(MIR_ADD, REG(ipReg), REG(ipReg), IMM(bcOffset));
    EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(ipReg));

    if (cached) {
        EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_CACHED), REG(cachedMethod));
        EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(7)); // EXIT_SEND_CACHED
    } else {
        EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_SEND));
    }

    MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
}

// ===== BYTECODE DECODER (simplified for Tier 2) =====

static bool decodeBytecodes(const uint8_t* bc, size_t len, std::vector<T2BC>& out) {
    out.clear();
    out.reserve(len);

    int extA = 0, extB = 0;
    size_t i = 0;
    while (i < len) {
        T2BC d;
        d.opcode = bc[i];
        d.operand = -1;
        d.operand2 = -1;
        d.branchTarget = -1;
        d.bcOffset = (int)i;
        d.bcLength = 1;

        uint8_t op = bc[i];

        if (op <= 0x0F) {
            d.operand = op & 0x0F;  // pushRecvVar
        } else if (op <= 0x1F) {
            d.operand = op & 0x0F;  // pushLitVar
        } else if (op <= 0x3F) {
            d.operand = op & 0x1F;  // pushLitConst
        } else if (op <= 0x4B) {
            d.operand = op - SistaV1::PushTempBase;  // pushTemp
        } else if (op >= 0x54 && op <= 0x57) {
            // Unused — nop
            out.push_back(d);
            i += d.bcLength;
            extA = extB = 0;
            continue;
        } else if (op >= SistaV1::ArithBase && op <= 0x6F) {
            d.operand = d.bcOffset;
            d.operand2 = 1;  // all arith are 1-arg sends
        } else if (op >= 0x70 && op <= 0x7F) {
            d.operand = op - 0x70;
            static const uint8_t specialNArgs[16] = {1,2,0,0,1,0,1,0,1,0,1,1,0,1,0,0};
            d.operand2 = specialNArgs[d.operand];
        } else if (op >= SistaV1::Send0Base && op <= 0x8F) {
            d.operand = op & 0x0F;
            d.operand2 = 0;
        } else if (op >= SistaV1::Send1Base && op <= 0x9F) {
            d.operand = op & 0x0F;
            d.operand2 = 1;
        } else if (op >= SistaV1::Send2Base && op <= 0xAF) {
            d.operand = op & 0x0F;
            d.operand2 = 2;
        } else if (op >= SistaV1::ShortJumpBase && op <= 0xB7) {
            d.branchTarget = (int)i + 1 + (op & 0x07) + 1;
        } else if (op >= SistaV1::ShortJumpTrueBase && op <= 0xBF) {
            d.branchTarget = (int)i + 1 + (op & 0x07) + 1;
        } else if (op >= SistaV1::ShortJumpFalseBase && op <= 0xC7) {
            d.branchTarget = (int)i + 1 + (op & 0x07) + 1;
        } else if (op >= SistaV1::PopStoreRecvBase && op <= 0xCF) {
            d.operand = op & 0x07;
        } else if (op >= SistaV1::PopStoreTempBase && op <= 0xD7) {
            d.operand = op - SistaV1::PopStoreTempBase;
        } else if (op == SistaV1::Pop) {
            // no operand
        } else if (op == SistaV1::ExtendA) {
            if (i + 1 >= len) break;
            extA = (extA << 8) | bc[i + 1];
            d.bcLength = 2;
            out.push_back(d);
            i += d.bcLength;
            continue;
        } else if (op == SistaV1::ExtendB) {
            if (i + 1 >= len) break;
            uint8_t eb = bc[i + 1];
            extB = (eb >= 128) ? (extB << 8) | eb | (int)0xFFFFFF00u : (extB << 8) | eb;
            d.bcLength = 2;
            out.push_back(d);
            i += d.bcLength;
            continue;
        } else if (op == SistaV1::ExtPushRecvVar) {
            if (i + 1 >= len) break;
            d.operand = (extA << 8) | bc[i + 1];
            d.bcLength = 2;
            d.opcode = 0x00;  // Normalize to pushRecvVar
        } else if (op == SistaV1::ExtPushLitVar) {
            if (i + 1 >= len) break;
            d.operand = (extA << 8) | bc[i + 1];
            d.bcLength = 2;
            d.opcode = 0x10;
        } else if (op == SistaV1::ExtPushLitConst) {
            if (i + 1 >= len) break;
            d.operand = (extA << 8) | bc[i + 1];
            d.bcLength = 2;
            d.opcode = 0x20;
        } else if (op == SistaV1::ExtPushTemp) {
            if (i + 1 >= len) break;
            d.operand = bc[i + 1];
            d.bcLength = 2;
            d.opcode = SistaV1::PushTempBase;
        } else if (op == SistaV1::PushInteger) {
            if (i + 1 >= len) break;
            int value = (extB << 8) | bc[i + 1];
            d.operand = value;
            d.bcLength = 2;
        } else if (op == SistaV1::ExtSend) {
            if (i + 1 >= len) break;
            uint8_t desc = bc[i + 1];
            d.operand = ((extA << 5) | (desc >> 3)) & 0xFFFF;
            d.operand2 = ((extB << 3) | (desc & 0x07)) & 0xFF;
            d.bcLength = 2;
        } else if (op == SistaV1::ExtJump) {
            if (i + 1 >= len) break;
            int offset = bc[i + 1] + (extB << 8);
            d.branchTarget = (int)i + 2 + offset;
            d.bcLength = 2;
            d.opcode = SistaV1::ShortJumpBase;
        } else if (op == SistaV1::ExtJumpTrue) {
            if (i + 1 >= len) break;
            int offset = bc[i + 1] + (extB << 8);
            d.branchTarget = (int)i + 2 + offset;
            d.bcLength = 2;
            d.opcode = SistaV1::ShortJumpTrueBase;
        } else if (op == SistaV1::ExtJumpFalse) {
            if (i + 1 >= len) break;
            int offset = bc[i + 1] + (extB << 8);
            d.branchTarget = (int)i + 2 + offset;
            d.bcLength = 2;
            d.opcode = SistaV1::ShortJumpFalseBase;
        } else if (op == SistaV1::ExtPopStoreRecv) {
            if (i + 1 >= len) break;
            d.operand = (extA << 8) | bc[i + 1];
            d.bcLength = 2;
            d.opcode = SistaV1::PopStoreRecvBase;
        } else if (op == SistaV1::ExtPopStoreTemp) {
            if (i + 1 >= len) break;
            d.operand = bc[i + 1];
            d.bcLength = 2;
            d.opcode = SistaV1::PopStoreTempBase;
        } else if (op == SistaV1::ExtStoreTemp) {
            if (i + 1 >= len) break;
            d.operand = bc[i + 1];
            d.bcLength = 2;
        } else if (op == SistaV1::CallPrimitive) {
            // 3-byte primitive call — skip it (already handled by activateMethod)
            if (i + 2 >= len) break;
            d.bcLength = 3;
            out.push_back(d);
            i += d.bcLength;
            extA = extB = 0;
            continue;
        } else if (op == SistaV1::PushFullBlock) {
            // Block creation — bail out of T2 (methods with blocks are complex)
            return false;
        } else if (op == SistaV1::PushClosure) {
            return false;
        } else if (op >= 0xDA && op <= 0xDF) {
            // Reserved — nop
            out.push_back(d);
            i += d.bcLength;
            extA = extB = 0;
            continue;
        } else {
            // Unsupported bytecode — bail
            return false;
        }

        out.push_back(d);
        i += d.bcLength;
        extA = extB = 0;
    }
    return true;
}

// ===== MAIN COMPILE =====

void* Tier2Compiler::compile(Oop compiledMethod, JITMethod* oldVersion) {
    (void)oldVersion;  // TODO: use IC data for type specialization

    // Create a fresh MIR context for each compilation.
    // MIR_finish frees generated code, so we copy it to a persistent
    // mmap allocation before destroying the context.
    mirCtx_ = MIR_init();
    if (!mirCtx_) return nullptr;
    MIR_set_error_func(mirCtx_, reinterpret_cast<MIR_error_func_t>(mir_error_handler));

    // --- Extract method bytecodes ---
    ObjectHeader* methodObj = reinterpret_cast<ObjectHeader*>(compiledMethod.rawBits());
    Oop headerOop = methodObj->slotAt(0);
    uint64_t header = headerOop.rawBits();
    if (!(header & 1)) return nullptr;  // Not a SmallInteger header

    int numLiterals = (int)((header >> 3) & 0x7FFF);
    int tempCount   = (int)((header >> 18) & 0x3F);
    int argCount    = (int)((header >> 24) & 0x0F);
    int primNum     = (int)((header >> 28) & 0x3FF);

    // Methods with primitives: Tier 1 handles them fine, skip for Tier 2
    if (primNum != 0) {
        MIR_finish(mirCtx_); mirCtx_ = nullptr;
        return nullptr;
    }

    const uint8_t* bytecodes = reinterpret_cast<const uint8_t*>(methodObj) + 8 + (1 + numLiterals) * 8;
    size_t bcLen = methodObj->byteSize() - 8 - (1 + numLiterals) * 8;

    if (bcLen == 0 || bcLen > 4096) {
        MIR_finish(mirCtx_); mirCtx_ = nullptr;
        compilationsFailed_++;
        return nullptr;
    }

    fprintf(stderr, "[T2] Attempting method %p: %d lits, %d temps, %d args, %zu bc\n",
            (void*)compiledMethod.rawBits(), numLiterals, tempCount, argCount, bcLen);

    // --- Decode bytecodes ---
    std::vector<T2BC> decoded;
    if (!decodeBytecodes(bytecodes, bcLen, decoded)) {
        MIR_finish(mirCtx_); mirCtx_ = nullptr;
        static int decodeFailCount = 0;
        if (decodeFailCount++ < 5)
            fprintf(stderr, "[T2] Decode failed for method %p (bc len %zu)\n",
                    (void*)compiledMethod.rawBits(), bcLen);
        compilationsFailed_++;
        return nullptr;
    }

    if (decoded.empty()) {
        MIR_finish(mirCtx_); mirCtx_ = nullptr;
        compilationsFailed_++;
        return nullptr;
    }

    // --- Create MIR module and function ---
    // Each compilation gets a fresh module (MIR_finish resets all)
    // Each compilation gets a unique module/function name (MIR context is reused)
    char modName[32], funcName[32];
    snprintf(modName, sizeof(modName), "t2_%zu", methodsCompiled_ + compilationsFailed_);
    snprintf(funcName, sizeof(funcName), "t2m_%zu", methodsCompiled_ + compilationsFailed_);

    mirModule_ = MIR_new_module(mirCtx_, modName);

    // Function takes JITState* (i64), returns void
    mirFunc_ = MIR_new_func(mirCtx_, funcName, 0, nullptr, 1, MIR_T_I64, "state");

    // Get the state pointer register
    reg_statePtr_ = MIR_reg(mirCtx_, "state", mirFunc_->u.func);

    // Create well-known registers
    reg_sp_       = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, "sp");
    reg_receiver_ = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, "rcv");
    reg_tempBase_ = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, "tb");
    reg_literals_ = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, "lit");
    reg_trueOop_  = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, "tru");
    reg_falseOop_ = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, "fls");
    reg_nilOop_   = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, "nil_");

    scratchCounter_ = 0;
    vstackDepth_ = 0;

    // --- Pre-create labels for branch targets ---
    // Build a map from bcOffset to label
    std::memset(bcLabelUsed_, 0, sizeof(bcLabelUsed_));
    for (auto& bc : decoded) {
        if (bc.branchTarget >= 0 && bc.branchTarget < MaxBCLabels) {
            if (!bcLabelUsed_[bc.branchTarget]) {
                bcLabels_[bc.branchTarget] = MIR_new_label(mirCtx_);
                bcLabelUsed_[bc.branchTarget] = true;
            }
        }
    }

    // --- Emit prologue ---
    emitPrologue(tempCount, argCount);

    // --- Emit bytecodes ---
    bool bail = false;
    for (size_t idx = 0; idx < decoded.size(); idx++) {
        auto& bc = decoded[idx];

        // If this bytecode offset is a branch target, insert the label
        // and flush vstack (merge point — can't have values in regs
        // that might not exist on the other path)
        if (bc.bcOffset >= 0 && bc.bcOffset < MaxBCLabels && bcLabelUsed_[bc.bcOffset]) {
            flushVStack();
            // Store sp back to state before label (other paths need consistent state)
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
            MIR_append_insn(mirCtx_, mirFunc_, bcLabels_[bc.bcOffset]);
            // Reload sp from state (it might have been modified by other path)
            EMIT(MIR_MOV, REG(reg_sp_), MEM(MIR_T_I64, reg_statePtr_, OFF_SP));
        }

        uint8_t op = bc.opcode;

        // --- Extension bytecodes: skip (already decoded) ---
        if (op == SistaV1::ExtendA || op == SistaV1::ExtendB) continue;

        // --- Push instructions ---
        if (op <= 0x0F) {
            // pushRecvVar: load receiver slot
            int slotIdx = bc.operand;
            MIR_reg_t val = newScratch();
            // receiver is an Oop (object pointer). Slot is at receiver + 8 + slotIdx*8
            // ObjectHeader: first 8 bytes are header word, then slots
            MIR_reg_t addr = newScratch();
            EMIT(MIR_ADD, REG(addr), REG(reg_receiver_), IMM(8 + slotIdx * 8));
            EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, addr, 0));
            vpush(val);
        }
        else if (op <= 0x1F) {
            // pushLitVar: load association value
            // literals[operand] is an Association; its value is at slot 1 (offset 8+8=16)
            int litIdx = bc.operand;
            MIR_reg_t assoc = newScratch();
            MIR_reg_t addr = newScratch();
            EMIT(MIR_ADD, REG(addr), REG(reg_literals_), IMM(litIdx * 8));
            EMIT(MIR_MOV, REG(assoc), MEM(MIR_T_I64, addr, 0));
            // assoc->value is at assoc + 8 + 8 (header word + slot 1)
            MIR_reg_t val = newScratch();
            MIR_reg_t valAddr = newScratch();
            EMIT(MIR_ADD, REG(valAddr), REG(assoc), IMM(16));
            EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, valAddr, 0));
            vpush(val);
        }
        else if (op <= 0x3F) {
            // pushLitConst: load literal
            int litIdx = bc.operand;
            MIR_reg_t val = newScratch();
            MIR_reg_t addr = newScratch();
            EMIT(MIR_ADD, REG(addr), REG(reg_literals_), IMM(litIdx * 8));
            EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, addr, 0));
            vpush(val);
        }
        else if (op >= SistaV1::PushTempBase && op <= 0x4B) {
            // pushTemp
            int tmpIdx = bc.operand;
            if (tmpIdx < tempCount_ && tmpIdx < MaxTemps) {
                if (!tempLoaded_[tmpIdx]) {
                    // Load from tempBase[tmpIdx]
                    MIR_reg_t addr = newScratch();
                    EMIT(MIR_ADD, REG(addr), REG(reg_tempBase_), IMM(tmpIdx * 8));
                    EMIT(MIR_MOV, REG(tempRegs_[tmpIdx]), MEM(MIR_T_I64, addr, 0));
                    tempLoaded_[tmpIdx] = true;
                }
                vpush(tempRegs_[tmpIdx]);
            } else {
                // Fallback: load directly
                MIR_reg_t val = newScratch();
                MIR_reg_t addr = newScratch();
                EMIT(MIR_ADD, REG(addr), REG(reg_tempBase_), IMM(tmpIdx * 8));
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, addr, 0));
                vpush(val);
            }
        }
        else if (op == SistaV1::PushReceiver) {
            vpush(reg_receiver_);
        }
        else if (op == SistaV1::PushTrue) {
            vpush(reg_trueOop_);
        }
        else if (op == SistaV1::PushFalse) {
            vpush(reg_falseOop_);
        }
        else if (op == SistaV1::PushNil) {
            // Load nil from state (not pre-loaded to avoid waste if unused)
            // Actually let's just use a scratch with the known nil value
            // nil oop is 0 in our encoding (tag 0 = pointer to nil object)
            // Hmm, nil is an object pointer. Load from state->sp area? No.
            // We don't have nil in a register. Let's load it.
            if (reg_nilOop_ == 0) {
                // Not loaded yet — but we can't check at runtime. Always load.
            }
            MIR_reg_t nilVal = newScratch();
            // We need to know the nil Oop. It's typically at a well-known address.
            // For now, load from the Smalltalk stack: tempBase[-2] is where the
            // receiver lives, and nil is a specific object.
            // Better approach: the interpreter stores nil as a constant.
            // Actually, we can get nil from the method's literal frame or from
            // memory_->nilOop(). But we don't have that at code-gen time...
            // Simplest: add nil to JITState. It's already conceptually there.
            // For now: use immediate 0 as a placeholder and fix later.
            // Actually: ObjectMemory has nilObject() but we need the Oop value.
            // Let's store nil Oop in a field we pass at entry time.
            // The cleanest approach: read from a known literal. In Pharo, nil
            // is receiver slot 0 of the nil object... that's circular.
            // Let's just bail on pushNil for now and use a deopt.
            // NO — pushNil is extremely common. Let's pass nil through JITState.
            // We'll load it in the prologue from... hmm.
            //
            // OK: JITState already has receiver and true/false oops.
            // The Tier 1 stencils use _HOLE_NIL_OOP which gets patched.
            // For Tier 2, we can load nil from the method's literal frame:
            // actually no, nil might not be a literal.
            //
            // Best approach: add a nilOop field to JITState, set by the runtime.
            // But we can't change JITState layout (stencils depend on offsets).
            //
            // Alternative: the nil Oop value is known at compile time (it's a
            // constant for the image lifetime). Embed it as an immediate.
            uint64_t nilBits = memory_.nil().rawBits();
            EMIT(MIR_MOV, REG(nilVal), IMM(nilBits));
            vpush(nilVal);
        }
        else if (op == SistaV1::PushZero) {
            MIR_reg_t val = newScratch();
            // SmallInteger 0: (0 << 3) | 1 = 1
            EMIT(MIR_MOV, REG(val), IMM((0LL << 3) | SMALLINT_TAG));
            vpush(val);
        }
        else if (op == SistaV1::PushOne) {
            MIR_reg_t val = newScratch();
            // SmallInteger 1: (1 << 3) | 1 = 9
            EMIT(MIR_MOV, REG(val), IMM((1LL << 3) | SMALLINT_TAG));
            vpush(val);
        }
        else if (op == SistaV1::Dup) {
            if (vstackDepth_ > 0) {
                vpush(vpeek());
            } else {
                MIR_reg_t val = newScratch();
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
                vpush(val);
            }
        }
        else if (op == SistaV1::PushInteger) {
            MIR_reg_t val = newScratch();
            int64_t intVal = bc.operand;
            EMIT(MIR_MOV, REG(val), IMM((intVal << 3) | SMALLINT_TAG));
            vpush(val);
        }

        // --- Pop / Store ---
        else if (op == SistaV1::Pop) {
            if (vstackDepth_ > 0) {
                vpop();
            } else {
                // sp-- (pop from memory stack)
                EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
            }
        }
        else if (op >= SistaV1::PopStoreTempBase && op <= 0xD7) {
            int tmpIdx = bc.operand;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpop();
            } else {
                val = newScratch();
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
                EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
            }
            // Store to temp register
            if (tmpIdx < tempCount_ && tmpIdx < MaxTemps) {
                EMIT(MIR_MOV, REG(tempRegs_[tmpIdx]), REG(val));
                tempLoaded_[tmpIdx] = true;
            }
            // Also store to memory (for GC visibility and deopt)
            MIR_reg_t addr = newScratch();
            EMIT(MIR_ADD, REG(addr), REG(reg_tempBase_), IMM(tmpIdx * 8));
            EMIT(MIR_MOV, MEM(MIR_T_I64, addr, 0), REG(val));
        }
        else if (op >= SistaV1::PopStoreRecvBase && op <= 0xCF) {
            int slotIdx = bc.operand;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpop();
            } else {
                val = newScratch();
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
                EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
            }
            // Store to receiver slot
            MIR_reg_t addr = newScratch();
            EMIT(MIR_ADD, REG(addr), REG(reg_receiver_), IMM(8 + slotIdx * 8));
            EMIT(MIR_MOV, MEM(MIR_T_I64, addr, 0), REG(val));
        }
        else if (op == SistaV1::ExtStoreTemp) {
            int tmpIdx = bc.operand;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpeek();  // store without pop
            } else {
                val = newScratch();
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
            }
            if (tmpIdx < tempCount_ && tmpIdx < MaxTemps) {
                EMIT(MIR_MOV, REG(tempRegs_[tmpIdx]), REG(val));
                tempLoaded_[tmpIdx] = true;
            }
            MIR_reg_t addr = newScratch();
            EMIT(MIR_ADD, REG(addr), REG(reg_tempBase_), IMM(tmpIdx * 8));
            EMIT(MIR_MOV, MEM(MIR_T_I64, addr, 0), REG(val));
        }

        // --- Return instructions ---
        else if (op == SistaV1::ReturnReceiver) {
            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_RETVAL), REG(reg_receiver_));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_RETURN));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
        }
        else if (op == SistaV1::ReturnTrue) {
            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_RETVAL), REG(reg_trueOop_));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_RETURN));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
        }
        else if (op == SistaV1::ReturnFalse) {
            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_RETVAL), REG(reg_falseOop_));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_RETURN));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
        }
        else if (op == SistaV1::ReturnNil) {
            flushVStack();
            uint64_t nilBits = memory_.nil().rawBits();
            MIR_reg_t nilVal = newScratch();
            EMIT(MIR_MOV, REG(nilVal), IMM(nilBits));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_RETVAL), REG(nilVal));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_RETURN));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
        }
        else if (op == SistaV1::ReturnTop) {
            emitReturn();
        }

        // --- Arithmetic sends (0x60-0x6F) ---
        // Selector mapping: + - < > <= >= = ~= * / \\ @ bitShift: // bitAnd: bitOr:
        else if (op >= SistaV1::ArithBase && op <= 0x6F) {
            int arithOp = op - SistaV1::ArithBase;
            // SmallInteger fast-path for + - * < > <= >= = ~=
            bool handled = false;

            if (vstackDepth_ >= 2) {
                MIR_reg_t b = vpop();
                MIR_reg_t a = vpop();

                // Check both are SmallIntegers: (a & 7) == 1 && (b & 7) == 1
                MIR_label_t slowPath = MIR_new_label(mirCtx_);
                MIR_label_t doneLbl  = MIR_new_label(mirCtx_);
                MIR_reg_t tagA = newScratch();
                MIR_reg_t tagB = newScratch();
                EMIT(MIR_AND, REG(tagA), REG(a), IMM(TAG_MASK));
                EMIT(MIR_AND, REG(tagB), REG(b), IMM(TAG_MASK));
                EMIT(MIR_BNE, LABEL_OP(slowPath), REG(tagA), IMM(SMALLINT_TAG));
                EMIT(MIR_BNE, LABEL_OP(slowPath), REG(tagB), IMM(SMALLINT_TAG));

                MIR_reg_t result = newScratch();

                if (arithOp == 0) {
                    // + : (a >> 3) + (b >> 3), check overflow, retag
                    MIR_reg_t ua = newScratch();
                    MIR_reg_t ub = newScratch();
                    EMIT(MIR_RSH, REG(ua), REG(a), IMM(3));
                    EMIT(MIR_RSH, REG(ub), REG(b), IMM(3));
                    MIR_reg_t sum = newScratch();
                    EMIT(MIR_ADDO, REG(sum), REG(ua), REG(ub));
                    EMIT(MIR_BO, LABEL_OP(slowPath));
                    // Retag: (sum << 3) | 1
                    EMIT(MIR_LSH, REG(result), REG(sum), IMM(3));
                    EMIT(MIR_OR, REG(result), REG(result), IMM(SMALLINT_TAG));
                    handled = true;
                }
                else if (arithOp == 1) {
                    // -
                    MIR_reg_t ua = newScratch();
                    MIR_reg_t ub = newScratch();
                    EMIT(MIR_RSH, REG(ua), REG(a), IMM(3));
                    EMIT(MIR_RSH, REG(ub), REG(b), IMM(3));
                    MIR_reg_t diff = newScratch();
                    EMIT(MIR_SUBO, REG(diff), REG(ua), REG(ub));
                    EMIT(MIR_BO, LABEL_OP(slowPath));
                    EMIT(MIR_LSH, REG(result), REG(diff), IMM(3));
                    EMIT(MIR_OR, REG(result), REG(result), IMM(SMALLINT_TAG));
                    handled = true;
                }
                else if (arithOp == 8) {
                    // *
                    MIR_reg_t ua = newScratch();
                    MIR_reg_t ub = newScratch();
                    EMIT(MIR_RSH, REG(ua), REG(a), IMM(3));
                    EMIT(MIR_RSH, REG(ub), REG(b), IMM(3));
                    MIR_reg_t prod = newScratch();
                    EMIT(MIR_MULO, REG(prod), REG(ua), REG(ub));
                    EMIT(MIR_BO, LABEL_OP(slowPath));
                    EMIT(MIR_LSH, REG(result), REG(prod), IMM(3));
                    EMIT(MIR_OR, REG(result), REG(result), IMM(SMALLINT_TAG));
                    handled = true;
                }
                // Comparisons: 2=<, 3=>, 4=<=, 5=>=, 6==, 7=~=
                else if (arithOp >= 2 && arithOp <= 7) {
                    // Tagged SmallInts can be compared directly (same tag)
                    MIR_reg_t cmp = newScratch();
                    switch (arithOp) {
                    case 2: EMIT(MIR_LT, REG(cmp), REG(a), REG(b)); break;
                    case 3: EMIT(MIR_GT, REG(cmp), REG(a), REG(b)); break;
                    case 4: EMIT(MIR_LE, REG(cmp), REG(a), REG(b)); break;
                    case 5: EMIT(MIR_GE, REG(cmp), REG(a), REG(b)); break;
                    case 6: EMIT(MIR_EQ, REG(cmp), REG(a), REG(b)); break;
                    case 7: EMIT(MIR_NE, REG(cmp), REG(a), REG(b)); break;
                    }
                    // result = cmp ? trueOop : falseOop
                    MIR_label_t isTrue = MIR_new_label(mirCtx_);
                    EMIT(MIR_BT, LABEL_OP(isTrue), REG(cmp));
                    EMIT(MIR_MOV, REG(result), REG(reg_falseOop_));
                    EMIT(MIR_JMP, LABEL_OP(doneLbl));
                    MIR_append_insn(mirCtx_, mirFunc_, isTrue);
                    EMIT(MIR_MOV, REG(result), REG(reg_trueOop_));
                    handled = true;
                }

                if (handled) {
                    EMIT(MIR_JMP, LABEL_OP(doneLbl));

                    // Slow path: flush to memory, exit with send
                    MIR_append_insn(mirCtx_, mirFunc_, slowPath);
                    // Push a and b back onto vstack, then emit send exit
                    vpush(a);
                    vpush(b);
                    emitSendExit(1, bc.bcOffset, false, 0);

                    MIR_append_insn(mirCtx_, mirFunc_, doneLbl);
                    vpush(result);
                } else {
                    // Unsupported arith op — exit as send
                    vpush(a);
                    vpush(b);
                    emitSendExit(1, bc.bcOffset, false, 0);
                }
            } else {
                // Not enough on vstack — flush and exit
                emitSendExit(1, bc.bcOffset, false, 0);
            }
        }

        // --- Jumps ---
        else if (op >= SistaV1::ShortJumpBase && op <= 0xB7) {
            // Unconditional jump
            int target = bc.branchTarget;
            if (target >= 0 && target < MaxBCLabels && bcLabelUsed_[target]) {
                flushVStack();
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
                EMIT(MIR_JMP, LABEL_OP(bcLabels_[target]));
            } else {
                bail = true; break;
            }
        }
        else if (op >= SistaV1::ShortJumpTrueBase && op <= 0xBF) {
            // Jump if true
            int target = bc.branchTarget;
            if (target >= 0 && target < MaxBCLabels && bcLabelUsed_[target]) {
                MIR_reg_t val;
                if (vstackDepth_ > 0) {
                    val = vpop();
                } else {
                    val = newScratch();
                    EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                }
                // Compare val == trueOop
                flushVStack();
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
                EMIT(MIR_BEQ, LABEL_OP(bcLabels_[target]), REG(val), REG(reg_trueOop_));
            } else {
                bail = true; break;
            }
        }
        else if (op >= SistaV1::ShortJumpFalseBase && op <= 0xC7) {
            // Jump if false
            int target = bc.branchTarget;
            if (target >= 0 && target < MaxBCLabels && bcLabelUsed_[target]) {
                MIR_reg_t val;
                if (vstackDepth_ > 0) {
                    val = vpop();
                } else {
                    val = newScratch();
                    EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                }
                flushVStack();
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
                EMIT(MIR_BEQ, LABEL_OP(bcLabels_[target]), REG(val), REG(reg_falseOop_));
            } else {
                bail = true; break;
            }
        }

        // --- Sends ---
        else if ((op >= SistaV1::Send0Base && op <= 0xAF) ||
                 (op >= 0x70 && op <= 0x7F) ||
                 op == SistaV1::ExtSend || op == SistaV1::ExtSuperSend) {
            int nArgs = bc.operand2;
            if (nArgs < 0) nArgs = 0;
            // Sends: flush everything and exit to interpreter
            emitSendExit(nArgs, bc.bcOffset, false, 0);
        }

        // --- Unsupported: bail ---
        else {
            static int bailCount = 0;
            if (bailCount++ < 5)
                fprintf(stderr, "[T2] Bail on unsupported opcode 0x%02X at bc[%d]\n",
                        op, bc.bcOffset);
            bail = true;
            break;
        }
    }

    if (bail) {
        // Clean up and return failure
        MIR_finish_func(mirCtx_);
        MIR_finish_module(mirCtx_);
        MIR_finish(mirCtx_);
        mirCtx_ = nullptr;
        compilationsFailed_++;
        return nullptr;
    }

    // --- Finish MIR function ---
    MIR_finish_func(mirCtx_);
    MIR_finish_module(mirCtx_);

    // --- Debug: dump MIR IR ---
    static bool dumpMIR = !!getenv("PHARO_DUMP_MIR");
    if (dumpMIR) {
        fprintf(stderr, "[T2] === MIR IR dump ===\n");
        MIR_output(mirCtx_, stderr);
        fprintf(stderr, "[T2] === end dump ===\n");
    }

    // --- Generate native code ---
    // Use setjmp to recover from MIR internal errors.
    mir_error_active = true;
    if (setjmp(mir_error_jmp) != 0) {
        mir_error_active = false;
        MIR_finish(mirCtx_);
        mirCtx_ = nullptr;
        compilationsFailed_++;
        return nullptr;
    }

    MIR_load_module(mirCtx_, mirModule_);
    MIR_gen_init(mirCtx_);
    MIR_gen_set_optimize_level(mirCtx_, 2);
    MIR_link(mirCtx_, MIR_set_gen_interface, nullptr);

    void* mirCode = MIR_gen(mirCtx_, mirFunc_);
    mir_error_active = false;

    MIR_gen_finish(mirCtx_);

    if (!mirCode) {
        MIR_finish(mirCtx_);  // No generated code to keep alive
        mirCtx_ = nullptr;
        compilationsFailed_++;
        return nullptr;
    }

    // Keep MIR context alive so generated code stays valid.
    // Each context is ~50KB; acceptable for the hot method set.
    liveContexts_.push_back(mirCtx_);
    mirCtx_ = nullptr;

    methodsCompiled_++;
    return mirCode;
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
