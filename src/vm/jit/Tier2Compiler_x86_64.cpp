/*
 * Tier2Compiler_x86_64.cpp - asmjit-based optimizing JIT, x86_64 sibling
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Per-arch sibling of Tier2Compiler_arm64.cpp.  Selected by CMakeLists
 * on non-arm64 hosts (Linux x86_64, macOS x86_64).
 *
 * STATUS — op-by-op port in progress.  Pattern-matched method shapes:
 *
 *   Return-shape (always EXIT_RETURN):
 *     ReturnReceiver / ReturnTrue / ReturnFalse / ReturnNil   (1 byte)
 *     PushReceiver|PushTrue|PushFalse|PushNil + ReturnTop     (2 bytes)
 *     PushZero|PushOne + ReturnTop                            (2 bytes)
 *     PushRecvVar(N) + ReturnTop                              (getter)
 *     PushTemp(N) + ReturnTop                                 (^ tempN)
 *     PushLitConst(N) + ReturnTop                             (constant baked in)
 *     PushLitVar(N) + ReturnTop                               (Association.value)
 *     PushTemp(0) + PopStoreRecv(M) + ReturnReceiver          (setter)
 *     Push<const> + PopStoreRecv(M) + ReturnReceiver          (init)
 *
 *   SmallInt fast paths (EXIT_RETURN on success, EXIT_SEND on bail):
 *     <pushA> <pushB> + (0x60/0x61) + ReturnTop  (^ a +/- b)
 *     <pushA> <pushB> + 0x68        + ReturnTop  (^ a * b — 128-bit OF check)
 *     <pushA> <pushB> + (0x62..0x67) + ReturnTop (^ a cmp b — six ops)
 *     <pushA> <pushB> + (0x6E/0x6F) + ReturnTop  (^ a bitAnd:/bitOr: b)
 *     PushRecvVar(M) <pushB> (0x60/0x61) PopStoreRecv(M) RetRecv
 *                                                (ivar +/- ; ^ self, 5 bytes)
 *
 *   Inline-IC sends (probe → ExitSendCached on hit, ExitSend on miss):
 *     <pushA> <pushB> Send1(N) ReturnTop        (^ a foo: b)
 *     <push> Send0(N) ReturnTop                 (^ rcvr foo, gated:
 *                                                PHARO_T2_ZEROARG_IC=1
 *                                                — also gated on arm64)
 *
 * Anything else returns nullptr; runtime falls through to tier-1.
 * Adding ops: mirror the arm64 sibling's matcher / emitter, swapping
 * a64::* for asmjit::x86::*.  Each new op should land with a SUnit
 * run as the correctness gate.
 *
 * Calling convention matches Tier 1 / Tier 2 arm64:
 *   void fn(JITState* state)   — state in rdi (SysV) / rcx (Win64);
 *                                 asmjit Compiler abstracts the ABI.
 *   exit via state->returnValue + state->exitReason = EXIT_RETURN
 */

#include "Tier2Compiler.hpp"
#include "JITRuntime.hpp"
#include "JITCompiler.hpp"
#include "JITMethod.hpp"
#include "SistaV1.hpp"
#include "../ObjectMemory.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#if PHARO_JIT_ENABLED

#include <asmjit/x86.h>
#include <asmjit/core/jitruntime.h>

namespace pharo {
namespace jit {

namespace {

// JITState field offsets — must match Tier 1 / Tier 2 arm64 so the
// runtime can invoke either tier transparently.  See JITState.hpp.
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
constexpr int OFF_YIELDCD      = 176;  // int32_t yieldCountdown

// ObjectHeader classIndex layout (low 22 bits).
constexpr uint64_t CLASS_INDEX_MASK = 0x3FFFFFULL;

// Object slot layout: ObjectHeader is 8 bytes; slot[N] starts at
// byte 8 + N*8.  Association.value is slot[1].
constexpr int OBJ_SLOT_0  = 8;
constexpr int ASSOC_VALUE = 16;

// ExitReason values (JITState.hpp).
constexpr int EXIT_RETURN       = 1;
constexpr int EXIT_SEND         = 2;
constexpr int EXIT_SEND_CACHED  = 7;
constexpr int EXIT_YIELD        = 11;  // backward-jump yield bail

// IC data layout (mirror JITMethod.hpp): 6 entries × 24 bytes (key,
// methodBits, extra) + 8 bytes selectorBits at slot 18.
constexpr int IC_ENTRIES = 6;
constexpr int IC_SLOTS   = IC_ENTRIES * 3 + 1;
constexpr int IC_KEY_OFF(int i)    { return i * 3 * 8;       }
constexpr int IC_METHOD_OFF(int i) { return i * 3 * 8 + 8;   }

// Bail / compile counters.  Per-arch (matches the SistaLowering
// per-arch g_lowerStats pattern).
size_t g_compiled = 0;
size_t g_bailed   = 0;

// Multi-bytecode compile counters (separate from g_compiled/g_bailed
// so we can attribute MBC-vs-template throughput).
size_t g_mbcCompiled = 0;
size_t g_mbcBailed   = 0;

// PHARO_T2_MBC_JUMPS=0 disables short jumps + ExtJump variants in MBC
// compilation.  Default-on (matches arm64 sibling 2026-04-18 flip after
// SmallIntegerTest + whileTrue/ifTrue:ifFalse: stress validation).
bool jumpsEnabledByEnv() {
    const char* env = std::getenv("PHARO_T2_MBC_JUMPS");
    return !(env && env[0] == '0');
}

// Bytecodes the multi-bc walker knows how to emit inline.  Anything
// outside this set causes the whole method to bail (return nullptr).
// Short jumps gated by PHARO_T2_MBC_JUMPS.
bool isMBCSupported(uint8_t op) {
    if (op <= 0x4B) return true;                 // Push{RecvVar,LitVar,LitConst,Temp}
    if (op == SistaV1::PushReceiver) return true;
    if (op >= 0x4D && op <= 0x51) return true;   // Push{True,False,Nil,Zero,One}
    if (op == SistaV1::Dup) return true;
    if (op >= SistaV1::ReturnReceiver && op <= SistaV1::ReturnTop) return true;
    if (op >= 0x60 && op <= 0x6F) return true;   // arith
    if (op >= SistaV1::PopStoreRecvBase && op <= SistaV1::PopStoreRecvLast) return true;
    if (op >= SistaV1::PopStoreTempBase && op <= SistaV1::PopStoreTempLast) return true;
    if (op == SistaV1::Pop) return true;
    if (SistaV1::isAnyShortJump(op) && jumpsEnabledByEnv()) return true;
    return false;
}

// Send bytecodes that the multi-bc walker can emit a "bail to
// interpreter at this byte" prefix for.  Gated separately
// (PHARO_T2_MBC_SENDS / PHARO_T2_MBC_IC); default off.
bool isMBCBailableSend(uint8_t op) {
    return (op >= 0x70 && op <= 0xAF);
}

// Number of args the bailable send takes.  -1 means "don't support"
// (SpecialSelectors don't carry an arg count we can use here).
int mbcSendArgCount(uint8_t op) {
    if (op >= 0x70 && op <= 0x7F) return -1;     // SpecialSelector — skip
    if (op >= 0x80 && op <= 0x8F) return 0;      // Send0
    if (op >= 0x90 && op <= 0x9F) return 1;      // Send1
    if (op >= 0xA0 && op <= 0xAF) return 2;      // Send2
    return -1;
}

// Byte length of a supported bytecode at offset i.  Always 1 here;
// the multi-bc walker handles 2-byte ExtJump variants separately
// (i += 2 / i += 4 in those branches) so this only fires for the
// 1-byte ops that fall through the default-step path.
int mbcOpLen(uint8_t op) { (void)op; return 1; }

// What kind of return-shape method this is.  Maps onto the load-Oop
// emit branch in compile().  Mirrors the arm64 sibling's enum.
enum class ReturnKind {
    Receiver,            // ^ self            — load state.receiver
    True,                // ^ true            — load state.trueOop
    False,               // ^ false           — load state.falseOop
    NilImm,              // ^ nil             — bake nil bits
    ImmediateOop,        // ^ <const>         — bake immBits
    RecvVar,             // ^ instVar[N]      — load receiver.slot[N]
    TempReturn,          // ^ tempN           — load tempBase[N]
    LitVar,              // ^ literal-var N   — load literals[N].value
    SetterRecvVar,       // x := arg0; ^ self
    InitRecvVar,         // x := <const>; ^ self
    IntArithAddReturn,   // ^ a + b (SmallInt fast path)
    IntArithSubReturn,   // ^ a - b
    IntArithMulReturn,   // ^ a * b (SmallInt with 128-bit OF check)
    IntCmpReturn,        // ^ a cmp b (SmallInt; 6 ops)
    IntBitOpReturn,      // ^ a bitAnd:/bitOr: b
    IntAccumRecvVar,     // ivar[M] := ivar[M] +/- b; ^ self  (5 bytes)
    ZeroArgSendInlineIC, // ^ <push> selector  (3 bytes; gated)
    OneArgSendInlineIC,  // ^ <a> selector: <b>  (4 bytes)
};

// Where a "push" bytecode reads its value from.  Used by IntArith /
// IntCmp / IntBitOp loadPush helper.
enum class PushSrc { None, Receiver, RecvVar, LitVar, Temp, ImmOop };

struct Push {
    PushSrc  src  = PushSrc::None;
    int      idx  = 0;
    uint64_t bits = 0;
};

}  // namespace

Tier2Compiler::Tier2Compiler(CodeZone& zone, MethodMap& methodMap,
                             ObjectMemory& memory, Interpreter& interp)
    : methodMap_(methodMap), memory_(memory) {
    (void)zone;
    (void)interp;
}

Tier2Compiler::~Tier2Compiler() = default;

bool Tier2Compiler::initialize() {
    runtime_ = std::make_unique<asmjit::JitRuntime>();
    return true;
}

void Tier2Compiler::dumpBailStats() {
    fprintf(stderr, "  T2 (asmjit): compiled=%zu bailed=%zu\n",
            g_compiled, g_bailed);
}

void Tier2Compiler::flushAllICs() {
    for (auto& buf : icBuffers_) {
        std::memset(buf.get(), 0, IC_SLOTS * sizeof(uint64_t));
    }
}

void* Tier2Compiler::compile(Oop compiledMethod, JITMethod* oldVersion) {
    (void)oldVersion;
    static bool trace = std::getenv("PHARO_T2_X86_TRACE") != nullptr;
    if (trace) fprintf(stderr, "[T2-x86] compile entry: method=%p\n",
                       (void*)compiledMethod.rawBits());
    if (!runtime_) {
        if (!initialize()) {
            compilationsFailed_++;
            return nullptr;
        }
    }

    // Walk method header.  Format mirrors the arm64 sibling — keep in
    // sync if either changes.
    ObjectHeader* methodObj =
        reinterpret_cast<ObjectHeader*>(compiledMethod.rawBits());
    Oop headerOop = methodObj->slotAt(0);
    if (!headerOop.isSmallInteger()) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }
    int64_t headerBits = headerOop.asSmallInteger();
    int  numLiterals  = (int)(headerBits & 0x7FFF);
    bool hasPrimitive = (headerBits >> 16) & 1;

    if (hasPrimitive) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    uint8_t* bytes = methodObj->bytes();
    size_t bcStart = (1 + numLiterals) * 8;
    size_t totalBytes = methodObj->slotCount() * 8;
    uint8_t fmt = static_cast<uint8_t>(methodObj->format());
    int unusedBytes = (fmt >= 24) ? (fmt - 24) : 0;
    if (bcStart + (size_t)unusedBytes >= totalBytes) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }
    size_t bodyLen = totalBytes - bcStart - (size_t)unusedBytes;

    // Try the multi-bytecode walker first — it covers methods with
    // arbitrary push/pop/store/arith/jump mixes that don't fit any of
    // the leading-pattern templates below.  Returns nullptr if it
    // can't handle the method (unsupported op, no return, etc.).
    if (void* mbcFunc = tryCompileMultiBC(compiledMethod, bytes, bcStart, bodyLen)) {
        methodsCompiled_++;
        g_compiled++;
        return mbcFunc;
    }

    // ---- Pattern match -----------------------------------------------------
    //
    // Match against the leading bytecode(s).  Anything past the return is
    // unreachable, so trailing bytes are irrelevant beyond bodyLen-bound
    // checks.  Mirrors the arm64 sibling's matcher one-for-one.
    uint8_t b0 = bytes[bcStart];
    uint8_t b1 = (bodyLen >= 2) ? bytes[bcStart + 1] : 0;
    uint8_t b2 = (bodyLen >= 3) ? bytes[bcStart + 2] : 0;

    ReturnKind kind;
    int slotIndex   = 0;          // recvVar / temp / litVar / setter index
    uint64_t immBits = 0;         // for NilImm / ImmediateOop / InitRecvVar
    Push pushes[2];               // IntArith / IntCmp / IntBitOp / SendIC operands
    int sendIPOff   = 0;          // bail-to-interp: byte offset of arith / send op
    int arithOp     = 0;          // SistaV1 arith bytecode (0x60..0x6F)
    int numPushes   = 0;          // SendIC: # of values to push (1 or 2)

    auto bail = [&](const char* why) -> void* {
        if (trace) fprintf(stderr, "[T2-x86]   bail: %s bodyLen=%zu b0=0x%02x\n",
                           why, bodyLen, b0);
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    };

    // Decode a single-byte "push" bytecode into a Push descriptor for
    // the IntArith / IntCmp / IntBitOp matchers.
    auto decodePush = [&](uint8_t op, Push& p) -> bool {
        if (op == SistaV1::PushReceiver)  { p.src = PushSrc::Receiver; return true; }
        if (op == SistaV1::PushTrue)      { p.src = PushSrc::ImmOop;
            p.bits = memory_.specialObject(SpecialObjectIndex::TrueObject).rawBits();
            return true; }
        if (op == SistaV1::PushFalse)     { p.src = PushSrc::ImmOop;
            p.bits = memory_.specialObject(SpecialObjectIndex::FalseObject).rawBits();
            return true; }
        if (op == SistaV1::PushNil)       { p.src = PushSrc::ImmOop;
            p.bits = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits();
            return true; }
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
                            && b0 == SistaV1::PushZero) {
        kind = ReturnKind::ImmediateOop;
        immBits = Oop::fromSmallInteger(0).rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && b0 == SistaV1::PushOne) {
        kind = ReturnKind::ImmediateOop;
        immBits = Oop::fromSmallInteger(1).rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushRecvVar(b0)) {
        kind = ReturnKind::RecvVar;
        slotIndex = b0 - SistaV1::PushRecvVarBase;
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushTemp(b0)) {
        kind = ReturnKind::TempReturn;
        slotIndex = b0 - SistaV1::PushTempBase;
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushLitConst(b0)) {
        int idx = b0 - SistaV1::PushLitConstBase;
        Oop lit = methodObj->slotAt(1 + idx);
        kind = ReturnKind::ImmediateOop;
        immBits = lit.rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushLitVar(b0)) {
        kind = ReturnKind::LitVar;
        slotIndex = b0 - SistaV1::PushLitVarBase;
    } else if (bodyLen >= 3 && b0 == SistaV1::PushTempBase
                            && SistaV1::isPopStoreRecv(b1)
                            && b2 == SistaV1::ReturnReceiver) {
        kind = ReturnKind::SetterRecvVar;
        slotIndex = b1 - SistaV1::PopStoreRecvBase;
    } else if (bodyLen >= 3 && SistaV1::isPopStoreRecv(b1)
                            && b2 == SistaV1::ReturnReceiver
                            && (b0 == SistaV1::PushZero
                             || b0 == SistaV1::PushOne
                             || b0 == SistaV1::PushNil
                             || b0 == SistaV1::PushTrue
                             || b0 == SistaV1::PushFalse)) {
        // init-style: ivar := <constant>; ^ self
        kind = ReturnKind::InitRecvVar;
        slotIndex = b1 - SistaV1::PopStoreRecvBase;
        if      (b0 == SistaV1::PushZero)  immBits = Oop::fromSmallInteger(0).rawBits();
        else if (b0 == SistaV1::PushOne)   immBits = Oop::fromSmallInteger(1).rawBits();
        else if (b0 == SistaV1::PushNil)   immBits = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits();
        else if (b0 == SistaV1::PushTrue)  immBits = memory_.specialObject(SpecialObjectIndex::TrueObject).rawBits();
        else /* PushFalse */                immBits = memory_.specialObject(SpecialObjectIndex::FalseObject).rawBits();
    } else if (bodyLen >= 5 && SistaV1::isPushRecvVar(b0)
                            && (b2 == 0x60 || b2 == 0x61)
                            && SistaV1::isPopStoreRecv(bytes[bcStart + 3])
                            && bytes[bcStart + 4] == SistaV1::ReturnReceiver
                            && decodePush(b1, pushes[1])
                            && (b0 - SistaV1::PushRecvVarBase)
                                 == (bytes[bcStart + 3] - SistaV1::PopStoreRecvBase)) {
        // ivar[M] := ivar[M] +/- <b>; ^ self
        // 5 bytes: PushRecvVar(M); <pushB>; arith; PopStoreRecv(M); RetRecv.
        // The Push/PopStore indices must match (same instVar).  Match
        // precedes IntArith{Add,Sub} below since this 5-byte shape would
        // otherwise be a prefix of `^ ivar +/- b` followed by trailing
        // dead code.
        kind = ReturnKind::IntAccumRecvVar;
        slotIndex = b0 - SistaV1::PushRecvVarBase;
        sendIPOff = 2;
        arithOp = b2;
    } else if (bodyLen >= 4 && (b2 == 0x60 || b2 == 0x61)
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <a> + <b>  or  ^ <a> - <b>  (SmallInt fast path)
        kind = (b2 == 0x60) ? ReturnKind::IntArithAddReturn
                            : ReturnKind::IntArithSubReturn;
        sendIPOff = 2;
        arithOp = b2;
    } else if (bodyLen >= 4 && b2 == 0x68
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <a> * <b>  (SmallInt fast path with 128-bit OF check)
        kind = ReturnKind::IntArithMulReturn;
        sendIPOff = 2;
        arithOp = b2;
    } else if (bodyLen >= 4 && b2 >= 0x62 && b2 <= 0x67
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <a> cmp <b>  (0x62 < / 0x63 > / 0x64 <= / 0x65 >= / 0x66 = / 0x67 ~=)
        kind = ReturnKind::IntCmpReturn;
        sendIPOff = 2;
        arithOp = b2;
    } else if (bodyLen >= 4 && (b2 == 0x6E || b2 == 0x6F)
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <a> bitAnd: <b>  /  ^ <a> bitOr: <b>
        // Both operands tagged SmallInt; tag bit (1) is preserved by
        // both AND and OR — operate on tagged Oops directly.
        kind = ReturnKind::IntBitOpReturn;
        sendIPOff = 2;
        arithOp = b2;
    } else if (bodyLen >= 4 && SistaV1::isSend1(b2)
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <a> foo: <b>  — 1-arg send with inline IC probe.
        kind = ReturnKind::OneArgSendInlineIC;
        numPushes = 2;
        sendIPOff = 2;
    } else {
        // ZeroArgSendInlineIC stays gated — matches the arm64 sibling
        // which left it gated after measured perf regression on tier
        // interaction (see Tier2Compiler_arm64.cpp:412+).  Set
        // PHARO_T2_ZEROARG_IC=1 to opt in.
        static bool t2ZeroargIC =
            std::getenv("PHARO_T2_ZEROARG_IC") != nullptr;
        if (t2ZeroargIC && bodyLen >= 3 && SistaV1::isSend0(b1)
                        && b2 == SistaV1::ReturnTop
                        && decodePush(b0, pushes[0])) {
            kind = ReturnKind::ZeroArgSendInlineIC;
            numPushes = 1;
            sendIPOff = 1;
        } else {
            return bail("no pattern match");
        }
    }

    if (trace) fprintf(stderr,
                       "[T2-x86]   match kind=%d b0=0x%02x slotIndex=%d\n",
                       (int)kind, b0, slotIndex);

    // ---- Emit --------------------------------------------------------------
    using namespace asmjit;
    using namespace asmjit::x86;

    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());

    // PHARO_T2_X86_LOG=1 — dump asmjit IR + final machine code per
    // compile.  Heavy; only enable when actively debugging T2 emit.
    static asmjit::FileLogger* asmjitLogger = []() -> asmjit::FileLogger* {
        if (std::getenv("PHARO_T2_X86_LOG"))
            return new asmjit::FileLogger(stderr);
        return nullptr;
    }();
    if (asmjitLogger) code.set_logger(asmjitLogger);

    Compiler cc(&code);
    FuncNode* fn = cc.add_func(FuncSignature::build<void, void*>());
    Gp state = cc.new_gp64("state");
    fn->set_arg(0, state);

    // Helper: load a Push descriptor's value into a new gp64.  Used by
    // the IntArith / IntCmp / IntBitOp emit cases.
    auto loadPush = [&](const Push& p) -> Gp {
        Gp v = cc.new_gp64("pv");
        switch (p.src) {
        case PushSrc::Receiver:
            cc.mov(v, ptr(state, OFF_RECEIVER));
            break;
        case PushSrc::RecvVar: {
            Gp rp = cc.new_gp64("rp");
            cc.mov(rp, ptr(state, OFF_RECEIVER));
            cc.mov(v,  ptr(rp, OBJ_SLOT_0 + p.idx * 8));
            break;
        }
        case PushSrc::LitVar: {
            Gp lp = cc.new_gp64("lp");
            Gp as = cc.new_gp64("as");
            cc.mov(lp, ptr(state, OFF_LITERALS));
            cc.mov(as, ptr(lp, p.idx * 8));
            cc.mov(v,  ptr(as, ASSOC_VALUE));
            break;
        }
        case PushSrc::Temp: {
            Gp tp = cc.new_gp64("tp");
            cc.mov(tp, ptr(state, OFF_TEMPBASE));
            cc.mov(v,  ptr(tp, p.idx * 8));
            break;
        }
        case PushSrc::ImmOop:
            cc.mov(v, Imm(p.bits));
            break;
        case PushSrc::None:
            break;
        }
        return v;
    };

    // Helper: emit the bail-to-interpreter sequence for the SmallInt
    // fast-path kinds.  Push a + b onto state.sp, advance state.ip to
    // the arith bytecode, set sendArgCount=1, exit ExitSend.  The
    // interpreter resumes at the arith byte and dispatches via its own
    // SmallInt fast path / full send fallback.
    auto emitArithBail = [&](Gp a, Gp b) {
        Gp sp = cc.new_gp64("sp");
        cc.mov(sp, ptr(state, OFF_SP));
        cc.mov(ptr(sp, 0), a);
        cc.mov(ptr(sp, 8), b);
        cc.add(sp, Imm(16));
        cc.mov(ptr(state, OFF_SP), sp);

        Gp ip = cc.new_gp64("ip");
        cc.mov(ip, ptr(state, OFF_IP));
        cc.add(ip, Imm(sendIPOff));
        cc.mov(ptr(state, OFF_IP), ip);

        Gp w1 = cc.new_gp32("w1");
        cc.mov(w1, Imm(1));
        cc.mov(ptr(state, OFF_SENDARGCOUNT), w1);

        Gp zero64 = cc.new_gp64("zero64");
        cc.xor_(zero64, zero64);
        cc.mov(ptr(state, OFF_ICDATAPTR), zero64);

        Gp exitBail = cc.new_gp32("exitBail");
        cc.mov(exitBail, Imm(EXIT_SEND));
        cc.mov(ptr(state, OFF_EXIT), exitBail);
    };

    Gp retOop = cc.new_gp64("retOop");
    bool fullEmit = false;  // true → case below emits its own ret/end_func
    switch (kind) {
    case ReturnKind::Receiver:
        cc.mov(retOop, ptr(state, OFF_RECEIVER));
        break;
    case ReturnKind::True:
        cc.mov(retOop, ptr(state, OFF_TRUEOOP));
        break;
    case ReturnKind::False:
        cc.mov(retOop, ptr(state, OFF_FALSEOOP));
        break;
    case ReturnKind::NilImm:
    case ReturnKind::ImmediateOop:
        cc.mov(retOop, Imm(immBits));
        break;
    case ReturnKind::RecvVar: {
        Gp recvPtr = cc.new_gp64("recvPtr");
        cc.mov(recvPtr, ptr(state, OFF_RECEIVER));
        cc.mov(retOop,  ptr(recvPtr, OBJ_SLOT_0 + slotIndex * 8));
        break;
    }
    case ReturnKind::TempReturn: {
        Gp tempPtr = cc.new_gp64("tempPtr");
        cc.mov(tempPtr, ptr(state, OFF_TEMPBASE));
        cc.mov(retOop,  ptr(tempPtr, slotIndex * 8));
        break;
    }
    case ReturnKind::LitVar: {
        Gp litsPtr = cc.new_gp64("litsPtr");
        Gp assoc   = cc.new_gp64("assoc");
        cc.mov(litsPtr, ptr(state, OFF_LITERALS));
        cc.mov(assoc,   ptr(litsPtr, slotIndex * 8));
        cc.mov(retOop,  ptr(assoc, ASSOC_VALUE));
        break;
    }
    case ReturnKind::SetterRecvVar: {
        Gp recvPtr = cc.new_gp64("recvPtr");
        Gp tempPtr = cc.new_gp64("tempPtr");
        Gp argVal  = cc.new_gp64("argVal");
        cc.mov(recvPtr, ptr(state, OFF_RECEIVER));
        cc.mov(tempPtr, ptr(state, OFF_TEMPBASE));
        cc.mov(argVal,  ptr(tempPtr, 0));
        cc.mov(ptr(recvPtr, OBJ_SLOT_0 + slotIndex * 8), argVal);
        cc.mov(retOop, recvPtr);
        break;
    }
    case ReturnKind::InitRecvVar: {
        // ivar := <const>; ^ self
        Gp recvPtr = cc.new_gp64("recvPtr");
        Gp cstVal  = cc.new_gp64("cstVal");
        cc.mov(recvPtr, ptr(state, OFF_RECEIVER));
        cc.mov(cstVal,  Imm(immBits));
        cc.mov(ptr(recvPtr, OBJ_SLOT_0 + slotIndex * 8), cstVal);
        cc.mov(retOop, recvPtr);
        break;
    }

    case ReturnKind::IntArithAddReturn:
    case ReturnKind::IntArithSubReturn: {
        // SmallInt-tagged arithmetic.  Encoding: Oop = value*8 + 1.
        // For add we use (a-1) + b = a_val*8 + b_val*8 + 1 — already
        // tagged result.  For sub we use (a - b) + 1 = (a_val-b_val)*8 + 1.
        // Bail on tag mismatch (either operand non-SmI) or signed-64
        // overflow.  Mirrors arm64 sibling's commentary.
        Gp a = loadPush(pushes[0]);
        Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        // Tag-check both: low 3 bits must equal 1.
        Gp aTag = cc.new_gp64("aTag");
        cc.mov(aTag, a);
        cc.and_(aTag, Imm(7));
        cc.cmp(aTag, Imm(1));
        cc.jne(lblBail);
        Gp bTag = cc.new_gp64("bTag");
        cc.mov(bTag, b);
        cc.and_(bTag, Imm(7));
        cc.cmp(bTag, Imm(1));
        cc.jne(lblBail);

        Gp result = cc.new_gp64("result");
        cc.mov(result, a);
        if (kind == ReturnKind::IntArithAddReturn) {
            cc.sub(result, Imm(1));   // a-1 = a_val*8
            cc.add(result, b);        // (a_val+b_val)*8 + 1
        } else {
            cc.sub(result, b);        // (a_val-b_val)*8
            cc.add(result, Imm(1));   // (a_val-b_val)*8 + 1
        }
        cc.jo(lblBail);  // signed-64 overflow → bail

        // Fast path: store tagged result, EXIT_RETURN.
        cc.mov(ptr(state, OFF_RETVAL), result);
        Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, Imm(EXIT_RETURN));
        cc.mov(ptr(state, OFF_EXIT), exitOk);
        cc.jmp(lblDone);

        cc.bind(lblBail);
        emitArithBail(a, b);

        cc.bind(lblDone);
        cc.ret();
        cc.end_func();
        fullEmit = true;
        break;
    }

    case ReturnKind::IntArithMulReturn: {
        // ^ <a> * <b> — SmallInt multiply.  Untag both operands, signed
        // multiply, check int64 overflow via x86 OF flag (jo), check
        // SmallInt range via shl-3 / sar-3 round-trip, retag.
        Gp a = loadPush(pushes[0]);
        Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        Gp aTag = cc.new_gp64("aTag");
        cc.mov(aTag, a);
        cc.and_(aTag, Imm(7));
        cc.cmp(aTag, Imm(1));
        cc.jne(lblBail);
        Gp bTag = cc.new_gp64("bTag");
        cc.mov(bTag, b);
        cc.and_(bTag, Imm(7));
        cc.cmp(bTag, Imm(1));
        cc.jne(lblBail);

        // Untag (arithmetic shift right by 3).
        Gp prod = cc.new_gp64("prod");
        cc.mov(prod, a);
        cc.sar(prod, Imm(3));
        Gp bVal = cc.new_gp64("bVal");
        cc.mov(bVal, b);
        cc.sar(bVal, Imm(3));

        // Signed 64-bit multiply.  imul (2-op) sets OF if the int64
        // result doesn't fit signed.
        cc.imul(prod, bVal);
        cc.jo(lblBail);

        // SmallInt range check: prod * 8 must fit in int64 signed too
        // (i.e., bits 60..63 of prod must all be 0 or all be 1).
        // Verify via shl 3 / sar 3 round-trip — if the round-tripped
        // value differs, range was exceeded.
        Gp shifted = cc.new_gp64("shifted");
        cc.mov(shifted, prod);
        cc.shl(shifted, Imm(3));
        Gp back = cc.new_gp64("back");
        cc.mov(back, shifted);
        cc.sar(back, Imm(3));
        cc.cmp(back, prod);
        cc.jne(lblBail);

        // Retag (set bit 0).
        cc.or_(shifted, Imm(1));

        cc.mov(ptr(state, OFF_RETVAL), shifted);
        Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, Imm(EXIT_RETURN));
        cc.mov(ptr(state, OFF_EXIT), exitOk);
        cc.jmp(lblDone);

        cc.bind(lblBail);
        emitArithBail(a, b);

        cc.bind(lblDone);
        cc.ret();
        cc.end_func();
        fullEmit = true;
        break;
    }

    case ReturnKind::IntAccumRecvVar: {
        // ivar[M] := ivar[M] +/- <b>; ^ self.  5-byte sequence.
        // Load `a` from receiver.slot[M], `b` from pushes[1], do the
        // tagged add/sub with overflow, store back to ivar[M], return
        // receiver.  Bail mirrors the 4-byte arith path: push a + b,
        // bump ip by 2 (to arith byte), sendArgCount=1, ExitSend.
        Gp recvPtr = cc.new_gp64("recvPtr");
        cc.mov(recvPtr, ptr(state, OFF_RECEIVER));
        int slotOff = OBJ_SLOT_0 + slotIndex * 8;
        Gp a = cc.new_gp64("ivarA");
        cc.mov(a, ptr(recvPtr, slotOff));
        Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        Gp aTag = cc.new_gp64("aTag");
        cc.mov(aTag, a);
        cc.and_(aTag, Imm(7));
        cc.cmp(aTag, Imm(1));
        cc.jne(lblBail);
        Gp bTag = cc.new_gp64("bTag");
        cc.mov(bTag, b);
        cc.and_(bTag, Imm(7));
        cc.cmp(bTag, Imm(1));
        cc.jne(lblBail);

        Gp result = cc.new_gp64("accumResult");
        cc.mov(result, a);
        if (arithOp == 0x60) {
            cc.sub(result, Imm(1));
            cc.add(result, b);
        } else {
            cc.sub(result, b);
            cc.add(result, Imm(1));
        }
        cc.jo(lblBail);

        // Store result back to receiver.slot[M], then return receiver.
        cc.mov(ptr(recvPtr, slotOff), result);
        cc.mov(ptr(state, OFF_RETVAL), recvPtr);
        Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, Imm(EXIT_RETURN));
        cc.mov(ptr(state, OFF_EXIT), exitOk);
        cc.jmp(lblDone);

        cc.bind(lblBail);
        emitArithBail(a, b);

        cc.bind(lblDone);
        cc.ret();
        cc.end_func();
        fullEmit = true;
        break;
    }

    case ReturnKind::IntCmpReturn: {
        // ^ a cmp b — SmallInt comparison.  Tag check both, then cmp
        // the tagged values directly (signed order is preserved since
        // both operands are tag-1 values shifted by 3).  Materialize
        // trueOop / falseOop via jcc-based select.  The arm64 sibling
        // uses csel; on x86, SistaLowering_x86_64 also avoids cmov
        // due to register-allocator quirks — same pattern here.
        Gp a = loadPush(pushes[0]);
        Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        Gp aTag = cc.new_gp64("aTag");
        cc.mov(aTag, a);
        cc.and_(aTag, Imm(7));
        cc.cmp(aTag, Imm(1));
        cc.jne(lblBail);
        Gp bTag = cc.new_gp64("bTag");
        cc.mov(bTag, b);
        cc.and_(bTag, Imm(7));
        cc.cmp(bTag, Imm(1));
        cc.jne(lblBail);

        cc.cmp(a, b);
        Gp result = cc.new_gp64("cmpResult");
        // Branch-based select: result = (cond ? trueOop : falseOop).
        // Map the negated condition to the "skip" branch.
        cc.mov(result, ptr(state, OFF_FALSEOOP));
        Label lblSkip = cc.new_label();
        switch (arithOp) {
        case 0x62: cc.jge(lblSkip); break;   // < : skip if !( <)  → >=
        case 0x63: cc.jle(lblSkip); break;   // > : skip if !( >)  → <=
        case 0x64: cc.jg(lblSkip);  break;   // <=: skip if !(<=)  → >
        case 0x65: cc.jl(lblSkip);  break;   // >=: skip if !(>=)  → <
        case 0x66: cc.jne(lblSkip); break;   // = : skip if !( =)  → ~=
        case 0x67: cc.je(lblSkip);  break;   // ~=: skip if !(~=)  → =
        }
        cc.mov(result, ptr(state, OFF_TRUEOOP));
        cc.bind(lblSkip);

        cc.mov(ptr(state, OFF_RETVAL), result);
        Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, Imm(EXIT_RETURN));
        cc.mov(ptr(state, OFF_EXIT), exitOk);
        cc.jmp(lblDone);

        cc.bind(lblBail);
        emitArithBail(a, b);

        cc.bind(lblDone);
        cc.ret();
        cc.end_func();
        fullEmit = true;
        break;
    }

    case ReturnKind::IntBitOpReturn: {
        // ^ a bitAnd: b   /  ^ a bitOr: b
        // Tag bit (low bit = 1) is preserved by both AND and OR when
        // both operands have it set, so the tagged Oops can be combined
        // directly.
        Gp a = loadPush(pushes[0]);
        Gp b = loadPush(pushes[1]);

        Label lblBail = cc.new_label();
        Label lblDone = cc.new_label();

        Gp aTag = cc.new_gp64("aTag");
        cc.mov(aTag, a);
        cc.and_(aTag, Imm(7));
        cc.cmp(aTag, Imm(1));
        cc.jne(lblBail);
        Gp bTag = cc.new_gp64("bTag");
        cc.mov(bTag, b);
        cc.and_(bTag, Imm(7));
        cc.cmp(bTag, Imm(1));
        cc.jne(lblBail);

        Gp result = cc.new_gp64("result");
        cc.mov(result, a);
        if (arithOp == 0x6E)       cc.and_(result, b);  // bitAnd:
        else /* 0x6F */            cc.or_(result, b);   // bitOr:

        cc.mov(ptr(state, OFF_RETVAL), result);
        Gp exitOk = cc.new_gp32("exitOk");
        cc.mov(exitOk, Imm(EXIT_RETURN));
        cc.mov(ptr(state, OFF_EXIT), exitOk);
        cc.jmp(lblDone);

        cc.bind(lblBail);
        emitArithBail(a, b);

        cc.bind(lblDone);
        cc.ret();
        cc.end_func();
        fullEmit = true;
        break;
    }

    case ReturnKind::ZeroArgSendInlineIC:
    case ReturnKind::OneArgSendInlineIC: {
        // ^ <push0> [foo: <push1>] — 1 or 2 pushes + inline IC probe.
        //   hit:  cachedTarget = icData[N*3+1], exitReason = ExitSendCached
        //   miss: cachedTarget left alone, exitReason = ExitSend
        // Both paths set icDataPtr, sendArgCount=(numPushes-1),
        // ip += sendIPOff.  Mirrors arm64 sibling at Tier2Compiler
        // _arm64.cpp:1193+; the only x86-specific bits are jcc instead
        // of cbz/b_eq and the explicit mov-then-and 2-op forms.
        int sendByteOff = sendIPOff;
        uint8_t sendOp = bytes[bcStart + sendByteOff];
        uint8_t sendBase = (numPushes == 1) ? SistaV1::Send0Base
                                            : SistaV1::Send1Base;
        int selIdx = sendOp - sendBase;

        auto ic = std::make_unique<uint64_t[]>(IC_SLOTS);
        std::memset(ic.get(), 0, IC_SLOTS * sizeof(uint64_t));
        uint64_t icAddr = reinterpret_cast<uint64_t>(ic.get());
        icBuffers_.push_back(std::move(ic));
        // Seed selectorBits at slot 18 so runtime miss path can resolve.
        icBuffers_.back()[18] = methodObj->slotAt(1 + selIdx).rawBits();

        // Load receiver — keep two copies in independent virtual regs
        // so asmjit's RA doesn't alias the push and lookupKey uses
        // (fixed flakiness on arm64; safe to mirror here).
        Gp recvLoaded = loadPush(pushes[0]);
        Gp recvForPush = cc.new_gp64("recvForPush");
        Gp recvForKey  = cc.new_gp64("recvForKey");
        cc.mov(recvForPush, recvLoaded);
        cc.mov(recvForKey,  recvLoaded);

        Label lblHeap   = cc.new_label();
        Label lblCheck  = cc.new_label();
        Label lblMiss   = cc.new_label();
        Label lblHit[IC_ENTRIES];
        for (int i = 0; i < IC_ENTRIES; i++) lblHit[i] = cc.new_label();
        Label lblEpi    = cc.new_label();

        // Compute lookupKey:
        //   tag = recv & 7
        //   if tag == 0  → use header.classIndex (heap object)
        //   else         → use tag | 0x80000000   (immediate)
        Gp tag = cc.new_gp64("tag");
        Gp lookupKey = cc.new_gp64("lookupKey");
        cc.mov(tag, recvForKey);
        cc.and_(tag, Imm(7));
        cc.test(tag, tag);
        cc.jz(lblHeap);

        cc.mov(lookupKey, tag);
        // x86 `or r64, imm32` sign-extends imm32 → setting bit 31 from
        // a 32-bit imm would also set bits 32-63.  Materialize the OR
        // mask into a 64-bit reg first so asmjit picks `movabs` and
        // the upper bits stay zero.
        Gp keyOrMask = cc.new_gp64("keyOrMask");
        cc.mov(keyOrMask, Imm(0x80000000ULL));
        cc.or_(lookupKey, keyOrMask);
        cc.jmp(lblCheck);

        cc.bind(lblHeap);
        Gp header = cc.new_gp64("header");
        cc.mov(header, ptr(recvForKey, 0));
        cc.mov(lookupKey, header);
        cc.and_(lookupKey, Imm(CLASS_INDEX_MASK));

        cc.bind(lblCheck);
        // Push receiver + (any args) onto interp stack.
        Gp sp = cc.new_gp64("sp");
        cc.mov(sp, ptr(state, OFF_SP));
        cc.mov(ptr(sp, 0), recvForPush);
        cc.add(sp, Imm(8));
        if (numPushes >= 2) {
            Gp arg = loadPush(pushes[1]);
            cc.mov(ptr(sp, 0), arg);
            cc.add(sp, Imm(8));
        }
        cc.mov(ptr(state, OFF_SP), sp);

        Gp icData = cc.new_gp64("icData");
        cc.mov(icData, Imm(icAddr));

        // 6-way IC probe.  PHARO_T2_FORCE_MISS=1 skips it (forces miss
        // path) — matches arm64 diagnostic flag.
        static bool t2ForceMiss =
            std::getenv("PHARO_T2_FORCE_MISS") != nullptr;
        if (!t2ForceMiss) {
            for (int i = 0; i < IC_ENTRIES; i++) {
                Gp key = cc.new_gp64("key");
                cc.mov(key, ptr(icData, IC_KEY_OFF(i)));
                cc.cmp(lookupKey, key);
                cc.je(lblHit[i]);
            }
        }
        cc.jmp(lblMiss);

        // Hit paths: load method oop, set ExitSendCached, jump epilogue.
        Gp method   = cc.new_gp64("method");
        Gp exitCode = cc.new_gp32("exitCode");
        for (int i = 0; i < IC_ENTRIES; i++) {
            cc.bind(lblHit[i]);
            cc.mov(method, ptr(icData, IC_METHOD_OFF(i)));
            cc.mov(exitCode, Imm(EXIT_SEND_CACHED));
            cc.mov(ptr(state, OFF_CACHEDTARGET), method);
            cc.jmp(lblEpi);
        }

        // Miss path: ExitSend (runtime resolves via icData->selectorBits).
        cc.bind(lblMiss);
        cc.mov(exitCode, Imm(EXIT_SEND));

        // Shared epilogue: store icData ptr, sendArgCount, advance ip,
        // store exit code.  Then ret.
        cc.bind(lblEpi);
        cc.mov(ptr(state, OFF_ICDATAPTR), icData);
        Gp argCount = cc.new_gp32("argCount");
        cc.mov(argCount, Imm(numPushes - 1));
        cc.mov(ptr(state, OFF_SENDARGCOUNT), argCount);
        Gp ip = cc.new_gp64("ip");
        cc.mov(ip, ptr(state, OFF_IP));
        cc.add(ip, Imm(sendIPOff));
        cc.mov(ptr(state, OFF_IP), ip);
        cc.mov(ptr(state, OFF_EXIT), exitCode);

        cc.ret();
        cc.end_func();
        fullEmit = true;
        break;
    }
    }

    if (!fullEmit) {
        cc.mov(ptr(state, OFF_RETVAL), retOop);
        Gp exitVal = cc.new_gp32("exitVal");
        cc.mov(exitVal, Imm(EXIT_RETURN));
        cc.mov(ptr(state, OFF_EXIT), exitVal);
        cc.ret();
        cc.end_func();
    }

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

// Walk the full bytecode sequence of a method and emit asmjit code
// per op.  Two passes:
//   1. Validate — every op must be in the supported set, jumps must be
//      forward-or-validated-backward, methods must reach a return or a
//      bailable send.
//   2. Emit — straight linear walk over the bytecode, with labels at
//      jump targets (pre-allocated from the validation pass).
//
// Mirrors the arm64 sibling at Tier2Compiler_arm64.cpp:1462+; the
// only x86-specific bits are jcc-based-select-instead-of-cmov, the
// 2-op `mov dst, src; sar dst, imm` style for arith vs arm64's 3-op
// form, and the `mov keyOrMask, Imm(0x80000000); or lookupKey, keyOrMask`
// dance for the IC lookupKey OR mask (32-bit imm sign-extends on x86).
void* Tier2Compiler::tryCompileMultiBC(Oop compiledMethod,
                                        const uint8_t* bytes,
                                        size_t bcStart,
                                        size_t bodyLen) {
    if (bodyLen < 2) return nullptr;

    // ---- Pass 1: validate ----
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
    const bool jumpsEnabled = jumpsEnabledByEnv();
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
            i += mbcOpLen(op);
            continue;
        }
        if (isExtForwardJump(op) && jumpsEnabled) {
            if (i + 1 >= bodyLen) {
                g_mbcBailed++;
                return nullptr;
            }
            uint8_t offByte = bytes[bcStart + i + 1];
            size_t target = i + 2 + (size_t)offByte;
            if (target >= bodyLen || target <= i) {
                g_mbcBailed++;
                return nullptr;
            }
            jumpTargets.insert(target);
            i += 2;
            continue;
        }
        if (op == SistaV1::ExtendB && jumpsEnabled) {
            // ExtB <extByte> ExtJump <offByte> — backward unconditional
            // jump.  ExtB before any other op is unsupported here.
            if (i + 3 >= bodyLen) {
                g_mbcBailed++;
                return nullptr;
            }
            uint8_t extByte = bytes[bcStart + i + 1];
            uint8_t nextOp  = bytes[bcStart + i + 2];
            uint8_t offByte = bytes[bcStart + i + 3];
            if (nextOp != SistaV1::ExtJump) {
                g_mbcBailed++;
                return nullptr;
            }
            int extBVal = (extByte >= 128) ? ((int)extByte | ~0xFF) : (int)extByte;
            int offset = (extBVal << 8) | (int)offByte;
            int64_t targetSigned = (int64_t)i + 2 + 2 + offset;
            if (targetSigned < 0 || (size_t)targetSigned >= bodyLen) {
                g_mbcBailed++;
                return nullptr;
            }
            jumpTargets.insert((size_t)targetSigned);
            i += 4;
            continue;
        }
        if (isMBCSupported(op)) {
            if (op >= SistaV1::ReturnReceiver && op <= SistaV1::ReturnTop) {
                sawReturn = true;
                break;
            }
            i += mbcOpLen(op);
            continue;
        }
        if (isMBCBailableSend(op)) {
            // Sends are gated.  PHARO_T2_MBC_SENDS=1 → simple bail.
            // PHARO_T2_MBC_IC=1 → inline IC probe with hit/miss exit.
            // Default off — both regress IC hit rate when T1's warmup
            // is intercepted by T2 at this site.
            static bool t2MbcSends =
                std::getenv("PHARO_T2_MBC_SENDS") != nullptr;
            static bool t2MbcIC =
                std::getenv("PHARO_T2_MBC_IC") != nullptr;
            if (!t2MbcSends && !t2MbcIC) {
                g_mbcBailed++;
                return nullptr;
            }
            int nArgs = mbcSendArgCount(op);
            if (nArgs < 0) { g_mbcBailed++; return nullptr; }
            willBailAtSend = true;
            bailSendOffset = i;
            bailSendOp = op;
            (void)bailSendOp;
            break;
        }
        g_mbcBailed++;
        return nullptr;
    }

    if (!sawReturn && !willBailAtSend) {
        g_mbcBailed++;
        return nullptr;
    }

    ObjectHeader* methodObj =
        reinterpret_cast<ObjectHeader*>(compiledMethod.rawBits());
    uint64_t nilBits  = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits();
    uint64_t oneBits  = Oop::fromSmallInteger(1).rawBits();
    uint64_t zeroBits = Oop::fromSmallInteger(0).rawBits();
    int numLiterals = 0;
    {
        Oop hdrOop = methodObj->slotAt(0);
        if (hdrOop.isSmallInteger()) {
            numLiterals = (int)(hdrOop.asSmallInteger() & 0x7FFF);
        }
    }

    // ---- Pass 2: emit ----
    using namespace asmjit;
    using namespace asmjit::x86;

    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());
    static asmjit::FileLogger* asmjitLogger = []() -> asmjit::FileLogger* {
        if (std::getenv("PHARO_T2_X86_LOG"))
            return new asmjit::FileLogger(stderr);
        return nullptr;
    }();
    if (asmjitLogger) code.set_logger(asmjitLogger);

    Compiler cc(&code);
    FuncNode* fn = cc.add_func(FuncSignature::build<void, void*>());
    Gp state = cc.new_gp64("state");
    fn->set_arg(0, state);

    // Live sp: loaded once at entry, written back to state.sp on every
    // arith bail / send bail / return (plus before back-edge yield).
    Gp sp = cc.new_gp64("sp");
    cc.mov(sp, ptr(state, OFF_SP));

    auto emitPush = [&](Gp v) {
        cc.mov(ptr(sp, 0), v);
        cc.add(sp, Imm(8));
    };
    auto emitPop = [&]() -> Gp {
        cc.sub(sp, Imm(8));
        Gp v = cc.new_gp64("popV");
        cc.mov(v, ptr(sp, 0));
        return v;
    };

    Label lblBailTail = cc.new_label();
    Gp bailIPOff = cc.new_gp64("bailIPOff");
    Label lblFuncEnd = cc.new_label();

    std::map<size_t, Label> offsetLabels;
    for (size_t t : jumpTargets) offsetLabels[t] = cc.new_label();

    bool emittedReturn = false;
    bool emittedSendBail = false;

    for (size_t i = 0; i < bodyLen && !emittedReturn && !emittedSendBail;) {
        uint8_t op = bytes[bcStart + i];
        size_t opStart = i;

        {
            auto it = offsetLabels.find(i);
            if (it != offsetLabels.end()) cc.bind(it->second);
        }

        // ---- bailable send (gated; mirrors arm64 1699-1848) ----
        if (willBailAtSend && i == bailSendOffset) {
            cc.mov(ptr(state, OFF_SP), sp);

            int nArgs = mbcSendArgCount(op);

            Gp ip = cc.new_gp64("sendIp");
            cc.mov(ip, ptr(state, OFF_IP));
            cc.add(ip, Imm(static_cast<int>(i)));
            cc.mov(ptr(state, OFF_IP), ip);

            Gp wArgs = cc.new_gp32("wArgs");
            cc.mov(wArgs, Imm(nArgs));
            cc.mov(ptr(state, OFF_SENDARGCOUNT), wArgs);

            static bool t2MbcSends =
                std::getenv("PHARO_T2_MBC_SENDS") != nullptr;
            if (t2MbcSends) {
                Gp zero64 = cc.new_gp64("zero64");
                cc.xor_(zero64, zero64);
                cc.mov(ptr(state, OFF_ICDATAPTR), zero64);
                Gp exitV = cc.new_gp32("exitV");
                cc.mov(exitV, Imm(EXIT_SEND));
                cc.mov(ptr(state, OFF_EXIT), exitV);
                emittedSendBail = true;
                break;
            }

            // Inline-IC mode (PHARO_T2_MBC_IC=1).  Try shared-IC with T1
            // first; private buffer fallback.  Mirrors arm64 1742-1787.
            uint64_t icAddr = 0;
            if (t1Compiler_) {
                const std::vector<uint16_t>* sites =
                    t1Compiler_->getSendSiteBCOffsets(compiledMethod.rawBits());
                JITMethod* t1Method = methodMap_.lookup(compiledMethod.rawBits());
                if (sites && t1Method && t1Method->numICEntries > 0) {
                    uint16_t sendIdx = UINT16_MAX;
                    for (size_t s = 0; s < sites->size(); s++) {
                        if ((*sites)[s] == (uint16_t)i) {
                            sendIdx = static_cast<uint16_t>(s);
                            break;
                        }
                    }
                    if (sendIdx < t1Method->numICEntries) {
                        uint8_t* icStart = t1Method->icZoneStart();
                        uint8_t* icBase  = icStart +
                                            sendIdx * IC_BYTES_PER_SITE;
                        icAddr = reinterpret_cast<uint64_t>(icBase);
                    }
                }
            }
            if (icAddr == 0) {
                auto ic = std::make_unique<uint64_t[]>(IC_SLOTS);
                std::memset(ic.get(), 0, IC_SLOTS * sizeof(uint64_t));
                icAddr = reinterpret_cast<uint64_t>(ic.get());
                icBuffers_.push_back(std::move(ic));
                int sendBase = 0x80;
                if (op >= 0x90 && op <= 0x9F) sendBase = 0x90;
                else if (op >= 0xA0 && op <= 0xAF) sendBase = 0xA0;
                int selIdx = op - sendBase;
                if (selIdx >= 0 && selIdx < numLiterals) {
                    icBuffers_.back()[18] =
                        methodObj->slotAt(1 + selIdx).rawBits();
                }
            }

            // Get receiver from stack: sp[-(nArgs+1)*8].
            Gp recv = cc.new_gp64("recvIC");
            cc.mov(recv, ptr(sp, -(nArgs + 1) * 8));

            Label lblHeap   = cc.new_label();
            Label lblCheck  = cc.new_label();
            Label lblMiss   = cc.new_label();
            Label lblEpi    = cc.new_label();
            Label lblHit[IC_ENTRIES];
            for (int k = 0; k < IC_ENTRIES; k++) lblHit[k] = cc.new_label();

            Gp tag = cc.new_gp64("tag");
            Gp lookupKey = cc.new_gp64("lookupKey");
            cc.mov(tag, recv);
            cc.and_(tag, Imm(7));
            cc.test(tag, tag);
            cc.jz(lblHeap);
            cc.mov(lookupKey, tag);
            // 32-bit imm sign-extends on `or r64, imm32`; materialize.
            Gp keyOrMask = cc.new_gp64("keyOrMask");
            cc.mov(keyOrMask, Imm(0x80000000ULL));
            cc.or_(lookupKey, keyOrMask);
            cc.jmp(lblCheck);

            cc.bind(lblHeap);
            cc.test(recv, recv);
            cc.jz(lblMiss);   // nil → bail
            Gp header = cc.new_gp64("hdr");
            cc.mov(header, ptr(recv, 0));
            cc.mov(lookupKey, header);
            cc.and_(lookupKey, Imm(CLASS_INDEX_MASK));

            cc.bind(lblCheck);
            Gp icData = cc.new_gp64("icData");
            cc.mov(icData, Imm(icAddr));
            for (int k = 0; k < IC_ENTRIES; k++) {
                Gp key = cc.new_gp64("key");
                cc.mov(key, ptr(icData, IC_KEY_OFF(k)));
                cc.cmp(lookupKey, key);
                cc.je(lblHit[k]);
            }
            cc.jmp(lblMiss);

            Gp exitCode = cc.new_gp32("exitIC");
            for (int k = 0; k < IC_ENTRIES; k++) {
                cc.bind(lblHit[k]);
                Gp method = cc.new_gp64("methodIC");
                cc.mov(method, ptr(icData, IC_METHOD_OFF(k)));
                cc.mov(ptr(state, OFF_CACHEDTARGET), method);
                cc.mov(exitCode, Imm(EXIT_SEND_CACHED));
                cc.jmp(lblEpi);
            }

            cc.bind(lblMiss);
            cc.mov(exitCode, Imm(EXIT_SEND));

            cc.bind(lblEpi);
            cc.mov(ptr(state, OFF_ICDATAPTR), icData);
            cc.mov(ptr(state, OFF_EXIT), exitCode);

            emittedSendBail = true;
            break;
        }

        // ---- pushes ----
        if (op >= SistaV1::PushRecvVarBase && op <= SistaV1::PushRecvVarLast) {
            int idx = op - SistaV1::PushRecvVarBase;
            Gp recv = cc.new_gp64("recv");
            cc.mov(recv, ptr(state, OFF_RECEIVER));
            Gp v = cc.new_gp64("v");
            cc.mov(v, ptr(recv, OBJ_SLOT_0 + idx * 8));
            emitPush(v);
        }
        else if (op >= SistaV1::PushLitVarBase && op <= SistaV1::PushLitVarLast) {
            int idx = op - SistaV1::PushLitVarBase;
            Gp lp = cc.new_gp64("lp");
            cc.mov(lp, ptr(state, OFF_LITERALS));
            Gp as = cc.new_gp64("as");
            cc.mov(as, ptr(lp, idx * 8));
            Gp v = cc.new_gp64("v");
            cc.mov(v, ptr(as, ASSOC_VALUE));
            emitPush(v);
        }
        else if (op >= SistaV1::PushLitConstBase && op <= SistaV1::PushLitConstLast) {
            int idx = op - SistaV1::PushLitConstBase;
            Oop lit = methodObj->slotAt(1 + idx);
            Gp v = cc.new_gp64("v");
            cc.mov(v, Imm(lit.rawBits()));
            emitPush(v);
        }
        else if (op >= SistaV1::PushTempBase && op <= SistaV1::PushTempLast) {
            int idx = op - SistaV1::PushTempBase;
            Gp tp = cc.new_gp64("tp");
            cc.mov(tp, ptr(state, OFF_TEMPBASE));
            Gp v = cc.new_gp64("v");
            cc.mov(v, ptr(tp, idx * 8));
            emitPush(v);
        }
        else if (op == SistaV1::PushReceiver) {
            Gp v = cc.new_gp64("v");
            cc.mov(v, ptr(state, OFF_RECEIVER));
            emitPush(v);
        }
        else if (op == SistaV1::PushTrue) {
            Gp v = cc.new_gp64("v");
            cc.mov(v, ptr(state, OFF_TRUEOOP));
            emitPush(v);
        }
        else if (op == SistaV1::PushFalse) {
            Gp v = cc.new_gp64("v");
            cc.mov(v, ptr(state, OFF_FALSEOOP));
            emitPush(v);
        }
        else if (op == SistaV1::PushNil) {
            Gp v = cc.new_gp64("v");
            cc.mov(v, Imm(nilBits));
            emitPush(v);
        }
        else if (op == SistaV1::PushZero) {
            Gp v = cc.new_gp64("v");
            cc.mov(v, Imm(zeroBits));
            emitPush(v);
        }
        else if (op == SistaV1::PushOne) {
            Gp v = cc.new_gp64("v");
            cc.mov(v, Imm(oneBits));
            emitPush(v);
        }
        else if (op == SistaV1::Dup) {
            Gp v = cc.new_gp64("dupv");
            cc.mov(v, ptr(sp, -8));
            emitPush(v);
        }
        // ---- arith (0x60-0x6F) ----
        else if (op >= 0x60 && op <= 0x6F) {
            cc.mov(ptr(state, OFF_SP), sp);

            Gp b = cc.new_gp64("b");
            Gp a = cc.new_gp64("a");
            cc.mov(b, ptr(sp, -8));
            cc.mov(a, ptr(sp, -16));

            Label lblLocalBail = cc.new_label();

            Gp tA = cc.new_gp64("tA");
            cc.mov(tA, a);
            cc.and_(tA, Imm(7));
            cc.cmp(tA, Imm(1));
            cc.jne(lblLocalBail);
            Gp tB = cc.new_gp64("tB");
            cc.mov(tB, b);
            cc.and_(tB, Imm(7));
            cc.cmp(tB, Imm(1));
            cc.jne(lblLocalBail);

            Gp result = cc.new_gp64("result");
            bool isComp = (op >= 0x62 && op <= 0x67);
            bool isBit  = (op == 0x6E || op == 0x6F);

            if (op == 0x60) {
                cc.mov(result, a);
                cc.sub(result, Imm(1));
                cc.add(result, b);
                cc.jo(lblLocalBail);
            }
            else if (op == 0x61) {
                cc.mov(result, a);
                cc.sub(result, b);
                cc.add(result, Imm(1));
                cc.jo(lblLocalBail);
            }
            else if (op == 0x68) {
                Gp prod = cc.new_gp64("prod");
                cc.mov(prod, a);
                cc.sar(prod, Imm(3));
                Gp bVal = cc.new_gp64("bVal");
                cc.mov(bVal, b);
                cc.sar(bVal, Imm(3));
                cc.imul(prod, bVal);
                cc.jo(lblLocalBail);
                Gp shifted = cc.new_gp64("shifted");
                cc.mov(shifted, prod);
                cc.shl(shifted, Imm(3));
                Gp back = cc.new_gp64("back");
                cc.mov(back, shifted);
                cc.sar(back, Imm(3));
                cc.cmp(back, prod);
                cc.jne(lblLocalBail);
                cc.or_(shifted, Imm(1));
                cc.mov(result, shifted);
            }
            else if (isBit) {
                cc.mov(result, a);
                if (op == 0x6E) cc.and_(result, b);
                else            cc.or_(result, b);
            }
            else if (isComp) {
                cc.cmp(a, b);
                cc.mov(result, ptr(state, OFF_FALSEOOP));
                Label lblSkip = cc.new_label();
                switch (op) {
                case 0x62: cc.jge(lblSkip); break;   // <  → skip if !( <)
                case 0x63: cc.jle(lblSkip); break;   // >  → skip if !( >)
                case 0x64: cc.jg(lblSkip);  break;   // <= → skip if !(<=)
                case 0x65: cc.jl(lblSkip);  break;   // >= → skip if !(>=)
                case 0x66: cc.jne(lblSkip); break;   // =  → skip if !( =)
                case 0x67: cc.je(lblSkip);  break;   // ~= → skip if !(~=)
                }
                cc.mov(result, ptr(state, OFF_TRUEOOP));
                cc.bind(lblSkip);
            }
            else {
                // Unsupported arith op (/, //, \\, @, bitShift:) — bail.
                cc.jmp(lblLocalBail);
                cc.bind(lblLocalBail);
                cc.mov(bailIPOff, Imm(static_cast<int>(opStart)));
                cc.jmp(lblBailTail);
                i += mbcOpLen(op);
                continue;
            }

            // Replace top 2 stack entries with 1 result.
            cc.sub(sp, Imm(8));
            cc.mov(ptr(sp, -8), result);
            Label lblArithDone = cc.new_label();
            cc.jmp(lblArithDone);

            cc.bind(lblLocalBail);
            cc.mov(bailIPOff, Imm(static_cast<int>(opStart)));
            cc.jmp(lblBailTail);

            cc.bind(lblArithDone);
        }
        // ---- stores + pop ----
        else if (op >= SistaV1::PopStoreRecvBase && op <= SistaV1::PopStoreRecvLast) {
            int idx = op - SistaV1::PopStoreRecvBase;
            Gp v = emitPop();
            Gp recv = cc.new_gp64("recv");
            cc.mov(recv, ptr(state, OFF_RECEIVER));
            cc.mov(ptr(recv, OBJ_SLOT_0 + idx * 8), v);
        }
        else if (op >= SistaV1::PopStoreTempBase && op <= SistaV1::PopStoreTempLast) {
            int idx = op - SistaV1::PopStoreTempBase;
            Gp v = emitPop();
            Gp tp = cc.new_gp64("tp");
            cc.mov(tp, ptr(state, OFF_TEMPBASE));
            cc.mov(ptr(tp, idx * 8), v);
        }
        else if (op == SistaV1::Pop) {
            cc.sub(sp, Imm(8));
        }
        // ---- returns ----
        else if (op >= SistaV1::ReturnReceiver && op <= SistaV1::ReturnTop) {
            cc.mov(ptr(state, OFF_SP), sp);
            Gp ret = cc.new_gp64("ret");
            switch (op) {
            case SistaV1::ReturnReceiver:
                cc.mov(ret, ptr(state, OFF_RECEIVER));
                break;
            case SistaV1::ReturnTrue:
                cc.mov(ret, ptr(state, OFF_TRUEOOP));
                break;
            case SistaV1::ReturnFalse:
                cc.mov(ret, ptr(state, OFF_FALSEOOP));
                break;
            case SistaV1::ReturnNil:
                cc.mov(ret, Imm(nilBits));
                break;
            case SistaV1::ReturnTop:
                cc.mov(ret, ptr(sp, -8));
                break;
            }
            cc.mov(ptr(state, OFF_RETVAL), ret);
            Gp exitCode = cc.new_gp32("exit");
            cc.mov(exitCode, Imm(EXIT_RETURN));
            cc.mov(ptr(state, OFF_EXIT), exitCode);
            emittedReturn = true;
        }
        // ---- short jumps ----
        else if (SistaV1::isShortJump(op)) {
            size_t target = i + 2 + (size_t)(op - SistaV1::ShortJumpBase);
            cc.jmp(offsetLabels.at(target));
        }
        else if (SistaV1::isConditionalShortJump(op) ||
                 op == SistaV1::ExtJumpTrue ||
                 op == SistaV1::ExtJumpFalse) {
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
                jumpIfTrue = (op == SistaV1::ExtJumpTrue);
                target = i + 2 + (size_t)bytes[bcStart + i + 1];
                opLen = 2;
            }

            cc.mov(ptr(state, OFF_SP), sp);

            Gp v = cc.new_gp64("jv");
            cc.mov(v, ptr(sp, -8));
            Gp tOop = cc.new_gp64("tOop");
            Gp fOop = cc.new_gp64("fOop");
            cc.mov(tOop, ptr(state, OFF_TRUEOOP));
            cc.mov(fOop, ptr(state, OFF_FALSEOOP));

            Label lblJumpTaken   = cc.new_label();
            Label lblFallThrough = cc.new_label();
            Label lblNonBool     = cc.new_label();

            cc.cmp(v, jumpIfTrue ? tOop : fOop);
            cc.je(lblJumpTaken);
            cc.cmp(v, jumpIfTrue ? fOop : tOop);
            cc.jne(lblNonBool);

            cc.sub(sp, Imm(8));
            cc.jmp(lblFallThrough);

            cc.bind(lblJumpTaken);
            cc.sub(sp, Imm(8));
            cc.jmp(offsetLabels.at(target));

            cc.bind(lblNonBool);
            cc.mov(bailIPOff, Imm(static_cast<int>(opStart)));
            cc.jmp(lblBailTail);

            cc.bind(lblFallThrough);

            if (opLen == 2) {
                i += 2;
                continue;
            }
        }
        else if (op == SistaV1::ExtJump) {
            size_t target = i + 2 + (size_t)bytes[bcStart + i + 1];
            cc.jmp(offsetLabels.at(target));
            i += 2;
            continue;
        }
        else if (op == SistaV1::ExtendB) {
            // ExtB <extByte> ExtJump <offByte> — backward unconditional
            // jump with yield-countdown.
            uint8_t extByte = bytes[bcStart + i + 1];
            uint8_t offByte = bytes[bcStart + i + 3];
            int extBVal = (extByte >= 128) ? ((int)extByte | ~0xFF) : (int)extByte;
            int offset = (extBVal << 8) | (int)offByte;
            size_t target = (size_t)((int64_t)i + 2 + 2 + offset);

            cc.mov(ptr(state, OFF_SP), sp);

            // yieldCountdown-- ; if <= 0 → ExitYield.
            Gp yc = cc.new_gp32("yc");
            cc.mov(yc, ptr(state, OFF_YIELDCD));
            cc.sub(yc, Imm(1));
            cc.mov(ptr(state, OFF_YIELDCD), yc);
            Label lblYieldBail = cc.new_label();
            cc.jle(lblYieldBail);

            cc.jmp(offsetLabels.at(target));

            cc.bind(lblYieldBail);
            Gp ipYld = cc.new_gp64("ipYld");
            cc.mov(ipYld, ptr(state, OFF_IP));
            cc.add(ipYld, Imm(static_cast<int>(target)));
            cc.mov(ptr(state, OFF_IP), ipYld);
            Gp exitY = cc.new_gp32("exitY");
            cc.mov(exitY, Imm(EXIT_YIELD));
            cc.mov(ptr(state, OFF_EXIT), exitY);
            cc.jmp(lblFuncEnd);
            i += 4;
            continue;
        }
        else {
            // Should have been caught in pass 1.
            g_mbcBailed++;
            return nullptr;
        }

        i += mbcOpLen(op);
    }

    // Skip past the bail tail on normal flow.
    cc.jmp(lblFuncEnd);

    cc.bind(lblBailTail);
    Gp ip = cc.new_gp64("ip");
    cc.mov(ip, ptr(state, OFF_IP));
    cc.add(ip, bailIPOff);
    cc.mov(ptr(state, OFF_IP), ip);

    Gp sendArgs = cc.new_gp32("sendArgs");
    cc.mov(sendArgs, Imm(1));   // arith / mustBeBoolean is 1-arg
    cc.mov(ptr(state, OFF_SENDARGCOUNT), sendArgs);

    Gp zero64 = cc.new_gp64("zero64");
    cc.xor_(zero64, zero64);
    cc.mov(ptr(state, OFF_ICDATAPTR), zero64);

    Gp exitBail = cc.new_gp32("exitBail");
    cc.mov(exitBail, Imm(EXIT_SEND));
    cc.mov(ptr(state, OFF_EXIT), exitBail);

    cc.bind(lblFuncEnd);
    cc.ret();
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

}  // namespace jit
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
