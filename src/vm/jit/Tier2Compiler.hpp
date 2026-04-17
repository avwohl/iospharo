/*
 * Tier2Compiler.hpp - asmjit-based optimizing JIT compiler
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Replaces the removed MIR-based tier-2 with asmjit (2026-04-17).
 * See docs/jit-toolkit-evaluation.md for the tradeoff analysis.
 *
 * ARCHITECTURE
 *
 *   Per-method compile: walk decoded SistaV1 bytecodes, emit native
 *   code via asmjit's Compiler API. asmjit handles register
 *   allocation and instruction selection.
 *
 *   Per-arch code lives in Tier2Compiler_arm64.cpp / Tier2Compiler_x64.cpp
 *   (arm64 first; x64 later). Shared plumbing (method walk, bailout,
 *   code-zone allocation) is in Tier2Compiler.cpp.
 *
 *   Every generated function has signature `void(*)(JITState*)`.
 *   It reads state.sp / state.receiver / etc., does its work, then
 *   writes state.returnValue + state.exitReason and returns.
 *
 * CURRENT STATUS
 *
 *   MVP: compile supports only `ReturnReceiver` (bytecode 0x58) —
 *   just enough to prove the asmjit pipeline works end-to-end.
 *   All other bytecode patterns bail (compile returns nullptr,
 *   runtime falls through to Tier 1).
 */

#ifndef PHARO_TIER2_COMPILER_HPP
#define PHARO_TIER2_COMPILER_HPP

#include "JITConfig.hpp"
#include "JITState.hpp"
#include "JITMethod.hpp"
#include "../Oop.hpp"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

#if PHARO_JIT_ENABLED

// We need the full JitRuntime type because we hold it by
// std::unique_ptr. asmjit uses an inline ABI namespace so a simple
// forward-declare in `namespace asmjit { class JitRuntime; }` doesn't
// match the real definition in `asmjit::vXX::JitRuntime`.
#include <asmjit/core/jitruntime.h>

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

    bool initialize();

    // Compile a hot method. Returns a function pointer of type
    // `void(JITState*)` or nullptr if the method can't be handled
    // (too complex, unsupported bytecode, or asmjit error).
    void* compile(Oop compiledMethod, JITMethod* oldVersion);

    size_t methodsCompiled() const { return methodsCompiled_; }
    size_t compilationsFailed() const { return compilationsFailed_; }

    static void dumpBailStats();

    // Called from JITRuntime::recoverAfterGC.  CompiledMethod Oops
    // stored in T2 IC data (as methodBits) may become stale after a
    // GC moves objects — zero all entries so the next send does a
    // full lookup and re-patches.
    void flushAllICs();

private:
    CodeZone&     zone_;
    MethodMap&    methodMap_;
    ObjectMemory& memory_;
    Interpreter&  interp_;

    // asmjit runtime owns the code memory allocator. One JitRuntime
    // is reused across all compilations; it handles MAP_JIT / W^X
    // protection on macOS/iOS internally.
    std::unique_ptr<asmjit::JitRuntime> runtime_;

    // IC data buffers owned by the compiler — one per T2-compiled
    // method that contains a send.  Layout matches the T1 IC (see
    // JITMethod.hpp: 6 entries × 24 bytes + 8 bytes selectorBits =
    // 152 bytes).  We use std::vector<uint64_t> so the buffer is
    // naturally aligned and easy to zero-fill.
    std::vector<std::vector<uint64_t>> icBuffers_;

    size_t methodsCompiled_ = 0;
    size_t compilationsFailed_ = 0;
};

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
#endif // PHARO_TIER2_COMPILER_HPP
