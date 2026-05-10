/*
 * Tier2Compiler_x86_64.cpp - asmjit-based optimizing JIT, x86_64 stub
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 *
 * Per-arch sibling of Tier2Compiler_arm64.cpp.  Selected by CMakeLists
 * on non-arm64 hosts (Linux x86_64, macOS x86_64).
 *
 * STATUS — stub.  Every compile() and tryCompileMultiBC() call
 * currently bails (returns nullptr); the runtime falls through to
 * tier-1.  Same effective behaviour as the arm64 path with every
 * method's pattern-match failing, but without the dead-code dispatch.
 *
 * The op-by-op port follows the same model SistaLowering_x86_64.cpp
 * is using: each new commit adds emit branches for one bytecode /
 * pattern, with a SUnit run as the correctness gate.  The arm64
 * sibling stays the source of truth for op semantics; only the
 * asmjit emit code differs (a64::* -> x86::*).
 *
 * Calling convention matches Tier 1 / Tier 2 arm64:
 *   void fn(JITState* state)   — state in rdi (SysV) / rcx (Win64)
 *   exit via state->returnValue + state->exitReason = EXIT_RETURN
 */

#include "Tier2Compiler.hpp"
#include "JITRuntime.hpp"

#include <cstdio>
#include <cstring>

#if PHARO_JIT_ENABLED

#include <asmjit/x86.h>
#include <asmjit/core/jitruntime.h>

namespace pharo {
namespace jit {

namespace {

// IC data layout (mirror JITMethod.hpp).  Used by flushAllICs().
constexpr int IC_ENTRIES = 6;
constexpr int IC_SLOTS   = IC_ENTRIES * 3 + 1;

// Bail / compile counters.  Kept here (not in a shared .cpp) so the
// arm64 sibling owns its own copy too — matches the SistaLowering
// per-arch g_lowerStats pattern.
size_t g_compiled = 0;
size_t g_bailed   = 0;

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
    (void)compiledMethod;
    (void)oldVersion;
    if (!runtime_) {
        if (!initialize()) {
            compilationsFailed_++;
            return nullptr;
        }
    }
    compilationsFailed_++;
    g_bailed++;
    return nullptr;
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
