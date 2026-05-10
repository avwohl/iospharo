/*
 * Tier2Compiler_x86_64.cpp - asmjit-based optimizing JIT, x86_64 sibling
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Per-arch sibling of Tier2Compiler_arm64.cpp.  Selected by CMakeLists
 * on non-arm64 hosts (Linux x86_64, macOS x86_64).
 *
 * STATUS — op-by-op port in progress.  Pattern-matched return-shape
 * methods supported:
 *
 *   ReturnReceiver / ReturnTrue / ReturnFalse / ReturnNil   (1 byte)
 *   PushReceiver|PushTrue|PushFalse|PushNil + ReturnTop     (2 bytes)
 *   PushZero|PushOne + ReturnTop                            (2 bytes)
 *   PushRecvVar(N) + ReturnTop                              (2 bytes; getter)
 *   PushTemp(N) + ReturnTop                                 (2 bytes; ^ tempN)
 *   PushLitConst(N) + ReturnTop                             (2 bytes; constant baked in)
 *   PushLitVar(N) + ReturnTop                               (2 bytes; Association.value)
 *   PushTemp(0) + PopStoreRecv(M) + ReturnReceiver          (3 bytes; setter of arg 0)
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
constexpr int OFF_RECEIVER = 8;
constexpr int OFF_LITERALS = 16;
constexpr int OFF_TEMPBASE = 24;
constexpr int OFF_EXIT     = 76;
constexpr int OFF_RETVAL   = 80;
constexpr int OFF_TRUEOOP  = 128;
constexpr int OFF_FALSEOOP = 136;

// Object slot layout: ObjectHeader is 8 bytes; slot[N] starts at
// byte 8 + N*8.  Association.value is slot[1].
constexpr int OBJ_SLOT_0  = 8;
constexpr int ASSOC_VALUE = 16;

// ExitReason values (JITState.hpp).
constexpr int EXIT_RETURN = 1;

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
    Receiver,        // ^ self            — load state.receiver
    True,            // ^ true            — load state.trueOop
    False,           // ^ false           — load state.falseOop
    NilImm,          // ^ nil             — bake nil bits
    ImmediateOop,    // ^ <const>         — bake immBits (PushZero/One/LitConst)
    RecvVar,         // ^ instVar[N]      — load receiver.slot[N]
    TempReturn,      // ^ tempN           — load tempBase[N]
    LitVar,          // ^ literal-var N   — load literals[N].value
    SetterRecvVar,   // x := arg0; ^ self — store, return receiver
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
    int slotIndex   = 0;          // recvVar / temp / litVar selector
    uint64_t immBits = 0;         // for NilImm / ImmediateOop

    auto bail = [&](const char* why) -> void* {
        if (trace) fprintf(stderr, "[T2-x86]   bail: %s bodyLen=%zu b0=0x%02x\n",
                           why, bodyLen, b0);
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
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
        // Literal frame starts at slot 1 (slot 0 is the header) — bake
        // the literal Oop in as an immediate.
        int idx = b0 - SistaV1::PushLitConstBase;
        Oop lit = methodObj->slotAt(1 + idx);
        kind = ReturnKind::ImmediateOop;
        immBits = lit.rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushLitVar(b0)) {
        // literals[N] is an Association; return association.value — must
        // load dynamically (the value can change at runtime).
        kind = ReturnKind::LitVar;
        slotIndex = b0 - SistaV1::PushLitVarBase;
    } else if (bodyLen >= 3 && b0 == SistaV1::PushTempBase
                            && SistaV1::isPopStoreRecv(b1)
                            && b2 == SistaV1::ReturnReceiver) {
        // Setter: receiver.instVar[M] := temp0; ^ self  (3 bytes).
        kind = ReturnKind::SetterRecvVar;
        slotIndex = b1 - SistaV1::PopStoreRecvBase;
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

    Gp retOop = cc.new_gp64("retOop");
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
        // tempBase points at temp/arg 0; copy that Oop into receiver's
        // slot[slotIndex] and return the receiver.
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
    }

    cc.mov(ptr(state, OFF_RETVAL), retOop);

    Gp exitVal = cc.new_gp32("exitVal");
    cc.mov(exitVal, Imm(EXIT_RETURN));
    cc.mov(ptr(state, OFF_EXIT), exitVal);

    cc.ret();
    cc.end_func();

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
