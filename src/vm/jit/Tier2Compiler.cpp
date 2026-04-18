/*
 * Tier2Compiler.cpp - asmjit-based optimizing JIT (MVP)
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * CURRENT SCOPE (MVP, 2026-04-17):
 *   Compiles only methods whose body is a single bytecode:
 *     0x58 returnReceiver
 *
 *   All other method shapes return nullptr; runtime falls through to
 *   Tier 1. This exists to prove the asmjit pipeline end-to-end: we
 *   can build MIR-generated-shaped machine code via asmjit, register
 *   it with the VM, and have it called from tryExecute.
 *
 *   Each subsequent commit will add one bytecode / pattern. Reach
 *   parity with the former MIR T2 feature set before benchmarking.
 *
 * ARM64 only for now; x64 will get its own file once the arm64 path
 * is validated.
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
#include <map>
#include <set>

#if PHARO_JIT_ENABLED

#include <asmjit/a64.h>
#include <asmjit/core/jitruntime.h>

namespace pharo {
namespace jit {

namespace {

// JITState field offsets (mirror JITState.hpp; keep these in sync).
constexpr int OFF_SP           = 0;
constexpr int OFF_RECEIVER     = 8;
constexpr int OFF_LITERALS     = 16;
constexpr int OFF_TEMPBASE     = 24;
constexpr int OFF_IP           = 48;
constexpr int OFF_EXIT         = 76;
constexpr int OFF_RETVAL       = 80;
constexpr int OFF_CACHEDTARGET = 88;
constexpr int OFF_ICDATAPTR    = 96;
constexpr int OFF_SENDARGCOUNT = 104;
constexpr int OFF_TRUEOOP      = 128;
constexpr int OFF_FALSEOOP     = 136;

// Oop object layout: ObjectHeader is 8 bytes, slots follow at offset 8.
// So slot[N] is at byte offset 8 + N*8 from the object pointer.
constexpr int OBJ_SLOT_0    = 8;
constexpr int ASSOC_VALUE   = 16;  // Association.value = slot[1]

// ExitReason values (mirror JITState.hpp).
constexpr int EXIT_RETURN         = 1;
constexpr int EXIT_SEND           = 2;
constexpr int EXIT_SEND_CACHED    = 7;  // ExitSendCached

// ObjectHeader classIndex layout (mirror ObjectHeader.hpp).
constexpr uint64_t CLASS_INDEX_MASK = 0x3FFFFFULL;  // bits 0-21

// IC data layout (mirror JITMethod.hpp: 6 entries × 24 bytes + 8 bytes
// selectorBits = 152 bytes, 19 × uint64_t).
constexpr int IC_ENTRIES = 6;
constexpr int IC_SLOTS   = IC_ENTRIES * 3 + 1;
constexpr int IC_KEY_OFF(int i)    { return i * 3 * 8;       }
constexpr int IC_METHOD_OFF(int i) { return i * 3 * 8 + 8;   }
constexpr int IC_EXTRA_OFF(int i)  { return i * 3 * 8 + 16;  }

} // namespace

Tier2Compiler::Tier2Compiler(CodeZone& zone, MethodMap& methodMap,
                             ObjectMemory& memory, Interpreter& interp)
    : zone_(zone), methodMap_(methodMap), memory_(memory), interp_(interp) {}

Tier2Compiler::~Tier2Compiler() = default;

bool Tier2Compiler::initialize() {
    runtime_ = std::make_unique<asmjit::JitRuntime>();
    return true;
}

namespace {
size_t g_compiled = 0;
size_t g_bailed   = 0;
} // namespace

void Tier2Compiler::dumpBailStats() {
    fprintf(stderr, "  T2 (asmjit): compiled=%zu bailed=%zu\n",
            g_compiled, g_bailed);
}

namespace {
size_t g_mbcCompiled = 0;
size_t g_mbcBailed   = 0;

// Is this bytecode fully compilable by multi-bc?  True means we emit
// inline asmjit code for it.  False means we'd have to bail.
//
// Short jumps (0xB0-0xC7: unconditional, if-true, if-false) are gated
// behind PHARO_T2_MBC_JUMPS=1 until the feature is proven stable.
bool isMBCSupported(uint8_t op) {
    // Pushes
    if (op <= 0x4B) return true;                 // 0x00-0x4B: pushRecvVar/LitVar/LitConst/Temp
    if (op == SistaV1::PushReceiver) return true;
    if (op >= 0x4D && op <= 0x51) return true;   // PushTrue/False/Nil/Zero/One
    if (op == SistaV1::Dup) return true;
    // Returns
    if (op >= SistaV1::ReturnReceiver && op <= SistaV1::ReturnTop) return true;
    // Arith (0x60-0x6F)
    if (op >= 0x60 && op <= 0x6F) return true;
    // Stores
    if (op >= SistaV1::PopStoreRecvBase && op <= SistaV1::PopStoreRecvLast) return true;
    if (op >= SistaV1::PopStoreTempBase && op <= SistaV1::PopStoreTempLast) return true;
    if (op == SistaV1::Pop) return true;
    // Short jumps — unconditional (0xB0-0xB7), if-true (0xB8-0xBF),
    // if-false (0xC0-0xC7).  All forward; gated.
    if (SistaV1::isAnyShortJump(op) && getenv("PHARO_T2_MBC_JUMPS") != nullptr) return true;
    return false;
}

// Is this a 1-byte send?  Special selectors (0x70-0x7F) and literal
// sends (0x80-0xAF).  If we hit one of these during multi-bc walk we
// emit a "flush state and bail to interpreter at this byte" path and
// stop compilation (the interpreter runs the rest of the method).
//
// Excludes arith (0x60-0x6F) because those are handled inline.
// Excludes 2-byte ExtSend/ExtSuperSend (0xEA/EB) — too complex for
// this first cut; method bails at compile time if it has those.
bool isMBCBailableSend(uint8_t op) {
    return (op >= 0x70 && op <= 0xAF);
}

// How many args does this send take (for sendArgCount on bail)?
int mbcSendArgCount(uint8_t op) {
    if (op >= 0x70 && op <= 0x7F) {
        // Special selector N (where N = op-0x70+16): arg count varies.
        // For the bail path, matching T1's behaviour is sufficient —
        // the interpreter re-dispatches the bytecode.  Use 0 as a
        // safe default (the interpreter ignores state.sendArgCount
        // for SpecialSelector bytecodes; it reads arg count from the
        // selector itself).  Conservative choice: return the wrong
        // count might corrupt stack handling.  Skip SpecialSelectors
        // for now — return -1 to signal "don't support".
        return -1;
    }
    if (op >= 0x80 && op <= 0x8F) return 0;  // Send0
    if (op >= 0x90 && op <= 0x9F) return 1;  // Send1
    if (op >= 0xA0 && op <= 0xAF) return 2;  // Send2
    return -1;
}

// Returns the byte length of a bytecode (all supported + bailable
// sends are 1 byte in this first cut).
int mbcOpLen(uint8_t op) {
    (void)op;
    return 1;
}
} // namespace

void Tier2Compiler::flushAllICs() {
    // Zero every T2 IC entry — next send re-populates via the
    // interpreter's pendingICPatch_ mechanism.  Called from
    // JITRuntime::recoverAfterGC because methodBits in IC entries
    // are raw Oops that can become stale after compaction.
    for (auto& buf : icBuffers_) {
        std::memset(buf.get(), 0, IC_SLOTS * sizeof(uint64_t));
    }
}

void* Tier2Compiler::compile(Oop compiledMethod, JITMethod* oldVersion) {
    (void)oldVersion;
    if (!runtime_) {
        if (!initialize()) {
            compilationsFailed_++;
            return nullptr;
        }
    }

    // --- Walk method header / bytecodes ---
    //
    // CompiledMethod header format (after SmallInteger decoding):
    //   bits 0-14:  numLiterals (15 bits)
    //   bit 15:     requiresCounters / needsLargeFrame
    //   bit 16:     hasPrimitive
    //   bit 17:     isOptimized / needsLargeFrame
    //   bits 18-23: numTemps
    //   bits 24-27: numArgs
    //   bits 28-29: accessModifier
    // The primitive *number* lives in the bytecode stream as the
    // CallPrimitive (0xF8) bytecode, not in the header.
    ObjectHeader* methodObj = reinterpret_cast<ObjectHeader*>(compiledMethod.rawBits());
    Oop headerOop = methodObj->slotAt(0);
    if (!headerOop.isSmallInteger()) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }
    int64_t headerBits = headerOop.asSmallInteger();
    int  numLiterals  = (int)(headerBits & 0x7FFF);
    bool hasPrimitive = (headerBits >> 16) & 1;

    // Methods with primitives stay on T1 (or call out to C).
    if (hasPrimitive) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    uint8_t* bytes = methodObj->bytes();
    size_t bcStart = (1 + numLiterals) * 8;
    size_t totalBytes = methodObj->slotCount() * 8;
    // Match JITCompiler.cpp: CompiledMethod objects may have trailing
    // padding bytes encoded in the format field (fmt-24 when fmt >= 24).
    uint8_t fmt = static_cast<uint8_t>(methodObj->format());
    int unusedBytes = (fmt >= 24) ? (fmt - 24) : 0;
    if (bcStart + (size_t)unusedBytes >= totalBytes) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }
    size_t bodyLen = totalBytes - bcStart - (size_t)unusedBytes;

    // --- Try multi-bytecode compilation first ---
    //
    // For methods containing no sends, no jumps, no blocks — just
    // arithmetic, pushes, stores, and a return — walk the bytecode
    // sequence and emit asmjit code per op.  Returns nullptr on any
    // unsupported bytecode; we then fall through to template matching.
    if (void* mbcFunc = tryCompileMultiBC(compiledMethod, bytes, bcStart, bodyLen)) {
        methodsCompiled_++;
        g_compiled++;
        return mbcFunc;
    }

    // --- Recognise a small set of leading patterns ---
    //
    // We match on the FIRST few bytes up to (and including) a return
    // bytecode.  Anything after the return is unreachable (the method is
    // done executing) so we don't care.
    //
    // Patterns currently handled:
    //   Return-only (1 byte):
    //     0x58   ReturnReceiver
    //     0x59   ReturnTrue
    //     0x5A   ReturnFalse
    //     0x5B   ReturnNil
    //   Push-then-ReturnTop (2 bytes):
    //     0x4C 0x5C           ^ self
    //     0x4D 0x5C           ^ true
    //     0x4E 0x5C           ^ false
    //     0x4F 0x5C           ^ nil
    //     0x00-0x0F 0x5C      ^ instVar[N]                  (getter)
    //   Setter (3 bytes):
    //     0x40 0xC8-0xCF 0x58 temp0 -> recvVar[N]; ^ self   (setter of arg 0)
    //
    // Everything else bails to T1.
    uint8_t b0 = bytes[bcStart];
    uint8_t b1 = (bodyLen >= 2) ? bytes[bcStart + 1] : 0;
    uint8_t b2 = (bodyLen >= 3) ? bytes[bcStart + 2] : 0;

    enum class ReturnKind {
        Receiver, True, False, NilImm, RecvVar, SetterRecvVar, ImmediateOop,
        InitRecvVar,        // push constant, pop into recvVar, return self
        LitVar,             // ^ literal-var N — push association's value from literals[N]
        TempReturn,         // ^ tempN — return argument or local temp
        SendExit,           // push N values + exit with ExitSend (interpreter finishes)
        ZeroArgSendInlineIC,// ^ <push> foo — 1 push + inline IC probe + Cached/Send
        OneArgSendInlineIC, // ^ <push0> foo: <push1> — 2 pushes + inline IC probe
        IntArithAddReturn,  // ^ <push0> + <push1> — SmallInt fast path (+ only)
        IntArithSubReturn,  // ^ <push0> - <push1> — SmallInt fast path (- only)
        IntArithMulReturn,  // ^ <push0> * <push1> — SmallInt multiply (with 128-bit overflow check)
        IntAccumRecvVar,    // ivar[M] := ivar[M] +/- <push1>; ^ self  (SmallInt)
        IntCmpReturn,       // ^ <push0> cmp <push1> — SmallInt compare (<,>,<=,>=,=,~=)
        IntBitOpReturn      // ^ <push0> bitAnd:/bitOr: <push1> — tagged bit op
    };
    int arithOp = 0;           // 0x60 (+) ... 0x6F (bitOr:) for IntArith*/IntCmp
    // SendExit push source (what to push before bailing to interpreter).
    enum class PushSrc { None, Receiver, RecvVar, LitVar, Temp, ImmOop };

    struct Push {
        PushSrc  src  = PushSrc::None;
        int      idx  = 0;        // recvVar / litVar / temp index
        uint64_t bits = 0;        // ImmOop raw bits (literals pre-fetched at compile time)
    };

    ReturnKind kind;
    uint64_t immBits = 0;      // ImmediateOop / NilImm / InitRecvVar value
    int recvVarIndex = 0;      // RecvVar / SetterRecvVar / InitRecvVar
    int litIndex     = 0;      // LitVar
    int sendIPOff    = 0;      // SendExit: byte offset of the send/arith bytecode
    Push pushes[2];            // SendExit: up to 2 values to push
    int numPushes    = 0;

    // Helper: decode a single-byte "push" bytecode into a Push entry.
    // Returns true on success, false if `op` isn't a supported push.
    auto decodePush = [&](uint8_t op, Push& p) -> bool {
        if (op == SistaV1::PushReceiver)  { p.src = PushSrc::Receiver; return true; }
        if (op == SistaV1::PushTrue)      { p.src = PushSrc::ImmOop;
            p.bits = memory_.specialObject(SpecialObjectIndex::TrueObject).rawBits(); return true; }
        if (op == SistaV1::PushFalse)     { p.src = PushSrc::ImmOop;
            p.bits = memory_.specialObject(SpecialObjectIndex::FalseObject).rawBits(); return true; }
        if (op == SistaV1::PushNil)       { p.src = PushSrc::ImmOop;
            p.bits = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits(); return true; }
        if (op == SistaV1::PushZero)      { p.src = PushSrc::ImmOop;
            p.bits = Oop::fromSmallInteger(0).rawBits(); return true; }
        if (op == SistaV1::PushOne)       { p.src = PushSrc::ImmOop;
            p.bits = Oop::fromSmallInteger(1).rawBits(); return true; }
        if (SistaV1::isPushRecvVar(op))   { p.src = PushSrc::RecvVar;
            p.idx = op - SistaV1::PushRecvVarBase; return true; }
        if (SistaV1::isPushLitVar(op))    { p.src = PushSrc::LitVar;
            p.idx = op - SistaV1::PushLitVarBase; return true; }
        if (SistaV1::isPushLitConst(op))  { p.src = PushSrc::ImmOop;
            p.bits = methodObj->slotAt(1 + (op - SistaV1::PushLitConstBase)).rawBits();
            return true; }
        if (SistaV1::isPushTemp(op))      { p.src = PushSrc::Temp;
            p.idx = op - SistaV1::PushTempBase; return true; }
        return false;
    };

    if (b0 == SistaV1::ReturnReceiver) {
        kind = ReturnKind::Receiver;
    } else if (b0 == SistaV1::ReturnTrue) {
        kind = ReturnKind::True;
    } else if (b0 == SistaV1::ReturnFalse) {
        kind = ReturnKind::False;
    } else if (b0 == SistaV1::ReturnNil) {
        kind = ReturnKind::NilImm;
        immBits = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && b0 == SistaV1::PushReceiver) {
        kind = ReturnKind::Receiver;
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && b0 == SistaV1::PushTrue) {
        kind = ReturnKind::True;
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && b0 == SistaV1::PushFalse) {
        kind = ReturnKind::False;
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && b0 == SistaV1::PushNil) {
        kind = ReturnKind::NilImm;
        immBits = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushRecvVar(b0)) {
        kind = ReturnKind::RecvVar;
        recvVarIndex = b0 - SistaV1::PushRecvVarBase;
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushTemp(b0)) {
        // ^ tempN — return an argument/local from tempBase[N].
        kind = ReturnKind::TempReturn;
        recvVarIndex = b0 - SistaV1::PushTempBase;
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && b0 == SistaV1::PushZero) {
        // ^ 0 -> SmallInteger zero with tag 001 in low bits
        kind = ReturnKind::ImmediateOop;
        immBits = Oop::fromSmallInteger(0).rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && b0 == SistaV1::PushOne) {
        // ^ 1
        kind = ReturnKind::ImmediateOop;
        immBits = Oop::fromSmallInteger(1).rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushLitConst(b0)) {
        // ^ literal-const N  — fetch from the method's literal frame at
        // compile time and bake in as an immediate.
        int idx = b0 - SistaV1::PushLitConstBase;
        // Literal frame starts at slot 1 (slot 0 is the header).
        Oop lit = methodObj->slotAt(1 + idx);
        kind = ReturnKind::ImmediateOop;
        immBits = lit.rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushLitVar(b0)) {
        // ^ literal-var N — literals[N] is an Association; return
        // association.value.  Value can change at runtime (global
        // reassignment) so load it dynamically via state.literals.
        kind = ReturnKind::LitVar;
        litIndex = b0 - SistaV1::PushLitVarBase;
    } else if (bodyLen >= 3 && b0 == SistaV1::PushTempBase
                            && SistaV1::isPopStoreRecv(b1)
                            && b2 == SistaV1::ReturnReceiver) {
        kind = ReturnKind::SetterRecvVar;
        recvVarIndex = b1 - SistaV1::PopStoreRecvBase;
    } else if (getenv("PHARO_T2_ZEROARG_IC")
                            && bodyLen >= 3 && SistaV1::isSend0(b1)
                            && b2 == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])) {
        // 0-arg inline IC — gated.  Tried narrowing to
        // `b0 == PushReceiver` only; still regresses (4/10 fast
        // vs 5/8 default) even though correctness is preserved
        // (0 DNU across 5 runs).  The tier interaction (§1.3)
        // still dominates.  Left gated.
        kind = ReturnKind::ZeroArgSendInlineIC;
        numPushes = 1;
        sendIPOff = 1;
    } else if (bodyLen >= 5 && SistaV1::isPushRecvVar(b0)
                            && (b2 == 0x60 || b2 == 0x61)
                            && SistaV1::isPopStoreRecv(bytes[bcStart + 3])
                            && bytes[bcStart + 4] == SistaV1::ReturnReceiver
                            && decodePush(b1, pushes[1])
                            && (b0 - SistaV1::PushRecvVarBase)
                                 == (bytes[bcStart + 3] - SistaV1::PopStoreRecvBase)) {
        // ivar[M] := ivar[M] +/- <push1>; ^ self
        // PushRecvVar M; <push1>; Arith(+/-); PopStoreRecvVar M; ReturnReceiver
        // (5 bytes).  The push/store indices must match — same instVar.
        kind = ReturnKind::IntAccumRecvVar;
        recvVarIndex = b0 - SistaV1::PushRecvVarBase;
        arithOp = b2;
        // numPushes not used; emission reads pushes[1] directly.
    } else if (bodyLen >= 4 && (b2 == 0x60 || b2 == 0x61)
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <push0> +/- <push1> — inline SmallInt fast path.
        // Tag-check both operands; bail to interpreter on tag mismatch
        // or overflow.
        kind = (b2 == 0x60) ? ReturnKind::IntArithAddReturn
                            : ReturnKind::IntArithSubReturn;
        numPushes = 2;
        sendIPOff = 2;
        arithOp = b2;
    } else if (bodyLen >= 4 && b2 == 0x68
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <push0> * <push1> — SmallInt multiply fast path.
        // Overflow handled via smulh/mul consistency check.
        kind = ReturnKind::IntArithMulReturn;
        numPushes = 2;
        sendIPOff = 2;
        arithOp = b2;
    } else if (bodyLen >= 4 && (b2 == 0x6E || b2 == 0x6F)
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <push0> bitAnd:/bitOr: <push1> — tagged bit ops
        // Tag=1 for both operands, and the tag bit is preserved by
        // bitwise AND (1&1=1) and OR (1|1=1) — so we can operate on
        // tagged Oops directly without untag/retag.
        kind = ReturnKind::IntBitOpReturn;
        numPushes = 2;
        sendIPOff = 2;
        arithOp = b2;
    } else if (bodyLen >= 4 && b2 >= 0x62 && b2 <= 0x67
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <push0> cmp <push1> — SmallInt comparison fast path.
        // Arith bytecodes 0x62-0x67: < > <= >= = ~=
        // For both SmallInt, the comparison on tagged values matches
        // the comparison on untagged values (signed order is preserved
        // by multiply-and-add-tag since tag bit is the same for both
        // operands).  Result is trueOop / falseOop via conditional
        // select.  Bail to interpreter on tag mismatch.
        kind = ReturnKind::IntCmpReturn;
        numPushes = 2;
        sendIPOff = 2;
        arithOp = b2;
    } else if (bodyLen >= 4 && SistaV1::isSend1(b2)
                && bytes[bcStart + 3] == SistaV1::ReturnTop
                && decodePush(b0, pushes[0])
                && decodePush(b1, pushes[1])) {
        // ^ <push0> foo: <push1> — 2 pushes + inline IC probe.
        //
        // Previously gated due to a flaky "#isNumber/#x not understood"
        // DNU that was root-caused to the IC-counter bump sequence
        // emitting extra instructions that corrupted register
        // allocation / code layout.  Removing the counters (hit/miss
        // no longer logged from emitted code) fixed it — 0 DNU
        // matches across 12 runs.
        kind = ReturnKind::OneArgSendInlineIC;
        numPushes = 2;
        sendIPOff = 2;
    } else if (false) {
        // 1-arg send pattern (Push + Push + Send1 + ReturnTop) is
        // correct by construction but the push+exit+resume-to-C
        // round-trip is MUCH slower than T1 staying in native code.
        // Measured 1.8× regression on array-fill benchmark.  Gated
        // off until we can emit the send *inline* (IC check + direct
        // J2J call on hit) instead of bailing to the interpreter.
        // See task #31.
    } else if (bodyLen >= 3 && SistaV1::isPopStoreRecv(b1)
                            && b2 == SistaV1::ReturnReceiver
                            && (b0 == SistaV1::PushZero
                             || b0 == SistaV1::PushOne
                             || b0 == SistaV1::PushNil
                             || b0 == SistaV1::PushTrue
                             || b0 == SistaV1::PushFalse)) {
        // init-style: ivar := <constant>; ^ self
        kind = ReturnKind::InitRecvVar;
        recvVarIndex = b1 - SistaV1::PopStoreRecvBase;
        if (b0 == SistaV1::PushZero)       immBits = Oop::fromSmallInteger(0).rawBits();
        else if (b0 == SistaV1::PushOne)   immBits = Oop::fromSmallInteger(1).rawBits();
        else if (b0 == SistaV1::PushNil)   immBits = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits();
        else if (b0 == SistaV1::PushTrue)  immBits = memory_.specialObject(SpecialObjectIndex::TrueObject).rawBits();
        else /* PushFalse */                immBits = memory_.specialObject(SpecialObjectIndex::FalseObject).rawBits();
    } else {
        // Pattern not recognised — fall through to T1.  PHARO_T2_VERBOSE=1
        // surfaces the leading bytes so we can see which shapes are hot
        // and decide what to add next.
        if (getenv("PHARO_T2_VERBOSE")) {
            fprintf(stderr, "[T2 bail] len=%zu bytes=%02x %02x %02x %02x\n",
                    bodyLen, b0, b1, b2,
                    bodyLen >= 4 ? bytes[bcStart+3] : 0);
        }
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    // --- Emit with asmjit ---
    using namespace asmjit;
    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());

    a64::Compiler cc(&code);
    FuncNode* funcNode = cc.add_func(FuncSignature::build<void, void*>());

    // arg0: JITState*
    a64::Gp statePtr = cc.new_gp64("state");
    funcNode->set_arg(0, statePtr);

    // Load or materialize the Oop to return.
    a64::Gp retOop = cc.new_gp64("retOop");
    switch (kind) {
    case ReturnKind::Receiver:
        cc.ldr(retOop, a64::ptr(statePtr, OFF_RECEIVER));
        break;
    case ReturnKind::True:
        cc.ldr(retOop, a64::ptr(statePtr, OFF_TRUEOOP));
        break;
    case ReturnKind::False:
        cc.ldr(retOop, a64::ptr(statePtr, OFF_FALSEOOP));
        break;
    case ReturnKind::NilImm:
    case ReturnKind::ImmediateOop:
        cc.mov(retOop, asmjit::Imm(immBits));
        break;
    case ReturnKind::RecvVar: {
        // Getter: return state.receiver->slotAt(recvVarIndex).
        // receiver is an object Oop, so its bits == pointer (low 3 bits 0).
        // sizeof(ObjectHeader) == 8, so slot N is at byte offset 8 + N*8.
        a64::Gp recvPtr = cc.new_gp64("recvPtr");
        cc.ldr(recvPtr, a64::ptr(statePtr, OFF_RECEIVER));
        int slotOff = 8 + recvVarIndex * 8;
        cc.ldr(retOop, a64::ptr(recvPtr, slotOff));
        break;
    }
    case ReturnKind::SetterRecvVar: {
        // Setter: receiver.instVar[recvVarIndex] := temp0; return receiver.
        // tempBase points to temp/arg 0, which is our source value.
        a64::Gp recvPtr = cc.new_gp64("recvPtr");
        a64::Gp tempPtr = cc.new_gp64("tempPtr");
        a64::Gp argVal  = cc.new_gp64("argVal");
        cc.ldr(recvPtr, a64::ptr(statePtr, OFF_RECEIVER));
        cc.ldr(tempPtr, a64::ptr(statePtr, OFF_TEMPBASE));
        cc.ldr(argVal,  a64::ptr(tempPtr, 0));
        int slotOff = 8 + recvVarIndex * 8;
        cc.str(argVal,  a64::ptr(recvPtr, slotOff));
        // Returned value is the receiver itself.
        cc.mov(retOop, recvPtr);
        break;
    }
    case ReturnKind::InitRecvVar: {
        // init-style: receiver.instVar[recvVarIndex] := <constant>; return receiver.
        a64::Gp recvPtr = cc.new_gp64("recvPtr");
        a64::Gp cstVal  = cc.new_gp64("cstVal");
        cc.ldr(recvPtr, a64::ptr(statePtr, OFF_RECEIVER));
        cc.mov(cstVal,  asmjit::Imm(immBits));
        int slotOff = OBJ_SLOT_0 + recvVarIndex * 8;
        cc.str(cstVal,  a64::ptr(recvPtr, slotOff));
        cc.mov(retOop,  recvPtr);
        break;
    }
    case ReturnKind::LitVar: {
        // ^ literal-var N = literals[N].value (association slot 1).
        a64::Gp litsPtr = cc.new_gp64("litsPtr");
        a64::Gp assoc   = cc.new_gp64("assoc");
        cc.ldr(litsPtr, a64::ptr(statePtr, OFF_LITERALS));
        cc.ldr(assoc,   a64::ptr(litsPtr, litIndex * 8));
        cc.ldr(retOop,  a64::ptr(assoc, ASSOC_VALUE));
        break;
    }
    case ReturnKind::TempReturn: {
        // ^ tempN — read from tempBase[N].  recvVarIndex reused as
        // the temp index.
        a64::Gp tempPtr = cc.new_gp64("tempPtr");
        cc.ldr(tempPtr, a64::ptr(statePtr, OFF_TEMPBASE));
        cc.ldr(retOop,  a64::ptr(tempPtr, recvVarIndex * 8));
        break;
    }
    case ReturnKind::IntAccumRecvVar: {
        // ivar[M] := ivar[M] +/- <push1>; ^ self.
        // Fast path: tag-check both, do tagged add/sub with overflow,
        // store back to ivar[M], return receiver.  Bail path: emit
        // the original sequence's equivalent by setting up state and
        // exiting ExitSend at the arith byte.
        auto loadPush = [&](const Push& p) -> a64::Gp {
            a64::Gp v = cc.new_gp64("pv");
            switch (p.src) {
            case PushSrc::Receiver:
                cc.ldr(v, a64::ptr(statePtr, OFF_RECEIVER));
                break;
            case PushSrc::RecvVar: {
                a64::Gp rp = cc.new_gp64("rp");
                cc.ldr(rp, a64::ptr(statePtr, OFF_RECEIVER));
                cc.ldr(v, a64::ptr(rp, OBJ_SLOT_0 + p.idx * 8));
                break;
            }
            case PushSrc::Temp: {
                a64::Gp tp = cc.new_gp64("tp");
                cc.ldr(tp, a64::ptr(statePtr, OFF_TEMPBASE));
                cc.ldr(v, a64::ptr(tp, p.idx * 8));
                break;
            }
            case PushSrc::ImmOop:
                cc.mov(v, asmjit::Imm(p.bits));
                break;
            case PushSrc::LitVar: {
                a64::Gp lp = cc.new_gp64("lp");
                a64::Gp as = cc.new_gp64("as");
                cc.ldr(lp, a64::ptr(statePtr, OFF_LITERALS));
                cc.ldr(as, a64::ptr(lp, p.idx * 8));
                cc.ldr(v,  a64::ptr(as, ASSOC_VALUE));
                break;
            }
            case PushSrc::None:
                break;
            }
            return v;
        };

        // Load receiver pointer + current ivar value.
        a64::Gp recvPtr = cc.new_gp64("recvPtr");
        cc.ldr(recvPtr, a64::ptr(statePtr, OFF_RECEIVER));
        int slotOff = OBJ_SLOT_0 + recvVarIndex * 8;
        a64::Gp a = cc.new_gp64("a");
        cc.ldr(a, a64::ptr(recvPtr, slotOff));
        a64::Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        a64::Gp aTag = cc.new_gp64("aTag");
        cc.and_(aTag, a, asmjit::Imm(0x7));
        cc.cmp(aTag, asmjit::Imm(1));
        cc.b_ne(lblBail);
        a64::Gp bTag = cc.new_gp64("bTag");
        cc.and_(bTag, b, asmjit::Imm(0x7));
        cc.cmp(bTag, asmjit::Imm(1));
        cc.b_ne(lblBail);

        a64::Gp result = cc.new_gp64("result");
        if (arithOp == 0x60) {
            a64::Gp tmp = cc.new_gp64("tmp");
            cc.sub(tmp, a, asmjit::Imm(1));
            cc.adds(result, tmp, b);
        } else {
            a64::Gp tmp = cc.new_gp64("tmp");
            cc.subs(tmp, a, b);
            cc.add(result, tmp, asmjit::Imm(1));
        }
        cc.b_vs(lblBail);

        // Store back to ivar[M].
        cc.str(result, a64::ptr(recvPtr, slotOff));
        // Return receiver.
        cc.str(recvPtr, a64::ptr(statePtr, OFF_RETVAL));
        a64::Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, EXIT_RETURN);
        cc.str(exitOk, a64::ptr(statePtr, OFF_EXIT));
        cc.b(lblDone);

        // Bail path: restore the original bytecode's stack state and
        // let the interpreter re-run from PushRecvVar.  Since we
        // haven't pushed anything in the fast path, just set ip back
        // to bcStart and exit ExitSend... actually simpler: push a and
        // b, set ip to the arith byte, sendArgCount=1, ExitSend.  The
        // interpreter will see <a> <b> on stack and dispatch the
        // arith send, then the PopStore + Return will follow in
        // interpreter mode.  Wait — on ExitSend the interpreter
        // dispatches the send bytecode at state.ip, consuming 2 stack
        // items.  But then PopStoreRecvVar would run AFTER on its
        // own.  So the flow works: send pops+pushes result, PopStore
        // stores and pops, Return fires.
        cc.bind(lblBail);
        a64::Gp sp = cc.new_gp64("sp");
        cc.ldr(sp, a64::ptr(statePtr, OFF_SP));
        cc.str(a, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(b, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(sp, a64::ptr(statePtr, OFF_SP));

        a64::Gp ip = cc.new_gp64("ip");
        cc.ldr(ip, a64::ptr(statePtr, OFF_IP));
        cc.add(ip, ip, 2);  // arith bytecode is at bcStart+2
        cc.str(ip, a64::ptr(statePtr, OFF_IP));

        a64::Gp w1 = cc.new_gp32("w1");
        cc.mov(w1, 1);
        cc.str(w1, a64::ptr(statePtr, OFF_SENDARGCOUNT));

        a64::Gp zero64 = cc.new_gp64("zero64");
        cc.mov(zero64, 0);
        cc.str(zero64, a64::ptr(statePtr, OFF_ICDATAPTR));

        a64::Gp exitBail = cc.new_gp32("exitBail");
        cc.mov(exitBail, EXIT_SEND);
        cc.str(exitBail, a64::ptr(statePtr, OFF_EXIT));

        cc.bind(lblDone);
        cc.end_func();
        goto emitted;
    }
    case ReturnKind::IntBitOpReturn: {
        // ^ <push0> bitAnd:/bitOr: <push1> — tagged SmallInt bit op.
        auto loadPush = [&](const Push& p) -> a64::Gp {
            a64::Gp v = cc.new_gp64("pv");
            switch (p.src) {
            case PushSrc::Receiver:
                cc.ldr(v, a64::ptr(statePtr, OFF_RECEIVER)); break;
            case PushSrc::RecvVar: {
                a64::Gp rp = cc.new_gp64("rp");
                cc.ldr(rp, a64::ptr(statePtr, OFF_RECEIVER));
                cc.ldr(v, a64::ptr(rp, OBJ_SLOT_0 + p.idx * 8));
                break;
            }
            case PushSrc::LitVar: {
                a64::Gp lp = cc.new_gp64("lp");
                a64::Gp as = cc.new_gp64("as");
                cc.ldr(lp, a64::ptr(statePtr, OFF_LITERALS));
                cc.ldr(as, a64::ptr(lp, p.idx * 8));
                cc.ldr(v,  a64::ptr(as, ASSOC_VALUE));
                break;
            }
            case PushSrc::Temp: {
                a64::Gp tp = cc.new_gp64("tp");
                cc.ldr(tp, a64::ptr(statePtr, OFF_TEMPBASE));
                cc.ldr(v, a64::ptr(tp, p.idx * 8));
                break;
            }
            case PushSrc::ImmOop:
                cc.mov(v, asmjit::Imm(p.bits)); break;
            case PushSrc::None: break;
            }
            return v;
        };

        a64::Gp a = loadPush(pushes[0]);
        a64::Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        a64::Gp aTag = cc.new_gp64("aTag");
        cc.and_(aTag, a, asmjit::Imm(0x7));
        cc.cmp(aTag, asmjit::Imm(1));
        cc.b_ne(lblBail);
        a64::Gp bTag = cc.new_gp64("bTag");
        cc.and_(bTag, b, asmjit::Imm(0x7));
        cc.cmp(bTag, asmjit::Imm(1));
        cc.b_ne(lblBail);

        // Tag bit is preserved by AND/OR since both operands have it set.
        a64::Gp result = cc.new_gp64("result");
        if (arithOp == 0x6E)       cc.and_(result, a, b);  // bitAnd:
        else /* 0x6F */            cc.orr(result, a, b);   // bitOr:

        cc.str(result, a64::ptr(statePtr, OFF_RETVAL));
        a64::Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, EXIT_RETURN);
        cc.str(exitOk, a64::ptr(statePtr, OFF_EXIT));
        cc.b(lblDone);

        cc.bind(lblBail);
        a64::Gp sp = cc.new_gp64("sp");
        cc.ldr(sp, a64::ptr(statePtr, OFF_SP));
        cc.str(a, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(b, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(sp, a64::ptr(statePtr, OFF_SP));

        a64::Gp ip = cc.new_gp64("ip");
        cc.ldr(ip, a64::ptr(statePtr, OFF_IP));
        cc.add(ip, ip, sendIPOff);
        cc.str(ip, a64::ptr(statePtr, OFF_IP));

        a64::Gp w1 = cc.new_gp32("w1");
        cc.mov(w1, 1);
        cc.str(w1, a64::ptr(statePtr, OFF_SENDARGCOUNT));

        a64::Gp zero64 = cc.new_gp64("zero64");
        cc.mov(zero64, 0);
        cc.str(zero64, a64::ptr(statePtr, OFF_ICDATAPTR));

        a64::Gp exitBail = cc.new_gp32("exitBail");
        cc.mov(exitBail, EXIT_SEND);
        cc.str(exitBail, a64::ptr(statePtr, OFF_EXIT));

        cc.bind(lblDone);
        cc.end_func();
        goto emitted;
    }
    case ReturnKind::IntCmpReturn: {
        // ^ <push0> cmp <push1> — SmallInt comparison fast path.
        // 0x62 <, 0x63 >, 0x64 <=, 0x65 >=, 0x66 =, 0x67 ~=.
        auto loadPush = [&](const Push& p) -> a64::Gp {
            a64::Gp v = cc.new_gp64("pv");
            switch (p.src) {
            case PushSrc::Receiver:
                cc.ldr(v, a64::ptr(statePtr, OFF_RECEIVER));
                break;
            case PushSrc::RecvVar: {
                a64::Gp rp = cc.new_gp64("rp");
                cc.ldr(rp, a64::ptr(statePtr, OFF_RECEIVER));
                cc.ldr(v, a64::ptr(rp, OBJ_SLOT_0 + p.idx * 8));
                break;
            }
            case PushSrc::LitVar: {
                a64::Gp lp = cc.new_gp64("lp");
                a64::Gp as = cc.new_gp64("as");
                cc.ldr(lp, a64::ptr(statePtr, OFF_LITERALS));
                cc.ldr(as, a64::ptr(lp, p.idx * 8));
                cc.ldr(v,  a64::ptr(as, ASSOC_VALUE));
                break;
            }
            case PushSrc::Temp: {
                a64::Gp tp = cc.new_gp64("tp");
                cc.ldr(tp, a64::ptr(statePtr, OFF_TEMPBASE));
                cc.ldr(v, a64::ptr(tp, p.idx * 8));
                break;
            }
            case PushSrc::ImmOop:
                cc.mov(v, asmjit::Imm(p.bits));
                break;
            case PushSrc::None:
                break;
            }
            return v;
        };

        a64::Gp a = loadPush(pushes[0]);
        a64::Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        a64::Gp aTag = cc.new_gp64("aTag");
        cc.and_(aTag, a, asmjit::Imm(0x7));
        cc.cmp(aTag, asmjit::Imm(1));
        cc.b_ne(lblBail);
        a64::Gp bTag = cc.new_gp64("bTag");
        cc.and_(bTag, b, asmjit::Imm(0x7));
        cc.cmp(bTag, asmjit::Imm(1));
        cc.b_ne(lblBail);

        // Load trueOop + falseOop from state (already materialised).
        a64::Gp tOop = cc.new_gp64("tOop");
        a64::Gp fOop = cc.new_gp64("fOop");
        cc.ldr(tOop, a64::ptr(statePtr, OFF_TRUEOOP));
        cc.ldr(fOop, a64::ptr(statePtr, OFF_FALSEOOP));

        // Compare tagged values (order preserved for same-tag SmallInts).
        cc.cmp(a, b);
        a64::Gp result = cc.new_gp64("result");
        // csel t, f, cond — result = cond ? t : f.  asmjit uses csel
        // with condition code.
        auto cond = asmjit::a64::CondCode::kEQ;
        switch (arithOp) {
        case 0x62: cond = asmjit::a64::CondCode::kLT; break;   // <
        case 0x63: cond = asmjit::a64::CondCode::kGT; break;   // >
        case 0x64: cond = asmjit::a64::CondCode::kLE; break;   // <=
        case 0x65: cond = asmjit::a64::CondCode::kGE; break;   // >=
        case 0x66: cond = asmjit::a64::CondCode::kEQ; break;   // =
        case 0x67: cond = asmjit::a64::CondCode::kNE; break;   // ~=
        }
        cc.csel(result, tOop, fOop, cond);

        cc.str(result, a64::ptr(statePtr, OFF_RETVAL));
        a64::Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, EXIT_RETURN);
        cc.str(exitOk, a64::ptr(statePtr, OFF_EXIT));
        cc.b(lblDone);

        // Bail: push both, exit at cmp byte with ExitSend.
        cc.bind(lblBail);
        a64::Gp sp = cc.new_gp64("sp");
        cc.ldr(sp, a64::ptr(statePtr, OFF_SP));
        cc.str(a, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(b, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(sp, a64::ptr(statePtr, OFF_SP));

        a64::Gp ip = cc.new_gp64("ip");
        cc.ldr(ip, a64::ptr(statePtr, OFF_IP));
        cc.add(ip, ip, sendIPOff);
        cc.str(ip, a64::ptr(statePtr, OFF_IP));

        a64::Gp w1 = cc.new_gp32("w1");
        cc.mov(w1, 1);
        cc.str(w1, a64::ptr(statePtr, OFF_SENDARGCOUNT));

        a64::Gp zero64 = cc.new_gp64("zero64");
        cc.mov(zero64, 0);
        cc.str(zero64, a64::ptr(statePtr, OFF_ICDATAPTR));

        a64::Gp exitBail = cc.new_gp32("exitBail");
        cc.mov(exitBail, EXIT_SEND);
        cc.str(exitBail, a64::ptr(statePtr, OFF_EXIT));

        cc.bind(lblDone);
        cc.end_func();
        goto emitted;
    }
    case ReturnKind::IntArithMulReturn: {
        // ^ <push0> * <push1> — SmallInt multiply fast path.
        // Untag, multiply, check int64 overflow via smulh, check
        // SmallInt range via lsl/asr round-trip, retag.
        auto loadPush = [&](const Push& p) -> a64::Gp {
            a64::Gp v = cc.new_gp64("pv");
            switch (p.src) {
            case PushSrc::Receiver:
                cc.ldr(v, a64::ptr(statePtr, OFF_RECEIVER)); break;
            case PushSrc::RecvVar: {
                a64::Gp rp = cc.new_gp64("rp");
                cc.ldr(rp, a64::ptr(statePtr, OFF_RECEIVER));
                cc.ldr(v, a64::ptr(rp, OBJ_SLOT_0 + p.idx * 8));
                break;
            }
            case PushSrc::LitVar: {
                a64::Gp lp = cc.new_gp64("lp");
                a64::Gp as = cc.new_gp64("as");
                cc.ldr(lp, a64::ptr(statePtr, OFF_LITERALS));
                cc.ldr(as, a64::ptr(lp, p.idx * 8));
                cc.ldr(v,  a64::ptr(as, ASSOC_VALUE));
                break;
            }
            case PushSrc::Temp: {
                a64::Gp tp = cc.new_gp64("tp");
                cc.ldr(tp, a64::ptr(statePtr, OFF_TEMPBASE));
                cc.ldr(v, a64::ptr(tp, p.idx * 8));
                break;
            }
            case PushSrc::ImmOop:
                cc.mov(v, asmjit::Imm(p.bits)); break;
            case PushSrc::None: break;
            }
            return v;
        };

        a64::Gp a = loadPush(pushes[0]);
        a64::Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        a64::Gp aTag = cc.new_gp64("aTag");
        cc.and_(aTag, a, asmjit::Imm(0x7));
        cc.cmp(aTag, asmjit::Imm(1));
        cc.b_ne(lblBail);
        a64::Gp bTag = cc.new_gp64("bTag");
        cc.and_(bTag, b, asmjit::Imm(0x7));
        cc.cmp(bTag, asmjit::Imm(1));
        cc.b_ne(lblBail);

        // Untag
        a64::Gp aVal = cc.new_gp64("aVal");
        a64::Gp bVal = cc.new_gp64("bVal");
        cc.asr(aVal, a, asmjit::Imm(3));
        cc.asr(bVal, b, asmjit::Imm(3));

        // Multiply (low 64 bits)
        a64::Gp resLo = cc.new_gp64("resLo");
        cc.mul(resLo, aVal, bVal);

        // High 64 bits for overflow check
        a64::Gp resHi = cc.new_gp64("resHi");
        cc.smulh(resHi, aVal, bVal);

        // If result fits in int64, high == asr(low, 63).  Otherwise overflow.
        a64::Gp expectedHi = cc.new_gp64("expHi");
        cc.asr(expectedHi, resLo, asmjit::Imm(63));
        cc.cmp(resHi, expectedHi);
        cc.b_ne(lblBail);

        // SmallInt range check: result * 8 must fit in int64 signed.
        // Equivalent to: |result| < 2^60.  Verify via lsl/asr round-trip.
        a64::Gp tagged = cc.new_gp64("tagged");
        cc.lsl(tagged, resLo, asmjit::Imm(3));
        a64::Gp back = cc.new_gp64("back");
        cc.asr(back, tagged, asmjit::Imm(3));
        cc.cmp(back, resLo);
        cc.b_ne(lblBail);

        // Set tag bit
        a64::Gp result = cc.new_gp64("result");
        cc.orr(result, tagged, asmjit::Imm(1));

        cc.str(result, a64::ptr(statePtr, OFF_RETVAL));
        a64::Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, EXIT_RETURN);
        cc.str(exitOk, a64::ptr(statePtr, OFF_EXIT));
        cc.b(lblDone);

        cc.bind(lblBail);
        a64::Gp sp = cc.new_gp64("sp");
        cc.ldr(sp, a64::ptr(statePtr, OFF_SP));
        cc.str(a, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(b, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(sp, a64::ptr(statePtr, OFF_SP));

        a64::Gp ip = cc.new_gp64("ip");
        cc.ldr(ip, a64::ptr(statePtr, OFF_IP));
        cc.add(ip, ip, sendIPOff);
        cc.str(ip, a64::ptr(statePtr, OFF_IP));

        a64::Gp w1 = cc.new_gp32("w1");
        cc.mov(w1, 1);
        cc.str(w1, a64::ptr(statePtr, OFF_SENDARGCOUNT));

        a64::Gp zero64 = cc.new_gp64("zero64");
        cc.mov(zero64, 0);
        cc.str(zero64, a64::ptr(statePtr, OFF_ICDATAPTR));

        a64::Gp exitBail = cc.new_gp32("exitBail");
        cc.mov(exitBail, EXIT_SEND);
        cc.str(exitBail, a64::ptr(statePtr, OFF_EXIT));

        cc.bind(lblDone);
        cc.end_func();
        goto emitted;
    }
    case ReturnKind::IntArithAddReturn:
    case ReturnKind::IntArithSubReturn: {
        // ^ <push0> +/- <push1> — SmallInt-tagged arithmetic fast path.
        //
        // Encoding reminder: SmallInt Oop = value * 8 + 1  (tag = 001).
        // For add, we can keep tagged representation and use the
        // identity (a*8+1) - 1 + (b*8+1) = (a+b)*8 + 1 — one sub + one
        // adds emits a correctly-tagged result.  Overflow on the ADDS
        // is the 64-bit signed overflow flag, which correctly catches
        // 61-bit SmallInt overflow too (proved by case analysis at
        // the edges of the SmallInt range).
        //
        // Bail path: push both operands and exit with ExitSend at
        // the arith bytecode; the interpreter's arith handler has
        // its own SmallInt fast path + full-send fallback.

        // Helper: same lambda as SendExit variants above, but we
        // inline a tiny copy here because we need the values both
        // for the fast path AND for bail pushing.
        auto loadPush = [&](const Push& p) -> a64::Gp {
            a64::Gp v = cc.new_gp64("pv");
            switch (p.src) {
            case PushSrc::Receiver:
                cc.ldr(v, a64::ptr(statePtr, OFF_RECEIVER));
                break;
            case PushSrc::RecvVar: {
                a64::Gp rp = cc.new_gp64("rp");
                cc.ldr(rp, a64::ptr(statePtr, OFF_RECEIVER));
                cc.ldr(v, a64::ptr(rp, OBJ_SLOT_0 + p.idx * 8));
                break;
            }
            case PushSrc::LitVar: {
                a64::Gp lp = cc.new_gp64("lp");
                a64::Gp as = cc.new_gp64("as");
                cc.ldr(lp, a64::ptr(statePtr, OFF_LITERALS));
                cc.ldr(as, a64::ptr(lp, p.idx * 8));
                cc.ldr(v,  a64::ptr(as, ASSOC_VALUE));
                break;
            }
            case PushSrc::Temp: {
                a64::Gp tp = cc.new_gp64("tp");
                cc.ldr(tp, a64::ptr(statePtr, OFF_TEMPBASE));
                cc.ldr(v, a64::ptr(tp, p.idx * 8));
                break;
            }
            case PushSrc::ImmOop:
                cc.mov(v, asmjit::Imm(p.bits));
                break;
            case PushSrc::None:
                break;
            }
            return v;
        };

        a64::Gp a = loadPush(pushes[0]);
        a64::Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        // Tag-check both.  SmallInt tag == 1.
        a64::Gp aTag = cc.new_gp64("aTag");
        cc.and_(aTag, a, asmjit::Imm(0x7));
        cc.cmp(aTag, asmjit::Imm(1));
        cc.b_ne(lblBail);

        a64::Gp bTag = cc.new_gp64("bTag");
        cc.and_(bTag, b, asmjit::Imm(0x7));
        cc.cmp(bTag, asmjit::Imm(1));
        cc.b_ne(lblBail);

        a64::Gp result = cc.new_gp64("result");
        if (kind == ReturnKind::IntArithAddReturn) {
            // result = (a - 1) + b  = a*8 + b*8 + 1 = (a_val+b_val)*8 + 1
            a64::Gp tmp = cc.new_gp64("tmp");
            cc.sub(tmp, a, asmjit::Imm(1));
            cc.adds(result, tmp, b);
        } else {
            // result = (a - b) + 1  = (a_val-b_val)*8 + 1
            a64::Gp tmp = cc.new_gp64("tmp");
            cc.subs(tmp, a, b);
            cc.add(result, tmp, asmjit::Imm(1));
            // subs sets V on overflow — already in flags.
        }
        cc.b_vs(lblBail);  // overflow → bail

        // Fast-path success: result is correctly tagged.
        cc.str(result, a64::ptr(statePtr, OFF_RETVAL));
        a64::Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, EXIT_RETURN);
        cc.str(exitOk, a64::ptr(statePtr, OFF_EXIT));
        cc.b(lblDone);

        // Bail path: push both operands, advance state.ip to the
        // arith bytecode, set sendArgCount=1, exit ExitSend.  The
        // interpreter resumes at the arith bytecode and handles it
        // (its own SmallInt fast path + full send fallback).
        cc.bind(lblBail);
        a64::Gp sp = cc.new_gp64("sp");
        cc.ldr(sp, a64::ptr(statePtr, OFF_SP));
        cc.str(a, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(b, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(sp, a64::ptr(statePtr, OFF_SP));

        a64::Gp ip = cc.new_gp64("ip");
        cc.ldr(ip, a64::ptr(statePtr, OFF_IP));
        cc.add(ip, ip, sendIPOff);
        cc.str(ip, a64::ptr(statePtr, OFF_IP));

        a64::Gp zero32 = cc.new_gp32("zero32");
        cc.mov(zero32, 1);          // sendArgCount = 1
        cc.str(zero32, a64::ptr(statePtr, OFF_SENDARGCOUNT));

        a64::Gp zero64 = cc.new_gp64("zero64");
        cc.mov(zero64, 0);
        cc.str(zero64, a64::ptr(statePtr, OFF_ICDATAPTR));

        a64::Gp exitBail = cc.new_gp32("exitBail");
        cc.mov(exitBail, EXIT_SEND);
        cc.str(exitBail, a64::ptr(statePtr, OFF_EXIT));

        cc.bind(lblDone);
        cc.end_func();
        goto emitted;
    }
    case ReturnKind::ZeroArgSendInlineIC:
    case ReturnKind::OneArgSendInlineIC: {
        // ^ <push0> [foo: <push1>] — 1 or 2 pushes + inline IC probe.
        //   hit:  cachedTarget = icData[N*3+1], exitReason = ExitSendCached
        //   miss: cachedTarget left alone, exitReason = ExitSend
        // Both paths set icDataPtr, sendArgCount=(numPushes-1), ip += sendIPOff.

        // Allocate IC buffer (152 bytes) and seed icData[18] with the
        // send's selector.  Send byte is at bcStart + (numPushes-1);
        // for Send0 base is 0x80, for Send1 base is 0x90.
        int sendByteOff = sendIPOff;  // offset of the send byte
        uint8_t sendOp = bytes[bcStart + sendByteOff];
        uint8_t sendBase = (numPushes == 1) ? SistaV1::Send0Base
                                            : SistaV1::Send1Base;
        int selIdx = sendOp - sendBase;

        auto ic = std::make_unique<uint64_t[]>(IC_SLOTS);
        std::memset(ic.get(), 0, IC_SLOTS * sizeof(uint64_t));
        uint64_t icAddr = reinterpret_cast<uint64_t>(ic.get());
        icBuffers_.push_back(std::move(ic));
        icBuffers_.back()[18] = methodObj->slotAt(1 + selIdx).rawBits();  // selbits slot

        // Helper to load a Push value into a register.
        auto loadPush = [&](const Push& p) -> a64::Gp {
            a64::Gp v = cc.new_gp64("pushV");
            switch (p.src) {
            case PushSrc::Receiver:
                cc.ldr(v, a64::ptr(statePtr, OFF_RECEIVER));
                break;
            case PushSrc::RecvVar: {
                a64::Gp rp = cc.new_gp64("rp");
                cc.ldr(rp, a64::ptr(statePtr, OFF_RECEIVER));
                cc.ldr(v, a64::ptr(rp, OBJ_SLOT_0 + p.idx * 8));
                break;
            }
            case PushSrc::LitVar: {
                a64::Gp lp = cc.new_gp64("lp");
                a64::Gp as = cc.new_gp64("as");
                cc.ldr(lp, a64::ptr(statePtr, OFF_LITERALS));
                cc.ldr(as, a64::ptr(lp, p.idx * 8));
                cc.ldr(v,  a64::ptr(as, ASSOC_VALUE));
                break;
            }
            case PushSrc::Temp: {
                a64::Gp tp = cc.new_gp64("tp");
                cc.ldr(tp, a64::ptr(statePtr, OFF_TEMPBASE));
                cc.ldr(v, a64::ptr(tp, p.idx * 8));
                break;
            }
            case PushSrc::ImmOop:
                cc.mov(v, asmjit::Imm(p.bits));
                break;
            case PushSrc::None:
                break;
            }
            return v;
        };

        // Load receiver (push0).  We'll keep TWO copies of it in
        // separate virtual registers — one for the push, one for
        // the lookupKey computation — so the register allocator can
        // independently assign them without the aliased usage causing
        // observed flakiness in the 1-arg path.
        a64::Gp recvLoaded = loadPush(pushes[0]);
        a64::Gp recvForPush = cc.new_gp64("recvForPush");
        a64::Gp recvForKey  = cc.new_gp64("recvForKey");
        cc.mov(recvForPush, recvLoaded);
        cc.mov(recvForKey,  recvLoaded);
        a64::Gp recv = recvForKey;  // used below for key computation
        Label lblHeap   = cc.new_label();
        Label lblCheck  = cc.new_label();
        Label lblMiss   = cc.new_label();
        Label lblHit[IC_ENTRIES];
        for (int i = 0; i < IC_ENTRIES; i++) lblHit[i] = cc.new_label();
        Label lblEpi    = cc.new_label();

        a64::Gp tag       = cc.new_gp64("tag");
        a64::Gp lookupKey = cc.new_gp64("lookupKey");
        cc.and_(tag, recv, asmjit::Imm(0x7));
        cc.cbz(tag, lblHeap);

        cc.mov(lookupKey, tag);
        cc.orr(lookupKey, lookupKey, asmjit::Imm(0x80000000ULL));
        cc.b(lblCheck);

        cc.bind(lblHeap);
        // Compute classIndex from header — for non-nil objects.  For
        // nil, we'll still compute key=0 (which won't match any IC
        // entry because empty slots == 0 and we gate that below), then
        // fall through to miss.
        a64::Gp header = cc.new_gp64("header");
        cc.ldr(header, a64::ptr(recv));
        cc.and_(lookupKey, header, asmjit::Imm(CLASS_INDEX_MASK));

        cc.bind(lblCheck);
        // Push receiver + any args onto the Smalltalk stack.
        a64::Gp sp = cc.new_gp64("sp");
        cc.ldr(sp, a64::ptr(statePtr, OFF_SP));
        cc.str(recvForPush, a64::ptr(sp));
        cc.add(sp, sp, 8);
        if (numPushes >= 2) {
            a64::Gp a = loadPush(pushes[1]);
            cc.str(a, a64::ptr(sp));
            cc.add(sp, sp, 8);
        }
        cc.str(sp, a64::ptr(statePtr, OFF_SP));

        a64::Gp icData = cc.new_gp64("icData");
        cc.mov(icData, asmjit::Imm(icAddr));

        // 6-way probe (PHARO_T2_FORCE_MISS=1 skips the probe to isolate
        // whether the IC hit path contributes to the 1-arg bug).
        if (!getenv("PHARO_T2_FORCE_MISS")) {
            for (int i = 0; i < IC_ENTRIES; i++) {
                a64::Gp key = cc.new_gp64("key");
                cc.ldr(key, a64::ptr(icData, IC_KEY_OFF(i)));
                cc.cmp(lookupKey, key);
                cc.b_eq(lblHit[i]);
            }
        }
        cc.b(lblMiss);

        // Hit paths: load method, set exitReason = ExitSendCached,
        // jump to epilogue.  (Removed the debug counter bump here —
        // it was emitting a 3-store per-path sequence that might
        // have contributed to the 1-arg flakiness.  If we need the
        // counters again they can go on the C side in the chain loop.)
        a64::Gp method   = cc.new_gp64("method");
        a64::Gp exitCode = cc.new_gp32("exitCode");
        for (int i = 0; i < IC_ENTRIES; i++) {
            cc.bind(lblHit[i]);
            cc.ldr(method, a64::ptr(icData, IC_METHOD_OFF(i)));
            cc.mov(exitCode, EXIT_SEND_CACHED);
            cc.str(method, a64::ptr(statePtr, OFF_CACHEDTARGET));
            cc.b(lblEpi);
        }

        // Miss: exitReason = ExitSend; fall through to epilogue.
        cc.bind(lblMiss);
        cc.mov(exitCode, EXIT_SEND);

        cc.bind(lblEpi);
        cc.str(icData, a64::ptr(statePtr, OFF_ICDATAPTR));
        a64::Gp w0 = cc.new_gp32("argCount");
        cc.mov(w0, numPushes - 1);   // 0-arg send = 0, 1-arg send = 1
        cc.str(w0, a64::ptr(statePtr, OFF_SENDARGCOUNT));
        a64::Gp ip = cc.new_gp64("ip");
        cc.ldr(ip, a64::ptr(statePtr, OFF_IP));
        cc.add(ip, ip, sendIPOff);
        cc.str(ip, a64::ptr(statePtr, OFF_IP));
        cc.str(exitCode, a64::ptr(statePtr, OFF_EXIT));

        cc.end_func();
        goto emitted;
    }
    case ReturnKind::SendExit: {
        // Push 1 or 2 values, advance state.ip to the send/arith byte,
        // clear IC + sendArgCount, exit with ExitSend.  The interpreter
        // resumes at the send bytecode, dispatches it, then executes the
        // trailing ReturnTop.
        a64::Gp sp = cc.new_gp64("sp");
        cc.ldr(sp, a64::ptr(statePtr, OFF_SP));
        for (int i = 0; i < numPushes; i++) {
            const Push& p = pushes[i];
            a64::Gp v = cc.new_gp64("pushV");
            switch (p.src) {
            case PushSrc::Receiver:
                cc.ldr(v, a64::ptr(statePtr, OFF_RECEIVER));
                break;
            case PushSrc::ImmOop:
                cc.mov(v, asmjit::Imm(p.bits));
                break;
            case PushSrc::RecvVar: {
                a64::Gp recvPtr = cc.new_gp64("recvPtr");
                cc.ldr(recvPtr, a64::ptr(statePtr, OFF_RECEIVER));
                cc.ldr(v, a64::ptr(recvPtr, OBJ_SLOT_0 + p.idx * 8));
                break;
            }
            case PushSrc::LitVar: {
                a64::Gp litsPtr = cc.new_gp64("litsPtr");
                a64::Gp assoc   = cc.new_gp64("assoc");
                cc.ldr(litsPtr, a64::ptr(statePtr, OFF_LITERALS));
                cc.ldr(assoc,   a64::ptr(litsPtr, p.idx * 8));
                cc.ldr(v,       a64::ptr(assoc, ASSOC_VALUE));
                break;
            }
            case PushSrc::Temp: {
                a64::Gp tempPtr = cc.new_gp64("tempPtr");
                cc.ldr(tempPtr, a64::ptr(statePtr, OFF_TEMPBASE));
                cc.ldr(v, a64::ptr(tempPtr, p.idx * 8));
                break;
            }
            case PushSrc::None:
                break;
            }
            cc.str(v, a64::ptr(sp));
            cc.add(sp, sp, 8);
        }
        cc.str(sp, a64::ptr(statePtr, OFF_SP));

        a64::Gp ip = cc.new_gp64("ip");
        cc.ldr(ip, a64::ptr(statePtr, OFF_IP));
        cc.add(ip, ip, sendIPOff);
        cc.str(ip, a64::ptr(statePtr, OFF_IP));

        a64::Gp zero32 = cc.new_gp32("zero32");
        cc.mov(zero32, 0);
        cc.str(zero32, a64::ptr(statePtr, OFF_SENDARGCOUNT));

        a64::Gp zero64 = cc.new_gp64("zero64");
        cc.mov(zero64, 0);
        cc.str(zero64, a64::ptr(statePtr, OFF_ICDATAPTR));

        a64::Gp exitVal = cc.new_gp32("exitVal");
        cc.mov(exitVal, EXIT_SEND);
        cc.str(exitVal, a64::ptr(statePtr, OFF_EXIT));

        cc.end_func();
        goto emitted;
    }
    }

    // state.returnValue = retOop
    cc.str(retOop, a64::ptr(statePtr, OFF_RETVAL));

    // state.exitReason = EXIT_RETURN (int32)
    {
        a64::Gp exitVal = cc.new_gp32("exitVal");
        cc.mov(exitVal, EXIT_RETURN);
        cc.str(exitVal, a64::ptr(statePtr, OFF_EXIT));
    }

    cc.end_func();
emitted:
    Error err = cc.finalize();
    if (err != kErrorOk) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    void* func = nullptr;
    err = runtime_->add(&func, &code);
    if (err != kErrorOk) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    methodsCompiled_++;
    g_compiled++;
    return func;
}

// ---------------------------------------------------------------------------
// Multi-bytecode compilation (task #33)
// ---------------------------------------------------------------------------
//
// Walks the full bytecode sequence and emits asmjit code for each op,
// maintaining state.sp in memory throughout (simple and safe — no SP
// register caching).  Supports pushes, stores, pops, arith with
// SmallInt fast path + bail, and returns.  Any other bytecode causes
// the whole method to bail.
//
// On arith bail (tag mismatch or overflow): advance state.ip to the
// arith bytecode, leave the two operands on state.sp, set
// sendArgCount=1, exit ExitSend.  The interpreter resumes at the arith
// byte and handles the send fallback.

void* Tier2Compiler::tryCompileMultiBC(Oop compiledMethod,
                                        const uint8_t* bytes,
                                        size_t bcStart,
                                        size_t bodyLen) {
    (void)compiledMethod;

    // Minimum bytecode count — skip tiny methods already covered by
    // template matching (also avoids empty methods).
    if (bodyLen < 2) return nullptr;

    // Pass 1: walk and classify.  Supported ops + one trailing return
    // are fully compiled.  A bailable send (Send0/1/2 with known arg
    // count) causes multi-bc to compile the PREFIX up to and including
    // the send-exit state flush; the interpreter runs the rest.
    // Anything else (unsupported op or special-selector send) causes
    // the whole method to bail to template matching.
    //
    // Forward jumps (0xB0-0xB7 unconditional, gated) are walked
    // linearly — dead code between source and target is compiled but
    // unreachable.  Targets must be forward (> current offset) and
    // within bodyLen.
    bool sawReturn = false;
    bool willBailAtSend = false;
    size_t bailSendOffset = 0;
    uint8_t bailSendOp = 0;
    std::set<size_t> jumpTargets;
    auto shortJumpTarget = [](uint8_t op, size_t i) -> size_t {
        uint8_t base = SistaV1::isShortJump(op)      ? SistaV1::ShortJumpBase
                     : SistaV1::isShortJumpTrue(op)  ? SistaV1::ShortJumpTrueBase
                                                    : SistaV1::ShortJumpFalseBase;
        return i + 2 + (size_t)(op - base);
    };
    const bool jumpsEnabled = getenv("PHARO_T2_MBC_JUMPS") != nullptr;
    // 2-byte extended jumps (0xED/0xEE/0xEF): offset is unsigned byte
    // (0-255, forward).  ExtA/ExtB prefix bytecodes (0xE0/0xE1) are
    // NOT supported yet — methods that use them bail via the generic
    // unsupported-op path.  This means we only compile forward jumps
    // up to +255 bytes; signed 16-bit offsets (incl. backward loops)
    // need 1.2d's extB state machine.
    auto isExtForwardJump = [](uint8_t op) {
        return op == SistaV1::ExtJump
            || op == SistaV1::ExtJumpTrue
            || op == SistaV1::ExtJumpFalse;
    };
    for (size_t i = 0; i < bodyLen;) {
        uint8_t op = bytes[bcStart + i];
        if (SistaV1::isAnyShortJump(op) && jumpsEnabled) {
            size_t target = shortJumpTarget(op, i);
            if (target >= bodyLen || target <= i) {
                g_mbcBailed++;
                return nullptr;
            }
            jumpTargets.insert(target);
            i += mbcOpLen(op);   // linear walk continues sequentially
            continue;
        }
        if (isExtForwardJump(op) && jumpsEnabled) {
            // 2-byte bytecode: op, offset-byte.  Must have room.
            if (i + 1 >= bodyLen) {
                g_mbcBailed++;
                return nullptr;
            }
            uint8_t offByte = bytes[bcStart + i + 1];
            size_t target = i + 2 + (size_t)offByte;
            // Forward-only (unsigned byte) within bodyLen.
            if (target >= bodyLen || target <= i) {
                g_mbcBailed++;
                return nullptr;
            }
            jumpTargets.insert(target);
            i += 2;
            continue;
        }
        if (isMBCSupported(op)) {
            if (op >= SistaV1::ReturnReceiver && op <= SistaV1::ReturnTop) {
                sawReturn = true;
                break;   // rest is unreachable
            }
            i += mbcOpLen(op);
            continue;
        }
        if (isMBCBailableSend(op)) {
            // Gated: enabling multi-bc-send (either simple bail or
            // inline IC) causes a bench regression similar to the
            // 0-arg inline IC experiment — T2 intercepts methods
            // that T1 warms up via its own IC path.  IC hit rate
            // drops from 97.5% to 49.9% when enabled.  Keep as
            // opt-in (PHARO_T2_MBC_SENDS=1 bail, default=off) until
            // we untangle the T1/T2 IC interaction.
            if (!getenv("PHARO_T2_MBC_SENDS") && !getenv("PHARO_T2_MBC_IC")) {
                g_mbcBailed++;
                return nullptr;
            }
            int nArgs = mbcSendArgCount(op);
            if (nArgs < 0) { g_mbcBailed++; return nullptr; }
            willBailAtSend = true;
            bailSendOffset = i;
            bailSendOp = op;
            break;
        }
        // Unsupported — bail the whole method.
        g_mbcBailed++;
        return nullptr;
    }

    if (!sawReturn && !willBailAtSend) {
        // Fell off the end (e.g. last op was an unsupported bc that we
        // lost count of).  Shouldn't happen with current logic but
        // guard anyway.
        g_mbcBailed++;
        return nullptr;
    }

    // If we WILL bail at a send, we don't need a return; we'll exit
    // ExitSend.  Otherwise we require a trailing return op.
    if (sawReturn) {
        uint8_t lastReturn = bytes[bcStart + bodyLen - 1];
        // The saw-return loop broke early, so lastReturn might not be
        // at bodyLen-1.  We just need A return somewhere along the
        // linear path; that check is already satisfied.
        (void)lastReturn;
    }

    ObjectHeader* methodObj = reinterpret_cast<ObjectHeader*>(compiledMethod.rawBits());
    uint64_t nilBits   = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits();
    uint64_t oneBits   = Oop::fromSmallInteger(1).rawBits();
    uint64_t zeroBits  = Oop::fromSmallInteger(0).rawBits();

    int numLiterals = 0;
    {
        Oop hdrOop = methodObj->slotAt(0);
        if (hdrOop.isSmallInteger()) {
            numLiterals = (int)(hdrOop.asSmallInteger() & 0x7FFF);
        }
    }

    // Pass 2: emit.  Maintain state.sp as a live register that we
    // write back to state on every arith bail + at method end.
    using namespace asmjit;
    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());

    a64::Compiler cc(&code);
    FuncNode* funcNode = cc.add_func(FuncSignature::build<void, void*>());
    a64::Gp statePtr = cc.new_gp64("state");
    funcNode->set_arg(0, statePtr);

    a64::Gp sp = cc.new_gp64("sp");
    cc.ldr(sp, a64::ptr(statePtr, OFF_SP));

    // Emit a push: *sp++ = v
    auto emitPush = [&](a64::Gp v) {
        cc.str(v, a64::ptr(sp));
        cc.add(sp, sp, 8);
    };

    // Emit a pop into a fresh reg: v = *--sp
    auto emitPop = [&]() -> a64::Gp {
        cc.sub(sp, sp, 8);
        a64::Gp v = cc.new_gp64("popV");
        cc.ldr(v, a64::ptr(sp));
        return v;
    };

    // Arith bail target — shared across all arith ops in the method.
    // On bail, state.sp is already the pre-arith sp (with operands on
    // the stack), so we just need to set ip/exit.
    Label lblBailTail = cc.new_label();
    // We use a location-register for the bail IP offset — each arith
    // that might bail stores its own bcStart-relative offset into ipOffReg
    // before branching to lblBailTail.
    a64::Gp bailIPOff = cc.new_gp64("bailIPOff");

    // Pre-allocate one asmjit label per jump target; bound when we
    // emit the op at that offset (below).
    std::map<size_t, Label> offsetLabels;
    for (size_t t : jumpTargets) offsetLabels[t] = cc.new_label();

    bool emittedReturn = false;
    bool emittedSendBail = false;

    for (size_t i = 0; i < bodyLen && !emittedReturn && !emittedSendBail;) {
        uint8_t op = bytes[bcStart + i];
        size_t opStart = i;

        // Bind label here if this offset is a jump target.
        {
            auto it = offsetLabels.find(i);
            if (it != offsetLabels.end()) cc.bind(it->second);
        }

        // At the first bailable send we have two modes:
        //   default: inline IC probe (6-way) → ExitSendCached on hit
        //            so the chain loop does a J2J direct call; only
        //            the miss exits to the interpreter.
        //   PHARO_T2_MBC_SENDS=1: bail straight to interpreter
        //            (the older simpler behaviour; kept for bisection).
        if (willBailAtSend && i == bailSendOffset) {
            // Flush state.sp so the interpreter/chain-loop sees the
            // right stack layout after our pushes.
            cc.str(sp, a64::ptr(statePtr, OFF_SP));

            int nArgs = mbcSendArgCount(op);

            // Always update state.ip to the send byte (both code paths
            // need this).
            a64::Gp ip = cc.new_gp64("sendIp");
            cc.ldr(ip, a64::ptr(statePtr, OFF_IP));
            cc.add(ip, ip, i);
            cc.str(ip, a64::ptr(statePtr, OFF_IP));

            a64::Gp wArgs = cc.new_gp32("wArgs");
            cc.mov(wArgs, nArgs);
            cc.str(wArgs, a64::ptr(statePtr, OFF_SENDARGCOUNT));

            // One of PHARO_T2_MBC_SENDS / PHARO_T2_MBC_IC is set here
            // (guaranteed by the pass-1 check above).
            if (getenv("PHARO_T2_MBC_SENDS")) {
                // SIMPLE BAIL mode: icDataPtr=0, exit ExitSend.
                a64::Gp zero64 = cc.new_gp64("zero64");
                cc.mov(zero64, 0);
                cc.str(zero64, a64::ptr(statePtr, OFF_ICDATAPTR));
                a64::Gp exitV = cc.new_gp32("exitV");
                cc.mov(exitV, EXIT_SEND);
                cc.str(exitV, a64::ptr(statePtr, OFF_EXIT));
                emittedSendBail = true;
                break;
            }

            // INLINE IC mode (PHARO_T2_MBC_IC=1): 6-way probe with
            // hit → ExitSendCached (chain loop makes direct J2J call
            // to target) and miss → ExitSend (interpreter patches IC
            // via pendingICPatch_).
            //
            // Tried shared T1 IC (computing T1's sendIdx by walking
            // bytecodes): gave 5× slowdown (1600ms vs 370ms) — the
            // sendIdx mapping is subtly wrong (T1 includes extra/
            // ext bytecodes in the count), so patches land on the
            // wrong IC slot and corrupt T1's IC data.  Reverted to
            // private IC; still hits the "T1 and T2 warm
            // independently" issue but at least doesn't corrupt.
            auto ic = std::make_unique<uint64_t[]>(IC_SLOTS);
            std::memset(ic.get(), 0, IC_SLOTS * sizeof(uint64_t));
            uint64_t icAddr = reinterpret_cast<uint64_t>(ic.get());
            icBuffers_.push_back(std::move(ic));
            int sendBase = 0x80;
            if (op >= 0x90 && op <= 0x9F) sendBase = 0x90;
            else if (op >= 0xA0 && op <= 0xAF) sendBase = 0xA0;
            int selIdx = op - sendBase;
            if (selIdx >= 0 && selIdx < (int)numLiterals) {
                icBuffers_.back()[18] = methodObj->slotAt(1 + selIdx).rawBits();
            }

            // Get receiver from stack: sp[-(nArgs+1)*8].
            a64::Gp recv = cc.new_gp64("recvIC");
            cc.ldr(recv, a64::ptr(sp, -(nArgs + 1) * 8));

            // Compute lookupKey: heap obj → classIndex, immediate → tag|0x80000000.
            Label lblHeap = cc.new_label();
            Label lblCheck = cc.new_label();
            Label lblMiss = cc.new_label();
            Label lblEpi = cc.new_label();
            Label lblHit[IC_ENTRIES];
            for (int k = 0; k < IC_ENTRIES; k++) lblHit[k] = cc.new_label();

            a64::Gp tag = cc.new_gp64("tag");
            a64::Gp lookupKey = cc.new_gp64("lookupKey");
            cc.and_(tag, recv, asmjit::Imm(0x7));
            cc.cbz(tag, lblHeap);
            cc.mov(lookupKey, tag);
            cc.orr(lookupKey, lookupKey, asmjit::Imm(0x80000000ULL));
            cc.b(lblCheck);

            cc.bind(lblHeap);
            cc.cbz(recv, lblMiss);   // nil → bail
            a64::Gp header = cc.new_gp64("hdr");
            cc.ldr(header, a64::ptr(recv));
            cc.and_(lookupKey, header, asmjit::Imm(CLASS_INDEX_MASK));

            cc.bind(lblCheck);
            a64::Gp icData = cc.new_gp64("icData");
            cc.mov(icData, asmjit::Imm(icAddr));
            for (int k = 0; k < IC_ENTRIES; k++) {
                a64::Gp key = cc.new_gp64("key");
                cc.ldr(key, a64::ptr(icData, IC_KEY_OFF(k)));
                cc.cmp(lookupKey, key);
                cc.b_eq(lblHit[k]);
            }
            cc.b(lblMiss);

            a64::Gp exitCode = cc.new_gp32("exitIC");
            // Hit paths: load method from entry, set cachedTarget,
            // exitReason=ExitSendCached, jump to epilogue.
            for (int k = 0; k < IC_ENTRIES; k++) {
                cc.bind(lblHit[k]);
                a64::Gp method = cc.new_gp64("methodIC");
                cc.ldr(method, a64::ptr(icData, IC_METHOD_OFF(k)));
                cc.str(method, a64::ptr(statePtr, OFF_CACHEDTARGET));
                cc.mov(exitCode, EXIT_SEND_CACHED);
                cc.b(lblEpi);
            }

            // Miss: exitReason=ExitSend; fall through to epilogue.
            cc.bind(lblMiss);
            cc.mov(exitCode, EXIT_SEND);

            cc.bind(lblEpi);
            cc.str(icData, a64::ptr(statePtr, OFF_ICDATAPTR));
            cc.str(exitCode, a64::ptr(statePtr, OFF_EXIT));

            emittedSendBail = true;
            break;
        }

        if (op >= SistaV1::PushRecvVarBase && op <= SistaV1::PushRecvVarLast) {
            int idx = op - SistaV1::PushRecvVarBase;
            a64::Gp recv = cc.new_gp64("recv");
            cc.ldr(recv, a64::ptr(statePtr, OFF_RECEIVER));
            a64::Gp v = cc.new_gp64("v");
            cc.ldr(v, a64::ptr(recv, OBJ_SLOT_0 + idx * 8));
            emitPush(v);
        }
        else if (op >= SistaV1::PushLitVarBase && op <= SistaV1::PushLitVarLast) {
            int idx = op - SistaV1::PushLitVarBase;
            a64::Gp lp = cc.new_gp64("lp");
            cc.ldr(lp, a64::ptr(statePtr, OFF_LITERALS));
            a64::Gp as = cc.new_gp64("as");
            cc.ldr(as, a64::ptr(lp, idx * 8));
            a64::Gp v = cc.new_gp64("v");
            cc.ldr(v, a64::ptr(as, ASSOC_VALUE));
            emitPush(v);
        }
        else if (op >= SistaV1::PushLitConstBase && op <= SistaV1::PushLitConstLast) {
            int idx = op - SistaV1::PushLitConstBase;
            Oop lit = methodObj->slotAt(1 + idx);
            a64::Gp v = cc.new_gp64("v");
            cc.mov(v, asmjit::Imm(lit.rawBits()));
            emitPush(v);
        }
        else if (op >= SistaV1::PushTempBase && op <= SistaV1::PushTempLast) {
            int idx = op - SistaV1::PushTempBase;
            a64::Gp tp = cc.new_gp64("tp");
            cc.ldr(tp, a64::ptr(statePtr, OFF_TEMPBASE));
            a64::Gp v = cc.new_gp64("v");
            cc.ldr(v, a64::ptr(tp, idx * 8));
            emitPush(v);
        }
        else if (op == SistaV1::PushReceiver) {
            a64::Gp v = cc.new_gp64("v");
            cc.ldr(v, a64::ptr(statePtr, OFF_RECEIVER));
            emitPush(v);
        }
        else if (op == SistaV1::PushTrue) {
            a64::Gp v = cc.new_gp64("v");
            cc.ldr(v, a64::ptr(statePtr, OFF_TRUEOOP));
            emitPush(v);
        }
        else if (op == SistaV1::PushFalse) {
            a64::Gp v = cc.new_gp64("v");
            cc.ldr(v, a64::ptr(statePtr, OFF_FALSEOOP));
            emitPush(v);
        }
        else if (op == SistaV1::PushNil) {
            a64::Gp v = cc.new_gp64("v");
            cc.mov(v, asmjit::Imm(nilBits));
            emitPush(v);
        }
        else if (op == SistaV1::PushZero) {
            a64::Gp v = cc.new_gp64("v");
            cc.mov(v, asmjit::Imm(zeroBits));
            emitPush(v);
        }
        else if (op == SistaV1::PushOne) {
            a64::Gp v = cc.new_gp64("v");
            cc.mov(v, asmjit::Imm(oneBits));
            emitPush(v);
        }
        else if (op == SistaV1::Dup) {
            // Read TOS (sp[-1]) and push it again.
            a64::Gp v = cc.new_gp64("dupv");
            cc.ldr(v, a64::ptr(sp, -8));
            emitPush(v);
        }
        else if (op >= 0x60 && op <= 0x6F) {
            // Arith.  For MVP we emit the fast path for +, -, *, and
            // the comparison/bit ops; bail for /, //, \\, @, bitShift:.
            // On bail, state.sp must reflect the pre-arith state (both
            // operands on stack).  We've not pushed any extras — the
            // operands ARE the top 2 stack entries.
            //
            // Before arith: state.sp IS correct already.  Write sp
            // back to state.sp so the bail path has consistent state.
            cc.str(sp, a64::ptr(statePtr, OFF_SP));

            // Load operands from stack (non-destructive, leave them
            // for potential bail).
            a64::Gp b = cc.new_gp64("b");
            a64::Gp a = cc.new_gp64("a");
            cc.ldr(b, a64::ptr(sp, -8));
            cc.ldr(a, a64::ptr(sp, -16));

            Label lblLocalBail = cc.new_label();

            // Tag-check both.
            a64::Gp tA = cc.new_gp64("tA");
            cc.and_(tA, a, asmjit::Imm(0x7));
            cc.cmp(tA, asmjit::Imm(1));
            cc.b_ne(lblLocalBail);
            a64::Gp tB = cc.new_gp64("tB");
            cc.and_(tB, b, asmjit::Imm(0x7));
            cc.cmp(tB, asmjit::Imm(1));
            cc.b_ne(lblLocalBail);

            a64::Gp result = cc.new_gp64("result");
            bool isComp = (op >= 0x62 && op <= 0x67);
            bool isBit  = (op == 0x6E || op == 0x6F);

            if (op == 0x60) {
                a64::Gp tmp = cc.new_gp64("tmp");
                cc.sub(tmp, a, asmjit::Imm(1));
                cc.adds(result, tmp, b);
                cc.b_vs(lblLocalBail);
            }
            else if (op == 0x61) {
                a64::Gp tmp = cc.new_gp64("tmp");
                cc.subs(tmp, a, b);
                cc.add(result, tmp, asmjit::Imm(1));
                cc.b_vs(lblLocalBail);
            }
            else if (op == 0x68) {
                // mul with overflow + range check
                a64::Gp aVal = cc.new_gp64("aVal");
                a64::Gp bVal = cc.new_gp64("bVal");
                cc.asr(aVal, a, asmjit::Imm(3));
                cc.asr(bVal, b, asmjit::Imm(3));
                a64::Gp resLo = cc.new_gp64("resLo");
                cc.mul(resLo, aVal, bVal);
                a64::Gp resHi = cc.new_gp64("resHi");
                cc.smulh(resHi, aVal, bVal);
                a64::Gp expHi = cc.new_gp64("expHi");
                cc.asr(expHi, resLo, asmjit::Imm(63));
                cc.cmp(resHi, expHi);
                cc.b_ne(lblLocalBail);
                a64::Gp tagged = cc.new_gp64("tagged");
                cc.lsl(tagged, resLo, asmjit::Imm(3));
                a64::Gp back = cc.new_gp64("back");
                cc.asr(back, tagged, asmjit::Imm(3));
                cc.cmp(back, resLo);
                cc.b_ne(lblLocalBail);
                cc.orr(result, tagged, asmjit::Imm(1));
            }
            else if (isBit) {
                if (op == 0x6E) cc.and_(result, a, b);
                else            cc.orr(result, a, b);
            }
            else if (isComp) {
                a64::Gp tOop = cc.new_gp64("tOop");
                a64::Gp fOop = cc.new_gp64("fOop");
                cc.ldr(tOop, a64::ptr(statePtr, OFF_TRUEOOP));
                cc.ldr(fOop, a64::ptr(statePtr, OFF_FALSEOOP));
                cc.cmp(a, b);
                auto cond = asmjit::a64::CondCode::kEQ;
                switch (op) {
                case 0x62: cond = asmjit::a64::CondCode::kLT; break;
                case 0x63: cond = asmjit::a64::CondCode::kGT; break;
                case 0x64: cond = asmjit::a64::CondCode::kLE; break;
                case 0x65: cond = asmjit::a64::CondCode::kGE; break;
                case 0x66: cond = asmjit::a64::CondCode::kEQ; break;
                case 0x67: cond = asmjit::a64::CondCode::kNE; break;
                }
                cc.csel(result, tOop, fOop, cond);
            }
            else {
                // Unsupported arith op (/ // \\ @ bitShift:) — bail always.
                cc.b(lblLocalBail);
                cc.bind(lblLocalBail);
                cc.mov(bailIPOff, asmjit::Imm(opStart));
                cc.b(lblBailTail);
                i += mbcOpLen(op);
                continue;
            }

            // Replace the top 2 stack entries with 1 result.
            cc.sub(sp, sp, 8);
            cc.str(result, a64::ptr(sp, -8));
            // Note: sp is still in register; we'll write it back when
            // the method returns or on next arith bail.
            Label lblArithDone = cc.new_label();
            cc.b(lblArithDone);

            cc.bind(lblLocalBail);
            // state.sp already written to state (above).  Branch to the
            // shared bail tail with opStart as the ip offset.
            cc.mov(bailIPOff, asmjit::Imm(opStart));
            cc.b(lblBailTail);

            cc.bind(lblArithDone);
        }
        else if (op >= SistaV1::PopStoreRecvBase && op <= SistaV1::PopStoreRecvLast) {
            int idx = op - SistaV1::PopStoreRecvBase;
            a64::Gp v = emitPop();
            a64::Gp recv = cc.new_gp64("recv");
            cc.ldr(recv, a64::ptr(statePtr, OFF_RECEIVER));
            cc.str(v, a64::ptr(recv, OBJ_SLOT_0 + idx * 8));
        }
        else if (op >= SistaV1::PopStoreTempBase && op <= SistaV1::PopStoreTempLast) {
            int idx = op - SistaV1::PopStoreTempBase;
            a64::Gp v = emitPop();
            a64::Gp tp = cc.new_gp64("tp");
            cc.ldr(tp, a64::ptr(statePtr, OFF_TEMPBASE));
            cc.str(v, a64::ptr(tp, idx * 8));
        }
        else if (op == SistaV1::Pop) {
            cc.sub(sp, sp, 8);
        }
        else if (op >= SistaV1::ReturnReceiver && op <= SistaV1::ReturnTop) {
            // Flush sp back to state before we exit (so the stack state
            // is consistent for the caller).
            cc.str(sp, a64::ptr(statePtr, OFF_SP));

            a64::Gp ret = cc.new_gp64("ret");
            switch (op) {
            case SistaV1::ReturnReceiver:
                cc.ldr(ret, a64::ptr(statePtr, OFF_RECEIVER));
                break;
            case SistaV1::ReturnTrue:
                cc.ldr(ret, a64::ptr(statePtr, OFF_TRUEOOP));
                break;
            case SistaV1::ReturnFalse:
                cc.ldr(ret, a64::ptr(statePtr, OFF_FALSEOOP));
                break;
            case SistaV1::ReturnNil:
                cc.mov(ret, asmjit::Imm(nilBits));
                break;
            case SistaV1::ReturnTop:
                cc.ldr(ret, a64::ptr(sp, -8));
                break;
            }
            cc.str(ret, a64::ptr(statePtr, OFF_RETVAL));
            a64::Gp exitCode = cc.new_gp32("exit");
            cc.mov(exitCode, EXIT_RETURN);
            cc.str(exitCode, a64::ptr(statePtr, OFF_EXIT));
            emittedReturn = true;
        }
        else if (SistaV1::isShortJump(op)) {
            // Unconditional forward jump: branch to target label.
            // Target was validated in pass 1.  Dead code between here
            // and target is still emitted (linear walk) but
            // unreachable — asmjit lays it out but control flow
            // never enters it.
            size_t target = i + 2 + (size_t)(op - SistaV1::ShortJumpBase);
            cc.b(offsetLabels.at(target));
        }
        else if (SistaV1::isConditionalShortJump(op) ||
                 op == SistaV1::ExtJumpTrue ||
                 op == SistaV1::ExtJumpFalse) {
            // Conditional forward jump: pop a boolean, branch to
            // target if condition matches, fall through otherwise.
            // On non-boolean: bail to interpreter at this op's
            // offset so it can send #mustBeBoolean.
            //
            // We peek TOS (no pop yet) so the bail path leaves the
            // value on the stack for the interpreter.  sp is flushed
            // before the comparison so the bail state is consistent.
            bool jumpIfTrue;
            size_t target;
            int opLen;
            if (SistaV1::isShortJumpTrue(op)) {
                jumpIfTrue = true;
                target = i + 2 + (size_t)(op - SistaV1::ShortJumpTrueBase);
                opLen = 1;
            } else if (SistaV1::isShortJumpFalse(op)) {
                jumpIfTrue = false;
                target = i + 2 + (size_t)(op - SistaV1::ShortJumpFalseBase);
                opLen = 1;
            } else {
                // ExtJumpTrue / ExtJumpFalse: 2-byte, offset is unsigned byte
                jumpIfTrue = (op == SistaV1::ExtJumpTrue);
                target = i + 2 + (size_t)bytes[bcStart + i + 1];
                opLen = 2;
            }

            cc.str(sp, a64::ptr(statePtr, OFF_SP));

            a64::Gp v = cc.new_gp64("jv");
            cc.ldr(v, a64::ptr(sp, -8));
            a64::Gp tOop = cc.new_gp64("tOop");
            a64::Gp fOop = cc.new_gp64("fOop");
            cc.ldr(tOop, a64::ptr(statePtr, OFF_TRUEOOP));
            cc.ldr(fOop, a64::ptr(statePtr, OFF_FALSEOOP));

            Label lblJumpTaken    = cc.new_label();
            Label lblFallThrough  = cc.new_label();
            Label lblNonBool      = cc.new_label();

            // Matches-condition check: is v the bool that triggers the jump?
            cc.cmp(v, jumpIfTrue ? tOop : fOop);
            cc.b_eq(lblJumpTaken);
            // Else check other boolean: fall through if matches,
            // bail on non-boolean.
            cc.cmp(v, jumpIfTrue ? fOop : tOop);
            cc.b_ne(lblNonBool);

            // Fall-through path: pop, continue sequentially.
            cc.sub(sp, sp, 8);
            cc.b(lblFallThrough);

            cc.bind(lblJumpTaken);
            cc.sub(sp, sp, 8);
            cc.b(offsetLabels.at(target));

            cc.bind(lblNonBool);
            // Leave the non-boolean on the stack; sp flushed above.
            // Set ip to this op's offset so the interpreter re-dispatches
            // and sends #mustBeBoolean itself.
            cc.mov(bailIPOff, asmjit::Imm(opStart));
            cc.b(lblBailTail);

            cc.bind(lblFallThrough);

            if (opLen == 2) {
                i += 2;
                continue;   // skip default i += mbcOpLen(op) (returns 1)
            }
        }
        else if (op == SistaV1::ExtJump) {
            // 2-byte unconditional forward jump (ExtB=0, no prefix).
            size_t target = i + 2 + (size_t)bytes[bcStart + i + 1];
            cc.b(offsetLabels.at(target));
            i += 2;
            continue;
        }
        else {
            // Should have been caught in pass 1.
            g_mbcBailed++;
            return nullptr;
        }

        i += mbcOpLen(op);
    }

    // End-of-function: asmjit closes the function naturally.
    // Shared arith bail tail: consumes bailIPOff as the offset into
    // the method's bytecodes.  state.sp is already flushed (we store
    // sp back to state before each arith op).
    Label lblFuncEnd = cc.new_label();
    cc.b(lblFuncEnd);   // skip past the bail tail on normal flow

    cc.bind(lblBailTail);
    a64::Gp ip = cc.new_gp64("ip");
    cc.ldr(ip, a64::ptr(statePtr, OFF_IP));
    cc.add(ip, ip, bailIPOff);
    cc.str(ip, a64::ptr(statePtr, OFF_IP));

    a64::Gp sendArgs = cc.new_gp32("sendArgs");
    cc.mov(sendArgs, 1);    // arith is 1-arg
    cc.str(sendArgs, a64::ptr(statePtr, OFF_SENDARGCOUNT));

    a64::Gp zero64 = cc.new_gp64("zero64");
    cc.mov(zero64, 0);
    cc.str(zero64, a64::ptr(statePtr, OFF_ICDATAPTR));

    a64::Gp exitBail = cc.new_gp32("exitBail");
    cc.mov(exitBail, EXIT_SEND);
    cc.str(exitBail, a64::ptr(statePtr, OFF_EXIT));

    cc.bind(lblFuncEnd);
    cc.end_func();

    Error err = cc.finalize();
    if (err != kErrorOk) {
        g_mbcBailed++;
        return nullptr;
    }
    void* func = nullptr;
    err = runtime_->add(&func, &code);
    if (err != kErrorOk) {
        g_mbcBailed++;
        return nullptr;
    }

    g_mbcCompiled++;
    return func;
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
