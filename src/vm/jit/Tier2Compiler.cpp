/*
 * Tier2Compiler.cpp - optimizing JIT compiler (MIR removed, asmjit pending)
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * MIR backend removed 2026-04-17 (see docs/jit-toolkit-evaluation.md).
 * The asmjit-based replacement is pending; during the transition this
 * compile() stub returns nullptr unconditionally. T2 is opt-in via
 * PHARO_T2=1 and already falls back to T1 when no T2 code is
 * available, so this is a no-op from the user's perspective until
 * the asmjit implementation lands.
 */

#include "Tier2Compiler.hpp"
#include "JITRuntime.hpp"
#include "CodeZone.hpp"
#include "JITMethod.hpp"
#include "../ObjectMemory.hpp"
#include "../Interpreter.hpp"
#include <cstdio>

#if PHARO_JIT_ENABLED

namespace pharo {
namespace jit {

Tier2Compiler::Tier2Compiler(CodeZone& zone, MethodMap& methodMap,
                             ObjectMemory& memory, Interpreter& interp)
    : zone_(zone), methodMap_(methodMap), memory_(memory), interp_(interp) {}

Tier2Compiler::~Tier2Compiler() = default;

bool Tier2Compiler::initialize() {
    // No backend to initialize during the MIR→asmjit transition.
    return true;
}

void* Tier2Compiler::compile(Oop compiledMethod, JITMethod* oldVersion) {
    (void)compiledMethod;
    (void)oldVersion;
    // Stub: MIR removed, asmjit implementation pending. Runtime falls
    // through to Tier 1.
    compilationsFailed_++;
    return nullptr;
}

void Tier2Compiler::dumpBailStats() {
    // No bail stats during the MIR→asmjit transition.
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
