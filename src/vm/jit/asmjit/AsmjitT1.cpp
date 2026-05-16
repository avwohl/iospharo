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

namespace {

// JITState field offsets — guarded by static_assert in JITState.hpp.
constexpr int OFF_SP             = 0;
constexpr int OFF_RECEIVER       = 8;
constexpr int OFF_LITERALS       = 16;
constexpr int OFF_TEMPBASE       = 24;
constexpr int OFF_IP             = 48;
constexpr int OFF_JITMETHOD      = 56;
constexpr int OFF_METHOD         = 64;
constexpr int OFF_EXIT           = 76;
constexpr int OFF_RETVAL         = 80;
constexpr int OFF_CACHED_TARGET  = 88;
constexpr int OFF_ICDATAPTR      = 96;
constexpr int OFF_SENDARGCOUNT   = 104;
constexpr int OFF_TRUEOOP        = 128;
constexpr int OFF_FALSEOOP       = 136;

// ExitReason values (JITState.hpp).
constexpr int EXIT_RETURN          = 1;
constexpr int EXIT_SEND            = 2;
constexpr int EXIT_ARITH_OVERFLOW  = 6;
constexpr int EXIT_SEND_CACHED     = 7;
constexpr int EXIT_MUST_BOOL       = 12;

// nArgs per special selector 0x70..0x7F (index = op - 0x70).
//   at: at:put: size next nextPut: atEnd == class ~~ value value: do: new new: x y
constexpr uint8_t kSpecialNArgs[16] =
    {1,2,0,0,1,0,1,0,1,0,1,1,0,1,0,0};
inline int sendNArgs(uint8_t op) {
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
            maxBranchTarget = (int)bcLen;  // unknown forward, conservative
        }
        i += (size_t)len;
        // Stop at first unconditional return if no branches point past us.
        if (SistaV1::isReturn(op) && (int)i > maxBranchTarget) {
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
    // Skip arith range (0x60..0x6F) — those have their own fast path.
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
        if (op >= SistaV1::ReturnReceiver
                && op <= SistaV1::ReturnTop) continue; // 0x58..0x5C
        if (isPhase3ArithOp(op)) continue;            // 0x60..0x67
        if (isPhase3BitOp(op)) continue;              // 0x6E, 0x6F bitAnd:/bitOr:
        if (isPhase3MulOp(op)) continue;              // 0x68 *
        if (isPhase3ShiftOp(op)) continue;            // 0x6C bitShift:
        if (isPhase4SendOp(op)) {
            if (noSendsBisect) return false;
            if (sendNArgs(op) > maxSendNArgs) return false;
            continue;                                  // 0x70..0xAF
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
            if (target < 0 || (size_t)target >= bcLen) return false;
            if (op > SistaV1::ShortJumpLast && (size_t)(i + 1) >= bcLen) {
                return false;
            }
            continue;
        }
        // Anything else (jumps, ext-prefixes, pushLitVar,
        // pushThisContext, dup, blockReturn, popStoreRecv/Temp,
        // mul/div/bit ops, ext bytecodes E0+, etc.) → unsupported.
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

    // Arith path: untag, do op, overflow check, retag.
    a.sar(rcx, asmjit::Imm(3));
    a.sar(rdx, asmjit::Imm(3));
    if (primIndex == 1) {
        a.add(rcx, rdx);
        a.jo(fail);
    } else if (primIndex == 2) {
        a.sub(rcx, rdx);
        a.jo(fail);
    } else if (primIndex == 9) {
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
                  int globalIdx) {
    using namespace asmjit::x86;
    (void)bcOffsetFromMethObj;
    (void)siteIdx;

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

        // Common state setup (same fields for hit and miss).
        a.mov(ptr(rdi, OFF_ICDATAPTR), rsi);
        a.mov(dword_ptr(rdi, OFF_SENDARGCOUNT), asmjit::Imm(nArgs));
        a.mov(rax, ptr(rdi, OFF_METHOD));
        // state.ip = AT the send opcode.  The chain loop reads
        // *instructionPointer_ (Interpreter.cpp:18702-18711) to
        // determine send length, then advances itself.
        a.add(rax, asmjit::Imm(bcOffsetFromMethObj));
        a.mov(ptr(rdi, OFF_IP), rax);

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
            a.bind(tryGetter);
            a.mov(rcx, r8);
            a.and_(rcx, asmjit::Imm(0xFFFF));    // slotIdx
            a.mov(rdx, ptr(rax, rcx, 3, 8));     // [rax + slotIdx*8 + 8]
            a.mov(rcx, ptr(rdi, OFF_SP));
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
            // tolerated.  The stencil JIT's inline setter relies on this
            // same property.
            a.bind(trySetter);
            a.mov(rcx, r8);
            a.and_(rcx, asmjit::Imm(0xFFFF));
            a.mov(rdx, ptr(rdi, OFF_SP));
            a.mov(r9, ptr(rdx, -8));             // arg = sp[-1]
            a.mov(ptr(rax, rcx, 3, 8), r9);      // recv->slot[slotIdx] = arg
            if (nArgs > 0) {
                a.sub(rdx, asmjit::Imm(8 * nArgs));
                a.mov(ptr(rdi, OFF_SP), rdx);
            }
            a.jmp(endOfSend);

            // === returnsSelf ===
            // Receiver is already at sp[-1-nArgs]; just pop nArgs and
            // it becomes TOS.
            a.bind(tryReturnsSelf);
            if (nArgs > 0) {
                a.mov(rcx, ptr(rdi, OFF_SP));
                a.sub(rcx, asmjit::Imm(8 * nArgs));
                a.mov(ptr(rdi, OFF_SP), rcx);
            }
            a.jmp(endOfSend);

            // === Plain cached dispatch (no inline opt) ===
            a.bind(dispatchCached);
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
            a.bind(miss);
            a.mov(dword_ptr(rdi, OFF_EXIT), asmjit::Imm(EXIT_SEND));
            a.ret();

            a.bind(endOfSend);
            return true;  // inline paths fall through to next bytecode
        }

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
        a.ldr(x6, ptr(x0, OFF_TRUEOOP));
        a.ldr(x7, ptr(x0, OFF_FALSEOOP));
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
    a.ldr(x2, ptr(x0, OFF_TEMPBASE));
    a.ldr(x2, ptr(x2));

    // SmI check.
    a.eor(x5, x1, asmjit::Imm(1));
    a.eor(x6, x2, asmjit::Imm(1));
    a.orr(x5, x5, x6);
    a.tst(x5, asmjit::Imm(7));
    a.b_ne(fail);

    if (primIndex >= 3 && primIndex <= 8) {
        a.ldr(x6, ptr(x0, OFF_TRUEOOP));
        a.ldr(x7, ptr(x0, OFF_FALSEOOP));
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

    a.asr(x1, x1, asmjit::Imm(3));
    a.asr(x2, x2, asmjit::Imm(3));
    if (primIndex == 1) {
        a.adds(x1, x1, x2);
        a.b_vs(fail);
    } else if (primIndex == 2) {
        a.subs(x1, x1, x2);
        a.b_vs(fail);
    } else if (primIndex == 9) {
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
                    int globalIdx) {
    using namespace asmjit::a64;
    (void)bcOffsetFromMethObj;
    (void)siteIdx;

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
        a.ldur(x1, a64::ptr(x2, -8));
        emitPushReg(a, x1);
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
    auto emitReturnPtr = [&](int srcOff) {
        a.ldr(x1, ptr(x0, srcOff));
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
    };
    auto emitReturnImm = [&](uint64_t imm) {
        a.mov(x1, asmjit::Imm(imm));
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
        a.ldr(x1, ptr(x2));
        a.str(x1, ptr(x0, OFF_RETVAL));
        a.mov(w3, asmjit::Imm(EXIT_RETURN));
        a.str(w3, ptr(x0, OFF_EXIT));
        a.ret(x30);
        return true;
    }
    // Phase 3 arithmetic on ARM64 (mirror of x86).
    //   x2 = sp;  x1 = a (sp[-2]);  x4 = b (sp[-1])
    //   eor x5, x1, #1;  eor x6, x4, #1;  orr x5, x5, x6
    //   tst x5, #7;  b.ne bail
    //   asr x1, x1, #3;  asr x4, x4, #3
    //   then op-specific
    //   write at sp[-2];  sp -= 8
    if (isPhase3ArithOp(op)) {
        asmjit::Label bail = a.new_label();
        asmjit::Label end  = a.new_label();

        a.ldr(x2, ptr(x0, OFF_SP));
        a.ldr(x1, ptr(x2, -16));
        a.ldr(x4, ptr(x2, -8));
        a.eor(x5, x1, asmjit::Imm(1));
        a.eor(x6, x4, asmjit::Imm(1));
        a.orr(x5, x5, x6);
        a.tst(x5, asmjit::Imm(7));
        a.b_ne(bail);

        a.asr(x1, x1, asmjit::Imm(3));
        a.asr(x4, x4, asmjit::Imm(3));

        if (op == 0x60) {        // +
            a.adds(x1, x1, x4);
            a.b_vs(bail);
            a.lsl(x1, x1, asmjit::Imm(3));
            a.orr(x1, x1, asmjit::Imm(SMI_TAG));
            a.str(x1, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            a.str(x2, ptr(x0, OFF_SP));
        } else if (op == 0x61) { // -
            a.subs(x1, x1, x4);
            a.b_vs(bail);
            a.lsl(x1, x1, asmjit::Imm(3));
            a.orr(x1, x1, asmjit::Imm(SMI_TAG));
            a.str(x1, ptr(x2, -16));
            a.sub(x2, x2, asmjit::Imm(8));
            a.str(x2, ptr(x0, OFF_SP));
        } else {
            // Comparisons: csel false/true based on signed flags.
            a.cmp(x1, x4);
            a.ldr(x5, ptr(x0, OFF_FALSEOOP));
            a.ldr(x6, ptr(x0, OFF_TRUEOOP));
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
    // Short forward jumps (0xB0..0xC7) — see x86 version for protocol.
    if (op >= SistaV1::ShortJumpBase && op <= SistaV1::ShortJumpFalseLast) {
        int targetIdx = SistaV1::shortJumpTarget(op, globalIdx);
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

        a.ldr(x4, ptr(x0, OFF_TRUEOOP));
        a.cmp(x1, x4);
        a.b_eq(jumpOnTrue ? takeBranch : fallThrough);
        a.ldr(x4, ptr(x0, OFF_FALSEOOP));
        a.cmp(x1, x4);
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

        // Common state setup.
        a.str(x5, ptr(x0, OFF_ICDATAPTR));
        a.mov(w3, asmjit::Imm(nArgs));
        a.str(w3, ptr(x0, OFF_SENDARGCOUNT));
        a.ldr(x6, ptr(x0, OFF_METHOD));
        a.add(x6, x6, asmjit::Imm(bcOffsetFromMethObj));
        a.str(x6, ptr(x0, OFF_IP));

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
            a.cbz(x7, dispatchCached);

            // Inline specializations need heap receiver (tag==0)
            a.tst(x1, asmjit::Imm(0x7));
            a.b_ne(dispatchCached);

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

            // === Inline getter: val = recv->slots[slotIdx] ===
            a.bind(tryGetter);
            a.and_(x6, x7, asmjit::Imm(0xFFFF));   // slotIdx
            // val = *(recv + 8 + slotIdx*8)
            a.add(x3, x1, x6, asmjit::a64::lsl(3));
            a.ldr(x6, ptr(x3, 8));
            a.ldr(x2, ptr(x0, OFF_SP));
            a.stur(x6, ptr(x2, rcvrOffsetBytes));
            if (nArgs > 0) {
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
            }
            a.b(endOfSend);

            // === Inline setter: recv->slots[slotIdx] = arg ===
            a.bind(trySetter);
            a.and_(x6, x7, asmjit::Imm(0xFFFF));
            a.ldr(x2, ptr(x0, OFF_SP));
            a.ldur(x3, ptr(x2, -8));            // arg = sp[-1]
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
                a.ldr(x2, ptr(x0, OFF_SP));
                a.sub(x2, x2, asmjit::Imm(8 * nArgs));
                a.str(x2, ptr(x0, OFF_SP));
            }
            a.b(endOfSend);

            // === Plain cached dispatch ===
            a.bind(dispatchCached);
            a.ldr(x6, ptr(x5, 8));            // icData[1] = method Oop
            a.str(x6, ptr(x0, OFF_CACHED_TARGET));
            a.mov(w3, asmjit::Imm(EXIT_SEND_CACHED));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);

            // === Miss ===
            a.bind(miss);
            a.mov(w3, asmjit::Imm(EXIT_SEND));
            a.str(w3, ptr(x0, OFF_EXIT));
            a.ret(x30);

            a.bind(endOfSend);
            return true;
        }

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
                             bcLabels, globalIdx)) {
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
            a.bind(bcLabels[globalIdx]);
            if (!emitOne_arm64(a, bcReal[i], nilBits,
                                bcOffsetBase + globalIdx, siteIdx,
                                bcLabels, globalIdx)) {
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
    // bytes.  128 bytes/bytecode is a comfortable upper bound.
    size_t cap = bcLen * 128 + 128;
    if (cap > 16384) cap = 16384;
    if (bcLen * 128 + 128 > cap) {
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
    if (!emitMethodBytes(bc, bcLen, nilBits, bcOffsetBase, primIdx,
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
            if (op >= 0x70 && op <= 0x7F) {
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
