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
    // Arithmetic overflow: restore entry SP and re-execute the whole method.
    // The interpreter will handle LargeInteger arithmetic.
    state->exitReason = ExitArithOverflow;
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

    // Store interpreter reference for stats access
    interp_ = &interp;

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
    helpers.megaCacheAddr = megaCache_;
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
        size_t icHits = interp_ ? interp_->jitICHits() : 0;
        size_t icMisses = interp_ ? interp_->jitICMisses() : 0;
        size_t icPatches = interp_ ? interp_->jitICPatches() : 0;
        size_t icStale = interp_ ? interp_->jitICStale() : 0;
        size_t icTotal = icHits + icMisses;
        int hitPct = icTotal > 0 ? static_cast<int>(icHits * 100 / icTotal) : 0;
        size_t j2jChains = interp_ ? interp_->jitJ2JChains() : 0;
        size_t j2jFallbacks = interp_ ? interp_->jitJ2JFallbacks() : 0;
        size_t j2jActChains = interp_ ? interp_->jitJ2JActChains() : 0;
        size_t j2jActFalls = interp_ ? interp_->jitJ2JActFalls() : 0;

        // Count map diagnostics
        size_t tracked = 0, hot = 0;
        for (size_t ci = 0; ci < CountMapSize; ci++) {
            if (countMap_[ci].key != 0) {
                tracked++;
                if (countMap_[ci].count >= CompileThreshold) hot++;
            }
        }
        fprintf(stderr, "[JIT] Stats: %zu sends, %zu compiled, %zu failed, "
                "%zu/%zu KB code | IC: %zu/%zu (%d%% hit, %zu patched, %zu stale) "
                "| J2J-r: %zu/%zu J2J-a: %zu/%zu | map: %zu tracked, %zu hot\n",
                totalEntries, compiler_->methodsCompiled(),
                compiler_->compilationsFailed(),
                codeZone_.usedBytes() / 1024, codeZone_.totalBytes() / 1024,
                icHits, icTotal, hitPct, icPatches, icStale,
                j2jChains, j2jChains + j2jFallbacks,
                j2jActChains, j2jActChains + j2jActFalls,
                tracked, hot);

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
                    std::string sel = interp_ ? interp_->memory().selectorOf(compiledMethod) : "?";
                    fprintf(stderr, "[JIT] Compiled #%s (entry %u, %u bytes)\n",
                            sel.c_str(), countMap_[i].count, jm->codeSize);
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

    // Sends are handled via deopt: the stencil sets state.ip to the send
    // bytecode and exits, letting the interpreter resume at the send.
    //
    // Heap writes (storeRecvVar, storeLitVar) are allowed because
    // generational GC is not implemented — all objects are in old space,
    // so the write barrier (isOld && isYoung check) is a no-op.
    // TODO: Add write barrier calls to store stencils when gen GC is added.

    // Touch for LRU tracking
    codeZone_.touch(jm);
    jm->executionCount++;

    // Set up JIT state
    state.jitMethod = jm;
    state.exitReason = ExitNone;

    // Validate JITState fields
    if (reinterpret_cast<uint64_t>(state.sp) < 0x10000 ||
        reinterpret_cast<uint64_t>(state.ip) < 0x10000 ||
        reinterpret_cast<uint64_t>(state.tempBase) < 0x10000 ||
        reinterpret_cast<uint64_t>(state.literals) < 0x10000) {
        fprintf(stderr, "[JIT] BUG: invalid JITState in tryExecute: sp=%p ip=%p tempBase=%p literals=%p\n",
                (void*)state.sp, (void*)state.ip,
                (void*)state.tempBase, (void*)state.literals);
        return false;
    }

    // Guard: immediate receivers can't have instance variables.
    // Methods with pushRecvVar/storeRecvVar dereference receiver as an object.
    if ((state.receiver.isSmallInteger() || state.receiver.isCharacter()) && jm->hasRecvFieldAccess) {
        return false;  // Let interpreter handle it
    }

    // Bounds-check: receiver must have enough slots for the max field index
    if (jm->hasRecvFieldAccess && state.receiver.isObject()) {
        ObjectHeader* recvObj = reinterpret_cast<ObjectHeader*>(state.receiver.rawBits());
        size_t slotCount = recvObj->slotCount();
        if (jm->maxRecvFieldIndex >= slotCount) {
            return false;
        }
    }

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

bool JITRuntime::tryResume(Oop compiledMethod, uint32_t bcOffset, JITState& state) {
    if (!initialized_) return false;

    JITMethod* jm = methodMap_.lookup(compiledMethod.rawBits());
    if (!jm || !jm->isExecutable()) return false;

    // Look up the code offset for this bytecode offset
    uint32_t codeOffset = jm->codeOffsetForBC(bcOffset);
    if (codeOffset == 0 || codeOffset >= jm->codeSize) return false;

    // Validate that codeOffset is within the machine code region, not the
    // literal pool / bcToCode table / IC data appended after it.
    // bcToCodeTable[numBytecodes] is the sentinel = end of machine code.
    uint32_t machineCodeEnd = jm->bcToCodeTable()[jm->numBytecodes];
    if (codeOffset >= machineCodeEnd) {
        fprintf(stderr, "[JIT] BUG: resume codeOffset %u >= machineCodeEnd %u (bc %u)\n",
                codeOffset, machineCodeEnd, bcOffset);
        return false;
    }

    // Validate state.sp is a reasonable pointer (not a SmallInteger or low address)
    if (reinterpret_cast<uint64_t>(state.sp) < 0x10000) {
        fprintf(stderr, "[JIT] BUG: resume sp=%p looks invalid\n", (void*)state.sp);
        return false;
    }

    // Touch for LRU tracking
    codeZone_.touch(jm);

    // Set up JIT state
    state.jitMethod = jm;
    state.exitReason = ExitNone;

    // Validate IC data area is within code zone
    if (jm->numICEntries > 0) {
        uint32_t icSize = jm->numICEntries * 72;
        uint8_t* icStart = jm->codeStart() + jm->codeSize - icSize;
        if (icStart < codeZone_.rawStart() || icStart + icSize > codeZone_.rawStart() + codeZone_.totalBytes()) {
            fprintf(stderr, "[JIT] BUG: IC data %p outside code zone [%p, %p)\n",
                    (void*)icStart, (void*)codeZone_.rawStart(),
                    (void*)(codeZone_.rawStart() + codeZone_.totalBytes()));
            return false;
        }
    }

    // Guard: immediate receivers can't have instance variables.
    if ((state.receiver.isSmallInteger() || state.receiver.isCharacter()) && jm->hasRecvFieldAccess) {
        return false;  // Let interpreter handle it
    }

    // Bounds-check: receiver must have enough slots for the max field index
    if (jm->hasRecvFieldAccess && state.receiver.isObject()) {
        ObjectHeader* recvObj = reinterpret_cast<ObjectHeader*>(state.receiver.rawBits());
        size_t slotCount = recvObj->slotCount();
        if (jm->maxRecvFieldIndex >= slotCount) {
            return false;  // Let interpreter handle
        }
    }

    // Toggle W^X to executable
    makeExecutable(jm->codeStart(), jm->codeSize);

    // Enter at the specified code offset
    StencilFunc entry = reinterpret_cast<StencilFunc>(jm->codeStart() + codeOffset);
    entry(&state);

    // Back to writable
    makeWritable(jm->codeStart(), jm->codeSize);

    return true;
}

void JITRuntime::flushCaches() {
    if (!initialized_) return;

    // Clear mega cache
    std::memset(megaCache_, 0, sizeof(megaCache_));

    // Clear all IC entries in compiled methods.
    // IC data layout per send site: 4 x [uint64_t key, uint64_t method] + uint64_t selectorBits
    // Zero ALL 72 bytes including selectorBits — after GC, selector Symbol Oops
    // may have moved, making selectorBits stale. The mega cache probe in
    // stencil_sendPoly skips if selectorBits==0, so this is safe (just slower
    // until the interpreter repopulates the mega cache on misses).
    static constexpr uint32_t IC_BYTES_PER_SITE = 72;
    JITMethod* m = codeZone_.firstMethod();
    while (m) {
        if (m->numICEntries > 0) {
            uint32_t icSize = m->numICEntries * IC_BYTES_PER_SITE;
            uint8_t* icStart = m->codeStart() + m->codeSize - icSize;
            std::memset(icStart, 0, icSize);
        }
        m = m->nextInZone;
    }
}

void JITRuntime::recoverAfterGC(ObjectMemory& memory) {
    if (!initialized_) return;

    // 1. Flush all caches (ICs contain stale method Oops, mega cache stale too)
    flushCaches();

    // 2. Update nil/true/false bits (GC may have moved them)
    updateSpecialOops(memory);

    // 3. Rebuild MethodMap — keys are compiledMethodOop bits which were updated
    //    in-place by forEachRoot during updatePointersAfterCompact, but the
    //    MethodMap hash table still has the old key values.
    methodMap_.clear();
    JITMethod* m = codeZone_.firstMethod();
    while (m) {
        if (m->state == MethodState::Compiled) {
            methodMap_.insert(m->compiledMethodOop, m);
        }
        m = m->nextInZone;
    }

    // 4. Clear count map — keys are stale CompiledMethod Oop bits.
    //    Methods will re-accumulate counts naturally. Since the compile
    //    threshold is only 2, this is a minor transient cost.
    std::memset(countMap_, 0, sizeof(countMap_));
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
