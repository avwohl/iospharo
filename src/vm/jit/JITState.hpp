/*
 * JITState.hpp - Execution state passed to JIT-compiled code
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Every JIT stencil receives a pointer to this struct in the first
 * argument register (x0 on ARM64, rdi on x86_64). It contains
 * everything a bytecode handler needs to execute.
 *
 * IMPORTANT: Field order and offsets are ABI — the stencil extraction
 * script bakes offsets into machine code. Do NOT reorder fields without
 * regenerating stencils.
 *
 * The Interpreter populates this struct before entering JIT code and
 * reads it back after JIT code returns to the interpreter.
 */

#ifndef PHARO_JIT_STATE_HPP
#define PHARO_JIT_STATE_HPP

#include "JITConfig.hpp"
#include "../Oop.hpp"
#include "../../platform/Platform.hpp"
#include <cstdint>
#include <cstddef>

#if PHARO_JIT_ENABLED

// Forward declarations — stencils only use pointers to these
namespace pharo {
    class ObjectMemory;
    class ObjectHeader;
    class Interpreter;
}

namespace pharo {
namespace jit {

struct JITMethod;

// ===== JIT EXECUTION STATE =====

struct JITState {
    // --- Hot fields (accessed every bytecode) ---

    Oop* sp;                  // offset 0:  Stack pointer (points to TOS)
    Oop  receiver;            // offset 8:  Current 'self'
    Oop* literals;            // offset 16: Literal frame (slot 1 of CompiledMethod)
    Oop* tempBase;            // offset 24: Base of temps/args in the stack

    // --- Warm fields (accessed on some bytecodes) ---

    ObjectMemory* memory;     // offset 32: For field access, classOf, allocation
    Interpreter*  interp;     // offset 40: For slow paths (full sends, primitives)

    // --- Cold fields (accessed on entry/exit/deopt) ---

    uint8_t* ip;              // offset 48: Bytecode IP (for deopt, exception handling)
    JITMethod* jitMethod;     // offset 56: Currently executing JIT method
    Oop  method;              // offset 64: Current CompiledMethod Oop
    int  argCount;            // offset 72: Number of arguments to current method

    // --- Return / exit ---

    int  exitReason;          // offset 76: Why JIT code exited (see ExitReason)
    Oop  returnValue;         // offset 80: Value to return (for return bytecodes)

    // --- Inline cache support ---

    Oop  cachedTarget;        // offset 88: Cached method Oop for IC hit (ExitSendCached)
    uint64_t* icDataPtr;      // offset 96: Pointer to IC data [classIndex, methodOop]
    int  sendArgCount;        // offset 104: Number of args for the current send (IC path)
    uint32_t sendBCLength;    // offset 108: Byte length of current send bytecode (T2 chain-loop)

    // --- SimStack register caching ---
    // TOS/NOS cached in JITState fields to avoid sp manipulation in
    // straight-line code. Accessed by SimStack stencil variants only.
    uint64_t simTOS;          // offset 112: Cached TOS bits
    uint64_t simNOS;          // offset 120: Cached NOS bits

    // --- Inline primitive support ---
    // True/false Oops for comparison results in lightweight J2J path.
    // Set once in tryJITActivation, constant for the image lifetime.
    Oop trueOop;              // offset 128
    Oop falseOop;             // offset 136

    // --- J2J stencil-to-stencil call support ---
    // Stencils handle J2J sends inline via tail-calls instead of exiting
    // to the C trampoline.  The save stack lives on the C stack in
    // tryJITActivation; stencils push/pop frames here directly.
    uint8_t* j2jSaveCursor;  // offset 144: current position in save stack
    uint8_t* j2jSaveLimit;   // offset 152: base + maxDepth * sizeof(J2JSave)
    int32_t  j2jDepth;       // offset 160: current nesting depth
    int32_t  j2jTotalCalls;  // offset 164: total J2J calls (for charging)

    // --- Trampoline helper support ---
    // Pointer to MethodMap, set by tryJITActivation before the trampoline.
    // Used by the C helper pharo_jit_convert_send() to look up cached targets
    // without needing access to Interpreter private members.
    void* methodMapPtr;      // offset 168: MethodMap* for trampoline conversion

    // --- Yield support ---
    // Backward-jump stencils decrement this counter. When it reaches 0,
    // the stencil exits with ExitYield so the chain loop can run scheduler
    // checks (checkCountdown_, timer semaphore, process switch). This
    // enables J2J save stack for resumed methods without starving the
    // scheduler on long-running loops.
    int32_t yieldCountdown;  // offset 176: backward-jump yield counter

    // --- Sista splice deopt scratch ---
    // Two oop slots used by Sista splices to spill receiver / vec values
    // before calling C++ helpers (e.g. jit_rt_sista_complete_array_do_accum)
    // and reload them on the deopt fall-through path.  Avoids relying on
    // asmjit virtual-reg lifetime tracking across cc.invoke + conditional
    // branch + use, which has produced garbage rcv/vec at deopt time
    // (corrupting the closure-vec capture and resulting in nil-receiver
    // DNUs on the resumed interp).  Documented in
    // memory/project_arraydo_helper_gate_2026_05_06.md.
    uint64_t spliceSpill0;   // offset 184: spill slot 0 (rcv)
    uint64_t spliceSpill1;   // offset 192: spill slot 1 (vec / accum)

    // --- Per-entry J2J depth (option (a) per deferred A6) ---
    // Records state.j2jDepth at the moment JIT code was entered (either
    // via tryJITActivation init, or via chain-loop JIT_CALL_WITH_ENTRY_DEPTH
    // around inner activations).  The return prelude pops a save ONLY
    // when current j2jDepth > j2jEntryDepth — i.e., this method pushed
    // a save itself.  Without this, chain-loop-activated methods would
    // see state.j2jDepth from OUTER inline-J2J pushes and incorrectly
    // pop the outer's save.
    int32_t j2jEntryDepth;   // offset 200: per-entry baseline depth
    int32_t _pad_j2j_204;    // padding to 8-byte alignment

    // Constant 0x00000001_00000001 used by the inline-J2J emit to
    // bump j2jDepth (low 32) and j2jTotalCalls (high 32) in a single
    // 64-bit add.  Materializing the immediate inline takes 2 instr
    // (movz + movk); loading from this slot is 1 instr.  Set once in
    // tryJITActivation and never modified.
    uint64_t j2jDepthInc;    // offset 208: always 0x100000001

    // jit-may22a B1: Sista inline-self save stack.  Mirrors the
    // J2JSave protocol but used by Sista's kSendInlineSelf lowering
    // when the recursive self-rec call is emitted as an inline BR
    // rather than via jit_rt_sista_call_send.
    //
    // sistaSaveCursor advances on push, retreats on return-prelude pop.
    // sistaSaveLimit is one-past-end of the per-thread pool, set by
    // tryJITActivation before entering Sista.
    uint8_t* sistaSaveCursor; // offset 216: current pos in save pool
    uint8_t* sistaSaveLimit;  // offset 224: end-of-pool sentinel
    int32_t  sistaSaveDepth;  // offset 232: current nesting depth
    int32_t  sistaEntryDepth; // offset 236: per-entry baseline depth

    // --- Cached space pointers for the inline-asm write barrier ---
    // SimStack store-recv-var stencils (popStoreRecvVar_{1..4},
    // storeRecvVar_1) need to range-check rcv against old-space and val
    // against young-space.  Reading these as ObjectMemory member fields
    // would require either two indirections (s->memory->oldSpaceStart_)
    // or hardcoded layout offsets.  Stashing them here, set once at
    // tryJITActivation entry, lets the barrier be a few `ldr`s off x0
    // — no function call, so the SimStack register cache (x19-x22)
    // stays untouched.
    uint8_t* oldSpaceStart;   // offset 240
    uint8_t* oldSpaceEnd;     // offset 248
    uint8_t* newSpaceStart;   // offset 256
    uint8_t* newSpaceEnd;     // offset 264
};

// Verify expected offsets (stencils depend on these)
static_assert(offsetof(JITState, sp)        == 0,  "sp offset");
static_assert(offsetof(JITState, receiver)  == 8,  "receiver offset");
static_assert(offsetof(JITState, literals)  == 16, "literals offset");
static_assert(offsetof(JITState, tempBase)  == 24, "tempBase offset");
static_assert(offsetof(JITState, memory)    == 32, "memory offset");
static_assert(offsetof(JITState, interp)    == 40, "interp offset");
static_assert(offsetof(JITState, ip)        == 48, "ip offset");
static_assert(offsetof(JITState, jitMethod) == 56, "jitMethod offset");
static_assert(offsetof(JITState, method)    == 64, "method offset");
static_assert(offsetof(JITState, cachedTarget)  == 88, "cachedTarget offset");
static_assert(offsetof(JITState, icDataPtr)     == 96, "icDataPtr offset");
static_assert(offsetof(JITState, sendArgCount)  == 104, "sendArgCount offset");
static_assert(offsetof(JITState, sendBCLength)  == 108, "sendBCLength offset");
static_assert(offsetof(JITState, j2jSaveCursor) == 144, "j2jSaveCursor offset");
static_assert(offsetof(JITState, j2jSaveLimit)  == 152, "j2jSaveLimit offset");
static_assert(offsetof(JITState, j2jDepth)      == 160, "j2jDepth offset");
static_assert(offsetof(JITState, j2jTotalCalls) == 164, "j2jTotalCalls offset");
static_assert(offsetof(JITState, methodMapPtr)  == 168, "methodMapPtr offset");
static_assert(offsetof(JITState, yieldCountdown) == 176, "yieldCountdown offset");
static_assert(offsetof(JITState, spliceSpill0)   == 184, "spliceSpill0 offset");
static_assert(offsetof(JITState, spliceSpill1)   == 192, "spliceSpill1 offset");
static_assert(offsetof(JITState, j2jEntryDepth)  == 200, "j2jEntryDepth offset");
static_assert(offsetof(JITState, j2jDepthInc)    == 208, "j2jDepthInc offset");
// jit-may22a B1
static_assert(offsetof(JITState, sistaSaveCursor) == 216, "sistaSaveCursor offset");
static_assert(offsetof(JITState, sistaSaveLimit)  == 224, "sistaSaveLimit offset");
static_assert(offsetof(JITState, sistaSaveDepth)  == 232, "sistaSaveDepth offset");
static_assert(offsetof(JITState, sistaEntryDepth) == 236, "sistaEntryDepth offset");
static_assert(offsetof(JITState, oldSpaceStart)   == 240, "oldSpaceStart offset");
static_assert(offsetof(JITState, oldSpaceEnd)     == 248, "oldSpaceEnd offset");
static_assert(offsetof(JITState, newSpaceStart)   == 256, "newSpaceStart offset");
static_assert(offsetof(JITState, newSpaceEnd)     == 264, "newSpaceEnd offset");

// SistaSave: 56 bytes per entry (8-byte aligned).  Mirrors J2JSave's
// layout but sized for Sista's needs.  See docs/jit-may22a.md for
// the full design.
struct SistaSave {
    Oop*     sp;          // offset 0: caller's state.sp at push time
    Oop      receiver;    // offset 8: caller's state.receiver
    Oop*     tempBase;    // offset 16: caller's state.tempBase
    uint8_t* ip;          // offset 24: caller's state.ip
    uint32_t bcOffset;    // offset 32: source bcOffset (for deopt replay)
    uint32_t _pad;        // offset 36: alignment padding
    uint8_t* resumeAddr;  // offset 40: arm64 BR-target on return
    uint64_t _pad2;       // offset 48: round size to 56 bytes
};
static_assert(sizeof(SistaSave) == 56, "SistaSave must be 56 bytes");
static_assert(offsetof(SistaSave, sp) == 0, "SistaSave.sp");
static_assert(offsetof(SistaSave, receiver) == 8, "SistaSave.receiver");
static_assert(offsetof(SistaSave, tempBase) == 16, "SistaSave.tempBase");
static_assert(offsetof(SistaSave, ip) == 24, "SistaSave.ip");
static_assert(offsetof(SistaSave, bcOffset) == 32, "SistaSave.bcOffset");
static_assert(offsetof(SistaSave, resumeAddr) == 40, "SistaSave.resumeAddr");

constexpr size_t MaxSistaSavePoolSize = 256;

// ===== EXIT REASONS =====
//
// When JIT code can't handle something, it sets exitReason and returns
// to the interpreter. The interpreter inspects the reason and handles it.

enum ExitReason : int {
    ExitNone        = 0,  // Normal completion (should not happen mid-method)
    ExitReturn      = 1,  // Return bytecode — returnValue is set
    ExitSend        = 2,  // Message send — ip points to send bytecode, sp is correct
    ExitPrimFail    = 3,  // Primitive failed — fall back to Smalltalk code
    ExitDeopt       = 4,  // Deoptimization needed (e.g., uncommon trap)
    ExitStackOverflow = 5, // Stack limit reached
    ExitArithOverflow = 6, // Arithmetic overflow — restore entry SP, re-execute
    ExitSendCached  = 7,  // IC hit — cachedTarget has resolved method, skip lookup
    ExitBlockCreate = 8,  // PushFullBlock — cachedTarget has packed (litIndex | flags<<32)
    ExitArrayCreate = 9,  // PushArray — cachedTarget has desc byte (arraySize | popIntoArray<<7)
    ExitJ2JCall     = 10, // J2J send: cachedTarget=method, returnValue=entry addr,
                          //   sendArgCount=nArgs, ip=past send bytecode.
                          //   Trampoline pushes frame, sets up callee, re-enters JIT.
    ExitYield       = 11, // Backward-jump yield — ip points to branch target bytecode.
                          //   Chain loop resets yieldCountdown and resumes JIT.
    ExitMustBool    = 12, // Conditional jump received a non-Boolean.  returnValue
                          //   holds the offending value; ip points to the
                          //   conditional-jump bytecode.  Trampoline pushes the
                          //   value back onto the interp stack and calls
                          //   sendMustBeBoolean per the Smalltalk spec.
};

// ===== TRAMPOLINE HELPER =====
// Called from the ASM trampoline (via BL) when exitReason == ExitSendCached.
// Looks up cachedTarget in the MethodMap; if the target is compiled and safe,
// converts to ExitJ2JCall (sets returnValue + exitReason) and returns 1.
// Otherwise returns 0 (trampoline should exit to chain loop).
// Defined in Interpreter.cpp.
extern "C" int pharo_jit_convert_send(JITState* state);

// ===== STENCIL FUNCTION SIGNATURE =====

// Every stencil and every continuation has this signature.
// The JITState pointer is the only argument; all state flows through it.
typedef void (*StencilFunc)(JITState*);

// ===== JIT CALL MACRO =====
//
// SimStack stencils clobber x19-x22 via inline asm (without clobber lists).
// When calling JIT code from C++, we must tell the compiler that x19-x22
// may be modified, so it saves/restores them around the call.
//
// JIT_CALL no longer flips W^X defensively.  The codebase invariant
// (W^X audit 2026-04-26) is that the thread is in EXECUTABLE mode
// whenever it is not inside a narrow write window.  Every write window
// (IC patch via patchJITICAfterSend RAII, IC upgrade via
// upgradeICToJ2J RAII, JIT compile via CodeZone::allocate+finalize,
// flushCaches, GC recovery) restores X via RAII or explicit
// makeExecutable.  Frequently-mutated per-method counters
// (executionCount, lastUsedEpoch, j2jDepthLimit, j2jCleanRuns) live
// in a heap-allocated JITMethodStats side-table — writes never
// touch MAP_JIT, no W flip needed.  Saves one APRR MSR write per
// JIT entry on Apple Silicon.
#define _JIT_CALL_PRE() do { } while (0)

#ifdef __aarch64__
#define JIT_CALL(entry_ptr, state_ptr) do { \
    _JIT_CALL_PRE(); \
    void* _jit_e = reinterpret_cast<void*>(entry_ptr); \
    void* _jit_s = reinterpret_cast<void*>(state_ptr); \
    asm volatile( \
        "mov x0, %[s]\n\t" \
        /* x20 = j2jDepthInc constant (0x100000001) — pre-loaded so */ \
        /* asmjit-T1 inline-J2J emit can use it directly instead of */ \
        /* an extra ldr per push.  Offset 208 = OFF_J2J_DEPTH_INC. */ \
        "ldr x20, [x0, #208]\n\t" \
        /* x19 = state.jitMethod (offset 56) — pre-loaded for IC HIT */ \
        /* path.  Saves 1 ldr per send for xmethod-off (default). */ \
        "ldr x19, [x0, #56]\n\t" \
        "blr %[e]" \
        : \
        : [s] "r"(_jit_s), [e] "r"(_jit_e) \
        : "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11", \
          "x12","x13","x14","x15","x16","x17","x19","x20","x21","x22","x30", \
          "memory","cc" \
    ); \
} while(0)
#else
// NOTE: must fully qualify ::pharo::jit::JITState — this macro is invoked
// from member functions of pharo::Interpreter (Interpreter.cpp), where
// `JITState` is not visible without the qualifier.  The arm64 branch
// above sidesteps the issue by not naming the type in its asm wrapper.
#define JIT_CALL(entry_ptr, state_ptr) do { \
    _JIT_CALL_PRE(); \
    ((void(*)(::pharo::jit::JITState*))(entry_ptr))(state_ptr); \
} while(0)
#endif

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_JIT_STATE_HPP
