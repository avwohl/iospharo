/*
 * JITRuntime.cpp - Runtime support for JIT-compiled code
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 */

#include "JITRuntime.hpp"
#include "PlatformJIT.hpp"
#include "../ObjectMemory.hpp"
#include "../Interpreter.hpp"
#include <cstring>
#include <cstdio>

#if PHARO_JIT_ENABLED

namespace pharo {
namespace jit {

// ===== RUNTIME HELPER IMPLEMENTATIONS =====

// These are the C functions that JIT stencils branch to when they
// can't handle something inline. They simply return — the JIT entry
// point checks exitReason after the call chain unwinds.

extern "C" void jit_rt_send(JITState* state) {
    // The exitReason is already set by the stencil.
    // Control returns to the JIT entry point (tryExecute), which
    // returns to the interpreter to handle the send.
    (void)state;
}

extern "C" void jit_rt_return(JITState* state) {
    // exitReason and returnValue already set by the stencil.
    (void)state;
}

extern "C" void jit_rt_arith_overflow(JITState* state) {
    // Treat overflow as a send (the interpreter will do the full
    // arithmetic message send with LargeInteger support).
    state->exitReason = ExitSend;
}

// ===== JITRuntime =====

JITRuntime::JITRuntime()
    : nilOopBits(0), trueOopBits(0), falseOopBits(0)
{
    std::memset(countMap_, 0, sizeof(countMap_));
}

JITRuntime::~JITRuntime() {
    delete compiler_;
}

bool JITRuntime::initialize(ObjectMemory& memory, Interpreter& interp) {
    if (initialized_) return true;

    // Initialize code zone
    if (!codeZone_.initialize()) {
        fprintf(stderr, "[JIT] Failed to initialize code zone\n");
        return false;
    }

    // Initialize method map
    if (!methodMap_.initialize()) {
        fprintf(stderr, "[JIT] Failed to initialize method map\n");
        return false;
    }

    // Set up special Oop values
    updateSpecialOops(memory);

    // Create the compiler
    compiler_ = new JITCompiler(codeZone_, methodMap_, memory, interp);

    // Register runtime helpers
    JITCompiler::RuntimeHelpers helpers;
    helpers.sendSlow = reinterpret_cast<void*>(&jit_rt_send);
    helpers.returnToInterp = reinterpret_cast<void*>(&jit_rt_return);
    helpers.arithOverflow = reinterpret_cast<void*>(&jit_rt_arith_overflow);
    helpers.nilOopAddr = &nilOopBits;
    helpers.trueOopAddr = &trueOopBits;
    helpers.falseOopAddr = &falseOopBits;
    compiler_->setHelpers(helpers);

    // After MAP_JIT mmap with PROT_EXEC, the initial W^X state might be
    // "executable" rather than "writable". Ensure we start in writable mode
    // so allocate() can zero the memory.
    makeWritable(codeZone_.rawStart(), codeZone_.totalBytes());

    initialized_ = true;
    fprintf(stderr, "[JIT] Initialized: %zu MB code zone at %p\n",
            codeZone_.totalBytes() / (1024 * 1024),
            (void*)codeZone_.rawStart());

    return true;
}

void JITRuntime::updateSpecialOops(ObjectMemory& memory) {
    nilOopBits = Oop::nil().rawBits();
    trueOopBits = memory.trueObject().rawBits();
    falseOopBits = memory.falseObject().rawBits();
}

void JITRuntime::noteMethodEntry(Oop compiledMethod) {
    if (!initialized_ || !compiler_) return;

    static size_t totalEntries = 0;
    totalEntries++;

    // Periodic stats (every ~64K entries)
    if ((totalEntries & 0xFFFF) == 0) {
        fprintf(stderr, "[JIT] Stats: %zu sends, %zu compiled, %zu failed, "
                "%zu/%zu KB code zone\n",
                totalEntries, compiler_->methodsCompiled(),
                compiler_->compilationsFailed(),
                codeZone_.usedBytes() / 1024, codeZone_.totalBytes() / 1024);
    }

    uint64_t key = compiledMethod.rawBits();
    size_t idx = (key >> 3) % CountMapSize;

    // Simple linear probe
    for (size_t probe = 0; probe < 8; probe++) {
        size_t i = (idx + probe) % CountMapSize;
        if (countMap_[i].key == key) {
            countMap_[i].count++;
            if (countMap_[i].count == CompileThreshold) {
                // Hit threshold — compile!
                JITMethod* jm = compiler_->compile(compiledMethod);
                if (jm) {
                    fprintf(stderr, "[JIT] Compiled method (entry count %u, %u bytes code)\n",
                            countMap_[i].count, jm->codeSize);
                }
            }
            return;
        }
        if (countMap_[i].key == 0) {
            countMap_[i].key = key;
            countMap_[i].count = 1;
            return;
        }
    }
    // Count map full for this bucket — just skip
}

bool JITRuntime::tryExecute(Oop compiledMethod, JITState& state) {
    if (!initialized_) return false;

    JITMethod* jm = methodMap_.lookup(compiledMethod.rawBits());
    if (!jm || !jm->isExecutable()) return false;

    // Can't execute methods with actual sends (need bytecode-level deopt)
    // or heap writes (need write barrier for GC).
    if (jm->hasSends || jm->hasHeapWrites) return false;

    // Touch for LRU tracking
    codeZone_.touch(jm);
    jm->executionCount++;

    // Set up JIT state
    state.jitMethod = jm;
    state.exitReason = ExitNone;

    // Toggle W^X to executable for the call, then back to writable.
    // On Apple Silicon this is just a register write (no syscall).
    makeExecutable(jm->codeStart(), jm->codeSize);

    // Call the compiled code
    StencilFunc entry = reinterpret_cast<StencilFunc>(jm->codeStart());
    entry(&state);

    // Back to writable so metadata updates (touch, counters) work
    makeWritable(jm->codeStart(), jm->codeSize);

    return true;
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
