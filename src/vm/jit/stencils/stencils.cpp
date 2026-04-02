/*
 * stencils.cpp - Copy-and-patch stencil source functions
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Each function here is a bytecode handler compiled by Clang to produce
 * a machine code "stencil". The stencils reference "hole" symbols (extern
 * declarations) that are left unresolved — the extraction script records
 * their positions, and the JIT compiler patches in runtime values.
 *
 * BUILD: clang++ -c -O2 -std=c++17 -fno-exceptions -fno-rtti
 *        -fno-asynchronous-unwind-tables -fno-stack-protector
 *        -o stencils.o stencils.cpp
 *
 * DO NOT link this file into the VM. It is only used at build time
 * by scripts/extract_stencils.py to generate generated_stencils.hpp.
 *
 * CALLING CONVENTION:
 *   - Every stencil takes JITState* in the first arg register
 *   - Every stencil ends with a tail call to _HOLE_CONTINUE or similar
 *   - Stencils must NOT touch the C stack (no alloca, VLAs, etc.)
 *   - Stencils must NOT call C++ code that throws exceptions
 */

#include <cstdint>
#include <cstddef>

// ===== MINIMAL TYPE DEFINITIONS =====
//
// We redefine just enough here so the stencils compile standalone without
// pulling in the full VM headers (which would bloat the object file with
// static data, vtables, etc.). These MUST match the real definitions.

// Oop is a 64-bit tagged value (matches pharo::Oop)
struct Oop {
    uint64_t bits;
};

// ObjectHeader lives at the start of every heap object
struct ObjectHeader {
    uint64_t header;

    // Slot count from header (bits 56-63, 0-254, 255=overflow)
    uint64_t slotCount() const {
        uint64_t count = (header >> 56) & 0xFF;
        if (count == 255) {
            const uint64_t* overflow = reinterpret_cast<const uint64_t*>(this) - 1;
            return ((*overflow) << 8) >> 8;
        }
        return count;
    }

    // Pointer to first slot (Oop array starts after the header word)
    Oop* slots() { return reinterpret_cast<Oop*>(this + 1); }
    const Oop* slots() const { return reinterpret_cast<const Oop*>(this + 1); }

    // Class index (bits 0-21)
    uint32_t classIndex() const { return static_cast<uint32_t>(header & 0x3FFFFF); }
};

// JITState matches pharo::jit::JITState exactly
struct JITState {
    Oop*          sp;           // offset 0
    Oop           receiver;     // offset 8
    Oop*          literals;     // offset 16
    Oop*          tempBase;     // offset 24
    void*         memory;       // offset 32 (ObjectMemory*)
    void*         interp;       // offset 40 (Interpreter*)
    uint8_t*      ip;           // offset 48
    void*         jitMethod;    // offset 56
    Oop           method;       // offset 64
    int           argCount;     // offset 72
    int           exitReason;   // offset 76
    Oop           returnValue;  // offset 80
};

// Tag bit constants (must match Oop.hpp)
static constexpr uint64_t SmallIntegerTag = 0x1;     // bit 0 = 1, bits 2:1 = 00
static constexpr uint64_t TagMask3 = 0x7;
static constexpr uint64_t ImmediateBit = 0x1;

static inline bool isSmallInteger(Oop o) { return (o.bits & TagMask3) == SmallIntegerTag; }
static inline int64_t asSmallInteger(Oop o) { return static_cast<int64_t>(o.bits) >> 3; }
static inline Oop fromSmallInteger(int64_t v) { return Oop{(static_cast<uint64_t>(v) << 3) | SmallIntegerTag}; }
static inline ObjectHeader* asObjectPtr(Oop o) { return reinterpret_cast<ObjectHeader*>(o.bits); }

// Special object Oops (must match ObjectMemory's nil/true/false)
// These are patched as HOLE values, not hardcoded
extern "C" Oop _HOLE_NIL_OOP;
extern "C" Oop _HOLE_TRUE_OOP;
extern "C" Oop _HOLE_FALSE_OOP;

// ===== HOLE DECLARATIONS =====
//
// These symbols are NEVER defined. The linker would fail on them.
// The extraction script sees them as relocations and records their offsets.

extern "C" {
    // Continuation (next stencil address)
    void _HOLE_CONTINUE(JITState*);

    // Branch target
    void _HOLE_BRANCH_TARGET(JITState*);

    // Operand values (cast from pointer to integer)
    extern char _HOLE_OPERAND;
    extern char _HOLE_OPERAND2;

    // Runtime helpers (implemented in JITRuntime.cpp)
    void _HOLE_RT_SEND(JITState*);
    void _HOLE_RT_RETURN(JITState*);
    void _HOLE_RT_ARITH_OVERFLOW(JITState*);
}

// Helper to get operand value (address of hole symbol = the operand integer)
#define OPERAND  ((int)(uintptr_t)&_HOLE_OPERAND)
#define OPERAND2 ((int)(uintptr_t)&_HOLE_OPERAND2)

// ===== EXIT REASONS (must match ExitReason enum) =====
static constexpr int EXIT_RETURN = 1;
static constexpr int EXIT_SEND = 2;

// =====================================================================
// STENCILS
// =====================================================================

// ----- PUSH STENCILS -----

// Push receiver instance variable [0..255]
// Bytecodes: 0x00-0x0F (short, index in low 4 bits)
//            0xE8+ext (long, extended operand)
extern "C" void stencil_pushRecvVar(JITState* s) {
    int idx = OPERAND;
    ObjectHeader* obj = asObjectPtr(s->receiver);
    Oop value;
    if (static_cast<uint64_t>(idx) < obj->slotCount()) {
        value = obj->slots()[idx];
    } else {
        value = *(Oop*)&_HOLE_NIL_OOP;
    }
    *(s->sp) = value;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push literal constant [0..255]
// Bytecodes: 0x20-0x3F (short, index in low 5 bits)
extern "C" void stencil_pushLitConst(JITState* s) {
    int idx = OPERAND;
    Oop value = s->literals[idx];
    *(s->sp) = value;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push literal variable (value of Association at literal index)
// Bytecodes: 0x10-0x1F
extern "C" void stencil_pushLitVar(JITState* s) {
    int idx = OPERAND;
    Oop assoc = s->literals[idx];
    // Association value is slot 1 (slot 0 = key)
    ObjectHeader* obj = asObjectPtr(assoc);
    Oop value = obj->slots()[1];
    *(s->sp) = value;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push temporary variable [0..255]
// Bytecodes: 0x40-0x4B (short, index in low 4 bits)
extern "C" void stencil_pushTemp(JITState* s) {
    int idx = OPERAND;
    Oop value = s->tempBase[idx];
    *(s->sp) = value;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push receiver (self)
// Bytecode: 0x4C
extern "C" void stencil_pushReceiver(JITState* s) {
    *(s->sp) = s->receiver;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push true
// Bytecode: 0x4D
extern "C" void stencil_pushTrue(JITState* s) {
    *(s->sp) = *(Oop*)&_HOLE_TRUE_OOP;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push false
// Bytecode: 0x4E
extern "C" void stencil_pushFalse(JITState* s) {
    *(s->sp) = *(Oop*)&_HOLE_FALSE_OOP;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push nil
// Bytecode: 0x4F
extern "C" void stencil_pushNil(JITState* s) {
    *(s->sp) = *(Oop*)&_HOLE_NIL_OOP;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push SmallInteger 0
// Bytecode: 0x50
extern "C" void stencil_pushZero(JITState* s) {
    *(s->sp) = fromSmallInteger(0);
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push SmallInteger 1
// Bytecode: 0x51
extern "C" void stencil_pushOne(JITState* s) {
    *(s->sp) = fromSmallInteger(1);
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Duplicate top of stack
// Bytecode: 0x53
extern "C" void stencil_dup(JITState* s) {
    Oop top = s->sp[-1];
    *(s->sp) = top;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Push SmallInteger (arbitrary value, pre-tagged by the JIT compiler)
// Bytecode: 0xE8 (extended push integer)
// OPERAND is the pre-computed tagged SmallInteger bits ((value << 3) | 1)
extern "C" void stencil_pushInteger(JITState* s) {
    Oop value;
    value.bits = static_cast<uint64_t>(OPERAND);
    *(s->sp) = value;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// ----- POP / STORE STENCILS -----

// Pop top of stack
// Bytecode: 0xD8
extern "C" void stencil_pop(JITState* s) {
    s->sp--;
    _HOLE_CONTINUE(s);
}

// Pop and store into receiver instance variable
// Bytecodes: 0xC8-0xCF
extern "C" void stencil_popStoreRecvVar(JITState* s) {
    int idx = OPERAND;
    s->sp--;
    Oop value = *(s->sp);
    ObjectHeader* obj = asObjectPtr(s->receiver);
    obj->slots()[idx] = value;
    _HOLE_CONTINUE(s);
}

// Pop and store into temporary variable
// Bytecodes: 0xD0-0xD7
extern "C" void stencil_popStoreTemp(JITState* s) {
    int idx = OPERAND;
    s->sp--;
    Oop value = *(s->sp);
    s->tempBase[idx] = value;
    _HOLE_CONTINUE(s);
}

// Store into receiver instance variable (no pop — TOS stays on stack)
// Bytecode: 0xF3
extern "C" void stencil_storeRecvVar(JITState* s) {
    int idx = OPERAND;
    Oop value = s->sp[-1];
    ObjectHeader* obj = asObjectPtr(s->receiver);
    obj->slots()[idx] = value;
    _HOLE_CONTINUE(s);
}

// Store into temporary variable (no pop — TOS stays on stack)
// Bytecode: 0xF5
extern "C" void stencil_storeTemp(JITState* s) {
    int idx = OPERAND;
    Oop value = s->sp[-1];
    s->tempBase[idx] = value;
    _HOLE_CONTINUE(s);
}

// Pop and store into literal variable (value of Association)
// Bytecode: 0xF1
extern "C" void stencil_popStoreLitVar(JITState* s) {
    int idx = OPERAND;
    s->sp--;
    Oop value = *(s->sp);
    Oop assoc = s->literals[idx];
    ObjectHeader* obj = asObjectPtr(assoc);
    // Association value is slot 1
    obj->slots()[1] = value;
    _HOLE_CONTINUE(s);
}

// ----- RETURN STENCILS -----

// Return top of stack
// Bytecode: 0x5C
extern "C" void stencil_returnTop(JITState* s) {
    s->sp--;
    s->returnValue = *(s->sp);
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// Return receiver (self)
// Bytecode: 0x58
extern "C" void stencil_returnReceiver(JITState* s) {
    s->returnValue = s->receiver;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// Return true
// Bytecode: 0x59
extern "C" void stencil_returnTrue(JITState* s) {
    s->returnValue = *(Oop*)&_HOLE_TRUE_OOP;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// Return false
// Bytecode: 0x5A
extern "C" void stencil_returnFalse(JITState* s) {
    s->returnValue = *(Oop*)&_HOLE_FALSE_OOP;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// Return nil
// Bytecode: 0x5B
extern "C" void stencil_returnNil(JITState* s) {
    s->returnValue = *(Oop*)&_HOLE_NIL_OOP;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// ----- JUMP STENCILS -----

// Unconditional jump
// Bytecodes: 0xB0-0xB7 (short), extended
extern "C" void stencil_jump(JITState* s) {
    _HOLE_BRANCH_TARGET(s);
}

// Jump if false (pop condition)
// Bytecodes: 0xC0-0xC7 (short), extended
extern "C" void stencil_jumpFalse(JITState* s) {
    s->sp--;
    Oop cond = *(s->sp);
    // In Smalltalk, false is the only false value
    // Compare against the false object
    Oop falseObj = *(Oop*)&_HOLE_FALSE_OOP;
    if (cond.bits == falseObj.bits) {
        _HOLE_BRANCH_TARGET(s);
    } else {
        _HOLE_CONTINUE(s);
    }
}

// Jump if true (pop condition)
// Bytecodes: 0xB8-0xBF (short), extended
extern "C" void stencil_jumpTrue(JITState* s) {
    s->sp--;
    Oop cond = *(s->sp);
    Oop trueObj = *(Oop*)&_HOLE_TRUE_OOP;
    if (cond.bits == trueObj.bits) {
        _HOLE_BRANCH_TARGET(s);
    } else {
        _HOLE_CONTINUE(s);
    }
}

// ----- ARITHMETIC STENCILS -----

// SmallInteger add (fast path + overflow exit)
// Bytecode: 0x60 (arithmetic selector +)
// OPERAND = bytecode offset for precise deopt
extern "C" void stencil_addSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        int64_t ai = asSmallInteger(a);
        int64_t bi = asSmallInteger(b);
        int64_t result = ai + bi;

        // Check overflow: SmallInteger range is -(2^60) to (2^60 - 1)
        if (result >= -(1LL << 60) && result < (1LL << 60)) {
            s->sp -= 2;
            *(s->sp) = fromSmallInteger(result);
            s->sp++;
            _HOLE_CONTINUE(s);
            return;  // unreachable but helps compiler
        }
    }
    // Overflow or non-SmallInteger: deopt to interpreter at this bytecode
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger subtract (fast path + overflow exit)
// Bytecode: 0x61 (arithmetic selector -)
extern "C" void stencil_subSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        int64_t ai = asSmallInteger(a);
        int64_t bi = asSmallInteger(b);
        int64_t result = ai - bi;

        if (result >= -(1LL << 60) && result < (1LL << 60)) {
            s->sp -= 2;
            *(s->sp) = fromSmallInteger(result);
            s->sp++;
            _HOLE_CONTINUE(s);
            return;
        }
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger less-than comparison
// Bytecode: 0x62 (arithmetic selector <)
extern "C" void stencil_lessThanSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        bool result = asSmallInteger(a) < asSmallInteger(b);
        s->sp -= 2;
        *(s->sp) = result ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->sp++;
        _HOLE_CONTINUE(s);
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger greater-than comparison
// Bytecode: 0x63 (arithmetic selector >)
extern "C" void stencil_greaterThanSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        bool result = asSmallInteger(a) > asSmallInteger(b);
        s->sp -= 2;
        *(s->sp) = result ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->sp++;
        _HOLE_CONTINUE(s);
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger equality
// Bytecode: 0x66 (arithmetic selector =)
extern "C" void stencil_equalSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        bool result = a.bits == b.bits;
        s->sp -= 2;
        *(s->sp) = result ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->sp++;
        _HOLE_CONTINUE(s);
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger not-equal
// Bytecode: 0x67 (arithmetic selector ~=)
extern "C" void stencil_notEqualSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        bool result = a.bits != b.bits;
        s->sp -= 2;
        *(s->sp) = result ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->sp++;
        _HOLE_CONTINUE(s);
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger multiply (fast path + overflow exit)
// Bytecode: 0x68 (arithmetic selector *)
extern "C" void stencil_mulSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        int64_t ai = asSmallInteger(a);
        int64_t bi = asSmallInteger(b);
        // Use __int128 to detect overflow
        __int128 wide = static_cast<__int128>(ai) * bi;
        int64_t result = static_cast<int64_t>(wide);
        if (wide == result && result >= -(1LL << 60) && result < (1LL << 60)) {
            s->sp -= 2;
            *(s->sp) = fromSmallInteger(result);
            s->sp++;
            _HOLE_CONTINUE(s);
            return;
        }
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// ----- SEND STENCIL -----

// Generic send: exit to interpreter for full lookup
// Bytecodes: 0x80-0xBF (send with 0-2 args), extended sends
// OPERAND = bytecode offset of this send (for deopt IP)
extern "C" void stencil_send(JITState* s) {
    s->ip = s->ip + OPERAND;  // Set deopt IP to this send's bytecode
    s->exitReason = EXIT_SEND;
    _HOLE_RT_SEND(s);
}

// ----- NOP STENCIL -----

// Used for bytecodes we skip or as padding
extern "C" void stencil_nop(JITState* s) {
    _HOLE_CONTINUE(s);
}
