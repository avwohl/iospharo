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
size_t   g_compiled = 0;
size_t   g_bailed   = 0;
uint64_t g_icHit    = 0;
uint64_t g_icMiss   = 0;
} // namespace

void Tier2Compiler::dumpBailStats() {
    uint64_t icTot = g_icHit + g_icMiss;
    fprintf(stderr,
        "  T2 (asmjit): compiled=%zu bailed=%zu | IC %llu/%llu (%.1f%% hit)\n",
        g_compiled, g_bailed,
        (unsigned long long)g_icHit, (unsigned long long)icTot,
        icTot ? 100.0 * g_icHit / icTot : 0.0);
}

void Tier2Compiler::flushAllICs() {
    // Zero every T2 IC entry — next send re-populates via the
    // interpreter's pendingICPatch_ mechanism.  Called from
    // JITRuntime::recoverAfterGC because methodBits in IC entries
    // are raw Oops that can become stale after compaction.
    for (auto& buf : icBuffers_) {
        std::memset(buf.data(), 0, buf.size() * sizeof(uint64_t));
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
        SelfSendInlineIC    // ^ self foo — push self + inline IC probe + exit Cached/Send
    };
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
    } else if (bodyLen >= 3 && b0 == SistaV1::PushReceiver
                            && SistaV1::isSend0(b1)
                            && b2 == SistaV1::ReturnTop) {
        // ^ self foo — push self + emit an inline 6-way IC probe;
        // on hit exit ExitSendCached, on miss exit ExitSend.  Stays
        // in native code on the hit path.
        kind = ReturnKind::SelfSendInlineIC;
        sendIPOff = 1;
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
    case ReturnKind::SelfSendInlineIC: {
        // ^ self foo — push self + inline IC probe.
        //   hit:  cachedTarget = icData[N*3+1], exitReason = ExitSendCached
        //   miss: cachedTarget left alone, exitReason = ExitSend
        // Both paths set icDataPtr, sendArgCount=0, ip += 1.
        // We use a shared epilogue and an `exitReason` register to
        // avoid duplicating the state-flush code.

        // Allocate IC buffer (152 bytes) and seed icData[18] with the
        // send's selector.  The chain loop in Interpreter.cpp reads
        // the selector from this slot on bail — if it's 0 we get
        // early-return and the IC never populates.
        icBuffers_.emplace_back(IC_SLOTS, uint64_t{0});
        uint64_t icAddr = reinterpret_cast<uint64_t>(icBuffers_.back().data());
        {
            // Selector is at literal index (b1 - 0x80) for Send0[N].
            int selIdx = bytes[bcStart + 1] - SistaV1::Send0Base;
            Oop sel = methodObj->slotAt(1 + selIdx);
            icBuffers_.back()[18] = sel.rawBits();
        }

        // Push receiver
        a64::Gp sp   = cc.new_gp64("sp");
        a64::Gp recv = cc.new_gp64("recv");
        cc.ldr(sp,   a64::ptr(statePtr, OFF_SP));
        cc.ldr(recv, a64::ptr(statePtr, OFF_RECEIVER));
        cc.str(recv, a64::ptr(sp));
        cc.add(sp, sp, 8);
        cc.str(sp, a64::ptr(statePtr, OFF_SP));

        // Compute lookupKey
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

        // Immediate receiver: lookupKey = tag | 0x80000000
        cc.mov(lookupKey, tag);
        cc.orr(lookupKey, lookupKey, asmjit::Imm(0x80000000ULL));
        cc.b(lblCheck);

        cc.bind(lblHeap);
        cc.cbz(recv, lblMiss);                     // nil receiver → full lookup
        a64::Gp header = cc.new_gp64("header");
        cc.ldr(header, a64::ptr(recv));
        cc.and_(lookupKey, header, asmjit::Imm(CLASS_INDEX_MASK));

        cc.bind(lblCheck);
        a64::Gp icData = cc.new_gp64("icData");
        cc.mov(icData, asmjit::Imm(icAddr));

        // 6-way probe
        for (int i = 0; i < IC_ENTRIES; i++) {
            a64::Gp key = cc.new_gp64("key");
            cc.ldr(key, a64::ptr(icData, IC_KEY_OFF(i)));
            cc.cmp(lookupKey, key);
            cc.b_eq(lblHit[i]);
        }
        cc.b(lblMiss);

        // Hit paths: load method, set exitReason = ExitSendCached,
        // jump to epilogue
        a64::Gp method   = cc.new_gp64("method");
        a64::Gp exitCode = cc.new_gp32("exitCode");
        for (int i = 0; i < IC_ENTRIES; i++) {
            cc.bind(lblHit[i]);
            cc.ldr(method, a64::ptr(icData, IC_METHOD_OFF(i)));
            cc.mov(exitCode, EXIT_SEND_CACHED);
            cc.str(method, a64::ptr(statePtr, OFF_CACHEDTARGET));
            // bump g_icHit (non-atomic; single-threaded for now)
            {
                a64::Gp ctrAddr = cc.new_gp64("ctrAddr");
                a64::Gp ctrVal  = cc.new_gp64("ctrVal");
                cc.mov(ctrAddr, asmjit::Imm(reinterpret_cast<uint64_t>(&g_icHit)));
                cc.ldr(ctrVal, a64::ptr(ctrAddr));
                cc.add(ctrVal, ctrVal, 1);
                cc.str(ctrVal, a64::ptr(ctrAddr));
            }
            cc.b(lblEpi);
        }

        // Miss: exitReason = ExitSend; fall through to epilogue.
        cc.bind(lblMiss);
        cc.mov(exitCode, EXIT_SEND);
        {
            a64::Gp ctrAddr = cc.new_gp64("ctrAddr");
            a64::Gp ctrVal  = cc.new_gp64("ctrVal");
            cc.mov(ctrAddr, asmjit::Imm(reinterpret_cast<uint64_t>(&g_icMiss)));
            cc.ldr(ctrVal, a64::ptr(ctrAddr));
            cc.add(ctrVal, ctrVal, 1);
            cc.str(ctrVal, a64::ptr(ctrAddr));
        }

        cc.bind(lblEpi);
        cc.str(icData, a64::ptr(statePtr, OFF_ICDATAPTR));
        a64::Gp w0 = cc.new_gp32("argCount");
        cc.mov(w0, 0);
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

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
