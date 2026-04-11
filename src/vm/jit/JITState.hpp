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
static_assert(offsetof(JITState, j2jSaveCursor) == 144, "j2jSaveCursor offset");
static_assert(offsetof(JITState, j2jSaveLimit)  == 152, "j2jSaveLimit offset");
static_assert(offsetof(JITState, j2jDepth)      == 160, "j2jDepth offset");
static_assert(offsetof(JITState, j2jTotalCalls) == 164, "j2jTotalCalls offset");

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
};

// ===== STENCIL FUNCTION SIGNATURE =====

// Every stencil and every continuation has this signature.
// The JITState pointer is the only argument; all state flows through it.
typedef void (*StencilFunc)(JITState*);

// ===== JIT CALL MACRO =====
//
// SimStack stencils clobber x19/x20 via inline asm (without clobber lists).
// When calling JIT code from C++, we must tell the compiler that x19/x20
// may be modified, so it saves/restores them around the call.
#ifdef __aarch64__
#define JIT_CALL(entry_ptr, state_ptr) do { \
    void* _jit_e = reinterpret_cast<void*>(entry_ptr); \
    void* _jit_s = reinterpret_cast<void*>(state_ptr); \
    asm volatile( \
        "mov x0, %[s]\n\t" \
        "blr %[e]" \
        : \
        : [s] "r"(_jit_s), [e] "r"(_jit_e) \
        : "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11", \
          "x12","x13","x14","x15","x16","x17","x19","x20","x30", \
          "memory","cc" \
    ); \
} while(0)
#else
#define JIT_CALL(entry_ptr, state_ptr) do { \
    ((void(*)(JITState*))(entry_ptr))(state_ptr); \
} while(0)
#endif

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_JIT_STATE_HPP
