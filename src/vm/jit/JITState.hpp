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
};

// ===== STENCIL FUNCTION SIGNATURE =====

// Every stencil and every continuation has this signature.
// The JITState pointer is the only argument; all state flows through it.
typedef void (*StencilFunc)(JITState*);

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_JIT_STATE_HPP
