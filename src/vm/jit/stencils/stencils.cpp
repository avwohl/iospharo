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

    // Slot accessors (match ObjectHeader.hpp)
    Oop slotAt(size_t index) const { return slots()[index]; }
    void slotAtPut(size_t index, Oop value) { slots()[index] = value; }

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
    // SimStack register caching
    uint64_t      simTOS;       // offset 112
    uint64_t      simNOS;       // offset 120
    // Inline primitive support
    Oop           trueOop;      // offset 128
    Oop           falseOop;     // offset 136
    // J2J stencil-to-stencil support
    uint8_t*      j2jSaveCursor; // offset 144
    uint8_t*      j2jSaveLimit;  // offset 152
    int32_t       j2jDepth;      // offset 160
    int32_t       j2jTotalCalls; // offset 164
};

// J2JSave matches the struct in Interpreter.cpp exactly (72 bytes).
// Field order: hot fields first for STP/LDP pairing.
struct J2JSave {
    Oop*     sp;              // 0
    Oop      receiver;        // 8
    Oop*     tempBase;        // 16
    uint8_t* ip;              // 24
    void*    jitMethod;       // 32 (bit 0 = self-recursive marker)
    uint8_t* resumeAddr;      // 40
    int      sendArgCount;    // 48
    int      argCount;        // 52
    Oop*     literals;        // 56
    uint8_t* bcStart;         // 64
};
static_assert(sizeof(J2JSave) == 72, "J2JSave must be 72 bytes");

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
    extern void (*_HOLE_RT_ARITH_OVERFLOW)(JITState*);

    // Megamorphic method cache (address resolved via literal pool)
    extern char _HOLE_MEGA_CACHE;

    // Resume address: address of the next stencil, used as a DATA value
    // (stored in J2JSave.resumeAddr) rather than a branch target.
    // Patched with the same value as _HOLE_CONTINUE.
    extern char _HOLE_RESUME_ADDR;
}

// _HOLE_RT_SEND / _HOLE_RT_RETURN used to tail-call jit_rt_send /
// jit_rt_return helpers. Those helpers were empty no-ops: stencil set
// state.exitReason, then branched through a 3-load GOT indirection to
// a helper that just returned. Since a void stencil function naturally
// returns to its caller, we can skip the helper entirely — define these
// as empty macros so Clang drops the adrp+ldr+ldr+br sequence at each
// exit site (~4 instructions saved per send/return, ~4M exits per fib(28)).
#define _HOLE_RT_SEND(s)    do { (void)(s); } while (0)
#define _HOLE_RT_RETURN(s)  do { (void)(s); } while (0)

// Helper to get operand value (address of hole symbol = the operand integer)
#define OPERAND  ((int)(uintptr_t)&_HOLE_OPERAND)
#define OPERAND2 ((int)(uintptr_t)&_HOLE_OPERAND2)
#define RESUME_ADDR ((uint8_t*)(uintptr_t)&_HOLE_RESUME_ADDR)

// ===== EXIT REASONS (must match ExitReason enum) =====
static constexpr int EXIT_RETURN = 1;
static constexpr int EXIT_SEND = 2;
static constexpr int EXIT_SEND_CACHED = 7;
static constexpr int EXIT_BLOCK_CREATE = 8;
static constexpr int EXIT_ARRAY_CREATE = 9;
static constexpr int EXIT_J2J_CALL = 10;

// ===== JITMethod field offsets (must match JITMethod.hpp) =====
// Used for raw pointer access from stencils (no header include).
static constexpr int JM_COMPILED_METHOD = 0;   // uint64_t compiledMethodOop
static constexpr int JM_METHOD_HEADER   = 16;  // uint64_t methodHeader
static constexpr int JM_TEMP_COUNT      = 35;  // uint8_t  tempCount
static constexpr int JM_ARG_COUNT       = 34;  // uint8_t  argCount
static constexpr int JM_SIZE            = 80;  // sizeof(JITMethod)

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

// J2J inline return: if j2jDepth > 0, pop save frame and tail-call
// to caller's resume stencil.  Otherwise fall through to normal exit.
// retVal is the Oop to push onto the caller's stack.
#define J2J_INLINE_RETURN(s, retVal) do {                                     \
    if (s->j2jDepth > 0) {                                                   \
        s->j2jDepth--;                                                        \
        s->j2jSaveCursor -= sizeof(J2JSave);                                 \
        J2JSave* _sv = (J2JSave*)s->j2jSaveCursor;                          \
        /* Restore caller state */                                           \
        s->receiver = _sv->receiver;                                          \
        s->tempBase = _sv->tempBase;                                         \
        uintptr_t _jmBits = (uintptr_t)_sv->jitMethod;                      \
        if ((_jmBits & 1) == 0) {                                            \
            /* Non-self-recursive: restore literals, jitMethod, argCount,    \
               ip (bcStart) */                                               \
            s->literals = _sv->literals;                                      \
            s->jitMethod = _sv->jitMethod;                                   \
            s->argCount = _sv->argCount;                                     \
            s->ip = _sv->bcStart;                                            \
        }                                                                     \
        /* Pop receiver+args, push retVal */                                 \
        int _nArgs = _sv->sendArgCount;                                      \
        _sv->sp[-(_nArgs + 1)] = retVal;                                     \
        s->sp = _sv->sp - _nArgs;                                           \
        /* Tail-call to caller's resume stencil */                           \
        uint8_t* _resume = _sv->resumeAddr;                                  \
        if (_resume != 0) {                                                  \
            ((void(*)(JITState*))_resume)(s);                                \
            return;                                                           \
        }                                                                     \
        /* Null resume: bail to interpreter */                               \
        s->ip = _sv->ip;                                                     \
        s->returnValue = retVal;                                              \
        s->exitReason = EXIT_RETURN;                                          \
        return;                                                               \
    }                                                                         \
} while(0)

// Return top of stack
// Bytecode: 0x5C
extern "C" void stencil_returnTop(JITState* s) {
    s->sp--;
    Oop retVal = *(s->sp);
    J2J_INLINE_RETURN(s, retVal);
    s->returnValue = retVal;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// Return receiver (self)
// Bytecode: 0x58
extern "C" void stencil_returnReceiver(JITState* s) {
    Oop retVal = s->receiver;
    J2J_INLINE_RETURN(s, retVal);
    s->returnValue = retVal;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// Return true
// Bytecode: 0x59
extern "C" void stencil_returnTrue(JITState* s) {
    Oop retVal = *(Oop*)&_HOLE_TRUE_OOP;
    J2J_INLINE_RETURN(s, retVal);
    s->returnValue = retVal;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// Return false
// Bytecode: 0x5A
extern "C" void stencil_returnFalse(JITState* s) {
    Oop retVal = *(Oop*)&_HOLE_FALSE_OOP;
    J2J_INLINE_RETURN(s, retVal);
    s->returnValue = retVal;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// Return nil
// Bytecode: 0x5B
extern "C" void stencil_returnNil(JITState* s) {
    Oop retVal = *(Oop*)&_HOLE_NIL_OOP;
    J2J_INLINE_RETURN(s, retVal);
    s->returnValue = retVal;
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
    // Layout: 4 entries x 3 uint64_t = [key0, method0, extra0, key1, method1, extra1, ...]
    // extra encodes inline getter/setter info for J2J dispatch:
    //   bit 63 set = getter, bits 15:0 = slot index
    //   bit 62 set = setter, bits 15:0 = slot index
    //   bit 61 set = returnsSelf (e.g. "yourself")
    // After 4 entries: icData[12] = selectorBits for megacache
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

    // Macro for inline getter/setter dispatch on IC hit
    // Avoids exiting to C++ for trivial methods (~500ns savings per send)
#define IC_HIT(entry_idx) do {                                              \
    uint64_t extra = icData[(entry_idx) * 3 + 2];                           \
    if (extra != 0 && tag == 0) {                                           \
        uint16_t slotIdx = (uint16_t)(extra & 0xFFFF);                      \
        ObjectHeader* recvObj = reinterpret_cast<ObjectHeader*>(receiver.bits); \
        if (extra & (1ULL << 63)) {                                         \
            /* Inline getter: replace receiver+args with field value */      \
            Oop val = recvObj->slotAt(slotIdx);                             \
            s->sp[-(nArgs + 1)] = val;                                      \
            s->sp -= nArgs;                                                 \
            _HOLE_CONTINUE(s);                                              \
            return;                                                         \
        }                                                                   \
        if (extra & (1ULL << 62)) {                                         \
            /* Inline setter: store arg to slot, return self */             \
            Oop arg = s->sp[-(nArgs)];                                      \
            recvObj->slotAtPut(slotIdx, arg);                               \
            s->sp[-(nArgs + 1)] = receiver;                                 \
            s->sp -= nArgs;                                                 \
            _HOLE_CONTINUE(s);                                              \
            return;                                                         \
        }                                                                   \
        if (extra & (1ULL << 61)) {                                         \
            /* returnsSelf: pop args, leave receiver on stack */             \
            s->sp -= nArgs;                                                 \
            _HOLE_CONTINUE(s);                                              \
            return;                                                         \
        }                                                                   \
    }                                                                       \
    s->cachedTarget.bits = icData[(entry_idx) * 3 + 1];                     \
    s->icDataPtr = icData;                                                  \
    s->sendArgCount = nArgs;                                                \
    s->ip = s->ip + bcOffset;                                               \
    s->exitReason = EXIT_SEND_CACHED;                                       \
    _HOLE_RT_SEND(s);                                                       \
    return;                                                                 \
} while(0)

    // Check 4 IC entries (unrolled for predictable code size)
    if (lookupKey == icData[0] && icData[0] != 0) { IC_HIT(0); }
    if (lookupKey == icData[3] && icData[3] != 0) { IC_HIT(1); }
    if (lookupKey == icData[6] && icData[6] != 0) { IC_HIT(2); }
    if (lookupKey == icData[9] && icData[9] != 0) { IC_HIT(3); }

#undef IC_HIT

    // IC MISS — probe megamorphic method cache before falling back
    {
        uint64_t selectorBits = icData[12];  // Stored at end of IC data by compiler
        if (selectorBits != 0) {
            MegaCacheEntry* cache = (MegaCacheEntry*)(uintptr_t)&_HOLE_MEGA_CACHE;
            // Primary probe
            size_t hash = (size_t)(selectorBits ^ lookupKey) & 65535;
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
            // Secondary probe (rotated hash)
            size_t hash2 = (size_t)((selectorBits >> 3) ^ (lookupKey << 2) ^ lookupKey) & 65535;
            entry = &cache[hash2];
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

// ----- JIT-TO-JIT SEND STENCIL -----
//
// Same as stencil_sendPoly but adds J2J direct calls for IC hits where the
// target method has compiled JIT code. The IC extra word bit 60 signals a
// J2J entry: bits 47:0 contain the target's JIT code entry address.
//
// When bit 60 is set (and bits 63:61 are clear), the stencil:
//   1. Saves caller JITState to C stack (Clang handles via callee-saved regs)
//   2. Calls jit_rt_push_frame to push interpreter frame for GC
//   3. BLR to target JIT code entry point
//   4. Callee returns via RET (return stencils preserve LR chain)
//   5. Calls jit_rt_pop_frame to restore interpreter frame
//   6. Restores caller JITState, pushes returnValue, continues
//
// OPERAND  = (argCount << 16) | bytecodeOffset
// OPERAND2 = pointer to IC data

// J2J entry bit in IC extra word
static constexpr uint64_t J2J_ENTRY_BIT = (1ULL << 60);
static constexpr uint64_t J2J_ADDR_MASK = 0x0000FFFFFFFFFFFFULL;

extern "C" void (*_HOLE_RT_PUSH_FRAME)(JITState*);
extern "C" void (*_HOLE_RT_POP_FRAME)(JITState*);
extern "C" void (*_HOLE_RT_J2J_CALL)(JITState*);
extern "C" uint64_t (*_HOLE_RT_ARRAY_PRIM)(JITState*, uint64_t);

extern "C" void stencil_sendJ2J(JITState* s) {
    int packed = OPERAND;
    int bcOffset = packed & 0xFFFF;
    int nArgs = (packed >> 16) & 0xFF;
    int bcLen = (packed >> 24) & 0xFF;  // send bytecode length for J2J saved IP

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

    // Macro for IC hit handling with J2J support.
    // For primKind >= 14 (array ops at:/at:put:/size), calls the shared
    // runtime helper via GOT. On success, continues; on failure, the
    // SmallInt tag check naturally fails (receiver is an object, tag=0)
    // and execution falls through to J2J/SEND_CACHED.

#define J2J_IC_HIT(entry_idx) do {                                            \
    uint64_t extra = icData[(entry_idx) * 3 + 2];                             \
    if (extra != 0 && tag == 0) {                                             \
        uint16_t slotIdx = (uint16_t)(extra & 0xFFFF);                        \
        ObjectHeader* recvObj = reinterpret_cast<ObjectHeader*>(receiver.bits);\
        if (extra & (1ULL << 63)) {                                           \
            /* Inline getter */                                                \
            Oop val = recvObj->slotAt(slotIdx);                               \
            s->sp[-(nArgs + 1)] = val;                                        \
            s->sp -= nArgs;                                                   \
            _HOLE_CONTINUE(s);                                                \
            return;                                                           \
        }                                                                     \
        if (extra & (1ULL << 62)) {                                           \
            /* Inline setter */                                                \
            Oop arg = s->sp[-(nArgs)];                                        \
            recvObj->slotAtPut(slotIdx, arg);                                 \
            s->sp[-(nArgs + 1)] = receiver;                                   \
            s->sp -= nArgs;                                                   \
            _HOLE_CONTINUE(s);                                                \
            return;                                                           \
        }                                                                     \
        if (extra & (1ULL << 61)) {                                           \
            /* returnsSelf */                                                  \
            s->sp -= nArgs;                                                   \
            _HOLE_CONTINUE(s);                                                \
            return;                                                           \
        }                                                                     \
    }                                                                         \
    /* Lightweight inline primitive (bits 52:48 = primKind, no frame needed)*/ \
    if ((extra >> 48) & 0x1F) {                                               \
        uint8_t primKind = (uint8_t)((extra >> 48) & 0x1F);                   \
        if (primKind != 0) {                                                  \
            /* Array prims (at:/at:put:/size) — out-of-line helper call */    \
            if (primKind >= 14) {                                             \
                uint64_t info = ((uint64_t)primKind << 8) | (uint64_t)nArgs; \
                if (_HOLE_RT_ARRAY_PRIM(s, info)) {                           \
                    _HOLE_CONTINUE(s);                                        \
                    return;                                                   \
                }                                                             \
                /* Failure: fall through to J2J/SEND_CACHED below */          \
            }                                                                 \
            Oop rcv = s->sp[-2];                                              \
            Oop arg = s->sp[-1];                                              \
            /* Both must be SmallInteger: (bits & 7) == 1 */                  \
            if ((rcv.bits & 7) == 1 && (arg.bits & 7) == 1) {                \
                int64_t a = (int64_t)rcv.bits >> 3;                           \
                int64_t b = (int64_t)arg.bits >> 3;                           \
                bool ok = false;                                              \
                Oop result;                                                   \
                if (primKind <= 2) { /* add(1), sub(2) */                     \
                    int64_t r = (primKind == 1) ? a + b : a - b;             \
                    /* Overflow: result must fit in 61-bit signed value */    \
                    if (r >= -(1LL << 60) && r < (1LL << 60)) {              \
                        result.bits = (uint64_t)(r << 3) | 1;                \
                        ok = true;                                            \
                    }                                                         \
                } else if (primKind <= 8) { /* comparisons 3-8 */            \
                    /* For SmallIntegers with same tag, raw signed           */ \
                    /* comparison works: both have tag 1 in low 3 bits       */ \
                    bool cmp;                                                 \
                    int64_t sa = (int64_t)rcv.bits;                           \
                    int64_t sb = (int64_t)arg.bits;                           \
                    switch (primKind) {                                       \
                    case 3: cmp = sa < sb; break;                            \
                    case 4: cmp = sa > sb; break;                            \
                    case 5: cmp = sa <= sb; break;                           \
                    case 6: cmp = sa >= sb; break;                           \
                    case 7: cmp = sa == sb; break;                           \
                    case 8: cmp = sa != sb; break;                           \
                    default: cmp = false;                                     \
                    }                                                         \
                    result = cmp ? s->trueOop : s->falseOop;                 \
                    ok = true;                                                \
                } else if (primKind == 9) { /* mul */                        \
                    __int128 r128 = (__int128)a * (__int128)b;               \
                    if (r128 >= -(1LL << 60) && r128 < (1LL << 60)) {       \
                        result.bits = (uint64_t)((int64_t)r128 << 3) | 1;   \
                        ok = true;                                            \
                    }                                                         \
                } else if (primKind == 10) { /* identical */                 \
                    result = (rcv.bits == arg.bits) ?                         \
                        s->trueOop : s->falseOop;                            \
                    ok = true;                                                \
                } else if (primKind == 11) { /* bitAnd */                    \
                    /* SmallInt & SmallInt: just AND the raw bits.           */ \
                    /* Tag bits (low 3 = 001) are preserved by AND.         */ \
                    result.bits = rcv.bits & arg.bits;                        \
                    ok = true;                                                \
                } else if (primKind == 12) { /* bitOr */                     \
                    /* SmallInt | SmallInt: OR raw bits preserves tag.       */ \
                    result.bits = rcv.bits | arg.bits;                        \
                    ok = true;                                                \
                } else if (primKind == 13) { /* bitShift */                  \
                    /* arg (b) is the shift amount. Positive=left, neg=right*/ \
                    if (b >= 0 && b < 61) {                                  \
                        int64_t r = a << b;                                  \
                        if ((r >> b) == a) { /* no overflow */               \
                            result.bits = (uint64_t)(r << 3) | 1;           \
                            ok = true;                                       \
                        }                                                    \
                    } else if (b < 0 && b > -64) {                          \
                        int64_t r = a >> (-b);                               \
                        result.bits = (uint64_t)(r << 3) | 1;               \
                        ok = true;                                           \
                    }                                                        \
                }                                                             \
                if (ok) {                                                     \
                    s->sp[-2] = result;                                       \
                    s->sp--;                                                  \
                    _HOLE_CONTINUE(s);                                        \
                    return;                                                   \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    /* Check for J2J direct call (bit 60, no bits 63:61) */                   \
    if (extra & J2J_ENTRY_BIT) {                                              \
        uint64_t entryAddr = extra & J2J_ADDR_MASK;                           \
        if (entryAddr != 0) {                                                 \
            /* Stencil-to-stencil J2J: push frame and tail-call callee */     \
            if ((uintptr_t)s->j2jSaveCursor >=                               \
                (uintptr_t)s->j2jSaveLimit) {                                \
                /* Save stack full — bail to interpreter */                   \
                s->cachedTarget.bits = icData[(entry_idx) * 3 + 1];          \
                s->sendArgCount = nArgs;                                      \
                s->ip = s->ip + bcOffset;                                     \
                s->exitReason = EXIT_SEND_CACHED;                             \
                return;                                                       \
            }                                                                 \
            J2JSave* _save = (J2JSave*)s->j2jSaveCursor;                     \
            _save->sp = s->sp;                                                \
            _save->receiver = s->receiver;                                    \
            _save->tempBase = s->tempBase;                                    \
            _save->ip = s->ip + bcOffset + bcLen;                            \
            _save->sendArgCount = nArgs;                                      \
            _save->resumeAddr = RESUME_ADDR;                                 \
            /* Detect self-recursive: calleeJM = entryAddr - JM_SIZE */      \
            uint8_t* _calleeJM = (uint8_t*)entryAddr - JM_SIZE;             \
            void* _callerJM = s->jitMethod;                                  \
            if (_callerJM == (void*)_calleeJM) {                             \
                _save->jitMethod =                                           \
                    (void*)((uintptr_t)_callerJM | 1ULL);                    \
            } else {                                                         \
                _save->jitMethod = _callerJM;                                \
                _save->literals = s->literals;                               \
                _save->argCount = s->argCount;                               \
                /* callerBCStart from JITMethod fields */                    \
                uint64_t _mh = *(uint64_t*)((uint8_t*)_callerJM             \
                                            + JM_METHOD_HEADER);             \
                int _nl = (int)(_mh & 0x7FFF);                               \
                uint64_t _cmo = *(uint64_t*)((uint8_t*)_callerJM            \
                                             + JM_COMPILED_METHOD);          \
                _save->bcStart = (uint8_t*)_cmo + (_nl + 2) * 8;            \
            }                                                                 \
            s->j2jSaveCursor += sizeof(J2JSave);                             \
            s->j2jDepth++;                                                    \
            s->j2jTotalCalls++;                                              \
            /* Setup callee */                                               \
            Oop _calleeRecv = s->sp[-(nArgs + 1)];                          \
            Oop* _fp = s->sp - (nArgs + 1);                                 \
            s->receiver = _calleeRecv;                                       \
            s->tempBase = _fp + 1;                                           \
            if (_callerJM != (void*)_calleeJM) {                             \
                uint64_t _methBits = icData[(entry_idx) * 3 + 1];           \
                ObjectHeader* _mo = (ObjectHeader*)_methBits;                \
                s->literals = _mo->slots() + 1;                              \
                s->argCount = nArgs;                                         \
                s->jitMethod = (void*)_calleeJM;                             \
                uint64_t _ch = _mo->slotAt(0).bits;                         \
                int _cnl = (_ch & 1) ? (int)((_ch >> 3) & 0x7FFF) : 0;     \
                s->ip = (uint8_t*)_mo + 8 + (1 + _cnl) * 8;                \
            } else {                                                         \
                /* Self-recursive: ip = callerBCStart */                     \
                uint64_t _mh = *(uint64_t*)((uint8_t*)_callerJM             \
                                            + JM_METHOD_HEADER);             \
                int _nl = (int)(_mh & 0x7FFF);                               \
                uint64_t _cmo = *(uint64_t*)((uint8_t*)_callerJM            \
                                             + JM_COMPILED_METHOD);          \
                s->ip = (uint8_t*)_cmo + (_nl + 2) * 8;                     \
            }                                                                 \
            /* Allocate temps if needed */                                   \
            uint8_t _tc = *((uint8_t*)_calleeJM + JM_TEMP_COUNT);          \
            if (nArgs < (int)_tc) {                                         \
                Oop _nil = *(Oop*)&_HOLE_NIL_OOP;                           \
                for (int _t = nArgs; _t < (int)_tc; _t++) {                 \
                    *(s->sp) = _nil;                                         \
                    s->sp++;                                                  \
                }                                                             \
            }                                                                 \
            /* Tail-call to callee JIT entry */                              \
            ((void(*)(JITState*))entryAddr)(s);                              \
            return;                                                           \
        }                                                                     \
    }                                                                         \
    /* Fallback: exit to C++ with cached target */                            \
    s->cachedTarget.bits = icData[(entry_idx) * 3 + 1];                       \
    s->icDataPtr = icData;                                                    \
    s->sendArgCount = nArgs;                                                  \
    s->ip = s->ip + bcOffset;                                                 \
    s->exitReason = EXIT_SEND_CACHED;                                         \
    _HOLE_RT_SEND(s);                                                         \
    return;                                                                   \
} while(0)

    // Check 4 IC entries (unrolled for predictable code size)
    if (lookupKey == icData[0] && icData[0] != 0) { J2J_IC_HIT(0); }
    if (lookupKey == icData[3] && icData[3] != 0) { J2J_IC_HIT(1); }
    if (lookupKey == icData[6] && icData[6] != 0) { J2J_IC_HIT(2); }
    if (lookupKey == icData[9] && icData[9] != 0) { J2J_IC_HIT(3); }

#undef J2J_IC_HIT

    // IC MISS — probe megamorphic method cache before falling back
    {
        uint64_t selectorBits = icData[12];
        if (selectorBits != 0) {
            MegaCacheEntry* cache = (MegaCacheEntry*)(uintptr_t)&_HOLE_MEGA_CACHE;
            // Primary probe
            size_t hash = (size_t)(selectorBits ^ lookupKey) & 65535;
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
            // Secondary probe (rotated hash)
            size_t hash2 = (size_t)((selectorBits >> 3) ^ (lookupKey << 2) ^ lookupKey) & 65535;
            entry = &cache[hash2];
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

// ----- ARRAY CREATION STENCIL -----
//
// Exit to interpreter to allocate a Smalltalk Array, then resume JIT.
// OPERAND = desc byte from 0xE7: bits 0-6 = arraySize, bit 7 = popIntoArray
extern "C" void stencil_pushArray(JITState* s) {
    s->cachedTarget.bits = static_cast<uint64_t>(static_cast<uint32_t>(OPERAND));
    s->exitReason = EXIT_ARRAY_CREATE;
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

// =====================================================================
// PRIMITIVE PROLOGUE STENCILS
// =====================================================================
//
// Machine-code fast paths for hot primitive methods. These run as the
// first stencil in a compiled method. If the type check passes, they
// set returnValue + EXIT_RETURN and tail-call _HOLE_RT_RETURN (which
// RETs to the caller via LR — exactly like return stencils). If the
// type check fails, they fall through to _HOLE_CONTINUE (the bytecodes).
//
// With J2J direct calls, this means: caller BLR → prologue → RET.
// Total ~12 cycles for the common case.
//
// Receiver = s->receiver, First arg = s->tempBase[0]
// (pushFrameForJIT sets tempBase = framePointer + 1, where frame is
//  [receiver, arg0, arg1, ...] so tempBase[0] = first arg)

// SmallInteger overflow check constant
// iOS Oop encoding: 3-bit tag, 61-bit value. Max SmallInteger = 2^60-1
static constexpr int64_t SmallIntMax = (1LL << 60) - 1;
static constexpr int64_t SmallIntMin = -(1LL << 60);

static inline bool canBeSmallInt(int64_t v) {
    return v >= SmallIntMin && v <= SmallIntMax;
}

// ----- Primitive 1: SmallInteger #+ -----
extern "C" void stencil_primAdd(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        int64_t result = a + b;
        if (canBeSmallInt(result)) {
            s->returnValue = fromSmallInteger(result);
            s->exitReason = EXIT_RETURN;
            _HOLE_RT_RETURN(s);
            return;
        }
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 2: SmallInteger #- -----
extern "C" void stencil_primSub(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        int64_t result = a - b;
        if (canBeSmallInt(result)) {
            s->returnValue = fromSmallInteger(result);
            s->exitReason = EXIT_RETURN;
            _HOLE_RT_RETURN(s);
            return;
        }
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 3: SmallInteger #< -----
extern "C" void stencil_primLessThan(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        s->returnValue = (a < b) ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 4: SmallInteger #> -----
extern "C" void stencil_primGreaterThan(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        s->returnValue = (a > b) ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 5: SmallInteger #<= -----
extern "C" void stencil_primLessEqual(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        s->returnValue = (a <= b) ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 6: SmallInteger #>= -----
extern "C" void stencil_primGreaterEqual(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        s->returnValue = (a >= b) ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 7: SmallInteger #= -----
extern "C" void stencil_primEqual(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        s->returnValue = (rcvr.bits == arg.bits) ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 8: SmallInteger #~= -----
extern "C" void stencil_primNotEqual(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        s->returnValue = (rcvr.bits != arg.bits) ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 9: SmallInteger #* -----
extern "C" void stencil_primMul(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        // Use __int128 to detect overflow
        __int128 result = (__int128)a * (__int128)b;
        if (result >= SmallIntMin && result <= SmallIntMax) {
            s->returnValue = fromSmallInteger((int64_t)result);
            s->exitReason = EXIT_RETURN;
            _HOLE_RT_RETURN(s);
            return;
        }
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 10: SmallInteger #/ (quotient, must divide evenly) -----
extern "C" void stencil_primQuo(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        if (b != 0 && (a % b) == 0) {
            int64_t result = a / b;
            if (canBeSmallInt(result)) {
                s->returnValue = fromSmallInteger(result);
                s->exitReason = EXIT_RETURN;
                _HOLE_RT_RETURN(s);
                return;
            }
        }
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 11: SmallInteger #\\ (modulo) -----
extern "C" void stencil_primMod(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        if (b != 0) {
            int64_t result = a % b;
            // Smalltalk mod: result has same sign as divisor
            if (result != 0 && ((result ^ b) < 0)) result += b;
            s->returnValue = fromSmallInteger(result);
            s->exitReason = EXIT_RETURN;
            _HOLE_RT_RETURN(s);
            return;
        }
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 12: SmallInteger #// (integer divide, truncate toward negative infinity) -----
extern "C" void stencil_primDiv(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t a = asSmallInteger(rcvr);
        int64_t b = asSmallInteger(arg);
        if (b != 0) {
            // Smalltalk //: truncate toward negative infinity
            int64_t result = a / b;
            if ((a % b != 0) && ((a ^ b) < 0)) result--;
            if (canBeSmallInt(result)) {
                s->returnValue = fromSmallInteger(result);
                s->exitReason = EXIT_RETURN;
                _HOLE_RT_RETURN(s);
                return;
            }
        }
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 14: SmallInteger #bitAnd: -----
extern "C" void stencil_primBitAnd(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        // Bit ops on tagged values: (a & b) preserves SmallInteger tag
        s->returnValue.bits = rcvr.bits & arg.bits;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 15: SmallInteger #bitOr: -----
extern "C" void stencil_primBitOr(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        s->returnValue.bits = rcvr.bits | arg.bits;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    _HOLE_CONTINUE(s);
}

// ----- Primitive 17: SmallInteger #bitShift: -----
extern "C" void stencil_primBitShift(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    if (isSmallInteger(rcvr) && isSmallInteger(arg)) {
        int64_t value = asSmallInteger(rcvr);
        int64_t shift = asSmallInteger(arg);
        int64_t result;
        if (shift >= 0) {
            if (shift >= 63) goto fail;
            result = value << shift;
            // Check no bits lost
            if ((result >> shift) != value || !canBeSmallInt(result)) goto fail;
        } else {
            if (shift <= -64) result = (value < 0) ? -1 : 0;
            else result = value >> (-shift);
        }
        s->returnValue = fromSmallInteger(result);
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
fail:
    _HOLE_CONTINUE(s);
}

// ----- Primitive 110: Object #== (identity) -----
extern "C" void stencil_primIdentical(JITState* s) {
    Oop rcvr = s->receiver;
    Oop arg = s->tempBase[0];
    s->returnValue = (rcvr.bits == arg.bits) ? *(Oop*)&_HOLE_TRUE_OOP : *(Oop*)&_HOLE_FALSE_OOP;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// ----- Primitive 111: Object #class -----
extern "C" void stencil_primClass(JITState* s) {
    // Class is looked up via ObjectMemory, which we can't access from stencils.
    // Fall through to bytecodes.
    _HOLE_CONTINUE(s);
}

// ----- Primitive 60: Object #at: -----
// Fast path for Array (format 2, no fixed fields).
// Handles only pointer-indexable objects with slotCount < 255 (no overflow header).
extern "C" void stencil_primAt(JITState* s) {
    Oop rcvr = s->receiver;
    Oop idx = s->tempBase[0];
    // Index must be SmallInteger
    if ((idx.bits & 7) != 1) { _HOLE_CONTINUE(s); return; }
    // Receiver must be an object pointer (tag == 0, not immediate)
    if ((rcvr.bits & 7) != 0 || rcvr.bits < 0x10000) { _HOLE_CONTINUE(s); return; }
    // Read object header directly
    uint64_t header = *reinterpret_cast<uint64_t*>(rcvr.bits);
    uint64_t fmt = (header >> 24) & 0x1F;
    uint64_t slotCount = (header >> 56) & 0xFF;
    if (slotCount == 255) {
        // Overflow: actual slot count in the 8 bytes before the object header.
        // Mask off top byte (Spur puts 0xFF there as a marker).
        uint64_t raw = *reinterpret_cast<uint64_t*>(rcvr.bits - 8);
        slotCount = (raw << 8) >> 8;
    }
    int64_t i = (int64_t)idx.bits >> 3;  // 1-based index
    if (fmt == 2) {
        // Format 2: Indexable (Array, no fixed fields) — read Oop slot
        if (i < 1 || (uint64_t)i > slotCount) { _HOLE_CONTINUE(s); return; }
        Oop* slots = reinterpret_cast<Oop*>(rcvr.bits + 8);
        s->returnValue = slots[i - 1];
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    if (fmt >= 16 && fmt <= 23) {
        // Formats 16-23: Byte objects (ByteArray, ByteString, etc.)
        // byteSize = slotCount * 8 - (fmt - 16)
        uint64_t byteSize = slotCount * 8 - (fmt - 16);
        if (i < 1 || (uint64_t)i > byteSize) { _HOLE_CONTINUE(s); return; }
        uint8_t* bytes = reinterpret_cast<uint8_t*>(rcvr.bits + 8);
        uint8_t byte = bytes[i - 1];
        // Return as SmallInteger: (value << 3) | 1
        s->returnValue.bits = ((uint64_t)byte << 3) | 1;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    // Unsupported format — fall through to bytecodes
    _HOLE_CONTINUE(s); return;
}

// ----- Primitive 61: Object #at:put: -----
// Fast path for Array (format 2, no fixed fields, not immutable).
extern "C" void stencil_primAtPut(JITState* s) {
    Oop rcvr = s->receiver;
    Oop idx = s->tempBase[0];
    Oop val = s->tempBase[1];
    // Index must be SmallInteger
    if ((idx.bits & 7) != 1) { _HOLE_CONTINUE(s); return; }
    // Receiver must be an object pointer
    if ((rcvr.bits & 7) != 0 || rcvr.bits < 0x10000) { _HOLE_CONTINUE(s); return; }
    uint64_t header = *reinterpret_cast<uint64_t*>(rcvr.bits);
    uint64_t fmt = (header >> 24) & 0x1F;
    // Check not immutable (bit 23)
    if (header & (1ULL << 23)) { _HOLE_CONTINUE(s); return; }
    uint64_t slotCount = (header >> 56) & 0xFF;
    if (slotCount == 255) {
        uint64_t raw = *reinterpret_cast<uint64_t*>(rcvr.bits - 8);
        slotCount = (raw << 8) >> 8;
    }
    int64_t i = (int64_t)idx.bits >> 3;
    if (fmt == 2) {
        // Format 2: Indexable (Array) — store Oop slot
        if (i < 1 || (uint64_t)i > slotCount) { _HOLE_CONTINUE(s); return; }
        Oop* slots = reinterpret_cast<Oop*>(rcvr.bits + 8);
        slots[i - 1] = val;
        s->returnValue = val;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    if (fmt >= 16 && fmt <= 23) {
        // Formats 16-23: Byte objects — value must be SmallInteger 0-255
        if ((val.bits & 7) != 1) { _HOLE_CONTINUE(s); return; }
        int64_t byteVal = (int64_t)val.bits >> 3;
        if (byteVal < 0 || byteVal > 255) { _HOLE_CONTINUE(s); return; }
        uint64_t byteSize = slotCount * 8 - (fmt - 16);
        if (i < 1 || (uint64_t)i > byteSize) { _HOLE_CONTINUE(s); return; }
        uint8_t* bytes = reinterpret_cast<uint8_t*>(rcvr.bits + 8);
        bytes[i - 1] = (uint8_t)byteVal;
        s->returnValue = val;
        s->exitReason = EXIT_RETURN;
        _HOLE_RT_RETURN(s);
        return;
    }
    // Unsupported format — fall through to bytecodes
    _HOLE_CONTINUE(s); return;
}

// ----- Primitive 62: Object #size -----
// Fast path for format 2 (Indexable) and formats 16-23 (byte objects).
extern "C" void stencil_primSize(JITState* s) {
    Oop rcvr = s->receiver;
    if ((rcvr.bits & 7) != 0 || rcvr.bits < 0x10000) { _HOLE_CONTINUE(s); return; }
    uint64_t header = *reinterpret_cast<uint64_t*>(rcvr.bits);
    uint64_t fmt = (header >> 24) & 0x1F;
    uint64_t slotCount = (header >> 56) & 0xFF;
    if (slotCount == 255) {
        uint64_t raw = *reinterpret_cast<uint64_t*>(rcvr.bits - 8);
        slotCount = (raw << 8) >> 8;
    }
    uint64_t size;
    if (fmt == 2) {
        size = slotCount;
    } else if (fmt >= 16 && fmt <= 23) {
        size = slotCount * 8 - (fmt - 16);
    } else {
        _HOLE_CONTINUE(s); return;
    }
    s->returnValue.bits = (size << 3) | 1;  // SmallInteger encoding
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// =====================================================================
// SIMSTACK STENCILS — Register-cached TOS/NOS in x19/x20
// =====================================================================
//
// We use x19/x20 to cache the top 1-2 stack values, eliminating
// redundant memory loads/stores in straight-line code.
//
// State convention (suffix):
//   _E = Empty (x19/x20 are not live, all values on memory stack)
//   _1 = One   (x19 = TOS bits, memory stack has the rest)
//   _2 = Two   (x19 = TOS bits, x20 = NOS bits, memory stack has the rest)
//
// Register approach: We use inline asm to read/write x19/x20 WITHOUT
// declaring them as clobbered. This works because:
//   1. Stencils are leaf/tail-call functions — Clang doesn't use
//      callee-saved registers (x19-x28) since there's no return path
//   2. Without clobber, Clang doesn't generate save/restore sequences
//   3. Stencils chain via B (branch), preserving x19/x20 across the chain
//   4. extract_stencils.py verifies no unwanted x19/x20 usage post-compile
//
// SAFETY: -ffixed-x19/-x20 is NOT available on ARM64 LLVM. We rely on
// the compiler not using callee-saved regs in these small functions.
// The stencil extractor statically verifies this property.

// Guard: SimStack stencils are only generated for ARM64
#ifdef __aarch64__

// ----- FLUSH: write cached registers to memory stack -----

// State 1 → E: write x19 (TOS) to stack
extern "C" void stencil_flush1(JITState* s) {
    uint64_t tos;
    asm volatile("mov %0, x19" : "=r"(tos));
    Oop val; val.bits = tos;
    *(s->sp) = val;
    s->sp++;
    _HOLE_CONTINUE(s);
}

// State 2 → E: write x20 (NOS) then x19 (TOS) to stack
extern "C" void stencil_flush2(JITState* s) {
    uint64_t tos, nos;
    asm volatile("mov %0, x19" : "=r"(tos));
    asm volatile("mov %0, x20" : "=r"(nos));
    s->sp[0].bits = nos;
    s->sp[1].bits = tos;
    s->sp += 2;
    _HOLE_CONTINUE(s);
}

// ----- PUSH TEMP variants -----

// E → 1: load temp into x19
extern "C" void stencil_pushTemp_E(JITState* s) {
    int idx = OPERAND;
    uint64_t val = s->tempBase[idx].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

// 1 → 2: shift x19 to x20, load new x19
extern "C" void stencil_pushTemp_1(JITState* s) {
    int idx = OPERAND;
    asm volatile("mov x20, x19" :::);
    uint64_t val = s->tempBase[idx].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

// 2 → 2: spill x20 to memory, shift x19 to x20, load new x19
extern "C" void stencil_pushTemp_2(JITState* s) {
    int idx = OPERAND;
    uint64_t nos;
    asm volatile("mov %0, x20" : "=r"(nos));
    Oop spillVal; spillVal.bits = nos;
    *(s->sp) = spillVal;
    s->sp++;
    asm volatile("mov x20, x19" :::);
    uint64_t val = s->tempBase[idx].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

// ----- PUSH RECV VAR variants -----

extern "C" void stencil_pushRecvVar_E(JITState* s) {
    int idx = OPERAND;
    ObjectHeader* obj = asObjectPtr(s->receiver);
    uint64_t val;
    if (static_cast<uint64_t>(idx) < obj->slotCount()) {
        val = obj->slots()[idx].bits;
    } else {
        val = (*(Oop*)&_HOLE_NIL_OOP).bits;
    }
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushRecvVar_1(JITState* s) {
    int idx = OPERAND;
    ObjectHeader* obj = asObjectPtr(s->receiver);
    uint64_t val;
    if (static_cast<uint64_t>(idx) < obj->slotCount()) {
        val = obj->slots()[idx].bits;
    } else {
        val = (*(Oop*)&_HOLE_NIL_OOP).bits;
    }
    asm volatile("mov x20, x19" :::);
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushRecvVar_2(JITState* s) {
    int idx = OPERAND;
    ObjectHeader* obj = asObjectPtr(s->receiver);
    uint64_t val;
    if (static_cast<uint64_t>(idx) < obj->slotCount()) {
        val = obj->slots()[idx].bits;
    } else {
        val = (*(Oop*)&_HOLE_NIL_OOP).bits;
    }
    uint64_t nos;
    asm volatile("mov %0, x20" : "=r"(nos));
    Oop spillVal; spillVal.bits = nos;
    *(s->sp) = spillVal;
    s->sp++;
    asm volatile("mov x20, x19" :::);
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

// ----- PUSH LIT CONST variants -----

extern "C" void stencil_pushLitConst_E(JITState* s) {
    int idx = OPERAND;
    uint64_t val = s->literals[idx].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushLitConst_1(JITState* s) {
    int idx = OPERAND;
    asm volatile("mov x20, x19" :::);
    uint64_t val = s->literals[idx].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushLitConst_2(JITState* s) {
    int idx = OPERAND;
    uint64_t nos;
    asm volatile("mov %0, x20" : "=r"(nos));
    Oop spillVal; spillVal.bits = nos;
    *(s->sp) = spillVal;
    s->sp++;
    asm volatile("mov x20, x19" :::);
    uint64_t val = s->literals[idx].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

// ----- PUSH LIT VAR variants -----

extern "C" void stencil_pushLitVar_E(JITState* s) {
    int idx = OPERAND;
    Oop assoc = s->literals[idx];
    ObjectHeader* obj = asObjectPtr(assoc);
    uint64_t val = obj->slots()[1].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushLitVar_1(JITState* s) {
    int idx = OPERAND;
    Oop assoc = s->literals[idx];
    ObjectHeader* obj = asObjectPtr(assoc);
    asm volatile("mov x20, x19" :::);
    uint64_t val = obj->slots()[1].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushLitVar_2(JITState* s) {
    int idx = OPERAND;
    Oop assoc = s->literals[idx];
    ObjectHeader* obj = asObjectPtr(assoc);
    uint64_t nos;
    asm volatile("mov %0, x20" : "=r"(nos));
    Oop spillVal; spillVal.bits = nos;
    *(s->sp) = spillVal;
    s->sp++;
    asm volatile("mov x20, x19" :::);
    uint64_t val = obj->slots()[1].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

// ----- PUSH RECEIVER variants -----

extern "C" void stencil_pushReceiver_E(JITState* s) {
    uint64_t val = s->receiver.bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushReceiver_1(JITState* s) {
    asm volatile("mov x20, x19" :::);
    uint64_t val = s->receiver.bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushReceiver_2(JITState* s) {
    uint64_t nos;
    asm volatile("mov %0, x20" : "=r"(nos));
    Oop spillVal; spillVal.bits = nos;
    *(s->sp) = spillVal;
    s->sp++;
    asm volatile("mov x20, x19" :::);
    uint64_t val = s->receiver.bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

// ----- PUSH CONSTANT variants (true/false/nil) -----

extern "C" void stencil_pushTrue_E(JITState* s) {
    uint64_t val = (*(Oop*)&_HOLE_TRUE_OOP).bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}
extern "C" void stencil_pushTrue_1(JITState* s) {
    asm volatile("mov x20, x19" :::);
    uint64_t val = (*(Oop*)&_HOLE_TRUE_OOP).bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}
extern "C" void stencil_pushTrue_2(JITState* s) {
    uint64_t nos;
    asm volatile("mov %0, x20" : "=r"(nos));
    Oop spillVal; spillVal.bits = nos;
    *(s->sp) = spillVal; s->sp++;
    asm volatile("mov x20, x19" :::);
    uint64_t val = (*(Oop*)&_HOLE_TRUE_OOP).bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushFalse_E(JITState* s) {
    uint64_t val = (*(Oop*)&_HOLE_FALSE_OOP).bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}
extern "C" void stencil_pushFalse_1(JITState* s) {
    asm volatile("mov x20, x19" :::);
    uint64_t val = (*(Oop*)&_HOLE_FALSE_OOP).bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}
extern "C" void stencil_pushFalse_2(JITState* s) {
    uint64_t nos;
    asm volatile("mov %0, x20" : "=r"(nos));
    Oop spillVal; spillVal.bits = nos;
    *(s->sp) = spillVal; s->sp++;
    asm volatile("mov x20, x19" :::);
    uint64_t val = (*(Oop*)&_HOLE_FALSE_OOP).bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

extern "C" void stencil_pushNil_E(JITState* s) {
    uint64_t val = (*(Oop*)&_HOLE_NIL_OOP).bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}
extern "C" void stencil_pushNil_1(JITState* s) {
    asm volatile("mov x20, x19" :::);
    uint64_t val = (*(Oop*)&_HOLE_NIL_OOP).bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}
extern "C" void stencil_pushNil_2(JITState* s) {
    uint64_t nos;
    asm volatile("mov %0, x20" : "=r"(nos));
    Oop spillVal; spillVal.bits = nos;
    *(s->sp) = spillVal; s->sp++;
    asm volatile("mov x20, x19" :::);
    uint64_t val = (*(Oop*)&_HOLE_NIL_OOP).bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

// ----- POP variants -----

// 2 → 1: discard x19, x20 becomes new x19
extern "C" void stencil_pop_2(JITState* s) {
    asm volatile("mov x19, x20" :::);
    _HOLE_CONTINUE(s);
}

// 1 → E: discard x19
extern "C" void stencil_pop_1(JITState* s) {
    // TOS dropped, state now Empty
    _HOLE_CONTINUE(s);
}

// E → E: pop from memory stack
extern "C" void stencil_pop_E(JITState* s) {
    s->sp--;
    _HOLE_CONTINUE(s);
}

// ----- DUP variants -----

// E → 1: read TOS from memory into x19 (don't pop)
extern "C" void stencil_dup_E(JITState* s) {
    uint64_t val = s->sp[-1].bits;
    asm volatile("mov x19, %0" : : "r"(val));
    _HOLE_CONTINUE(s);
}

// 1 → 2: x20 = x19 (duplicate TOS)
extern "C" void stencil_dup_1(JITState* s) {
    asm volatile("mov x20, x19" :::);
    _HOLE_CONTINUE(s);
}

// 2 → 2: spill x20, x20 = x19 (duplicate TOS)
extern "C" void stencil_dup_2(JITState* s) {
    uint64_t nos;
    asm volatile("mov %0, x20" : "=r"(nos));
    Oop spillVal; spillVal.bits = nos;
    *(s->sp) = spillVal;
    s->sp++;
    asm volatile("mov x20, x19" :::);
    _HOLE_CONTINUE(s);
}

// ----- STORE TEMP variants (store without pop) -----

// 1: store x19 to temp, keep in register
extern "C" void stencil_storeTemp_1(JITState* s) {
    int idx = OPERAND;
    uint64_t tos;
    asm volatile("mov %0, x19" : "=r"(tos));
    s->tempBase[idx].bits = tos;
    _HOLE_CONTINUE(s);
}

// 2: store x19 to temp, keep both in registers
extern "C" void stencil_storeTemp_2(JITState* s) {
    int idx = OPERAND;
    uint64_t tos;
    asm volatile("mov %0, x19" : "=r"(tos));
    s->tempBase[idx].bits = tos;
    _HOLE_CONTINUE(s);
}

// ----- POP+STORE TEMP variants -----

// 2 → 1: store x19 to temp, drop TOS, x20 becomes x19
extern "C" void stencil_popStoreTemp_2(JITState* s) {
    int idx = OPERAND;
    uint64_t tos;
    asm volatile("mov %0, x19" : "=r"(tos));
    s->tempBase[idx].bits = tos;
    asm volatile("mov x19, x20" :::);
    _HOLE_CONTINUE(s);
}

// 1 → E: store x19 to temp, drop TOS
extern "C" void stencil_popStoreTemp_1(JITState* s) {
    int idx = OPERAND;
    uint64_t tos;
    asm volatile("mov %0, x19" : "=r"(tos));
    s->tempBase[idx].bits = tos;
    _HOLE_CONTINUE(s);
}

// ----- STORE RECV VAR variants -----

// 1: store x19 to receiver field, keep in register
extern "C" void stencil_storeRecvVar_1(JITState* s) {
    int idx = OPERAND;
    ObjectHeader* obj = asObjectPtr(s->receiver);
    uint64_t tos;
    asm volatile("mov %0, x19" : "=r"(tos));
    obj->slots()[idx].bits = tos;
    _HOLE_CONTINUE(s);
}

// ----- POP+STORE RECV VAR variants -----

// 2 → 1: store x19 to receiver field, x20 becomes x19
extern "C" void stencil_popStoreRecvVar_2(JITState* s) {
    int idx = OPERAND;
    ObjectHeader* obj = asObjectPtr(s->receiver);
    uint64_t tos;
    asm volatile("mov %0, x19" : "=r"(tos));
    obj->slots()[idx].bits = tos;
    asm volatile("mov x19, x20" :::);
    _HOLE_CONTINUE(s);
}

// 1 → E: store x19 to receiver field, drop TOS
extern "C" void stencil_popStoreRecvVar_1(JITState* s) {
    int idx = OPERAND;
    ObjectHeader* obj = asObjectPtr(s->receiver);
    uint64_t tos;
    asm volatile("mov %0, x19" : "=r"(tos));
    obj->slots()[idx].bits = tos;
    _HOLE_CONTINUE(s);
}

// ----- BINARY ARITHMETIC variants (state 2 → 1) -----
// These consume NOS (receiver) and TOS (arg), produce result in TOS.

extern "C" void stencil_addSmallInt_2(JITState* s) {
    uint64_t a_bits, b_bits;
    asm volatile("mov %0, x20" : "=r"(a_bits));
    asm volatile("mov %0, x19" : "=r"(b_bits));
    bool ok = false;
    uint64_t r = 0;
    if ((a_bits & TagMask3) == SmallIntegerTag && (b_bits & TagMask3) == SmallIntegerTag) {
        int64_t va = static_cast<int64_t>(a_bits) >> 3;
        int64_t vb = static_cast<int64_t>(b_bits) >> 3;
        int64_t result = va + vb;
        if (result >= -0x1FFFFFFFFFFFFFFFLL && result <= 0x1FFFFFFFFFFFFFFFLL) {
            r = (static_cast<uint64_t>(result << 3) | SmallIntegerTag);
            ok = true;
        }
    }
    if (ok) {
        asm volatile("mov x19, %0" : : "r"(r));
        _HOLE_CONTINUE(s);
    } else {
        Oop an, bt;
        an.bits = a_bits; bt.bits = b_bits;
        s->sp[0] = an; s->sp[1] = bt; s->sp += 2;
        s->ip = s->ip + OPERAND;
        _HOLE_RT_ARITH_OVERFLOW(s);
    }
}

extern "C" void stencil_subSmallInt_2(JITState* s) {
    uint64_t a_bits, b_bits;
    asm volatile("mov %0, x20" : "=r"(a_bits));
    asm volatile("mov %0, x19" : "=r"(b_bits));
    if ((a_bits & TagMask3) == SmallIntegerTag && (b_bits & TagMask3) == SmallIntegerTag) {
        int64_t va = static_cast<int64_t>(a_bits) >> 3;
        int64_t vb = static_cast<int64_t>(b_bits) >> 3;
        int64_t result = va - vb;
        if (__builtin_expect(result >= -0x1FFFFFFFFFFFFFFFLL && result <= 0x1FFFFFFFFFFFFFFFLL, 1)) {
            uint64_t r = (static_cast<uint64_t>(result << 3) | SmallIntegerTag);
            asm volatile("mov x19, %0" : : "r"(r));
            _HOLE_CONTINUE(s);
            return;
        }
    }
    Oop an, bt;
    an.bits = a_bits; bt.bits = b_bits;
    s->sp[0] = an; s->sp[1] = bt; s->sp += 2;
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

extern "C" void stencil_mulSmallInt_2(JITState* s) {
    uint64_t a_bits, b_bits;
    asm volatile("mov %0, x20" : "=r"(a_bits));
    asm volatile("mov %0, x19" : "=r"(b_bits));
    bool ok = false;
    uint64_t r = 0;
    if ((a_bits & TagMask3) == SmallIntegerTag && (b_bits & TagMask3) == SmallIntegerTag) {
        int64_t va = static_cast<int64_t>(a_bits) >> 3;
        int64_t vb = static_cast<int64_t>(b_bits) >> 3;
        __int128 wide = (__int128)va * vb;
        int64_t result = (int64_t)wide;
        if (wide == result && result >= -0x1FFFFFFFFFFFFFFFLL && result <= 0x1FFFFFFFFFFFFFFFLL) {
            r = (static_cast<uint64_t>(result << 3) | SmallIntegerTag);
            ok = true;
        }
    }
    if (ok) {
        asm volatile("mov x19, %0" : : "r"(r));
        _HOLE_CONTINUE(s);
    } else {
        Oop an, bt;
        an.bits = a_bits; bt.bits = b_bits;
        s->sp[0] = an; s->sp[1] = bt; s->sp += 2;
        s->ip = s->ip + OPERAND;
        _HOLE_RT_ARITH_OVERFLOW(s);
    }
}

// ----- COMPARISON variants (state 2 → 1) -----

// Macro for comparison _2 variants: read x19/x20, compare, write result to x19
#define COMPARISON_2_STENCIL(name, op) \
extern "C" void name(JITState* s) { \
    uint64_t a_bits, b_bits; \
    asm volatile("mov %0, x20" : "=r"(a_bits)); \
    asm volatile("mov %0, x19" : "=r"(b_bits)); \
    if ((a_bits & TagMask3) == SmallIntegerTag && (b_bits & TagMask3) == SmallIntegerTag) { \
        int64_t va = static_cast<int64_t>(a_bits) >> 3; \
        int64_t vb = static_cast<int64_t>(b_bits) >> 3; \
        uint64_t r = (va op vb) ? (*(Oop*)&_HOLE_TRUE_OOP).bits : (*(Oop*)&_HOLE_FALSE_OOP).bits; \
        asm volatile("mov x19, %0" : : "r"(r)); \
        _HOLE_CONTINUE(s); \
        return; \
    } \
    Oop an, bt; \
    an.bits = a_bits; bt.bits = b_bits; \
    s->sp[0] = an; s->sp[1] = bt; s->sp += 2; \
    s->ip = s->ip + OPERAND; \
    _HOLE_RT_ARITH_OVERFLOW(s); \
}

COMPARISON_2_STENCIL(stencil_lessThanSmallInt_2, <)
COMPARISON_2_STENCIL(stencil_greaterThanSmallInt_2, >)
COMPARISON_2_STENCIL(stencil_lessEqualSmallInt_2, <=)
COMPARISON_2_STENCIL(stencil_greaterEqualSmallInt_2, >=)

extern "C" void stencil_equalSmallInt_2(JITState* s) {
    uint64_t a_bits, b_bits;
    asm volatile("mov %0, x20" : "=r"(a_bits));
    asm volatile("mov %0, x19" : "=r"(b_bits));
    if ((a_bits & TagMask3) == SmallIntegerTag && (b_bits & TagMask3) == SmallIntegerTag) {
        uint64_t r = (a_bits == b_bits) ? (*(Oop*)&_HOLE_TRUE_OOP).bits : (*(Oop*)&_HOLE_FALSE_OOP).bits;
        asm volatile("mov x19, %0" : : "r"(r));
        _HOLE_CONTINUE(s);
        return;
    }
    Oop an, bt;
    an.bits = a_bits;
    bt.bits = b_bits;
    s->sp[0] = an;
    s->sp[1] = bt;
    s->sp += 2;
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

extern "C" void stencil_notEqualSmallInt_2(JITState* s) {
    uint64_t a_bits, b_bits;
    asm volatile("mov %0, x20" : "=r"(a_bits));
    asm volatile("mov %0, x19" : "=r"(b_bits));
    if ((a_bits & TagMask3) == SmallIntegerTag && (b_bits & TagMask3) == SmallIntegerTag) {
        uint64_t r = (a_bits != b_bits) ? (*(Oop*)&_HOLE_TRUE_OOP).bits : (*(Oop*)&_HOLE_FALSE_OOP).bits;
        asm volatile("mov x19, %0" : : "r"(r));
        _HOLE_CONTINUE(s);
        return;
    }
    Oop an, bt;
    an.bits = a_bits; bt.bits = b_bits;
    s->sp[0] = an; s->sp[1] = bt; s->sp += 2;
    s->ip = s->ip + OPERAND;
    _HOLE_RT_ARITH_OVERFLOW(s);
}

#undef COMPARISON_2_STENCIL

// ----- CONDITIONAL JUMP variants (state 1 → E) -----
// These consume TOS (boolean) and branch.

extern "C" void stencil_jumpTrue_1(JITState* s) {
    uint64_t tos_bits;
    asm volatile("mov %0, x19" : "=r"(tos_bits));
    if (tos_bits == (*(Oop*)&_HOLE_TRUE_OOP).bits) {
        _HOLE_BRANCH_TARGET(s);
        return;
    }
    if (tos_bits == (*(Oop*)&_HOLE_FALSE_OOP).bits) {
        _HOLE_CONTINUE(s);
        return;
    }
    // Non-boolean: spill and deopt
    Oop val; val.bits = tos_bits;
    *(s->sp) = val;
    s->sp++;
    s->exitReason = EXIT_SEND;
    _HOLE_RT_SEND(s);
}

extern "C" void stencil_jumpFalse_1(JITState* s) {
    uint64_t tos_bits;
    asm volatile("mov %0, x19" : "=r"(tos_bits));
    if (tos_bits == (*(Oop*)&_HOLE_FALSE_OOP).bits) {
        _HOLE_BRANCH_TARGET(s);
        return;
    }
    if (tos_bits == (*(Oop*)&_HOLE_TRUE_OOP).bits) {
        _HOLE_CONTINUE(s);
        return;
    }
    // Non-boolean: spill and deopt
    Oop val; val.bits = tos_bits;
    *(s->sp) = val;
    s->sp++;
    s->exitReason = EXIT_SEND;
    _HOLE_RT_SEND(s);
}

// ----- RETURN variants -----

// 1: return x19 (TOS in register)
extern "C" void stencil_returnTop_1(JITState* s) {
    uint64_t tos;
    asm volatile("mov %0, x19" : "=r"(tos));
    Oop retVal;
    retVal.bits = tos;
    J2J_INLINE_RETURN(s, retVal);
    s->returnValue = retVal;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// E: return TOS from memory (same as base stencil)
extern "C" void stencil_returnTop_E(JITState* s) {
    s->sp--;
    Oop retVal = *(s->sp);
    J2J_INLINE_RETURN(s, retVal);
    s->returnValue = retVal;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

// Return receiver with state 1 (discard x19, return self)
extern "C" void stencil_returnReceiver_1(JITState* s) {
    Oop retVal = s->receiver;
    J2J_INLINE_RETURN(s, retVal);
    s->returnValue = retVal;
    s->exitReason = EXIT_RETURN;
    _HOLE_RT_RETURN(s);
}

#endif // __aarch64__
