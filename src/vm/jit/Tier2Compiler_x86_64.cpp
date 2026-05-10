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
 *     <pushA> <pushB> + (0x62..0x67) + ReturnTop (^ a cmp b — six ops)
 *     <pushA> <pushB> + (0x6E/0x6F) + ReturnTop  (^ a bitAnd:/bitOr: b)
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
#include "JITMethod.hpp"
#include "SistaV1.hpp"
#include "../ObjectMemory.hpp"

#include <cstdio>
#include <cstring>

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
constexpr int OFF_ICDATAPTR    = 96;
constexpr int OFF_SENDARGCOUNT = 104;
constexpr int OFF_TRUEOOP      = 128;
constexpr int OFF_FALSEOOP     = 136;

// Object slot layout: ObjectHeader is 8 bytes; slot[N] starts at
// byte 8 + N*8.  Association.value is slot[1].
constexpr int OBJ_SLOT_0  = 8;
constexpr int ASSOC_VALUE = 16;

// ExitReason values (JITState.hpp).
constexpr int EXIT_RETURN = 1;
constexpr int EXIT_SEND   = 2;

// IC data layout (mirror JITMethod.hpp).  Used by flushAllICs().
constexpr int IC_ENTRIES = 6;
constexpr int IC_SLOTS   = IC_ENTRIES * 3 + 1;

// Bail / compile counters.  Per-arch (matches the SistaLowering
// per-arch g_lowerStats pattern).
size_t g_compiled = 0;
size_t g_bailed   = 0;

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
    IntCmpReturn,        // ^ a cmp b (SmallInt; 6 ops)
    IntBitOpReturn,      // ^ a bitAnd:/bitOr: b
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
    Push pushes[2];               // IntArith / IntCmp / IntBitOp operands
    int sendIPOff   = 0;          // bail-to-interp: byte offset of arith op
    int arithOp     = 0;          // SistaV1 arith bytecode (0x60..0x6F)

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
    } else if (bodyLen >= 4 && (b2 == 0x60 || b2 == 0x61)
                            && bytes[bcStart + 3] == SistaV1::ReturnTop
                            && decodePush(b0, pushes[0])
                            && decodePush(b1, pushes[1])) {
        // ^ <a> + <b>  or  ^ <a> - <b>  (SmallInt fast path)
        kind = (b2 == 0x60) ? ReturnKind::IntArithAddReturn
                            : ReturnKind::IntArithSubReturn;
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
    } else {
        return bail("no pattern match");
    }

    if (trace) fprintf(stderr,
                       "[T2-x86]   match kind=%d b0=0x%02x slotIndex=%d\n",
                       (int)kind, b0, slotIndex);

    // ---- Emit --------------------------------------------------------------
    using namespace asmjit;
    using namespace asmjit::x86;

    CodeHolder code;
    code.init(runtime_->environment(), runtime_->cpu_features());

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

void* Tier2Compiler::tryCompileMultiBC(Oop compiledMethod,
                                        const uint8_t* bytes,
                                        size_t bcStart,
                                        size_t bodyLen) {
    (void)compiledMethod;
    (void)bytes;
    (void)bcStart;
    (void)bodyLen;
    return nullptr;
}

}  // namespace jit
}  // namespace pharo

#endif  // PHARO_JIT_ENABLED
