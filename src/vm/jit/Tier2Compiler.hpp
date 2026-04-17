/*
 * Tier2Compiler.hpp - optimizing JIT compiler interface
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * STATUS (2026-04-17): MIR backend removed; asmjit-based replacement
 * pending. Current implementation stubs Tier2Compiler::compile() to
 * return nullptr so the runtime falls through to Tier 1. No tier-2
 * code is generated until the asmjit implementation lands.
 *
 * PLAN: use asmjit's `Compiler` API for per-arch (arm64 + x64)
 * codegen. See docs/jit-toolkit-evaluation.md for why MIR was
 * dropped. Rewrite happens in src/vm/jit/Tier2Compiler.cpp.
 */

#ifndef PHARO_TIER2_COMPILER_HPP
#define PHARO_TIER2_COMPILER_HPP

#include "JITConfig.hpp"
#include "JITState.hpp"
#include "JITMethod.hpp"
#include "../Oop.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

#if PHARO_JIT_ENABLED

namespace pharo {

class ObjectMemory;
class Interpreter;

namespace jit {

class CodeZone;
class MethodMap;

class Tier2Compiler {
public:
    Tier2Compiler(CodeZone& zone, MethodMap& methodMap,
                  ObjectMemory& memory, Interpreter& interp);
    ~Tier2Compiler();

    // Initialize backend. Returns false on failure.
    bool initialize();

    // Compile a hot method to Tier 2. Returns function pointer or
    // nullptr. During the MIR→asmjit transition this always returns
    // nullptr; the runtime falls through to Tier 1.
    void* compile(Oop compiledMethod, JITMethod* oldVersion);

    // Statistics
    size_t methodsCompiled() const { return methodsCompiled_; }
    size_t compilationsFailed() const { return compilationsFailed_; }

    // Print T2 bail statistics (no-op during stub period).
    static void dumpBailStats();

private:
    CodeZone&     zone_;
    MethodMap&    methodMap_;
    ObjectMemory& memory_;
    Interpreter&  interp_;

    size_t methodsCompiled_ = 0;
    size_t compilationsFailed_ = 0;
};

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_TIER2_COMPILER_HPP
