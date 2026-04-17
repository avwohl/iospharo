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
#include "JITRuntime.hpp"
#include "CodeZone.hpp"
#include "JITMethod.hpp"
#include "SistaV1.hpp"
#include "../ObjectMemory.hpp"
#include "../Interpreter.hpp"
#include <cstring>
#include <cstdio>

#include <vector>

#include <setjmp.h>
#include <signal.h>

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

// SEGV recovery for MIR codegen: some methods hit latent MIR bugs that
// crash generate_func_code / MIR_link / MIR_gen. Catch the signal,
// longjmp back to compile(), and return nullptr so the method falls
// through to T1. The VM's main SEGV handler (sigsegvAction) is
// restored when the guard exits so unrelated crashes still produce the
// full diagnostic.
static thread_local jmp_buf mir_segv_jmp;
static thread_local bool mir_segv_active = false;
static thread_local uint64_t mir_segv_method_oop = 0;

static void mir_segv_handler(int sig, siginfo_t* info, void* ctx) {
    (void)info; (void)ctx;
    if (mir_segv_active) {
        mir_segv_active = false;  // prevent re-entry
        fprintf(stderr, "[T2] SEGV during MIR codegen on method 0x%llx — skipping\n",
                (unsigned long long)mir_segv_method_oop);
        fflush(stderr);
        longjmp(mir_segv_jmp, 1);
    }
    // Not ours — re-raise with default action
    signal(sig, SIG_DFL);
    raise(sig);
}

namespace pharo {
namespace jit {

// Sista V1 bytecode opcodes live in src/vm/jit/SistaV1.hpp (shared with
// JITCompiler.cpp). The `using namespace SistaV1` below brings the names
// into this translation unit without the `SistaV1::` prefix.
using namespace SistaV1;

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
static constexpr int OFF_SEND_BCLEN = 108;  // sendBCLength (uint32_t)
static constexpr int OFF_TRUE      = 128;
static constexpr int OFF_FALSE     = 136;
static constexpr int OFF_YIELD_CD  = 176;  // yieldCountdown (int32_t)

// Exit reasons (must match JITState.hpp)
static constexpr int EXIT_RETURN       = 1;
static constexpr int EXIT_SEND         = 2;
static constexpr int EXIT_BLOCK_CREATE = 8;
static constexpr int EXIT_ARRAY_CREATE = 9;
static constexpr int EXIT_YIELD        = 11;

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

// ===== T2 bail statistics (per opcode) =====
static int t2DecodeBails[256] = {};  // bail in decodeBytecodes
static int t2EmitBails[256] = {};    // bail in emit loop

void Tier2Compiler::dumpBailStats() {
    int totalDecode = 0, totalEmit = 0;
    for (int i = 0; i < 256; i++) {
        totalDecode += t2DecodeBails[i];
        totalEmit += t2EmitBails[i];
    }
    if (totalDecode + totalEmit == 0) return;
    fprintf(stderr, "[T2] Bail stats: %d decode, %d emit\n", totalDecode, totalEmit);
    for (int i = 0; i < 256; i++) {
        if (t2DecodeBails[i] || t2EmitBails[i])
            fprintf(stderr, "  0x%02X: decode=%d emit=%d\n",
                    i, t2DecodeBails[i], t2EmitBails[i]);
    }
}

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
    EMIT(MIR_MOV, REG(reg_sp_), MEM(MIR_T_I64, reg_statePtr_, OFF_SP));
    EMIT(MIR_MOV, REG(reg_receiver_), MEM(MIR_T_I64, reg_statePtr_, OFF_RECEIVER));
    EMIT(MIR_MOV, REG(reg_tempBase_), MEM(MIR_T_I64, reg_statePtr_, OFF_TEMPBASE));
    EMIT(MIR_MOV, REG(reg_literals_), MEM(MIR_T_I64, reg_statePtr_, OFF_LITERALS));

    // Load trueOop/falseOop from JITState (offsets 128/136).
    // Set once by tryJITActivation; stable for image lifetime (permanent space).
    EMIT(MIR_MOV, REG(reg_trueOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_TRUE));
    EMIT(MIR_MOV, REG(reg_falseOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_FALSE));

    // Compute bcBase = state.method (raw object ptr) + bcStartFromObj_
    // This gives the bytecodes start address, used for ip computation in send exits.
    EMIT(MIR_MOV, REG(reg_bcBase_), MEM(MIR_T_I64, reg_statePtr_, OFF_METHOD));
    EMIT(MIR_ADD, REG(reg_bcBase_), REG(reg_bcBase_), IMM(bcStartFromObj_));

    // Create temp registers (loaded lazily)
    tempCount_ = tempCount;
    for (int i = 0; i < tempCount && i < MaxTemps; i++) {
        char name[32];
        snprintf(name, sizeof(name), "tmp%d", i);
        tempRegs_[i] = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, name);
        tempLoaded_[i] = false;
    }

    // Resume dispatch: if state.ip != bcBase, jump to the resume table.
    // The resume table is emitted at the end of the function by emitResumeDispatch().
    // On initial entry, state.ip == bcBase (offset 0) → fall through to first bytecode.
    resumeDispatchLabel_ = MIR_new_label(mirCtx_);
    MIR_reg_t ipReg = newScratch();
    EMIT(MIR_MOV, REG(ipReg), MEM(MIR_T_I64, reg_statePtr_, OFF_IP));
    EMIT(MIR_BNE, LABEL_OP(resumeDispatchLabel_), REG(ipReg), REG(reg_bcBase_));
    // Fall through: initial entry at bytecode 0
}

void Tier2Compiler::flushVStack() {
    // Write all vstack values to memory, then advance sp.
    // Strategy: write vstack[i] to *(sp + i*8), then sp += vstackDepth_*8.
    for (int i = 0; i < vstackDepth_; i++) {
        EMIT(MIR_MOV, MEM(MIR_T_I64, reg_sp_, i * 8), REG(vstack_[i]));
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
        // TOS is at sp[-8]: after flushVStack or label merge, sp points PAST TOS.
        retVal = newScratch();
        EMIT(MIR_MOV, REG(retVal), MEM(MIR_T_I64, reg_sp_, -8));
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

    // state->ip = bcBase + bcOffset (absolute bytecode address)
    MIR_reg_t ipReg = newScratch();
    EMIT(MIR_ADD, REG(ipReg), REG(reg_bcBase_), IMM(bcOffset));
    EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(ipReg));

    // Clear icDataPtr for chain-loop exits (only inline sends use T2 IC).
    EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_ICDATA), IMM(0));

    if (cached) {
        EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_CACHED), REG(cachedMethod));
        EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(7)); // EXIT_SEND_CACHED
    } else {
        EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_SEND));
    }

    MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
}

MIR_label_t Tier2Compiler::emitSendCall(int bcOffset, int bcLength, bool registerResume) {
    // state->sendBCLength = bcLength (for post-send resume IP in jit_t2_send)
    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_SEND_BCLEN), IMM(bcLength));

    // CALL jit_t2_send(statePtr)
    MIR_append_insn(mirCtx_, mirFunc_,
        MIR_new_call_insn(mirCtx_, 3,
            MIR_new_ref_op(mirCtx_, sendProto_),
            MIR_new_ref_op(mirCtx_, sendImport_),
            REG(reg_statePtr_)));

    // Check exitReason — branch past bail if ExitNone
    MIR_reg_t exitReg = newScratch();
    EMIT(MIR_MOV, REG(exitReg), MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT));
    MIR_label_t continueLabel = MIR_new_label(mirCtx_);
    EMIT(MIR_BEQ, LABEL_OP(continueLabel), REG(exitReg), IMM(0));

    // Bail: jit_t2_send set exitReason (ExitSend or propagated)
    MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));

    // Continue: reload state from JITState
    MIR_append_insn(mirCtx_, mirFunc_, continueLabel);
    EMIT(MIR_MOV, REG(reg_sp_), MEM(MIR_T_I64, reg_statePtr_, OFF_SP));
    EMIT(MIR_MOV, REG(reg_receiver_), MEM(MIR_T_I64, reg_statePtr_, OFF_RECEIVER));
    EMIT(MIR_MOV, REG(reg_literals_), MEM(MIR_T_I64, reg_statePtr_, OFF_LITERALS));
    EMIT(MIR_MOV, REG(reg_tempBase_), MEM(MIR_T_I64, reg_statePtr_, OFF_TEMPBASE));
    EMIT(MIR_MOV, REG(reg_trueOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_TRUE));
    EMIT(MIR_MOV, REG(reg_falseOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_FALSE));

    // Invalidate temp register cache (callee may have modified stack/heap)
    for (int i = 0; i < tempCount_; i++) tempLoaded_[i] = false;

    // Register resume point for chain loop re-entry (bail case)
    if (registerResume) {
        int postSendBC = bcOffset + bcLength;
        if (resumeCount_ < MaxResume) {
            resumePoints_[resumeCount_].postSendBC = postSendBC;
            resumePoints_[resumeCount_].label = continueLabel;
            resumeCount_++;
        }
    }

    return continueLabel;
}


// ===== BYTECODE DECODER (simplified for Tier 2) =====

static bool decodeBytecodes(const uint8_t* bc, size_t len, std::vector<T2BC>& out) {
    out.clear();
    out.reserve(len);

    // PHARO_T2_BAIL_OP=FF,F5,... — comma-separated hex opcodes.
    // If any listed opcode appears in this method, bail T2 compilation.
    // Useful for bisecting T2 miscompiles per opcode handler.
    static int bailOps[256] = {0};
    static bool bailOpsInit = false;
    if (!bailOpsInit) {
        bailOpsInit = true;
        const char* env = getenv("PHARO_T2_BAIL_OP");
        if (env) {
            const char* p = env;
            while (*p) {
                char* end;
                long v = strtol(p, &end, 16);
                if (end != p && v >= 0 && v < 256) bailOps[v] = 1;
                if (*end == ',') end++;
                p = end;
            }
        }
    }

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
        if (bailOps[op]) {
            return false;  // bail: listed opcode triggers T2 bail
        }

        if (SistaV1::isPushRecvVar(op)) {
            d.operand = op & 0x0F;
        } else if (SistaV1::isPushLitVar(op)) {
            d.operand = op & 0x0F;
        } else if (SistaV1::isPushLitConst(op)) {
            d.operand = op & 0x1F;
        } else if (SistaV1::isPushTemp(op)) {
            d.operand = op - SistaV1::PushTempBase;
        } else if (op >= SistaV1::PushReceiver && op <= SistaV1::PushNil) {
            // pushReceiver, pushTrue, pushFalse, pushNil
        } else if (op == SistaV1::PushZero || op == SistaV1::PushOne) {
            // no operand
        } else if (op == SistaV1::PushThisContext) {
            // not supported in Tier 2
            t2DecodeBails[op]++;
            return false;
        } else if (op == SistaV1::Dup) {
            // no operand
        } else if (op >= 0x54 && op <= 0x57) {
            // Unused in Sista V1 — nop
            out.push_back(d);
            i += d.bcLength;
            extA = extB = 0;
            continue;
        } else if (SistaV1::isReturn(op)) {
            // returns: ReturnReceiver..ReturnTop
        } else if (op == SistaV1::BlockReturnNil) {
            // In FullBlock (extB=0): equivalent to ReturnNil.
            if (extB != 0) {
                t2DecodeBails[op]++;
                return false;
            }
            d.opcode = SistaV1::ReturnNil;
        } else if (op == SistaV1::BlockReturnTop) {
            // In FullBlock (extA=0): equivalent to ReturnTop.
            if (extA != 0) {
                // Non-local return (enclosingLevels > 0) — complex, bail
                t2DecodeBails[op]++;
                return false;
            }
            d.opcode = SistaV1::ReturnTop;
        } else if (op == 0x5F) {
            // Nop (no operation)
            out.push_back(d);
            i += d.bcLength;
            extA = extB = 0;
            continue;
        } else if (SistaV1::isArithSelector(op)) {
            d.operand = d.bcOffset;
            d.operand2 = 1;  // all arith are 1-arg sends
        } else if (SistaV1::isSpecialSelector(op)) {
            // Special sends: selector from special selectors array, not literals.
            // Arg counts are fixed by the Sista V1 spec:
            // at:(1) at:put:(2) size(0) next(0) nextPut:(1) atEnd(0) ==(1) class(0)
            // ~~(1) value(0) value:(1) do:(1) new(0) new:(1) x(0) y(0)
            static const int specialArgCounts[16] = {
                1, 2, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0
            };
            int idx = op - SistaV1::SpecialSendBase;
            d.operand = idx;
            d.operand2 = specialArgCounts[idx];
        } else if (SistaV1::isSend0(op)) {
            d.operand = op & 0x0F;
            d.operand2 = 0;
        } else if (SistaV1::isSend1(op)) {
            d.operand = op & 0x0F;
            d.operand2 = 1;
        } else if (SistaV1::isSend2(op)) {
            d.operand = op & 0x0F;
            d.operand2 = 2;
        } else if (SistaV1::isShortJump(op)) {
            d.branchTarget = (int)i + 1 + (op & 0x07) + 1;
        } else if (SistaV1::isShortJumpTrue(op)) {
            d.branchTarget = (int)i + 1 + (op & 0x07) + 1;
        } else if (SistaV1::isShortJumpFalse(op)) {
            d.branchTarget = (int)i + 1 + (op & 0x07) + 1;
        } else if (SistaV1::isPopStoreRecv(op)) {
            d.operand = op & 0x07;
        } else if (SistaV1::isPopStoreTemp(op)) {
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
        } else if (op == SistaV1::PushCharacter) {
            if (i + 1 >= len) break;
            int codepoint = (extB << 8) | bc[i + 1];
            d.operand = codepoint;
            d.bcLength = 2;
        } else if (op == 0xE6) {
            // UNASSIGNED (was pushNClosureTemps) — 2-byte nop
            if (i + 1 >= len) break;
            d.bcLength = 2;
            out.push_back(d);
            i += d.bcLength;
            extA = extB = 0;
            continue;
        } else if (op == SistaV1::PushArray) {
            // 0xE7: j=0: Push (Array new: k); j=1: Pop k into (Array new: k)
            if (i + 1 >= len) break;
            d.operand = bc[i + 1];  // desc byte: bit7=popIntoArray, bits0-6=arraySize
            d.bcLength = 2;
        } else if (op == SistaV1::ExtSend) {
            if (i + 1 >= len) break;
            uint8_t desc = bc[i + 1];
            d.operand = ((extA << 5) | (desc >> 3)) & 0xFFFF;
            d.operand2 = ((extB << 3) | (desc & 0x07)) & 0xFF;
            d.bcLength = 2;
        } else if (op == SistaV1::ExtSuperSend) {
            // 0xEB: same encoding as ExtSend — super lookup handled by emitter
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
        } else if (op == SistaV1::ExtPopStoreLitVar) {
            // 0xF1: Pop and store into literal variable (Association value slot)
            if (i + 1 >= len) break;
            d.operand = (extA << 8) | bc[i + 1];
            d.bcLength = 2;
        } else if (op == SistaV1::ExtStoreRecv) {
            // 0xF3: Store (no pop) into receiver variable
            if (i + 1 >= len) break;
            d.operand = (extA << 8) | bc[i + 1];
            d.bcLength = 2;
        } else if (op == SistaV1::ExtStoreLitVar) {
            // 0xF4: Store (no pop) into literal variable (Association value slot)
            if (i + 1 >= len) break;
            d.operand = (extA << 8) | bc[i + 1];
            d.bcLength = 2;
        } else if (op == 0xF6 || op == 0xF7) {
            // UNASSIGNED — 2-byte nop
            if (i + 1 >= len) break;
            d.bcLength = 2;
            out.push_back(d);
            i += d.bcLength;
            extA = extB = 0;
            continue;
        } else if (op == SistaV1::CallPrimitive) {
            // Primitive methods — T1's primitive prologue handles the prim,
            // T2 can't run the primitive (just has the fallback bytecodes).
            // Bail: T1 with primitive prologue is the right path.
            t2DecodeBails[op]++;
            return false;
        } else if (op == SistaV1::PushFullBlock) {
            // 0xF9: Push FullBlockClosure (3-byte)
            if (i + 2 >= len) break;
            d.operand = (extA << 8) | bc[i + 1];  // litIndex (+ ExtA)
            d.operand2 = bc[i + 2];               // flags byte
            d.bcLength = 3;
        } else if (op == SistaV1::PushClosure) {
            t2DecodeBails[op]++;
            return false;
        } else if (op == SistaV1::PushTempAtInVec) {
            // 0xFB: Push Temp At k In Temp Vector At j (3-byte)
            if (i + 2 >= len) break;
            d.operand = bc[i + 1];   // tempIndex (k)
            d.operand2 = bc[i + 2];  // vectorIndex (j)
            d.bcLength = 3;
        } else if (op == SistaV1::StoreTempAtInVec) {
            // 0xFC: Store Temp At k In Temp Vector At j (3-byte, no pop)
            if (i + 2 >= len) break;
            d.operand = bc[i + 1];   // tempIndex (k)
            d.operand2 = bc[i + 2];  // vectorIndex (j)
            d.bcLength = 3;
        } else if (op == SistaV1::PopStoreTempAtInVec) {
            // 0xFD: Pop and Store Temp At k In Temp Vector At j (3-byte)
            if (i + 2 >= len) break;
            d.operand = bc[i + 1];   // tempIndex (k)
            d.operand2 = bc[i + 2];  // vectorIndex (j)
            d.bcLength = 3;
        } else if (op == 0xFE || op == 0xFF) {
            // UNASSIGNED — 3-byte nop
            if (i + 2 >= len) break;
            d.bcLength = 3;
            out.push_back(d);
            i += d.bcLength;
            extA = extB = 0;
            continue;
        } else if (op >= 0xDA && op <= 0xDF) {
            // Reserved — nop
            out.push_back(d);
            i += d.bcLength;
            extA = extB = 0;
            continue;
        } else {
            // Unsupported bytecode — bail
            t2DecodeBails[op]++;
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

    uint8_t* bytes = methodObj->bytes();
    size_t bcStart = (1 + numLiterals) * 8;
    size_t totalBytes = methodObj->slotCount() * 8;
    uint8_t fmt = static_cast<uint8_t>(methodObj->format());
    int unusedBytes = (fmt >= 24) ? (fmt - 24) : 0;
    size_t bcLen = totalBytes - bcStart - unusedBytes;
    const uint8_t* bytecodes = bytes + bcStart;

    // bytes() returns (uint8_t*)this + 8 (past header word).
    // bcStart is offset from bytes() to bytecodes start.
    // bcStartFromObj_ = 8 + bcStart = offset from object pointer to bytecodes.
    bcStartFromObj_ = 8 + (int)bcStart;
    resumeCount_ = 0;

    if (bcLen == 0 || bcLen > 4096) {
        MIR_finish(mirCtx_); mirCtx_ = nullptr;
        compilationsFailed_++;
        return nullptr;
    }

    // --- Decode bytecodes ---
    std::vector<T2BC> decoded;
    if (!decodeBytecodes(bytecodes, bcLen, decoded)) {
        MIR_finish(mirCtx_); mirCtx_ = nullptr;
        compilationsFailed_++;
        return nullptr;
    }

    if (decoded.empty()) {
        MIR_finish(mirCtx_); mirCtx_ = nullptr;
        compilationsFailed_++;
        return nullptr;
    }

    // T2 compiles all decodable methods (with or without loops).
    // jit_t2_send's fallback restores caller state + ExitSend, so the
    // chain loop handles sends cleanly without SavedFrame complications.

    // --- Create MIR module and function ---
    // Each compilation gets a fresh module (MIR_finish resets all)
    // Each compilation gets a unique module/function name (MIR context is reused)
    char modName[32], funcName[32];
    snprintf(modName, sizeof(modName), "t2_%zu", methodsCompiled_ + compilationsFailed_);
    snprintf(funcName, sizeof(funcName), "t2m_%zu", methodsCompiled_ + compilationsFailed_);

    mirModule_ = MIR_new_module(mirCtx_, modName);

    // Create import and prototype for the inline send helper.
    // jit_t2_send(JITState*) → void
    sendImport_ = MIR_new_import(mirCtx_, "jit_t2_send");
    sendProto_  = MIR_new_proto(mirCtx_, "t2send_p", 0, nullptr, 1, MIR_T_I64, "s");

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
    reg_bcBase_   = MIR_new_func_reg(mirCtx_, mirFunc_->u.func, MIR_T_I64, "bcb");

    scratchCounter_ = 0;
    vstackDepth_ = 0;

    // --- Allocate T2 IC slots for monomorphic send caching ---
    nextICSlot_ = 0;
    icSlotCount_ = MaxICSlots;
    icSlots_ = reinterpret_cast<uint64_t*>(
        interp_.jitRuntime().allocT2ICSlots(MaxICSlots));
    // icSlots_ may be nullptr if pool is exhausted — sends just won't get IC

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
    // Debug: dump decoded bytecodes (first 3 methods)
    static int bcDumpCount = 0;
    static bool t2Verbose = !!getenv("T2_VERBOSE");
    if (t2Verbose && bcDumpCount++ < 3) {
        fprintf(stderr, "[T2-BC] Decoded %zu bytecodes:\n", decoded.size());
        for (size_t i = 0; i < decoded.size(); i++) {
            auto& d = decoded[i];
            fprintf(stderr, "  [%2zu] bc[%2d] op=0x%02X len=%d operand=%d operand2=%d branch=%d %s\n",
                    i, d.bcOffset, d.opcode, d.bcLength, d.operand, d.operand2, d.branchTarget,
                    (d.bcOffset >= 0 && d.bcOffset < MaxBCLabels && bcLabelUsed_[d.bcOffset]) ? "*LABEL*" : "");
        }
    }
    bool bail = false;
    bool unreachable = false;  // Set after ret; cleared by branch target label
    for (size_t idx = 0; idx < decoded.size(); idx++) {
        auto& bc = decoded[idx];

        // If this bytecode offset is a branch target, insert the label
        // and flush vstack (merge point — can't have values in regs
        // that might not exist on the other path)
        if (bc.bcOffset >= 0 && bc.bcOffset < MaxBCLabels && bcLabelUsed_[bc.bcOffset]) {
            static int labelDbg = 0;
            if (t2Verbose && labelDbg++ < 30)
                fprintf(stderr, "[T2-EMIT] Label at bc[%d] op=0x%02X unreach=%d vsd=%d\n",
                        bc.bcOffset, bc.opcode, unreachable, vstackDepth_);
            if (!unreachable) {
                flushVStack();
                // Store sp back to state before label (other paths need consistent state)
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
            }
            MIR_append_insn(mirCtx_, mirFunc_, bcLabels_[bc.bcOffset]);
            // Reload sp from state (it might have been modified by other path)
            EMIT(MIR_MOV, REG(reg_sp_), MEM(MIR_T_I64, reg_statePtr_, OFF_SP));
            unreachable = false;  // Code after a branch target is reachable
        }

        // Skip dead code after ret/send-exit (until a branch target makes it reachable)
        if (unreachable) continue;

        uint8_t op = bc.opcode;

        // --- Extension bytecodes: skip (already decoded) ---
        if (op == SistaV1::ExtendA || op == SistaV1::ExtendB) continue;

        // CallPrimitive: bails in decode, never reaches emit.

        // --- Push instructions ---
        if (SistaV1::isPushRecvVar(op)) {
            // pushRecvVar: load receiver slot (header + slotIdx*8)
            int slotIdx = bc.operand;
            MIR_reg_t val = newScratch();
            EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_receiver_, 8 + slotIdx * 8));
            vpush(val);
        }
        else if (SistaV1::isPushLitVar(op)) {
            // pushLitVar: load association value (assoc = literals[i], val = assoc->slot1)
            int litIdx = bc.operand;
            MIR_reg_t assoc = newScratch();
            EMIT(MIR_MOV, REG(assoc), MEM(MIR_T_I64, reg_literals_, litIdx * 8));
            MIR_reg_t val = newScratch();
            EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, assoc, 16));
            vpush(val);
        }
        else if (SistaV1::isPushLitConst(op)) {
            // pushLitConst: load literal directly
            int litIdx = bc.operand;
            MIR_reg_t val = newScratch();
            EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_literals_, litIdx * 8));
            vpush(val);
        }
        else if (SistaV1::isPushTemp(op)) {
            // pushTemp
            int tmpIdx = bc.operand;
            if (tmpIdx < tempCount_ && tmpIdx < MaxTemps) {
                if (!tempLoaded_[tmpIdx]) {
                    EMIT(MIR_MOV, REG(tempRegs_[tmpIdx]), MEM(MIR_T_I64, reg_tempBase_, tmpIdx * 8));
                    tempLoaded_[tmpIdx] = true;
                }
                vpush(tempRegs_[tmpIdx]);
            } else {
                // Fallback: load directly
                MIR_reg_t val = newScratch();
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_tempBase_, tmpIdx * 8));
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
                // sp points PAST TOS: TOS is at sp[-8]
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, -8));
                vpush(val);
            }
        }
        else if (op == SistaV1::PushInteger) {
            MIR_reg_t val = newScratch();
            int64_t intVal = bc.operand;
            EMIT(MIR_MOV, REG(val), IMM((intVal << 3) | SMALLINT_TAG));
            vpush(val);
        }
        else if (op == SistaV1::PushCharacter) {
            MIR_reg_t val = newScratch();
            int64_t codepoint = bc.operand;
            // Character tag = 3: (codepoint << 3) | 3
            EMIT(MIR_MOV, REG(val), IMM((codepoint << 3) | 3));
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
        else if (SistaV1::isPopStoreTemp(op)) {
            int tmpIdx = bc.operand;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpop();
            } else {
                val = newScratch();
                // sp points PAST TOS: decrement first, then read
                EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
            }
            // Store to temp register
            if (tmpIdx < tempCount_ && tmpIdx < MaxTemps) {
                EMIT(MIR_MOV, REG(tempRegs_[tmpIdx]), REG(val));
                tempLoaded_[tmpIdx] = true;
            }
            // Also store to memory (for GC visibility and deopt)
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_tempBase_, tmpIdx * 8), REG(val));
        }
        else if (SistaV1::isPopStoreRecv(op)) {
            int slotIdx = bc.operand;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpop();
            } else {
                val = newScratch();
                EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
            }
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_receiver_, 8 + slotIdx * 8), REG(val));
        }
        else if (op == SistaV1::ExtStoreTemp) {
            int tmpIdx = bc.operand;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpeek();  // store without pop
            } else {
                val = newScratch();
                // sp points PAST TOS: TOS is at sp[-8]
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, -8));
            }
            if (tmpIdx < tempCount_ && tmpIdx < MaxTemps) {
                EMIT(MIR_MOV, REG(tempRegs_[tmpIdx]), REG(val));
                tempLoaded_[tmpIdx] = true;
            }
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_tempBase_, tmpIdx * 8), REG(val));
        }
        // --- Extended store variants ---
        else if (op == SistaV1::ExtStoreRecv) {
            int slotIdx = bc.operand;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpeek();  // store without pop
            } else {
                val = newScratch();
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, -8));
            }
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_receiver_, 8 + slotIdx * 8), REG(val));
        }
        else if (op == SistaV1::ExtPopStoreLitVar) {
            // Pop and store into literal variable (Association's value slot)
            int litIdx = bc.operand;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpop();
            } else {
                val = newScratch();
                // sp points PAST TOS: decrement first, then read
                EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
            }
            MIR_reg_t assoc = newScratch();
            EMIT(MIR_MOV, REG(assoc), MEM(MIR_T_I64, reg_literals_, litIdx * 8));
            EMIT(MIR_MOV, MEM(MIR_T_I64, assoc, 16), REG(val));
        }
        else if (op == SistaV1::ExtStoreLitVar) {
            // Store (no pop) into literal variable (Association's value slot)
            int litIdx = bc.operand;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpeek();
            } else {
                val = newScratch();
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, -8));
            }
            MIR_reg_t assoc = newScratch();
            EMIT(MIR_MOV, REG(assoc), MEM(MIR_T_I64, reg_literals_, litIdx * 8));
            EMIT(MIR_MOV, MEM(MIR_T_I64, assoc, 16), REG(val));
        }

        // --- Return instructions ---
        else if (op == SistaV1::ReturnReceiver) {
            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_RETVAL), REG(reg_receiver_));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_RETURN));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
            unreachable = true;
        }
        else if (op == SistaV1::ReturnTrue) {
            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_RETVAL), REG(reg_trueOop_));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_RETURN));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
            unreachable = true;
        }
        else if (op == SistaV1::ReturnFalse) {
            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_RETVAL), REG(reg_falseOop_));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_RETURN));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
            unreachable = true;
        }
        else if (op == SistaV1::ReturnNil) {
            flushVStack();
            uint64_t nilBits = memory_.nil().rawBits();
            MIR_reg_t nilVal = newScratch();
            EMIT(MIR_MOV, REG(nilVal), IMM(nilBits));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_RETVAL), REG(nilVal));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_RETURN));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
            unreachable = true;
        }
        else if (op == SistaV1::ReturnTop) {
            emitReturn();
            unreachable = true;
        }

        // --- Arithmetic sends ---
        // Selector mapping: + - < > <= >= = ~= * / \\ @ bitShift: // bitAnd: bitOr:
        else if (SistaV1::isArithSelector(op)) {
            int arithOp = op - SistaV1::ArithBase;
            // DIAGNOSTIC: PHARO_T2_NO_ARITH_FAST=1 disables SmallInt fast path
            // for ALL arith ops. PHARO_T2_NO_ARITH_OPS="0,1,5,7" disables the
            // fast path only for listed ops (comma-separated decimal).
            static bool noArithFast = !!getenv("PHARO_T2_NO_ARITH_FAST");
            static int noArithOp[16] = {0};
            static bool noArithOpInit = false;
            if (!noArithOpInit) {
                noArithOpInit = true;
                const char* env = getenv("PHARO_T2_NO_ARITH_OPS");
                if (env) {
                    const char* p = env;
                    while (*p) {
                        char* end;
                        long v = strtol(p, &end, 10);
                        if (end != p && v >= 0 && v < 16) noArithOp[v] = 1;
                        if (*end == ',') end++;
                        p = end;
                    }
                }
            }
            bool thisOpNoFast = noArithFast || noArithOp[arithOp];
            // SmallInteger fast-path for + - * < > <= >= = ~=
            bool handled = false;

            {
                // Load operands from vstack or memory stack
                MIR_reg_t a, b;
                if (vstackDepth_ >= 2) {
                    b = vpop();
                    a = vpop();
                } else if (vstackDepth_ == 1) {
                    b = vpop();
                    a = newScratch();
                    EMIT(MIR_MOV, REG(a), MEM(MIR_T_I64, reg_sp_, -8));
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                } else {
                    b = newScratch();
                    a = newScratch();
                    EMIT(MIR_MOV, REG(b), MEM(MIR_T_I64, reg_sp_, -8));
                    EMIT(MIR_MOV, REG(a), MEM(MIR_T_I64, reg_sp_, -16));
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(16));
                }

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

                if (thisOpNoFast) {
                    // DIAGNOSTIC: force slow-path send for this arith op
                    EMIT(MIR_JMP, LABEL_OP(slowPath));
                }
                else if (arithOp == 0) {
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
                    // - : tagged subtraction with manual overflow check.
                    // Avoids MIR_SUBO which gives wrong results at opt level 2.
                    // a - b on tagged SmallInts: tags cancel (1 - 1 = 0),
                    // so SUB gives (va - vb) << 3 and we just OR the tag back.
                    MIR_reg_t diff = newScratch();
                    EMIT(MIR_SUB, REG(diff), REG(a), REG(b));
                    // Overflow iff (a ^ b) & (a ^ diff) has sign bit set
                    MIR_reg_t t1 = newScratch();
                    MIR_reg_t t2 = newScratch();
                    MIR_reg_t t3 = newScratch();
                    EMIT(MIR_XOR, REG(t1), REG(a), REG(b));
                    EMIT(MIR_XOR, REG(t2), REG(a), REG(diff));
                    EMIT(MIR_AND, REG(t3), REG(t1), REG(t2));
                    EMIT(MIR_BLT, LABEL_OP(slowPath), REG(t3), IMM(0));
                    EMIT(MIR_OR, REG(result), REG(diff), IMM(SMALLINT_TAG));
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

                    // Save compile-time vstack state (fast path: depth = N-2)
                    int savedDepth = vstackDepth_;
                    MIR_reg_t savedVstack[MaxVStack];
                    memcpy(savedVstack, vstack_, sizeof(MIR_reg_t) * savedDepth);

                    // Slow path: inline send via jit_t2_send for non-SmallInt/overflow
                    MIR_append_insn(mirCtx_, mirFunc_, slowPath);
                    vpush(a);
                    vpush(b);

                    // Resolve selector at compile time
                    Oop specialSelectors2 = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
                    uint64_t selBits2 = 0;
                    if (specialSelectors2.isObject() && specialSelectors2.rawBits() > 0x10000) {
                        ObjectHeader* ssArray2 = specialSelectors2.asObjectPtr();
                        size_t selectorSlot2 = arithOp * 2;
                        if (selectorSlot2 < ssArray2->slotCount()) {
                            selBits2 = ssArray2->slotAt(selectorSlot2).rawBits();
                        }
                    }

                    if (selBits2 != 0) {
                        flushVStack();
                        EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
                        EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_SENDNARGS), IMM(1));
                        MIR_reg_t ipReg3 = newScratch();
                        EMIT(MIR_ADD, REG(ipReg3), REG(reg_bcBase_), IMM(bc.bcOffset));
                        EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(ipReg3));
                        MIR_reg_t selReg3 = newScratch();
                        EMIT(MIR_MOV, REG(selReg3), IMM(selBits2));
                        EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_CACHED), REG(selReg3));
                        {
                            int64_t icAddr = 0;
                            if (icSlots_ && nextICSlot_ < icSlotCount_)
                                icAddr = reinterpret_cast<int64_t>(&icSlots_[nextICSlot_++ * 2]);
                            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_ICDATA), IMM(icAddr));
                        }
                        // No resume: stale vstack regs make resume-into-merge unsafe
                        emitSendCall(bc.bcOffset, bc.bcLength, /*registerResume=*/false);
                        // Read retval from sp, adjust sp to match fast path
                        EMIT(MIR_MOV, REG(result), MEM(MIR_T_I64, reg_sp_, -8));
                        EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM((savedDepth + 1) * 8));
                        EMIT(MIR_JMP, LABEL_OP(doneLbl));
                    } else {
                        emitSendExit(1, bc.bcOffset, false, 0);
                    }

                    // Restore compile-time vstack to fast-path state
                    vstackDepth_ = savedDepth;
                    memcpy(vstack_, savedVstack, sizeof(MIR_reg_t) * savedDepth);

                    MIR_append_insn(mirCtx_, mirFunc_, doneLbl);
                    vpush(result);
                } else {
                    // Unsupported arith op — inline send via jit_t2_send.
                    // Resolve selector at compile time from SpecialSelectorsArray.
                    Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
                    uint64_t selBits = 0;
                    if (specialSelectors.isObject() && specialSelectors.rawBits() > 0x10000) {
                        ObjectHeader* ssArray = specialSelectors.asObjectPtr();
                        size_t selectorSlot = arithOp * 2;
                        if (selectorSlot < ssArray->slotCount()) {
                            selBits = ssArray->slotAt(selectorSlot).rawBits();
                        }
                    }

                    MIR_append_insn(mirCtx_, mirFunc_, slowPath);
                    vpush(a);
                    vpush(b);

                    if (selBits == 0) {
                        // Can't resolve selector — bail to chain loop
                        emitSendExit(1, bc.bcOffset, false, 0);
                        unreachable = true;
                    } else {
                        flushVStack();
                        EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
                        EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_SENDNARGS), IMM(1));
                        MIR_reg_t ipReg2 = newScratch();
                        EMIT(MIR_ADD, REG(ipReg2), REG(reg_bcBase_), IMM(bc.bcOffset));
                        EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(ipReg2));
                        MIR_reg_t selReg2 = newScratch();
                        EMIT(MIR_MOV, REG(selReg2), IMM(selBits));
                        EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_CACHED), REG(selReg2));
                        {
                            int64_t icAddr = 0;
                            if (icSlots_ && nextICSlot_ < icSlotCount_)
                                icAddr = reinterpret_cast<int64_t>(&icSlots_[nextICSlot_++ * 2]);
                            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_ICDATA), IMM(icAddr));
                        }
                        emitSendCall(bc.bcOffset, bc.bcLength);
                    }
                }
            }
        }

        // --- Jumps ---
        else if (SistaV1::isShortJump(op)) {
            // Unconditional jump
            int target = bc.branchTarget;
            if (target >= 0 && target < MaxBCLabels && bcLabelUsed_[target]) {
                flushVStack();
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
                // Yield check for backward jumps
                if (target <= bc.bcOffset) {
                    MIR_reg_t ycd = newScratch();
                    MIR_label_t noYield = MIR_new_label(mirCtx_);
                    EMIT(MIR_MOV, REG(ycd), MEM(MIR_T_I32, reg_statePtr_, OFF_YIELD_CD));
                    EMIT(MIR_SUB, REG(ycd), REG(ycd), IMM(1));
                    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_YIELD_CD), REG(ycd));
                    EMIT(MIR_BGT, LABEL_OP(noYield), REG(ycd), IMM(0));
                    // Yield exit: set ip and exitReason, return
                    MIR_reg_t yip = newScratch();
                    EMIT(MIR_ADD, REG(yip), REG(reg_bcBase_), IMM(target));
                    EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(yip));
                    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_YIELD));
                    MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
                    MIR_append_insn(mirCtx_, mirFunc_, noYield);
                }
                EMIT(MIR_JMP, LABEL_OP(bcLabels_[target]));
                unreachable = true;
            } else {
                bail = true; break;
            }
        }
        else if (SistaV1::isShortJumpTrue(op)) {
            // Jump if true
            int target = bc.branchTarget;
            if (target >= 0 && target < MaxBCLabels && bcLabelUsed_[target]) {
                MIR_reg_t val;
                if (vstackDepth_ > 0) {
                    val = vpop();
                } else {
                    val = newScratch();
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                    EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
                }
                flushVStack();
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
                if (target <= bc.bcOffset) {
                    // Backward conditional: branch to yield check, then to target
                    MIR_label_t noJump = MIR_new_label(mirCtx_);
                    EMIT(MIR_BNE, LABEL_OP(noJump), REG(val), REG(reg_trueOop_));
                    // Yield check
                    MIR_reg_t ycd = newScratch();
                    MIR_label_t noYield = MIR_new_label(mirCtx_);
                    EMIT(MIR_MOV, REG(ycd), MEM(MIR_T_I32, reg_statePtr_, OFF_YIELD_CD));
                    EMIT(MIR_SUB, REG(ycd), REG(ycd), IMM(1));
                    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_YIELD_CD), REG(ycd));
                    EMIT(MIR_BGT, LABEL_OP(noYield), REG(ycd), IMM(0));
                    MIR_reg_t yip = newScratch();
                    EMIT(MIR_ADD, REG(yip), REG(reg_bcBase_), IMM(target));
                    EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(yip));
                    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_YIELD));
                    MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
                    MIR_append_insn(mirCtx_, mirFunc_, noYield);
                    EMIT(MIR_JMP, LABEL_OP(bcLabels_[target]));
                    MIR_append_insn(mirCtx_, mirFunc_, noJump);
                } else {
                    EMIT(MIR_BEQ, LABEL_OP(bcLabels_[target]), REG(val), REG(reg_trueOop_));
                }
            } else {
                bail = true; break;
            }
        }
        else if (SistaV1::isShortJumpFalse(op)) {
            // Jump if false
            int target = bc.branchTarget;
            if (target >= 0 && target < MaxBCLabels && bcLabelUsed_[target]) {
                MIR_reg_t val;
                if (vstackDepth_ > 0) {
                    val = vpop();
                } else {
                    val = newScratch();
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                    EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
                }
                flushVStack();
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
                if (target <= bc.bcOffset) {
                    // Backward conditional: branch to yield check, then to target
                    MIR_label_t noJump = MIR_new_label(mirCtx_);
                    EMIT(MIR_BNE, LABEL_OP(noJump), REG(val), REG(reg_falseOop_));
                    // Yield check
                    MIR_reg_t ycd = newScratch();
                    MIR_label_t noYield = MIR_new_label(mirCtx_);
                    EMIT(MIR_MOV, REG(ycd), MEM(MIR_T_I32, reg_statePtr_, OFF_YIELD_CD));
                    EMIT(MIR_SUB, REG(ycd), REG(ycd), IMM(1));
                    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_YIELD_CD), REG(ycd));
                    EMIT(MIR_BGT, LABEL_OP(noYield), REG(ycd), IMM(0));
                    MIR_reg_t yip = newScratch();
                    EMIT(MIR_ADD, REG(yip), REG(reg_bcBase_), IMM(target));
                    EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(yip));
                    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_YIELD));
                    MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
                    MIR_append_insn(mirCtx_, mirFunc_, noYield);
                    EMIT(MIR_JMP, LABEL_OP(bcLabels_[target]));
                    MIR_append_insn(mirCtx_, mirFunc_, noJump);
                } else {
                    EMIT(MIR_BEQ, LABEL_OP(bcLabels_[target]), REG(val), REG(reg_falseOop_));
                }
            } else {
                bail = true; break;
            }
        }

        // --- Sends ---
        // Regular sends: inline via jit_t2_send CALL
        else if (SistaV1::isLiteralSend(op) || op == SistaV1::ExtSend) {
            int nArgs = bc.operand2;
            if (nArgs < 0) nArgs = 0;
            int litIndex = bc.operand;

            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_SENDNARGS), IMM(nArgs));
            MIR_reg_t ipReg = newScratch();
            EMIT(MIR_ADD, REG(ipReg), REG(reg_bcBase_), IMM(bc.bcOffset));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(ipReg));
            MIR_reg_t selReg = newScratch();
            EMIT(MIR_MOV, REG(selReg), MEM(MIR_T_I64, reg_literals_, litIndex * 8));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_CACHED), REG(selReg));
            {
                int64_t icAddr = 0;
                if (icSlots_ && nextICSlot_ < icSlotCount_)
                    icAddr = reinterpret_cast<int64_t>(&icSlots_[nextICSlot_++ * 2]);
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_ICDATA), IMM(icAddr));
            }

            emitSendCall(bc.bcOffset, bc.bcLength);
        }
        // Super sends: store selector, exit to chain loop for super lookup
        else if (op == SistaV1::ExtSuperSend) {
            int nArgs = bc.operand2;
            int litIndex = bc.operand;
            if (nArgs < 0) nArgs = 0;

            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_SENDNARGS), IMM(nArgs));
            MIR_reg_t ipReg = newScratch();
            EMIT(MIR_ADD, REG(ipReg), REG(reg_bcBase_), IMM(bc.bcOffset));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(ipReg));
            // Store selector in cachedTarget for chain loop ExitSend handler
            MIR_reg_t selReg = newScratch();
            EMIT(MIR_MOV, REG(selReg), MEM(MIR_T_I64, reg_literals_, litIndex * 8));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_CACHED), REG(selReg));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_ICDATA), IMM(0));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_SEND));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));
            unreachable = true;
        }

        // --- Special sends (0x70-0x7F: special selectors 16..31) ---
        // at:(0) at:put:(1) size(2) next(3) nextPut:(4) atEnd(5) ==(6) class(7)
        // ~~(8) value(9) value:(10) do:(11) new(12) new:(13) x(14) y(15)
        else if (SistaV1::isSpecialSelector(op)) {
            int specIdx = bc.operand;  // 0-15
            int nArgs = bc.operand2;
            if (nArgs < 0) nArgs = 0;

            // --- Inline fast paths for trivial special sends ---

            // == (identity): pop arg+rcv, push (rcv == arg ? true : false)
            if (specIdx == 6) {
                MIR_reg_t a, b;
                if (vstackDepth_ >= 2) {
                    b = vpop(); a = vpop();
                } else if (vstackDepth_ == 1) {
                    b = vpop();
                    a = newScratch();
                    EMIT(MIR_MOV, REG(a), MEM(MIR_T_I64, reg_sp_, -8));
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                } else {
                    b = newScratch(); a = newScratch();
                    EMIT(MIR_MOV, REG(b), MEM(MIR_T_I64, reg_sp_, -8));
                    EMIT(MIR_MOV, REG(a), MEM(MIR_T_I64, reg_sp_, -16));
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(16));
                }
                MIR_reg_t result = newScratch();
                MIR_label_t isTrue = MIR_new_label(mirCtx_);
                MIR_label_t done = MIR_new_label(mirCtx_);
                EMIT(MIR_BEQ, LABEL_OP(isTrue), REG(a), REG(b));
                EMIT(MIR_MOV, REG(result), REG(reg_falseOop_));
                EMIT(MIR_JMP, LABEL_OP(done));
                MIR_append_insn(mirCtx_, mirFunc_, isTrue);
                EMIT(MIR_MOV, REG(result), REG(reg_trueOop_));
                MIR_append_insn(mirCtx_, mirFunc_, done);
                vpush(result);
            }
            // ~~ (not identical): pop arg+rcv, push (rcv != arg ? true : false)
            else if (specIdx == 8) {
                MIR_reg_t a, b;
                if (vstackDepth_ >= 2) {
                    b = vpop(); a = vpop();
                } else if (vstackDepth_ == 1) {
                    b = vpop();
                    a = newScratch();
                    EMIT(MIR_MOV, REG(a), MEM(MIR_T_I64, reg_sp_, -8));
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                } else {
                    b = newScratch(); a = newScratch();
                    EMIT(MIR_MOV, REG(b), MEM(MIR_T_I64, reg_sp_, -8));
                    EMIT(MIR_MOV, REG(a), MEM(MIR_T_I64, reg_sp_, -16));
                    EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(16));
                }
                MIR_reg_t result = newScratch();
                MIR_label_t isTrue = MIR_new_label(mirCtx_);
                MIR_label_t done = MIR_new_label(mirCtx_);
                EMIT(MIR_BNE, LABEL_OP(isTrue), REG(a), REG(b));
                EMIT(MIR_MOV, REG(result), REG(reg_falseOop_));
                EMIT(MIR_JMP, LABEL_OP(done));
                MIR_append_insn(mirCtx_, mirFunc_, isTrue);
                EMIT(MIR_MOV, REG(result), REG(reg_trueOop_));
                MIR_append_insn(mirCtx_, mirFunc_, done);
                vpush(result);
            }
            // --- Inline send for other special sends (via jit_t2_send) ---
            else {
                int selectorIndex = 16 + specIdx;
                Oop specialSelectors = memory_.specialObject(SpecialObjectIndex::SpecialSelectorsArray);
                uint64_t selBits = 0;
                if (specialSelectors.isObject() && specialSelectors.rawBits() > 0x10000) {
                    ObjectHeader* ssArray = specialSelectors.asObjectPtr();
                    size_t selectorSlot = selectorIndex * 2;
                    if (selectorSlot < ssArray->slotCount()) {
                        selBits = ssArray->slotAt(selectorSlot).rawBits();
                    }
                }
                if (selBits == 0) {
                    t2EmitBails[op]++;
                    bail = true;
                    break;
                }

                flushVStack();
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
                EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_SENDNARGS), IMM(nArgs));
                MIR_reg_t ipReg = newScratch();
                EMIT(MIR_ADD, REG(ipReg), REG(reg_bcBase_), IMM(bc.bcOffset));
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(ipReg));
                MIR_reg_t selReg = newScratch();
                EMIT(MIR_MOV, REG(selReg), IMM(selBits));
                EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_CACHED), REG(selReg));
                {
                    int64_t icAddr = 0;
                    if (icSlots_ && nextICSlot_ < icSlotCount_)
                        icAddr = reinterpret_cast<int64_t>(&icSlots_[nextICSlot_++ * 2]);
                    EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_ICDATA), IMM(icAddr));
                }

                emitSendCall(bc.bcOffset, bc.bcLength);
            }
        }

        // --- PushFullBlock (0xF9): exit to chain loop for closure creation ---
        else if (op == SistaV1::PushFullBlock) {
            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
            // Set ip to PushFullBlock bytecode address
            MIR_reg_t ipReg = newScratch();
            EMIT(MIR_ADD, REG(ipReg), REG(reg_bcBase_), IMM(bc.bcOffset));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(ipReg));
            // Pack litIndex and flags into cachedTarget: (litIndex & 0xFFFF) | (flags << 32)
            int litIndex = bc.operand;
            int flags = bc.operand2;
            uint64_t packed = (uint64_t)(litIndex & 0xFFFF) | ((uint64_t)(uint32_t)flags << 32);
            MIR_reg_t packReg = newScratch();
            EMIT(MIR_MOV, REG(packReg), IMM((int64_t)packed));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_CACHED), REG(packReg));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_BLOCK_CREATE));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));

            // Resume label: chain loop creates closure, then re-enters T2 here
            MIR_label_t resumeLabel = MIR_new_label(mirCtx_);
            MIR_append_insn(mirCtx_, mirFunc_, resumeLabel);
            // Reload state after chain loop handled the closure creation
            EMIT(MIR_MOV, REG(reg_sp_), MEM(MIR_T_I64, reg_statePtr_, OFF_SP));
            EMIT(MIR_MOV, REG(reg_receiver_), MEM(MIR_T_I64, reg_statePtr_, OFF_RECEIVER));
            EMIT(MIR_MOV, REG(reg_literals_), MEM(MIR_T_I64, reg_statePtr_, OFF_LITERALS));
            EMIT(MIR_MOV, REG(reg_tempBase_), MEM(MIR_T_I64, reg_statePtr_, OFF_TEMPBASE));
            EMIT(MIR_MOV, REG(reg_trueOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_TRUE));
            EMIT(MIR_MOV, REG(reg_falseOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_FALSE));
            for (int i = 0; i < tempCount_; i++) tempLoaded_[i] = false;
            int postBC = bc.bcOffset + bc.bcLength;
            if (resumeCount_ < MaxResume) {
                resumePoints_[resumeCount_].postSendBC = postBC;
                resumePoints_[resumeCount_].label = resumeLabel;
                resumeCount_++;
            }
            // NOT unreachable — code continues after resume
        }

        // --- PushArray (0xE7): exit to chain loop for array allocation ---
        else if (op == SistaV1::PushArray) {
            flushVStack();
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_SP), REG(reg_sp_));
            // Set ip to PushArray bytecode address
            MIR_reg_t ipReg = newScratch();
            EMIT(MIR_ADD, REG(ipReg), REG(reg_bcBase_), IMM(bc.bcOffset));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_IP), REG(ipReg));
            // cachedTarget = desc byte (arraySize | popIntoArray<<7)
            MIR_reg_t descReg = newScratch();
            EMIT(MIR_MOV, REG(descReg), IMM(bc.operand));
            EMIT(MIR_MOV, MEM(MIR_T_I64, reg_statePtr_, OFF_CACHED), REG(descReg));
            EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_ARRAY_CREATE));
            MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));

            // Resume label
            MIR_label_t resumeLabel = MIR_new_label(mirCtx_);
            MIR_append_insn(mirCtx_, mirFunc_, resumeLabel);
            EMIT(MIR_MOV, REG(reg_sp_), MEM(MIR_T_I64, reg_statePtr_, OFF_SP));
            EMIT(MIR_MOV, REG(reg_receiver_), MEM(MIR_T_I64, reg_statePtr_, OFF_RECEIVER));
            EMIT(MIR_MOV, REG(reg_literals_), MEM(MIR_T_I64, reg_statePtr_, OFF_LITERALS));
            EMIT(MIR_MOV, REG(reg_tempBase_), MEM(MIR_T_I64, reg_statePtr_, OFF_TEMPBASE));
            EMIT(MIR_MOV, REG(reg_trueOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_TRUE));
            EMIT(MIR_MOV, REG(reg_falseOop_), MEM(MIR_T_I64, reg_statePtr_, OFF_FALSE));
            for (int i = 0; i < tempCount_; i++) tempLoaded_[i] = false;
            int postBC = bc.bcOffset + bc.bcLength;
            if (resumeCount_ < MaxResume) {
                resumePoints_[resumeCount_].postSendBC = postBC;
                resumePoints_[resumeCount_].label = resumeLabel;
                resumeCount_++;
            }
        }

        // --- Temp vector operations (closure captured temps) ---
        // Temp vectors: temp[vectorIndex] is an Array, access its slot at tempIndex
        // Array layout: header (8 bytes) + slots (8 bytes each)
        // So slot k is at array_ptr + 8 + k*8
        else if (op == SistaV1::PushTempAtInVec) {
            int tempIndex = bc.operand;
            int vectorIndex = bc.operand2;
            // Load temp vector from tempBase[vectorIndex]
            MIR_reg_t vec = newScratch();
            MIR_reg_t vecAddr = newScratch();
            EMIT(MIR_ADD, REG(vecAddr), REG(reg_tempBase_), IMM(vectorIndex * 8));
            EMIT(MIR_MOV, REG(vec), MEM(MIR_T_I64, vecAddr, 0));
            // Load slot tempIndex from the vector (array_ptr + 8 + tempIndex*8)
            MIR_reg_t val = newScratch();
            MIR_reg_t slotAddr = newScratch();
            EMIT(MIR_ADD, REG(slotAddr), REG(vec), IMM(8 + tempIndex * 8));
            EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, slotAddr, 0));
            vpush(val);
        }
        else if (op == SistaV1::StoreTempAtInVec) {
            int tempIndex = bc.operand;
            int vectorIndex = bc.operand2;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpeek();  // store without pop
            } else {
                val = newScratch();
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, -8));
            }
            // Load temp vector from tempBase[vectorIndex]
            MIR_reg_t vec = newScratch();
            MIR_reg_t vecAddr = newScratch();
            EMIT(MIR_ADD, REG(vecAddr), REG(reg_tempBase_), IMM(vectorIndex * 8));
            EMIT(MIR_MOV, REG(vec), MEM(MIR_T_I64, vecAddr, 0));
            // Store to slot tempIndex (array_ptr + 8 + tempIndex*8)
            MIR_reg_t slotAddr = newScratch();
            EMIT(MIR_ADD, REG(slotAddr), REG(vec), IMM(8 + tempIndex * 8));
            EMIT(MIR_MOV, MEM(MIR_T_I64, slotAddr, 0), REG(val));
        }
        else if (op == SistaV1::PopStoreTempAtInVec) {
            int tempIndex = bc.operand;
            int vectorIndex = bc.operand2;
            MIR_reg_t val;
            if (vstackDepth_ > 0) {
                val = vpop();
            } else {
                val = newScratch();
                EMIT(MIR_SUB, REG(reg_sp_), REG(reg_sp_), IMM(8));
                EMIT(MIR_MOV, REG(val), MEM(MIR_T_I64, reg_sp_, 0));
            }
            // Load temp vector from tempBase[vectorIndex]
            MIR_reg_t vec = newScratch();
            MIR_reg_t vecAddr = newScratch();
            EMIT(MIR_ADD, REG(vecAddr), REG(reg_tempBase_), IMM(vectorIndex * 8));
            EMIT(MIR_MOV, REG(vec), MEM(MIR_T_I64, vecAddr, 0));
            // Store to slot tempIndex (array_ptr + 8 + tempIndex*8)
            MIR_reg_t slotAddr = newScratch();
            EMIT(MIR_ADD, REG(slotAddr), REG(vec), IMM(8 + tempIndex * 8));
            EMIT(MIR_MOV, MEM(MIR_T_I64, slotAddr, 0), REG(val));
        }

        // --- Nop bytecodes (unassigned/reserved, skip in emit) ---
        else if ((op >= 0x54 && op <= 0x57) || op == 0x5F ||
                 (op >= 0xDA && op <= 0xDF) ||
                 op == 0xE6 || op == 0xF6 || op == 0xF7 ||
                 op == 0xFE || op == 0xFF) {
            // No-op: these bytecodes exist in the stream but do nothing
        }

        // --- Unsupported: bail ---
        else {
            t2EmitBails[op]++;
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

    // If the function didn't end with an explicit return, emit a safety
    // return (sets exitReason = EXIT_RETURN, returnValue = TOS).
    if (!unreachable) {
        emitReturn();
    }

    // --- Emit resume dispatch table ---
    // Reached when state.ip != bcBase (resume after send).
    // Compare (state.ip - bcBase) against each resume point's offset.
    MIR_append_insn(mirCtx_, mirFunc_, resumeDispatchLabel_);
    if (resumeCount_ > 0) {
        MIR_reg_t ipReg2 = newScratch();
        MIR_reg_t offsetReg = newScratch();
        EMIT(MIR_MOV, REG(ipReg2), MEM(MIR_T_I64, reg_statePtr_, OFF_IP));
        EMIT(MIR_SUB, REG(offsetReg), REG(ipReg2), REG(reg_bcBase_));
        for (int r = 0; r < resumeCount_; r++) {
            EMIT(MIR_BEQ, LABEL_OP(resumePoints_[r].label),
                 REG(offsetReg), IMM(resumePoints_[r].postSendBC));
        }
    }
    // Unknown resume offset — bail with ExitSend (interpreter will handle)
    EMIT(MIR_MOV, MEM(MIR_T_I32, reg_statePtr_, OFF_EXIT), IMM(EXIT_SEND));
    MIR_append_insn(mirCtx_, mirFunc_, MIR_new_ret_insn(mirCtx_, 0));

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

    // Dump MIR IR for debugging (first 3 compilations only, if T2_VERBOSE)
    static int dumpCount = 0;
    static bool dumpVerbose = !!getenv("T2_VERBOSE");
    if (dumpVerbose && dumpCount++ < 3) {
        fprintf(stderr, "\n=== MIR IR for method #%d ===\n", dumpCount);
        MIR_output_item(mirCtx_, stderr, mirFunc_);
        fprintf(stderr, "=== END MIR IR ===\n\n");
    }

    // Register external symbols before linking
    MIR_load_external(mirCtx_, "jit_t2_send", reinterpret_cast<void*>(jit_t2_send));

    MIR_gen_init(mirCtx_);
    // Default MIR optimize level is 1, not 2. At level 2+ the MIR register
    // allocator's live-range analysis miscompiles our tagged-integer
    // arithmetic — specifically the ADDO/SUBO/MULO + tag-bit slotted
    // sequences and the cmp→trueOop/falseOop select pattern. Crashes
    // reproduce at T2_LIMIT=9 and 81+ on `t2_minimal.st` with OPT=2, clean
    // at OPT=0/1.  Op-bisect (PHARO_T2_NO_ARITH_OPS=1 or 2 or 3 clears it)
    // pointed at the arith fast path; level-bisect narrowed to OPT=2+.
    // PHARO_T2_OPT=0|1|2|3 lets callers opt in higher levels for
    // benchmarking — at your own risk on send-heavy workloads.
    int optLevel = 1;
    if (const char* e = getenv("PHARO_T2_OPT")) {
        int v = atoi(e);
        if (v >= 0 && v <= 3) optLevel = v;
    }
    MIR_gen_set_optimize_level(mirCtx_, optLevel);
    // DIAG: log the method being compiled so crashes in MIR can be
    // attributed to a specific CompiledMethod oop. PHARO_T2_COMPILE_TRACE=1.
    static bool compileTrace = !!getenv("PHARO_T2_COMPILE_TRACE");
    if (compileTrace) {
        std::string sel = interp_.memory().selectorOf(compiledMethod);
        fprintf(stderr, "[T2-COMPILE] #%zu oop=0x%llx sel=#%s numBC=%zu\n",
                methodsCompiled_ + 1,
                (unsigned long long)compiledMethod.rawBits(),
                sel.c_str(), decoded.size());
        fflush(stderr);
    }

    // Guard MIR codegen against latent crashes (seen in
    // generate_func_code, MIR_link, MIR_gen on specific methods). On
    // SEGV we longjmp back here and bail the compilation.
    void* mirCode = nullptr;
    struct sigaction prevSegv, prevBus;
    struct sigaction newAction;
    newAction.sa_sigaction = mir_segv_handler;
    newAction.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&newAction.sa_mask);
    sigaction(SIGSEGV, &newAction, &prevSegv);
    sigaction(SIGBUS,  &newAction, &prevBus);
    mir_segv_method_oop = compiledMethod.rawBits();
    if (setjmp(mir_segv_jmp) == 0) {
        mir_segv_active = true;
        MIR_link(mirCtx_, MIR_set_gen_interface, nullptr);
        mirCode = MIR_gen(mirCtx_, mirFunc_);
        mir_segv_active = false;
    } else {
        // SEGV during codegen — already logged in handler. Tear down
        // partial MIR state and return nullptr so the caller falls
        // through to T1 execution for this method.
        mir_segv_active = false;
    }
    sigaction(SIGSEGV, &prevSegv, nullptr);
    sigaction(SIGBUS,  &prevBus,  nullptr);
    mir_error_active = false;

    // MIR_gen_finish is still safe to call on a torn-down gen context.
    MIR_gen_finish(mirCtx_);

    if (!mirCode) {
        MIR_finish(mirCtx_);  // No generated code to keep alive
        mirCtx_ = nullptr;
        compilationsFailed_++;
        return nullptr;
    }

    // DIAG: dump the first N bytes of native code for a specific method.
    // PHARO_T2_NATIVE_DUMP=<selector> dumps to stderr. Enable with
    // PHARO_T2_NATIVE_DUMP_MAX to cap byte count (default 2048).
    {
        static const char* dumpSel = getenv("PHARO_T2_NATIVE_DUMP");
        static int dumpMax = []() {
            const char* e = getenv("PHARO_T2_NATIVE_DUMP_MAX");
            return e ? atoi(e) : 2048;
        }();
        if (dumpSel && *dumpSel) {
            std::string sel = interp_.memory().selectorOf(compiledMethod);
            if (sel == dumpSel) {
                fprintf(stderr, "[T2-NATIVE] #%zu sel=#%s oop=0x%llx code=%p\n",
                        methodsCompiled_ + 1, sel.c_str(),
                        (unsigned long long)compiledMethod.rawBits(),
                        mirCode);
                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(mirCode);
                for (int i = 0; i < dumpMax; i += 16) {
                    fprintf(stderr, "  %04x:", i);
                    for (int j = 0; j < 16 && i + j < dumpMax; j++) {
                        fprintf(stderr, " %02x", bytes[i + j]);
                    }
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
        }
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
