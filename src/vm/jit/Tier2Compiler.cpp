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
constexpr int OFF_SP       = 0;
constexpr int OFF_RECEIVER = 8;
constexpr int OFF_RETVAL   = 80;
constexpr int OFF_EXIT     = 76;

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
    ObjectHeader* methodObj = reinterpret_cast<ObjectHeader*>(compiledMethod.rawBits());
    Oop headerOop = methodObj->slotAt(0);
    uint64_t header = headerOop.rawBits();
    if (!(header & 1)) {                 // header must be a SmallInteger
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }
    int numLiterals = (int)((header >> 3) & 0x7FFF);
    int primNum     = (int)((header >> 28) & 0x3FF);

    // Skip methods with primitives; Tier 1 handles them.
    if (primNum != 0) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    uint8_t* bytes = methodObj->bytes();
    size_t bcStart = (1 + numLiterals) * 8;
    size_t totalBytes = methodObj->slotCount() * 8;
    if (bcStart >= totalBytes) {
        compilationsFailed_++;
        g_bailed++;
        return nullptr;
    }

    // --- MVP: only the single-byte returnReceiver pattern ---
    uint8_t firstByte = bytes[bcStart];
    if (firstByte != SistaV1::ReturnReceiver) {
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

    // temp: receiver
    a64::Gp recv = cc.new_gp64("recv");
    cc.ldr(recv, a64::ptr(statePtr, OFF_RECEIVER));

    // state.returnValue = receiver
    cc.str(recv, a64::ptr(statePtr, OFF_RETVAL));

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
