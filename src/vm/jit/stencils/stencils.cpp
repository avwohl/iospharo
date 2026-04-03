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

// MegaCacheEntry matches pharo::jit::MegaCacheEntry exactly
struct MegaCacheEntry {
    uint64_t selectorBits;
    uint64_t classIndex;     // For objects: class index (22-bit); for immediates: tag|0x80000000
    uint64_t methodBits;     // Oop bits of the resolved CompiledMethod
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
    // IC support
    Oop           cachedTarget; // offset 88
    uint64_t*     icDataPtr;    // offset 96
    int           sendArgCount; // offset 104
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

    // Runtime helpers — declared as function pointer variables so the
    // compiler generates GOT-style adrp+ldr (±4GB range) instead of
    // direct BL (BRANCH26, ±128MB range which is too small when the
    // code zone is far from the helper functions in memory).
    extern void (*_HOLE_RT_SEND)(JITState*);
    extern void (*_HOLE_RT_RETURN)(JITState*);
    extern void (*_HOLE_RT_ARITH_OVERFLOW)(JITState*);

    // Megamorphic method cache (address resolved via literal pool)
    extern char _HOLE_MEGA_CACHE;
}

// Helper to get operand value (address of hole symbol = the operand integer)
#define OPERAND  ((int)(uintptr_t)&_HOLE_OPERAND)
#define OPERAND2 ((int)(uintptr_t)&_HOLE_OPERAND2)

// ===== EXIT REASONS (must match ExitReason enum) =====
static constexpr int EXIT_RETURN = 1;
static constexpr int EXIT_SEND = 2;
static constexpr int EXIT_SEND_CACHED = 7;
static constexpr int EXIT_BLOCK_CREATE = 8;

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

// Store into literal variable (no pop — TOS stays on stack)
// Bytecode: 0xF4
extern "C" void stencil_storeLitVar(JITState* s) {
    int idx = OPERAND;
    Oop value = s->sp[-1];
    Oop assoc = s->literals[idx];
    ObjectHeader* obj = asObjectPtr(assoc);
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

// SmallInteger less-equal
// Bytecode: 0x64 (arithmetic selector <=)
extern "C" void stencil_lessEqualSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        bool result = asSmallInteger(a) <= asSmallInteger(b);
        s->sp -= 2;
        *(s->sp) = result ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->sp++;
        _HOLE_CONTINUE(s);
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger greater-equal
// Bytecode: 0x65 (arithmetic selector >=)
extern "C" void stencil_greaterEqualSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        bool result = asSmallInteger(a) >= asSmallInteger(b);
        s->sp -= 2;
        *(s->sp) = result ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->sp++;
        _HOLE_CONTINUE(s);
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger integer division (//)
// Bytecode: 0x6D (arithmetic selector //)
extern "C" void stencil_divSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        int64_t bi = asSmallInteger(b);
        if (bi != 0) {
            int64_t ai = asSmallInteger(a);
            // Smalltalk // is floor division (rounds toward -infinity)
            int64_t result = ai / bi;
            if ((ai ^ bi) < 0 && result * bi != ai) result--;
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

// SmallInteger modulo (\\)
// Bytecode: 0x6A (arithmetic selector \\)
extern "C" void stencil_modSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        int64_t bi = asSmallInteger(b);
        if (bi != 0) {
            int64_t ai = asSmallInteger(a);
            // Smalltalk \\ is floor modulo (result has same sign as divisor)
            int64_t result = ai % bi;
            if (result != 0 && (ai ^ bi) < 0) result += bi;
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

// SmallInteger bitAnd:
// Bytecode: 0x6E (arithmetic selector bitAnd:)
extern "C" void stencil_bitAndSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        // Bitwise AND on tagged values: (a & b) preserves the tag
        // since both have tag 001 in bits 2:0
        uint64_t result = a.bits & b.bits;
        s->sp -= 2;
        *(s->sp) = Oop{result};
        s->sp++;
        _HOLE_CONTINUE(s);
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger bitOr:
// Bytecode: 0x6F (arithmetic selector bitOr:)
extern "C" void stencil_bitOrSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        // Bitwise OR on tagged values: (a | b) preserves the tag
        uint64_t result = a.bits | b.bits;
        s->sp -= 2;
        *(s->sp) = Oop{result};
        s->sp++;
        _HOLE_CONTINUE(s);
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// SmallInteger bitShift:
// Bytecode: 0x6C (arithmetic selector bitShift:)
extern "C" void stencil_bitShiftSmallInt(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];

    if (isSmallInteger(a) && isSmallInteger(b)) {
        int64_t ai = asSmallInteger(a);
        int64_t bi = asSmallInteger(b);

        if (bi >= 0 && bi < 60) {
            // Left shift — check for overflow
            int64_t result = ai << bi;
            if ((result >> bi) == ai && result >= -(1LL << 60) && result < (1LL << 60)) {
                s->sp -= 2;
                *(s->sp) = fromSmallInteger(result);
                s->sp++;
                _HOLE_CONTINUE(s);
                return;
            }
        } else if (bi < 0 && bi > -64) {
            // Right shift (arithmetic)
            int64_t result = ai >> (-bi);
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

// ----- MONOMORPHIC INLINE CACHE SEND -----
//
// OPERAND  = (argCount << 16) | bytecodeOffset
// OPERAND2 = pointer to IC data: uint64_t[2] = { cachedClassIndex, cachedMethodBits }
//
// On IC hit: exits with ExitSendCached + cachedTarget
// On IC miss: exits with ExitSend (interpreter does full lookup and patches IC)

extern "C" void stencil_sendPoly(JITState* s) {
    int packed = OPERAND;
    int bcOffset = packed & 0xFFFF;
    int nArgs = (packed >> 16) & 0xFF;

    // Load IC data pointer from literal pool (full 64-bit via GOT load)
    // Layout: 4 entries x 2 uint64_t = [key0, method0, key1, method1, ...]
    uint64_t* icData = (uint64_t*)(uintptr_t)&_HOLE_OPERAND2;

    // Get receiver: below the args on the stack
    Oop receiver = s->sp[-(nArgs + 1)];

    // Compute lookup key: classIndex for objects, tag|0x80000000 for immediates
    uint64_t lookupKey;
    uint64_t tag = receiver.bits & 0x7;
    if (tag == 0) {
        ObjectHeader* obj = reinterpret_cast<ObjectHeader*>(receiver.bits);
        lookupKey = obj->classIndex();
    } else {
        lookupKey = tag | 0x80000000ULL;
    }

    // Check 4 IC entries (unrolled for predictable code size)
    if (lookupKey == icData[0] && icData[0] != 0) {
        s->cachedTarget.bits = icData[1];
        s->icDataPtr = icData;
        s->sendArgCount = nArgs;
        s->ip = s->ip + bcOffset;
        s->exitReason = EXIT_SEND_CACHED;
        _HOLE_RT_SEND(s);
        return;
    }
    if (lookupKey == icData[2] && icData[2] != 0) {
        s->cachedTarget.bits = icData[3];
        s->icDataPtr = icData;
        s->sendArgCount = nArgs;
        s->ip = s->ip + bcOffset;
        s->exitReason = EXIT_SEND_CACHED;
        _HOLE_RT_SEND(s);
        return;
    }
    if (lookupKey == icData[4] && icData[4] != 0) {
        s->cachedTarget.bits = icData[5];
        s->icDataPtr = icData;
        s->sendArgCount = nArgs;
        s->ip = s->ip + bcOffset;
        s->exitReason = EXIT_SEND_CACHED;
        _HOLE_RT_SEND(s);
        return;
    }
    if (lookupKey == icData[6] && icData[6] != 0) {
        s->cachedTarget.bits = icData[7];
        s->icDataPtr = icData;
        s->sendArgCount = nArgs;
        s->ip = s->ip + bcOffset;
        s->exitReason = EXIT_SEND_CACHED;
        _HOLE_RT_SEND(s);
        return;
    }

    // IC MISS — probe megamorphic method cache before falling back
    {
        uint64_t selectorBits = icData[8];  // Stored at end of IC data by compiler
        if (selectorBits != 0) {
            MegaCacheEntry* cache = (MegaCacheEntry*)(uintptr_t)&_HOLE_MEGA_CACHE;
            size_t hash = (size_t)(selectorBits ^ lookupKey) & 4095;
            MegaCacheEntry* entry = &cache[hash];
            if (entry->selectorBits == selectorBits && entry->classIndex == lookupKey) {
                s->cachedTarget.bits = entry->methodBits;
                s->icDataPtr = icData;
                s->sendArgCount = nArgs;
                s->ip = s->ip + bcOffset;
                s->exitReason = EXIT_SEND_CACHED;
                _HOLE_RT_SEND(s);
                return;
            }
        }
    }

    // Mega cache miss — full interpreter lookup
    s->icDataPtr = icData;
    s->sendArgCount = nArgs;
    s->ip = s->ip + bcOffset;
    s->exitReason = EXIT_SEND;
    _HOLE_RT_SEND(s);
}

// ----- REMOTE TEMP STENCILS -----
//
// Remote temps are accessed through a temp vector (an Array stored in a local).
// Used by closures that capture variables from outer scopes.
// OPERAND = (vectorIndex << 8) | tempIndex

// Push Temp At k In Temp Vector At j
// Bytecode: 0xFB tempIndex vectorIndex
extern "C" void stencil_pushRemoteTemp(JITState* s) {
    int packed = OPERAND;
    int tempIndex = packed & 0xFF;
    int vectorIndex = (packed >> 8) & 0xFF;

    Oop tempVector = s->tempBase[vectorIndex];
    ObjectHeader* tvObj = asObjectPtr(tempVector);
    Oop value = tvObj->slots()[tempIndex];

    *(s->sp) = value;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// Store Temp At k In Temp Vector At j (no pop)
// Bytecode: 0xFC tempIndex vectorIndex
extern "C" void stencil_storeRemoteTemp(JITState* s) {
    int packed = OPERAND;
    int tempIndex = packed & 0xFF;
    int vectorIndex = (packed >> 8) & 0xFF;

    Oop value = s->sp[-1];  // TOS, no pop
    Oop tempVector = s->tempBase[vectorIndex];
    ObjectHeader* tvObj = asObjectPtr(tempVector);
    tvObj->slots()[tempIndex] = value;
    _HOLE_CONTINUE(s);
}

// Pop and Store Temp At k In Temp Vector At j
// Bytecode: 0xFD tempIndex vectorIndex
extern "C" void stencil_popStoreRemoteTemp(JITState* s) {
    int packed = OPERAND;
    int tempIndex = packed & 0xFF;
    int vectorIndex = (packed >> 8) & 0xFF;

    s->sp--;
    Oop value = *(s->sp);
    Oop tempVector = s->tempBase[vectorIndex];
    ObjectHeader* tvObj = asObjectPtr(tempVector);
    tvObj->slots()[tempIndex] = value;
    _HOLE_CONTINUE(s);
}

// ----- BLOCK CREATION STENCIL -----
//
// Exit to interpreter to create a FullBlockClosure, then resume JIT.
// OPERAND = (bcOffset << 16) | (litIndex & 0xFFFF)
// OPERAND2 = flags byte (numCopied:6 | ignoreOuterContext:1 | receiverOnStack:1)
extern "C" void stencil_pushBlock(JITState* s) {
    int packed = OPERAND;
    int bcOffset = (packed >> 16) & 0xFFFF;
    s->ip = s->ip + bcOffset;
    // Store litIndex and flags for the handler
    s->cachedTarget.bits = (static_cast<uint64_t>(packed & 0xFFFF)) |
                           (static_cast<uint64_t>(static_cast<uint32_t>(OPERAND2)) << 32);
    s->exitReason = EXIT_BLOCK_CREATE;
    _HOLE_RT_RETURN(s);
}

// ----- SPECIAL SELECTOR STENCILS -----

// == (identity compare): pop receiver and arg, push true/false
// This works for ALL receiver types — no class-specific behavior.
extern "C" void stencil_identicalTo(JITState* s) {
    Oop arg = s->sp[-1];
    Oop rcvr = s->sp[-2];
    s->sp -= 1;  // Pop arg, replace receiver with result
    s->sp[-1] = (rcvr.bits == arg.bits) ? _HOLE_TRUE_OOP : _HOLE_FALSE_OOP;
    _HOLE_CONTINUE(s);
}

// ~~ (identity not-equal): pop receiver and arg, push true/false
extern "C" void stencil_notIdenticalTo(JITState* s) {
    Oop arg = s->sp[-1];
    Oop rcvr = s->sp[-2];
    s->sp -= 1;
    s->sp[-1] = (rcvr.bits != arg.bits) ? _HOLE_TRUE_OOP : _HOLE_FALSE_OOP;
    _HOLE_CONTINUE(s);
}

// ----- SUPERINSTRUCTION STENCILS -----
//
// Fused comparison + conditional jump. Eliminates:
// - Boolean Oop creation (no true/false push)
// - Stack round-trip (no push then pop of boolean)
// - Stencil boundary overhead between comparison and jump
//
// OPERAND = bytecode offset (for deopt on non-SmallInteger)
// CONTINUE = fall-through (condition NOT taken)
// BRANCH_TARGET = jump target (condition taken)

// lessThan + jumpFalse: jump if NOT (a < b), i.e. a >= b
// Pattern: `i < n ifTrue: [body]` → jump over body if false
extern "C" void stencil_ltJumpFalse(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) < asSmallInteger(b)) {
            _HOLE_CONTINUE(s);
        } else {
            _HOLE_BRANCH_TARGET(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// lessThan + jumpTrue: jump if a < b
// Pattern: `i < n ifFalse: [body]` → jump over body if true
extern "C" void stencil_ltJumpTrue(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) < asSmallInteger(b)) {
            _HOLE_BRANCH_TARGET(s);
        } else {
            _HOLE_CONTINUE(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// greaterThan + jumpFalse: jump if NOT (a > b), i.e. a <= b
extern "C" void stencil_gtJumpFalse(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) > asSmallInteger(b)) {
            _HOLE_CONTINUE(s);
        } else {
            _HOLE_BRANCH_TARGET(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// greaterThan + jumpTrue: jump if a > b
extern "C" void stencil_gtJumpTrue(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) > asSmallInteger(b)) {
            _HOLE_BRANCH_TARGET(s);
        } else {
            _HOLE_CONTINUE(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// lessEqual + jumpFalse: jump if NOT (a <= b), i.e. a > b
extern "C" void stencil_leJumpFalse(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) <= asSmallInteger(b)) {
            _HOLE_CONTINUE(s);
        } else {
            _HOLE_BRANCH_TARGET(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// lessEqual + jumpTrue: jump if a <= b
extern "C" void stencil_leJumpTrue(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) <= asSmallInteger(b)) {
            _HOLE_BRANCH_TARGET(s);
        } else {
            _HOLE_CONTINUE(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// greaterEqual + jumpFalse: jump if NOT (a >= b), i.e. a < b
extern "C" void stencil_geJumpFalse(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) >= asSmallInteger(b)) {
            _HOLE_CONTINUE(s);
        } else {
            _HOLE_BRANCH_TARGET(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// greaterEqual + jumpTrue: jump if a >= b
extern "C" void stencil_geJumpTrue(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) >= asSmallInteger(b)) {
            _HOLE_BRANCH_TARGET(s);
        } else {
            _HOLE_CONTINUE(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// equal + jumpFalse: jump if NOT (a = b)
extern "C" void stencil_eqJumpFalse(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) == asSmallInteger(b)) {
            _HOLE_CONTINUE(s);
        } else {
            _HOLE_BRANCH_TARGET(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// equal + jumpTrue: jump if a = b
extern "C" void stencil_eqJumpTrue(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) == asSmallInteger(b)) {
            _HOLE_BRANCH_TARGET(s);
        } else {
            _HOLE_CONTINUE(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// notEqual + jumpFalse: jump if NOT (a ~= b), i.e. a = b
extern "C" void stencil_neqJumpFalse(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) != asSmallInteger(b)) {
            _HOLE_CONTINUE(s);
        } else {
            _HOLE_BRANCH_TARGET(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// notEqual + jumpTrue: jump if a ~= b
extern "C" void stencil_neqJumpTrue(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    if (isSmallInteger(a) && isSmallInteger(b)) {
        s->sp -= 2;
        if (asSmallInteger(a) != asSmallInteger(b)) {
            _HOLE_BRANCH_TARGET(s);
        } else {
            _HOLE_CONTINUE(s);
        }
        return;
    }
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

// ----- IDENTITY + JUMP SUPERINSTRUCTIONS -----
//
// Fused identity comparison + conditional jump. No type guard needed —
// identity comparison works on all types. No deopt path.

// == + jumpFalse: jump if NOT identical (a ~~ b)
// Pattern: `x == nil ifTrue: [body]` → jump over body if not identical
extern "C" void stencil_identJumpFalse(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    s->sp -= 2;
    if (a.bits == b.bits) {
        _HOLE_CONTINUE(s);
    } else {
        _HOLE_BRANCH_TARGET(s);
    }
}

// == + jumpTrue: jump if identical (a == b)
// Pattern: `x == nil ifFalse: [body]` → jump over body if identical
extern "C" void stencil_identJumpTrue(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    s->sp -= 2;
    if (a.bits == b.bits) {
        _HOLE_BRANCH_TARGET(s);
    } else {
        _HOLE_CONTINUE(s);
    }
}

// ~~ + jumpFalse: jump if identical (a == b)
// Pattern: `x ~~ nil ifTrue: [body]` → jump over body if identical
extern "C" void stencil_notIdentJumpFalse(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    s->sp -= 2;
    if (a.bits != b.bits) {
        _HOLE_CONTINUE(s);
    } else {
        _HOLE_BRANCH_TARGET(s);
    }
}

// ~~ + jumpTrue: jump if not identical (a ~~ b)
// Pattern: `x ~~ nil ifFalse: [body]` → jump over body if not identical
extern "C" void stencil_notIdentJumpTrue(JITState* s) {
    Oop b = s->sp[-1];
    Oop a = s->sp[-2];
    s->sp -= 2;
    if (a.bits != b.bits) {
        _HOLE_BRANCH_TARGET(s);
    } else {
        _HOLE_CONTINUE(s);
    }
}

// ----- NOP STENCIL -----

// Used for bytecodes we skip or as padding
extern "C" void stencil_nop(JITState* s) {
    _HOLE_CONTINUE(s);
}
