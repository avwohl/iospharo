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
constexpr int OFF_SP        = 0;
constexpr int OFF_RECEIVER  = 8;
constexpr int OFF_TEMPBASE  = 24;
constexpr int OFF_EXIT      = 76;
constexpr int OFF_RETVAL    = 80;
constexpr int OFF_TRUEOOP   = 128;
constexpr int OFF_FALSEOOP  = 136;

// ExitReason values (mirror JITState.hpp).
constexpr int EXIT_RETURN = 1;

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

    enum class ReturnKind { Receiver, True, False, NilImm, RecvVar, SetterRecvVar };
    ReturnKind kind;
    uint64_t nilBits = 0;
    int recvVarIndex = 0;  // for RecvVar (getter) and SetterRecvVar (setter target)

    if (b0 == SistaV1::ReturnReceiver) {
        kind = ReturnKind::Receiver;
    } else if (b0 == SistaV1::ReturnTrue) {
        kind = ReturnKind::True;
    } else if (b0 == SistaV1::ReturnFalse) {
        kind = ReturnKind::False;
    } else if (b0 == SistaV1::ReturnNil) {
        kind = ReturnKind::NilImm;
        nilBits = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits();
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
        nilBits = memory_.specialObject(SpecialObjectIndex::NilObject).rawBits();
    } else if (bodyLen >= 2 && b1 == SistaV1::ReturnTop
                            && SistaV1::isPushRecvVar(b0)) {
        kind = ReturnKind::RecvVar;
        recvVarIndex = b0 - SistaV1::PushRecvVarBase;
    } else if (bodyLen >= 3 && b0 == SistaV1::PushTempBase
                            && SistaV1::isPopStoreRecv(b1)
                            && b2 == SistaV1::ReturnReceiver) {
        kind = ReturnKind::SetterRecvVar;
        recvVarIndex = b1 - SistaV1::PopStoreRecvBase;
    } else {
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
        cc.mov(retOop, asmjit::Imm(nilBits));
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
    }

    // state.returnValue = retOop
    cc.str(retOop, a64::ptr(statePtr, OFF_RETVAL));

    // state.exitReason = EXIT_RETURN (int32)
    a64::Gp exitVal = cc.new_gp32("exitVal");
    cc.mov(exitVal, EXIT_RETURN);
    cc.str(exitVal, a64::ptr(statePtr, OFF_EXIT));

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

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
