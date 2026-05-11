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

bool allBytecodesSupported(const uint8_t* bc, size_t bcLen) {
    static const bool noSendsBisect =
        std::getenv("PHARO_ASMJIT_T1_NO_SENDS_BISECT") != nullptr;
    static const int maxSendNArgs = []() {
        const char* v = std::getenv("PHARO_ASMJIT_T1_MAX_SEND_NARGS");
        return v ? atoi(v) : 99;
    }();
    for (size_t i = 0; i < bcLen; i++) {
        uint8_t op = bc[i];
        if (op <= 0x0F) continue;                     // pushRecvVar 0..15
        if (op >= 0x20 && op <= 0x3F) continue;       // pushLitConst 0..31
        if (op >= 0x40 && op <= 0x4B) continue;       // pushTemp 0..11
        if (op == SistaV1::PushReceiver) continue;    // 0x4C
        if (op >= 0x4D && op <= 0x51) continue;       // pushTrue/False/Nil/Zero/One
        if (op == SistaV1::Dup) continue;             // 0x53
        if (op >= SistaV1::ReturnReceiver
                && op <= SistaV1::ReturnTop) continue; // 0x58..0x5C
        if (isPhase3ArithOp(op)) continue;            // 0x60..0x67
        if (isPhase4SendOp(op)) {
            if (noSendsBisect) return false;
            if (sendNArgs(op) > maxSendNArgs) return false;
            continue;                                  // 0x70..0xAF
        }
        if (op >= SistaV1::PopStoreTempBase
                && op <= SistaV1::PopStoreTempLast) continue;  // 0xD0..0xD7
        if (op == SistaV1::Pop) continue;             // 0xD8
        // Anything else (jumps, ext-prefixes, pushLitVar,
        // pushThisContext, dup, blockReturn, popStoreRecv/Temp,
        // mul/div/bit ops, ext bytecodes E0+, etc.) → unsupported.
        return false;
    }
    return true;
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
                  int siteIdx) {
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
        a.mov(r8,  rcx);
        a.xor_(r8, asmjit::Imm(1));
        a.mov(r9,  rdx);
        a.xor_(r9, asmjit::Imm(1));
        a.or_(r8, r9);
        a.test(r8.r8(), asmjit::Imm(7));
        a.jne(bail);

        a.sar(rcx, asmjit::Imm(3));  // untag (signed)
        a.sar(rdx, asmjit::Imm(3));

        if (op == 0x60) {            // +
            a.add(rcx, rdx);
            a.jo(bail);
            a.shl(rcx, asmjit::Imm(3));
            a.or_(rcx, asmjit::Imm(SMI_TAG));
            a.mov(ptr(rax, -16), rcx);
            a.sub(rax, 8);
            a.mov(ptr(rdi, OFF_SP), rax);
        } else if (op == 0x61) {     // -
            a.sub(rcx, rdx);
            a.jo(bail);
            a.shl(rcx, asmjit::Imm(3));
            a.or_(rcx, asmjit::Imm(SMI_TAG));
            a.mov(ptr(rax, -16), rcx);
            a.sub(rax, 8);
            a.mov(ptr(rdi, OFF_SP), rax);
        } else {
            // Comparison ops.  cmp signed; cmov true/false.
            a.cmp(rcx, rdx);
            a.mov(rsi, ptr(rdi, OFF_FALSEOOP));   // default: false
            a.mov(r8,  ptr(rdi, OFF_TRUEOOP));
            switch (op) {
                case 0x62: a.cmovl(rsi, r8); break;   // <
                case 0x63: a.cmovg(rsi, r8); break;   // >
                case 0x64: a.cmovle(rsi, r8); break;  // <=
                case 0x65: a.cmovge(rsi, r8); break;  // >=
                case 0x66: a.cmove(rsi, r8); break;   // =
                case 0x67: a.cmovne(rsi, r8); break;  // ~=
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
    // Phase 4b.2 (partial): set up IC context and ExitSend.  The
    // chain loop's ExitSend handler does method lookup, patches the
    // IC for next time via patchJITICAfterSend, and inline-activates
    // the callee — significantly faster than the prior bail-with-
    // ExitArithOverflow which went through full interp send dispatch.
    //
    // Inline IC HIT probe + ExitSendCached emit is NOT enabled in
    // this step.  Empirical bisect: even with valid IC context, the
    // chain loop's resume path after callee return interacts with
    // our methods in ways that cause mustBeBoolean cascades.
    // Resolving requires deeper J2JSave/SavedFrame protocol work.
    // For now we get IC management without the inline probe.
    if (isPhase4SendOp(op)) {
        int nArgs = sendNArgs(op);

        // Per-site icData address: jm->icBuffer + siteIdx*IC_BYTES_PER_SITE.
        a.mov(rdx, ptr(rdi, OFF_JITMETHOD));
        a.mov(rsi, ptr(rdx, (int)offsetof(JITMethod, icBuffer)));
        a.add(rsi, asmjit::Imm(siteIdx * (int)IC_BYTES_PER_SITE));

        a.mov(ptr(rdi, OFF_ICDATAPTR), rsi);
        a.mov(dword_ptr(rdi, OFF_SENDARGCOUNT), asmjit::Imm(nArgs));
        a.mov(rax, ptr(rdi, OFF_METHOD));
        // state.ip = AT the send opcode.  The chain loop reads
        // *instructionPointer_ (Interpreter.cpp:18702-18711) to
        // determine send length, then advances itself.
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

bool emitOne_arm64(asmjit::a64::Assembler& a, uint8_t op,
                    uint64_t nilBits, int bcOffsetFromMethObj,
                    int siteIdx) {
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
    // Phase 4b.2 (partial) single-byte sends on ARM64: set up IC
    // context and ExitSend.  See x86 version for rationale.
    if (isPhase4SendOp(op)) {
        int nArgs = sendNArgs(op);

        // Per-site IC address.
        a.ldr(x5, ptr(x0, OFF_JITMETHOD));
        a.ldr(x5, ptr(x5, (int)offsetof(JITMethod, icBuffer)));
        a.add(x5, x5, asmjit::Imm(siteIdx * (int)IC_BYTES_PER_SITE));

        a.str(x5, ptr(x0, OFF_ICDATAPTR));
        a.mov(w3, asmjit::Imm(nArgs));
        a.str(w3, ptr(x0, OFF_SENDARGCOUNT));
        a.ldr(x6, ptr(x0, OFF_METHOD));
        // state.ip = AT the send opcode (see x86 version).
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
bool emitMethodBytes(const uint8_t* bc, size_t bcLen, uint64_t nilBits,
                     int bcOffsetBase,
                     uint8_t* out, size_t outCap,
                     size_t* outSize, bool* isReal,
                     uint32_t* bcToCodeOut) {
    using namespace asmjit;

    Environment env = Environment::host();
    CodeHolder code;
    Error err = code.init(env);
    if (err != kErrorOk) return false;

    bool real = (bcLen > 0) && allBytecodesSupported(bc, bcLen);
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
    if (bcToCodeOut && real) {
        bcLabels.reserve(bcLen);
        for (size_t i = 0; i < bcLen; i++) bcLabels.push_back(a.new_label());
    }
    if (real) {
        int siteIdx = 0;
        for (size_t i = 0; i < bcLen; i++) {
            if (bcToCodeOut) a.bind(bcLabels[i]);
            if (!emitOne_x86(a, bc[i], nilBits,
                             bcOffsetBase + (int)i, siteIdx)) {
                // pre-scan said yes but emit said no.  Abort —
                // safer than emitting partial garbage; caller sees
                // failure and the method goes uncompiled.
                std::fprintf(stderr,
                    "[asmjit-t1] BUG: prescan/emit disagree at bc[%zu]=0x%02x\n",
                    i, bc[i]);
                return false;
            }
            if (isPhase4SendOp(bc[i])) siteIdx++;
        }
        // Defensive epilogue if the method's last bytecode wasn't a
        // return (well-formed methods always end in return).
        if (bcLen == 0
                || bc[bcLen-1] < SistaV1::ReturnReceiver
                || bc[bcLen-1] > SistaV1::ReturnTop) {
            a.mov(x86::dword_ptr(x86::rdi, OFF_EXIT), Imm(EXIT_SEND));
            a.ret();
        }
    } else {
        a.mov(x86::dword_ptr(x86::rdi, OFF_EXIT), Imm(EXIT_SEND));
        a.ret();
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    a64::Assembler a(&code);
    if (bcToCodeOut && real) {
        bcLabels.reserve(bcLen);
        for (size_t i = 0; i < bcLen; i++) bcLabels.push_back(a.new_label());
    }
    if (real) {
        int siteIdx = 0;
        for (size_t i = 0; i < bcLen; i++) {
            if (bcToCodeOut) a.bind(bcLabels[i]);
            if (!emitOne_arm64(a, bc[i], nilBits,
                                bcOffsetBase + (int)i, siteIdx)) {
                std::fprintf(stderr,
                    "[asmjit-t1] BUG: prescan/emit disagree at bc[%zu]=0x%02x\n",
                    i, bc[i]);
                return false;
            }
            if (isPhase4SendOp(bc[i])) siteIdx++;
        }
        if (bcLen == 0
                || bc[bcLen-1] < SistaV1::ReturnReceiver
                || bc[bcLen-1] > SistaV1::ReturnTop) {
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

    if (hasPrimitive) {
        // Phase 2 doesn't emit primitive prologues; bail at compile.
        g_failed++;
        return nullptr;
    }

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
    if (!emitMethodBytes(bc, bcLen, nilBits, bcOffsetBase,
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
    // Bisect knobs (default = off):
    //   PHARO_ASMJIT_T1_NO_BCTOCODE=1   — never advertise bcToCode
    //   PHARO_ASMJIT_T1_NO_NUMBC=1      — never advertise numBytecodes
    //   PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS=1
    //                                   — advertise resume even for
    //                                     send-containing methods
    //                                     (KNOWN BROKEN; for debug)
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
    jm->hasPrimPrologue   = false;
    jm->isBlock           = false;
    jm->pinned            = false;
    jm->hasSends          = false;
    jm->hasHeapWrites     = false;
    jm->hasRecvFieldAccess= false;
    jm->hasRecvFieldWrite = false;
    jm->hasLitVarWrite    = false;
    jm->maxRecvFieldIndex = 0;
    jm->isSpliceTarget    = false;

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

    static const bool trace = std::getenv("PHARO_USE_ASMJIT_T1_TRACE") != nullptr;
    bool emitTrace = trace && (g_compiled <= 10 || (g_compiled % 100 == 0)
                                || (isReal && g_compiledReal <= 30));
    if (emitTrace) {
        std::fprintf(stderr,
                     "[asmjit-t1] #%zu (%s) compiled %llu -> jm=%p code=%p (%zu bytes, %zu bc)\n",
                     g_compiled, isReal ? "real" : "stub",
                     static_cast<unsigned long long>(compiledMethod.rawBits()),
                     (void*)jm, (void*)jm->codeStart(), emitted, bcLen);
    }

    return jm;
}

size_t asmjitT1Compiled() { return g_compiled; }
size_t asmjitT1Failed()   { return g_failed;   }

}  // namespace jit
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
