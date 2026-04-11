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

namespace pharo { extern uint64_t g_stepNum; }
using pharo::g_stepNum;

// JIT_CALL is now defined in JITState.hpp (shared with Interpreter.cpp)

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
    interp->incJ2JStencilCalls();
    interp->pushFrameForJIT(state);
}

extern "C" void jit_rt_pop_frame(JITState* state) {
    // J2J direct call: pop the interpreter frame after callee returns.
    Interpreter* interp = state->interp;
    interp->incJ2JStencilReturns();
    interp->popFrameForJIT(state);
}

// Out-of-line array primitive handler for IC hit path.
// Called from sendJ2J stencil when primKind >= 14 (at:/at:put:/size).
// info = (primKind << 8) | nArgs
// Returns 1 on success (result written to sp), 0 on failure.
//
// Note: JITState uses pharo::Oop but stencils define their own Oop
// with public .bits field. We work with rawBits() here and use
// Oop::fromRawBits() to write back.
extern "C" uint64_t jit_rt_array_prim(JITState* s, uint64_t info) {
    uint8_t primKind = (uint8_t)(info >> 8);
    int nArgs = (int)(info & 0xFF);

    uint64_t rcvBits = s->sp[-(nArgs + 1)].rawBits();

    // Receiver must be an object pointer (tag == 0, not immediate)
    if ((rcvBits & 7) != 0 || rcvBits < 0x10000)
        return 0;

    uint64_t header = *reinterpret_cast<uint64_t*>(rcvBits);
    uint64_t fmt = (header >> 24) & 0x1F;
    uint64_t slotCount = (header >> 56) & 0xFF;
    if (slotCount == 255) {
        uint64_t raw = *reinterpret_cast<uint64_t*>(rcvBits - 8);
        slotCount = (raw << 8) >> 8;
    }

    if (primKind == 14) {
        // at: — read from Array or byte object
        uint64_t idxBits = s->sp[-nArgs].rawBits();
        if ((idxBits & 7) != 1) return 0;
        int64_t i = (int64_t)idxBits >> 3;
        if (fmt == 2) {
            if (i < 1 || (uint64_t)i > slotCount) return 0;
            Oop* slots = reinterpret_cast<Oop*>(rcvBits + 8);
            s->sp[-(nArgs + 1)] = slots[i - 1];
            s->sp -= nArgs;
            return 1;
        }
        if (fmt >= 16 && fmt <= 23) {
            uint64_t byteSize = slotCount * 8 - (fmt - 16);
            if (i < 1 || (uint64_t)i > byteSize) return 0;
            uint8_t byte = reinterpret_cast<uint8_t*>(rcvBits + 8)[i - 1];
            s->sp[-(nArgs + 1)] = Oop::fromRawBits(((uint64_t)byte << 3) | 1);
            s->sp -= nArgs;
            return 1;
        }
        return 0;
    }

    if (primKind == 15) {
        // at:put: — write to Array or byte object
        uint64_t idxBits = s->sp[-nArgs].rawBits();
        Oop val = s->sp[-(nArgs - 1)];
        if ((idxBits & 7) != 1) return 0;
        if (header & (1ULL << 23)) return 0;  // immutable
        int64_t i = (int64_t)idxBits >> 3;
        if (fmt == 2) {
            if (i < 1 || (uint64_t)i > slotCount) return 0;
            Oop* slots = reinterpret_cast<Oop*>(rcvBits + 8);
            slots[i - 1] = val;
            s->sp[-(nArgs + 1)] = val;
            s->sp -= nArgs;
            return 1;
        }
        if (fmt >= 16 && fmt <= 23) {
            uint64_t valBits = val.rawBits();
            if ((valBits & 7) != 1) return 0;
            int64_t byteVal = (int64_t)valBits >> 3;
            if (byteVal < 0 || byteVal > 255) return 0;
            uint64_t byteSize = slotCount * 8 - (fmt - 16);
            if (i < 1 || (uint64_t)i > byteSize) return 0;
            reinterpret_cast<uint8_t*>(rcvBits + 8)[i - 1] = (uint8_t)byteVal;
            s->sp[-(nArgs + 1)] = val;
            s->sp -= nArgs;
            return 1;
        }
        return 0;
    }

    if (primKind == 16) {
        // size — return slot count or byte size
        uint64_t size;
        if (fmt == 2) {
            size = slotCount;
        } else if (fmt >= 16 && fmt <= 23) {
            size = slotCount * 8 - (fmt - 16);
        } else {
            return 0;
        }
        s->sp[-(nArgs + 1)] = Oop::fromRawBits((size << 3) | 1);
        s->sp -= nArgs;
        return 1;
    }

    return 0;
}

extern "C" void jit_rt_j2j_call(JITState* state) {
    // Merged J2J call: push frame, call callee, pop frame in one C++ call.
    //
    // On entry: cachedTarget = method Oop, sendArgCount = nArgs,
    //           returnValue = entry address bits, ip = past send bytecode.
    // On exit (success): exitReason=0, returnValue set, JITState restored.
    // On exit (bailout): exitReason!=0, JITState has callee's state.
    //
    // Performance: at 6.17M calls/run for fib(28), total overhead is ~16ms
    // (~2.6ns per call). Apple Silicon's store buffer and branch predictor
    // make Clang's 14-register prologue nearly free — tested alternatives
    // (noinline wrappers, SavedFrame-based state) are net-negative.
    // Breaking below 16ms requires lazy frame materialization.

    Interpreter* interp = state->interp;
    interp->incJ2JStencilCalls();

    // Save caller JITState to C locals
    Oop* savedSP = state->sp;
    Oop savedRecv = state->receiver;
    Oop* savedLit = state->literals;
    Oop* savedTemp = state->tempBase;
    JITMethod* savedJM = state->jitMethod;
    Oop savedMethod = state->method;
    int savedArgCount = state->argCount;
    uint8_t* savedIP = state->ip;

    uint8_t* entryAddr = reinterpret_cast<uint8_t*>(state->returnValue.rawBits());
    state->jitMethod = reinterpret_cast<JITMethod*>(entryAddr);

    interp->pushFrameForJIT(state);

    if (__builtin_expect(state->exitReason != 0, 0)) {
        state->sp = savedSP;
        state->receiver = savedRecv;
        state->literals = savedLit;
        state->tempBase = savedTemp;
        state->jitMethod = savedJM;
        state->method = savedMethod;
        state->argCount = savedArgCount;
        state->ip = savedIP;
        return;
    }

    JIT_CALL(entryAddr, state);

    if (__builtin_expect(state->exitReason == ExitReturn, 1)) {
        interp->incJ2JStencilReturns();

        interp->j2jPopFrame(savedMethod, savedRecv);

        state->sp = savedSP;
        state->receiver = savedRecv;
        state->literals = savedLit;
        state->tempBase = savedTemp;
        state->jitMethod = savedJM;
        state->method = savedMethod;
        state->argCount = savedArgCount;
        state->ip = savedIP;
        state->exitReason = ExitNone;
    }
    // Non-ExitReturn: leave callee's state for interpreter bailout.
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
    helpers.j2jCall = reinterpret_cast<void*>(&jit_rt_j2j_call);
    helpers.arrayPrim = reinterpret_cast<void*>(&jit_rt_array_prim);
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

    // Deferral: skip counting for the first N million interpreter steps.
    // This lets the interpreter run at full speed during startup.
    // PHARO_JIT_DEFER=N (seconds; default: 0 = no deferral)
    static int64_t deferSteps = -1; // -1 = uninitialized
    if (deferSteps == -1) {
        const char* env = getenv("PHARO_JIT_DEFER");
        // Convert seconds to approximate step count (~30M steps/sec on interpreter)
        deferSteps = env ? (int64_t)atoi(env) * 30000000 : 0;
        if (deferSteps > 0)
            fprintf(stderr, "[JIT] Deferring compilation for ~%lld steps\n", (long long)deferSteps);
    }
    if (deferSteps > 0 && (int64_t)g_stepNum < deferSteps) return;

    // Bisection support: JIT_MAX_COMPILE=N stops after N compilations
    static int maxCompile = -2; // -2 = uninitialized
    if (maxCompile == -2) {
        const char* env = getenv("JIT_MAX_COMPILE");
        maxCompile = env ? atoi(env) : -1; // -1 = unlimited
    }

    static size_t totalEntries = 0;
    totalEntries++;

    // Debug: log first few entries for specific selectors
    if (totalEntries <= 5000 && interp_) {
        std::string sel = interp_->memory().selectorOf(compiledMethod);
        if (sel == "benchFib" || sel.find("fib") != std::string::npos || sel.find("Fib") != std::string::npos) {
            fprintf(stderr, "[NOTE] #%zu #%s method=0x%llx\n",
                    totalEntries, sel.c_str(), (unsigned long long)compiledMethod.rawBits());
        }
    }

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
        size_t j2jSCalls = interp_ ? interp_->jitJ2JStencilCalls() : 0;
        size_t j2jSReturns = interp_ ? interp_->jitJ2JStencilReturns() : 0;

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
                "| J2J-r: %zu/%zu J2J-a: %zu/%zu J2J-d: %zu J2J-s: %zu/%zu"
                " | map: %zu tracked, %zu hot\n",
                totalEntries, compiler_->methodsCompiled(),
                compiler_->compilationsFailed(),
                codeZone_.usedBytes() / 1024, codeZone_.totalBytes() / 1024,
                icHits, icTotal, hitPct, icPatches, icStale,
                j2jChains, j2jChains + j2jFallbacks,
                j2jActChains, j2jActChains + j2jActFalls,
                j2jDirect,
                j2jSReturns, j2jSCalls,
                tracked, hot);

    }

    uint64_t key = compiledMethod.rawBits();
    size_t idx = (key >> 3) % CountMapSize;

    // Simple linear probe
    for (size_t probe = 0; probe < 8; probe++) {
        size_t i = (idx + probe) % CountMapSize;
        if (countMap_[i].key == key) {
            countMap_[i].count++;
            // Runtime-configurable threshold: PHARO_JIT_THRESHOLD=N (default: CompileThreshold=2)
            static uint32_t threshold = 0;
            if (threshold == 0) {
                const char* env = getenv("PHARO_JIT_THRESHOLD");
                threshold = env ? static_cast<uint32_t>(atoi(env)) : CompileThreshold;
                if (threshold < 1) threshold = 1;
            }
            if (countMap_[i].count == threshold) {
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

    return tryExecute(compiledMethod, state, jm);
}

bool JITRuntime::tryExecute(Oop compiledMethod, JITState& state, JITMethod* jm) {
    // Verify method map integrity (cheap, catches GC/rehash bugs)
    if (jm->compiledMethodOop != compiledMethod.rawBits()) {
        fprintf(stderr, "[JIT] BUG: methodMap returned wrong JITMethod! "
                "requested=0x%llx got=0x%llx\n",
                (unsigned long long)compiledMethod.rawBits(),
                (unsigned long long)jm->compiledMethodOop);
        return false;
    }

    // Touch for LRU tracking
    codeZone_.touch(jm);
    jm->executionCount++;

    // Set up JIT state
    state.jitMethod = jm;
    state.exitReason = ExitNone;
    // j2jDepth is zeroed here; j2jSaveCursor/j2jSaveLimit are set by
    // tryJITActivation before calling tryExecute.
    state.j2jDepth = 0;

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
    if ((state.receiver.isSmallInteger() || state.receiver.isCharacter()) && jm->hasRecvFieldAccess) {
        return false;
    }

    // Bounds-check: receiver must have enough slots for the max field index
    if (jm->hasRecvFieldAccess && state.receiver.isObject()) {
        ObjectHeader* recvObj = reinterpret_cast<ObjectHeader*>(state.receiver.rawBits());
        if (jm->maxRecvFieldIndex >= recvObj->slotCount()) {
            return false;
        }
    }

    // Toggle W^X to executable for JIT execution.
    makeExecutable(jm->codeStart(), jm->codeSize);

    // Call the compiled code
    JIT_CALL(jm->codeStart(), &state);

    // Back to writable (for IC patching etc.)
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
    // Zero J2J state — stencils check j2jDepth on return and compare
    // j2jSaveCursor >= j2jSaveLimit on send. Both must be sane.
    // Setting cursor == limit == nullptr ensures: (1) return stencils
    // skip J2J_INLINE_RETURN (depth 0), (2) send stencils skip
    // stencil J2J (0 >= 0 is true → bail to EXIT_SEND_CACHED).
    state.j2jDepth = 0;
    state.j2jTotalCalls = 0;
    state.j2jSaveCursor = nullptr;
    state.j2jSaveLimit = nullptr;

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

    // Toggle W^X to executable for JIT execution.
    // ICache was flushed during compilation — no need to re-flush on every call.
    makeExecutable(jm->codeStart(), jm->codeSize);

    // Enter at the specified code offset
    StencilFunc entry = reinterpret_cast<StencilFunc>(jm->codeStart() + codeOffset);

    // Final sp validation just before entry
    if (reinterpret_cast<uint64_t>(state.sp) < 0x10000) {
        makeWritable(jm->codeStart(), jm->codeSize);
        fprintf(stderr, "[JIT] BUG: sp=%p just before stencil entry (bc=%u code=%u)\n",
                (void*)state.sp, bcOffset, codeOffset);
        return false;
    }

    entry(&state);

    // Back to writable (for IC patching etc.)
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
