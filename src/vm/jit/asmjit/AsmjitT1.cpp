/*
 * AsmjitT1.cpp - Phase 2 of the asmjit-based Tier-1 JIT compiler.
 *
 * Per scripts/jit-diff/plan_asmjit_replacement.md.  See AsmjitT1.hpp.
 *
 * compileViaAsmjit() does:
 *   1. Pre-scan the method's bytecodes.  If every byte is in the
 *      Phase 2 supported set, emit real per-bytecode code.  Else
 *      fall back to the Phase 1 bail-on-entry trampoline (which
 *      makes the method run in the interpreter via ExitSend dispatch).
 *   2. Allocate a JITMethod sized for the emitted bytes; copy bytes
 *      into codeStart(); flushICache + makeExecutable; register in
 *      MethodMap.
 *
 * Supported in Phase 2 (no IC dispatch, no arithmetic, no control
 * flow):
 *
 *   pushReceiver, pushTemp(0..11), pushRecvVar(0..15),
 *   pushLitConst(0..31), pushTrue, pushFalse, pushNil,
 *   pushZero, pushOne, pop,
 *   returnReceiver, returnTrue, returnFalse, returnNil, returnTop
 *
 * Methods that contain anything else compile to the Phase 1 stub.
 *
 * Stack discipline matches the stencil JIT: state.sp points to the
 * next-free slot (one past TOS).  Push writes to *sp then sp++; pop
 * is sp--; returnTop reads *(--sp).
 */

#include "AsmjitT1.hpp"

#if PHARO_JIT_ENABLED

#include "../CodeZone.hpp"
#include "../JITState.hpp"
#include "../PlatformJIT.hpp"
#include "../SistaV1.hpp"
#include "../../ObjectMemory.hpp"
#include "../../Interpreter.hpp"
#include "../../DebugSettings.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <asmjit/x86.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <asmjit/a64.h>
#else
#error "Unsupported architecture for AsmjitT1"
#endif

#include <asmjit/core/codeholder.h>
#include <asmjit/core/logger.h>  // FileLogger (PHARO_ASMJIT_T1_LOG=1 dump)

namespace pharo {
namespace jit {

// Forward decl: inline block-value prep helper lives in JITRuntime.cpp.
// Called from the asmjit-T1 IC HIT emit when BLOCK_VALUE_BIT is set.
extern "C" void* jit_rt_inline_block_value_prep(
    void* state, int nArgs, void* resumeAddr);

// Block-value inline counters (PHARO_T1_INLINE_BLOCK_VALUE=1 telemetry).
extern "C" uint64_t g_blockValue_tries = 0;  // BLOCK_VALUE_BIT detected
extern "C" uint64_t g_blockValue_hits  = 0;  // helper returned entry
extern "C" uint64_t g_blockValue_bails = 0;  // helper returned NULL

// Inline-J2J bail-reason counters (PHARO_T1_INLINE_J2J=1 instrumentation).
// Incremented from JIT-emitted code via address-load + atomic-ish increment;
// printed by JITRuntime stats.  Process-global (single VM per process).
extern "C" uint64_t g_inlineJ2J_hits      = 0;  // path taken (br to entry)
extern "C" uint64_t g_inlineJ2J_bail_zero = 0;  // entryAddr == 0
extern "C" uint64_t g_inlineJ2J_bail_full = 0;  // save stack at limit
extern "C" uint64_t g_inlineJ2J_bail_self = 0;  // calleeJM != callerJM
// Counters for inline-prim path firings (PHARO_T1_INLINE_PRIM_COUNTERS=1).
extern "C" uint64_t g_primAt_hits         = 0;  // tryPrimAt inline fired
extern "C" uint64_t g_primAtPut_hits      = 0;  // tryPrimAtPut inline fired
extern "C" uint64_t g_primSize_hits       = 0;  // tryPrimSize inline fired
extern "C" uint64_t g_primBitOp_hits      = 0;  // bitAnd/bitOr/bitXor fired
extern "C" uint64_t g_primFloatOp_hits    = 0;  // SmallFloat send-site fired
extern "C" uint64_t g_bcFloatArith_hits   = 0;  // 0x60/0x61 SmallFloat bytecode fired
extern "C" uint64_t g_bcArithBail_hits    = 0;  // 0x60/0x61 SmI fast-path bailed
extern "C" uint64_t g_bcRemoteTemp_hits   = 0;  // 0xFB/0xFC/0xFD inline fired
// Debug: last-seen values at the self-recursive check.  Overwritten each
// entry so post-run we can see what the comparison was checking.
extern "C" uint64_t g_inlineJ2J_dbg_caller_method = 0;
extern "C" uint64_t g_inlineJ2J_dbg_callee_method = 0;
extern "C" uint64_t g_inlineJ2J_dbg_extra         = 0;
// Cross-method bisection counter + limit.  At runtime, bail when
// g_xmethod_count > g_xmethod_max.  Limit set from
// PHARO_T1_INLINE_J2J_XMETHOD_MAX env var (default UINT64_MAX = no limit).
extern "C" uint64_t g_xmethod_count = 0;
extern "C" uint64_t g_xmethod_max   = UINT64_MAX;

// Compact xmethod trace.  Stores per-fire data in a buffer, prints
// at process exit via atexit registration.  Avoids in-loop fprintf
// (which perturbs timing such that subsequent fires don't happen).
struct XMethodTrace {
    uint64_t calleeCM;
    uint64_t callerCM;
    uint64_t stateMethod;
    uint64_t stateJitMethod;
    uint64_t stateReceiver;
    uint64_t stateSp;
    uint64_t stateTempBase;
    uint64_t stateIp;
    int32_t  stateJ2JDepth;
    int32_t  stateArgCount;
};
static constexpr size_t kXMethodTraceMax = 64;
extern "C" XMethodTrace g_xmethod_trace[kXMethodTraceMax];
XMethodTrace g_xmethod_trace[kXMethodTraceMax] = {};
extern "C" size_t g_xmethod_trace_count;
size_t g_xmethod_trace_count = 0;

static const int xmethod_atexit_install = []() {
    if (std::getenv("PHARO_T1_INLINE_J2J_XMETHOD_LOG")) {
        std::atexit([]() {
            extern void jit_rt_xmethod_dump_trace_extern();
            jit_rt_xmethod_dump_trace_extern();
        });
    }
    return 0;
}();

extern "C" void jit_rt_xmethod_dump_trace();

void jit_rt_xmethod_dump_trace_extern() {
    fprintf(stderr, "[XMETHOD-ATEXIT] called, trace_count=%zu\n",
            g_xmethod_trace_count);
    fflush(stderr);
    jit_rt_xmethod_dump_trace();
}

extern "C" void jit_rt_xmethod_dump_trace() {
    fprintf(stderr, "[XMETHOD-TRACE] %zu fires captured\n",
            std::min(g_xmethod_trace_count, kXMethodTraceMax));
    fflush(stderr);
    size_t n = std::min(g_xmethod_trace_count, kXMethodTraceMax);
    for (size_t i = 0; i < n; i++) {
        XMethodTrace& t = g_xmethod_trace[i];
        fprintf(stderr,
            "  #%zu calleeCM=0x%llx callerCM=0x%llx\n"
            "      state.method=0x%llx state.jitMethod=0x%llx\n"
            "      state.receiver=0x%llx state.sp=0x%llx state.tempBase=0x%llx\n"
            "      state.ip=0x%llx state.j2jDepth=%d state.argCount=%d\n",
            i + 1,
            (unsigned long long)t.calleeCM, (unsigned long long)t.callerCM,
            (unsigned long long)t.stateMethod, (unsigned long long)t.stateJitMethod,
            (unsigned long long)t.stateReceiver, (unsigned long long)t.stateSp,
            (unsigned long long)t.stateTempBase, (unsigned long long)t.stateIp,
            t.stateJ2JDepth, t.stateArgCount);
    }
    fflush(stderr);
}

// Helper called from cross-method emit (PHARO_T1_INLINE_J2J_XMETHOD_LOG=1).
// Updated 2026-05-18: writes to in-memory trace buffer instead of
// fprintf, so subsequent fires aren't suppressed by timing
// perturbation.  Call jit_rt_xmethod_dump_trace() to print.
extern "C" uint64_t jit_rt_xmethod_log(uint64_t state, uint64_t calleeJM,
                                       uint64_t callerJM, uint64_t calleeCM,
                                       uint64_t callerCM) {
    // Circular buffer: ring of size kXMethodTraceMax, captures the
    // most recent fires (most useful for post-corruption diagnosis).
    static size_t logN = 0;
    size_t slot = logN % kXMethodTraceMax;
    logN++;
    g_xmethod_trace_count = logN;
    // Skip fprintf for fires past the first buffer fill — only buffer.
    // Fast-path-friendly.
    {
        XMethodTrace& t = g_xmethod_trace[slot];
        uint64_t* s = (uint64_t*)state;
        t.calleeCM = calleeCM;
        t.callerCM = callerCM;
        t.stateMethod    = s[64/8];  // state.method @ 64
        t.stateJitMethod = s[56/8];  // state.jitMethod @ 56
        t.stateReceiver  = s[8/8];   // state.receiver @ 8
        t.stateSp        = s[0];     // state.sp @ 0
        t.stateTempBase  = s[24/8];  // state.tempBase @ 24
        t.stateIp        = s[48/8];  // state.ip @ 48
        t.stateJ2JDepth  = *(int32_t*)((uint8_t*)state + 160);
        t.stateArgCount  = *(int32_t*)((uint8_t*)state + 72);
        // Look up selector names for both methods.
        // state.interp is at offset 40 from state.
        Interpreter* interp = *(Interpreter**)((uint8_t*)state + 40);
        ObjectMemory* mem = *(ObjectMemory**)((uint8_t*)state + 32);
        std::string calleeSel = "?";
        std::string callerSel = "?";
        int calleePrim = -1;
        int calleeNumLits = -1;
        bool calleeHasPrim = false;
        if (interp) {
            Oop calleeOop = Oop::fromRawBits(calleeCM);
            Oop callerOop = Oop::fromRawBits(callerCM);
            calleeSel = interp->memory().selectorOf(calleeOop);
            callerSel = interp->memory().selectorOf(callerOop);
            if (calleeOop.isObject()) {
                ObjectHeader* mo = calleeOop.asObjectPtr();
                Oop hdr = mo->slotAt(0);
                if (hdr.isSmallInteger()) {
                    int64_t hb = hdr.asSmallInteger();
                    calleeNumLits = (int)(hb & 0x7FFF);
                    calleeHasPrim = ((hb >> 16) & 1) != 0;
                    if (calleeHasPrim) {
                        const uint8_t* bc = mo->bytes()
                            + (1 + calleeNumLits) * 8;
                        if (bc[0] == 0xF8) {
                            calleePrim = bc[1] | ((bc[2] & 0x1F) << 8);
                        }
                    }
                }
            }
        }
        (void)mem;
        // Dump first 16 bytes of callee bytecodes
        char bcStr[80] = "";
        uint64_t calleeJMMethodHeader = 0;
        uint8_t calleeJMArgCount = 0;
        uint8_t calleeJMTempCount = 0;
        if (interp && calleeNumLits >= 0) {
            Oop calleeOop = Oop::fromRawBits(calleeCM);
            ObjectHeader* mo = calleeOop.asObjectPtr();
            const uint8_t* bc = mo->bytes() + (1 + calleeNumLits) * 8;
            for (int k = 0; k < 12; k++) {
                snprintf(bcStr + k*3, sizeof(bcStr) - k*3, "%02x ", bc[k]);
            }
            // Inspect JM struct (offset 16 = methodHeader,
            //                    offset 34 = argCount, offset 35 = tempCount)
            const uint8_t* jmBytes = (const uint8_t*)calleeJM;
            calleeJMMethodHeader = *(const uint64_t*)(jmBytes + 16);
            calleeJMArgCount = jmBytes[34];
            calleeJMTempCount = jmBytes[35];
        }
        // Per-fire fprintf only for the first 8 fires (diagnostic);
        // subsequent fires only update the ring buffer.
        if (logN <= 8) {
            fprintf(stderr, "[XLOG #%zu] callee=#%s (cm=0x%llx prim=%d hasPrim=%d numLits=%d) caller=#%s (cm=0x%llx) bc[0..11]: %s jmMH=0x%llx jmArgC=%d jmTempC=%d\n",
                logN, calleeSel.c_str(), (unsigned long long)calleeCM,
                calleePrim, (int)calleeHasPrim, calleeNumLits,
                callerSel.c_str(), (unsigned long long)callerCM, bcStr,
                (unsigned long long)calleeJMMethodHeader,
                calleeJMArgCount, calleeJMTempCount);
            fflush(stderr);
        }
        // Optional per-fire dump (PHARO_T1_INLINE_J2J_XMETHOD_LIVE=1).
        // Default off — the trace buffer is the primary capture.
        static const bool liveDump =
            std::getenv("PHARO_T1_INLINE_J2J_XMETHOD_LIVE") != nullptr;
        if (liveDump) {
            fprintf(stderr,
                "[XLOG #%zu] calleeCM=0x%llx callerCM=0x%llx "
                "method=0x%llx jm=0x%llx rcv=0x%llx sp=0x%llx tb=0x%llx "
                "ip=0x%llx j2jDepth=%d argCount=%d\n",
                logN, (unsigned long long)calleeCM,
                (unsigned long long)callerCM,
                (unsigned long long)t.stateMethod, (unsigned long long)t.stateJitMethod,
                (unsigned long long)t.stateReceiver, (unsigned long long)t.stateSp,
                (unsigned long long)t.stateTempBase, (unsigned long long)t.stateIp,
                t.stateJ2JDepth, t.stateArgCount);
            fflush(stderr);
        }
    }
    return 1;
}

// Old verbose log helper (kept for reference; unused).
static uint64_t jit_rt_xmethod_log_old(uint64_t state, uint64_t calleeJM,
                                        uint64_t callerJM, uint64_t calleeCM,
                                        uint64_t callerCM) {
    static size_t logN = 0;
    if (logN < 5) {
        logN++;
        uint64_t* s = (uint64_t*)state;
        // Decode method header to get numLits/argCount/tempCount.
        // methodHeader = SmI bits at methodObj.slot[0] (heap offset 8).
        uint64_t calleeMH = *(uint64_t*)(calleeCM + 8);
        uint64_t callerMH = *(uint64_t*)(callerCM + 8);
        int calleeNumLits = (int)((calleeMH >> 3) & 0x7FFF);  // SmI: shift 3
        int callerNumLits = (int)((callerMH >> 3) & 0x7FFF);
        int calleeArgCount = (int)((calleeMH >> (3+15)) & 0x1F);
        int callerArgCount = (int)((callerMH >> (3+15)) & 0x1F);
        int calleeTempCount = (int)((calleeMH >> (3+15+5)) & 0x3F);
        int callerTempCount = (int)((callerMH >> (3+15+5)) & 0x3F);
        // First bytecode of callee
        uint8_t* calleeBC = (uint8_t*)(calleeCM + 8 + (1 + calleeNumLits) * 8);
        fprintf(stderr, "[XMETHOD #%zu] state=%p\n"
                        "  callee CM=0x%llx JM=0x%llx numLits=%d argCount=%d tempCount=%d\n"
                        "    bc[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x\n"
                        "  caller CM=0x%llx JM=0x%llx numLits=%d argCount=%d tempCount=%d\n"
                        "  state.sp=0x%llx state.receiver=0x%llx state.literals=0x%llx\n"
                        "  state.tempBase=0x%llx state.ip=0x%llx state.jitMethod=0x%llx\n"
                        "  state.method=0x%llx state.argCount=%llu\n",
                logN, (void*)state,
                (unsigned long long)calleeCM, (unsigned long long)calleeJM,
                calleeNumLits, calleeArgCount, calleeTempCount,
                calleeBC[0], calleeBC[1], calleeBC[2], calleeBC[3],
                calleeBC[4], calleeBC[5], calleeBC[6], calleeBC[7],
                (unsigned long long)callerCM, (unsigned long long)callerJM,
                callerNumLits, callerArgCount, callerTempCount,
                (unsigned long long)s[0], (unsigned long long)s[1],
                (unsigned long long)s[2], (unsigned long long)s[3],
                (unsigned long long)s[6], (unsigned long long)s[7],
                (unsigned long long)s[8], (unsigned long long)s[9]);
    }
    return 1;
}
extern "C" uint64_t g_inlineJ2J_dbg_ic_hits       = 0;  // count IC HIT events
extern "C" uint64_t g_inlineJ2J_dbg_extra_no_bit60 = 0; // IC HIT with extra but no bit 60
extern "C" uint64_t g_inlineJ2J_dbg_miss          = 0;  // count IC MISS events
extern "C" uint64_t g_inlineJ2J_dbg_dispatch      = 0;  // count dispatchCached events

namespace {

// JITState field offsets — guarded by static_assert in JITState.hpp.
constexpr int OFF_SP             = 0;
constexpr int OFF_RECEIVER       = 8;
constexpr int OFF_LITERALS       = 16;
constexpr int OFF_TEMPBASE       = 24;
constexpr int OFF_IP             = 48;
constexpr int OFF_JITMETHOD      = 56;
constexpr int OFF_METHOD         = 64;
constexpr int OFF_ARGCOUNT       = 72;
constexpr int OFF_EXIT           = 76;
constexpr int OFF_RETVAL         = 80;
constexpr int OFF_CACHED_TARGET  = 88;
constexpr int OFF_ICDATAPTR      = 96;
constexpr int OFF_SENDARGCOUNT   = 104;
constexpr int OFF_TRUEOOP        = 128;
constexpr int OFF_FALSEOOP       = 136;
constexpr int OFF_J2J_SAVE_CURSOR = 144;
constexpr int OFF_J2J_SAVE_LIMIT  = 152;
constexpr int OFF_J2J_DEPTH       = 160;
constexpr int OFF_J2J_TOTAL_CALLS = 164;
constexpr int OFF_J2J_ENTRY_DEPTH = 200;

// ExitReason values (JITState.hpp).
constexpr int EXIT_RETURN          = 1;
constexpr int EXIT_SEND            = 2;
constexpr int EXIT_ARITH_OVERFLOW  = 6;
constexpr int EXIT_SEND_CACHED     = 7;
constexpr int EXIT_BLOCK_CREATE    = 8;
constexpr int EXIT_ARRAY_CREATE    = 9;
constexpr int EXIT_MUST_BOOL       = 12;

// nArgs per special selector 0x70..0x7F (index = op - 0x70).
//   at: at:put: size next nextPut: atEnd == class ~~ value value: do: new new: x y
constexpr uint8_t kSpecialNArgs[16] =
    {1,2,0,0,1,0,1,0,1,0,1,1,0,1,0,0};
inline int sendNArgs(uint8_t op) {
    if (op >= 0x60 && op <= 0x6F) return 1;  // binary special selectors
    if (op <= 0x7F) return kSpecialNArgs[op - 0x70];
    if (op <= 0x8F) return 0;   // literal send 0 args
    if (op <= 0x9F) return 1;   // literal send 1 arg
    return 2;                   // 0xA0..0xAF: literal send 2 args
}

// Oop tag for SmallInteger: low 3 bits = 001.
//   fromSmallInteger(N) = (N << 3) | 1
constexpr uint64_t SMI_TAG = 0x1;
constexpr uint64_t smiBits(int64_t n) {
    return (static_cast<uint64_t>(n) << 3) | SMI_TAG;
}

// Pharo ObjectHeader is 8 bytes; slot N starts at byte 8 + N*8.
constexpr int OBJ_SLOT_0 = 8;

// Stats.
size_t g_compiled       = 0;   // total compileViaAsmjit successes
size_t g_compiledReal   = 0;   // of which actually emitted real code
size_t g_compiledStub   = 0;   // of which used the bail-on-entry stub
size_t g_failed         = 0;

// Phase 3 supported arithmetic ops (all bail to arith_overflow on
// non-SmI or signed overflow):
//   0x60 +     0x61 -     0x62 <     0x63 >
//   0x64 <=    0x65 >=    0x66 =     0x67 ~=
// Phase 3 explicitly does NOT support * / // \\ bitAnd: bitOr:
// bitShift: @ — those have edge cases (multiply overflow detection,
// divide-by-zero, exact-divide check, point allocation) that need
// dedicated emit + bail logic.  Methods using them fall through to
// the bail stub.
inline bool isPhase3ArithOp(uint8_t op) {
    return op >= 0x60 && op <= 0x67;
}

// bitAnd: (0x6E) / bitOr: (0x6F): SmI tag bits 0..2 are 001 for both
// operands, so direct bitwise op of the tagged values produces a valid
// tagged SmI result.  No untag/retag needed.
inline bool isPhase3BitOp(uint8_t op) {
    return op == 0x6E || op == 0x6F;
}

// * (0x68): SmI mul with overflow check.
inline bool isPhase3MulOp(uint8_t op) {
    return op == 0x68;
}

// bitShift: (0x6C): SmI shift with bounds + overflow check.
inline bool isPhase3ShiftOp(uint8_t op) {
    return op == 0x6C;
}

// \\ (0x6A) modulo, // (0x6D) integer divide: SmI floor-div + mod
// with sign-mismatch adjustment.
inline bool isPhase3ModOp(uint8_t op) {
    return op == 0x6A || op == 0x6D;
}

// Compute the live bytecode length: walk forward decoding bytecodes
// using SistaV1::bytecodeLength().  Tracks max forward branch target
// so a `return` only terminates decoding when no jump points past it.
//
// Mirrors the stencil decoder's logic (JITCompiler.cpp:639-660): the
// stencil JIT also stops decoding at the first unconditional return
// whose position exceeds maxBranchTarget.  Without this, the trailer
// bytes past the last return (selector/temp-name oops packed into the
// CompiledMethod's bytes) get scanned as if they were bytecodes —
// which masquerades as random opcodes and breaks downstream emit.
//
// Returns the live byte count (≤ bcLen).  If the bytecode stream is
// malformed (a multi-byte op runs past bcLen), returns the count up
// to that point (effectively trims the malformed tail).
size_t computeLiveLength(const uint8_t* bc, size_t bcLen) {
    int maxBranchTarget = 0;
    size_t i = 0;
    while (i < bcLen) {
        uint8_t op = bc[i];
        int len = SistaV1::bytecodeLength(op);
        if (i + (size_t)len > bcLen) {
            // Multi-byte op truncated — treat the truncated bytes as
            // dead.  (The stencil decoder does `goto done`.)
            return i;
        }
        // Track forward branch targets for short jumps.  ExtJump*
        // (0xED..0xEF) need extB which we don't decode here; for now,
        // assume any ExtJump points forward and pessimistically extend
        // maxBranchTarget to bcLen so we DON'T trim past an ExtJump.
        // (Phase 4 doesn't yet emit jumps anyway, so any method with
        // ExtJump bails-on-entry via the pre-scan; this is just for
        // safety against future Phase-5 work.)
        if (SistaV1::isAnyShortJump(op)) {
            int tgt = SistaV1::shortJumpTarget(op, (int)i);
            if (tgt > maxBranchTarget) maxBranchTarget = tgt;
        } else if (op == SistaV1::ExtJump
                || op == SistaV1::ExtJumpTrue
                || op == SistaV1::ExtJumpFalse) {
            // Long jump operand is a signed 8-bit byte at bc[i+1].
            // Target = (i + 2) + offset.  We don't track ExtB
            // prefix state here so prefixed long jumps may have a
            // larger effective offset; for those rare cases, fall
            // back to the pessimistic "could jump past bcLen".
            if (i + 1 < bcLen) {
                int8_t offset = static_cast<int8_t>(bc[i + 1]);
                int tgt = static_cast<int>(i) + 2 + offset;
                if (tgt > maxBranchTarget) maxBranchTarget = tgt;
            } else {
                maxBranchTarget = (int)bcLen;
            }
        }
        i += (size_t)len;
        // Stop at first unconditional return if no branches point past us.
        // Includes block returns (0x5D BlockReturnNil, 0x5E BlockReturnTop)
        // — block bodies typically end with one and the bytes past it are
        // packed temp-name / decoration data, not real bytecodes.
        if ((SistaV1::isReturn(op) || SistaV1::isBlockReturn(op))
                && (int)i > maxBranchTarget) {
            return i;
        }
    }
    return bcLen;
}

// Bytecode pre-scan: returns true iff every byte in [bc, bc+bcLen)
// is a single-byte opcode in our supported set (Phases 2 + 3).
// Caller must pass bcLen = computeLiveLength(...) so trailer bytes
// past the method's last return don't get scanned (they're not
// real bytecodes — they're packed selector/temp-name oop trailers).
//
// Rejects multi-byte opcodes (sends, jumps, ext-prefixes), arithmetic
// outside the Phase 3 allowlist, and any single-byte op outside the
// explicit allowlist.
// Phase 4 single-byte sends: 0x70-0xAF (special + literal sends).
// Each emits a bail-to-interp at the send byte; interp dispatches
// the send normally, then continues with the rest of the method.
// Same GC-safe state.ip computation as arith bail (state.method +
// bcOffsetFromMethObj).  See emitOne_x86 / emitOne_arm64 below.
inline bool isPhase4SendOp(uint8_t op) {
    // Binary special selectors with no inline arith fast path:
    //   0x69 /, 0x6B @
    // (0x60-0x67 + comparisons go through Phase 3 inline emit;
    //  0x68 *, 0x6A \\, 0x6C bitShift:, 0x6D //, 0x6E bitAnd:,
    //  0x6F bitOr: also inline.)  These two bail to interp via
    //  the standard IC miss path.
    if (op == 0x69 || op == 0x6B) {
        return true;
    }
    // 0x70..0x7F: special selectors 16..31
    // 0x80..0x8F: literal send 0 args
    // 0x90..0x9F: literal send 1 arg
    // 0xA0..0xAF: literal send 2 args
    return op >= 0x70 && op <= 0xAF;
}

// Counter of methods successfully real-emitted with conditional jumps.
// Used by the FIRST_N / ONLY_N / SKIP_N bisect knobs.  Incremented in
// compileViaAsmjit after the JITMethod is finalized.
size_t g_condJumpRealCompiles = 0;

bool allBytecodesSupported(const uint8_t* bc, size_t bcLen) {
    const bool noSendsBisect = g_debug.t1NoSendsBisect;
    const int maxSendNArgs = g_debug.t1MaxSendNArgs;
    // PHARO_T1_INLINE_J2J_TRACE_UNSUPPORTED: log unsupported byte for first
    // N calls.  Helps identify why benchFib falls through to STUB compile.
    static const bool traceUnsupp =
        std::getenv("PHARO_T1_INLINE_J2J_TRACE_UNSUPP") != nullptr;
    auto traceFail = [&](size_t at, uint8_t op, const char* why) {
        if (traceUnsupp) {
            static size_t n = 0;
            if (n++ < 30) {
                fprintf(stderr, "[T1-UNSUPPORTED #%zu] at=%zu op=0x%02x why=%s bcLen=%zu\n",
                    n, at, op, why, bcLen);
            }
        }
    };
    // First pass: ExtA/ExtB prefix bytes.  We accept the bundle iff
    // the next bytecode is one that CONSUMES the prefix value
    // (sends, jumps, prefix-modifiable push/store variants).
    // Bytecodes that don't consume the prefix would leak extA_/extB_
    // into the bytecode AFTER them — unsafe for our bail-to-interp
    // model since the rest of the method then runs in interp with
    // stale extA_/extB_ from our perspective.
    //
    // Allowed next ops:
    //   0xE2-0xE5: ExtPush{RecvVar,LitVar,LitConst,Temp}  (consume extA)
    //   0xEA-0xEB: ExtSend/ExtSuperSend  (consume extA/extB)
    //   0xED-0xEF: ExtJump variants  (consume extB)
    //   0xF0-0xF5: ExtPop/Store{Recv,LitVar,Temp}  (consume extA)
    //   0x80-0xAF: Phase 4 literal sends  (consume extA — index = extA*16+i)
    //   0x70-0x7F: special selectors  (consume extB for nArgs)
    for (size_t i = 0; i < bcLen; i++) {
        if (bc[i] == SistaV1::ExtendA || bc[i] == SistaV1::ExtendB) {
            if (i + 2 >= bcLen) {
                traceFail(i, bc[i], "ext-prefix-truncated");
                return false;
            }
            uint8_t nextOp = bc[i + 2];
            bool acceptable = false;
            // Bytecodes that consume the prefix:
            if (nextOp == SistaV1::ExtSend
                    || nextOp == SistaV1::ExtSuperSend) {
                acceptable = true;
            } else if (bc[i] == SistaV1::ExtendB
                    && (nextOp == SistaV1::ExtJump
                        || nextOp == SistaV1::ExtJumpTrue
                        || nextOp == SistaV1::ExtJumpFalse)
                    && g_debug.t1EnableJumps) {
                acceptable = true;
            } else if (nextOp >= 0x70 && nextOp <= 0xAF) {
                // Phase 4 sends (single-byte) — extA/extB extend the
                // selector index or numArgs respectively.
                acceptable = true;
            } else if (nextOp == SistaV1::ExtPushRecvVar
                    || nextOp == SistaV1::ExtPushLitVar
                    || nextOp == SistaV1::ExtPushLitConst
                    || nextOp == SistaV1::ExtPushTemp
                    || nextOp == SistaV1::ExtPopStoreRecv
                    || nextOp == SistaV1::ExtPopStoreLitVar
                    || nextOp == SistaV1::ExtPopStoreTemp
                    || nextOp == SistaV1::ExtStoreRecv
                    || nextOp == SistaV1::ExtStoreLitVar
                    || nextOp == SistaV1::ExtStoreTemp) {
                acceptable = true;
            }
            if (!acceptable) {
                traceFail(i, bc[i], "ext-prefix-not-handled-pattern");
                return false;
            }
        }
    }
    for (size_t i = 0; i < bcLen; i++) {
        uint8_t op = bc[i];
        if (op <= 0x0F) continue;                     // pushRecvVar 0..15
        if (op >= SistaV1::PushLitVarBase
                && op <= SistaV1::PushLitVarLast) continue;  // 0x10..0x1F
        if (op >= 0x20 && op <= 0x3F) continue;       // pushLitConst 0..31
        if (op >= 0x40 && op <= 0x4B) continue;       // pushTemp 0..11
        if (op == SistaV1::PushReceiver) continue;    // 0x4C
        if (op >= 0x4D && op <= 0x51) continue;       // pushTrue/False/Nil/Zero/One
        if (op == SistaV1::Dup) continue;             // 0x53
        // PushThisContext 0x52 — emit bails to interp (interp
        // materializes the context and pushes it).  Rest of method
        // runs in interp; no resume.  Methods using thisContext are
        // rare enough that partial-JIT coverage is still a win over
        // the previous full-method bail.
        if (op == SistaV1::PushThisContext) continue;  // 0x52
        if (op >= SistaV1::ReturnReceiver
                && op <= SistaV1::ReturnTop) continue; // 0x58..0x5C
        // BlockReturnNil/BlockReturnTop 0x5D/0x5E — bail to interp
        // (block returns involve frame walking + enclosingLevels
        // semantics not worth inlining).  Methods/blocks that
        // contain a block return get partial JIT coverage.
        if (op == SistaV1::BlockReturnNil
                || op == SistaV1::BlockReturnTop) continue;
        if (isPhase3ArithOp(op)) continue;            // 0x60..0x67
        if (isPhase3BitOp(op)) continue;              // 0x6E, 0x6F bitAnd:/bitOr:
        if (isPhase3MulOp(op)) continue;              // 0x68 *
        if (isPhase3ShiftOp(op)) continue;            // 0x6C bitShift:
        if (isPhase3ModOp(op)) continue;              // 0x6A \\, 0x6D //
        if (isPhase4SendOp(op)) {
            if (noSendsBisect) return false;
            if (sendNArgs(op) > maxSendNArgs) return false;
            continue;                  // 0x69/0x6A/0x6B/0x6D + 0x70..0xAF
        }
        if (op >= SistaV1::PopStoreRecvBase
                && op <= SistaV1::PopStoreRecvLast) continue;  // 0xC8..0xCF
        if (op >= SistaV1::PopStoreTempBase
                && op <= SistaV1::PopStoreTempLast) continue;  // 0xD0..0xD7
        if (op == SistaV1::Pop) continue;             // 0xD8
        // Short forward jumps: 0xB0..0xC7 (uncond / true / false).
        // Only forward jumps in this range (offset 1..8).  Verify the
        // jump target lands at a valid bytecode position within [0, bcLen).
        // Out-of-range targets indicate a malformed method or a target
        // we'd need to handle specially (e.g., past the live region).
        if (op >= SistaV1::ShortJumpBase
                && op <= SistaV1::ShortJumpFalseLast) {
            // Conditional jumps remain DISABLED by default.  Enabling them
            // (PHARO_ASMJIT_T1_ENABLE_JUMPS=1) causes the eval-startup
            // path to DNU on garbage class indices — symptoms point at a
            // JIT-side emit bug (corrupted stack or wrong jump target),
            // NOT at the bail protocol as the memory note originally
            // claimed.  Verified 2026-05-12: ExitMustBool fires 0 times
            // even with jumps enabled and no canBailMidMethod gate, so
            // the hang isn't a bail cascade.
            //
            // The canBailMidMethod field + gate in this file and in
            // Interpreter.cpp:18731-18733 is correct infrastructure
            // for when this emit bug is fixed.  Default-on since
            // 2026-05-16 — fuzzer 39/39 PASS, -4% user CPU on bench.
            if (!g_debug.t1EnableJumps) return false;
            const int jumpsFirstN = g_debug.t1JumpsFirstN;
            const int jumpsOnlyN  = g_debug.t1JumpsOnlyN;
            const int jumpsSkipN  = g_debug.t1JumpsSkipN;
            const int jumpsSkipFrom = g_debug.t1JumpsSkipFrom;
            const int jumpsSkipTo   = g_debug.t1JumpsSkipTo;
            if (jumpsFirstN >= 0 || jumpsOnlyN >= 0 || jumpsSkipN >= 0
                    || jumpsSkipFrom >= 0) {
                size_t next = g_condJumpRealCompiles + 1;
                if (jumpsFirstN >= 0 && (int)next > jumpsFirstN)
                    return false;
                if (jumpsOnlyN >= 0 && (int)next != jumpsOnlyN)
                    return false;
                if (jumpsSkipN >= 0 && (int)next == jumpsSkipN)
                    return false;
                if (jumpsSkipFrom >= 0 && jumpsSkipTo >= 0
                        && (int)next >= jumpsSkipFrom
                        && (int)next <= jumpsSkipTo)
                    return false;
            }
            int target = SistaV1::shortJumpTarget(op, (int)i);
            // Out-of-range targets are typically unreachable trailer bytes
            // (Pharo CompiledMethod trailer encoding looks like bytecodes
            // but execution never reaches them).  Allow; the emit will
            // produce a bail for the offending instruction.  Required to
            // JIT benchFib (whose trailer ends with 0xb0 → target past
            // bcLen) and most other methods with trailers.
            (void)target;
            if (op > SistaV1::ShortJumpLast && (size_t)(i + 1) >= bcLen) {
                return false;
            }
            continue;
        }
        // InlinedPrimitive 0xEC: 2-byte no-op for the interpreter
        // (per JITCompiler.cpp:395+).  Sista marks an inlined prim;
        // bytecodes that the inlined version replaced execute normally
        // around it.  Just skip the operand byte.
        if (op == SistaV1::InlinedPrimitive) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "inlined-prim-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // PushInteger 0xE8: opcode + signed 8-bit immediate.  Pushes
        // SmI(N) where N = (int8_t)operand byte.  Common; e.g. methods
        // with literal integers outside [-1, 2] range.
        if (op == SistaV1::PushInteger) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "push-integer-truncated");
                return false;
            }
            i += 1;  // skip operand byte
            continue;
        }
        // PushCharacter 0xE9: opcode + unsigned 8-bit codepoint.  Pushes
        // Character(N).  Common in string-processing methods.
        if (op == SistaV1::PushCharacter) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "push-character-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // PushFullBlock 0xF9: 3-byte (opcode + 2 operand bytes).  Emit
        // bails to ExitBlockCreate so the chain loop creates the
        // FullBlockClosure (memory allocation + outer-context capture).
        if (op == SistaV1::PushFullBlock) {
            if (i + 2 >= bcLen) {
                traceFail(i, op, "push-full-block-truncated");
                return false;
            }
            i += 2;  // skip 2 operand bytes
            continue;
        }
        // PushArray 0xE7: 2-byte (opcode + desc).  Emit bails to
        // ExitArrayCreate so the chain loop allocates the Array and
        // optionally pops `size` elements into it.  desc = arraySize:7,
        // popIntoArray:1.
        if (op == SistaV1::PushArray) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "push-array-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // PushTempAtInVec 0xFB / StoreTempAtInVec 0xFC /
        // PopStoreTempAtInVec 0xFD: 3-byte (opcode + tempIdx + vecIdx).
        // Remote temp access (read/write a slot in a temp vector held
        // by an enclosing closure).  Bail to interp at the opcode;
        // interp executes the 3-byte op (no extA_/extB_ dependency).
        // Rest of method runs in interp.
        if (op == SistaV1::PushTempAtInVec
                || op == SistaV1::StoreTempAtInVec
                || op == SistaV1::PopStoreTempAtInVec) {
            if (i + 2 >= bcLen) {
                traceFail(i, op, "remote-temp-truncated");
                return false;
            }
            i += 2;
            continue;
        }
        // ExtSend 0xEA / ExtSuperSend 0xEB: 2-byte sends.  Even
        // with the first-pass ExtA/ExtB rejection, ExtSend
        // acceptance breaks some methods (DNU cascade in snapshot
        // error path).  Investigation pending — for now stay opt-in.
        // ExtA/ExtB prefix bundle.  First-pass guard verified the
        // next bytecode consumes the prefix.  Skip the prefix data
        // byte + nextOp's full length (1, 2, or 3 bytes).
        if (op == SistaV1::ExtendA || op == SistaV1::ExtendB) {
            uint8_t nextOp = bc[i + 2];
            int nextLen = SistaV1::bytecodeLength(nextOp);
            if (i + 1 + nextLen >= bcLen) {
                traceFail(i, op, "ext-prefix-bundle-truncated");
                return false;
            }
            if (nextOp == SistaV1::ExtSuperSend) {
                static const bool acceptExtSuper =
                    std::getenv("PHARO_T1_ACCEPT_EXTSUPERSEND") != nullptr;
                if (!acceptExtSuper) {
                    traceFail(i, op, "ext-super-send-bundle-disabled");
                    return false;
                }
            }
            i += 1 + nextLen;
            continue;
        }
        if (op == SistaV1::ExtSend || op == SistaV1::ExtSuperSend) {
            // ExtSend 0xEA is safe; ExtSuperSend 0xEB needs further
            // investigation (super-send needs methodClass and the
            // bail may need extra state restore).  Opt-in flag
            // PHARO_T1_ACCEPT_EXTSUPERSEND=1 to test.
            //
            // Naked here means no preceding ExtA/B — the bundle case
            // is handled above when ExtA/B is at i.
            if (op == SistaV1::ExtSuperSend) {
                static const bool acceptExtSuper =
                    std::getenv("PHARO_T1_ACCEPT_EXTSUPERSEND") != nullptr;
                if (!acceptExtSuper) {
                    traceFail(i, op, "ext-super-send-disabled");
                    return false;
                }
            }
            if (i + 1 >= bcLen) {
                traceFail(i, op, "ext-send-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // Extended push/store variants — all 2-byte: opcode + 1-byte index.
        // Push: ExtPushRecvVar 0xE2, ExtPushLitVar 0xE3,
        //       ExtPushLitConst 0xE4, ExtPushTemp 0xE5.
        // Store-with-immutable-check (recv): ExtPopStoreRecv 0xF0,
        //                                    ExtStoreRecv 0xF3.
        // Store-no-check (temp): ExtPopStoreTemp 0xF2, ExtStoreTemp 0xF5.
        // Association store (litvar): ExtPopStoreLitVar 0xF1,
        //                             ExtStoreLitVar 0xF4.
        //   Stores to literals[idx].slot[1] — no write barrier
        //   because YG scavenge does a full old-space scan.  Naked
        //   only (the ExtA prefix-rejection first-pass guarantees no
        //   prefix extension on idx).
        if (op == SistaV1::ExtPushRecvVar || op == SistaV1::ExtPushLitVar
                || op == SistaV1::ExtPushLitConst
                || op == SistaV1::ExtPushTemp
                || op == SistaV1::ExtPopStoreTemp
                || op == SistaV1::ExtStoreTemp
                || op == SistaV1::ExtPopStoreRecv
                || op == SistaV1::ExtStoreRecv
                || op == SistaV1::ExtPopStoreLitVar
                || op == SistaV1::ExtStoreLitVar) {
            if (i + 1 >= bcLen) {
                traceFail(i, op, "ext-bytecode-truncated");
                return false;
            }
            i += 1;
            continue;
        }
        // Long jumps 0xED/0xEE/0xEF: opcode + 1-byte signed offset.
        // Target = (i + 2) + offset.  No ExtA/ExtB prefix support yet.
        // Gated on the same t1EnableJumps knob as short jumps.
        if (op == SistaV1::ExtJump || op == SistaV1::ExtJumpTrue
                || op == SistaV1::ExtJumpFalse) {
            if (!g_debug.t1EnableJumps) {
                traceFail(i, op, "long-jump-knob-off");
                return false;
            }
            if (i + 1 >= bcLen) {
                traceFail(i, op, "long-jump-truncated");
                return false;
            }
            int8_t offset = static_cast<int8_t>(bc[i + 1]);
            int target = static_cast<int>(i) + 2 + offset;
            // Allow out-of-range targets (unreachable trailer bytes).
            (void)target;
            i += 1;  // skip offset byte (loop will increment by 1 more)
            continue;
        }
        // Anything else (jumps, ext-prefixes, pushLitVar,
        // pushThisContext, dup, blockReturn, popStoreRecv/Temp,
        // mul/div/bit ops, ext bytecodes E0+, etc.) → unsupported.
        traceFail(i, op, "fallthrough-unsupported");
        return false;
    }
    return true;
}

// If the method has a primitive whose JIT prologue we can emit,
// return the primitive index.  Otherwise return -1.  Caller is
// responsible for handing us a primitive method (header bit 16 set)
// whose first 3 bytes are the CallPrimitive (0xF8 lo hi) bytecode.
//
// Currently only prim 1 (SmallInteger>>#+).  More to follow.
inline int supportedPrimIndex(const uint8_t* bc, size_t bcLen) {
    if (bcLen < 3 || bc[0] != SistaV1::CallPrimitive) return -1;
    int primIndex = bc[1] | ((bc[2] & 0x1F) << 8);
    switch (primIndex) {
    case 1:  return primIndex;   // SmallInteger>>+
    case 2:  return primIndex;   // SmallInteger>>-
    case 3:  return primIndex;   // SmallInteger>><
    case 4:  return primIndex;   // SmallInteger>>>
    case 5:  return primIndex;   // SmallInteger>><=
    case 6:  return primIndex;   // SmallInteger>>>=
    case 7:  return primIndex;   // SmallInteger>>=
    case 8:  return primIndex;   // SmallInteger>>~=
    case 9:  return primIndex;   // SmallInteger>>*
    case 14: return primIndex;   // SmallInteger>>bitAnd:
    case 15: return primIndex;   // SmallInteger>>bitOr:
    case 16: return primIndex;   // SmallInteger>>bitXor:
    // Prims 60/61/62 prologue handles fmt 2 (Array), fmt 10-11 (32-bit
    // WordArray/IntegerArray), and fmt 16-23 (byte indexable: ByteArray,
    // String, Symbol).  Other formats (fmt 9 Indexable64, fmt 12-15
    // 16-bit, fmt 3-5 variable+fixed) still fall through to the
    // Smalltalk fallback — for `Object>>basicAtPut:` that's
    // errorImproperStore, which is correct for cases the C primitive
    // would also fail on (e.g., wrong type) but raises spuriously for
    // legal receivers in those formats.  Re-enable when those land.
    case 60: return primIndex;   // Array/ByteArray/WordArray >> at:
    case 61: return primIndex;   // Array/ByteArray/WordArray >> at:put:
    case 62: return primIndex;   // Array/ByteArray/WordArray >> size
    case 541: return primIndex;  // SmallFloat>>+
    case 542: return primIndex;  // SmallFloat>>-
    case 549: return primIndex;  // SmallFloat>>*
    case 110: return primIndex;  // ProtoObject>>==
    default: return -1;
    }
}

#if defined(__x86_64__) || defined(_M_X64)

// Emit a "push value into Smalltalk stack" sequence on x86_64.  The
// value to push must already be in `valReg` (any 64-bit gp reg
// other than rdi/rcx).  Sequence:
//
//   mov rcx, [rdi+OFF_SP]       ; load sp
//   mov [rcx], valReg           ; *sp = value
//   add rcx, 8                  ; sp++
//   mov [rdi+OFF_SP], rcx       ; store sp back
//
// rcx is clobbered.  (If we ever cache sp across multiple bytecodes
// we'll factor this differently; Phase 2 reloads on every push.)
void emitPushReg(asmjit::x86::Assembler& a, asmjit::x86::Gp valReg) {
    using namespace asmjit::x86;
    a.mov(rcx, ptr(rdi, OFF_SP));
    a.mov(ptr(rcx), valReg);
    a.add(rcx, 8);
    a.mov(ptr(rdi, OFF_SP), rcx);
}

// Emit the JIT prologue for a supported primitive.  Used as a fast
// path when the chain loop inline-activates this method — without
// it, hasPrimPrologue=false blocks inline activation entirely
// (Interpreter.cpp:18731-18732).
//
// On entry:
//   rdi = state ptr
//   state.receiver = SmI candidate
//   state.tempBase[0] = SmI candidate (the argument)
// On success: set state.returnValue, set ExitReturn, ret.
// On failure (non-SmI, arith overflow): fall through.  Caller's emit
// continues with the fallback bytecode.
//
// Mirrors the stencil prologues at stencils/stencils.cpp:3231+.
//   prim 1 = #+   prim 2 = #-                       (arith, overflow check)
//   prim 3 = #<   prim 4 = #>   prim 5 = #<=
//   prim 6 = #>=  prim 7 = #=   prim 8 = #~=        (compare, return bool)
//   prim 110 = #==                                  (identical: oop bit-compare)
//
// Signed comparison of tagged SmI bits gives the same ordering as
// untagged values (tag is the same low 3 bits, so shifting preserves
// order).  So all comparisons skip the untag step.
void emitPrimProlog_x86(asmjit::x86::Assembler& a, int primIndex) {
    using namespace asmjit::x86;

    // prim 110 (#==): no type check — compare raw oop bits.
    if (primIndex == 110) {
        a.mov(rcx, ptr(rdi, OFF_RECEIVER));
        a.mov(rdx, ptr(rdi, OFF_TEMPBASE));
        a.mov(rdx, ptr(rdx));
        a.mov(rax, ptr(rdi, OFF_FALSEOOP));
        a.cmp(rcx, rdx);
        a.cmove(rax, ptr(rdi, OFF_TRUEOOP));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return;  // never falls through (always succeeds)
    }

    asmjit::Label fail = a.new_label();

    // Shared SmI check: (a^b) | (a-1) low 3 bits = 0 iff both SmI.
    a.mov(rcx, ptr(rdi, OFF_RECEIVER));   // rcx = receiver
    a.mov(rdx, ptr(rdi, OFF_TEMPBASE));
    a.mov(rdx, ptr(rdx));                 // rdx = tempBase[0] = arg
    a.mov(r8,  rcx);
    a.xor_(r8, rdx);
    a.lea(r9, asmjit::x86::ptr(rcx, -1));
    a.or_(r8, r9);
    a.test(r8.r8(), asmjit::Imm(7));
    a.jne(fail);

    if (primIndex >= 3 && primIndex <= 8) {
        // Comparison: cmp + cmov with memory source (saves trueOop load).
        a.mov(rax, ptr(rdi, OFF_FALSEOOP));
        a.cmp(rcx, rdx);
        switch (primIndex) {
            case 3: a.cmovl (rax, ptr(rdi, OFF_TRUEOOP)); break;  // <
            case 4: a.cmovg (rax, ptr(rdi, OFF_TRUEOOP)); break;  // >
            case 5: a.cmovle(rax, ptr(rdi, OFF_TRUEOOP)); break;  // <=
            case 6: a.cmovge(rax, ptr(rdi, OFF_TRUEOOP)); break;  // >=
            case 7: a.cmove (rax, ptr(rdi, OFF_TRUEOOP)); break;  // =
            case 8: a.cmovne(rax, ptr(rdi, OFF_TRUEOOP)); break;  // ~=
        }
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        a.bind(fail);
        return;
    }

    if (primIndex == 14 || primIndex == 15 || primIndex == 16) {
        // bitAnd / bitOr: SmI tag is preserved by both ops because
        // both operands have low 3 bits = 001, so AND/OR keeps it 001.
        // bitXor: low 3 = 001 XOR 001 = 000, so we re-OR the SMI_TAG.
        // Either way: operate on tagged values, no untag.
        if (primIndex == 14) a.and_(rcx, rdx);
        else if (primIndex == 15) a.or_(rcx, rdx);
        else {
            a.xor_(rcx, rdx);
            a.or_(rcx, asmjit::Imm(SMI_TAG));
        }
        a.mov(ptr(rdi, OFF_RETVAL), rcx);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        a.bind(fail);
        return;
    }

    // Arith path: tagged-arith for + and - (saves untag+retag).
    // For + : a_bits + b_bits = (a+b)*8 + 2 → sub 1 to tag.
    // For - : a_bits - b_bits = (a-b)*8     → add 1 to tag.
    // jo on the tagged op catches SmI-range overflow because tagged
    // form pre-shifts by 8 (matches the bytecode arith emit).
    if (primIndex == 1) {
        a.add(rcx, rdx);
        a.jo(fail);
        a.sub(rcx, asmjit::Imm(1));
        a.mov(ptr(rdi, OFF_RETVAL), rcx);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        a.bind(fail);
        return;
    }
    if (primIndex == 2) {
        a.sub(rcx, rdx);
        a.jo(fail);
        a.add(rcx, asmjit::Imm(1));
        a.mov(ptr(rdi, OFF_RETVAL), rcx);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        a.bind(fail);
        return;
    }

    // prim 9 (*): need to untag, multiply, check overflow, retag.
    a.sar(rcx, asmjit::Imm(3));
    a.sar(rdx, asmjit::Imm(3));
    if (primIndex == 9) {
        // a * b: imul sets OF on 64-bit signed overflow.  After that
        // we also need to confirm the result fits in 61-bit SmI range:
        // ((result << 3) >> 3) must equal result.
        a.imul(rcx, rdx);
        a.jo(fail);
        a.mov(r8, rcx);
        a.shl(r8, asmjit::Imm(3));
        a.sar(r8, asmjit::Imm(3));
        a.cmp(r8, rcx);
        a.jne(fail);
    } else {
        // Shouldn't happen — supportedPrimIndex gates this.
        a.bind(fail);
        return;
    }
    a.shl(rcx, asmjit::Imm(3));
    a.or_(rcx, asmjit::Imm(SMI_TAG));
    a.mov(ptr(rdi, OFF_RETVAL), rcx);
    a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
    a.ret();

    a.bind(fail);
}

// Emit per-bytecode code on x86_64.  Returns true if the opcode was
// handled.  The pre-scan guarantees we'll see only supported ops.
//
// `nilBits` is the raw bits of the special-objects nil — passed in
// so push-nil can bake it as a 64-bit immediate (nil is image-local,
// not a JITState field).
//
// `bcOffsetFromMethObj` is the byte offset of THIS bytecode from the
// CompiledMethod object's address.  Arith bails compute
// `state.ip = state.method.rawBits() + bcOffsetFromMethObj` at
// runtime, NOT at JIT-compile time.  This survives GC compaction:
// `state.method` is a GC-tracked Oop that gets updated in place when
// the CompiledMethod moves, while a baked absolute address would
// dangle.  Mirrors the stencil JIT, which uses `s->ip = s->ip +
// bcOffset` and relies on `afterGC()` updating state.ip.
bool emitOne_x86(asmjit::x86::Assembler& a, uint8_t op,
                  uint64_t nilBits, int bcOffsetFromMethObj,
                  int siteIdx,
                  const std::vector<asmjit::Label>& bcLabels,
                  int globalIdx,
                  int callerArgCount, int callerTempCount) {
    using namespace asmjit::x86;
    (void)bcOffsetFromMethObj;
    (void)siteIdx;
    (void)callerArgCount;
    (void)callerTempCount;

    // pushRecvVar N: push receiver.slot[N].
    if (op <= 0x0F) {
        int n = op & 0x0F;
        a.mov(rax, ptr(rdi, OFF_RECEIVER));
        a.mov(rax, ptr(rax, OBJ_SLOT_0 + n * 8));
        emitPushReg(a, rax);
        return true;
    }
    // pushLitConst N: push literals[N].
    if (op >= 0x20 && op <= 0x3F) {
        int n = op - 0x20;
        a.mov(rax, ptr(rdi, OFF_LITERALS));
        a.mov(rax, ptr(rax, n * 8));
        emitPushReg(a, rax);
        return true;
    }
    // pushLitVar N (0x10..0x1F): literals[N] is an Association;
    // push Association.value (slot 1).
    if (op >= SistaV1::PushLitVarBase && op <= SistaV1::PushLitVarLast) {
        int n = op - SistaV1::PushLitVarBase;
        a.mov(rax, ptr(rdi, OFF_LITERALS));
        a.mov(rax, ptr(rax, n * 8));                     // Association oop
        a.mov(rax, ptr(rax, OBJ_SLOT_0 + 8));            // slot[1] = value
        emitPushReg(a, rax);
        return true;
    }
    // pushTemp N: push tempBase[N].
    if (op >= 0x40 && op <= 0x4B) {
        int n = op - 0x40;
        a.mov(rax, ptr(rdi, OFF_TEMPBASE));
        a.mov(rax, ptr(rax, n * 8));
        emitPushReg(a, rax);
        return true;
    }
    // pushReceiver: push state.receiver.
    if (op == SistaV1::PushReceiver) {
        a.mov(rax, ptr(rdi, OFF_RECEIVER));
        emitPushReg(a, rax);
        return true;
    }
    // Dup: read sp[-1], push it (stack [..., v] → [..., v, v]).
    if (op == SistaV1::Dup) {
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.mov(rax, ptr(rcx, -8));
        emitPushReg(a, rax);
        return true;
    }
    // pushTrue: push state.trueOop.
    if (op == 0x4D) {
        a.mov(rax, ptr(rdi, OFF_TRUEOOP));
        emitPushReg(a, rax);
        return true;
    }
    // pushFalse: push state.falseOop.
    if (op == 0x4E) {
        a.mov(rax, ptr(rdi, OFF_FALSEOOP));
        emitPushReg(a, rax);
        return true;
    }
    // pushNil: push baked nil immediate.
    if (op == 0x4F) {
        a.mov(rax, asmjit::Imm(nilBits));
        emitPushReg(a, rax);
        return true;
    }
    // pushZero: push fromSmallInteger(0) = 1.
    if (op == 0x50) {
        a.mov(rax, asmjit::Imm(smiBits(0)));
        emitPushReg(a, rax);
        return true;
    }
    // pushOne: push fromSmallInteger(1) = 9.
    if (op == 0x51) {
        a.mov(rax, asmjit::Imm(smiBits(1)));
        emitPushReg(a, rax);
        return true;
    }
    // pop: state.sp--.
    if (op == SistaV1::Pop) {
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        return true;
    }
    // popStoreTemp N (0xD0..0xD7): pop TOS, store into tempBase[N].
    if (op >= SistaV1::PopStoreTempBase && op <= SistaV1::PopStoreTempLast) {
        int n = op - SistaV1::PopStoreTempBase;
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.mov(rax, ptr(rcx));
        a.mov(rdx, ptr(rdi, OFF_TEMPBASE));
        a.mov(ptr(rdx, n * 8), rax);
        return true;
    }
    // popStoreRecv N (0xC8..0xCF): pop TOS, store into receiver.slot[N].
    // Tests the receiver's immutable bit (header bit 23) before writing
    // and bails to interp if set — mirrors Interpreter::setReceiverInstVar,
    // which sends #attemptToAssign:withIndex: (Object's default raises
    // ModificationForbidden).  Without this check, JIT-compiled setters
    // bypassed the read-only enforcement (ObjectTest>>testBeRecursively-
    // ReadOnlyObject failed under default JIT).  Direct heap write
    // otherwise — no GC barrier (the stencil JIT writes inline too).
    if (op >= SistaV1::PopStoreRecvBase && op <= SistaV1::PopStoreRecvLast) {
        int n = op - SistaV1::PopStoreRecvBase;
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        a.mov(rdx, ptr(rdi, OFF_RECEIVER));
        // Test bit 23 of the 64-bit header.  test r/m32, imm32 reads
        // the low 4 bytes and ANDs against imm32; ImmutableBit lives
        // in that half so a 32-bit test is sufficient.
        a.test(dword_ptr(rdx),
               asmjit::Imm(static_cast<int32_t>(0x800000)));
        a.jne(bail);
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.mov(rax, ptr(rcx));
        a.mov(ptr(rdx, OBJ_SLOT_0 + n * 8), rax);
        a.jmp(end);
        a.bind(bail);
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();
        a.bind(end);
        return true;
    }
    // returnReceiver: returnValue = receiver; exitReason = ExitReturn; ret.
    if (op == SistaV1::ReturnReceiver) {
        a.mov(rax, ptr(rdi, OFF_RECEIVER));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    // returnTrue / returnFalse: similar with bake-via-state.
    if (op == 0x59) {
        a.mov(rax, ptr(rdi, OFF_TRUEOOP));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    if (op == 0x5A) {
        a.mov(rax, ptr(rdi, OFF_FALSEOOP));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    // returnNil: bake nil immediate.
    if (op == 0x5B) {
        a.mov(rax, asmjit::Imm(nilBits));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    // returnTop: pop into rax (sp--, then *sp), store as retVal.
    if (op == SistaV1::ReturnTop) {
        a.mov(rcx, ptr(rdi, OFF_SP));
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.mov(rax, ptr(rcx));
        a.mov(ptr(rdi, OFF_RETVAL), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_RETURN));
        a.ret();
        return true;
    }
    // Phase 3 arithmetic: 0x60..0x67 (+, -, <, >, <=, >=, =, ~=).
    // All share the same prologue (load operands + SmI check + bail
    // setup) and epilogue (write result, advance SP, fall through).
    if (isPhase3ArithOp(op)) {
        // Pattern (x86-64 SysV; rdi = state):
        //   rax = state.sp;  rcx = a = sp[-2];  rdx = b = sp[-1]
        //   r8 = a XOR 1;  r9 = b XOR 1;  r8 |= r9
        //   if (r8 & 7) goto bail
        //   sar rcx, 3;  sar rdx, 3   ; untag (signed)
        //   then op-specific body
        //   write result to sp[-2];  sp -= 8
        //   jmp end
        // bail:
        //   r8 = state.method + bcOffsetFromMethObj   ; ip-relative-to-method
        //   mov [rdi+OFF_IP], r8
        //   mov dword [rdi+OFF_EXIT], EXIT_ARITH_OVERFLOW
        //   ret
        // end:
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();

        a.mov(rax, ptr(rdi, OFF_SP));
        a.mov(rcx, ptr(rax, -16));   // a
        a.mov(rdx, ptr(rax, -8));    // b
        // SmI tag check (both SmI): 5 instructions, replaces the older
        // 7-instr XOR-pair-OR sequence.
        //   r8 = a^b   — low 3 = 0 iff same tag
        //   r9 = a-1   — low 3 = 0 iff a is SmI (tag 001)
        //   r8 |= r9   — combined; low 3 = 0 iff both SmI
        //   test low 8 bits against mask 7 — checks low 3 bits.
        // Correctness: both SmI <=> same tag AND a is SmI.  Verified
        // for SmI min/max + boundary cases.  Saves ~3 cycles per arith.
        a.mov(r8,  rcx);
        a.xor_(r8, rdx);
        a.lea(r9, asmjit::x86::ptr(rcx, -1));
        a.or_(r8, r9);
        a.test(r8.r8(), asmjit::Imm(7));
        a.jne(bail);

        if (op == 0x60) {            // +
            // Tagged-add trick: a_bits + b_bits = (a+b)*8 + 2.
            // jo catches SmI-range overflow directly (since tagged
            // form pre-shifts by 8, 64-bit overflow ⇔ SmI overflow).
            // Then sub 1 to convert (a+b)*8 + 2 → (a+b)*8 + 1 (tagged).
            // Saves 6 instructions vs the untag/add/retag pattern.
            a.add(rcx, rdx);
            a.jo(bail);
            a.sub(rcx, asmjit::Imm(1));
            a.mov(ptr(rax, -16), rcx);
            a.sub(rax, 8);
            a.mov(ptr(rdi, OFF_SP), rax);
        } else if (op == 0x61) {     // -
            // Tagged-sub: a_bits - b_bits = (a-b)*8.
            // jo catches SmI overflow (same reasoning as +).
            // Add 1 to convert (a-b)*8 → (a-b)*8 + 1 (tagged).
            a.sub(rcx, rdx);
            a.jo(bail);
            a.add(rcx, asmjit::Imm(1));
            a.mov(ptr(rax, -16), rcx);
            a.sub(rax, 8);
            a.mov(ptr(rdi, OFF_SP), rax);
        } else {
            // Comparison ops.  cmp tagged bits directly — the map
            // x → 8x+1 is monotonic for signed values, so cmp(a_bits,
            // b_bits) gives the same flags as cmp(a, b).  Saves 2× sar.
            // cmov takes a memory operand directly — saves a load.
            a.cmp(rcx, rdx);
            a.mov(rsi, ptr(rdi, OFF_FALSEOOP));   // default: false
            switch (op) {
                case 0x62: a.cmovl(rsi, ptr(rdi, OFF_TRUEOOP)); break;   // <
                case 0x63: a.cmovg(rsi, ptr(rdi, OFF_TRUEOOP)); break;   // >
                case 0x64: a.cmovle(rsi, ptr(rdi, OFF_TRUEOOP)); break;  // <=
                case 0x65: a.cmovge(rsi, ptr(rdi, OFF_TRUEOOP)); break;  // >=
                case 0x66: a.cmove(rsi, ptr(rdi, OFF_TRUEOOP)); break;   // =
                case 0x67: a.cmovne(rsi, ptr(rdi, OFF_TRUEOOP)); break;  // ~=
            }
            a.mov(ptr(rax, -16), rsi);
            a.sub(rax, 8);
            a.mov(ptr(rdi, OFF_SP), rax);
        }
        a.jmp(end);

        a.bind(bail);
        // r8 = state.method.rawBits + bcOffsetFromMethObj
        // (state.method is GC-tracked Oop; this is the post-GC-safe
        // address of the failing bytecode.)
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();

        a.bind(end);
        return true;
    }
    // Phase 3 bitwise: 0x6E bitAnd:, 0x6F bitOr:.  SmI tag bits (low 3
    // = 001) commute with bitwise AND/OR — a & b and a | b on tagged
    // SmIs produce a valid tagged SmI result without untag/retag.
    if (isPhase3BitOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        a.mov(rax, ptr(rdi, OFF_SP));
        a.mov(rcx, ptr(rax, -16));   // a
        a.mov(rdx, ptr(rax, -8));    // b
        // SmI check: (a^b) | (a-1) low 3 bits = 0 iff both SmI.
        a.mov(r8,  rcx);
        a.xor_(r8, rdx);
        a.lea(r9, asmjit::x86::ptr(rcx, -1));
        a.or_(r8, r9);
        a.test(r8.r8(), asmjit::Imm(7));
        a.jne(bail);
        if (op == 0x6E) {
            a.and_(rcx, rdx);    // bitAnd: tag bits stay 001
        } else {
            a.or_(rcx, rdx);     // bitOr: tag bits stay 001
        }
        a.mov(ptr(rax, -16), rcx);
        a.sub(rax, 8);
        a.mov(ptr(rdi, OFF_SP), rax);
        a.jmp(end);

        a.bind(bail);
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();

        a.bind(end);
        return true;
    }
    // Phase 3 multiplication: 0x68 *.
    if (isPhase3MulOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        a.mov(rax, ptr(rdi, OFF_SP));
        a.mov(rcx, ptr(rax, -16));   // a
        a.mov(rdx, ptr(rax, -8));    // b
        // SmI check: (a^b) | (a-1) low 3 bits = 0 iff both SmI.
        a.mov(r8,  rcx);
        a.xor_(r8, rdx);
        a.lea(r9, asmjit::x86::ptr(rcx, -1));
        a.or_(r8, r9);
        a.test(r8.r8(), asmjit::Imm(7));
        a.jne(bail);

        // Untag both (arithmetic shift to preserve sign).
        a.sar(rcx, asmjit::Imm(3));
        a.sar(rdx, asmjit::Imm(3));
        // imul rcx, rdx — signed multiply, sets OF on overflow.
        a.imul(rcx, rdx);
        a.jo(bail);
        // Retag: shift left 3 + or 1.  Three add+jo to catch the
        // case where the result fits in 64-bit signed (no jo on imul)
        // but not in 61-bit signed (would overflow on retag).
        a.add(rcx, rcx); a.jo(bail);
        a.add(rcx, rcx); a.jo(bail);
        a.add(rcx, rcx); a.jo(bail);
        a.or_(rcx, asmjit::Imm(SMI_TAG));
        a.mov(ptr(rax, -16), rcx);
        a.sub(rax, 8);
        a.mov(ptr(rdi, OFF_SP), rax);
        a.jmp(end);

        a.bind(bail);
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();

        a.bind(end);
        return true;
    }
    // Phase 3 bitShift: (0x6C).  Positive b → left shift with overflow
    // check; negative b → arithmetic right shift.  Matches interp's
    // bounds: b in [0,62] for left, b in [-63,-1] for right; else bail.
    if (isPhase3ShiftOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        asmjit::Label rightShift = a.new_label();
        a.mov(rax, ptr(rdi, OFF_SP));
        a.mov(rcx, ptr(rax, -16));   // a
        a.mov(rdx, ptr(rax, -8));    // b
        // SmI check: (a^b) | (a-1) low 3 bits = 0 iff both SmI.
        a.mov(r8,  rcx);
        a.xor_(r8, rdx);
        a.lea(r9, asmjit::x86::ptr(rcx, -1));
        a.or_(r8, r9);
        a.test(r8.r8(), asmjit::Imm(7));
        a.jne(bail);

        a.sar(rcx, asmjit::Imm(3));  // untag a
        a.sar(rdx, asmjit::Imm(3));  // untag b
        a.cmp(rdx, asmjit::Imm(0));
        a.jl(rightShift);            // b < 0 → right shift

        // Left shift: b in [0, 62].
        a.cmp(rdx, asmjit::Imm(63));
        a.jge(bail);
        // shl rcx, cl — but cl is rdx's low byte.  We need to move
        // shift count to cl.  rdx's low byte = dl (not cl).  Use the
        // movzx + use cl pattern instead.
        a.mov(r8, rcx);              // save original a in r8
        // x86 SHL with reg operand requires CL.  Temporarily save rcx
        // (= a, after untag), move shift count to cl via xchg with rdx.
        // Simpler: move rcx to r9 (untagged a), move dl to cl.
        a.mov(r9, rcx);              // r9 = a untagged
        a.mov(rcx, rdx);             // rcx = b untagged (now cl = b mod 256)
        a.shl(r9, asmjit::x86::cl);  // r9 = a << b
        // Overflow check: (r9 >> b) == a?
        a.mov(r10, r9);
        a.sar(r10, asmjit::x86::cl);
        a.cmp(r10, r8);
        a.jne(bail);
        a.mov(rcx, r9);              // result back to rcx
        // Retag with overflow checks.
        a.add(rcx, rcx); a.jo(bail);
        a.add(rcx, rcx); a.jo(bail);
        a.add(rcx, rcx); a.jo(bail);
        a.or_(rcx, asmjit::Imm(SMI_TAG));
        a.mov(ptr(rax, -16), rcx);
        a.sub(rax, 8);
        a.mov(ptr(rdi, OFF_SP), rax);
        a.jmp(end);

        a.bind(rightShift);
        // b in [-63, -1].  Negate to get shift count.
        a.neg(rdx);
        a.cmp(rdx, asmjit::Imm(63));
        a.jg(bail);
        a.mov(r9, rcx);              // r9 = a untagged
        a.mov(rcx, rdx);             // rcx = shift count (cl = -b)
        a.sar(r9, asmjit::x86::cl);  // arithmetic right shift
        a.mov(rcx, r9);
        // Retag: result is smaller than input, no overflow possible.
        a.shl(rcx, asmjit::Imm(3));
        a.or_(rcx, asmjit::Imm(SMI_TAG));
        a.mov(ptr(rax, -16), rcx);
        a.sub(rax, 8);
        a.mov(ptr(rdi, OFF_SP), rax);
        a.jmp(end);

        a.bind(bail);
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT),
              asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.ret();

        a.bind(end);
        return true;
    }
    // Short forward jumps (0xB0..0xC7).
    //   0xB0..0xB7 = unconditional
    //   0xB8..0xBF = jumpTrue  (pop; jump if was true)
    //   0xC0..0xC7 = jumpFalse (pop; jump if was false)
    if (op >= SistaV1::ShortJumpBase && op <= SistaV1::ShortJumpFalseLast) {
        int targetIdx = SistaV1::shortJumpTarget(op, globalIdx);
        if (op <= SistaV1::ShortJumpLast) {
            // Unconditional forward jump — safe even from inline-activated
            // contexts because no bail involved.
            a.jmp(bcLabels[targetIdx]);
            return true;
        }
        // Conditional jumps emit a mid-method ExitMustBool bail.  The
        // method gets canBailMidMethod=true in compileViaAsmjit so the
        // chain loop's inline-activate gate skips it (see
        // Interpreter.cpp:18731-18733).
        bool jumpOnTrue = (op >= SistaV1::ShortJumpTrueBase
                           && op <= SistaV1::ShortJumpTrueLast);
        asmjit::Label notBoolean = a.new_label();
        asmjit::Label fallThru   = a.new_label();

        a.mov(rcx, ptr(rdi, OFF_SP));
        a.mov(rax, ptr(rcx, -8));               // TOS (don't pop)

        // First cmp: against the "branch-taken" boolean.
        // For jumpTrue: branch when TOS == trueOop.
        // For jumpFalse: branch when TOS == falseOop.
        int takeBranchOop = jumpOnTrue ? OFF_TRUEOOP : OFF_FALSEOOP;
        int fallThruOop   = jumpOnTrue ? OFF_FALSEOOP : OFF_TRUEOOP;
        asmjit::Label takeBranch = a.new_label();
        a.cmp(rax, ptr(rdi, takeBranchOop));
        a.je(takeBranch);
        a.cmp(rax, ptr(rdi, fallThruOop));
        a.jne(notBoolean);

        // Was the fall-through boolean.  Pop and fall through.
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.jmp(fallThru);

        a.bind(takeBranch);
        // Was the take-branch boolean.  Pop and jump.
        a.sub(rcx, 8);
        a.mov(ptr(rdi, OFF_SP), rcx);
        a.jmp(bcLabels[targetIdx]);

        a.bind(notBoolean);
        // Don't pop — interp re-runs and sends mustBeBoolean.
        a.mov(r8, ptr(rdi, OFF_METHOD));
        a.add(r8, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), r8);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_MUST_BOOL));
        a.ret();

        a.bind(fallThru);
        return true;
    }
    if (isPhase4SendOp(op)) {
        int nArgs = sendNArgs(op);

        // Per-site icData address: jm->icBuffer + siteIdx*IC_BYTES_PER_SITE.
        a.mov(rdx, ptr(rdi, OFF_JITMETHOD));
        a.mov(rsi, ptr(rdx, (int)offsetof(JITMethod, icBuffer)));
        a.add(rsi, asmjit::Imm(siteIdx * (int)IC_BYTES_PER_SITE));

        // Deferred state setup: inline-spec paths (getter/setter/
        // returnsSelf) continue inline and don't need state.icDataPtr,
        // sendArgCount, or ip.  Only dispatchCached + miss paths return
        // to the chain loop; they emit the state stores at the end.
        // Saves 5 instructions per inline-spec HIT (the common case).

        // Selector-range bisect knob: PHARO_T1_IC_PROBE_MIN/MAX limit
        // which opcodes get the probe.  Default: all sends (0x70..0xAF).
        bool probeThis = g_debug.t1ICProbe;
        if (probeThis) {
            int icMin = g_debug.t1ICProbeMin;
            int icMax = g_debug.t1ICProbeMax;
            if (icMin >= 0 && (int)op < icMin) probeThis = false;
            if (icMax >= 0 && (int)op > icMax) probeThis = false;
        }
        if (probeThis) {
            // === Compute send-receiver's IC lookup key into rdx ===
            // The send's receiver is at stackValue(nArgs) = sp[-1-nArgs]
            // (NOT state.receiver, which is the *enclosing* method's self).
            // Object (tag=0):  classIndex = low 22 bits of header word.
            // Immediate:       key = (tag | 0x80000000) — matches
            //                  patchJITICAfterSend in Interpreter.cpp:16660.
            asmjit::Label imm = a.new_label();
            asmjit::Label haveKey = a.new_label();
            asmjit::Label miss = a.new_label();
            asmjit::Label dispatchCached = a.new_label();
            asmjit::Label tryGetter = a.new_label();
            asmjit::Label trySetter = a.new_label();
            asmjit::Label tryReturnsSelf = a.new_label();
            asmjit::Label endOfSend = a.new_label();

            a.mov(rcx, ptr(rdi, OFF_SP));
            int rcvrOffsetBytes = -8 * (nArgs + 1);
            a.mov(rax, ptr(rcx, rcvrOffsetBytes));   // rax = send receiver
            a.mov(rdx, rax);
            a.and_(rdx, asmjit::Imm(0x7));   // tag bits
            a.jnz(imm);
            // Object path: edx = header_low32 & 0x3FFFFF (classIndex).
            // 32-bit ops zero-extend into rdx, so the result is bounded.
            a.mov(edx, dword_ptr(rax));
            a.and_(edx, asmjit::Imm(0x3FFFFF));
            a.jmp(haveKey);
            a.bind(imm);
            a.or_(edx, asmjit::Imm(0x80000000));
            a.bind(haveKey);

            // === Probe icData[0] only (slot 0 monomorphic) ===
            a.cmp(rdx, ptr(rsi));
            a.jne(miss);
            a.cmp(rdx, asmjit::Imm(0));      // reject empty IC slot
            a.je(miss);
            if (g_debug.t1ProbeAlwaysMiss) {
                a.jmp(miss);                  // diagnostic: never take HIT
            }

            // === Examine extras (icData[2]) to choose dispatch ===
            // r8 = extras.  If 0 → plain dispatch via ExitSendCached.
            // Otherwise the patchJITICAfterSend classification chose an
            // inline-getter (bit 63), inline-setter (bit 62),
            // returnsSelf (bit 61), or one of the other flags we don't
            // inline yet (bit 60 J2J, 59 BLOCK_VALUE, 58 returnsLiteral,
            // 57 multi-slot, 48..52 primKind).  Unhandled extras →
            // dispatch the cached method and let the chain loop +
            // activated method do the work.
            //
            // ===== PERF TODO (deferred.md A6, 2026-05-17) =====
            // Bit 60 (J2J_ENTRY_BIT) inline-call is the largest unrealized
            // perf win — current arm64 fib(28) is 2× slower than interp
            // because every J2J hit round-trips JIT→C++→JIT via
            // activateMethod (~500 cycles per send overhead, ~80 ms over
            // fib's 514K recursive calls).  The legacy stencil JIT had this
            // inline at stencils.cpp:1733-1877 (`j2j_direct_call:` block);
            // see deferred.md A6 for the port plan and J2JSave protocol.
            // Same fix needed on both x86 and arm64 arms.
            // ====================================================
            a.mov(r8, qword_ptr(rsi, 16));
            a.test(r8, r8);
            a.jz(dispatchCached);

            // Inline specializations need a heap-class receiver
            // (the stencil's `tag == 0` gate). Immediates fall through
            // to plain cached dispatch.
            a.test(rax.r8(), asmjit::Imm(7));
            a.jnz(dispatchCached);

            if (g_debug.t1InlineGetter) {
                a.bt(r8, asmjit::Imm(63));
                a.jc(tryGetter);
            }
            if (g_debug.t1InlineSetter) {
                a.bt(r8, asmjit::Imm(62));
                a.jc(trySetter);
            }
            if (g_debug.t1InlineReturnsSelf) {
                a.bt(r8, asmjit::Imm(61));
                a.jc(tryReturnsSelf);
            }
            a.jmp(dispatchCached);

            // === Inline getter ===
            // val = receiver->slots[slotIdx]; sp[-1-nArgs] = val; sp -= nArgs.
            // For 0-arg getters (the common case) nArgs=0 → sp unchanged,
            // val replaces receiver at sp[-1].
            //
            // Note: rcx still holds SP from the probe entry (no clobber
            // through the probe path).  Use rdx for slotIdx so rcx stays
            // SP — saves 1 mov per inline-spec HIT.
            a.bind(tryGetter);
            a.mov(rdx, r8);
            a.and_(rdx, asmjit::Imm(0xFFFF));    // slotIdx in rdx
            a.mov(rdx, ptr(rax, rdx, 3, 8));     // load slot value into rdx
            a.mov(ptr(rcx, rcvrOffsetBytes), rdx);
            if (nArgs > 0) {
                a.sub(rcx, asmjit::Imm(8 * nArgs));
                a.mov(ptr(rdi, OFF_SP), rcx);
            }
            a.jmp(endOfSend);

            // === Inline setter ===
            // arg = sp[-1]; receiver->slots[slotIdx] = arg;
            // sp[-1-nArgs] = receiver (already true); sp -= nArgs.
            // No write barrier — the YG scavenge does a full old-space scan
            // (ObjectMemory.cpp:1538) so missed remembered-set updates are
            // tolerated.
            a.bind(trySetter);
            a.mov(rdx, r8);
            a.and_(rdx, asmjit::Imm(0xFFFF));    // slotIdx in rdx
            a.mov(r9, ptr(rcx, -8));             // arg = sp[-1] (rcx is SP)
            a.mov(ptr(rax, rdx, 3, 8), r9);      // recv->slot[slotIdx] = arg
            if (nArgs > 0) {
                a.sub(rcx, asmjit::Imm(8 * nArgs));
                a.mov(ptr(rdi, OFF_SP), rcx);
            }
            a.jmp(endOfSend);

            // === returnsSelf ===
            // Receiver is already at sp[-1-nArgs]; just pop nArgs and
            // it becomes TOS.  rcx still has SP from probe.
            a.bind(tryReturnsSelf);
            if (nArgs > 0) {
                a.sub(rcx, asmjit::Imm(8 * nArgs));
                a.mov(ptr(rdi, OFF_SP), rcx);
            }
            a.jmp(endOfSend);

            // === Plain cached dispatch (no inline opt) ===
            // Emit deferred state setup here (chain loop reads it).
            a.bind(dispatchCached);
            a.mov(ptr(rdi, OFF_ICDATAPTR), rsi);
            a.mov(dword_ptr(rdi, OFF_SENDARGCOUNT), asmjit::Imm(nArgs));
            a.mov(rax, ptr(rdi, OFF_METHOD));
            a.add(rax, asmjit::Imm(bcOffsetFromMethObj));
            a.mov(ptr(rdi, OFF_IP), rax);
            if (g_debug.t1HitAsMiss) {
                a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_SEND));
                a.ret();
            } else {
                a.mov(rax, ptr(rsi, 8));         // icData[1] = method Oop
                a.mov(ptr(rdi, OFF_CACHED_TARGET), rax);
                a.mov(dword_ptr(rdi, OFF_EXIT),
                      asmjit::Imm(EXIT_SEND_CACHED));
                a.ret();
            }

            // === Miss — chain loop does the IC fill ===
            // Emit deferred state setup here too.
            a.bind(miss);
            a.mov(ptr(rdi, OFF_ICDATAPTR), rsi);
            a.mov(dword_ptr(rdi, OFF_SENDARGCOUNT), asmjit::Imm(nArgs));
            a.mov(rax, ptr(rdi, OFF_METHOD));
            a.add(rax, asmjit::Imm(bcOffsetFromMethObj));
            a.mov(ptr(rdi, OFF_IP), rax);
            a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_SEND));
            a.ret();

            a.bind(endOfSend);
            return true;  // inline paths fall through to next bytecode
        }

        // Non-probe fallback: emit state setup + bail (chain loop will
        // do the full lookup).
        a.mov(ptr(rdi, OFF_ICDATAPTR), rsi);
        a.mov(dword_ptr(rdi, OFF_SENDARGCOUNT), asmjit::Imm(nArgs));
        a.mov(rax, ptr(rdi, OFF_METHOD));
        a.add(rax, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), rax);
        a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_SEND));
        a.ret();
        return true;
    }
    return false;  // pre-scan failed to filter — bug in allBytecodesSupported
}

#elif defined(__aarch64__) || defined(_M_ARM64)

// Equivalent ARM64 emitters.  Same stack discipline; uses w/x regs.
//   x0 = state ptr (input, preserved)
//   x1 = scratch value
//   x2 = scratch sp
//   w3 = scratch int (for exitReason store)

void emitPushReg(asmjit::a64::Assembler& a, asmjit::a64::Gp valReg) {
    using namespace asmjit::a64;
    a.ldr(x2, ptr(x0, OFF_SP));
    a.str(valReg, ptr(x2));
    a.add(x2, x2, asmjit::Imm(8));
    a.str(x2, ptr(x0, OFF_SP));
}

// ARM64 mirror of emitPrimProlog_x86.  See that function for context.
void emitPrimProlog_arm64(asmjit::a64::Assembler& a, int primIndex) {
    using namespace asmjit::a64;

    if (primIndex == 110) {
        // #== : compare raw oop bits.
        a.ldr(x1, ptr(x0, OFF_RECEIVER));
        a.ldr(x2, ptr(x0, OFF_TEMPBASE));
        a.ldr(x2, ptr(x2));
        // ldp loads two adjacent 8-byte slots in one instruction; TRUEOOP
        // (offset 128) and FALSEOOP (offset 136) are intentionally
        // adjacent in JITState.  Saves 1 ldr per #== prim.
        a.ldp(x6, x7, ptr(x0, OFF_TRUEOOP));
        a.cmp(x1, x2);
        a.csel(x1, x6, x7, CondCode::kEQ);
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        return;
    }

    asmjit::Label fail = a.new_label();

    a.ldr(x1, ptr(x0, OFF_RECEIVER));

    // === Heap-receiver primitives (no SmI check) — at: / at:put: / size ===
    if (primIndex == 60 || primIndex == 61 || primIndex == 62) {
        // x1 = receiver (must be heap pointer).
        a.tst(x1, asmjit::Imm(7));
        a.b_ne(fail);
        // Load header word.
        a.ldr(x4, ptr(x1));
        // fmt = (hdr >> 24) & 0x1F
        a.lsr(x6, x4, asmjit::Imm(24));
        a.and_(x6, x6, asmjit::Imm(0x1F));
        // slotCount = (hdr >> 56) & 0xFF, bail on overflow (255).
        a.lsr(x5, x4, asmjit::Imm(56));
        a.and_(x5, x5, asmjit::Imm(0xFF));
        a.cmp(x5, asmjit::Imm(255));
        a.b_eq(fail);

        if (primIndex == 62) {
            // size — fmt 2 (Array), fmt 10-11 (32-bit WordArray), or 16-23 (byte).
            asmjit::Label tryWords = a.new_label();
            asmjit::Label tryBytes = a.new_label();
            a.cmp(x6, asmjit::Imm(2));
            a.b_ne(tryWords);
            // Array: result = slotCount tagged.
            a.lsl(x5, x5, asmjit::Imm(3));
            a.orr(x5, x5, asmjit::Imm(1));
            a.str(x5, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            a.bind(tryWords);
            // fmt 10-11: 32-bit indexable (WordArray, IntegerArray).
            // numElements = slotCount*2 - (fmt - 10).
            a.sub(x8, x6, asmjit::Imm(10));
            a.cmp(x8, asmjit::Imm(2));
            a.b_hs(tryBytes);
            a.lsl(x5, x5, asmjit::Imm(1));         // sc*2
            a.sub(x5, x5, x8);                       // numElements
            a.lsl(x5, x5, asmjit::Imm(3));
            a.orr(x5, x5, asmjit::Imm(1));
            a.str(x5, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            a.bind(tryBytes);
            // fmt - 16 in [0..7] → byte indexable.
            a.sub(x8, x6, asmjit::Imm(16));
            a.cmp(x8, asmjit::Imm(8));
            a.b_hs(fail);
            // byteSize = slotCount*8 - (fmt & 7)
            a.lsl(x5, x5, asmjit::Imm(3));
            a.and_(x6, x6, asmjit::Imm(0x7));
            a.sub(x5, x5, x6);
            // Tag as SmI.
            a.lsl(x5, x5, asmjit::Imm(3));
            a.orr(x5, x5, asmjit::Imm(1));
            a.str(x5, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            a.bind(fail);
            return;
        }

        // prim 60 / 61: load idx (tagged) from first temp.
        a.ldr(x2, ptr(x0, OFF_TEMPBASE));
        a.ldr(x3, ptr(x2));                  // x3 = idx
        a.and_(x7, x3, asmjit::Imm(0x7));
        a.cmp(x7, asmjit::Imm(1));
        a.b_ne(fail);                         // idx not SmI

        if (primIndex == 60) {
            // at: — fmt 2 (Array), fmt 10-11 (32-bit WordArray), or 16-23 (byte).
            asmjit::Label tryWordsAt = a.new_label();
            asmjit::Label tryBytesAt = a.new_label();
            a.cmp(x6, asmjit::Imm(2));
            a.b_ne(tryWordsAt);
            a.asr(x4, x3, asmjit::Imm(3));   // idx untagged
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x5);
            a.b_gt(fail);
            // recv[idx*8] = slot at index idx-1 (offset 8 + (idx-1)*8 = idx*8).
            a.lsl(x4, x4, asmjit::Imm(3));
            a.add(x6, x1, x4);
            a.ldr(x4, ptr(x6));
            a.str(x4, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            a.bind(tryWordsAt);
            // fmt 10-11: 32-bit indexable (WordArray, IntegerArray).
            // numElements = slotCount*2 - (fmt - 10).  Load uint32, tag SmI.
            a.sub(x8, x6, asmjit::Imm(10));
            a.cmp(x8, asmjit::Imm(2));
            a.b_hs(tryBytesAt);
            a.lsl(x9, x5, asmjit::Imm(1));         // sc*2
            a.sub(x9, x9, x8);                      // numElements
            a.asr(x4, x3, asmjit::Imm(3));          // idx
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x9);
            a.b_gt(fail);
            // 32-bit slot at recv + 4 + idx*4 (slot[0]=offset 8, slot[idx-1]=offset 8+(idx-1)*4).
            a.lsl(x4, x4, asmjit::Imm(2));
            a.add(x6, x1, x4);
            a.ldur(w7, ptr(x6, 4));
            // Tag as SmI: ((uint64)w7 << 3) | 1.
            a.lsl(x7, x7, asmjit::Imm(3));
            a.orr(x7, x7, asmjit::Imm(1));
            a.str(x7, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            a.bind(tryBytesAt);
            a.sub(x8, x6, asmjit::Imm(16));
            a.cmp(x8, asmjit::Imm(8));
            a.b_hs(fail);
            // byteSize = slotCount*8 - (fmt & 7)
            a.lsl(x5, x5, asmjit::Imm(3));
            a.and_(x9, x6, asmjit::Imm(0x7));
            a.sub(x5, x5, x9);
            a.asr(x4, x3, asmjit::Imm(3));
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x5);
            a.b_gt(fail);
            // load byte at recv + 7 + idx (because slot[0] at +8, idx 1 = +8 = recv+7+1).
            a.add(x4, x4, asmjit::Imm(7));
            a.add(x6, x1, x4);
            a.ldrb(w7, ptr(x6));
            // Tag as SmI.
            a.lsl(x7, x7, asmjit::Imm(3));
            a.orr(x7, x7, asmjit::Imm(1));
            a.str(x7, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            a.bind(fail);
            return;
        }

        // primIndex == 61: at:put:.  Two args: idx (x3), val (load from temp+8).
        if (primIndex == 61) {
            // Immutability check (bit 23).
            a.tbnz(x4, asmjit::Imm(23), fail);
            a.ldr(x9, ptr(x2, 8));            // x9 = val (tagged)
            asmjit::Label tryWordsAtPut = a.new_label();
            asmjit::Label tryBytesAtPut = a.new_label();
            a.cmp(x6, asmjit::Imm(2));
            a.b_ne(tryWordsAtPut);
            // Array path.
            a.asr(x4, x3, asmjit::Imm(3));
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x5);
            a.b_gt(fail);
            a.lsl(x4, x4, asmjit::Imm(3));
            a.add(x6, x1, x4);
            a.str(x9, ptr(x6));               // store value at slot
            a.str(x9, ptr(x0, OFF_RETVAL));   // return value
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            a.bind(tryWordsAtPut);
            // fmt 10-11: 32-bit indexable (WordArray, IntegerArray).
            // val must be SmI in [0, 0xFFFFFFFF].
            a.sub(x8, x6, asmjit::Imm(10));
            a.cmp(x8, asmjit::Imm(2));
            a.b_hs(tryBytesAtPut);
            // val SmI tag check.
            a.and_(x7, x9, asmjit::Imm(0x7));
            a.cmp(x7, asmjit::Imm(1));
            a.b_ne(fail);
            a.asr(x7, x9, asmjit::Imm(3));    // val untagged (signed)
            // val < 0 → fail; val > 0xFFFFFFFF → fail.
            a.cmp(x7, asmjit::Imm(0));
            a.b_lt(fail);
            a.mov(x10, asmjit::Imm(0xFFFFFFFFULL));
            a.cmp(x7, x10);
            a.b_hi(fail);
            // numElements = slotCount*2 - (fmt - 10).
            a.lsl(x10, x5, asmjit::Imm(1));   // sc*2
            a.sub(x10, x10, x8);              // numElements
            a.asr(x4, x3, asmjit::Imm(3));    // idx
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x10);
            a.b_gt(fail);
            // Store uint32 at recv + 4 + idx*4.
            a.lsl(x4, x4, asmjit::Imm(2));
            a.add(x6, x1, x4);
            a.stur(w7, ptr(x6, 4));
            a.str(x9, ptr(x0, OFF_RETVAL));   // return value
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            a.bind(tryBytesAtPut);
            a.sub(x8, x6, asmjit::Imm(16));
            a.cmp(x8, asmjit::Imm(8));
            a.b_hs(fail);
            // val must be SmI in 0..255.
            a.and_(x7, x9, asmjit::Imm(0x7));
            a.cmp(x7, asmjit::Imm(1));
            a.b_ne(fail);
            a.asr(x7, x9, asmjit::Imm(3));
            a.cmp(x7, asmjit::Imm(255));
            a.b_hi(fail);
            // byteSize = slotCount*8 - (fmt & 7)
            a.lsl(x5, x5, asmjit::Imm(3));
            a.and_(x10, x6, asmjit::Imm(0x7));
            a.sub(x5, x5, x10);
            a.asr(x4, x3, asmjit::Imm(3));
            a.cmp(x4, asmjit::Imm(1));
            a.b_lt(fail);
            a.cmp(x4, x5);
            a.b_gt(fail);
            a.add(x4, x4, asmjit::Imm(7));
            a.add(x6, x1, x4);
            a.strb(w7, ptr(x6));
            a.str(x9, ptr(x0, OFF_RETVAL));
            a.mov(w3, asmjit::Imm(EXIT_RETURN));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            a.bind(fail);
            return;
        }
    }

    a.ldr(x2, ptr(x0, OFF_TEMPBASE));
    a.ldr(x2, ptr(x2));

    // === SmallFloat binary arith (prim 541/542/549) ===
    // Both operands must be SmallFloat (tag = 5).  Decode, op, encode.
    // Bail on ±0 receiver/arg (uncommon, complex encoding) and on
    // exponent under/overflow of the result.
    if (primIndex == 541 || primIndex == 542 || primIndex == 549) {
        // Tag-check both operands == 5.
        a.and_(x4, x1, asmjit::Imm(0x7));
        a.cmp(x4, asmjit::Imm(5));
        a.b_ne(fail);
        a.and_(x4, x2, asmjit::Imm(0x7));
        a.cmp(x4, asmjit::Imm(5));
        a.b_ne(fail);

        // Decode receiver.
        a.lsr(x4, x1, asmjit::Imm(3));
        a.cmp(x4, asmjit::Imm(1));
        a.b_ls(fail);                        // ±0 → bail
        a.mov(x5, asmjit::Imm(0x7000000000000000ULL));
        a.add(x4, x4, x5);
        a.ror(x4, x4, asmjit::Imm(1));       // doubleBits
        a.fmov(d0, x4);

        // Decode arg (x5 still holds offset).
        a.lsr(x4, x2, asmjit::Imm(3));
        a.cmp(x4, asmjit::Imm(1));
        a.b_ls(fail);
        a.add(x4, x4, x5);
        a.ror(x4, x4, asmjit::Imm(1));
        a.fmov(d1, x4);

        switch (primIndex) {
            case 541: a.fadd(d0, d0, d1); break;
            case 542: a.fsub(d0, d0, d1); break;
            case 549: a.fmul(d0, d0, d1); break;
        }

        // Encode result.
        a.fmov(x4, d0);
        // Special NaN/inf: exponent bits all 1 → high 11 bits after sign-LSB
        // rotation form a value ≥ 0xFFE0_0000_0000_0000 in the rotated form.
        // Range check after subtract catches these.
        a.ror(x4, x4, asmjit::Imm(63));      // ROL by 1 (sign bit → LSB)
        a.cmp(x4, x5);
        a.b_lo(fail);                         // exp underflow
        a.sub(x4, x4, x5);
        a.mov(x6, asmjit::Imm(0x1FFFFFFFFFFFFFFFULL));
        a.cmp(x4, x6);
        a.b_hi(fail);                         // exp overflow
        a.lsl(x4, x4, asmjit::Imm(3));
        a.orr(x4, x4, asmjit::Imm(5));        // SmallFloatTag
        a.str(x4, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(fail);
        return;
    }

    // SmI check (5 ops instead of 7): (a^b) | (a-1), low 3 bits = 0
    // iff same tag AND a is SmI.  Mirrors emitPrimProlog_x86.
    a.eor(x5, x1, x2);
    a.sub(x6, x1, asmjit::Imm(1));
    a.orr(x5, x5, x6);
    a.tst(x5, asmjit::Imm(7));
    a.b_ne(fail);

    if (primIndex >= 3 && primIndex <= 8) {
        // Compare tagged bits directly — x → 8x+1 is monotonic for
        // signed values, so cmp(a_bits, b_bits) matches cmp(a, b).
        // (Mirrors emitPrimProlog_x86 — skips the untag step.)
        // ldp loads both adjacent oop slots in one instruction.
        a.ldp(x6, x7, ptr(x0, OFF_TRUEOOP));
        a.cmp(x1, x2);
        CondCode cc = CondCode::kEQ;  // default; overridden below
        switch (primIndex) {
            case 3: cc = CondCode::kLT; break;  // signed <
            case 4: cc = CondCode::kGT; break;  // signed >
            case 5: cc = CondCode::kLE; break;  // signed <=
            case 6: cc = CondCode::kGE; break;  // signed >=
            case 7: cc = CondCode::kEQ; break;  // =
            case 8: cc = CondCode::kNE; break;  // ~=
        }
        a.csel(x1, x6, x7, cc);
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(fail);
        return;
    }

    if (primIndex == 14 || primIndex == 15 || primIndex == 16) {
        // bitAnd / bitOr / bitXor on tagged bits — see x86 version.
        if (primIndex == 14) a.and_(x1, x1, x2);
        else if (primIndex == 15) a.orr (x1, x1, x2);
        else {
            a.eor(x1, x1, x2);
            a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        }
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(fail);
        return;
    }

    // Tagged-arith for + and - : skip untag/retag.  See x86 prim
    // prologue for derivation.  jo on the tagged add/sub catches
    // SmI-range overflow because the tagged form is (val<<3)|1.
    if (primIndex == 1) {
        // a_bits + b_bits = (a+b)*8 + 2 → sub 1 to retag.
        a.adds(x1, x1, x2);
        a.b_vs(fail);
        a.sub(x1, x1, asmjit::Imm(1));
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(fail);
        return;
    }
    if (primIndex == 2) {
        // a_bits - b_bits = (a-b)*8 → add 1 to retag.
        a.subs(x1, x1, x2);
        a.b_vs(fail);
        a.add(x1, x1, asmjit::Imm(1));
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(fail);
        return;
    }

    // prim 9 (*): untag, multiply, check overflow, retag.
    a.asr(x1, x1, asmjit::Imm(3));
    a.asr(x2, x2, asmjit::Imm(3));
    if (primIndex == 9) {
        // mul: detect 64-bit overflow via smulh (high 64 bits of 128-bit
        // signed product) — must equal sign-extension of low 64 bits.
        a.mul(x6, x1, x2);
        a.smulh(x7, x1, x2);
        a.cmp(x7, x6, asmjit::a64::asr(63));
        a.b_ne(fail);
        // Also confirm fits in 61-bit SmI range.
        a.mov(x1, x6);
        a.lsl(x4, x1, asmjit::Imm(3));
        a.asr(x4, x4, asmjit::Imm(3));
        a.cmp(x4, x1);
        a.b_ne(fail);
    } else {
        a.bind(fail);
        return;
    }
    a.lsl(x1, x1, asmjit::Imm(3));
    a.orr(x1, x1, asmjit::Imm(SMI_TAG));
    a.str(x1, ptr(x0, OFF_RETVAL));
    a.mov(w3, asmjit::Imm(EXIT_RETURN));
    a.str(w3, ptr(x0, OFF_EXIT));
    a.ret(x30);

    a.bind(fail);
}

bool emitOne_arm64(asmjit::a64::Assembler& a, uint8_t op,
                    uint64_t nilBits, int bcOffsetFromMethObj,
                    int siteIdx,
                    const std::vector<asmjit::Label>& bcLabels,
                    int globalIdx,
                    int callerArgCount, int callerTempCount) {
    using namespace asmjit::a64;
    (void)bcOffsetFromMethObj;
    (void)siteIdx;
    (void)callerArgCount;

    if (op <= 0x0F) {
        int n = op & 0x0F;
        a.ldr(x1, ptr(x0, OFF_RECEIVER));
        a.ldr(x1, ptr(x1, OBJ_SLOT_0 + n * 8));
        emitPushReg(a, x1);
        return true;
    }
    if (op >= 0x20 && op <= 0x3F) {
        int n = op - 0x20;
        a.ldr(x1, ptr(x0, OFF_LITERALS));
        a.ldr(x1, ptr(x1, n * 8));
        emitPushReg(a, x1);
        return true;
    }
    // pushLitVar N (0x10..0x1F): push literals[N].value (Association.slot[1]).
    if (op >= SistaV1::PushLitVarBase && op <= SistaV1::PushLitVarLast) {
        int n = op - SistaV1::PushLitVarBase;
        a.ldr(x1, ptr(x0, OFF_LITERALS));
        a.ldr(x1, ptr(x1, n * 8));
        a.ldr(x1, ptr(x1, OBJ_SLOT_0 + 8));
        emitPushReg(a, x1);
        return true;
    }
    if (op >= 0x40 && op <= 0x4B) {
        int n = op - 0x40;
        a.ldr(x1, ptr(x0, OFF_TEMPBASE));
        a.ldr(x1, ptr(x1, n * 8));
        emitPushReg(a, x1);
        return true;
    }
    if (op == SistaV1::PushReceiver) {
        a.ldr(x1, ptr(x0, OFF_RECEIVER));
        emitPushReg(a, x1);
        return true;
    }
    if (op == 0x4D) { a.ldr(x1, ptr(x0, OFF_TRUEOOP));  emitPushReg(a, x1); return true; }
    if (op == 0x4E) { a.ldr(x1, ptr(x0, OFF_FALSEOOP)); emitPushReg(a, x1); return true; }
    if (op == 0x4F) { a.mov(x1, asmjit::Imm(nilBits));  emitPushReg(a, x1); return true; }
    if (op == 0x50) { a.mov(x1, asmjit::Imm(smiBits(0))); emitPushReg(a, x1); return true; }
    if (op == 0x51) { a.mov(x1, asmjit::Imm(smiBits(1))); emitPushReg(a, x1); return true; }
    if (op == SistaV1::Pop) {
        a.ldr(x2, ptr(x0, OFF_SP));
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        return true;
    }
    // Dup: read sp[-1], push it.
    if (op == SistaV1::Dup) {
        a.ldr(x2, ptr(x0, OFF_SP));
        a.ldur(x1, ptr(x2, -8));
        emitPushReg(a, x1);
        return true;
    }
    // PushThisContext 0x52: bail to interp.  Materializing thisContext
    // requires walking the inline frame stack (Interpreter.cpp
    // case jit::SistaV1::PushThisContext) — too complex to emit
    // inline.  Reuse EXIT_ARITH_OVERFLOW: chain-loop handler syncs
    // ip+sp and returns to interp's main loop, which executes the
    // 0x52 normally and runs the rest of the method in interp.
    if (op == SistaV1::PushThisContext) {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        return true;
    }
    // BlockReturnNil 0x5D / BlockReturnTop 0x5E: bail to interp.
    // Block return involves frame walking, enclosingLevels (extA_),
    // and jumpDist (extB_) — too complex to emit inline.  Interp
    // handles it correctly; subsequent JIT code is unreachable
    // anyway since block returns terminate the block body.
    if (op == SistaV1::BlockReturnNil || op == SistaV1::BlockReturnTop) {
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        return true;
    }
    // popStoreTemp N (0xD0..0xD7): pop TOS, store into tempBase[N].
    if (op >= SistaV1::PopStoreTempBase && op <= SistaV1::PopStoreTempLast) {
        int n = op - SistaV1::PopStoreTempBase;
        a.ldr(x2, ptr(x0, OFF_SP));
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.ldr(x1, ptr(x2));
        a.ldr(x4, ptr(x0, OFF_TEMPBASE));
        a.str(x1, ptr(x4, n * 8));
        return true;
    }
    // popStoreRecv N (0xC8..0xCF): pop TOS, store into receiver.slot[N].
    // Tests the immutable bit (header bit 23) before writing and bails
    // to interp if set — see the x86 version above for full rationale.
    if (op >= SistaV1::PopStoreRecvBase && op <= SistaV1::PopStoreRecvLast) {
        int n = op - SistaV1::PopStoreRecvBase;
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        a.ldr(x4, ptr(x0, OFF_RECEIVER));
        a.ldr(w5, ptr(x4));                 // low 32 bits of header
        a.tst(w5, asmjit::Imm(0x800000));   // bit 23 = ImmutableBit
        a.b_ne(bail);
        a.ldr(x2, ptr(x0, OFF_SP));
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.ldr(x1, ptr(x2));
        a.str(x1, ptr(x4, OBJ_SLOT_0 + n * 8));
        a.b(end);
        a.bind(bail);
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // J2J chain return prelude — emitted before each return path.
    // If state.j2jDepth > 0: pop the most recent save, restore caller
    // state, push retval to caller's sp, tail-call to save->resumeAddr.
    // Otherwise: fall through to normal exit-to-C++ return.
    //
    // Mirrors stencils.cpp:491-531 J2J_INLINE_RETURN_IMPL protocol.
    // Retval is in x1 by convention; x2-x15 free to clobber.
    //
    // Default ON since 2026-05-17 (PHARO_T1_NO_INLINE_J2J=1 opt-out).
    // When off, the ldr+cbz is still emitted but j2jDepth is always 0
    // so we fall through to normal-return — tiny overhead, zero
    // semantic change.
    const bool inlineJ2J = g_debug.t1InlineJ2J;
    auto emitJ2JReturnPreludeIfEnabled = [&]() {
        if (!inlineJ2J) return;
        asmjit::Label normalReturn = a.new_label();
        // Check current j2jDepth > j2jEntryDepth (this method pushed a save).
        // Without entry-depth gate, chain-loop-activated methods would
        // incorrectly pop OUTER inline-J2J saves.  See deferred A6 option (a).
        a.ldr(w3, ptr(x0, OFF_J2J_DEPTH));
        a.ldr(w4, ptr(x0, OFF_J2J_ENTRY_DEPTH));
        a.cmp(w3, w4);
        a.b_le(normalReturn);   // current <= entry → no save pushed by this method

        // Pop save: cursor -= sizeof(J2JSave) = 56; depth--
        a.ldr(x4, ptr(x0, OFF_J2J_SAVE_CURSOR));
        a.sub(x4, x4, asmjit::Imm(56));
        a.str(x4, ptr(x0, OFF_J2J_SAVE_CURSOR));
        a.sub(w3, w3, asmjit::Imm(1));
        a.str(w3, ptr(x0, OFF_J2J_DEPTH));

        // Load save fields with ldp pairs.  Layout (stencils.cpp:113):
        //   [0]=sp, [8]=receiver, [16]=tempBase, [24]=ip,
        //   [32]=jitMethod, [40]=resumeAddr, [48]=sendArgCount
        a.ldp(x5, x6, ptr(x4,  0));   // sp + receiver
        a.str(x6, ptr(x0, OFF_RECEIVER));
        a.ldp(x6, x10, ptr(x4, 16));  // tempBase + ip
        a.str(x6, ptr(x0, OFF_TEMPBASE));
        a.str(x10, ptr(x0, OFF_IP));
        a.ldr(x8, ptr(x4, 40));       // resumeAddr (always needed)
        a.ldr(w9, ptr(x4, 48));       // sendArgCount

        // Derive method/literals/argCount/jitMethod from callerJM.
        // When xmethod is OFF (default), the J2J push path is strictly
        // self-recursive (callee == caller — gated by SELF_REC_BIT
        // tbz), so state.method, state.literals, state.argCount, and
        // state.jitMethod were never modified during the J2J call —
        // skip the 5 redundant stores.  When xmethod is ON, the
        // cross-method update path may have changed those fields, so
        // restore from save.
        if (g_debug.t1InlineJ2JXmethod) {
            a.ldr(x7, ptr(x4, 32));   // jitMethod
            a.str(x7, ptr(x0, OFF_JITMETHOD));
            a.ldr(x6, ptr(x7, 0));    // method = callerJM[0]
            a.str(x6, ptr(x0, OFF_METHOD));
            a.add(x10, x6, asmjit::Imm(16));
            a.str(x10, ptr(x0, OFF_LITERALS));
            a.ldrb(w11, ptr(x7, 34)); // callerJM.argCount byte
            a.str(w11, ptr(x0, OFF_ARGCOUNT));
        }

        // Pop callee's args from caller's sp, push retval (in x1).
        // semantics: *(sp - (nArgs+1)*8) = retval; sp -= nArgs*8
        a.lsl(x12, x9, asmjit::Imm(3));            // x12 = nArgs*8
        a.add(x13, x12, asmjit::Imm(8));           // x13 = (nArgs+1)*8
        a.sub(x14, x5, x13);                        // x14 = sp - (nArgs+1)*8
        a.str(x1, ptr(x14));                        // write retval
        a.sub(x5, x5, x12);                         // sp_new = sp - nArgs*8
        a.str(x5, ptr(x0, OFF_SP));

        // Clear exitReason so callers don't see stale EXIT_RETURN.
        a.mov(w15, asmjit::Imm(0));
        a.str(w15, ptr(x0, OFF_EXIT));

        // Tail-call to caller's resumeAddr.
        a.br(x8);

        a.bind(normalReturn);
    };

    auto emitReturnPtr = [&](int srcOff) {
        a.ldr(x1, ptr(x0, srcOff));
        emitJ2JReturnPreludeIfEnabled();
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
    };
    auto emitReturnImm = [&](uint64_t imm) {
        a.mov(x1, asmjit::Imm(imm));
        emitJ2JReturnPreludeIfEnabled();
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
    };
    if (op == SistaV1::ReturnReceiver) { emitReturnPtr(OFF_RECEIVER); return true; }
    if (op == 0x59) { emitReturnPtr(OFF_TRUEOOP);  return true; }
    if (op == 0x5A) { emitReturnPtr(OFF_FALSEOOP); return true; }
    if (op == 0x5B) { emitReturnImm(nilBits);      return true; }
    if (op == SistaV1::ReturnTop) {
        a.ldr(x2, ptr(x0, OFF_SP));
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.ldr(x1, ptr(x2));      // x1 = retval (TOS)
        emitJ2JReturnPreludeIfEnabled();
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        return true;
    }
    // Phase 3 arithmetic on ARM64 (mirror of x86, with tagged-arith).
    //   x2 = sp;  x1 = a (sp[-2]);  x4 = b (sp[-1])
    //   5-op SmI check: (a^b) | (a-1)
    //   + / - : tagged-add/sub directly (b_vs catches SmI overflow
    //           since tagged form pre-shifts by 8); then sub/add 1.
    //   compares: cmp tagged bits directly (monotonic transform).
    if (isPhase3ArithOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();

        a.ldr(x2, ptr(x0, OFF_SP));
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        // SmI tag check (5 ops): (a^b) | (a-1) low 3 bits = 0 iff
        // both SmI AND same tag.
        a.eor(x5, x1, x4);
        a.sub(x6, x1, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);

        if (op == 0x60) {        // +
            // a_bits + b_bits = (a+b)*8 + 2 → sub 1 to retag.
            a.adds(x1, x1, x4);
            a.b_vs(bail);
            a.sub(x1, x1, asmjit::Imm(1));
            a.str(x1, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            a.str(x2, ptr(x0, OFF_SP));
        } else if (op == 0x61) { // -
            // a_bits - b_bits = (a-b)*8 → add 1 to retag.
            a.subs(x1, x1, x4);
            a.b_vs(bail);
            a.add(x1, x1, asmjit::Imm(1));
            a.str(x1, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            a.str(x2, ptr(x0, OFF_SP));
        } else {
            // Comparisons: csel false/true on signed flags.  Compare
            // tagged bits directly — monotonic transform preserves
            // ordering (mirrors x86 skip-untag-for-compare opt).
            a.cmp(x1, x4);
            // ldp loads both oops in one instruction (x6=TRUEOOP at +128,
            // x5=FALSEOOP at +136).  Saves 1 ldr per inline comparison
            // bytecode (0x62-0x67).
            a.ldp(x6, x5, ptr(x0, OFF_TRUEOOP));
            switch (op) {
                case 0x62: a.csel(x5, x6, x5, CondCode::kLT); break;
                case 0x63: a.csel(x5, x6, x5, CondCode::kGT); break;
                case 0x64: a.csel(x5, x6, x5, CondCode::kLE); break;
                case 0x65: a.csel(x5, x6, x5, CondCode::kGE); break;
                case 0x66: a.csel(x5, x6, x5, CondCode::kEQ); break;
                case 0x67: a.csel(x5, x6, x5, CondCode::kNE); break;
            }
            a.str(x5, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            a.str(x2, ptr(x0, OFF_SP));
        }
        a.b(end);

        a.bind(bail);
        // Counter: how often the SmI fast path bails (for any reason).
        if (op == 0x60 || op == 0x61) {
            a.mov(x14, asmjit::Imm((uint64_t)&g_bcArithBail_hits));
            a.ldr(x15, ptr(x14));
            a.add(x15, x15, asmjit::Imm(1));
            a.str(x15, ptr(x14));
        }
        // Inline SmallFloat + / - at the bytecode level (op 0x60 / 0x61).
        // Reached when the SmI fast-path fails.  Both operands must have
        // tag 5 and non-zero shifted bits (i.e. not ±0).
        if (op == 0x60 || op == 0x61) {
            using namespace asmjit::a64;
            asmjit::Label notFloat = a.new_label();
            a.and_(x5, x1, asmjit::Imm(0x7));
            a.cmp(x5, asmjit::Imm(5));
            a.b_ne(notFloat);
            a.and_(x5, x4, asmjit::Imm(0x7));
            a.cmp(x5, asmjit::Imm(5));
            a.b_ne(notFloat);
            // Counter increment (every successful tag check).
            {
                a.mov(x14, asmjit::Imm((uint64_t)&g_bcFloatArith_hits));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
            }
            // Decode rcv to d0.
            a.lsr(x5, x1, asmjit::Imm(3));
            a.cmp(x5, asmjit::Imm(1));
            a.b_ls(notFloat);                 // ±0 → bail
            a.mov(x6, asmjit::Imm(0x7000000000000000ULL));
            a.add(x5, x5, x6);
            a.ror(x5, x5, asmjit::Imm(1));
            a.fmov(d0, x5);
            // Decode arg to d1 (x6 still has the offset).
            a.lsr(x5, x4, asmjit::Imm(3));
            a.cmp(x5, asmjit::Imm(1));
            a.b_ls(notFloat);
            a.add(x5, x5, x6);
            a.ror(x5, x5, asmjit::Imm(1));
            a.fmov(d1, x5);
            if (op == 0x60) a.fadd(d0, d0, d1);
            else            a.fsub(d0, d0, d1);
            // Encode.
            a.fmov(x5, d0);
            a.ror(x5, x5, asmjit::Imm(63));
            a.cmp(x5, x6);
            a.b_lo(notFloat);
            a.sub(x5, x5, x6);
            a.mov(x7, asmjit::Imm(0x1FFFFFFFFFFFFFFFULL));
            a.cmp(x5, x7);
            a.b_hi(notFloat);
            a.lsl(x5, x5, asmjit::Imm(3));
            a.orr(x5, x5, asmjit::Imm(5));
            a.str(x5, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            a.str(x2, ptr(x0, OFF_SP));
            a.b(end);
            a.bind(notFloat);
        }
        // Inline ByteString = (op 0x66 only).  Mirrors stencils.cpp:880-926.
        // After SmI tag check fails, try byte-equality for two byte
        // objects (fmt 16-23).  Helps Dictionary>>scanFor:'s key=arg
        // (dict bench) and other String comparisons.
        if (op == 0x66) {
            using namespace asmjit::a64;
            asmjit::Label bsBail = a.new_label();
            asmjit::Label resultFalse = a.new_label();
            asmjit::Label resultTrue = a.new_label();
            // Both must be heap objects (tag == 0, addr >= 0x10000).
            a.and_(x5, x1, asmjit::Imm(0x7));
            a.cbnz(x5, bsBail);
            a.cmp(x1, asmjit::Imm(0x10000));
            a.b_lo(bsBail);
            a.and_(x5, x4, asmjit::Imm(0x7));
            a.cbnz(x5, bsBail);
            a.cmp(x4, asmjit::Imm(0x10000));
            a.b_lo(bsBail);
            // Headers.
            a.ldr(x5, ptr(x1));                // header_a
            a.ldr(x6, ptr(x4));                // header_b
            // fmt_a / fmt_b in 16-23
            a.lsr(x7, x5, asmjit::Imm(24));
            a.and_(x7, x7, asmjit::Imm(0x1F));
            a.sub(x8, x7, asmjit::Imm(16));
            a.cmp(x8, asmjit::Imm(8));
            a.b_hs(bsBail);
            a.lsr(x9, x6, asmjit::Imm(24));
            a.and_(x9, x9, asmjit::Imm(0x1F));
            a.sub(x10, x9, asmjit::Imm(16));
            a.cmp(x10, asmjit::Imm(8));
            a.b_hs(bsBail);
            // Slot counts (reject 255 overflow encoding).
            a.lsr(x11, x5, asmjit::Imm(56));
            a.and_(x11, x11, asmjit::Imm(0xFF));
            a.cmp(x11, asmjit::Imm(255));
            a.b_eq(bsBail);
            a.lsr(x12, x6, asmjit::Imm(56));
            a.and_(x12, x12, asmjit::Imm(0xFF));
            a.cmp(x12, asmjit::Imm(255));
            a.b_eq(bsBail);
            // byteSize = slots*8 - (fmt - 16) (same as fmt & 7 for fmt in [16,23]).
            a.lsl(x11, x11, asmjit::Imm(3));
            a.and_(x7, x7, asmjit::Imm(0x7));
            a.sub(x11, x11, x7);                // bytes_a
            a.lsl(x12, x12, asmjit::Imm(3));
            a.and_(x9, x9, asmjit::Imm(0x7));
            a.sub(x12, x12, x9);                // bytes_b
            a.cmp(x11, x12);
            a.b_ne(resultFalse);                // size mismatch → false
            // Compare bytes [0..bytes_a).
            a.add(x13, x1, asmjit::Imm(8));   // ba
            a.add(x14, x4, asmjit::Imm(8));   // bb
            a.mov(x15, asmjit::Imm(0));
            {
                asmjit::Label cmpLoop = a.new_label();
                a.bind(cmpLoop);
                a.cmp(x15, x11);
                a.b_hs(resultTrue);
                a.ldrb(w5, ptr(x13, x15));
                a.ldrb(w6, ptr(x14, x15));
                a.cmp(w5, w6);
                a.b_ne(resultFalse);
                a.add(x15, x15, asmjit::Imm(1));
                a.b(cmpLoop);
            }
            // true / false result.
            a.bind(resultTrue);
            a.ldr(x6, ptr(x0, OFF_TRUEOOP));
            a.str(x6, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            a.str(x2, ptr(x0, OFF_SP));
            a.b(end);
            a.bind(resultFalse);
            a.ldr(x6, ptr(x0, OFF_FALSEOOP));
            a.str(x6, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            a.str(x2, ptr(x0, OFF_SP));
            a.b(end);
            a.bind(bsBail);
        }
        // x5 = state.method.rawBits + bcOffsetFromMethObj  (post-GC safe)
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);

        a.bind(end);
        return true;
    }
    // Phase 3 bitwise on ARM64: 0x6E bitAnd:, 0x6F bitOr:.  Tag bits
    // commute with AND/OR — no untag/retag needed.  Mirror of x86.
    if (isPhase3BitOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        a.ldr(x2, ptr(x0, OFF_SP));
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        a.eor(x5, x1, x4);
        a.sub(x6, x1, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);
        if (op == 0x6E) a.and_(x1, x1, x4);   // bitAnd:
        else            a.orr (x1, x1, x4);   // bitOr:
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.b(end);
        a.bind(bail);
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // Phase 3 multiplication on ARM64: 0x68 *.  Use smulh to detect
    // 64-bit overflow, then verify the result fits in 61-bit SmI.
    if (isPhase3MulOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        a.ldr(x2, ptr(x0, OFF_SP));
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        a.eor(x5, x1, x4);
        a.sub(x6, x1, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);
        a.asr(x1, x1, asmjit::Imm(3));
        a.asr(x4, x4, asmjit::Imm(3));
        a.mul (x6, x1, x4);
        a.smulh(x7, x1, x4);
        a.cmp(x7, x6, asmjit::a64::asr(63));
        a.b_ne(bail);
        // Verify result fits in 61-bit SmI range.
        a.lsl(x7, x6, asmjit::Imm(3));
        a.asr(x7, x7, asmjit::Imm(3));
        a.cmp(x7, x6);
        a.b_ne(bail);
        a.lsl(x1, x6, asmjit::Imm(3));
        a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.b(end);
        a.bind(bail);
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // Phase 3 bitShift: on ARM64: 0x6C.  Positive b → left shift with
    // overflow check; negative b → arithmetic right shift.
    if (isPhase3ShiftOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        asmjit::Label rightShift = a.new_label();
        a.ldr(x2, ptr(x0, OFF_SP));
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        a.eor(x5, x1, x4);
        a.sub(x6, x1, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);
        a.asr(x1, x1, asmjit::Imm(3));  // untag a
        a.asr(x4, x4, asmjit::Imm(3));  // untag b
        a.cmp(x4, asmjit::Imm(0));
        a.b_lt(rightShift);
        // Left shift: b in [0, 62].  Bail if >= 63.
        a.cmp(x4, asmjit::Imm(63));
        a.b_ge(bail);
        a.lsl(x6, x1, x4);              // result = a << b
        a.asr(x7, x6, x4);              // sanity: shifted-back equals a?
        a.cmp(x7, x1);
        a.b_ne(bail);
        // Retag: lsl-3 may overflow; verify with sar.
        a.lsl(x1, x6, asmjit::Imm(3));
        a.asr(x7, x1, asmjit::Imm(3));
        a.cmp(x7, x6);
        a.b_ne(bail);
        a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.b(end);
        a.bind(rightShift);
        // b in [-62, -1].  Negate.
        a.neg(x4, x4);
        a.cmp(x4, asmjit::Imm(63));
        a.b_gt(bail);
        a.asr(x1, x1, x4);              // signed right shift
        // Retag — result is smaller than input, no overflow possible.
        a.lsl(x1, x1, asmjit::Imm(3));
        a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.b(end);
        a.bind(bail);
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // Phase 3 \\ (0x6A) and // (0x6D) — SmI floor-div / floor-mod on
    // ARM64 with sign-mismatch adjustment.  Pharo uses floor semantics
    // (a // b = floor(a/b); a \\ b = a - (a // b) * b), while ARM64
    // sdiv is trunc semantics.  Adjust by adding b to the remainder
    // when (a XOR b) is negative AND the remainder is non-zero.
    //   //: result = trunc quotient + adjustment (-1 if rem!=0 && signs differ)
    //   \\: result = trunc remainder + b      (if rem!=0 && signs differ)
    // Bails on non-SmI, divisor==0, or SmI overflow (impossible for
    // mod; possible for // only on a=INT_MIN/b=-1 — out of SmI range).
    if (isPhase3ModOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();
        asmjit::Label noAdjust = a.new_label();
        a.ldr(x2, ptr(x0, OFF_SP));
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        // SmI tag check
        a.eor(x5, x1, x4);
        a.sub(x6, x1, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);
        // Untag both
        a.asr(x1, x1, asmjit::Imm(3));
        a.asr(x4, x4, asmjit::Imm(3));
        // Divisor == 0 → bail
        a.cbz(x4, bail);
        // sdiv x6 = a/b (trunc), msub x7 = a - q*b (trunc rem)
        a.sdiv(x6, x1, x4);
        a.msub(x7, x6, x4, x1);
        // Floor adjustment: if rem != 0 AND (a XOR b) < 0
        a.cbz(x7, noAdjust);
        a.eor(x5, x1, x4);
        a.tbz(x5, asmjit::Imm(63), noAdjust);
        if (op == 0x6A) {
            // \\: rem += b
            a.add(x7, x7, x4);
        } else {
            // //: q -= 1
            a.sub(x6, x6, asmjit::Imm(1));
        }
        a.bind(noAdjust);
        // Result is in x7 (rem) for \\ or x6 (quot) for //.  Retag.
        asmjit::a64::Gp result = (op == 0x6A) ? x7 : x6;
        a.lsl(x1, result, asmjit::Imm(3));
        // Verify retag didn't overflow SmI (61 bits signed).
        a.asr(x5, x1, asmjit::Imm(3));
        a.cmp(x5, result);
        a.b_ne(bail);
        a.orr(x1, x1, asmjit::Imm(SMI_TAG));
        a.str(x1, ptr(x2, -16));
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.b(end);
        a.bind(bail);
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_ARITH_OVERFLOW));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        a.bind(end);
        return true;
    }
    // Short forward jumps (0xB0..0xC7) — see x86 version for protocol.
    if (op >= SistaV1::ShortJumpBase && op <= SistaV1::ShortJumpFalseLast) {
        int targetIdx = SistaV1::shortJumpTarget(op, globalIdx);
        // Out-of-range target = unreachable trailer byte; emit a bail.
        // (Pharo's CompiledMethod trailer follows the bytecodes and can
        // include short jumps with bogus offsets.)
        if (targetIdx < 0 || targetIdx >= (int)bcLabels.size()) {
            a.ldr(x5, ptr(x0, OFF_METHOD));
            a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
            a.str(x5, ptr(x0, OFF_IP));
            a.mov(w3, asmjit::Imm(EXIT_SEND));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);
            return true;
        }
        if (op <= SistaV1::ShortJumpLast) {
            a.b(bcLabels[targetIdx]);
            return true;
        }
        bool jumpOnTrue = (op >= SistaV1::ShortJumpTrueBase
                           && op <= SistaV1::ShortJumpTrueLast);
        asmjit::Label mustBoolBail = a.new_label();
        asmjit::Label takeBranch   = a.new_label();
        asmjit::Label fallThrough  = a.new_label();

        a.ldr(x2, ptr(x0, OFF_SP));
        a.ldur(x1, asmjit::a64::ptr(x2, -8));   // x1 = TOS (not popped)

        // ldp loads TRUEOOP+FALSEOOP in one instruction.  Both adjacent
        // at offsets 128 and 136 in JITState.  Slightly wasted load on
        // the fast-true case (we branch before reaching the falseOop
        // cmp), but ldp is typically L1-cycle-equivalent to a single ldr
        // and saves 1 instruction worth of i-cache.
        a.ldp(x4, x5, ptr(x0, OFF_TRUEOOP));
        a.cmp(x1, x4);
        a.b_eq(jumpOnTrue ? takeBranch : fallThrough);
        a.cmp(x1, x5);
        a.b_ne(mustBoolBail);
        a.b(jumpOnTrue ? fallThrough : takeBranch);

        a.bind(takeBranch);
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.b(bcLabels[targetIdx]);

        a.bind(fallThrough);
        a.sub(x2, x2, asmjit::Imm(8));
        a.str(x2, ptr(x0, OFF_SP));
        a.b(bcLabels[globalIdx + 1]);

        a.bind(mustBoolBail);
        a.ldr(x5, ptr(x0, OFF_METHOD));
        a.add(x5, x5, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x5, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_MUST_BOOL));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        return true;
    }
    // ARM64 send emit: see x86 version for IC probe rationale + layout.
    if (isPhase4SendOp(op)) {
        using namespace asmjit::a64;
        int nArgs = sendNArgs(op);

        // Per-site IC address into x5.
        a.ldr(x5, ptr(x0, OFF_JITMETHOD));
        a.ldr(x5, ptr(x5, (int)offsetof(JITMethod, icBuffer)));
        a.add(x5, x5, asmjit::Imm(siteIdx * (int)IC_BYTES_PER_SITE));

        // Deferred state setup: inline-spec paths skip the state
        // store entirely.  dispatchCached / miss / non-probe paths
        // emit it inline.  Mirrors x86 commit c4d325eb.

        bool probeThis = g_debug.t1ICProbe;
        if (probeThis) {
            int icMin = g_debug.t1ICProbeMin;
            int icMax = g_debug.t1ICProbeMax;
            if (icMin >= 0 && (int)op < icMin) probeThis = false;
            if (icMax >= 0 && (int)op > icMax) probeThis = false;
        }
        if (probeThis) {
            // See x86 version for the protocol; this is the ARM64 mirror
            // with the inline-getter / inline-setter / returnsSelf
            // specializations.
            asmjit::Label imm = a.new_label();
            asmjit::Label haveKey = a.new_label();
            asmjit::Label miss = a.new_label();
            asmjit::Label dispatchCached = a.new_label();
            asmjit::Label tryGetter = a.new_label();
            asmjit::Label trySetter = a.new_label();
            asmjit::Label tryReturnsSelf = a.new_label();
            asmjit::Label tryPrimBitAnd = a.new_label();
            asmjit::Label tryPrimBitOr = a.new_label();
            asmjit::Label tryPrimBitXor = a.new_label();
            asmjit::Label tryPrimBitShift = a.new_label();
            asmjit::Label tryPrimIdentityHash = a.new_label();
            asmjit::Label tryPrimAt = a.new_label();
            asmjit::Label tryPrimAtPut = a.new_label();
            asmjit::Label tryPrimSize = a.new_label();
            asmjit::Label tryPrimSmallFloatOp = a.new_label();
            // dispatchCachedRestoreX5: inline-prim bail target that
            // reloads x5 from OFF_ICDATAPTR (where the original icDataPtr
            // was stashed before the inline-prim code clobbered x5 with
            // slotCount).  Falls through to dispatchCached.
            asmjit::Label dispatchCachedRestoreX5 = a.new_label();
            asmjit::Label j2jBailHeap = a.new_label();
            asmjit::Label j2jBailHeap2 = a.new_label();
            asmjit::Label endOfSend = a.new_label();

            a.ldr(x2, ptr(x0, OFF_SP));
            int rcvrOffsetBytes = -8 * (nArgs + 1);
            a.ldur(x1, ptr(x2, rcvrOffsetBytes));   // x1 = receiver
            a.and_(x4, x1, asmjit::Imm(0x7));
            a.cbnz(x4, imm);
            a.ldr(w4, ptr(x1));               // low 32 bits of header
            a.and_(w4, w4, asmjit::Imm(0x3FFFFF));
            a.b(haveKey);
            a.bind(imm);
            a.orr(w4, w4, asmjit::Imm(0x80000000U));
            a.bind(haveKey);

            a.ldr(x6, ptr(x5));               // icData[0]
            a.cmp(x4, x6);
            a.b_ne(miss);
            a.cbz(x4, miss);
            if (g_debug.t1ProbeAlwaysMiss) {
                a.b(miss);                     // diagnostic: never take HIT
            }

            // x7 = extras
            a.ldr(x7, ptr(x5, 16));
            // Debug: count every IC HIT (PHARO_T1_INLINE_J2J=1 telemetry)
            {
                static const bool inlineJ2JDbg =
                    std::getenv("PHARO_T1_INLINE_J2J") != nullptr;
                if (inlineJ2JDbg) {
                    a.mov(x4, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_ic_hits));
                    a.ldr(x6, ptr(x4));
                    a.add(x6, x6, asmjit::Imm(1));
                    a.str(x6, ptr(x4));
                }
            }
            a.cbz(x7, dispatchCached);
            // Debug: count IC HITs where extra is set but bit 60 isn't
            {
                static const bool inlineJ2JDbg =
                    std::getenv("PHARO_T1_INLINE_J2J") != nullptr;
                if (inlineJ2JDbg) {
                    asmjit::Label haveBit60 = a.new_label();
                    a.tbnz(x7, asmjit::Imm(60), haveBit60);
                    a.mov(x4, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_extra_no_bit60));
                    a.ldr(x6, ptr(x4));
                    a.add(x6, x6, asmjit::Imm(1));
                    a.str(x6, ptr(x4));
                    a.bind(haveBit60);
                }
            }

            // ===== INLINE J2J (PHARO_T1_INLINE_J2J=1, opt-in 2026-05-17) =====
            // Bit 60 (J2J_ENTRY_BIT): callee is JIT-compiled; tail-call its
            // entry directly instead of round-tripping JIT→C++→JIT via the
            // chain loop's activateMethod path.  Saves ~500 cycles/send on
            // recursive sends.  MVP: self-recursive only (caller == callee).
            // Mirrors stencils.cpp:1733-1877's `j2j_direct_call:` block.
            // See deferred.md A6 for full design.
            const bool inlineJ2J = g_debug.t1InlineJ2J;
            if (inlineJ2J) {
                asmjit::Label tryInlineJ2J = a.new_label();
                asmjit::Label j2jBail      = a.new_label();
                // Bit 59 (BLOCK_VALUE_BIT) takes precedence over bit 60.
                // Block-value IC entries may have bit 59 alone (when the
                // value: method is not safe to J2J-call) — check 59 first.
                if (g_debug.t1InlineBlockValue) {
                    a.tbnz(x7, asmjit::Imm(59), tryInlineJ2J);
                }
                // Bit 60 set → try inline J2J; works for any receiver tag
                // (SmI receivers benefit too, unlike inline-getter/setter).
                a.tbnz(x7, asmjit::Imm(60), tryInlineJ2J);

                // (fall through to existing inline-spec dispatch)
                // Inline specializations need heap receiver for getter/setter,
                // SmI receiver for primKind bitwise dispatch.
                {
                    asmjit::Label fallSmIBranch = a.new_label();
                    a.tst(x1, asmjit::Imm(0x7));
                    if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                        a.b_ne(fallSmIBranch);
                    } else {
                        a.b_ne(dispatchCached);
                    }
                    // Heap receiver path
                    if (g_debug.t1InlineGetter) {
                        a.tbnz(x7, asmjit::Imm(63), tryGetter);
                    }
                    if (g_debug.t1InlineSetter) {
                        a.tbnz(x7, asmjit::Imm(62), trySetter);
                    }
                    if (g_debug.t1InlineReturnsSelf) {
                        a.tbnz(x7, asmjit::Imm(61), tryReturnsSelf);
                    }
                    a.b(dispatchCached);
                    // SmI receiver path: check primKind for inline bitwise.
                    if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                        a.bind(fallSmIBranch);
                        a.lsr(x6, x7, asmjit::Imm(48));
                        a.and_(x6, x6, asmjit::Imm(0x1F));
                        a.cmp(x6, asmjit::Imm(11));
                        a.b_eq(tryPrimBitAnd);
                        a.cmp(x6, asmjit::Imm(12));
                        a.b_eq(tryPrimBitOr);
                        a.cmp(x6, asmjit::Imm(19));
                        a.b_eq(tryPrimBitXor);
                        a.cmp(x6, asmjit::Imm(13));
                        a.b_eq(tryPrimBitShift);
                        a.b(dispatchCached);
                    }
                }

                // Per-bail-reason counters — gated on PHARO_T1_INLINE_J2J
                // env var (debug-only).  When env var off (production
                // default), counters NOT emitted — saves ~24 bytes per
                // inline-J2J emit site = ~100+ bytes for typical fib-like
                // methods.  When env var on, the per-bail counter writes
                // are emitted for diagnostic visibility.
                static const bool inlineJ2JCounters =
                    std::getenv("PHARO_T1_INLINE_J2J") != nullptr;
                asmjit::Label j2jBailZero = a.new_label();
                asmjit::Label j2jBailFull = a.new_label();
                asmjit::Label j2jBailSelf = a.new_label();
                auto emitIncCounter = [&](uint64_t addr) {
                    if (!inlineJ2JCounters) return;
                    a.mov(x15, asmjit::Imm(addr));
                    a.ldr(x14, ptr(x15));
                    a.add(x14, x14, asmjit::Imm(1));
                    a.str(x14, ptr(x15));
                };

                a.bind(tryInlineJ2J);
                // Resume label used by both block-value inline and xmethod
                // self-recursive paths.  Declared early so block-value emit
                // can adr into it.
                asmjit::Label afterSend = a.new_label();
                // Block-value inline (PHARO_T1_INLINE_BLOCK_VALUE=1).
                // When BLOCK_VALUE_BIT (bit 59) is set, the IC site is a
                // value/value:/value:... send to a FullBlockClosure.  The
                // IC's entryAddr (low 48 of x7) points at the value:
                // method's JIT entry (or stub), NOT the user-block's
                // compiled code.  We delegate validation + J2J save +
                // state setup + capture copy to a C helper, then `br x0`
                // to enter the block's JIT entry.
                if (g_debug.t1InlineBlockValue) {
                    asmjit::Label tryBlockValue = a.new_label();
                    asmjit::Label blockValueDone = a.new_label();
                    a.tbnz(x7, asmjit::Imm(59), tryBlockValue);
                    a.b(blockValueDone);
                    a.bind(tryBlockValue);
                    // Counter: try (helper about to be called).
                    a.mov(x14, asmjit::Imm((uint64_t)&g_blockValue_tries));
                    a.ldr(x15, ptr(x14));
                    a.add(x15, x15, asmjit::Imm(1));
                    a.str(x15, ptr(x14));
                    // Helper signature: (state, nArgs, resumeAddr)
                    //   x0 in: state ptr; x0 out: blockEntry or NULL
                    //   x1: nArgs (compile-time constant)
                    //   x2: resumeAddr (= afterSend label)
                    // Save state ptr + LR across the blr; blr clobbers x0.
                    a.sub(sp, sp, asmjit::Imm(16));
                    a.str(x0,  ptr(sp, 0));
                    a.str(x30, ptr(sp, 8));
                    a.mov(w1, asmjit::Imm(nArgs));
                    a.adr(x2, afterSend);
                    a.mov(x9, asmjit::Imm((uint64_t)
                        &jit_rt_inline_block_value_prep));
                    a.blr(x9);
                    // x0 = blockEntry or NULL; move to x9 before
                    // restoring x0 = state.
                    a.mov(x9, x0);
                    a.ldr(x0,  ptr(sp, 0));
                    a.ldr(x30, ptr(sp, 8));
                    a.add(sp, sp, asmjit::Imm(16));
                    {
                        asmjit::Label bailBV = a.new_label();
                        asmjit::Label hitBV  = a.new_label();
                        a.cbz(x9, bailBV);
                        a.mov(x14, asmjit::Imm((uint64_t)&g_blockValue_hits));
                        a.ldr(x15, ptr(x14));
                        a.add(x15, x15, asmjit::Imm(1));
                        a.str(x15, ptr(x14));
                        a.b(hitBV);
                        a.bind(bailBV);
                        a.mov(x14, asmjit::Imm((uint64_t)&g_blockValue_bails));
                        a.ldr(x15, ptr(x14));
                        a.add(x15, x15, asmjit::Imm(1));
                        a.str(x15, ptr(x14));
                        a.b(j2jBail);
                        a.bind(hitBV);
                    }
                    // Block entry expects x0 = state; we already restored.
                    // The helper pushed the J2J save with resumeAddr =
                    // afterSend, so the block's return prelude tail-calls
                    // back to afterSend → endOfSend.
                    a.br(x9);
                    a.bind(blockValueDone);
                }
                // 2026-05-17 loop iter 2: self-recursive inline-J2J.
                // Implements the simplest viable subset: callee == caller
                // (same JITMethod).  For benchFib-style recursion, all
                // inner sends are either prims (no chain-break) or
                // self-recursive (also inlined), so no chain-break risk.
                // For non-self-recursive case, bail (chain-break protocol
                // unresolved — see deferred.md A6).
                //
                // Register state at entry:
                //   x0  = state ptr
                //   x1  = receiver (from IC HIT setup)
                //   x5  = icDataPtr
                //   x7  = ic.extra (bit 60 set, entryAddr in low 48 bits)
                // nArgs is known at JIT compile time.
                //
                // Resume label (after_send) goes into save.resumeAddr.
                // Return prelude tail-calls there when callee returns.
                // (afterSend declared above so block-value inline can use it.)
                asmjit::Label j2jBailSelf2 = a.new_label();

                // Compute entryAddr (x9) and calleeJM (x10)
                // entryAddr = x7 & J2J_ADDR_MASK (low 48 bits)
                a.and_(x9, x7, asmjit::Imm(0x0000FFFFFFFFFFFFULL));
                a.sub(x10, x9, asmjit::Imm((int)sizeof(JITMethod)));  // calleeJM = entry - JM_SIZE

                // Self-recursive check via SELF_REC_BIT (bit 56) in the
                // IC extra word.  The IC patcher sets this bit when
                // callerCM == calleeCM at IC-fill time (see
                // Interpreter::upgradeICToJ2J + patchJITICAfterSend).
                // Bit preserved across recompile by
                // rewriteIcEntriesAfterRecompile (only entryAddr in
                // bits 47:0 is rewritten).  Replaces the prior
                // 2-ldr + cmp + branch CM-oop comparison (saves
                // 3 instr per inline-J2J site).
                a.ldr(x11, ptr(x0, OFF_JITMETHOD));   // x11 = callerJM (save-push)

                // Debug counters: stash last-seen values — gated on env
                // var so production emit doesn't carry the overhead.
                // CM loads only happen in the debug branch.
                if (inlineJ2JCounters) {
                    a.ldr(x12, ptr(x11, 0));    // caller's CM oop
                    a.ldr(x13, ptr(x10, 0));    // callee's CM oop
                    a.mov(x14, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_caller_method));
                    a.str(x12, ptr(x14));
                    a.mov(x14, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_callee_method));
                    a.str(x13, ptr(x14));
                    a.mov(x14, asmjit::Imm((uint64_t)&g_inlineJ2J_dbg_extra));
                    a.str(x7, ptr(x14));
                }

                // CROSS-METHOD ATTEMPT (PHARO_T1_INLINE_J2J_XMETHOD=1 opt-in).
                // Default OFF — known to corrupt state.  Opt-in for lldb
                // debugging only.  PHARO_T1_INLINE_J2J_XMETHOD_MAX=N
                // bisection-limits the number of cross-method fires.
                const bool xmethod = g_debug.t1InlineJ2JXmethod;
                if (g_debug.t1InlineJ2JXmethodMax >= 0) {
                    g_xmethod_max = (uint64_t)g_debug.t1InlineJ2JXmethodMax;
                }
                asmjit::Label sameMethodSkipUpdate = a.new_label();
                if (xmethod) {
                    // Bit 56 set → self-recursive, skip the cross-method
                    // update.  Bit 56 not set → fall through to the
                    // cross-method gates and update.
                    a.tbnz(x7, asmjit::Imm(56), sameMethodSkipUpdate);
                    // Cross-method gates use callee's methodHeader,
                    // numICEntries, isStubOnEntry, canBailMidMethod.
                    if (!inlineJ2JCounters) {
                        a.ldr(x13, ptr(x10, 0));   // callee CM (for state.method)
                    }
                    a.ldr(x4, ptr(x10, 16));            // methodHeader (decoded)
                    a.tbnz(x4, asmjit::Imm(16), j2jBailSelf2);  // has prim → bail
                    // JITMethod::numICEntries at offset 28 (uint16).
                    a.ldrh(w4, ptr(x10, 28));
                    a.cbnz(w4, j2jBailSelf2);
                    // isStubOnEntry at JM[47] — stubs never invoke return
                    // prelude → save would leak.
                    a.ldrb(w4, ptr(x10, 47));
                    a.cbnz(w4, j2jBailSelf2);
                    // canBailMidMethod at JM[46] — mid-method bails
                    // corrupt caller frame via inline-activate path.
                    a.ldrb(w4, ptr(x10, 46));
                    a.cbnz(w4, j2jBailSelf2);
                    a.mov(x14, asmjit::Imm((uint64_t)&g_xmethod_count));
                    a.ldr(x15, ptr(x14));
                    a.add(x15, x15, asmjit::Imm(1));
                    a.str(x15, ptr(x14));
                    a.mov(x14, asmjit::Imm((uint64_t)&g_xmethod_max));
                    a.ldr(x14, ptr(x14));
                    a.cmp(x15, x14);
                    a.b_hi(j2jBailSelf2);
                    // Cross-method update:
                    a.str(x10, ptr(x0, OFF_JITMETHOD));
                    a.str(x13, ptr(x0, OFF_METHOD));
                    a.add(x14, x13, asmjit::Imm(16));
                    a.str(x14, ptr(x0, OFF_LITERALS));
                    a.mov(w14, asmjit::Imm(nArgs));
                    a.str(w14, ptr(x0, OFF_ARGCOUNT));
                    static const bool xlog =
                        std::getenv("PHARO_T1_INLINE_J2J_XMETHOD_LOG") != nullptr;
                    if (xlog) {
                        using namespace asmjit::a64;
                        if (!inlineJ2JCounters) {
                            // xlog helper signature uses x12 = callerCM.
                            a.ldr(x12, ptr(x11, 0));
                        }
                        a.sub(sp, sp, asmjit::Imm(80));
                        a.stp(x0, x7,   ptr(sp, 0));
                        a.stp(x9, x10,  ptr(sp, 16));
                        a.stp(x11, x12, ptr(sp, 32));
                        a.stp(x13, x30, ptr(sp, 48));
                        a.mov(x1, x10);
                        a.mov(x2, x11);
                        a.mov(x3, x13);
                        a.mov(x4, x12);
                        a.mov(x5, asmjit::Imm((uint64_t)&jit_rt_xmethod_log));
                        a.blr(x5);
                        a.ldp(x0, x7,   ptr(sp, 0));
                        a.ldp(x9, x10,  ptr(sp, 16));
                        a.ldp(x11, x12, ptr(sp, 32));
                        a.ldp(x13, x30, ptr(sp, 48));
                        a.add(sp, sp, asmjit::Imm(80));
                    }
                    a.bind(sameMethodSkipUpdate);
                } else {
                    // Default (xmethod off): bit 56 not set → cross-method,
                    // which we can't handle here.  Bail to dispatchCached.
                    a.tbz(x7, asmjit::Imm(56), j2jBailSelf2);
                }

                // Check save stack space.  cursor (offset 144) and limit
                // (offset 152) are adjacent — load both with one ldp.
                static_assert(OFF_J2J_SAVE_LIMIT == OFF_J2J_SAVE_CURSOR + 8,
                              "cursor/limit adjacency required for ldp fold");
                a.ldp(x6, x14, ptr(x0, OFF_J2J_SAVE_CURSOR));  // x6=cursor, x14=limit
                a.cmp(x6, x14);
                a.b_hs(j2jBailFull);

                // Load resumeAddr (label adr after the send completes)
                a.adr(x14, afterSend);

                // Push J2J save (56 bytes).  Uses ldp/stp for adjacent
                // state fields: sp+receiver (offsets 0/8) loaded with
                // one ldp; jitMethod+resumeAddr stored with one stp.
                // Saves 3 instructions per inline-J2J site.
                //   [0]=sp, [8]=receiver, [16]=tempBase, [24]=ip,
                //   [32]=jitMethod, [40]=resumeAddr, [48]=sendArgCount
                a.ldp(x15, x4, ptr(x0, OFF_SP));   // sp + receiver
                a.stp(x15, x4, ptr(x6, 0));
                // tempBase (offset 16) + ip (offset 24) are adjacent; load
                // both into two regs and stp.  Saves 1 instruction vs the
                // prior two ldr-str pairs.
                //
                // save.ip: PHARO_T1_J2J_POST_SEND_IP=1 stores method +
                // bcOffsetFromMethObj + 1 (mirroring chain-loop's J2JCall
                // which advances ip past send before saving — see
                // Interpreter.cpp:18829-18833 + 18892).  Default OFF
                // preserves pre-existing behavior.
                //
                // For postSendIp we need callerCM; in the bit-56-gated
                // emit it isn't loaded by default, so fetch it on demand
                // from callerJM[0].
                a.ldr(x15, ptr(x0, OFF_TEMPBASE));
                if (g_debug.t1J2JPostSendIp) {
                    if (!inlineJ2JCounters) {
                        a.ldr(x12, ptr(x11, 0));   // callerCM
                    }
                    a.add(x4, x12, asmjit::Imm(bcOffsetFromMethObj + 1));
                } else {
                    a.ldr(x4, ptr(x0, OFF_IP));
                }
                a.stp(x15, x4, ptr(x6, 16));       // tempBase + ip
                a.stp(x11, x14, ptr(x6, 32));      // callerJM + resumeAddr
                a.mov(w15, asmjit::Imm(nArgs));
                a.str(w15, ptr(x6, 48));

                // Bump cursor + depth + totalCalls.  depth and totalCalls
                // are adjacent int32 fields (offsets 160/164); fold the
                // two ldr-add-str pairs (6 instrs) into one 64-bit
                // ldr-add-str + movz/movk materialization (5 instrs).
                // depth caps at recursion depth (way below 2^31) so
                // adding 1 to the low 32 bits never carries into the
                // high 32 bits where totalCalls lives.
                a.add(x6, x6, asmjit::Imm(56));
                a.str(x6, ptr(x0, OFF_J2J_SAVE_CURSOR));
                static_assert(OFF_J2J_TOTAL_CALLS == OFF_J2J_DEPTH + 4,
                              "depth/totalCalls adjacency required for 64-bit batched increment");
                a.movz(x14, asmjit::Imm(0x1));
                a.movk(x14, asmjit::Imm(0x1), 32);   // x14 = (1<<32) | 1
                a.ldr(x13, ptr(x0, OFF_J2J_DEPTH));
                a.add(x13, x13, x14);
                a.str(x13, ptr(x0, OFF_J2J_DEPTH));

                // Set up callee state for self-recursion:
                //   receiver = sp[-(nArgs+1)*8]
                //   tempBase = sp - nArgs*8
                //   ip       = method bytecode start
                //   sp       = tempBase + tempCount*8
                //
                // x1 already holds the new receiver (loaded by the IC HIT
                // setup at `ldur x1, [x2, rcvrOffsetBytes]` and preserved
                // through the inline-J2J emit) — skip the redundant
                // sub + ldr that re-reads it from the stack.
                a.str(x1, ptr(x0, OFF_RECEIVER));        // recv from x1
                a.ldr(x12, ptr(x0, OFF_SP));             // x12 = caller sp
                a.sub(x13, x12, asmjit::Imm(nArgs * 8)); // x13 = new tempBase
                a.str(x13, ptr(x0, OFF_TEMPBASE));

                // Load cached bcStart from JITMethod (offset 96).  Pre-
                // computed in compileViaAsmjit from compiledMethodOop +
                // methodHeader — both immutable post-construct.  Saves
                // 6 instructions per inline-J2J emit site (was a 7-instr
                // numLits-shift-add chain).
                a.ldr(x14, ptr(x10, (int)offsetof(JITMethod, bcStartCache)));
                a.str(x14, ptr(x0, OFF_IP));

                // sp = tempBase + tempCount*8.  For the SELF-RECURSIVE
                // path (the only path when xmethod is off), callee ==
                // caller so callee.tempCount == compile-time-known
                // callerTempCount.  Skip the dynamic tempCount load +
                // init loop entirely when we know the layout statically.
                //
                // Three static cases (callerTempCount known here):
                //   1. nArgs == callerTempCount: no extra temps.  New sp
                //      equals caller's pre-send sp (still in x12).
                //   2. nArgs < callerTempCount: unroll N nil-stores +
                //      compute static sp offset.
                //   3. xmethod ON: callee.tempCount can differ from
                //      caller's; fall back to the dynamic loop.
                const bool xmethodMayDiffer = xmethod;
                if (!xmethodMayDiffer && callerTempCount >= nArgs) {
                    int extras = callerTempCount - nArgs;
                    if (extras == 0) {
                        // New sp = caller's sp (no temp init).
                        a.str(x12, ptr(x0, OFF_SP));
                    } else {
                        // Unroll nil-stores for the (small) extra temp
                        // count and compute new sp statically.  Methods
                        // with very large extras-counts are rare; cap
                        // unroll at 8 and fall back to dynamic loop
                        // beyond that to avoid blowing emit size.
                        if (extras <= 8) {
                            a.mov(x4, asmjit::Imm(nilBits));
                            for (int k = 0; k < extras; k++) {
                                a.str(x4, ptr(x13, (nArgs + k) * 8));
                            }
                            // New sp = tempBase + tempCount*8
                            //        = caller_sp + (tempCount-nArgs)*8
                            a.add(x15, x12, asmjit::Imm(extras * 8));
                            a.str(x15, ptr(x0, OFF_SP));
                        } else {
                            // Large extras: fall back to dynamic loop
                            // (same as xmethod path).
                            asmjit::Label initLoop = a.new_label();
                            asmjit::Label initDone = a.new_label();
                            a.add(x14, x13, asmjit::Imm(nArgs * 8));
                            a.add(x15, x13, asmjit::Imm(callerTempCount * 8));
                            a.mov(x4, asmjit::Imm(nilBits));
                            a.bind(initLoop);
                            a.cmp(x14, x15);
                            a.b_hs(initDone);
                            a.str(x4, ptr(x14));
                            a.add(x14, x14, asmjit::Imm(8));
                            a.b(initLoop);
                            a.bind(initDone);
                            a.str(x15, ptr(x0, OFF_SP));
                        }
                    }
                } else {
                    // xmethod path: callee tempCount is dynamic (read
                    // from callee JM[35]).  Initialize slots
                    // [nArgs..tempCount) to nil and compute new sp.
                    a.ldrb(w15, ptr(x10, 35));       // tempCount
                    asmjit::Label initLoop = a.new_label();
                    asmjit::Label initDone = a.new_label();
                    a.add(x14, x13, asmjit::Imm(nArgs * 8));
                    a.lsl(w15, w15, asmjit::Imm(3));
                    a.add(x15, x13, x15);
                    a.mov(x4, asmjit::Imm(nilBits));
                    a.bind(initLoop);
                    a.cmp(x14, x15);
                    a.b_hs(initDone);
                    a.str(x4, ptr(x14));
                    a.add(x14, x14, asmjit::Imm(8));
                    a.b(initLoop);
                    a.bind(initDone);
                    a.str(x15, ptr(x0, OFF_SP));
                }

                // Bump hits counter
                emitIncCounter((uint64_t)&g_inlineJ2J_hits);

                // Tail-call (br) to entry. x9 = entryAddr.
                a.br(x9);

                // afterSend: return prelude tail-calls here.  Caller state
                // restored; retval already on top of caller sp.
                a.bind(afterSend);
                a.b(endOfSend);

                a.bind(j2jBailSelf2);
                emitIncCounter((uint64_t)&g_inlineJ2J_bail_self);
                a.b(j2jBail);
                a.bind(j2jBailZero);
                a.bind(j2jBailFull);
                emitIncCounter((uint64_t)&g_inlineJ2J_bail_full);
                a.b(j2jBail);
                a.bind(j2jBailSelf);

                a.bind(j2jBail);
                // Fall through to inline-spec dispatch (same as non-J2J path).
                // For nArgs == 1, check primKind bits 52:48 for inline SmI
                // bitwise ops (bitAnd/bitOr/bitXor) — these skip the chain-
                // loop round-trip on SmI sends, mirroring stencils.cpp:1609-1614.
                if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                    a.tst(x1, asmjit::Imm(0x7));     // SmI? tag != 0
                    a.b_eq(j2jBailHeap);              // tag==0 → heap path
                    // Immediate receiver (SmI or SmallFloat).  Check primKind.
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    a.cmp(x6, asmjit::Imm(11));
                    a.b_eq(tryPrimBitAnd);
                    a.cmp(x6, asmjit::Imm(12));
                    a.b_eq(tryPrimBitOr);
                    a.cmp(x6, asmjit::Imm(19));
                    a.b_eq(tryPrimBitXor);
                    a.cmp(x6, asmjit::Imm(13));
                    a.b_eq(tryPrimBitShift);
                    // SmallFloat ops — primKind 21/22/23.
                    a.sub(x6, x6, asmjit::Imm(21));
                    a.cmp(x6, asmjit::Imm(3));
                    a.b_lo(tryPrimSmallFloatOp);
                    a.b(dispatchCached);
                    a.bind(j2jBailHeap);
                } else {
                    a.tst(x1, asmjit::Imm(0x7));
                    a.b_ne(dispatchCached);
                }
                if (g_debug.t1InlineGetter) {
                    a.tbnz(x7, asmjit::Imm(63), tryGetter);
                }
                if (g_debug.t1InlineSetter) {
                    a.tbnz(x7, asmjit::Imm(62), trySetter);
                }
                if (g_debug.t1InlineReturnsSelf) {
                    a.tbnz(x7, asmjit::Imm(61), tryReturnsSelf);
                }
                // Inline at: / at:put: / size for heap receivers
                // (primKind 14/15/16).  Mirrors stencils.cpp:1538-1554.
                if ((nArgs == 0 || nArgs == 1 || nArgs == 2)
                        && g_debug.t1InlinePrimAt) {
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    if (nArgs == 1) {
                        a.cmp(x6, asmjit::Imm(14));
                        a.b_eq(tryPrimAt);
                    } else if (nArgs == 2) {
                        a.cmp(x6, asmjit::Imm(15));
                        a.b_eq(tryPrimAtPut);
                    } else {  // nArgs == 0
                        a.cmp(x6, asmjit::Imm(16));
                        a.b_eq(tryPrimSize);
                        a.cmp(x6, asmjit::Imm(20));
                        a.b_eq(tryPrimIdentityHash);
                    }
                }
                a.b(dispatchCached);
            } else {
                // Inline specializations need heap receiver (tag==0).
                // Also check primKind for SmI receivers with bitwise prim
                // dispatch (skips chain-loop for nArgs==1 bitAnd/bitOr/bitXor).
                if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                    a.tst(x1, asmjit::Imm(0x7));     // SmI? tag != 0
                    a.b_eq(j2jBailHeap2);             // tag==0 → heap path
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    a.cmp(x6, asmjit::Imm(11));
                    a.b_eq(tryPrimBitAnd);
                    a.cmp(x6, asmjit::Imm(12));
                    a.b_eq(tryPrimBitOr);
                    a.cmp(x6, asmjit::Imm(19));
                    a.b_eq(tryPrimBitXor);
                    a.cmp(x6, asmjit::Imm(13));
                    a.b_eq(tryPrimBitShift);
                    a.b(dispatchCached);
                    a.bind(j2jBailHeap2);
                } else {
                    a.tst(x1, asmjit::Imm(0x7));
                    a.b_ne(dispatchCached);
                }

                if (g_debug.t1InlineGetter) {
                    a.tbnz(x7, asmjit::Imm(63), tryGetter);
                }
                if (g_debug.t1InlineSetter) {
                    a.tbnz(x7, asmjit::Imm(62), trySetter);
                }
                if (g_debug.t1InlineReturnsSelf) {
                    a.tbnz(x7, asmjit::Imm(61), tryReturnsSelf);
                }
                // Inline at: / at:put: / size for heap receivers
                // (primKind 14/15/16).  Mirrors stencils.cpp:1538-1554.
                if ((nArgs == 0 || nArgs == 1 || nArgs == 2)
                        && g_debug.t1InlinePrimAt) {
                    a.lsr(x6, x7, asmjit::Imm(48));
                    a.and_(x6, x6, asmjit::Imm(0x1F));
                    if (nArgs == 1) {
                        a.cmp(x6, asmjit::Imm(14));
                        a.b_eq(tryPrimAt);
                    } else if (nArgs == 2) {
                        a.cmp(x6, asmjit::Imm(15));
                        a.b_eq(tryPrimAtPut);
                    } else {  // nArgs == 0
                        a.cmp(x6, asmjit::Imm(16));
                        a.b_eq(tryPrimSize);
                        a.cmp(x6, asmjit::Imm(20));
                        a.b_eq(tryPrimIdentityHash);
                    }
                }
                a.b(dispatchCached);
            }

            // === Inline getter: val = recv->slots[slotIdx] ===
            // x2 still holds SP from probe entry (mirrors x86 rcx-keep).
            a.bind(tryGetter);
            a.and_(x6, x7, asmjit::Imm(0xFFFF));   // slotIdx
            a.add(x3, x1, x6, asmjit::a64::lsl(3));
            a.ldr(x6, ptr(x3, 8));                // val = *(recv+slot*8+8)
            a.stur(x6, ptr(x2, rcvrOffsetBytes));  // replace receiver
            if (nArgs > 0) {
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
            }
            a.b(endOfSend);

            // === Inline setter: recv->slots[slotIdx] = arg ===
            // x2 still holds SP from probe entry.
            a.bind(trySetter);
            a.and_(x6, x7, asmjit::Imm(0xFFFF));
            a.ldur(x3, ptr(x2, -8));               // arg = sp[-1]
            a.add(x4, x1, x6, asmjit::a64::lsl(3));
            a.str(x3, ptr(x4, 8));
            if (nArgs > 0) {
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
            }
            a.b(endOfSend);

            // === returnsSelf ===
            a.bind(tryReturnsSelf);
            if (nArgs > 0) {
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
            }
            a.b(endOfSend);

            // Counter helper for inline-prim diagnostics (PHARO_T1_INLINE_PRIM_COUNTERS=1).
            static const bool primCountersEnabled =
                std::getenv("PHARO_T1_INLINE_PRIM_COUNTERS") != nullptr;
            auto emitIncPrimCounter = [&](uint64_t addr) {
                if (!primCountersEnabled) return;
                a.mov(x14, asmjit::Imm(addr));
                a.ldr(x15, ptr(x14));
                a.add(x15, x15, asmjit::Imm(1));
                a.str(x15, ptr(x14));
            };

            // === Inline SmI bitwise prims (primKind 11/12/19) ===
            // Receiver was confirmed SmI in the dispatch (tst x1, 0x7 → ne).
            // Arg must also be SmI; bail to dispatchCached if not.
            // bitAnd/bitOr preserve the tag (both operands have low 3 = 001,
            // AND/OR keeps it 001).  bitXor produces low 3 = 000, re-OR
            // SMI_TAG to retag.  Mirrors stencils.cpp:1609-1614.
            // x2 holds SP; rcvrOffsetBytes = -8 * (nArgs+1) = -16 for nArgs=1.
            // x1 = receiver (tagged), arg at sp[-8].
            if (nArgs == 1 && g_debug.t1InlinePrimBitOps) {
                a.bind(tryPrimBitAnd);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                a.ldur(x3, ptr(x2, -8));               // arg
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCached);
                a.and_(x4, x1, x3);                    // bitAnd tagged
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
                a.b(endOfSend);

                a.bind(tryPrimBitOr);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                a.ldur(x3, ptr(x2, -8));
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCached);
                a.orr(x4, x1, x3);                     // bitOr tagged
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
                a.b(endOfSend);

                a.bind(tryPrimBitXor);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                a.ldur(x3, ptr(x2, -8));
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCached);
                a.eor(x4, x1, x3);                     // bitXor (clears tag)
                a.orr(x4, x4, asmjit::Imm(0x1));       // restore SMI_TAG
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
                a.b(endOfSend);

                // === Inline SmI bitShift: (primKind 13) ===
                // Mirrors stencils.cpp:1615-1626.  Positive count = shift
                // left with overflow check; negative count = shift right.
                // x1 = tagged receiver, arg at sp[-8].
                a.bind(tryPrimBitShift);
                emitIncPrimCounter((uint64_t)&g_primBitOp_hits);
                {
                    asmjit::Label shiftRight = a.new_label();
                    asmjit::Label shiftDone = a.new_label();
                    a.ldur(x3, ptr(x2, -8));           // x3 = tagged shift count
                    a.and_(x6, x3, asmjit::Imm(0x7));
                    a.cmp(x6, asmjit::Imm(1));
                    a.b_ne(dispatchCached);           // not SmI
                    a.asr(x3, x3, asmjit::Imm(3));    // untag shift (signed)
                    a.asr(x4, x1, asmjit::Imm(3));    // untag receiver (signed)
                    a.cmp(x3, asmjit::Imm(0));
                    a.b_lt(shiftRight);
                    // positive: shift left with overflow check.
                    // Bail if shift >= 61 (would overflow SmI for any
                    // non-zero a) — stencils.cpp's `b < 61` gate.
                    a.cmp(x3, asmjit::Imm(61));
                    a.b_ge(dispatchCached);
                    a.lsl(x6, x4, x3);                // r = a << b
                    // Overflow check: (r >> b) == a iff no overflow.
                    a.asr(x5, x6, x3);
                    a.cmp(x5, x4);
                    a.b_ne(dispatchCached);
                    a.b(shiftDone);
                    a.bind(shiftRight);
                    // count < 0: shift right by -count.  Stencils uses
                    // `b > -64`, so |count| < 64.
                    a.neg(x3, x3);
                    a.cmp(x3, asmjit::Imm(64));
                    a.b_ge(dispatchCached);
                    a.asr(x6, x4, x3);                // r = a >> -b (signed)
                    a.bind(shiftDone);
                    // Retag: (r << 3) | 1.
                    a.lsl(x6, x6, asmjit::Imm(3));
                    a.orr(x6, x6, asmjit::Imm(0x1));
                    a.stur(x6, ptr(x2, rcvrOffsetBytes));
                    a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                    a.str(x2, ptr(x0, OFF_SP));
                    a.b(endOfSend);
                }
            } else {
                // Labels still need to be bound somewhere even if unused —
                // bind them as aliases for dispatchCached.
                a.bind(tryPrimBitAnd);
                a.bind(tryPrimBitOr);
                a.bind(tryPrimBitXor);
                a.bind(tryPrimBitShift);
                a.b(dispatchCached);
            }

            // Note: primKind 10 (==) inline was attempted earlier but
            // the dispatch overhead (~4 instr/send applied unconditionally)
            // didn't pay off — bytecode 0x76 already inlines == for SmI
            // and named-send #== is rare.  Could revisit if a workload
            // shows hot named-send == invocations.

            // === Inline at: (primKind 14, nArgs=1, heap receiver) ===
            // Receiver class is whatever IC matched; runtime check fmt
            // and decode header slot count.  Two paths:
            //   fmt == 2  (variable pointer / Array): return slot[idx]
            //   fmt 16-23 (indexable bytes / ByteArray, String):
            //              return SmI of byte at idx
            // Mirrors stencils.cpp:1538-1545 (Array path).
            // x1 = recv (heap), x2 = sp, x7 = extras.
            if (nArgs == 1 && g_debug.t1InlinePrimAt) {
                asmjit::Label tryByteAt = a.new_label();
                a.bind(tryPrimAt);
                emitIncPrimCounter((uint64_t)&g_primAt_hits);
                // Stash icDataPtr to memory so bails restore it via
                // dispatchCachedRestoreX5 (x5 gets clobbered with slotCount).
                a.str(x5, ptr(x0, OFF_ICDATAPTR));
                a.ldr(x4, ptr(x1));                  // x4 = header word
                a.lsr(x6, x4, asmjit::Imm(24));      // shift fmt to low
                a.and_(x6, x6, asmjit::Imm(0x1F));
                a.cmp(x6, asmjit::Imm(2));           // fmt 2 = Array path
                a.b_ne(tryByteAt);                   // else try byte path
                // Slot count in header bits 56:63 (1 byte). If 255 = overflow.
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);     // overflow → slow
                // Load idx (arg)
                a.ldur(x3, ptr(x2, -8));
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);     // arg not SmI
                a.asr(x4, x3, asmjit::Imm(3));       // x4 = idx (signed untag)
                a.cmp(x4, asmjit::Imm(1));
                a.b_lt(dispatchCachedRestoreX5);     // idx < 1
                a.cmp(x4, x5);
                a.b_gt(dispatchCachedRestoreX5);     // idx > sc
                // Slot offset = idx * 8 (slot[idx-1] at offset 8 + (idx-1)*8)
                a.lsl(x4, x4, asmjit::Imm(3));
                a.add(x6, x1, x4);
                a.ldr(x4, ptr(x6));                  // val = recv[idx*8]
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
                a.b(endOfSend);

                // Byte-indexed at:: fmt 16-23 (ByteArray, String, Symbol).
                // byteSize = slotCount*8 - (fmt & 7).  byte[idx] at
                // recvAddr + 8 + (idx-1).  Returns SmI of byte value.
                a.bind(tryByteAt);
                a.sub(x8, x6, asmjit::Imm(16));      // x8 = fmt - 16
                a.cmp(x8, asmjit::Imm(8));
                a.b_hs(dispatchCachedRestoreX5);     // fmt not in 16..23
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);     // slotCount==255 → ovf
                a.lsl(x5, x5, asmjit::Imm(3));       // sc*8 = max byte capacity
                a.and_(x6, x6, asmjit::Imm(0x7));    // extra bytes (= fmt&7)
                a.sub(x5, x5, x6);                   // byteSize
                a.ldur(x3, ptr(x2, -8));
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);     // idx not SmI
                a.asr(x4, x3, asmjit::Imm(3));       // x4 = idx
                a.cmp(x4, asmjit::Imm(1));
                a.b_lt(dispatchCachedRestoreX5);
                a.cmp(x4, x5);
                a.b_gt(dispatchCachedRestoreX5);     // idx > byteSize
                // byte at recvAddr + 8 + (idx-1) = recvAddr + 7 + idx
                a.add(x4, x4, asmjit::Imm(7));
                a.add(x6, x1, x4);
                a.ldrb(w4, ptr(x6));                 // zero-extended byte
                a.lsl(x4, x4, asmjit::Imm(3));       // retag SmI
                a.orr(x4, x4, asmjit::Imm(0x1));
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
                a.b(endOfSend);
            } else if (!(nArgs == 2 && g_debug.t1InlinePrimAt)) {
                a.bind(tryPrimAt);
                a.b(dispatchCached);
            }

            // === Inline at:put: (primKind 15, nArgs=2, heap receiver) ===
            // Mirrors stencils.cpp:1546-1554.  Also gates on immutable
            // flag (bit 23 of header).
            // sp layout before send: [..., recv, idx, val]  (val at sp[-1])
            if (nArgs == 2 && g_debug.t1InlinePrimAt) {
                asmjit::Label tryByteAtPut = a.new_label();
                a.bind(tryPrimAtPut);
                emitIncPrimCounter((uint64_t)&g_primAtPut_hits);
                // Stash icDataPtr — bails route through dispatchCachedRestoreX5.
                a.str(x5, ptr(x0, OFF_ICDATAPTR));
                a.ldr(x4, ptr(x1));                  // x4 = header word
                a.lsr(x6, x4, asmjit::Imm(24));
                a.and_(x6, x6, asmjit::Imm(0x1F));
                a.cmp(x6, asmjit::Imm(2));           // fmt 2 Array path
                a.b_ne(tryByteAtPut);
                // Immutability check (bit 23)
                a.tbnz(x4, asmjit::Imm(23), dispatchCachedRestoreX5);
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);
                // sp[-1] = val, sp[-2] = idx (after pushes for nArgs=2)
                a.ldur(x3, ptr(x2, -16));            // idx
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);
                a.asr(x4, x3, asmjit::Imm(3));       // idx untagged
                a.cmp(x4, asmjit::Imm(1));
                a.b_lt(dispatchCachedRestoreX5);
                a.cmp(x4, x5);
                a.b_gt(dispatchCachedRestoreX5);
                // Load val + store at recv[idx*8]
                a.ldur(x3, ptr(x2, -8));             // x3 = val
                a.lsl(x4, x4, asmjit::Imm(3));
                a.add(x6, x1, x4);
                a.str(x3, ptr(x6));                  // recv[idx*8] = val
                // at:put: returns the value (sp[-3]=recv slot becomes val)
                a.stur(x3, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
                a.b(endOfSend);

                // Byte at:put: for fmt 16-23.  Val must be SmI in 0..255.
                a.bind(tryByteAtPut);
                a.sub(x8, x6, asmjit::Imm(16));
                a.cmp(x8, asmjit::Imm(8));
                a.b_hs(dispatchCachedRestoreX5);
                a.tbnz(x4, asmjit::Imm(23), dispatchCachedRestoreX5);
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);
                a.lsl(x5, x5, asmjit::Imm(3));      // sc*8
                a.and_(x6, x6, asmjit::Imm(0x7));    // extra bytes (fmt-16)
                a.sub(x5, x5, x6);                   // byteSize
                a.ldur(x3, ptr(x2, -16));            // idx (tagged SmI)
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);
                a.asr(x4, x3, asmjit::Imm(3));       // idx
                a.cmp(x4, asmjit::Imm(1));
                a.b_lt(dispatchCachedRestoreX5);
                a.cmp(x4, x5);
                a.b_gt(dispatchCachedRestoreX5);
                a.ldur(x3, ptr(x2, -8));             // val (tagged SmI)
                a.and_(x6, x3, asmjit::Imm(0x7));
                a.cmp(x6, asmjit::Imm(1));
                a.b_ne(dispatchCachedRestoreX5);     // val not SmI
                a.asr(x6, x3, asmjit::Imm(3));       // val untagged
                a.cmp(x6, asmjit::Imm(255));
                a.b_hi(dispatchCachedRestoreX5);     // val > 255
                // Store byte at recvAddr + 8 + (idx-1) = recv + 7 + idx.
                // x6 holds the untagged value; use x7 for the address.
                a.add(x4, x4, asmjit::Imm(7));
                a.add(x7, x1, x4);
                a.strb(w6, ptr(x7));
                // at:put: returns the value (tagged SmI), write at recv slot
                a.stur(x3, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
                a.b(endOfSend);
            } else if (!(nArgs == 1 && g_debug.t1InlinePrimAt)
                       && !(nArgs == 0 && g_debug.t1InlinePrimAt)) {
                a.bind(tryPrimAtPut);
                a.b(dispatchCached);
            }

            // === Inline size (primKind 16, nArgs=0, heap receiver) ===
            // Three paths by header fmt:
            //   fmt 2:     Array — size = slotCount
            //   fmt 16-23: byte indexable — size = slotCount*8 - (fmt&7)
            //   fmt 9:     Indexable64 — size = slotCount
            // Result is SmI(byteSize-or-slotCount).
            if (nArgs == 0 && g_debug.t1InlinePrimAt) {
                asmjit::Label sizeBytes = a.new_label();
                asmjit::Label sizeDone = a.new_label();
                a.bind(tryPrimSize);
                emitIncPrimCounter((uint64_t)&g_primSize_hits);
                // Stash icDataPtr — bails route through dispatchCachedRestoreX5.
                a.str(x5, ptr(x0, OFF_ICDATAPTR));
                a.ldr(x4, ptr(x1));                  // header
                a.lsr(x6, x4, asmjit::Imm(24));
                a.and_(x6, x6, asmjit::Imm(0x1F));
                a.lsr(x5, x4, asmjit::Imm(56));
                a.and_(x5, x5, asmjit::Imm(0xFF));
                a.cmp(x5, asmjit::Imm(255));
                a.b_eq(dispatchCachedRestoreX5);     // overflow header → slow
                // fmt 2 or 9: result = slotCount (no adjust)
                a.cmp(x6, asmjit::Imm(2));
                a.b_eq(sizeDone);
                a.cmp(x6, asmjit::Imm(9));
                a.b_eq(sizeDone);
                // fmt 16-23: byte path
                a.sub(x8, x6, asmjit::Imm(16));
                a.cmp(x8, asmjit::Imm(8));
                a.b_hs(dispatchCachedRestoreX5);     // not byte fmt
                a.lsl(x5, x5, asmjit::Imm(3));       // sc*8
                a.and_(x6, x6, asmjit::Imm(0x7));    // extra bytes
                a.sub(x5, x5, x6);                   // byteSize
                a.bind(sizeDone);
                // Tag SmI: result = (size << 3) | 1
                a.lsl(x5, x5, asmjit::Imm(3));
                a.orr(x5, x5, asmjit::Imm(0x1));
                a.stur(x5, ptr(x2, rcvrOffsetBytes));
                a.b(endOfSend);
                (void)sizeBytes;
            } else if (!(nArgs == 1 && g_debug.t1InlinePrimAt)
                       && !(nArgs == 2 && g_debug.t1InlinePrimAt)) {
                a.bind(tryPrimSize);
                a.b(dispatchCached);
            }

            // === Inline identityHash (primKind 20, nArgs=0, heap receiver) ===
            // identityHash = (header >> 8) & 0x3FFFFF (22-bit hash field).
            // Tag as SmI and store.
            if (nArgs == 0 && g_debug.t1InlinePrimAt) {
                a.bind(tryPrimIdentityHash);
                a.ldr(x4, ptr(x1));                  // header
                a.lsr(x4, x4, asmjit::Imm(8));
                a.and_(x4, x4, asmjit::Imm(0x3FFFFF));
                a.lsl(x4, x4, asmjit::Imm(3));
                a.orr(x4, x4, asmjit::Imm(0x1));
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.b(endOfSend);
            } else {
                a.bind(tryPrimIdentityHash);
                a.b(dispatchCached);
            }

            // === Inline SmallFloat +/-/* at send site (primKind 21/22/23) ===
            // Receiver is SmallFloat (tag=5), arg is also SmallFloat.
            // Decode, op, encode.  Bails on ±0 or exponent over/underflow.
            if (nArgs == 1) {
                a.bind(tryPrimSmallFloatOp);
                emitIncPrimCounter((uint64_t)&g_primFloatOp_hits);
                // Stash icDataPtr — about to clobber x5.
                a.str(x5, ptr(x0, OFF_ICDATAPTR));
                // Arg at sp[-8].  Must be SmallFloat (tag 5).
                a.ldur(x3, ptr(x2, -8));
                a.and_(x4, x3, asmjit::Imm(0x7));
                a.cmp(x4, asmjit::Imm(5));
                a.b_ne(dispatchCachedRestoreX5);
                // Decode rcv.
                a.lsr(x4, x1, asmjit::Imm(3));
                a.cmp(x4, asmjit::Imm(1));
                a.b_ls(dispatchCachedRestoreX5);     // ±0 → bail
                a.mov(x5, asmjit::Imm(0x7000000000000000ULL));
                a.add(x4, x4, x5);
                a.ror(x4, x4, asmjit::Imm(1));
                a.fmov(d0, x4);
                // Decode arg.
                a.lsr(x4, x3, asmjit::Imm(3));
                a.cmp(x4, asmjit::Imm(1));
                a.b_ls(dispatchCachedRestoreX5);
                a.add(x4, x4, x5);
                a.ror(x4, x4, asmjit::Imm(1));
                a.fmov(d1, x4);
                // primKind already in x6 in lower bits 0..2 form (x6 = pk-21)
                // when we branched here (we did `sub x6, x6, #21`).
                // pk-21 = 0 → add, 1 → sub, 2 → mul.
                asmjit::Label opSub = a.new_label();
                asmjit::Label opMul = a.new_label();
                asmjit::Label opDone = a.new_label();
                a.cmp(x6, asmjit::Imm(1));
                a.b_eq(opSub);
                a.cmp(x6, asmjit::Imm(2));
                a.b_eq(opMul);
                a.fadd(d0, d0, d1);
                a.b(opDone);
                a.bind(opSub);
                a.fsub(d0, d0, d1);
                a.b(opDone);
                a.bind(opMul);
                a.fmul(d0, d0, d1);
                a.bind(opDone);
                // Encode result.
                a.fmov(x4, d0);
                a.ror(x4, x4, asmjit::Imm(63));      // ROL 1
                a.cmp(x4, x5);
                a.b_lo(dispatchCachedRestoreX5);     // underflow
                a.sub(x4, x4, x5);
                a.mov(x6, asmjit::Imm(0x1FFFFFFFFFFFFFFFULL));
                a.cmp(x4, x6);
                a.b_hi(dispatchCachedRestoreX5);     // overflow
                a.lsl(x4, x4, asmjit::Imm(3));
                a.orr(x4, x4, asmjit::Imm(5));        // SmallFloatTag
                a.stur(x4, ptr(x2, rcvrOffsetBytes));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
                a.b(endOfSend);
            } else {
                a.bind(tryPrimSmallFloatOp);
                a.b(dispatchCached);
            }

            // === Inline-prim bail restore stub ===
            // tryPrim* blocks clobber x5 (slotCount) but dispatchCached
            // needs x5 = icDataPtr.  They stash icDataPtr to OFF_ICDATAPTR
            // at the top of each block and route bails through this label.
            a.bind(dispatchCachedRestoreX5);
            a.ldr(x5, ptr(x0, OFF_ICDATAPTR));
            // Fall through.

            // === Plain cached dispatch === (emits deferred state setup)
            a.bind(dispatchCached);
            a.str(x5, ptr(x0, OFF_ICDATAPTR));
            a.mov(w3, asmjit::Imm(nArgs));
            a.str(w3, ptr(x0, OFF_SENDARGCOUNT));
            a.ldr(x6, ptr(x0, OFF_METHOD));
            a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj));
            a.str(x6, ptr(x0, OFF_IP));
            a.ldr(x6, ptr(x5, 8));            // icData[1] = method Oop
            a.str(x6, ptr(x0, OFF_CACHED_TARGET));
            a.mov(w3, asmjit::Imm(EXIT_SEND_CACHED));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);

            // === Miss === (emits deferred state setup)
            a.bind(miss);
            a.str(x5, ptr(x0, OFF_ICDATAPTR));
            a.mov(w3, asmjit::Imm(nArgs));
            a.str(w3, ptr(x0, OFF_SENDARGCOUNT));
            a.ldr(x6, ptr(x0, OFF_METHOD));
            a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj));
            a.str(x6, ptr(x0, OFF_IP));
            a.mov(w3, asmjit::Imm(EXIT_SEND));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);

            a.bind(endOfSend);
            return true;
        }

        // Non-probe fallback: emit state setup + bail.
        a.str(x5, ptr(x0, OFF_ICDATAPTR));
        a.mov(w3, asmjit::Imm(nArgs));
        a.str(w3, ptr(x0, OFF_SENDARGCOUNT));
        a.ldr(x6, ptr(x0, OFF_METHOD));
        a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x6, ptr(x0, OFF_IP));
        a.mov(w3, asmjit::Imm(EXIT_SEND));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        return true;
    }
    return false;
}
#endif

// Emit either real per-bytecode code OR the bail-to-interp stub
// (returns ExitSend immediately).  Writes the resulting bytes into
// `out` (caller buffer of `outCap` bytes); on success sets *outSize
// and *isReal (true if real codegen, false if bail stub).
//
// `bcOffsetBase` is the offset of bc[0] from the CompiledMethod
// object's address (i.e., 8 [object header] + 8*(1+numLiterals)).
// Per-bytecode bail emit uses `bcOffsetBase + i` so state.ip can be
// computed as `state.method.rawBits() + offset` at runtime — GC-safe.
//
// `bcToCodeOut` (if non-null, size bcLen+1) is filled with the per-
// bytecode emit start offset within the output buffer.  Slot bcLen
// holds the end-of-machine-code offset.  Used by the chain loop
// (codeOffsetForBC) to resume our JIT method at a specific bytecode
// after a callee completes.  Zero means "not a valid re-entry" —
// per the runtime contract (JITMethod.hpp::codeOffsetForBC).  For
// stub-only methods this is left as zeros except slot 0 = 0 and
// slot bcLen = emitted_size.
// `primIndex`: if > 0, emit the JIT prologue for this primitive at the
// start of code.  Caller guarantees bc[0..2] is the CallPrimitive
// bytecode for this primIndex and asks us to skip those 3 bytes from
// the fallback-emit pass.  bcToCode[0..2] stay 0 (not valid re-entry).
bool emitMethodBytes(const uint8_t* bc, size_t bcLen, uint64_t nilBits,
                     int bcOffsetBase, int primIndex,
                     int callerArgCount, int callerTempCount,
                     uint8_t* out, size_t outCap,
                     size_t* outSize, bool* isReal,
                     uint32_t* bcToCodeOut) {
    using namespace asmjit;

    Environment env = Environment::host();
    CodeHolder code;
    Error err = code.init(env);
    if (err != kErrorOk) return false;

    // PHARO_ASMJIT_T1_LOG=1 — dump asmjit asm to stderr per compile.
    // Heavy; only enable when actively debugging emit.
    static asmjit::FileLogger* asmjitLogger = []() -> asmjit::FileLogger* {
        if (std::getenv("PHARO_ASMJIT_T1_LOG"))
            return new asmjit::FileLogger(stderr);
        return nullptr;
    }();
    if (asmjitLogger) code.set_logger(asmjitLogger);

    // When emitting a prim prologue we skip the CallPrimitive bytes
    // (bc[0..2]) for the fallback emit — they're consumed by the
    // prologue.  The pre-scan + emit operate on bc + emitSkip.
    int emitSkip = (primIndex > 0) ? 3 : 0;
    const uint8_t* bcReal = bc + emitSkip;
    size_t bcRealLen = (bcLen >= (size_t)emitSkip) ? (bcLen - emitSkip) : 0;
    bool real = (bcRealLen > 0) && allBytecodesSupported(bcReal, bcRealLen);
    // Sieve correctness gate (2026-05-19): methods with prim 60/61/62
    // declared at method entry AND conditional jumps in the body trigger
    // a still-unexplained interaction that returns wrong results for
    // `Integer>>benchmark` (sieve x3 returns 1 instead of 1028).
    // Bisection isolated the bug to the cond-jump emit in Array>>at:put:
    // (3rd cond-jump compile in sieve's compile order); skipping any one
    // of {1,2,3} cures the symptom.  Stubbing methods with these prims
    // is harmless: the send-site catch via primKind 14/15/16 in IC extras
    // still fires for the common case, and the prim prologue is the only
    // thing lost — <5% perf delta on bench per docs/deferred.md A6 iter
    // N+19.  Detect by scanning the BODY for conditional jumps; the
    // prologue has already consumed the CallPrimitive header at bc[0..2].
    if (real && (primIndex == 60 || primIndex == 61 || primIndex == 62)) {
        bool hasCJ = false;
        for (size_t bi = 0; bi < bcRealLen; bi++) {
            uint8_t op = bcReal[bi];
            if ((op >= SistaV1::ShortJumpTrueBase
                    && op <= SistaV1::ShortJumpFalseLast)
                || op == SistaV1::ExtJumpTrue
                || op == SistaV1::ExtJumpFalse) {
                hasCJ = true; break;
            }
        }
        if (hasCJ) real = false;  // fall through to stub-compile
    }
    // PHARO_T1_INLINE_J2J_DUMP_BC=1: dump bytecode for failed compiles
    if (!real && std::getenv("PHARO_T1_INLINE_J2J_DUMP_BC")) {
        static size_t dumpN = 0;
        if (dumpN < 30 && bcRealLen < 80) {
            dumpN++;
            std::string bcstr;
            for (size_t i = 0; i < bcRealLen; i++) {
                char buf[8];
                snprintf(buf, sizeof(buf), " %02x", bcReal[i]);
                bcstr += buf;
            }
            fprintf(stderr, "[T1-BC-DUMP #%zu] bcLen=%zu bc=%s\n",
                dumpN, bcRealLen, bcstr.c_str());
        }
    }
    // PHARO_ASMJIT_T1_STUB_ONLY=1: kill switch — force every method to
    // the bail-on-entry stub regardless of bytecode support.  Used to
    // bisect Phase 2 emit bugs against the known-good Phase 1 behavior.
    static const bool stubOnly = std::getenv("PHARO_ASMJIT_T1_STUB_ONLY") != nullptr;
    if (stubOnly) real = false;
    // PHARO_ASMJIT_T1_HARDCODE_STUB=1: emit the stub by hardcoding the
    // bytes (mov dword [rdi+76], 2; ret).  Bypasses asmjit emit/copy
    // entirely so we can isolate whether the bug is in the asmjit
    // codegen path or in the integration plumbing.
    static const bool hardcodeStub = std::getenv("PHARO_ASMJIT_T1_HARDCODE_STUB") != nullptr;
    if (hardcodeStub && !real) {
        static const uint8_t kStubBytes[8] = {
            0xC7, 0x47, 0x4C, 0x02, 0x00, 0x00, 0x00, 0xC3
        };
        if (outCap < 8) return false;
        std::memcpy(out, kStubBytes, 8);
        *outSize = 8;
        *isReal = false;
        return true;
    }

    // Per-bytecode labels — bound just before each bytecode's emit.
    // After flatten, label_offset_from_base gives the emit start of
    // each bytecode → fills bcToCodeOut for the chain loop's resume.
    // Labels are created by the Assembler (not CodeHolder); see below
    // in each per-arch block.
    std::vector<Label> bcLabels;

#if defined(__x86_64__) || defined(_M_X64)
    x86::Assembler a(&code);
    // Always allocate per-bytecode labels when emitting real code.
    // Conditional jumps need them even if bcToCodeOut is null (which
    // doesn't happen in practice but is API-safe).
    if (real) {
        bcLabels.reserve(bcLen);
        for (size_t i = 0; i < bcLen; i++) bcLabels.push_back(a.new_label());
    }
    // Emit prim prologue first (if any).  Fall-through enters the
    // fallback bytecode emit below.
    if (primIndex > 0) {
        emitPrimProlog_x86(a, primIndex);
    }
    if (real) {
        int siteIdx = 0;
        for (size_t i = 0; i < bcRealLen; i++) {
            int globalIdx = (int)i + emitSkip;
            a.bind(bcLabels[globalIdx]);
            // Diagnostic: track every emit's bcOffsetFromMethObj for the
            // sortStructs corruption hunt.
            static const bool t1TraceEmit =
                std::getenv("PHARO_T1_TRACE_EMIT") != nullptr;
            if (__builtin_expect(t1TraceEmit, 0)) {
                fprintf(stderr,
                    "[T1-EMIT] i=%zu globalIdx=%d op=0x%02x "
                    "bcOffsetFromMethObj=%d siteIdx=%d\n",
                    i, globalIdx, bcReal[i],
                    bcOffsetBase + globalIdx, siteIdx);
            }
            if (!emitOne_x86(a, bcReal[i], nilBits,
                             bcOffsetBase + globalIdx, siteIdx,
                             bcLabels, globalIdx,
                             callerArgCount, callerTempCount)) {
                std::fprintf(stderr,
                    "[asmjit-t1] BUG: prescan/emit disagree at bc[%d]=0x%02x\n",
                    globalIdx, bcReal[i]);
                return false;
            }
            if (isPhase4SendOp(bcReal[i])) siteIdx++;
        }
        if (bcRealLen == 0
                || bcReal[bcRealLen-1] < SistaV1::ReturnReceiver
                || bcReal[bcRealLen-1] > SistaV1::ReturnTop) {
            a.mov(x86::dword_ptr(x86::rdi, OFF_EXIT), Imm(EXIT_SEND));
            a.ret();
        }
    } else {
        a.mov(x86::dword_ptr(x86::rdi, OFF_EXIT), Imm(EXIT_SEND));
        a.ret();
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    a64::Assembler a(&code);
    if (real) {
        bcLabels.reserve(bcLen);
        for (size_t i = 0; i < bcLen; i++) bcLabels.push_back(a.new_label());
    }
    if (primIndex > 0) {
        emitPrimProlog_arm64(a, primIndex);
    }
    if (real) {
        int siteIdx = 0;
        for (size_t i = 0; i < bcRealLen; i++) {
            int globalIdx = (int)i + emitSkip;
            uint8_t op = bcReal[i];
            a.bind(bcLabels[globalIdx]);

            // InlinedPrimitive 0xEC: 2-byte no-op.  Bind operand label
            // and skip.
            if (op == SistaV1::InlinedPrimitive) {
                a.bind(bcLabels[globalIdx + 1]);
                i++;
                continue;
            }
            // PushInteger 0xE8: push SmI((int8_t)operand) onto sp.
            if (op == SistaV1::PushInteger) {
                int8_t imm = static_cast<int8_t>(bcReal[i + 1]);
                // SmI bits: (val << 3) | 1
                uint64_t smiBits = (static_cast<uint64_t>(
                    static_cast<int64_t>(imm)) << 3) | 1ULL;
                a.mov(a64::x1, asmjit::Imm(smiBits));
                a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                a.str(a64::x1, a64::ptr(a64::x2));
                a.add(a64::x2, a64::x2, asmjit::Imm(8));
                a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                a.bind(bcLabels[globalIdx + 1]);  // operand byte's label
                i++;
                continue;
            }
            // PushCharacter 0xE9: push Character((uint8_t)operand) onto sp.
            // Character bits: (codepoint << 3) | 3 (CharacterTag).
            if (op == SistaV1::PushCharacter) {
                uint8_t cp = bcReal[i + 1];
                uint64_t charBits = (static_cast<uint64_t>(cp) << 3) | 3ULL;
                a.mov(a64::x1, asmjit::Imm(charBits));
                a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                a.str(a64::x1, a64::ptr(a64::x2));
                a.add(a64::x2, a64::x2, asmjit::Imm(8));
                a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                a.bind(bcLabels[globalIdx + 1]);
                i++;
                continue;
            }
            // PushArray 0xE7: 2-byte (opcode + desc).  Bail to
            // ExitArrayCreate so the chain loop allocates the Array
            // (and optionally pops `arraySize` elements into it).
            //
            // desc layout (per Sista V1):
            //   bits 0-6: arraySize (0..127)
            //   bit 7:    popIntoArray (if set, pop size elements)
            //
            // Chain loop's ExitArrayCreate handler reads desc from
            // state.cachedTarget and instructionPointer_ from state.ip,
            // then `instructionPointer_ += 2` advances past the
            // PushArray bytecode.
            if (op == SistaV1::PushArray) {
                uint8_t desc = bcReal[i + 1];
                a.mov(a64::x1, asmjit::Imm(static_cast<uint64_t>(desc)));
                a.str(a64::x1, a64::ptr(a64::x0, OFF_CACHED_TARGET));
                a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                a.add(a64::x5, a64::x5,
                      asmjit::Imm(bcOffsetBase + globalIdx));
                a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                a.mov(a64::w3, Imm(EXIT_ARRAY_CREATE));
                a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                a.ret(a64::x30);
                a.bind(bcLabels[globalIdx + 1]);
                i += 1;
                continue;
            }
            // ExtA/ExtB prefix bundle.  Bail at the prefix; interp
            // executes the prefix + the prefixed bytecode (which
            // consumes extA_/extB_), then continues the rest of the
            // method in interp.  Bytes consumed = 2 + nextLen
            // (prefix opcode + data + next bytecode's full length).
            if (op == SistaV1::ExtendA || op == SistaV1::ExtendB) {
                uint8_t nextOp = bcReal[i + 2];
                int nextLen = SistaV1::bytecodeLength(nextOp);
                a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                a.add(a64::x5, a64::x5,
                      asmjit::Imm(bcOffsetBase + globalIdx));
                a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                a.mov(a64::w3, Imm(EXIT_ARITH_OVERFLOW));
                a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                a.ret(a64::x30);
                // Bind labels for all bytes consumed.  Loop's ++ skips
                // the prefix opcode; we explicitly skip the rest.
                for (int k = 1; k <= 1 + nextLen; k++) {
                    if ((size_t)(globalIdx + k) < bcLabels.size()) {
                        a.bind(bcLabels[globalIdx + k]);
                    }
                }
                i += 1 + nextLen;
                continue;
            }
            // ExtSend 0xEA / ExtSuperSend 0xEB: 2-byte send.  Naked
            // (no prefix — bundle case above handles ExtA/B+ExtSend).
            // Bail to interp; chain loop returns, interp dispatches
            // the send normally and continues the rest of the method.
            if (op == SistaV1::ExtSend || op == SistaV1::ExtSuperSend) {
                a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                a.add(a64::x5, a64::x5,
                      asmjit::Imm(bcOffsetBase + globalIdx));
                a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                a.mov(a64::w3, Imm(EXIT_ARITH_OVERFLOW));
                a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                a.ret(a64::x30);
                a.bind(bcLabels[globalIdx + 1]);
                i += 1;
                continue;
            }
            // Remote temp access 0xFB/0xFC/0xFD: 3-byte ops.
            // Inline: vec = temps[vecIdx]; if vec is an Object,
            //   PushTempAtInVec:      push vec[tempIdx]
            //   StoreTempAtInVec:     vec[tempIdx] = stackTop (no pop)
            //   PopStoreTempAtInVec:  vec[tempIdx] = pop()
            // If vec is not an Object (nil), PushTempAtInVec pushes nil
            // and the stores are no-ops.  No GC write barrier yet —
            // stores from inside a closure body to a captured vector
            // typically hit eden; if surfacing as a leak, add the bit
            // 21 store barrier on the target object.
            if (op == SistaV1::PushTempAtInVec
                    || op == SistaV1::StoreTempAtInVec
                    || op == SistaV1::PopStoreTempAtInVec) {
                uint8_t tempIdx = bcReal[i + 1];
                uint8_t vecIdx  = bcReal[i + 2];
                // Counter increment.
                {
                    a.mov(a64::x14, asmjit::Imm((uint64_t)&g_bcRemoteTemp_hits));
                    a.ldr(a64::x15, a64::ptr(a64::x14));
                    a.add(a64::x15, a64::x15, asmjit::Imm(1));
                    a.str(a64::x15, a64::ptr(a64::x14));
                }
                if (op == SistaV1::PushTempAtInVec) {
                    asmjit::Label vecObj = a.new_label();
                    asmjit::Label pushDone = a.new_label();
                    a.ldr(a64::x4, a64::ptr(a64::x0, OFF_TEMPBASE));
                    a.ldr(a64::x5, a64::ptr(a64::x4, vecIdx * 8));
                    a.tst(a64::x5, asmjit::Imm(0x7));
                    a.b_eq(vecObj);                   // tag==0 → real obj
                    // Not an object — push nil.
                    a.mov(a64::x6, asmjit::Imm(nilBits));
                    a.b(pushDone);
                    a.bind(vecObj);
                    // x5 = TempVector; slot tempIdx at offset 8 + tempIdx*8.
                    a.ldr(a64::x6, a64::ptr(a64::x5, 8 + tempIdx * 8));
                    a.bind(pushDone);
                    a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.str(a64::x6, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                } else {
                    // 0xFC (store, no pop) / 0xFD (pop+store)
                    asmjit::Label vecNotObj = a.new_label();
                    a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.ldr(a64::x6, a64::ptr(a64::x2, -8));  // x6 = stackTop
                    if (op == SistaV1::PopStoreTempAtInVec) {
                        a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                        a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    }
                    a.ldr(a64::x4, a64::ptr(a64::x0, OFF_TEMPBASE));
                    a.ldr(a64::x5, a64::ptr(a64::x4, vecIdx * 8));
                    a.tst(a64::x5, asmjit::Imm(0x7));
                    a.b_ne(vecNotObj);                // not Object → skip store
                    a.str(a64::x6, a64::ptr(a64::x5, 8 + tempIdx * 8));
                    a.bind(vecNotObj);
                }
                a.bind(bcLabels[globalIdx + 1]);
                a.bind(bcLabels[globalIdx + 2]);
                i += 2;
                continue;
            }
            // PushFullBlock 0xF9: 3-byte (opcode + LL + HH).  Bail to
            // ExitBlockCreate so the chain loop allocates the
            // FullBlockClosure.  state.cachedTarget = (LL | HH << 32)
            // matches JITState.hpp:160's packed format.
            if (op == SistaV1::PushFullBlock) {
                uint8_t litIdx = bcReal[i + 1];
                uint8_t flags  = bcReal[i + 2];
                uint64_t packed = static_cast<uint64_t>(litIdx)
                                | (static_cast<uint64_t>(flags) << 32);
                a.mov(a64::x1, asmjit::Imm(packed));
                a.str(a64::x1, a64::ptr(a64::x0, OFF_CACHED_TARGET));
                a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                a.add(a64::x5, a64::x5,
                      asmjit::Imm(bcOffsetBase + globalIdx));
                a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                a.mov(a64::w3, Imm(EXIT_BLOCK_CREATE));
                a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                a.ret(a64::x30);
                a.bind(bcLabels[globalIdx + 1]);
                a.bind(bcLabels[globalIdx + 2]);
                i += 2;
                continue;
            }
            // Extended push/store with 1-byte index operand.
            //   ExtPushRecvVar 0xE2:   push recv.slot[index]
            //   ExtPushLitVar 0xE3:    push literals[index].slot[1] (assoc val)
            //   ExtPushLitConst 0xE4:  push literals[index]
            //   ExtPushTemp 0xE5:      push tempBase[index]
            //   ExtPopStoreRecv 0xF0:  pop, store recv.slot[index] (immut check)
            //   ExtPopStoreTemp 0xF2:  pop, store tempBase[index]
            //   ExtStoreRecv 0xF3:     store TOS to recv.slot[index] (immut check)
            //   ExtStoreTemp 0xF5:     store TOS (no pop) to tempBase[index]
            // ExtA/ExtB prefixes not yet supported — index is just the
            // single operand byte, so this covers index 0-255.
            if (op == SistaV1::ExtPushRecvVar
                    || op == SistaV1::ExtPushLitVar
                    || op == SistaV1::ExtPushLitConst
                    || op == SistaV1::ExtPushTemp
                    || op == SistaV1::ExtPopStoreTemp
                    || op == SistaV1::ExtStoreTemp
                    || op == SistaV1::ExtPopStoreRecv
                    || op == SistaV1::ExtStoreRecv
                    || op == SistaV1::ExtPopStoreLitVar
                    || op == SistaV1::ExtStoreLitVar) {
                int idx = bcReal[i + 1];
                if (op == SistaV1::ExtPushRecvVar) {
                    a.ldr(a64::x1, a64::ptr(a64::x0, OFF_RECEIVER));
                    a.ldr(a64::x1, a64::ptr(a64::x1, OBJ_SLOT_0 + idx * 8));
                    a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                } else if (op == SistaV1::ExtPushLitConst) {
                    a.ldr(a64::x1, a64::ptr(a64::x0, OFF_LITERALS));
                    a.ldr(a64::x1, a64::ptr(a64::x1, idx * 8));
                    a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                } else if (op == SistaV1::ExtPushLitVar) {
                    a.ldr(a64::x1, a64::ptr(a64::x0, OFF_LITERALS));
                    a.ldr(a64::x1, a64::ptr(a64::x1, idx * 8));
                    a.ldr(a64::x1, a64::ptr(a64::x1, OBJ_SLOT_0 + 8));
                    a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                } else if (op == SistaV1::ExtPushTemp) {
                    a.ldr(a64::x1, a64::ptr(a64::x0, OFF_TEMPBASE));
                    a.ldr(a64::x1, a64::ptr(a64::x1, idx * 8));
                    a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.str(a64::x1, a64::ptr(a64::x2));
                    a.add(a64::x2, a64::x2, asmjit::Imm(8));
                    a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                } else if (op == SistaV1::ExtPopStoreTemp) {
                    a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                    a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.ldr(a64::x1, a64::ptr(a64::x2));
                    a.ldr(a64::x4, a64::ptr(a64::x0, OFF_TEMPBASE));
                    a.str(a64::x1, a64::ptr(a64::x4, idx * 8));
                } else if (op == SistaV1::ExtStoreTemp) {
                    a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.ldur(a64::x1, asmjit::a64::ptr(a64::x2, -8));
                    a.ldr(a64::x4, a64::ptr(a64::x0, OFF_TEMPBASE));
                    a.str(a64::x1, a64::ptr(a64::x4, idx * 8));
                } else if (op == SistaV1::ExtPopStoreLitVar
                        || op == SistaV1::ExtStoreLitVar) {
                    // Literal var (Association) store.  literals[idx]
                    // is an Association; its slot 1 is the .value
                    // slot.  No write barrier — YG scavenge scans
                    // all of old space.
                    if (op == SistaV1::ExtPopStoreLitVar) {
                        a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                        a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                        a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                        a.ldr(a64::x1, a64::ptr(a64::x2));
                    } else /* ExtStoreLitVar */ {
                        a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                        a.ldur(a64::x1, asmjit::a64::ptr(a64::x2, -8));
                    }
                    a.ldr(a64::x4, a64::ptr(a64::x0, OFF_LITERALS));
                    a.ldr(a64::x4, a64::ptr(a64::x4, idx * 8));
                    a.str(a64::x1, a64::ptr(a64::x4, OBJ_SLOT_0 + 8));
                } else if (op == SistaV1::ExtPopStoreRecv
                        || op == SistaV1::ExtStoreRecv) {
                    // Receiver store with immutable-bit check (mirror
                    // popStoreRecv 0xC8-CF emit at line 1545).  If the
                    // receiver's header bit 23 is set, bail to interp.
                    asmjit::Label bail = a.new_label();
                    asmjit::Label end  = a.new_label();
                    a.ldr(a64::x4, a64::ptr(a64::x0, OFF_RECEIVER));
                    a.ldr(a64::w5, a64::ptr(a64::x4));
                    a.tst(a64::w5, asmjit::Imm(0x800000));
                    a.b_ne(bail);
                    if (op == SistaV1::ExtPopStoreRecv) {
                        a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                        a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                        a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                        a.ldr(a64::x1, a64::ptr(a64::x2));
                    } else /* ExtStoreRecv */ {
                        a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                        a.ldur(a64::x1, asmjit::a64::ptr(a64::x2, -8));
                    }
                    a.str(a64::x1, a64::ptr(a64::x4, OBJ_SLOT_0 + idx * 8));
                    a.b(end);
                    a.bind(bail);
                    a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                    a.add(a64::x5, a64::x5,
                          asmjit::Imm(bcOffsetBase + globalIdx));
                    a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                    a.mov(a64::w3, Imm(EXIT_ARITH_OVERFLOW));
                    a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                    a.ret(a64::x30);
                    a.bind(end);
                }
                a.bind(bcLabels[globalIdx + 1]);
                i++;
                continue;
            }
            // Long jumps 0xED/0xEE/0xEF: 2-byte opcode + signed 8-bit
            // offset.  Handled in the main loop so we can read the
            // operand byte without changing emitOne_arm64's signature.
            // Target = (globalIdx + 2) + offset.
            if (op == SistaV1::ExtJump
                    || op == SistaV1::ExtJumpTrue
                    || op == SistaV1::ExtJumpFalse) {
                int8_t offset = static_cast<int8_t>(bcReal[i + 1]);
                int target = globalIdx + 2 + offset;
                // Out-of-range target = unreachable trailer byte; emit a bail.
                if (target < 0 || target >= (int)bcLabels.size()) {
                    a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                    a.add(a64::x5, a64::x5,
                          asmjit::Imm(bcOffsetBase + globalIdx));
                    a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                    a.mov(a64::w3, Imm(EXIT_SEND));
                    a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                    a.ret(a64::x30);
                    a.bind(bcLabels[globalIdx + 1]);
                    i++;
                    continue;
                }
                if (op == SistaV1::ExtJump) {
                    a.b(bcLabels[target]);
                } else {
                    // Conditional long jump: same structure as the
                    // short conditional jump emit (~line 1777) but
                    // with target from the operand byte.
                    bool jumpOnTrue = (op == SistaV1::ExtJumpTrue);
                    asmjit::Label mustBoolBail = a.new_label();
                    asmjit::Label takeBranch   = a.new_label();
                    asmjit::Label fallThrough  = a.new_label();
                    a.ldr(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.ldur(a64::x1, asmjit::a64::ptr(a64::x2, -8));
                    a.ldp(a64::x4, a64::x5,
                          a64::ptr(a64::x0, OFF_TRUEOOP));
                    a.cmp(a64::x1, a64::x4);
                    a.b_eq(jumpOnTrue ? takeBranch : fallThrough);
                    a.cmp(a64::x1, a64::x5);
                    a.b_ne(mustBoolBail);
                    a.b(jumpOnTrue ? fallThrough : takeBranch);
                    a.bind(takeBranch);
                    a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                    a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.b(bcLabels[target]);
                    a.bind(fallThrough);
                    a.sub(a64::x2, a64::x2, asmjit::Imm(8));
                    a.str(a64::x2, a64::ptr(a64::x0, OFF_SP));
                    a.b(bcLabels[globalIdx + 2]);  // next bytecode
                    a.bind(mustBoolBail);
                    a.ldr(a64::x5, a64::ptr(a64::x0, OFF_METHOD));
                    a.add(a64::x5, a64::x5,
                          asmjit::Imm(bcOffsetBase + globalIdx));
                    a.str(a64::x5, a64::ptr(a64::x0, OFF_IP));
                    a.mov(a64::w3, Imm(EXIT_MUST_BOOL));
                    a.str(a64::w3, a64::ptr(a64::x0, OFF_EXIT));
                    a.ret(a64::x30);
                }
                // Bind operand byte's label to current PC so any
                // jump targeting it doesn't dangle at finalize.
                a.bind(bcLabels[globalIdx + 1]);
                i++;  // skip operand byte (loop's i++ advances past it)
                continue;
            }

            if (!emitOne_arm64(a, op, nilBits,
                                bcOffsetBase + globalIdx, siteIdx,
                                bcLabels, globalIdx,
                                callerArgCount, callerTempCount)) {
                std::fprintf(stderr,
                    "[asmjit-t1] BUG: prescan/emit disagree at bc[%d]=0x%02x\n",
                    globalIdx, op);
                return false;
            }
            if (isPhase4SendOp(op)) siteIdx++;
        }
        if (bcRealLen == 0
                || bcReal[bcRealLen-1] < SistaV1::ReturnReceiver
                || bcReal[bcRealLen-1] > SistaV1::ReturnTop) {
            a.mov(a64::w1, Imm(EXIT_SEND));
            a.str(a64::w1, a64::ptr(a64::x0, OFF_EXIT));
            a.ret(a64::x30);
        }
    } else {
        a.mov(a64::w1, Imm(EXIT_SEND));
        a.str(a64::w1, a64::ptr(a64::x0, OFF_EXIT));
        a.ret(a64::x30);
    }
#endif

    err = code.flatten();
    if (err != kErrorOk) return false;

    // Fill bcToCodeOut from bound labels.  bcToCodeOut[bcLen] is the
    // end-of-machine-code offset (used by findMethodByPC etc.).
    // Per JITMethod.hpp contract: bcToCode[i]==0 means "not a valid
    // re-entry point"; bcToCode[0] is conventionally 0 (initial entry
    // goes through codeStart() directly).
    //
    // PHARO_ASMJIT_T1_BCTOCODE_ZERO=1: write all zeros except [bcLen]
    // (the end-of-mc sentinel).  Chain loop's resume check
    // `if (codeOff == 0 || codeOff >= codeSize) bail;` then always
    // bails to interp — effectively disabling JIT-side resume while
    // still advertising numBytecodes.  Bisect helper.
    static const bool zeroBcToCode =
        std::getenv("PHARO_ASMJIT_T1_BCTOCODE_ZERO") != nullptr;
    if (bcToCodeOut) {
        if (real && !zeroBcToCode) {
            for (size_t i = 0; i < bcLen; i++) {
                uint32_t off = (uint32_t)code.label_offset_from_base(bcLabels[i]);
                // Per contract, slot 0 is conventionally 0 (initial entry
                // goes through codeStart() directly).
                bcToCodeOut[i] = (i == 0) ? 0u : off;
            }
        } else {
            // Stub-only OR zeroBcToCode: no per-bytecode entry points.
            for (size_t i = 0; i < bcLen; i++) bcToCodeOut[i] = 0;
        }
        // bcToCodeOut[bcLen] is set by the caller to the emitted size.
    }
    size_t total = code.code_size();
    if (total == 0 || total > outCap) {
        std::fprintf(stderr,
                     "[asmjit-t1] code.code_size=%zu out of [1, %zu]\n",
                     total, outCap);
        return false;
    }
    err = code.copy_flattened_data(out, outCap, CopySectionFlags::kPadSectionBuffer);
    if (err != kErrorOk) return false;
    *outSize = total;
    *isReal = real;
    return true;
}

}  // namespace

JITMethod* compileViaAsmjit(CodeZone& zone, MethodMap& methodMap,
                             ObjectMemory& memory, Interpreter& interp,
                             Oop compiledMethod) {
    (void)interp;

    if (!compiledMethod.isObject() || compiledMethod.rawBits() < 0x10000) {
        g_failed++;
        return nullptr;
    }
    ObjectHeader* methObj = compiledMethod.asObjectPtr();
    Oop headerOop = methObj->slotAt(0);
    if (!headerOop.isSmallInteger()) {
        g_failed++;
        return nullptr;
    }
    int64_t headerBits = headerOop.asSmallInteger();
    int  numLiterals = static_cast<int>(headerBits & 0x7FFF);
    bool hasPrimitive = (headerBits >> 16) & 1;

    uint8_t* bytes = methObj->bytes();
    size_t bcStart = (1 + numLiterals) * 8;
    size_t totalBytes = methObj->slotCount() * 8;
    uint8_t fmt = static_cast<uint8_t>(methObj->format());
    int unusedBytes = (fmt >= 24) ? (fmt - 24) : 0;
    if (bcStart + (size_t)unusedBytes >= totalBytes) {
        g_failed++;
        return nullptr;
    }
    size_t bcLenRaw = totalBytes - bcStart - (size_t)unusedBytes;
    const uint8_t* bc = bytes + bcStart;

    // Detect a supported primitive prologue.  If hasPrimitive is set
    // but the primitive isn't in our supported set (or the first 3
    // bytes aren't CallPrimitive), bail — we'd otherwise route through
    // a bail-stub which doesn't try the prim, and the chain loop's
    // inline-activation gate (Interpreter.cpp:18731-18732) blocks
    // hasPrimPrologue=false callees with primitives anyway.  Bail to
    // C++ so the prim runs via the standard activateMethod path.
    int primIdx = -1;
    if (hasPrimitive) {
        primIdx = supportedPrimIndex(bc, bcLenRaw);
        if (primIdx < 0) {
            // Unsupported prim: bail compile, let C++ handle it.
            g_failed++;
            return nullptr;
        }
    }
    // g_debug.t1SkipSelectors: comma-separated selector list to reject
    // from real-emit (compile as stub-on-entry).  Unlike index-based
    // bisects, selectors are stable across runs even when compile
    // order is non-deterministic.  Set via PHARO_T1_SKIP_SELECTORS=
    // "addTemp:,methodClass,validate" etc.
    if (g_debug.t1SkipSelectors) {
        std::string sel = memory.selectorOf(compiledMethod);
        const char* list = g_debug.t1SkipSelectors;
        size_t selLen = sel.size();
        for (const char* p = list; *p; ) {
            const char* end = std::strchr(p, ',');
            size_t len = end ? (size_t)(end - p) : std::strlen(p);
            if (len == selLen && std::memcmp(p, sel.c_str(), len) == 0) {
                g_failed++;
                return nullptr;
            }
            p = end ? end + 1 : p + len;
        }
    }
    // Block JIT-compile bisect knobs.
    //   PHARO_T1_NO_BLOCKS=1            — reject every CompiledBlock.
    //   PHARO_T1_BLOCKS_FIRST_N=K       — accept the first K block
    //                                     compiles, reject the rest.
    //   PHARO_T1_BLOCKS_ONLY_N=K        — accept only block index K
    //                                     (1-based; 0 = none).
    //   PHARO_T1_BLOCKS_SKIP_FROM=A
    //   PHARO_T1_BLOCKS_SKIP_TO=B       — reject blocks in range [A,B]
    //                                     inclusive (combine with FIRST_N
    //                                     to bisect partner blocks).
    //   PHARO_T1_BLOCKS_TRACE=1         — log every block compile with
    //                                     bcLen + first 16 bytecodes.
    {
        uint32_t cls = methObj->classIndex();
        if (cls == interp.compiledBlockClassIndex()) {
            static int blockCount = 0;
            blockCount++;
            bool reject = false;
            if (g_debug.t1NoBlocks) reject = true;
            if (g_debug.t1BlocksFirstN >= 0
                    && blockCount > g_debug.t1BlocksFirstN) reject = true;
            if (g_debug.t1BlocksOnlyN >= 0
                    && blockCount != g_debug.t1BlocksOnlyN) reject = true;
            if (g_debug.t1BlocksSkipFrom >= 0
                    && g_debug.t1BlocksSkipTo >= 0
                    && blockCount >= g_debug.t1BlocksSkipFrom
                    && blockCount <= g_debug.t1BlocksSkipTo) {
                reject = true;
            }
            if (g_debug.t1BlocksTrace) {
                fprintf(stderr,
                        "[T1-BLOCK] #%d oop=0x%llx bcLen=%zu %s bc=",
                        blockCount,
                        (unsigned long long)compiledMethod.rawBits(),
                        bcLenRaw, reject ? "REJECT" : "accept");
                for (size_t bi = 0; bi < bcLenRaw && bi < 16; bi++)
                    fprintf(stderr, "%02x ", bc[bi]);
                fprintf(stderr, "\n");
            }
            if (reject) {
                g_failed++;
                return nullptr;
            }
        }
    }
    // Trim trailer bytes past the method's last unconditional return,
    // mirroring the stencil decoder's logic (JITCompiler.cpp:639-660):
    // post-return bytes are dead code — selector/temp-name oop trailers
    // that the image packs into CompiledMethod.bytes() — and treating
    // them as bytecodes would pollute the pre-scan.
    //
    // Set PHARO_ASMJIT_T1_NO_TRIM=1 to disable for bisection.
    static const bool noTrim =
        std::getenv("PHARO_ASMJIT_T1_NO_TRIM") != nullptr;
    size_t bcLen = noTrim ? bcLenRaw : computeLiveLength(bc, bcLenRaw);
    // Diagnostic for the sortStructs:into: corruption hunt.
    if (__builtin_expect(g_debug.sortstrWatch, 0)) {
        std::string sel = memory.selectorOf(compiledMethod);
        if (sel == "startup:" || sel == "registeredClass") {
            static size_t cCount = 0;
            cCount++;
            fprintf(stderr,
                "[T1-COMPILE-TRACE #%zu] #%s bcLenRaw=%zu bcLen=%zu "
                "method_oop=0x%llx bytes:",
                cCount, sel.c_str(),
                bcLenRaw, bcLen,
                (unsigned long long)compiledMethod.rawBits());
            for (size_t bi = 0; bi < bcLenRaw && bi < 32; bi++) {
                fprintf(stderr, " %02x", bc[bi]);
            }
            fprintf(stderr, "\n");
        }
    }

    // Buffer for emitted bytes.  Send emit (1-slot IC probe + miss path)
    // takes ~25 instructions ≈ 100 bytes; arith ≈ 70 bytes; pushes ≈ 30
    // bytes.  Inline-J2J emit (PHARO_T1_INLINE_J2J=1) adds ~200 bytes per
    // send site.  Raised from 128 to 512 bytes/bytecode (2026-05-17) to
    // accommodate inline-J2J + per-bail counters + return prelude.
    size_t cap = bcLen * 512 + 512;
    if (cap > 65536) cap = 65536;
    if (bcLen * 512 + 512 > cap) {
        g_failed++;
        return nullptr;
    }
    std::vector<uint8_t> buf(cap);
    size_t emitted = 0;
    bool   isReal  = false;
    uint64_t nilBits = memory.nil().rawBits();
    // Offset of bc[0] from the CompiledMethod object's address.
    //   methObj layout:  [ObjectHeader 8B][slot 0 = header][slot 1..N = lits][bytes...]
    //   bc[0] address  = methObj + 8 (header) + 8 * (1 + numLiterals)
    // Bail emit uses `state.method.rawBits() + (bcOffsetBase + i)` so
    // state.ip survives GC compaction (the alternative — baking the
    // absolute bytecode address — dangles when the method moves).
    int bcOffsetBase = 8 + (int)((1 + numLiterals) * 8);
    // bcToCode: per-bytecode emit start within the JIT code.  Filled
    // by emitMethodBytes from per-bytecode labels.  Slot [bcLen] gets
    // the end-of-machine-code offset after emit.
    std::vector<uint32_t> bcToCode(bcLen + 1, 0);
    // Caller method's argCount/tempCount — needed by the inline-J2J
    // emit's self-recursive callee-setup so it can skip the dynamic
    // tempCount load + init-loop overhead when tempCount is known at
    // compile time.
    int callerArgCount  = (int)((headerBits >> 24) & 0x0F);
    int callerTempCount = (int)((headerBits >> 18) & 0x3F);
    if (!emitMethodBytes(bc, bcLen, nilBits, bcOffsetBase, primIdx,
                         callerArgCount, callerTempCount,
                         buf.data(), cap, &emitted, &isReal,
                         bcToCode.data())) {
        g_failed++;
        return nullptr;
    }
    bcToCode[bcLen] = (uint32_t)emitted;

    // Count send sites and compute the IC layout.  Each single-byte
    // send opcode (0x70..0xAF) gets one IC site.
    uint16_t numSendSites = 0;
    if (isReal) {
        for (size_t i = 0; i < bcLen; i++) {
            if (isPhase4SendOp(bc[i])) numSendSites++;
        }
    }

    // Payload layout after the JITMethod header:
    //   [machine code, `emitted` bytes]
    //   [pad to 4-byte align]
    //   [bcToCode table, (bcLen+1)*4 bytes]
    //   [pad to 8-byte align]
    //   [selBitsArray, numSendSites*8 bytes]
    // codeSize passed to allocate() is the FULL payload size.
    uint32_t bcToCodeTableOffset =
        (uint32_t)((emitted + 3u) & ~3u);
    uint32_t bcToCodeTableSize   =
        (uint32_t)((bcLen + 1) * sizeof(uint32_t));
    uint32_t selBitsArrayOffset  =
        (bcToCodeTableOffset + bcToCodeTableSize + 7u) & ~7u;
    uint32_t selBitsArraySize    =
        (uint32_t)(numSendSites * sizeof(uint64_t));
    uint32_t payloadSize = selBitsArrayOffset + selBitsArraySize;

    // Allocate the JITMethod with full payload + IC sites.  CodeZone
    // calloc()s a heap-side icBuffer of numSendSites*IC_BYTES_PER_SITE.
    JITMethod* jm = zone.allocate(payloadSize, numSendSites);
    if (!jm) {
        g_failed++;
        return nullptr;
    }

    jm->compiledMethodOop = compiledMethod.rawBits();
    jm->methodHeader      = static_cast<uint64_t>(headerBits);
    // Pre-compute bcStart so the inline-J2J emit can read it in one load
    // instead of recomputing per send.  Mirrors JITMethod::bcStart() —
    // depends only on the now-set compiledMethodOop + methodHeader.
    {
        uint64_t numLits = jm->methodHeader & 0x7FFFu;
        jm->bcStartCache = jm->compiledMethodOop + (2 + numLits) * 8;
    }
    jm->argCount          = static_cast<uint8_t>((headerBits >> 24) & 0x0F);
    jm->tempCount         = static_cast<uint8_t>((headerBits >> 18) & 0x3F);
    // numBytecodes is the source-bytecode count for the live region.
    // The runtime indexes bcToCodeTable() via this — `bcToCode[i] = 0`
    // for i where re-entry is invalid (the convention; entry-by-default
    // is via codeStart()).  slot [numBytecodes] holds the
    // end-of-machine-code offset for findMethodByPC.
    // 4b.2: advertise numBytecodes + bcToCodeTableOffset for chain-loop
    // resume — but ONLY for methods that contain no sends.  Methods
    // with sends fail post-resume with mustBeBoolean cascades; the
    // protocol mismatch is not yet understood (see plan_asmjit_replacement.md
    // §"Phase 4b.2 resume protocol gap").  Send-free methods are safe
    // to resume because there's no inline activation in their flow.
    //
    // 2026-05-15: tried turning this on (PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1)
    // plus syncing state.method at J2J call/return.  Still breaks the
    // differential fuzzer (every test JIT_DIFF, MUSTBOOL cascade in
    // #encoderClass, eventual stack overflow).  More than state.method
    // is desync'd at the trampoline — investigation deferred.
    //
    // Bisect knobs (default = off):
    //   PHARO_ASMJIT_T1_NO_BCTOCODE=1   — never advertise bcToCode
    //   PHARO_ASMJIT_T1_NO_NUMBC=1      — never advertise numBytecodes
    //   PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1
    //                                   — advertise resume even for
    //                                     send-containing methods (BROKEN)
    static const bool noBcToCode =
        std::getenv("PHARO_ASMJIT_T1_NO_BCTOCODE") != nullptr;
    static const bool noNumBc =
        std::getenv("PHARO_ASMJIT_T1_NO_NUMBC") != nullptr;
    static const bool forceResumeForSends =
        std::getenv("PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS") != nullptr;
    bool advertiseResume = isReal && !noNumBc && !noBcToCode && bcLen > 0;
    if (numSendSites > 0 && !forceResumeForSends) advertiseResume = false;
    jm->numBytecodes      = advertiseResume ? (uint16_t)bcLen : 0;
    jm->numICEntries      = numSendSites;
    jm->bcToCodeTableOffset = advertiseResume ? bcToCodeTableOffset : 0;
    jm->selBitsArrayOffset =
        numSendSites > 0 ? selBitsArrayOffset : 0;
    jm->tier              = 1;
    // hasPrimPrologue = true means the chain loop's inline-activation
    // path (Interpreter.cpp:18731-18732) will activate this method
    // without a separate C++ primitive try.  Our prologue does the
    // SmI fast path inline and rets on success; on failure falls
    // through to the bytecode emit.
    jm->hasPrimPrologue   = (primIdx > 0);
    jm->isBlock           = false;
    jm->pinned            = false;
    jm->hasSends          = false;
    jm->hasHeapWrites     = false;
    jm->hasRecvFieldAccess= false;
    jm->hasRecvFieldWrite = false;
    jm->hasLitVarWrite    = false;
    jm->maxRecvFieldIndex = 0;
    jm->isSpliceTarget    = false;
    jm->isStubOnEntry     = !isReal;
    // canBailMidMethod = true when the emitter produces a mid-method
    // ExitMustBool bail (conditional jumps).  The chain loop's
    // inline-activate path (Interpreter.cpp:18807-18809) skips this
    // method when set; the activateMethod-recursive path is used
    // instead, which pushes a C++ frame the bail can return into.
    // Only real-emit methods can bail mid-method.  Stub-on-entry methods
    // (isReal=false) bail with ExitSend on entry — no mid-method bail, so
    // the inline-activate gate doesn't need to block them.
    {
        bool hasCondJump = false;
        if (isReal) {
            for (size_t i = 0; i < bcLen; ) {
                uint8_t op = bc[i];
                if (SistaV1::isConditionalShortJump(op)) { hasCondJump = true; break; }
                int len = SistaV1::bytecodeLength(op);
                if (len <= 0 || i + (size_t)len > bcLen) break;
                i += (size_t)len;
            }
        }
        jm->canBailMidMethod = hasCondJump;
        // g_debug.t1ForceBailMid (set by PHARO_T1_FORCE_BAIL_MID or
        // PHARO_T1_FORCE_SIMPLE) — force canBailMidMethod on every
        // method to disable the chain-loop inline-activate fast path.
        if (g_debug.t1ForceBailMid) {
            jm->canBailMidMethod = true;
        }
        if (hasCondJump) {
            g_condJumpRealCompiles++;
            if (std::getenv("PHARO_ASMJIT_T1_TRACE_COND")) {
                fprintf(stderr,
                        "[T1-COND-COMPILE] #%zu sel=#%s bcLen=%zu oop=0x%llx bc=",
                        g_condJumpRealCompiles,
                        memory.selectorOf(compiledMethod).c_str(),
                        bcLen, (unsigned long long)compiledMethod.rawBits());
                for (size_t bi = 0; bi < bcLen && bi < 32; bi++)
                    fprintf(stderr, "%02x ", bc[bi]);
                fprintf(stderr, "primIdx=%d argCount=%d tempCount=%d advRes=%d "
                        "numBC=%u\n",
                        primIdx, jm->argCount, jm->tempCount,
                        (int)advertiseResume, jm->numBytecodes);
                fprintf(stderr, "  bcToCode:");
                for (size_t bi = 0; bi <= bcLen && bi < 32; bi++)
                    fprintf(stderr, " [%zu]=%u", bi, bcToCode[bi]);
                fprintf(stderr, "\n");
            }
        }
        // PHARO_SORTSTR_WATCH=1: dump bcToCode for #isEmpty + #size, the
        // methods involved in sortStructs:into:'s failing chain.  Tells us
        // whether the bytecode→code mapping is plausible (e.g., whether
        // bcToCode[2] for #isEmpty's PushZero is distinct from bcToCode[4]
        // for ReturnTop).
        if (g_debug.sortstrWatch && isReal && bcLen > 0) {
            std::string sel = memory.selectorOf(compiledMethod);
            if (sel == "isEmpty" || sel == "size") {
                fprintf(stderr,
                        "[T1-COMPILE-DBG] sel=#%s bcLen=%zu emitted=%zu "
                        "oop=0x%llx primIdx=%d numBC=%u bc=",
                        sel.c_str(), bcLen, emitted,
                        (unsigned long long)compiledMethod.rawBits(),
                        primIdx, jm->numBytecodes);
                for (size_t bi = 0; bi < bcLen && bi < 32; bi++)
                    fprintf(stderr, "%02x ", bc[bi]);
                fprintf(stderr, "\n  bcToCode:");
                for (size_t bi = 0; bi <= bcLen && bi < 32; bi++)
                    fprintf(stderr, " [%zu]=%u", bi, bcToCode[bi]);
                fprintf(stderr, "\n");
            }
        }
    }

    std::memcpy(jm->codeStart(), buf.data(), emitted);
    // Write bcToCode table after the machine code.  Required by the
    // chain loop (Interpreter.cpp:18062-18207) to compute resume
    // offsets after callee returns.  Skipped for stub-only methods
    // (numBytecodes = 0; bcToCodeTableOffset = 0 — no table).
    if (isReal && bcLen > 0) {
        uint32_t* tbl = reinterpret_cast<uint32_t*>(
            jm->codeStart() + bcToCodeTableOffset);
        for (size_t i = 0; i <= bcLen; i++) tbl[i] = bcToCode[i];
    }
    // Write selBitsArray (per-send selector Symbol Oop).  Used by
    // jit_rt_ic_miss after GC to recover the selector when the
    // in-IC slot[18] has been GC-zeroed (Task #41).
    if (numSendSites > 0) {
        uint64_t* sba = reinterpret_cast<uint64_t*>(
            jm->codeStart() + selBitsArrayOffset);
        uint16_t siteIdx = 0;
        Oop* literals = methObj->slots() + 1;
        Oop ssArrayOop = memory.specialObject(
            SpecialObjectIndex::SpecialSelectorsArray);
        ObjectHeader* ssHdr = (ssArrayOop.isObject()
                               && ssArrayOop.rawBits() > 0x10000)
                              ? ssArrayOop.asObjectPtr() : nullptr;
        for (size_t i = 0; i < bcLen; i++) {
            uint8_t op = bc[i];
            if (!isPhase4SendOp(op)) continue;
            uint64_t selBits = 0;
            if (op >= 0x60 && op <= 0x6F) {
                // Binary special selectors: ssArray[(op - 0x60) * 2].
                // Only 0x69 /, 0x6A \\, 0x6B @, 0x6D // reach here —
                // the others have Phase 3 inline emits.
                if (ssHdr) {
                    size_t slot = (size_t)(op - 0x60) * 2;
                    if (slot < ssHdr->slotCount()) {
                        selBits = ssHdr->slotAt(slot).rawBits();
                    }
                }
            } else if (op >= 0x70 && op <= 0x7F) {
                // Special selector: ssArray[(op - 0x70 + 16) * 2].
                // Slot 0 = selector, slot 1 = nArgs (we don't need
                // nArgs here — the runtime gets it from the bytecode).
                if (ssHdr) {
                    size_t slot = (size_t)((op - 0x70) + 16) * 2;
                    if (slot < ssHdr->slotCount()) {
                        selBits = ssHdr->slotAt(slot).rawBits();
                    }
                }
            } else {
                // Literal send 0/1/2 args: selector = literals[op & 0x0F].
                int litIdx = op & 0x0F;
                if (litIdx < numLiterals) {
                    selBits = literals[litIdx].rawBits();
                }
            }
            sba[siteIdx++] = selBits;
            // Also seed the in-IC slot[18] (selectorBits) so the
            // stencil-style probe contract works.  recoverAfterGC
            // zeros this on compaction; jit_rt_ic_miss recovers from
            // sba in that case.
            if (jm->icBuffer) {
                uint8_t* icStart = jm->icZoneStart();
                uint64_t* siteSlots = reinterpret_cast<uint64_t*>(
                    icStart + (siteIdx - 1) * IC_BYTES_PER_SITE);
                siteSlots[IC_SELBITS_SLOT] = selBits;
            }
        }
    }
    platform::flushICache(jm->codeStart(), emitted);
    jm->state = MethodState::Compiled;
    platform::makeExecutable(jm, jm->totalSize);

    methodMap.insert(compiledMethod.rawBits(), jm);

    g_compiled++;
    if (isReal) g_compiledReal++;
    else        g_compiledStub++;

    const bool trace = g_debug.useAsmjitT1Trace;
    bool emitTrace = trace && (g_compiled <= 10 || (g_compiled % 100 == 0)
                                || (isReal && g_compiledReal <= 30));
    if (emitTrace) {
        std::fprintf(stderr,
                     "[asmjit-t1] #%zu (%s) compiled %llu -> jm=%p code=%p (%zu bytes, %zu bc)\n",
                     g_compiled, isReal ? "real" : "stub",
                     static_cast<unsigned long long>(compiledMethod.rawBits()),
                     (void*)jm, (void*)jm->codeStart(), emitted, bcLen);
    }
    // PHARO_T1_DUMP_SEL=<selector>: dump raw emitted bytes for the
    // named method to /tmp/jit_<sel>.bin (overwrites).  Useful for
    // objdump-ing the exact code that ran.
    if (const char* dumpSel = g_debug.t1DumpSel) {
        std::string sel = memory.selectorOf(compiledMethod);
        if (sel == dumpSel) {
            static int dumpIdx = 0;
            dumpIdx++;
            std::string path = std::string("/tmp/jit_") + sel
                + "_" + std::to_string(dumpIdx) + ".bin";
            // Sanitize: replace ':' with '_' for filesystem-friendly name.
            for (auto& c : path) if (c == ':') c = '_';
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(jm->codeStart(), 1, emitted, f);
                std::fclose(f);
                std::fprintf(stderr,
                             "[T1-DUMP] #%d wrote %zu bytes of #%s oop=0x%llx bcLen=%zu bc=",
                             dumpIdx, emitted, sel.c_str(),
                             (unsigned long long)compiledMethod.rawBits(),
                             bcLen);
                for (size_t bi = 0; bi < bcLen && bi < 32; bi++)
                    std::fprintf(stderr, "%02x ", bc[bi]);
                std::fprintf(stderr, "isReal=%d\n", (int)isReal);
            }
        }
    }

    return jm;
}

size_t asmjitT1Compiled() { return g_compiled; }
size_t asmjitT1Failed()   { return g_failed;   }

}  // namespace jit
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
