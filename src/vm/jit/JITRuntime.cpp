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
}

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

extern "C" void jit_rt_push_frame(JITState* state) {
    // J2J direct call: push an interpreter frame for GC root scanning.
    // Reads cachedTarget (method Oop), sendArgCount, ip from state.
    Interpreter* interp = state->interp;
    interp->pushFrameForJIT(state);
}

extern "C" void jit_rt_pop_frame(JITState* state) {
    // J2J direct call: pop the interpreter frame after callee returns.
    Interpreter* interp = state->interp;
    interp->popFrameForJIT(state);
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
    helpers.pushFrame = reinterpret_cast<void*>(&jit_rt_push_frame);
    helpers.popFrame = reinterpret_cast<void*>(&jit_rt_pop_frame);
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

    // Bisection support: JIT_MAX_COMPILE=N stops after N compilations
    static int maxCompile = -2; // -2 = uninitialized
    if (maxCompile == -2) {
        const char* env = getenv("JIT_MAX_COMPILE");
        maxCompile = env ? atoi(env) : -1; // -1 = unlimited
    }

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
        size_t j2jDirect = interp_ ? interp_->jitJ2JDirectPatches() : 0;

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
                "| J2J-r: %zu/%zu J2J-a: %zu/%zu J2J-d: %zu | map: %zu tracked, %zu hot\n",
                totalEntries, compiler_->methodsCompiled(),
                compiler_->compilationsFailed(),
                codeZone_.usedBytes() / 1024, codeZone_.totalBytes() / 1024,
                icHits, icTotal, hitPct, icPatches, icStale,
                j2jChains, j2jChains + j2jFallbacks,
                j2jActChains, j2jActChains + j2jActFalls,
                j2jDirect,
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
                // Bisection: stop after N compilations
                if (maxCompile >= 0 && (int)compiler_->methodsCompiled() >= maxCompile) {
                    return;
                }
                // Selector exclusion: JIT_EXCLUDE=sel1,sel2,...
                {
                    static const char* excludeEnv = getenv("JIT_EXCLUDE");
                    if (excludeEnv && interp_) {
                        std::string sel = interp_->memory().selectorOf(compiledMethod);
                        std::string excl(excludeEnv);
                        // Simple comma-separated check
                        size_t pos = 0;
                        while (pos < excl.size()) {
                            size_t comma = excl.find(',', pos);
                            if (comma == std::string::npos) comma = excl.size();
                            std::string token = excl.substr(pos, comma - pos);
                            if (sel == token) {
                                fprintf(stderr, "[JIT] EXCLUDED #%s\n", sel.c_str());
                                return;
                            }
                            pos = comma + 1;
                        }
                    }
                }
                // Hit threshold — compile!
                size_t gcBefore = interp_ ? interp_->memory().statistics().gcCount : 0;
                JITMethod* jm = compiler_->compile(compiledMethod);
                if (interp_) {
                    size_t gcAfter = interp_->memory().statistics().gcCount;
                    if (gcAfter > gcBefore) {
                        fprintf(stderr, "[JIT] GC during compile #%zu! "
                                "(%zu GCs: %zu→%zu)\n",
                                compiler_->methodsCompiled(),
                                gcAfter - gcBefore, gcBefore, gcAfter);
                    }
                }
                if (jm) {
                    std::string sel = interp_ ? interp_->memory().selectorOf(compiledMethod) : "?";
                    // Get class name from penultimate literal (methodClass association)
                    std::string cls = "?";
                    if (interp_) {
                        size_t nLits = interp_->memory().numLiteralsOf(compiledMethod);
                        if (nLits >= 2) {
                            // penultimate literal (index nLits, 1-based in literal frame)
                            Oop assoc = interp_->memory().fetchPointer(nLits, compiledMethod);
                            if (assoc.isObject() && !assoc.isNil()) {
                                // Association value is slot 1 (0-based)
                                Oop classOop = interp_->memory().fetchPointer(1, assoc);
                                if (classOop.isObject() && !classOop.isNil()) {
                                    cls = interp_->memory().nameOfClass(classOop);
                                }
                            }
                        }
                    }
                    uint64_t zoneOff = reinterpret_cast<uint64_t>(jm->codeStart())
                                     - reinterpret_cast<uint64_t>(codeZone_.zoneStart());
                    fprintf(stderr, "[JIT] Compiled #%s [%s] (entry %u, %u bytes%s) @0x%llx\n",
                            sel.c_str(), cls.c_str(), countMap_[i].count, jm->codeSize,
                            jm->hasPrimPrologue ? ", prim" : "",
                            (unsigned long long)zoneOff);
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

    // Verify method map integrity (cheap, catches GC/rehash bugs)
    if (jm->compiledMethodOop != compiledMethod.rawBits()) {
        fprintf(stderr, "[JIT] BUG: methodMap returned wrong JITMethod! "
                "requested=0x%llx got=0x%llx\n",
                (unsigned long long)compiledMethod.rawBits(),
                (unsigned long long)jm->compiledMethodOop);
        return false;
    }

    // Inline getter/setter/yourself sends are dispatched directly in the
    // stencil (stencil_sendPoly IC_HIT macro) without exiting to C++.
    // Non-trivial sends still exit via ExitSendCached, but J2J chaining in
    // the interpreter handles those without the full activate overhead.

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

    // Flush I-cache + toggle W^X to executable, then back to writable.
    flushICache(jm->codeStart(), jm->codeSize);
    makeExecutable(jm->codeStart(), jm->codeSize);

    // Call the compiled code
    StencilFunc entry = reinterpret_cast<StencilFunc>(jm->codeStart());
    entry(&state);

    // Back to writable
    makeWritable(jm->codeStart(), jm->codeSize);

    return true;
}

bool JITRuntime::tryResume(Oop compiledMethod, uint32_t bcOffset, JITState& state) {
    if (!initialized_) return false;
    static bool noResume = !!getenv("PHARO_NO_RESUME");
    if (noResume) return false;

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
        uint32_t icSize = jm->numICEntries * 104;
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

    // Flush I-cache + toggle W^X
    flushICache(jm->codeStart(), jm->codeSize);
    makeExecutable(jm->codeStart(), jm->codeSize);

    // Enter at the specified code offset
    StencilFunc entry = reinterpret_cast<StencilFunc>(jm->codeStart() + codeOffset);

    // Final sp validation just before entry (verify it wasn't corrupted since the check above)
    if (reinterpret_cast<uint64_t>(state.sp) < 0x10000) {
        makeWritable(jm->codeStart(), jm->codeSize);
        fprintf(stderr, "[JIT] BUG: sp=%p just before stencil entry (bc=%u code=%u)\n",
                (void*)state.sp, bcOffset, codeOffset);
        return false;
    }

    entry(&state);

    // Back to writable
    makeWritable(jm->codeStart(), jm->codeSize);

    return true;
}

void JITRuntime::flushCaches() {
    if (!initialized_) return;

    // Clear mega cache
    std::memset(megaCache_, 0, sizeof(megaCache_));

    // Clear IC entries but preserve selectorBits (slot 12 of each 13-slot IC).
    // selectorBits is written once at compile time and never re-patched,
    // so zeroing it would permanently disable megamorphic cache probes.
    // Layout per IC site: 4 entries × [key, method, extra] + selectorBits = 13 uint64_t = 104 bytes
    JITMethod* m = codeZone_.firstMethod();
    while (m) {
        if (m->numICEntries > 0) {
            uint8_t* icStart = m->codeStart() + m->codeSize
                             - m->numICEntries * 104;
            for (uint32_t i = 0; i < m->numICEntries; i++) {
                uint64_t* slots = reinterpret_cast<uint64_t*>(icStart + i * 104);
                // Zero the 4 IC entries (slots 0-11) but keep slot 12 (selectorBits)
                std::memset(slots, 0, 12 * sizeof(uint64_t));
            }
        }
        m = m->nextInZone;
    }
}

void JITRuntime::recoverAfterGC(ObjectMemory& memory) {
    if (!initialized_) return;

    // IC entries (method Oops, selector Oops) were updated in-place by
    // forEachRoot during compaction. No need to flush them.
    // Only clear the mega cache: it's a hash table keyed by selectorBits,
    // and those bits changed, making hash positions stale. Cheaper to flush
    // than rehash.
    std::memset(megaCache_, 0, sizeof(megaCache_));

    // Update nil/true/false bits (GC may have moved them)
    updateSpecialOops(memory);

    // Rebuild MethodMap — keys are compiledMethodOop bits which were updated
    // in-place by forEachRoot during updatePointersAfterCompact, but the
    // MethodMap hash table still has the old key values.
    methodMap_.clear();
    JITMethod* m = codeZone_.firstMethod();
    while (m) {
        if (m->state == MethodState::Compiled) {
            methodMap_.insert(m->compiledMethodOop, m);
        }
        m = m->nextInZone;
    }

    // Count map keys (CompiledMethod Oop bits) were updated in-place by
    // forEachRoot during compaction. No need to clear — methods preserve
    // their accumulated counts across GC.
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
