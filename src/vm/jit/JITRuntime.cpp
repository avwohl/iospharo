/*
 * JITRuntime.cpp - Runtime support for JIT-compiled code
 *
 * Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.
 */

#include "JITRuntime.hpp"
#include "PlatformJIT.hpp"
#include "../DebugSettings.hpp"
#include "../ObjectMemory.hpp"
#include "../Interpreter.hpp"
#include "sista/SistaRuntime.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>

// Defined in Interpreter.cpp; nullptr until Sista hook fires for the
// first time.  JITRuntime peeks at it to skip counting methods Sista
// has compiled with a splice (avoids T1-vs-Sista race —
// memory/project_t1_vs_sista_race.md).
namespace pharo {
namespace sista { class Runtime; }
extern sista::Runtime* sistaRuntimeForGCHook_;
}

namespace pharo { extern uint64_t g_stepNum; }
using pharo::g_stepNum;

// JIT_CALL is now defined in JITState.hpp (shared with Interpreter.cpp)

#if PHARO_JIT_ENABLED

// JIT trampoline JM_SIZE sentinel — arch-specific impl lives in
// src/platform/jit_jmsize_{arm64,other}.cpp (selected by CMake).

namespace pharo {
}

namespace pharo {
namespace jit {

// 2026-05-08 image-init-complete signal — declared in JITRuntime.hpp.
// Definition here so it can link against both Interpreter and the
// JIT stencil helpers (via jit_rt_resolver_check_helper).
volatile bool g_resolverClassVarSet = false;
volatile uint64_t g_resolverSetAtStep = 0;

void noteLitVarStore(uint64_t assocBits, ObjectMemory& memory) {
    Oop assoc = Oop::fromRawBits(assocBits);
    if (!assoc.isObject() || assoc.rawBits() < 0x10000) return;
    if (!memory.isValidPointer(assoc)) return;
    ObjectHeader* aHdr = assoc.asObjectPtr();
    if (aHdr->slotCount() < 2) return;
    Oop value = aHdr->slots()[1];
    Oop key = aHdr->slots()[0];
    if (!key.isObject() || key.rawBits() < 0x10000) return;
    if (!memory.symbolEquals(key, "Resolver")) return;
    // Only flip flag for non-nil stores.
    if (g_resolverClassVarSet) return;
    if (value.isNil()) return;
    g_resolverClassVarSet = true;
    g_resolverSetAtStep = g_stepNum;
    static int n = 0;
    if (n++ < 3) {
        fprintf(stderr,
            "[INIT-RESOLVER] #%d Resolver class var set non-nil at "
            "step %llu (value=0x%llx) — defer will lift after buffer\n",
            n, (unsigned long long)g_stepNum,
            (unsigned long long)value.rawBits());
    }
}

// JIT-side helper called from stencil_popStoreLitVar when
// g_resolverClassVarSet is false.  Wraps noteLitVarStore so the
// stencil only needs to know about JITState.
extern "C" void jit_rt_check_resolver_store(JITState* state, uint64_t assocBits) {
    if (g_resolverClassVarSet) return;
    if (!state || !state->interp) return;
    noteLitVarStore(assocBits, state->interp->memory());
}

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

// B5 diagnostic: ring buffer capturing last N J2J save/restore events.
// Dump to stderr on request (via dumpB5Trace) or periodically.
struct B5Event {
    uint64_t count;
    uint64_t sp;
    uint64_t extra1;
    uint64_t extra2;
    int32_t depth;
    uint32_t eventType;  // 1=save, 2=return
};
static constexpr size_t B5_RING = 512;
static B5Event g_b5Ring[B5_RING];
static size_t g_b5Head = 0;
static size_t g_b5Count = 0;

extern "C" void jit_b5_dump_ring(const char* tag) {
    if (g_b5Count == 0) return;
    fprintf(stderr, "=== B5 ring dump (%s, %zu total events, last %zu) ===\n",
            tag ? tag : "?", g_b5Count, std::min(g_b5Count, B5_RING));
    size_t n = std::min(g_b5Count, B5_RING);
    size_t start = g_b5Count >= B5_RING ? g_b5Head : 0;
    for (size_t i = 0; i < n; i++) {
        const B5Event& e = g_b5Ring[(start + i) % B5_RING];
        uint64_t callerCM, nArgs;
        if (e.eventType == 1) {
            callerCM = e.extra2;
            nArgs = e.extra1;
        } else {
            callerCM = e.extra2 & 0x0000FFFFFFFFFFFFULL;
            nArgs = (e.extra2 >> 48) & 0xFF;
        }
        if (e.eventType == 1) {
            fprintf(stderr, "[B5] #%llu SAVE sp=0x%llx depth=%d nArgs=%llu callerCM=0x%llx\n",
                    (unsigned long long)e.count, (unsigned long long)e.sp,
                    e.depth, (unsigned long long)nArgs,
                    (unsigned long long)callerCM);
        } else {
            fprintf(stderr, "[B5] #%llu RET  sp=0x%llx depth=%d retVal=0x%llx "
                           "callerCM=0x%llx savedArgs=%llu\n",
                    (unsigned long long)e.count, (unsigned long long)e.sp,
                    e.depth, (unsigned long long)e.extra1,
                    (unsigned long long)callerCM, (unsigned long long)nArgs);
        }
    }
    fprintf(stderr, "=== end B5 dump ===\n");
}

// True no-op for the J2J trace fast path.  When B5 / primAtOob diagnostic
// flags are off (the production case), the stencil's _HOLE_RT_J2J_TRACE
// pointer can target this empty function instead of jit_rt_j2j_trace
// — saves the conditional load + branch on every J2J save/return.
// The function-call overhead remains (caller-saved register spills around
// the call site) but the body is a single ret.
extern "C" void jit_rt_j2j_trace_noop(JITState*, uint64_t,
                                       uint64_t, uint64_t) {
    // intentionally empty
}

// B5 diagnostic: called from stencils at J2J save-push and J2J return.
// event=1: save-push.  extra1 = nArgs, extra2 = caller compiledMethod oop.
// event=2: return.  extra1 = retVal.bits, extra2 = callerCM | args<<48.
extern "C" void jit_rt_j2j_trace(JITState* state, uint64_t event,
                                 uint64_t extra1, uint64_t extra2) {
    // event=99: stencil_primAt OOB fall-through (Array/Byte path).
    // event=100: stencil_primAt unsupported-format fall-through.
    //   extra2 layout for event 99: limit/byteSize.
    //   extra2 layout for event 100: (fmt << 32) | classIdx.
    // events 201/202: previously used for SYMCLS investigation; the
    // stencil-side calls have been removed but keep the placeholder so
    // unexpected re-introductions from old stencils get cleanly ignored.
    if (event == 201 || event == 202) {
        return;
    }
    if (event == 99 || event == 100) {
        static bool primAtOob = std::getenv("PHARO_PRIMAT_OOB") != nullptr;
        if (primAtOob) {
            static int oobCount = 0;
            if (++oobCount <= 30) {
                if (event == 99) {
                    fprintf(stderr,
                        "[STENCIL-PRIMAT OOB #%d] i=%lld limit=%llu\n",
                        oobCount, (long long)extra1,
                        (unsigned long long)extra2);
                } else {
                    uint64_t fmt = (extra2 >> 32) & 0xFF;
                    uint64_t classIdx = extra2 & 0x3FFFFF;
                    Oop classOop = (state && state->interp)
                        ? state->interp->memory().classAtIndex((uint32_t)classIdx)
                        : Oop::nil();
                    std::string cname = (state && state->interp && classOop.isObject())
                        ? state->interp->memory().classNameOf(classOop) : "(idx?)";
                    fprintf(stderr,
                        "[STENCIL-PRIMAT BAD-FMT #%d] i=%lld fmt=%llu "
                        "rcvClassIdx=%llu (%s)\n",
                        oobCount, (long long)extra1,
                        (unsigned long long)fmt,
                        (unsigned long long)classIdx, cname.c_str());
                }
            }
        }
        return;
    }
    static bool trace = g_debug.b5Trace;
    if (!trace) return;
    // Raw entry log so we can see if the stencil ever calls us with event=3.
    static size_t entryCount = 0;
    entryCount++;
    // Always record to ring; filter only controls live stderr print.
    g_b5Count++;
    B5Event& slot = g_b5Ring[g_b5Head];
    slot.count = g_b5Count;
    slot.sp = (uint64_t)(uintptr_t)state->sp;
    slot.extra1 = extra1;
    slot.extra2 = extra2;
    slot.depth = state->j2jDepth;
    slot.eventType = (uint32_t)event;
    g_b5Head = (g_b5Head + 1) % B5_RING;
    // Only trace when the caller-oop matches one of the target methods in
    // the atEnd chain to limit output: decodeBytes:, readStream, and
    // PositionableStream class>>on: / PositionableStream>>on:.
    static uint64_t focusOops[8] = {0};
    static int focusCount = 0;
    static bool focusInit = false;
    if (!focusInit) {
        focusInit = true;
        if (const char* env = g_debug.b5Focus) {
            // Comma-separated hex oops
            const char* p = env;
            while (*p && focusCount < 8) {
                char* endp;
                uint64_t v = strtoull(p, &endp, 0);
                if (endp == p) break;
                focusOops[focusCount++] = v;
                p = endp;
                if (*p == ',') p++;
            }
        }
    }
    // Auto-trigger dump on suspicious return: callerCM is one of the 4
    // focus methods AND retVal is a SmallInteger (tag bit 0 set).  This
    // is the exact signature of the B5 bug — the return places a
    // SmallInt where a stream object should be.
    if (event == 2 && (extra1 & 0x7) == 1) {
        uint64_t callerCM = extra2 & 0x0000FFFFFFFFFFFFULL;
        for (int i = 0; i < focusCount; i++) {
            if (focusOops[i] == callerCM) {
                static int autoDumped = 0;
                if (autoDumped < 3) {
                    autoDumped++;
                    fprintf(stderr, "=== B5 AUTO-TRIGGER: SmallInt retVal=0x%llx "
                                    "to focus method 0x%llx ===\n",
                            (unsigned long long)extra1,
                            (unsigned long long)callerCM);
                    jit_b5_dump_ring("auto-smallint-return");
                }
                break;
            }
        }
    }
    // Event-specific decoding:
    // event=1 (save-push): extra1 = nArgs, extra2 = callerCM (unpacked).
    // event=2 (return):    extra1 = retVal.bits, extra2 = callerCM | nArgs<<48.
    uint64_t callerCM, nArgs;
    if (event == 1) {
        callerCM = extra2;
        nArgs = extra1;
    } else {
        callerCM = extra2 & 0x0000FFFFFFFFFFFFULL;
        nArgs = (extra2 >> 48) & 0xFF;
    }
    if (focusCount > 0) {
        bool hit = false;
        for (int i = 0; i < focusCount; i++) {
            if (focusOops[i] == callerCM) { hit = true; break; }
        }
        if (!hit) return;
    }
    static size_t count = 0;
    static size_t skipEarly = (size_t)g_debug.b5Skip;
    static size_t maxEvents = g_debug.b5Max > 0 ? (size_t)g_debug.b5Max : 800;
    count++;
    if (count <= skipEarly) return;
    if (count - skipEarly > maxEvents) return;
    if (event == 1) {
        std::string sel;
        std::string cls;
        if (state->interp && callerCM > 0x10000) {
            Oop cm = Oop::fromRawBits(callerCM);
            sel = state->interp->memory().selectorOf(cm);
            cls = state->interp->classNameOfMethod(cm);
        }
        // Also log state.receiver (caller's self at save time) — useful
        // for tracking down the bug-14 corrupted-receiver chain.
        std::string rcvrKind = state->receiver.isSmallInteger() ? "SmI"
            : state->receiver.isObject()
              ? state->interp->memory().classNameOf(state->receiver).c_str()
              : "other";
        // The stencil emits this trace BEFORE bumping j2jSaveCursor, so the
        // freshly-written save slot sits at the current cursor.  Read its
        // resumeAddr so we can correlate against bcToCodeTable entries.
        void* savedResume = nullptr;
        if (state->j2jSaveCursor) {
            Interpreter::J2JSave* sv = reinterpret_cast<Interpreter::J2JSave*>(
                state->j2jSaveCursor);
            savedResume = sv->resumeAddr;
        }
        fprintf(stderr, "[B5] #%zu SAVE sp=%p depth=%d "
                        "nArgs=%llu rcvr=0x%llx(%s) resume=%p "
                        "callerCM=0x%llx cls=%s sel=#%s\n",
                count, state->sp, state->j2jDepth,
                (unsigned long long)nArgs,
                (unsigned long long)state->receiver.rawBits(),
                rcvrKind.c_str(), savedResume,
                (unsigned long long)callerCM,
                cls.empty() ? "?" : cls.c_str(),
                sel.c_str());
    } else if (event == 2) {
        // Lookup selector + defining class of callerCM so the trace tells
        // you `ClassName>>selector` directly, no manual bisection needed.
        std::string sel;
        std::string cls;
        if (state->interp && callerCM > 0x10000) {
            Oop cm = Oop::fromRawBits(callerCM);
            sel = state->interp->memory().selectorOf(cm);
            cls = state->interp->classNameOfMethod(cm);
        }
        fprintf(stderr, "[B5] #%zu RET  sp=%p depth=%d "
                        "retVal=0x%llx callerCM=0x%llx savedArgs=%llu "
                        "cls=%s sel=#%s\n",
                count, state->sp, state->j2jDepth,
                (unsigned long long)extra1,
                (unsigned long long)callerCM, (unsigned long long)nArgs,
                cls.empty() ? "?" : cls.c_str(),
                sel.c_str());
    } else if (event == 3) {
        // Diagnostic event added 2026-04-22 to chase bug 14:
        // extra1 = resumeAddr (the tail-call target after J2J return)
        // extra2 = _sv->sp    (caller's sp captured at save time)
        fprintf(stderr, "[B5] #%zu TAIL resumeAddr=0x%llx savedSp=0x%llx\n",
                count, (unsigned long long)extra1, (unsigned long long)extra2);
    } else if (event == 4 || event == 5) {
        // Bug-14 event=4: returnReceiver detected SmallInt receiver.
        // Bug-14 event=5: returnTop detected SmallInt on TOS.
        // extra1 = retVal.bits (SmallInt)
        // extra2 = (uintptr_t)s->jitMethod
        uint64_t jmRaw = extra2;
        uint64_t cmOop = 0;
        std::string cls = "?", sel = "?";
        if (jmRaw && state->interp) {
            cmOop = *(uint64_t*)(uintptr_t)jmRaw;
            if (cmOop > 0x10000) {
                Oop cm = Oop::fromRawBits(cmOop);
                sel = state->interp->memory().selectorOf(cm);
                cls = state->interp->classNameOfMethod(cm);
            }
        }
        const char* tag = (event == 4) ? "RCVR" : "TOP";
        fprintf(stderr, "[B5-SMI-%s] #%zu retVal=0x%llx "
                        "jitMethod=0x%llx cmOop=0x%llx cls=%s sel=#%s "
                        "j2jDepth=%d sp=%p\n",
                tag, count, (unsigned long long)extra1,
                (unsigned long long)jmRaw, (unsigned long long)cmOop,
                cls.c_str(), sel.c_str(),
                state->j2jDepth, state->sp);
    } else {
        fprintf(stderr, "[B5] #%zu evt=%llu e1=0x%llx e2=0x%llx\n",
                count, (unsigned long long)event,
                (unsigned long long)extra1, (unsigned long long)extra2);
    }
}

extern "C" void jit_rt_return(JITState* state) {
    // exitReason and returnValue already set by the stencil.
    (void)state;
}

// Bug-14 diagnostic: called from the ASM trampoline's return path
// (TrampolineAsm.S, Ltramp_return) right before the save is popped.
// Logs the retVal being written + the popped save's key fields.
// Rate-limited and gated by PHARO_B5_TRACE.
extern "C" void pharo_jit_b5_tramp_ret(JITState* state, Interpreter::J2JSave* save) {
    if (!g_debug.b5Trace) return;
    static size_t count = 0;
    count++;
    if (count > 3000) return;
    Oop retVal = state->returnValue;
    std::string kind = retVal.isSmallInteger() ? "SmI"
        : retVal.isObject() && state->interp
            ? state->interp->memory().classNameOf(retVal).c_str()
            : "other";
    // Decode the caller's method (save->jitMethod->compiledMethodOop).
    uint64_t cmOop = 0;
    std::string cls = "?", sel = "?";
    if (save->jitMethod && state->interp) {
        cmOop = save->jitMethod->compiledMethodOop;
        if (cmOop > 0x10000) {
            Oop cm = Oop::fromRawBits(cmOop);
            sel = state->interp->memory().selectorOf(cm);
            cls = state->interp->classNameOfMethod(cm);
        }
    }
    fprintf(stderr, "[B5-TRAMP-RET-ASM] #%zu retVal=0x%llx(%s) "
                    "save.sp=%p save.nArgs=%d save.resume=%p "
                    "callerCM=0x%llx cls=%s sel=#%s\n",
            count, (unsigned long long)retVal.rawBits(), kind.c_str(),
            save->sp, (int)save->sendArgCount, save->resumeAddr,
            (unsigned long long)cmOop, cls.c_str(), sel.c_str());
}

extern "C" void jit_rt_arith_overflow(JITState* state) {
    // Arithmetic overflow: restore entry SP and re-execute the whole method.
    // The interpreter will handle LargeInteger arithmetic.
    state->exitReason = ExitArithOverflow;

    // Diagnostic: count how often the fallback fires per method. Enabled by
    // JIT_ARITH_OFLOW_TRACE=1. Prints a periodic summary at 10k firings.
    static const bool trace = []() {
        const char* v = std::getenv("JIT_ARITH_OFLOW_TRACE");
        return v && *v == '1';
    }();
    if (trace) {
        static size_t totalFirings = 0;
        totalFirings++;
        if (totalFirings <= 20 || (totalFirings % 100) == 0) {
            uint64_t methodOop = state->jitMethod ? state->jitMethod->compiledMethodOop : 0;
            std::string sel = state->interp ?
                state->interp->memory().selectorOf(Oop::fromRawBits(methodOop)) : "?";
            fprintf(stderr, "[ARITH-OFLOW] #%zu sel=#%s method=0x%llx sp=%p ip=%p\n",
                    totalFirings, sel.c_str(), (unsigned long long)methodOop,
                    (void*)state->sp, (void*)state->ip);
            fflush(stderr);
        }
    }
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

// Safe-point recompile queue.  stencil_sendJ2J's inline path bumps
// callee count; when threshold crossed, calls this helper to enqueue
// the callee for recompile.  Drained by Interpreter::drainRecompileQueue
// at periodic safe points (preemption check, ~every 64K interp steps).
//
// Single-threaded VM — no atomic.  Bounded ring buffer of 32 slots;
// overflow is silently dropped (worst case: missed recompile, no
// correctness issue).
constexpr size_t kRecompileQueueSize = 32;
static uint64_t g_recompileQueue[kRecompileQueueSize] = {0};
static size_t g_recompileQueueHead = 0;  // next free slot
static size_t g_recompileQueueDrained = 0;  // total processed (diag)

extern "C" void jit_rt_recompile_queue(void* calleeJM) {
    if (!calleeJM) return;
    // PHARO_NO_J2J_INLINE_BUMP=1: kill switch — if set, the helper is
    // a no-op so any latent regression caused by the inline-bump path
    // can be bisected away without re-extracting stencils.
    static const bool disabled =
        std::getenv("PHARO_NO_J2J_INLINE_BUMP") != nullptr;
    if (disabled) return;
    JITMethod* jm = reinterpret_cast<JITMethod*>(calleeJM);
    uint64_t methBits = jm->compiledMethodOop;
    if (methBits == 0) return;
    // Push to ring buffer.  If full, drop (caller can re-trigger by
    // bumping past threshold again — eventual consistency).
    size_t head = g_recompileQueueHead;
    if (head < kRecompileQueueSize) {
        // Dedup: skip if same method already queued.
        for (size_t i = 0; i < head; i++) {
            if (g_recompileQueue[i] == methBits) return;
        }
        g_recompileQueue[head] = methBits;
        g_recompileQueueHead = head + 1;
    }
}

void JITRuntime::noteLateSpecBit(JITMethod* callerJM, uint64_t newExtra) {
    if (!callerJM || !callerJM->stats) return;
    // Only meaningful once the method has been recompiled at least once
    // and is now tier=2 (initial compile already missed this bit).
    if (callerJM->tier != 2) return;
    // One-shot cap: each method can only be re-recompiled once via this
    // path.  Without it, repeat IC fills could trigger an unbounded
    // recompile loop.
    if (callerJM->stats->flags & kLateSpecRecompiledOnce) return;
    // Already at-or-past threshold: queued (or about to drain).  Don't
    // pay the ring-buffer dedup scan on every subsequent IC fill while
    // we wait for the safe-point drain.
    if (callerJM->stats->lateSpecCount >= kLateSpecRecompileThreshold) return;

    // Default-on after bench-suite validation 2026-05-03 (parity with
    // and without across 5+ runs of sort/sieve/fib/dict/sum/factorial).
    // PHARO_NO_LATE_SPEC_RECOMPILE=1 to opt out.
    static const bool disabled =
        std::getenv("PHARO_NO_LATE_SPEC_RECOMPILE") != nullptr;
    if (disabled) return;

    // Weight by classification value.  High-value bits (block-value,
    // multi-slot, returnsLiteral) unlock dedicated specialized stencils
    // worth a re-recompile on a single fill.  Plain J2J_ENTRY_BIT only
    // unlocks MonoJ2J spec (modest gain) — needs at least two fills to
    // be worth it.
    uint8_t weight = 0;
    if (newExtra & (1ULL << 59)) weight = 2;       // BLOCK_VALUE_BIT
    else if (newExtra & (1ULL << 57)) weight = 2;  // MULTI_SLOT
    else if (newExtra & (1ULL << 58)) weight = 2;  // RETURNS_LITERAL
    else if (newExtra & (1ULL << 60)) weight = 1;  // J2J_ENTRY_BIT
    if (weight == 0) return;

    uint16_t newCount = (uint16_t)callerJM->stats->lateSpecCount + weight;
    if (newCount > 0xFF) newCount = 0xFF;
    callerJM->stats->lateSpecCount = (uint8_t)newCount;

    if (callerJM->stats->lateSpecCount < kLateSpecRecompileThreshold) return;

    // Threshold crossed — queue for re-recompile.  Same ring buffer used
    // by the J2J inline-bump path (drained at safe points by Interpreter).
    uint64_t methBits = callerJM->compiledMethodOop;
    if (methBits == 0) return;
    size_t head = g_recompileQueueHead;
    if (head < kRecompileQueueSize) {
        for (size_t i = 0; i < head; i++) {
            if (g_recompileQueue[i] == methBits) return;  // already queued
        }
        g_recompileQueue[head] = methBits;
        g_recompileQueueHead = head + 1;
    }
}

// T2 monomorphic IC counters
int g_t2ICHits = 0;
int g_t2ICMisses = 0;

// Tier 2 inline send helper.
// Called from MIR-generated code at each send site.
//
// Pre-conditions (set by MIR code before the CALL):
//   state->cachedTarget  = selector Oop
//   state->sendArgCount  = nArgs
//   state->sp             = stack pointer (vstack flushed, args pushed)
//   state->ip             = send bytecode address (for chain-loop fallback)
//
// Post-conditions on success (ExitNone):
//   state->sp adjusted (receiver+args popped, retval pushed)
//   state->exitReason = ExitNone
//
// Post-conditions on fallback:
//   state->exitReason = ExitSend (chain loop handles from scratch)
//   OR callee's non-ExitReturn reason (SavedFrame pushed for T2 caller)
extern "C" void jit_t2_send(JITState* state) {
    using namespace pharo;
    using namespace pharo::jit;

    static int t2SendDepth = 0;
    // A1 chain-loop continuation is implemented but gated behind
    // PHARO_T2_A1=1 because T2 itself is currently disabled by default
    // (MIR holds stale oops across GC — see commit b18e71e). Once the T2
    // GC issue is resolved, flip A1 on to replace the correctness-safe
    // always-bail with the chain-loop callee invocation.
    static bool a1Enabled = g_debug.t2A1;

    Interpreter* interp = state->interp;
    ObjectMemory* mem = state->memory;

    // Depth guard: prevent unbounded C-stack growth from recursive T2 sends.
    if (t2SendDepth >= 200) {
        state->exitReason = ExitSend;
        return;
    }

    // Always-bail unless A1 is explicitly enabled. Correctness-safe default.
    if (!a1Enabled) {
        state->exitReason = ExitSend;
        return;
    }

    // A1: need J2J save slot for the caller so the chain loop can
    // materialize it on callee bail.
    if (!state->j2jSaveCursor || !state->j2jSaveLimit ||
        (uintptr_t)state->j2jSaveCursor + sizeof(Interpreter::J2JSave)
          > (uintptr_t)state->j2jSaveLimit) {
        state->exitReason = ExitSend;
        return;
    }
    Interpreter::J2JSave* saveEntry =
        reinterpret_cast<Interpreter::J2JSave*>(state->j2jSaveCursor);

    Oop selector = state->cachedTarget;
    int nArgs = state->sendArgCount;
    uint32_t bcLen = state->sendBCLength;
    Oop rcvr = state->sp[-(nArgs + 1)];

    t2SendDepth++;

    // --- T2 monomorphic IC: skip method lookup if receiver class matches ---
    Oop resolved;
    uint64_t* ic = reinterpret_cast<uint64_t*>(state->icDataPtr);
    if (ic && ic[0] != 0) {
        uint64_t bits = rcvr.rawBits();
        uint32_t classIdx = 0;
        if ((bits & 7) == 0 && bits != 0) {
            classIdx = static_cast<uint32_t>(
                *reinterpret_cast<uint64_t*>(bits) & 0x3FFFFFULL);
        } else if ((bits & 7) == 1) {
            classIdx = interp->jitRuntime().smallIntClassIdx;
        }
        if (classIdx != 0 && classIdx == static_cast<uint32_t>(ic[0])) {
            resolved = Oop::fromRawBits(ic[1]);
            g_t2ICHits++;
            goto ic_hit;
        }
    }

    {
        Oop rcvrClass = mem->classOf(rcvr);
        resolved = interp->lookupMethodForSend(selector, rcvrClass);
        if (!resolved.isObject() || resolved.rawBits() < 0x10000) {
            state->exitReason = ExitSend;
            t2SendDepth--;
            return;
        }
        g_t2ICMisses++;
        if (ic) {
            uint64_t bits = rcvr.rawBits();
            uint32_t classIdx = 0;
            if ((bits & 7) == 0 && bits != 0) {
                classIdx = static_cast<uint32_t>(
                    *reinterpret_cast<uint64_t*>(bits) & 0x3FFFFFULL);
            } else if ((bits & 7) == 1) {
                classIdx = interp->jitRuntime().smallIntClassIdx;
            }
            if (classIdx != 0) {
                ic[0] = classIdx;
                ic[1] = resolved.rawBits();
            }
        }
    }

ic_hit:
    // Callee must be JIT-compiled to run here.
    MethodMap* mm = reinterpret_cast<MethodMap*>(state->methodMapPtr);
    JITMethod* jm = mm->lookup(resolved.rawBits());
    if (!jm || !jm->isExecutable()) {
        state->exitReason = ExitSend;
        t2SendDepth--;
        return;
    }

    // Primitive methods need the T1 prim prologue path; if neither tier has
    // one, bail so the interpreter runs the primitive cleanly.
    bool hasPrim = (jm->methodHeader >> 16) & 1;
    if (hasPrim && !jm->hasPrimPrologue) {
        state->exitReason = ExitSend;
        t2SendDepth--;
        return;
    }
    if (resolved.asObjectPtr()->classIndex() != interp->compiledMethodClassIdx()) {
        state->exitReason = ExitSend;
        t2SendDepth--;
        return;
    }

    // === Push J2JSave for CALLER before setting up callee state ===
    // On non-ExitReturn bail, we leave this entry in the pool and the chain
    // loop materializes it as a SavedFrame. On ExitReturn we pop it.
    // Use post-send IP so resume (via T2 dispatch table) lands at continueLabel.
    JITMethod* callerJM = state->jitMethod;
    saveEntry->sp           = state->sp;
    saveEntry->receiver     = state->receiver;
    saveEntry->tempBase     = state->tempBase;
    saveEntry->ip           = state->ip + bcLen;
    saveEntry->jitMethod    = callerJM;
    saveEntry->resumeAddr   = callerJM ? callerJM->codeStart() : nullptr;
    saveEntry->sendArgCount = nArgs;
    state->j2jSaveCursor += sizeof(Interpreter::J2JSave);
    state->j2jDepth++;
    state->j2jTotalCalls++;

    // === Set up CALLEE state ===
    ObjectHeader* methObj = resolved.asObjectPtr();
    Oop* fp = state->sp - (nArgs + 1);

    state->receiver = rcvr;
    state->literals = methObj->slots() + 1;
    state->tempBase = fp + 1;
    state->argCount = nArgs;
    state->jitMethod = jm;
    state->method = resolved;
    state->icDataPtr = nullptr;
    state->sendArgCount = 0;

    int numLits = static_cast<int>(jm->methodHeader & 0x7FFF);
    state->ip = methObj->bytes() + (1 + numLits) * 8;

    int totalTemps = jm->tempCount;
    if (__builtin_expect(nArgs < totalTemps, 0)) {
        Oop nil = mem->nil();
        for (int i = nArgs; i < totalTemps; i++) {
            *state->sp = nil;
            state->sp++;
        }
    }

    // Keep j2jSaveCursor/limit — callee may push deeper entries.
    state->yieldCountdown = 1000;

    // Prefer T2 code; fall back to T1 stencil for leaf / prim-prologue callees.
    JITRuntime& rt = interp->jitRuntime();
    void* t2code = hasPrim ? nullptr : rt.lookupTier2(resolved.rawBits());
    size_t gcBefore = mem->statistics().gcCount;
    if (t2code && t2code != (void*)1) {
        state->exitReason = ExitNone;
        state->returnValue = Oop::fromRawBits(0xDEAD0001ULL);
        ((void(*)(JITState*))t2code)(state);
    } else {
        JIT_CALL(jm->codeStart(), state);
    }

    // GC during callee invalidates C-stack oops (selector, rcvr, etc.).
    // Our pushed save has GC-reachable oops (receiver, jitMethod-derived
    // compiledMethodOop) via the pool, which tryJITActivation scans.
    // State reflects callee — chain loop re-derives from Smalltalk stack.
    if (__builtin_expect(mem->statistics().gcCount != gcBefore, 0)) {
        state->exitReason = ExitSend;
        t2SendDepth--;
        return;
    }

    if (__builtin_expect(state->exitReason == ExitReturn, 1)) {
        // Fast path: pop our save, restore caller, return value on stack.
        state->j2jSaveCursor -= sizeof(Interpreter::J2JSave);
        state->j2jDepth--;

        Oop retVal = state->returnValue;
        state->sp = saveEntry->sp;
        state->receiver = saveEntry->receiver;
        state->tempBase = saveEntry->tempBase;
        state->jitMethod = saveEntry->jitMethod;
        state->ip = saveEntry->ip;
        if (saveEntry->jitMethod) {
            state->argCount = saveEntry->jitMethod->argCount;
            state->literals = reinterpret_cast<Oop*>(
                saveEntry->jitMethod->compiledMethodOop + 8);
            state->method = Oop::fromRawBits(
                saveEntry->jitMethod->compiledMethodOop);
        }

        state->sp[-(nArgs + 1)] = retVal;
        state->sp -= nArgs;

        state->exitReason = ExitNone;
        t2SendDepth--;
        return;
    }

    // Callee bailed (ExitSend / ExitSendCached / ExitYield / etc.).
    // LEAVE our save in the pool; the chain loop will materialize it and
    // any deeper saves the callee pushed. State reflects callee — that's
    // the correct current frame for the interpreter to resume from.
    state->exitReason = ExitSend;
    t2SendDepth--;
    return;
}

// Out-of-line IC-miss megacache probe + exit-state setup.
// todo.md §2.5: factoring this out of stencil_sendJ2J's rarely-
// taken ic_miss branch shrinks the hot stencil body.  The branch
// executes only on IC miss (~1-2% of sends on warm code), so the
// call + indirection cost here is amortised.
//
// Returns:
//   0 = megacache miss, state already set for EXIT_SEND
//   1 = megacache hit, state set for EXIT_SEND_CACHED
//   2 = megacache hit with J2J entry — caller does inline
//       j2j_direct_call with *out_extra / *out_methodBits.
// Cold-IC fill helper for stencil's mega-cache-hit path.
//
// CURRENTLY DEAD — wired up but the stencil call site was reverted on
// 2026-05-03.  Even with a tier=2 + slot-0-empty inline gate, the
// per-fill W^X flip (pthread_jit_write_protect_np ~few hundred
// cycles) caused sieve x100 to regress 44ms → 87ms (2×) and sort
// 100K to drift 214ms → 220ms.  Same root cause as the prior failed
// attempt at full out-of-lining (todo.md §2.5).
//
// Kept in tree as scaffolding: the helper, _HOLE_RT_FILL_IC,
// helpers.fillIC, extract_stencils ID 18, and the JITCompiler
// wire-up are all in place.  To activate: re-add a call to
// _HOLE_RT_FILL_IC in stencils.cpp's mega-hit branch (and re-extract
// stencils).  Future approaches that could make activation viable:
//   1. Move IC entries out of MAP_JIT into a separate RW zone so
//      the W^X flip becomes free (matches Linux behaviour today).
//   2. Batch IC fills in a deferred queue, drained at safe points.
//   3. Reduce per-flip cost by switching to per-page mprotect on
//      Linux/x86 only (Apple Silicon has no per-region toggle).
//
// Helper itself is correct: handles W^X, decodes primitive index for
// BLOCK_VALUE_BIT (207/209), and bumps late-spec count via
// noteLateSpecBit.  Gated by PHARO_NO_MEGAHIT_IC_FILL=1 kill switch.
extern "C" void jit_rt_fill_ic(JITState* s, uint64_t* icData,
                                uint64_t lookupKey, uint64_t extra,
                                uint64_t methodBits) {
    // PHARO_NO_MEGAHIT_IC_FILL=1: kill switch.  Set if the mega-cache
    // hit IC fill regresses a workload.
    static const bool disabled =
        std::getenv("PHARO_NO_MEGAHIT_IC_FILL") != nullptr;
    if (disabled) return;
    if (!icData || methodBits == 0) return;
    if (!s || !s->interp) return;
    JITRuntime* jr = &s->interp->jitRuntime();
    if (!jr) return;
    constexpr uint64_t BLOCK_VALUE_BIT = 1ULL << 59;
    // Find first empty slot — re-check because another path may have
    // filled slot 0 between the stencil's check and this call (no
    // synchronization on single-threaded VM, but defense-in-depth).
    int firstEmpty = -1;
    for (int e = 0; e < 6; e++) {
        if (icData[e * 3] == 0) { firstEmpty = e; break; }
        if (icData[e * 3] == lookupKey) return;  // already classified
    }
    if (firstEmpty < 0) return;

    // Decode callee primitive to add BLOCK_VALUE_BIT for prim 207/209.
    Oop methodOop = Oop::fromRawBits(methodBits);
    if (methodOop.isObject() && methodBits > 0x10000) {
        ObjectHeader* mh = methodOop.asObjectPtr();
        Oop hdr = mh->slotAt(0);
        if (hdr.isSmallInteger()
                && ((hdr.asSmallInteger() >> 16) & 1)) {
            int primIdx = s->interp->primitiveIndexOf(methodOop);
            if (primIdx == 207 || primIdx == 209) {
                extra |= BLOCK_VALUE_BIT;
            }
        }
    }

    // 2026-05-03: IC entries are heap-allocated (RW always); no W^X
    // flip needed.
    icData[firstEmpty * 3]     = lookupKey;
    icData[firstEmpty * 3 + 1] = methodBits;
    icData[firstEmpty * 3 + 2] = extra;

    // Account for late-spec opportunity on tier=2 callers.  Same hook
    // used by upgradeICToJ2J / patchJITICAfterSend.
    if (s->jitMethod) {
        jr->noteLateSpecBit(reinterpret_cast<JITMethod*>(s->jitMethod), extra);
    }
}

// NOTE (2026-05-03): this helper is currently DEAD CODE.  Stencils
// inline the mega-cache probe themselves (stencils.cpp:1715-1762)
// because out-of-lining regressed array-fill 15-20% (todo.md §2.5).
// Kept for future experiments.  Any IC-fill changes that need to fire
// on the mega-cache hot path must be added inline in the stencil
// (or via a new _HOLE_RT_* runtime call invoked from there) — adding
// them to this function does nothing.
extern "C" int jit_rt_ic_miss(
    JITState* s, uint64_t* icData, uint64_t lookupKey,
    int nArgs, int bcOffset,
    uint64_t* out_extra, uint64_t* out_methodBits)
{
    // Constants matching stencils.cpp
    constexpr uint64_t J2J_ENTRY_BIT  = 1ULL << 60;
    constexpr uint64_t J2J_ADDR_MASK  = 0x0000FFFFFFFFFFFFULL;
    // JITMethod header size used by stencils to find the methodObj
    // immediately before jitEntry (codeStart).
    constexpr int JM_SIZE_STENCIL = sizeof(JITMethod);
    constexpr int JM_STATE_OFFSET = 32;

    uint64_t selectorBits = icData[18];
    // Task #41: icData[18] is memset to zero on GC (stencil-probe safety —
    // the stencil IC loop derefs it as part of entry-slot iteration).
    // Recover the selector from the JIT method's side-channel array so the
    // megacache lookup doesn't bail out on every post-GC miss.  Don't
    // write the recovered value back into slot 18 — that would make the
    // stencil's null-check fall through to a crashing deref.
    if (selectorBits == 0 && s->jitMethod && s->jitMethod->numICEntries > 0) {
        JITMethod* jm = s->jitMethod;
        uint8_t* icStart = jm->icZoneStart();
        ptrdiff_t offset = reinterpret_cast<uint8_t*>(icData) - icStart;
        if (offset >= 0 && (offset % IC_BYTES_PER_SITE) == 0) {
            uint32_t siteIdx = static_cast<uint32_t>(offset / IC_BYTES_PER_SITE);
            if (siteIdx < jm->numICEntries) {
                if (uint64_t* sba = jm->selBitsArray()) {
                    selectorBits = sba[siteIdx];
                }
            }
        }
    }
    JITRuntime* jr = s->interp ? &s->interp->jitRuntime() : nullptr;
    if (selectorBits != 0 && jr) {
        MegaCacheEntry* cache = jr->megaCache();
        MegaCacheEntry* megaHit = nullptr;

        size_t hash = (size_t)(selectorBits ^ lookupKey) & (MegaCacheSize - 1);
        MegaCacheEntry* entry = &cache[hash];
        if (entry->selectorBits == selectorBits && entry->classIndex == lookupKey) {
            megaHit = entry;
        } else {
            size_t hash2 = (size_t)((selectorBits >> 3) ^ (lookupKey << 2) ^ lookupKey)
                           & (MegaCacheSize - 1);
            entry = &cache[hash2];
            if (entry->selectorBits == selectorBits && entry->classIndex == lookupKey) {
                megaHit = entry;
            }
        }
        if (megaHit) {
            if (megaHit->jitEntry != 0) {
                uint8_t* jm = reinterpret_cast<uint8_t*>(megaHit->jitEntry) - JM_SIZE_STENCIL;
                if (*(jm + JM_STATE_OFFSET) == 1) {
                    *out_extra = J2J_ENTRY_BIT | (megaHit->jitEntry & J2J_ADDR_MASK);
                    *out_methodBits = megaHit->methodBits;
                    return 2;
                }
            }
            s->cachedTarget = Oop::fromRawBits(megaHit->methodBits);
            s->icDataPtr = icData;
            s->sendArgCount = nArgs;
            s->ip = s->ip + bcOffset;
            s->exitReason = ExitSendCached;
            return 1;
        }
    }
    // Megacache miss
    s->icDataPtr = icData;
    s->sendArgCount = nArgs;
    s->ip = s->ip + bcOffset;
    s->exitReason = ExitSend;
    return 0;
}

// Pointer-object basicAt: for unhandled stencil_primAt formats
// (3 IndexableWithFixed, 4 Weak, 5 WeakWithFixed).  Computes
// fixedFieldCount via the receiver's class, validates the index,
// returns the slot oop.  Returns 1 on success (out written), 0 on
// OoB / bad receiver.  Stencil passes JITState* so we can reach
// memory via state->memory.
extern "C" int jit_rt_primat_ptr(JITState* s, uint64_t rcvBits,
                                  uint64_t i, uint64_t* out) {
    if ((rcvBits & 7) != 0 || rcvBits < 0x10000) return 0;
    auto* rh = reinterpret_cast<pharo::ObjectHeader*>(rcvBits);
    uint32_t fmt = (uint32_t)rh->format();
    size_t slotCount = rh->slotCount();

    // fmt 9: Indexable64 (DoubleWordArray, BoxedFloat64).  Each slot
    // holds one 64-bit word.  If the word fits in SmallInteger return
    // inline; otherwise allocate a LargePositiveInteger.
    if (fmt == 9) {
        if (i < 1 || i > slotCount) return 0;
        if (!s || !s->memory) return 0;
        pharo::ObjectMemory* mem = static_cast<pharo::ObjectMemory*>(s->memory);
        uint64_t bits = mem->fetchWord64((size_t)(i - 1),
            pharo::Oop::fromRawBits(rcvBits));
        if (bits <= (uint64_t)pharo::Oop::smallIntegerMax()) {
            *out = ((uint64_t)bits << 3) | 1;  // SmI tag
            return 1;
        }
        // Allocate LargePositiveInteger as 8-byte object.
        // Note: alloc may trigger GC.  We've already captured `bits`
        // locally, so the receiver moving doesn't affect us.  After
        // return, the stencil's J2J return path uses j2jSave entries
        // (already on the J2J pool which the GC walks) — safe.
        pharo::Oop intClass = mem->specialObject(
            pharo::SpecialObjectIndex::ClassLargePositiveInteger);
        uint32_t classIndex = mem->indexOfClass(intClass);
        pharo::Oop newOop = mem->allocateBytes(classIndex, 8);
        if (!newOop.isObject()) return 0;
        auto* nh = newOop.asObjectPtr();
        // Write little-endian
        for (int b = 0; b < 8; b++) {
            nh->bytes()[b] = (uint8_t)(bits >> (b * 8));
        }
        *out = newOop.rawBits();
        return 1;
    }

    // fmt 3/4/5: IndexableWithFixed / Weak.
    uint32_t classIdx = rh->classIndex();
    if (!s || !s->memory) return 0;
    pharo::ObjectMemory* mem = static_cast<pharo::ObjectMemory*>(s->memory);
    pharo::Oop classOop = mem->classAtIndex(classIdx);
    if (!classOop.isObject()) return 0;
    auto* classHdr = classOop.asObjectPtr();
    if (classHdr->slotCount() < 3) return 0;
    pharo::Oop instSpec = classHdr->slotAt(2);
    if (!instSpec.isSmallInteger()) return 0;
    size_t fixedFields = (size_t)(instSpec.asSmallInteger() & 0xFFFF);
    if (fixedFields > slotCount) return 0;
    size_t indexableSize = slotCount - fixedFields;
    if (i < 1 || i > indexableSize) return 0;
    size_t actualSlot = fixedFields + (i - 1);
    *out = rh->slots()[actualSlot].rawBits();
    return 1;
}

// Pointer-object basicAt:put: for fmt 3/4/5.  Returns 1 on success,
// 0 on OoB / bad receiver / immutable.
extern "C" int jit_rt_primatput_ptr(JITState* s, uint64_t rcvBits,
                                     uint64_t i, uint64_t valBits) {
    if ((rcvBits & 7) != 0 || rcvBits < 0x10000) return 0;
    auto* rh = reinterpret_cast<pharo::ObjectHeader*>(rcvBits);
    uint64_t header = *reinterpret_cast<uint64_t*>(rcvBits);
    if (header & (1ULL << 23)) return 0;  // immutable

    if (!s || !s->memory) return 0;
    pharo::ObjectMemory* mem = static_cast<pharo::ObjectMemory*>(s->memory);
    uint32_t classIdx = rh->classIndex();
    pharo::Oop classOop = mem->classAtIndex(classIdx);
    if (!classOop.isObject()) return 0;
    auto* classHdr = classOop.asObjectPtr();
    if (classHdr->slotCount() < 3) return 0;
    pharo::Oop instSpec = classHdr->slotAt(2);
    if (!instSpec.isSmallInteger()) return 0;
    size_t fixedFields = (size_t)(instSpec.asSmallInteger() & 0xFFFF);
    size_t slotCount = rh->slotCount();
    if (fixedFields > slotCount) return 0;
    size_t indexableSize = slotCount - fixedFields;
    if (i < 1 || i > indexableSize) return 0;
    size_t actualSlot = fixedFields + (i - 1);
    rh->slots()[actualSlot] = pharo::Oop::fromRawBits(valBits);
    // Note: write barrier for old-to-young pointer might be needed.
    // For now, defer to GC's full scan.  TODO: emit remember bit.
    return 1;
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

    // PHARO_INLINE_PRIM_DEBUG=1: log every inline at: failure with the
    // index and the receiver's slot count.  Needed because primitive 60
    // is called inline from JIT code, bypassing Interpreter::primitiveAt.
    static bool inlineDbg = std::getenv("PHARO_INLINE_PRIM_DEBUG") != nullptr;
    static int inlineLog = 0;

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
        if ((idxBits & 7) != 1) {
            if (inlineDbg && ++inlineLog <= 30) {
                fprintf(stderr,
                    "[INLINE-AT BADIDX #%d] idxBits=0x%llx (not SmI) "
                    "rcvFmt=%llu slots=%llu\n",
                    inlineLog, (unsigned long long)idxBits,
                    (unsigned long long)fmt, (unsigned long long)slotCount);
            }
            return 0;
        }
        int64_t i = (int64_t)idxBits >> 3;
        if (fmt == 2) {
            if (i < 1 || (uint64_t)i > slotCount) {
                if (inlineDbg && ++inlineLog <= 30) {
                    fprintf(stderr,
                        "[INLINE-AT OOB #%d] fmt=2 i=%lld slots=%llu rcv=0x%llx\n",
                        inlineLog, (long long)i,
                        (unsigned long long)slotCount,
                        (unsigned long long)rcvBits);
                }
                return 0;
            }
            Oop* slots = reinterpret_cast<Oop*>(rcvBits + 8);
            s->sp[-(nArgs + 1)] = slots[i - 1];
            s->sp -= nArgs;
            return 1;
        }
        if (fmt >= 16 && fmt <= 23) {
            uint64_t byteSize = slotCount * 8 - (fmt - 16);
            if (i < 1 || (uint64_t)i > byteSize) {
                if (inlineDbg && ++inlineLog <= 30) {
                    fprintf(stderr,
                        "[INLINE-AT OOB #%d] fmt=%llu(byte) i=%lld byteSize=%llu rcv=0x%llx\n",
                        inlineLog, (unsigned long long)fmt,
                        (long long)i, (unsigned long long)byteSize,
                        (unsigned long long)rcvBits);
                }
                return 0;
            }
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

// Out-of-line new/new: primitive handler for IC hit path.
// Called from sendJ2J stencil when primKind == 17 (new) or 18 (new:).
// info = (primKind << 8) | nArgs
// Returns 1 on success (new object pushed to sp), 0 on failure.
extern "C" uint64_t jit_rt_new_prim(JITState* s, uint64_t info) {
    uint8_t primKind = (uint8_t)(info >> 8);
    int nArgs = (int)(info & 0xFF);

    // Receiver is the class (at sp[-(nArgs+1)])
    uint64_t classBits = s->sp[-(nArgs + 1)].rawBits();
    if ((classBits & 7) != 0 || classBits < 0x10000)
        return 0;  // Not an object

    // For new: (primKind 18), size arg must be a SmallInteger
    int64_t indexableSize = 0;
    if (primKind == 18) {
        uint64_t sizeBits = s->sp[-nArgs].rawBits();
        if ((sizeBits & 7) != 1) return 0;
        indexableSize = (int64_t)sizeBits >> 3;
        if (indexableSize < 0) return 0;
    }

    // Read class format (slot 2 of the class object)
    // Slot layout: [superclass, methodDict, format, ...]
    // format encodes: bits 0-15 = fixedSize, bits 16-20 = instSpec
    Oop* classSlots = reinterpret_cast<Oop*>(classBits + 8);  // skip header
    uint64_t fmtBits = classSlots[2].rawBits();  // format is slot 2
    if ((fmtBits & 7) != 1) return 0;  // format must be SmallInteger
    int64_t format = (int64_t)fmtBits >> 3;

    int instSpec = (int)((format >> 16) & 0x1F);
    size_t fixedSize = format & 0xFFFF;

    if (primKind == 17) {
        // new: fixed-size class only (instSpec 0 or 1)
        if (instSpec >= 2) return 0;
        indexableSize = 0;
    } else {
        // new:: variable-size class required (instSpec >= 2)
        if (instSpec < 2) return 0;
    }

    // Compute slot count based on instance specification
    size_t slotCount;
    bool isBytes = instSpec >= 16;
    if (isBytes) {
        // Byte-indexed (ByteString, ByteArray, etc.)
        slotCount = ((size_t)indexableSize + 7) / 8;
    } else if (instSpec >= 10 && instSpec <= 11) {
        // 32-bit words
        slotCount = ((size_t)indexableSize * 4 + 7) / 8;
    } else if (instSpec >= 12 && instSpec <= 15) {
        // 16-bit words
        slotCount = ((size_t)indexableSize * 2 + 7) / 8;
    } else if (instSpec == 9) {
        // 64-bit words
        slotCount = (size_t)indexableSize;
    } else {
        // Pointer-indexed (Array, etc.) or fixed+variable
        slotCount = fixedSize + (size_t)indexableSize;
    }

    // Limit: don't inline huge allocations
    if (slotCount > 1024) return 0;

    // Compute object format for header
    uint8_t objFormat;
    if (isBytes) {
        size_t padding = slotCount * 8 - (size_t)indexableSize;
        objFormat = (uint8_t)(16 + padding);  // ObjectFormat::Indexable8 + padding
    } else if (instSpec >= 12 && instSpec <= 15) {
        size_t padding = slotCount * 8 / 2 - (size_t)indexableSize;
        objFormat = (uint8_t)(12 + padding);
    } else if (instSpec >= 10 && instSpec <= 11) {
        size_t padding = slotCount * 8 / 4 - (size_t)indexableSize;
        objFormat = (uint8_t)(10 + padding);
    } else if (instSpec == 9) {
        objFormat = 9;
    } else if (slotCount == 0 && fixedSize == 0) {
        objFormat = 0;  // zero-size
    } else if (instSpec >= 2) {
        objFormat = 2;  // variable pointer (Array)
    } else {
        objFormat = 1;  // fixed pointer
    }

    // Need overflow slot for > 254 slots
    bool hasOverflow = slotCount >= 255;
    size_t headerSize = 8 + (hasOverflow ? 8 : 0);
    size_t totalSize = headerSize + slotCount * 8;
    totalSize = (totalSize + 7) & ~7ULL;
    if (totalSize < 16) totalSize = 16;

    // Get class index
    ObjectMemory& mem = s->interp->memory();
    uint32_t classIndex = mem.indexOfClass(Oop::fromRawBits(classBits));
    if (classIndex == 0) return 0;

    // Bump allocation from old space
    uint8_t* freePtr = mem.oldSpaceFree();
    uint8_t* endPtr = freePtr + totalSize;
    if (endPtr > mem.oldSpaceEnd()) return 0;

    // Allocate
    ObjectHeader* obj;
    if (hasOverflow) {
        uint64_t* overflow = reinterpret_cast<uint64_t*>(freePtr);
        *overflow = slotCount | (0xFFULL << 56);
        obj = reinterpret_cast<ObjectHeader*>(freePtr + 8);
    } else {
        obj = reinterpret_cast<ObjectHeader*>(freePtr);
    }

    // Build header: slotCount | hash | format | classIndex | flags
    uint8_t headerSlots = hasOverflow ? 255 : (uint8_t)slotCount;
    uint64_t header = ObjectHeader::makeHeader(headerSlots, 0,
        static_cast<ObjectFormat>(objFormat), classIndex);
    *reinterpret_cast<uint64_t*>(obj) = header;

    // Zero-fill all slots (includes nil for pointer objects, 0 for bytes)
    if (slotCount > 0) {
        std::memset(reinterpret_cast<uint8_t*>(obj) + 8, 0, slotCount * 8);
    }

    // For pointer objects, fill with nil instead of zero
    if (!isBytes && instSpec < 9) {
        uint64_t nilBits = mem.nil().rawBits();
        if (nilBits != 0) {  // nil is Oop(0) in our encoding, so skip if 0
            Oop* slots = reinterpret_cast<Oop*>(reinterpret_cast<uint8_t*>(obj) + 8);
            for (size_t i = 0; i < slotCount; i++) {
                slots[i] = Oop::fromRawBits(nilBits);
            }
        }
    }

    // Commit allocation
    mem.setOldSpaceFreePointer(endPtr);

    // Convert to Oop
    Oop newObj = mem.oopFromPointer(obj);

    // Push result: replace receiver (and arg for new:) with new object
    s->sp[-(nArgs + 1)] = newObj;
    s->sp -= nArgs;
    return 1;
}

// Sista runtime helper for kBlockCreate.
//
// Called from Sista-compiled code via asmjit cc.invoke.  Mirrors the
// PushFullBlock interpreter handler — JIT'd code has pushed the
// block's copied values (and possibly an extra receiver) onto
// state->sp, then calls this helper.  We trampoline through
// Interpreter::jitSistaCreateFullBlock which syncs stackPointer_,
// invokes createFullBlockWithLiteral (allocates, may GC, pops the
// consumed operands, pushes the new block), then pops the block back
// off so subsequent compiled IR sees a clean stack.  The block oop is
// returned to the JIT'd caller via x0/r0.
//
// GC safety: the helper synchronizes interp state before the
// allocation, so the GC walker can find oops on the interp stack.
// JIT-side register-resident oops aren't walkable — callers of this
// helper must spill any live oops to interp stack before invoke.
//
// Today's use: kBlockCreate's lowering replaces the bail-only path
// with a real call here, so compiled execution can continue past
// PushFullBlock instead of exiting to the interpreter.
// Sista runtime helpers for Array operations — used by B2 splice.
extern "C" uint64_t jit_rt_sista_basic_size(JITState* state,
                                              uint64_t rcvBits) {
    if (!state || !state->interp) return 0;
    return state->interp->jitSistaBasicSize(state, rcvBits);
}

extern "C" uint64_t jit_rt_sista_basic_at(JITState* state,
                                            uint64_t rcvBits,
                                            uint64_t idxBits) {
    if (!state || !state->interp) return 0;
    return state->interp->jitSistaBasicAt(state, rcvBits, idxBits);
}

extern "C" uint64_t jit_rt_sista_basic_at_put(JITState* state,
                                                uint64_t rcvBits,
                                                uint64_t idxBits,
                                                uint64_t valBits) {
    if (!state || !state->interp) return 0;
    return state->interp->jitSistaBasicAtPut(state, rcvBits, idxBits,
                                              valBits);
}

// B-1 helper: synchronously invoke a send and return its result.
// Replaces the old kSendUnspeculated bail-and-exit path so compiled
// code can continue after sends.  See jitSistaCallSend in
// Interpreter.cpp for caveats around NLR / process switches / GC.
extern "C" uint64_t jit_rt_sista_call_send(JITState* state,
                                             uint64_t selBits,
                                             uint64_t nArgs) {
    if (!state || !state->interp) return 0;
    // Diagnostic: PHARO_SISTA_HELPER_FORCE_BAIL=1 forces every
    // helper-send to return 0, exercising the deopt path on every
    // call.  Confirmed (2026-04-29) that the deopt path is correct
    // — under FORCE_BAIL=1 the eval smoke + larger eval succeed
    // cleanly.  The remaining B7 bug is in the helper SUCCESS path
    // (slow per-call frame leak, ~0.5 fd accumulation per call).
    static const bool forceBail =
        std::getenv("PHARO_SISTA_HELPER_FORCE_BAIL") != nullptr;
    if (forceBail) return 0;
    return state->interp->jitSistaCallSend(state, selBits, nArgs);
}

// SpecialSend (0x70-0x7F) variant of the helper-send.  Selector lives
// in the global SpecialSelectorsArray, slot (ssIdx + 16) * 2.
// Resolves the selector once per call and routes through the same
// jitSistaCallSend path.  Lets Sista lift past SpecialSends for the
// Phase 6 body-triplet splice (which needs to reach preLoopStart even
// when the prologue has SpecialSend sends like `OC new`).
extern "C" uint64_t jit_rt_sista_special_call_send(JITState* state,
                                                    uint64_t ssIdx,
                                                    uint64_t nArgs) {
    if (!state || !state->interp || !state->memory) return 0;
    static const bool forceBail =
        std::getenv("PHARO_SISTA_HELPER_FORCE_BAIL") != nullptr;
    if (forceBail) return 0;
    Oop ssArrayOop = state->memory->specialObject(
        SpecialObjectIndex::SpecialSelectorsArray);
    if (!ssArrayOop.isObject() || ssArrayOop.rawBits() < 0x10000) return 0;
    ObjectHeader* ssHdr = ssArrayOop.asObjectPtr();
    size_t selSlot = (size_t)(ssIdx + 16) * 2;
    if (selSlot >= ssHdr->slotCount()) return 0;
    Oop sel = ssHdr->slotAt(selSlot);
    if (sel.isNil()) return 0;
    return state->interp->jitSistaCallSend(state, sel.rawBits(), nArgs);
}

// kStoreInstVar lowering helper: write a value into a heap object's
// instance-variable slot with the safety guards setReceiverInstVar
// uses.  Returns 1 on success, 0 if the store was refused (non-object
// / immutable / bytes / OOB).  See Interpreter::jitStoreInstVar for
// the full guard set.
extern "C" uint64_t jit_rt_store_inst_var(JITState* state,
                                            uint64_t recvBits,
                                            uint64_t ivarIdx,
                                            uint64_t valBits) {
    if (!state || !state->interp) return 0;
    return state->interp->jitStoreInstVar(
        Oop::fromRawBits(recvBits), ivarIdx,
        Oop::fromRawBits(valBits));
}

// Compiled splice's per-iter SmI tag-check, on miss, invokes this helper
// instead of bailing to PushFullBlock — preserving iters 0..startIdx-1
// of correct work.  See Interpreter::jitSistaCompleteArrayDoAccum.
extern "C" uint64_t jit_rt_sista_complete_array_do_accum(
    JITState* state,
    uint64_t rcvBits,
    uint64_t vecBits,
    uint64_t slotByteOff,
    uint64_t startIdx,
    uint64_t accBits,
    uint64_t arithCode) {
    if (!state || !state->interp) return 0;
    static const bool forceBail =
        std::getenv("PHARO_SISTA_DOACCUM_FORCE_BAIL") != nullptr;
    if (forceBail) return 0;
    return state->interp->jitSistaCompleteArrayDoAccum(
        state, rcvBits, vecBits, slotByteOff, startIdx, accBits,
        arithCode);
}

// Sista deopt-with-resume completion helper for canonical-shape
// kCountedLoopInjectInto.  Same architecture as do-accum's helper but
// for `[:acc :e | acc OP e]` blocks; returns final accumulator.
extern "C" uint64_t jit_rt_sista_complete_array_inject_into(
    JITState* state,
    uint64_t rcvBits,
    uint64_t startIdx,
    uint64_t accBits,
    uint64_t arithCode) {
    if (!state || !state->interp) return 0;
    static const bool forceBail =
        std::getenv("PHARO_SISTA_INJECT_RESUME_FORCE_BAIL") != nullptr;
    if (forceBail) return 0;
    return state->interp->jitSistaCompleteArrayInjectInto(
        state, rcvBits, startIdx, accBits, arithCode);
}

// Sista deopt-with-resume completion helper for canonical-shape
// kCountedLoopArrayCollect.  Block matches `[:e | e OP const]`.
extern "C" uint64_t jit_rt_sista_complete_array_collect(
    JITState* state,
    uint64_t rcvBits,
    uint64_t resultBits,
    uint64_t startIdx,
    uint64_t constBits,
    uint64_t arithCode) {
    if (!state || !state->interp) return 0;
    static const bool forceBail =
        std::getenv("PHARO_SISTA_COLLECT_RESUME_FORCE_BAIL") != nullptr;
    if (forceBail) return 0;
    return state->interp->jitSistaCompleteArrayCollect(
        state, rcvBits, resultBits, startIdx, constBits, arithCode);
}

extern "C" uint64_t jit_rt_sista_alloc_array(JITState* state,
                                               uint64_t size) {
    if (!state || !state->interp) return 0;
    static const bool dbg =
        std::getenv("PHARO_SISTA_ALLOC_ARRAY_TRACE") != nullptr;
    if (dbg) {
        static size_t calls = 0;
        if (++calls < 8 || (calls & 0xFFFF) == 0) {
            std::fprintf(stderr,
                "[ALLOC-ARRAY] calls=%zu size=%llu\n",
                calls, (unsigned long long)size);
        }
    }
    return state->interp->jitSistaAllocArray(state, size);
}

extern "C" uint64_t jit_rt_sista_block_create(JITState* state,
                                                uint64_t litIndex,
                                                uint64_t numCopied,
                                                uint64_t flags) {
    if (!state || !state->interp) return 0;
    static const bool dbg =
        std::getenv("PHARO_SISTA_BLOCK_HELPER_TRACE") != nullptr;
    static size_t calls = 0;
    calls++;
    if (dbg && (calls < 8 || (calls & 0xFFF) == 0)) {
        std::fprintf(stderr,
            "[SISTA-BLOCK-HELPER] calls=%zu lit=%llu n=%llu fl=0x%llx\n",
            calls, (unsigned long long)litIndex,
            (unsigned long long)numCopied, (unsigned long long)flags);
    }
    return state->interp->jitSistaCreateFullBlock(
        state, (int)litIndex, (int)numCopied, (int)flags);
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

        // Bump callee's executionCount and trigger recompile if
        // threshold crossed.  Catches J2J-only callees (Array>>do:,
        // etc.) that never reach tryExecute and thus stay tier=1
        // forever.  Past attempt (2026-04-30) bumped CALLER without
        // splice gate — broke splice race.  This variant uses
        // callee's cached isSpliceTarget flag for race-free gating.
        // Default-on (commit 2026-05-02) after 10/10 bench-suite
        // validation showed neutral-to-positive impact (fib -1ms,
        // sum -3ms, blocks -2ms; no regressions).  Opt out via
        // PHARO_NO_J2J_CALLEE_BUMP=1.
        static const bool calleeBumpDisabled =
            std::getenv("PHARO_NO_J2J_CALLEE_BUMP") != nullptr;
        if (!calleeBumpDisabled) {
            // Note: this code path is rarely reached.  stencil_sendJ2J's
            // INLINE j2j_direct_call (stencils.cpp line 1569+) is the hot
            // path and does save/BLR/restore directly.  jit_rt_j2j_call
            // (this function) is a fallback for the few callers that need
            // it — ~0.0006% of J2J calls in bench-suite runs (84/15M).
            // So the bump here only catches a tiny minority of J2J calls.
            // To recompile hot J2J-only callees (benchFib, Array>>do:),
            // would need to bump from inside the inline path — but past
            // attempts hit the splice race (see comment at line 1600 in
            // stencils.cpp) and project_j2j_callee_bump_2026_05_02.md.
            JITMethod* callee = reinterpret_cast<JITMethod*>(
                entryAddr - sizeof(JITMethod));
            if (callee->stats && !callee->isSpliceTarget
                && callee->tier == 1 && callee->numICEntries > 0) {
                uint32_t newCount = ++(callee->stats->executionCount);
                // Use >= to handle the race where noteMethodEntry +
                // this bump both increment (skipping the threshold).
                // tier==1 guarantees recompile fires at most once
                // (recompile sets tier=2).
                if (newCount >= (uint32_t)g_debug.recompileAt) {
                    Oop calleeMethod =
                        Oop::fromRawBits(callee->compiledMethodOop);
                    interp->jitRuntime().maybeRecompileForOSR(calleeMethod);
                }
            }
        }
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
    delete tier2Compiler_;
    delete compiler_;
}

bool JITRuntime::initialize(ObjectMemory& memory, Interpreter& interp) {
    if (initialized_) return true;

    // Bug 11b layer 5 sentinel: hand-coded JM_SIZE in TrampolineAsm.S
    // (an `.S` file — can't use `sizeof`) MUST equal the real
    // sizeof(JITMethod).  If layout drifts, every asm trampoline
    // computes `add Xn, methodHdr, #JM_SIZE` to the wrong address.
    // Fail loudly at startup instead of crashing later.  On non-arm64
    // architectures the platform sentinel returns sizeof itself, so
    // the check is a tautology — but keeping the call site uniform
    // means there's no arch divergence in VM core.
    {
        uint64_t sentinel = pharo::platform::jitTrampolineJMSize();
        if (sentinel != sizeof(JITMethod)) {
            fprintf(stderr, "[JIT] FATAL: TrampolineAsm.S JM_SIZE=%llu != sizeof(JITMethod)=%zu — "
                    "stencil math will land outside method code.  Update JM_SIZE in TrampolineAsm.S.\n",
                    (unsigned long long)sentinel, sizeof(JITMethod));
            return false;
        }
    }

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
    helpers.newPrim = reinterpret_cast<void*>(&jit_rt_new_prim);
    helpers.icMiss = reinterpret_cast<void*>(&jit_rt_ic_miss);
    // Use the no-op trace when no diagnostic flag is active — the real
    // function checks `if (event == 99 || event == 100)` then `if (!trace)
    // return`, which is ~5-10 cycles per send.  At 1M sends/bench that's
    // ~10ms on the hot path.  PHARO_B5_TRACE / PHARO_PRIMAT_OOB flip to
    // the real impl.
    bool needTrace = g_debug.b5Trace
                   || std::getenv("PHARO_PRIMAT_OOB") != nullptr;
    helpers.j2jTrace = reinterpret_cast<void*>(needTrace
        ? &jit_rt_j2j_trace
        : &jit_rt_j2j_trace_noop);
    helpers.primAtPtr = reinterpret_cast<void*>(&jit_rt_primat_ptr);
    helpers.primAtPutPtr = reinterpret_cast<void*>(&jit_rt_primatput_ptr);
    helpers.recompileQueue = reinterpret_cast<void*>(&jit_rt_recompile_queue);
    helpers.fillIC = reinterpret_cast<void*>(&jit_rt_fill_ic);
    compiler_->setHelpers(helpers);

    // Create Tier 2 compiler (asmjit-based)
    tier2Compiler_ = new Tier2Compiler(codeZone_, methodMap_, memory, interp);
    if (!tier2Compiler_->initialize()) {
        fprintf(stderr, "[JIT] Warning: Tier 2 compiler failed to initialize\n");
        delete tier2Compiler_;
        tier2Compiler_ = nullptr;
    } else {
        // §1.3a: let Tier2 query T1's send-site map for shared IC.
        tier2Compiler_->setT1Compiler(compiler_);
    }

    // After MAP_JIT mmap with PROT_EXEC, the initial W^X state might be
    // "executable" rather than "writable". Ensure we start in writable mode
    // so allocate() can zero the memory.
    makeWritable(codeZone_.rawStart(), codeZone_.totalBytes());

    // FullBlockClosure classIndex is resolved lazily — see
    // resolveFullBlockClosureClassIndex() (some images don't have the
    // class table fully populated until startup completes).
    fullBlockClosureClassIndex_ = 0;

    initialized_ = true;
    fprintf(stderr, "[JIT] Initialized: %zu MB code zone at %p\n",
            codeZone_.totalBytes() / (1024 * 1024),
            (void*)codeZone_.rawStart());

    return true;
}

uint32_t JITRuntime::resolveFullBlockClosureClassIndex() {
    if (fullBlockClosureClassIndex_ != 0) return fullBlockClosureClassIndex_;
    if (!interp_) return 0;
    // Reuse Interpreter's cached value (resolved via name-based class
    // table lookup at startup).  Avoids reimplementing the same scan
    // and matches the value used by the closure-creation path.
    fullBlockClosureClassIndex_ = interp_->fullBlockClosureClassIndex();
    return fullBlockClosureClassIndex_;
}

void JITRuntime::updateSpecialOops(ObjectMemory& memory) {
    nilOopBits = Oop::nil().rawBits();
    trueOopBits = memory.trueObject().rawBits();
    falseOopBits = memory.falseObject().rawBits();
    // SmallInteger classIndex for T2 IC (identityHash of SmallInteger class)
    Oop siClass = memory.specialObject(SpecialObjectIndex::ClassSmallInteger);
    if (siClass.isObject()) {
        smallIntClassIdx = siClass.asObjectPtr()->identityHash();
    }
}

size_t JITRuntime::drainRecompileQueue() {
    size_t head = g_recompileQueueHead;
    if (head == 0) return 0;
    size_t processed = 0;
    for (size_t i = 0; i < head; i++) {
        uint64_t methBits = g_recompileQueue[i];
        if (methBits == 0) continue;
        Oop method = Oop::fromRawBits(methBits);
        if (maybeRecompileForOSR(method)) processed++;
        g_recompileQueueDrained++;
    }
    // Reset queue.
    g_recompileQueueHead = 0;
    return processed;
}

// Initial-compile queue.  Methods that crossed the JIT threshold during
// interp dispatch are pushed here (instead of being compiled inline in
// noteMethodEntry).  Drained at the safe point alongside
// drainRecompileQueue, so compile happens between bytecodes — never
// mid-bytecode while interp local state is in flux.
//
// The 256-slot ring is bigger than g_recompileQueue (32) because initial
// compile is the common case: every method that crosses threshold during
// startup or eval-body execution lands here.  Overflow drops silently
// (the method's next call will re-cross the count == threshold check
// and re-queue if there's room).
constexpr size_t kInitialCompileQueueSize = 256;
static uint64_t g_initialCompileQueue[kInitialCompileQueueSize] = {0};
static size_t g_initialCompileQueueHead = 0;
static size_t g_initialCompileQueueDrained = 0;
static size_t g_initialCompileQueueDropped = 0;

void JITRuntime::queueInitialCompile(Oop compiledMethod) {
    uint64_t methBits = compiledMethod.rawBits();
    if (methBits == 0) return;
    size_t head = g_initialCompileQueueHead;
    if (head < kInitialCompileQueueSize) {
        for (size_t i = 0; i < head; i++) {
            if (g_initialCompileQueue[i] == methBits) return;  // dedup
        }
        g_initialCompileQueue[head] = methBits;
        g_initialCompileQueueHead = head + 1;
    } else {
        g_initialCompileQueueDropped++;
    }
}

size_t JITRuntime::drainInitialCompileQueue() {
    size_t head = g_initialCompileQueueHead;
    if (head == 0) return 0;
    size_t processed = 0;
    // Reset head BEFORE compiling.  compile() can call back into
    // noteMethodEntry (e.g., via scavenging GC during compile), and
    // we want re-entrant queue pushes to land in fresh slots, not
    // overwrite ones we're about to drain.
    g_initialCompileQueueHead = 0;
    uint64_t snapshot[kInitialCompileQueueSize];
    for (size_t i = 0; i < head; i++) {
        snapshot[i] = g_initialCompileQueue[i];
        g_initialCompileQueue[i] = 0;
    }
    Oop nilOop = interp_ ? interp_->memory().nil() : Oop::nil();
    Oop trueOop = interp_ ? interp_->memory().trueObject() : Oop::nil();
    Oop falseOop = interp_ ? interp_->memory().falseObject() : Oop::nil();
    for (size_t i = 0; i < head; i++) {
        uint64_t methBits = snapshot[i];
        if (methBits == 0) continue;
        Oop method = Oop::fromRawBits(methBits);
        if (!method.isObject() || method.rawBits() < 0x10000) continue;
        // Skip nil/true/false (deferred.md A1 P0): startup-window
        // queueing can race with GC and end up with a stale oop that
        // happens to land on a special object (nil = heap base).
        // Calling compile() with nil crashes deep in IC setup
        // (lldb-confirmed at JITCompiler.cpp:2402, fault addr 0x90).
        if (method.rawBits() == nilOop.rawBits()
            || method.rawBits() == trueOop.rawBits()
            || method.rawBits() == falseOop.rawBits()) {
            g_initialCompileQueueDrained++;
            continue;
        }
        // Validate it's actually a CompiledMethod by checking the
        // header is a SmallInteger (CompiledMethod's slot 0 is the
        // method header, always a SmallInt).
        if (interp_ && interp_->memory().isValidPointer(method)) {
            ObjectHeader* h = method.asObjectPtr();
            if (h->slotCount() < 1) {
                g_initialCompileQueueDrained++;
                continue;
            }
            Oop hdr = h->slotAt(0);
            if (!hdr.isSmallInteger()) {
                g_initialCompileQueueDrained++;
                continue;
            }
        } else {
            g_initialCompileQueueDrained++;
            continue;
        }
        // Skip if already compiled (race against another path).
        if (methodMap_.lookup(method.rawBits())) {
            g_initialCompileQueueDrained++;
            continue;
        }
        // Skip Sista-spliced methods (race-avoidance — same gate as in
        // noteMethodEntry's direct path).
        if (sistaRuntimeForGCHook_
            && sistaRuntimeForGCHook_->hasSplice(method)) {
            g_initialCompileQueueDrained++;
            continue;
        }
        compiler_->compile(method);
        processed++;
        g_initialCompileQueueDrained++;

        // Tier-2 (asmjit) hook on the queue-compile drain path.  The
        // legacy non-queue compile path (line 2607+) invokes T2 right
        // after T1 finishes; without this hook, T2 only fires under
        // PHARO_NO_QUEUE_COMPILE=1 (which has its own flakiness — see
        // memory/early_defer_lift_flaky.md).  Same gate set as legacy:
        //   - PHARO_T2=1                 strict opt-in
        //   - !tier2Lookup               not already T2-compiled
        //   - methodsCompiled < t2Limit  PHARO_T2_LIMIT cap
        //   - !skipCoexist               PHARO_T2_REPLACE=1 needed when
        //                                 the just-compiled T1 method is
        //                                 executable (which it usually is
        //                                 here — drain just compiled it)
        //   - warmup gate                PHARO_T2_WARMUP=0 to bypass; at
        //                                 drain time stats->executionCount
        //                                 is 0 so warmup blocks otherwise
        if (tier2Compiler_) {
            static bool noT2 = []() {
                const char* v = std::getenv("PHARO_T2");
                return !(v && v[0] == '1');
            }();
            static int t2Limit  = g_debug.t2Limit;
            static int t2Warmup = g_debug.t2Warmup;
            static bool t2ReplaceNote = g_debug.t2Replace;
            uint64_t key = method.rawBits();
            if (!noT2 && !tier2Lookup(key)
                && (int)tier2Compiler_->methodsCompiled() < t2Limit) {
                JITMethod* jm = methodMap_.lookup(key);
                bool skipCoexist = !t2ReplaceNote && jm && jm->isExecutable();
                bool warmupBlocks = (t2Warmup > 0 && jm && jm->stats
                    && (int)jm->stats->executionCount < t2Warmup);
                if (!skipCoexist && !warmupBlocks) {
                    void* t2code = tier2Compiler_->compile(method, jm);
                    tier2Insert(key, t2code ? t2code : (void*)1);
                    if (t2code && interp_) {
                        std::string sel = interp_->memory().selectorOf(method);
                        fprintf(stderr,
                            "[JIT] Tier 2 compiled method %p '%s' (%zu total) [drain]\n",
                            (void*)key, sel.c_str(),
                            tier2Compiler_->methodsCompiled());
                    }
                }
            }
        }
    }
    return processed;
}

bool JITRuntime::maybeRecompileForOSR(Oop compiledMethod) {
    if (!initialized_ || !compiler_) return false;
    if (!compiledMethod.isObject() || compiledMethod.rawBits() < 0x10000)
        return false;
    JITMethod* jm = methodMap_.lookup(compiledMethod.rawBits());
    if (!jm || !jm->isExecutable()) return false;
    if (jm->numICEntries == 0) return false;
    // Tier gate.  T1 → unconditional recompile (the original OSR-recompile
    // path).  T2 → only allowed for the late-spec one-shot path: caller
    // must have crossed kLateSpecRecompileThreshold and not yet been
    // re-recompiled.  See JITRuntime::noteLateSpecBit.
    bool lateSpec = false;
    if (jm->tier == 1) {
        // first-recompile path
    } else if (jm->tier == 2 && jm->stats
               && !(jm->stats->flags & kLateSpecRecompiledOnce)
               && jm->stats->lateSpecCount >= kLateSpecRecompileThreshold) {
        lateSpec = true;
    } else {
        return false;
    }

    // Sista already owns this method via a counted-loop splice — don't
    // race with it.  applyICSpecialization (run during recompile) emits
    // stencil_sendInlineMonoJ2J for monomorphic sites; entering that T1
    // code via OSR forces a slower per-iter dispatch than Sista's splice.
    // A/B (2026-04-30, bench panel best-of-5):
    //   default                          5/7/7/7/7/5
    //   PHARO_OSR_RECOMPILE=1            6/11/11/11/11/7  (regression)
    //   PHARO_OSR_RECOMPILE=1 + this gate 6/9/9/9/9/6     (partial fix)
    //   PHARO_OSR_RECOMPILE=1 +
    //     PHARO_NO_MONOJ2J_SPEC=1        5/7/7/7/7/5      (parity)
    // The same hasSplice gate is already in noteMethodEntry to fix the
    // T1-vs-Sista race; mirror it here for consistency.  Residual ~2 ms
    // regression comes from `do:` (no splice) being recompiled and its
    // MonoJ2J-specialized callees becoming a slower hot path; not yet
    // resolved.  See memory/project_t1_vs_sista_race.md and
    // memory/project_osr_recompile_regression.md.
    if (sistaRuntimeForGCHook_
        && sistaRuntimeForGCHook_->hasSplice(compiledMethod)) {
        return false;
    }

    // Check that at least one IC entry has data — otherwise there's no
    // specialization opportunity and recompile would just produce the
    // same code (waste of code-zone memory).  A "high water mark"
    // version requiring N% IC fill was tried 2026-04-30 but regressed
    // bench panel by ~14% — the extra wait pushed methods further into
    // interp mode and the larger fill threshold didn't unlock enough
    // additional specializations to compensate.  See deferred B10.
    uint8_t* icStart = jm->icZoneStart();
    bool anyData = false;
    for (uint32_t i = 0; i < jm->numICEntries; i++) {
        uint64_t* slots = reinterpret_cast<uint64_t*>(
            icStart + i * IC_BYTES_PER_SITE);
        if (slots[0] != 0) { anyData = true; break; }
    }
    if (!anyData) return false;

    static const bool traceRecompile =
        std::getenv("PHARO_JIT_TRACE_RECOMPILE") != nullptr;
    static const char* dumpICSel =
        std::getenv("PHARO_DUMP_RECOMPILE_IC");
    std::string sel;
    if (traceRecompile || dumpICSel) {
        sel = interp_->memory().selectorOf(compiledMethod);
    }
    if (traceRecompile) {
        fprintf(stderr,
                "[RECOMPILE-OSR] %s (icEntries=%u execCount=%u%s)\n",
                sel.c_str(), jm->numICEntries,
                jm->stats ? jm->stats->executionCount : 0,
                lateSpec ? " late-spec" : "");
    }
    if (dumpICSel && sel.find(dumpICSel) != std::string::npos
            && jm->selBitsArray()) {
        uint64_t* sba = jm->selBitsArray();
        for (uint32_t i = 0; i < jm->numICEntries; i++) {
            uint64_t* slots = reinterpret_cast<uint64_t*>(
                icStart + i * IC_BYTES_PER_SITE);
            std::string siteSel = sba[i] != 0
                ? interp_->memory().oopToString(Oop::fromRawBits(sba[i]))
                : std::string("(null)");
            if (siteSel.empty()) siteSel = "?";
            fprintf(stderr,
                    "[IC-DUMP] %s site=%u sel=%s key0=0x%llx extra0=0x%llx key1=0x%llx\n",
                    sel.c_str(), i, siteSel.c_str(),
                    (unsigned long long)slots[0],
                    (unsigned long long)slots[2],
                    (unsigned long long)slots[3]);
        }
    }
    JITMethod* newJM = compiler_->recompile(compiledMethod);
    if (newJM) {
        rewriteIcEntriesAfterRecompile(
            compiledMethod.rawBits(),
            reinterpret_cast<uint64_t>(newJM->codeStart()));
        if (lateSpec && newJM->stats) {
            // Cap to one re-recompile per method, ever.  Reset count
            // so any further spec-bit additions don't keep re-flagging
            // a method that's already been given its second chance.
            newJM->stats->flags |= kLateSpecRecompiledOnce;
            newJM->stats->lateSpecCount = 0;
        }
    }
    return newJM != nullptr;
}

// Rewrite J2J entry-addr bits in every IC site whose methodBits matches the
// recompiled method.  See header comment for rationale.
void JITRuntime::rewriteIcEntriesAfterRecompile(uint64_t methodBits,
                                                uint64_t newEntryAddr) {
    if (!initialized_) return;
    JITMethod* m = codeZone_.firstMethod();
    if (!m) return;

    constexpr uint64_t kJ2JEntryBit  = 1ULL << 60;
    constexpr uint64_t kJ2JAddrMask  = 0x0000FFFFFFFFFFFFULL;

    // 2026-05-03: IC zone moved to heap; no W^X flip required.  Single
    // pass over methods, write directly to heap-side icBuffer.
    while (m) {
        if (m->numICEntries > 0 && m->icBuffer) {
            uint8_t* icStart = m->icZoneStart();
            for (uint32_t i = 0; i < m->numICEntries; i++) {
                uint64_t* slots = reinterpret_cast<uint64_t*>(
                    icStart + i * IC_BYTES_PER_SITE);
                for (int e = 0; e < 6; e++) {
                    if (slots[e * 3 + 1] == methodBits) {
                        uint64_t extra = slots[e * 3 + 2];
                        if (extra & kJ2JEntryBit) {
                            extra = (extra & ~kJ2JAddrMask)
                                  | (newEntryAddr & kJ2JAddrMask);
                            slots[e * 3 + 2] = extra;
                        }
                    }
                }
            }
        }
        m = m->nextInZone;
    }

    for (size_t k = 0; k < MegaCacheSize; k++) {
        if (megaCache_[k].methodBits == methodBits
            && megaCache_[k].jitEntry != 0) {
            megaCache_[k].jitEntry = newEntryAddr;
        }
    }
}

void JITRuntime::noteMethodEntry(Oop compiledMethod) {
    if (!initialized_ || !compiler_) return;

    // No deferral: JIT compiles methods as soon as they're hot.  Older
    // versions of this function gated noteMethodEntry on a startup-window
    // floor (default 4s headless) plus a Resolver-class-var-set signal.
    // Removed 2026-05-10: user wants JIT always on.  Bugs that surface
    // during the 0..120M-step window (image init in flight, Morphic
    // setup races, etc.) need to be fixed in the JIT, not papered over
    // by a startup gate.

    // Sista already owns this method via a counted-loop splice — don't
    // race with it.  Without this check, ~50% of bench-panel runs T1
    // compiles sumArr, intercepts the activation in tryJITActivation,
    // and falls back to per-iter IC speed (~150× slowdown).  See
    // memory/project_t1_vs_sista_race.md.
    if (sistaRuntimeForGCHook_) {
        Oop m = compiledMethod;
        if (sistaRuntimeForGCHook_->hasSplice(m)) {
            // Cache the splice flag on the JITMethod so stencils can
            // skip per-call counter bumps (avoiding the T1-vs-Sista
            // race) without an unordered_set probe per send.
            JITMethod* jm = methodMap_.lookup(m.rawBits());
            if (jm) jm->isSpliceTarget = true;
            return;
        }
    }

    // Bisection support: JIT_MAX_COMPILE=N stops after N compilations
    static const int maxCompile = g_debug.jitMaxCompile;

    static size_t totalEntries = 0;
    totalEntries++;

    // (Removed 2026-05-09: per-entry [NOTE] log for #benchFib that was
    // an A4 investigation aid.  A4 is resolved at root cause; the
    // selectorOf() call per method entry was needless overhead.)

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

        // Dump top methods by executionCount (opt-in: PHARO_JIT_TOP=1)
        static int dumpTop = g_debug.jitTop ? atoi(g_debug.jitTop) : 0;
        if (dumpTop > 0 && interp_) {
            struct TopEntry { uint32_t count; JITMethod* m; };
            const size_t K = 10;
            TopEntry top[K] = {};
            size_t filled = 0;
            JITMethod* m = codeZone_.firstMethod();
            while (m) {
                if (m->state == MethodState::Compiled) {
                    uint32_t c = m->stats ? m->stats->executionCount : 0;
                    if (filled < K) {
                        top[filled++] = {c, m};
                    } else {
                        size_t minIdx = 0;
                        for (size_t k = 1; k < K; k++) {
                            if (top[k].count < top[minIdx].count) minIdx = k;
                        }
                        if (c > top[minIdx].count) top[minIdx] = {c, m};
                    }
                }
                m = m->nextInZone;
            }
            std::sort(top, top + filled, [](const TopEntry& a, const TopEntry& b) {
                return a.count > b.count;
            });
            fprintf(stderr, "[JIT] Top-%zu by executionCount:\n", filled);
            for (size_t k = 0; k < filled; k++) {
                std::string sel = interp_->memory().selectorOf(
                    Oop::fromRawBits(top[k].m->compiledMethodOop));
                fprintf(stderr, "[JIT]   %8u #%s (oop=0x%llx %ub tier=%u)\n",
                        top[k].count, sel.c_str(),
                        (unsigned long long)top[k].m->compiledMethodOop,
                        top[k].m->codeSize, top[k].m->tier);
            }
        }
    }

    uint64_t key = compiledMethod.rawBits();
    size_t idx = (key >> 3) % CountMapSize;

    // Runtime-configurable threshold: PHARO_JIT_THRESHOLD=N (default: CompileThreshold=2)
    static uint32_t threshold = 0;
    if (threshold == 0) {
        threshold = (g_debug.jitThreshold > 0)
            ? static_cast<uint32_t>(g_debug.jitThreshold) : CompileThreshold;
        if (threshold < 1) threshold = 1;
    }

    // Simple linear probe
    for (size_t probe = 0; probe < 8; probe++) {
        size_t i = (idx + probe) % CountMapSize;
        if (countMap_[i].key == key) {
            countMap_[i].count++;
            if (countMap_[i].count == threshold) {
            compile_check:
                // Bisection: stop after N compilations
                if (maxCompile >= 0 && (int)compiler_->methodsCompiled() >= maxCompile) {
                    return;
                }
                // Selector exclusion: JIT_EXCLUDE=sel1,sel2,...
                {
                    static const char* excludeEnv = g_debug.jitExclude;
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
                // Oop exclusion: JIT_EXCLUDE_OOP=0xhex1,0xhex2,...
                {
                    static const char* excludeOopEnv = g_debug.jitExcludeOop;
                    if (excludeOopEnv) {
                        uint64_t mOop = compiledMethod.rawBits();
                        std::string excl(excludeOopEnv);
                        size_t pos = 0;
                        while (pos < excl.size()) {
                            size_t comma = excl.find(',', pos);
                            if (comma == std::string::npos) comma = excl.size();
                            std::string tok = excl.substr(pos, comma - pos);
                            uint64_t v = strtoull(tok.c_str(), nullptr, 0);
                            if (v == mOop) {
                                fprintf(stderr, "[JIT] EXCLUDED oop=0x%llx\n",
                                        (unsigned long long)mOop);
                                return;
                            }
                            pos = comma + 1;
                        }
                    }
                }
                // Built-in exclusion: exception infrastructure selectors.
                // When these methods are JIT-compiled, exception handling causes
                // recursive native stack growth that overflows at ~400 methods.
                // The cycle: signal → receiver → signalerContext → ... → signal
                //
                // Two tiers:
                //   1. Unambiguous selectors (only used in exception/context handling)
                //   2. Ambiguous selectors (also used by Semaphore, etc.) —
                //      only excluded when the method's class is Exception-related
                {
                    // Always-excluded: original exception infrastructure.
                    // These were already broken before chain-loop work and
                    // stay excluded regardless of chain mode.
                    static const char* alwaysExcluded[] = {
                        "signalerContext",
                        "signalForException:",
                        "raiseUnhandledError",
                        "handleSignal:",
                        "findContextSuchThat:",
                        "cannotReturn:",
                        "aboutToReturn:through:",
                        "noHandler:",
                        nullptr
                    };
                    // Chain-loop-only exclusions: only excluded when
                    // PHARO_RESUME_J2J=1 (chain on).  These methods JIT
                    // fine in chain-off mode (default) and should be
                    // compiled for perf — chain-loop exposes pre-existing
                    // codegen bugs (state corruption, stack overflow) that
                    // need a real fix, not workaround exclusion.
                    static const char* chainOnlyExcluded[] = {
                        // Context-walk for exception handler search
                        "nextHandlerContext",
                        "findNextHandlerContext",
                        // Context-NLR-walk methods
                        "resume:through:",
                        "return:through:",
                        "terminateTo:",
                        // Delay scheduler internals
                        "scheduleAtTimingPriority",
                        "timingPriorityScheduleTicker:",
                        "timingPrioritySignalExpired",
                        "waitForUserSignalled:orExpired:",
                        "tickAfterMilliseconds:",
                        "millisecondsUntilTick:",
                        "nowTick",
                        "primSignal:atUTCMicroseconds:",
                        // SmallInteger>>benchmark + benchFib — sieve and
                        // benchFib hot paths (CRITICAL for bench scores).
                        "benchmark",
                        "benchFib",
                        // SubscriptOutOfBounds signal-family
                        "signalFor:lowerBound:upperBound:",
                        "signalFor:lowerBound:upperBound:in:",
                        "errorSubscriptBounds:",
                        // FFI call
                        "ffiCall:",
                        nullptr
                    };
                    static const char* ambiguousSelectors[] = {
                        "signal",
                        "signal:",
                        "doesNotUnderstand:",
                        "receiver",
                        "defaultAction",
                        "pass",
                        "outer",
                        "resume:",
                        "retry",
                        "return:",
                        nullptr
                    };
                    if (interp_) {
                        std::string sel = interp_->memory().selectorOf(compiledMethod);
                        for (const char** p = alwaysExcluded; *p; p++) {
                            if (sel == *p) {
                                fprintf(stderr, "[JIT] AUTO-EXCLUDED exception infra #%s\n",
                                        sel.c_str());
                                return;
                            }
                        }
                        if (g_debug.resumeJ2J) {
                            for (const char** p = chainOnlyExcluded; *p; p++) {
                                if (sel == *p) {
                                    fprintf(stderr, "[JIT] AUTO-EXCLUDED chain-only #%s\n",
                                            sel.c_str());
                                    return;
                                }
                            }
                        }
                        for (const char** p = ambiguousSelectors; *p; p++) {
                            if (sel == *p) {
                                // Check if method belongs to an Exception-related class
                                Oop methodClass = interp_->methodClassOf(compiledMethod);
                                std::string cls = interp_->memory().classNameOf(methodClass);
                                // Exclude if class name contains "Exception", "Error",
                                // "Notification", or is "Object" (for doesNotUnderstand:)
                                if (cls.find("Exception") != std::string::npos ||
                                    cls.find("Error") != std::string::npos ||
                                    cls.find("Notification") != std::string::npos ||
                                    cls.find("Warning") != std::string::npos ||
                                    (sel == "doesNotUnderstand:" && cls == "Object") ||
                                    (sel == "doesNotUnderstand:" && cls == "ProtoObject")) {
                                    fprintf(stderr, "[JIT] AUTO-EXCLUDED exception infra #%s (class %s)\n",
                                            sel.c_str(), cls.c_str());
                                    return;
                                }
                            }
                        }
                    }
                }
                // Hit threshold — compile!
                //
                // Defer the actual compile to the safe-point drain
                // (Interpreter calls drainInitialCompileQueue at
                // preemption checks, every 64K interp steps, between
                // bytecodes).  Avoids the "compile mid-bytecode while
                // interp local state is in flux" hazard documented in
                // deferred.md E.0 / project_eval_doit_attempt_2026_05_05.md
                // (sender-chain corruption when JIT compile interleaves
                // with eval body dispatch).  The method's NEXT activation
                // sees the JIT entry; this call falls through to interp.
                //
                // Default-on after wider validation 2026-05-05:
                //   - bench-suite 10/10 each mode: queue 3 done vs baseline
                //     2 done (within noise).  Performance identical when
                //     both complete (line-for-line same timings on all 14
                //     benches).  Both fail at floatSum due to pre-existing
                //     image issue, not introduced.
                //   - SUnit 5-class: 640/640 identical (4 skipped).
                //   - SUnit 20-class: 682/682 identical (6 classes complete
                //     in 43s vs 42s — within timing noise).
                // PHARO_NO_QUEUE_COMPILE=1 to opt out (falls back to direct
                // inline compile, the legacy mid-bytecode behavior).
                static const bool queueCompileDisabled =
                    std::getenv("PHARO_NO_QUEUE_COMPILE") != nullptr;
                if (!queueCompileDisabled) {
                    queueInitialCompile(compiledMethod);
                    return;
                }
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
                    std::string t1sel = interp_ ? interp_->memory().selectorOf(compiledMethod) : "?";
                    std::string t1cls = interp_ ? interp_->classNameOfMethod(compiledMethod) : "?";
                    fprintf(stderr, "[JIT] Compiled method #%zu %p %s>>%s (entry %u, %u bytes%s)\n",
                            compiler_->methodsCompiled(),
                            (void*)compiledMethod.rawBits(),
                            t1cls.c_str(), t1sel.c_str(),
                            countMap_[i].count, jm->codeSize,
                            jm->hasPrimPrologue ? ", prim" : "");
                    // Diagnostic: if selector unresolved, dump last-literal class
                    // (usually class binding Association or outer CompiledMethod)
                    if (t1sel == "?" && interp_) {
                        auto& mem = interp_->memory();
                        size_t nLits = mem.numLiteralsOf(compiledMethod);
                        std::string lastCls = "?";
                        std::string penCls = "?";
                        if (nLits >= 1) {
                            Oop last = mem.fetchPointer(nLits, compiledMethod);
                            lastCls = mem.classNameOf(last);
                        }
                        if (nLits >= 2) {
                            Oop pen = mem.fetchPointer(nLits - 1, compiledMethod);
                            penCls = mem.classNameOf(pen);
                        }
                        fprintf(stderr, "[JIT]   nLits=%zu pen.cls=%s last.cls=%s\n",
                                nLits, penCls.c_str(), lastCls.c_str());
                    }

                    // Tier 2 (MIR) compilation is now opt-in.  Enabling it
                    // made arith loops 5-12× slower than T1 alone (measured
                    // sum 3M: T1 237ms, T2 1123ms, interp 96ms).  Stability
                    // fixes from 2b1629f/a311688/fd03572/9ffa5f7/b9ab22e still
                    // apply — T2 runs correctly when enabled — but the
                    // MIR-generated code pays a big per-send overhead that
                    // more than eats the register-allocation win on hot
                    // loops.  Keep PHARO_T2=1 as the opt-in for when that
                    // overhead gets fixed (inline hot sends, cut MIR
                    // preamble/postamble, skip unnecessary state flushes).
                    // PHARO_T2 is strict "=1" here (compile gate — don't
                    // enable accidentally).  Presence elsewhere is fine.
                    static bool noT2 = []() {
                        const char* v = std::getenv("PHARO_T2");
                        return !(v && v[0] == '1');
                    }();
                    static int t2Limit = g_debug.t2Limit;
                    // Warmup delay (todo.md §1.2f real fix option b):
                    // defer T2 compilation until T1 has run this method
                    // PHARO_T2_WARMUP times before T2 intercepts.  This
                    // lets T1's inline IC populate before T2 replaces
                    // the method — without warmup, T2=1 drops IC hit
                    // rate from ~89% to ~82% on array-fill bench.
                    //
                    // Default changed from 0 to 3 (2026-04-18) after
                    // measurement: T2=1 WARMUP=3 preserves the 89.3%
                    // IC hit rate (vs 78-86% with WARMUP=0).  T2 still
                    // compiles 27 methods on array-fill (vs 100 with
                    // WARMUP=0), but those 73 extra weren't providing
                    // a win anyway.  Users who want the old behavior
                    // set PHARO_T2_WARMUP=0 explicitly.
                    static int t2Warmup = g_debug.t2Warmup;
                    // §1.3c: under coexist mode (default), T2 code
                    // never replaces T1 on activation.  Skip the
                    // T2 compile entirely for methods T1 already
                    // handles — saves asmjit memory + compile time.
                    // Set PHARO_T2_REPLACE=1 to reactivate T2
                    // compilation for these methods (and let T2
                    // actually run).
                    static bool t2ReplaceNote = g_debug.t2Replace;
                    bool skipCoexist = !t2ReplaceNote && jm &&
                                       jm->isExecutable();
                    if (!noT2 && tier2Compiler_ && !tier2Lookup(key) &&
                        !skipCoexist &&
                        (int)tier2Compiler_->methodsCompiled() < t2Limit) {
                        if (t2Warmup > 0 && jm && jm->stats &&
                            (int)jm->stats->executionCount < t2Warmup) {
                            // Not warm yet — do NOT insert sentinel;
                            // we'll retry on the next activation.
                        } else {
                            void* t2code = tier2Compiler_->compile(compiledMethod, jm);
                            tier2Insert(key, t2code ? t2code : (void*)1);
                            if (t2code) {
                                std::string t2sel = interp_->memory().selectorOf(compiledMethod);
                                fprintf(stderr, "[JIT] Tier 2 compiled method %p '%s' (%zu total)\n",
                                        (void*)compiledMethod.rawBits(),
                                        t2sel.c_str(),
                                        tier2Compiler_->methodsCompiled());
                            }
                        }
                    }
                }
            }
            return;
        }
        if (countMap_[i].key == 0) {
            countMap_[i].key = key;
            countMap_[i].count = 1;
            if (threshold <= 1) {
                // Threshold=1 means compile on first sighting.
                goto compile_check;
            }
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
    // No makeWritable here — all JITMethod-field writes that used to
    // require a W window were moved to the heap-side JITMethodStats
    // struct (W^X audit 2026-04-26).  Remaining JITMethod field reads
    // are safe in either W or X mode.

    // Verify method map integrity (cheap, catches GC/rehash bugs)
    if (jm->compiledMethodOop != compiledMethod.rawBits()) {
        fprintf(stderr, "[JIT] BUG: methodMap returned wrong JITMethod! "
                "requested=0x%llx got=0x%llx\n",
                (unsigned long long)compiledMethod.rawBits(),
                (unsigned long long)jm->compiledMethodOop);
        return false;
    }

    // Touch for LRU tracking + bump executionCount.  Both writes go to
    // the heap-allocated JITMethodStats side-table (W^X audit
    // 2026-04-26), NOT to MAP_JIT — no flips needed.
    if (jm->stats) {
        codeZone_.touch(jm);
        jm->stats->executionCount++;
    }

    // Promote hot methods: recompile Tier 1 with IC profiling data.
    // Threshold: 500 executions, tier 1 only, must have send sites.
    // (Tier 2 is now leaf-only, so skip it for methods with sends.)
    //
    // Skip Sista-spliced methods — same race noteMethodEntry +
    // maybeRecompileForOSR already gate against.  Without this, a
    // splice method that gets activated 500 times via interp (e.g.
    // post-deopt) would recompile; entering the recompiled T1 code
    // via OSR forces a slower per-iter dispatch than Sista's splice.
    // See memory/project_t1_vs_sista_race.md.
    bool isSplice = sistaRuntimeForGCHook_
                 && sistaRuntimeForGCHook_->hasSplice(compiledMethod);
    // Cache splice flag on JITMethod for stencil-side bump skip.
    if (isSplice) jm->isSpliceTarget = true;
    // Use >= rather than == so methods whose executionCount jumps past
    // the threshold in one go still trigger recompile.  The `tier == 1`
    // check ensures recompile only fires once per method (recompile
    // sets tier=2).  Future PR may bump executionCount from J2J paths;
    // == would miss those.
    if (jm->stats && jm->stats->executionCount >= (uint32_t)g_debug.recompileAt &&
        jm->tier == 1 && jm->numICEntries > 0 && !isSplice) {
        if (compiler_) {
            static const bool traceRecompile =
                std::getenv("PHARO_JIT_TRACE_RECOMPILE") != nullptr;
            if (traceRecompile) {
                std::string sel = interp_->memory().selectorOf(compiledMethod);
                fprintf(stderr,
                        "[RECOMPILE] %s (icEntries=%u execCount=%u)\n",
                        sel.c_str(), jm->numICEntries,
                        jm->stats->executionCount);
            }
            JITMethod* newJM = compiler_->recompile(compiledMethod);
            if (newJM) {
                // Patch every IC J2J entry-addr pointing at the OLD code
                // to the new (specialized) code.  Without this, callers'
                // stencil_sendJ2J / sendInlineMonoJ2J / sendBlockValue*
                // tail-calls keep entering OLD entry — wasted recompile.
                rewriteIcEntriesAfterRecompile(
                    compiledMethod.rawBits(),
                    reinterpret_cast<uint64_t>(newJM->codeStart()));
                jm = newJM;
            }
        }
    }

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

    // Check for Tier 2 compiled code (asmjit-based).  T2 execution
    // opt-in via PHARO_T2=1.  Default off because T2 is currently
    // equal-or-slower than T1 (multi-bc inline IC doesn't beat T1's
    // 2092-byte stencil_sendJ2J on the hot path).
    //
    // §1.3c: COEXIST MODE (default when T2 enabled).  Even with
    // PHARO_T2=1, let T1 stay in charge for methods both tiers
    // compiled.  T2 only wins when T1 isn't available — which in
    // practice means T1 failed to compile (excluded class,
    // primitive-only, etc.) and someone still wants JIT for those
    // methods.  Opt out via PHARO_T2_REPLACE=1 to restore the old
    // "T2 replaces T1" behavior for bisection.
    // PHARO_T2 is strict "=1" (execute gate).
    static bool noT2Exec = []() {
        const char* v = std::getenv("PHARO_T2");
        return !(v && v[0] == '1');
    }();
    static bool t2Replace = g_debug.t2Replace;
    void* t2code = noT2Exec ? nullptr : tier2Lookup(compiledMethod.rawBits());
    bool t1Executable = jm && jm->isExecutable();
    bool useT2 = (t2code && t2code != (void*)1) &&
                 (!t1Executable || t2Replace);
    if (useT2) {
        // Tier 2 code has resume support: on initial entry, state.ip == bcStart
        // (offset 0 → start from beginning). On resume after send, state.ip
        // == bcStart + N → dispatch to post-send label.
        // Set jitMethod so chargeJITBytecodes works for T2 entries.
        state.jitMethod = jm ? jm : methodMap_.lookup(compiledMethod.rawBits());
        // No flips — codebase invariant (W^X audit 2026-04-26): thread
        // is in EXECUTABLE mode by default.
        ((void(*)(JITState*))t2code)(&state);
        return true;
    }

    // No flips — codebase invariant: thread is in X mode.  W^X audit
    // 2026-04-26.  Stats writes above went to the heap side-table.
    JIT_CALL(jm->codeStart(), &state);

    return true;
}

bool JITRuntime::tryResume(Oop compiledMethod, uint32_t bcOffset, JITState& state) {
    if (!initialized_) return false;
    static bool noResume = g_debug.noResume;
    if (noResume) return false;

    // T2 resume DISABLED: MIR's register allocator spills values to the C
    // stack during normal execution. When resume dispatch jumps to a label
    // inside the function, those spill slots contain stale data from previous
    // calls (e.g., megaCache pointers from jit_t2_send), causing stack
    // corruption (session 20: &megaCache_[65535] at FP+6).
    // T2 code still runs via tryExecute (entry at offset 0) where the
    // function prologue properly initializes all spill slots.
    // Fall through to T1 stencil resume below.

    JITMethod* jm = methodMap_.lookup(compiledMethod.rawBits());
    if (!jm || !jm->isExecutable()) return false;

    // Safety: refuse to resume at a bytecode offset whose stencil reads
    // operands from registers (x19-x22, "_N" variants). The stencil chain
    // before this offset would have populated those registers, but we're
    // entering cold — registers contain whatever the C caller left there.
    // Non-boolean spill-and-deopt paths in the _1 jump stencils would
    // push garbage and EXIT_SEND without updating s->ip, creating a stack
    // leak. Deferred-issues.md #4 (Session 15).
    if (getBcEntryState(jm, bcOffset) != 0) return false;

    // No makeWritable — touch's lastUsedEpoch write is now to the heap
    // side-table (W^X audit 2026-04-26).  No JITMethod field writes
    // happen below.

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
    // Reset per-entry J2J counters. Cursor/limit are NOT cleared here —
    // the caller controls whether J2J is enabled by setting them to a valid
    // pool slice (enable) or nullptr (disable, safe default since
    // nullptr >= nullptr → true → stencils bail to EXIT_SEND_CACHED).
    state.j2jDepth = 0;
    state.j2jTotalCalls = 0;
    // Note: yieldCountdown is NOT reset here — it must persist across chain
    // loop iterations so backward jumps eventually reach 0 and yield.
    // It's initialized by tryJITActivation and reset by ExitYield handlers.

    // Validate IC buffer is allocated when expected (heap-side, not in
    // code zone — see JITMethod::icBuffer 2026-05-03 comment).
    if (jm->numICEntries > 0 && !jm->icBuffer) {
        fprintf(stderr,
                "[JIT] BUG: numICEntries=%u but icBuffer is null\n",
                jm->numICEntries);
        return false;
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

    // No flips — codebase invariant (W^X audit 2026-04-26): thread is
    // in EXECUTABLE mode by default.

    // Enter at the specified code offset
    StencilFunc entry = reinterpret_cast<StencilFunc>(jm->codeStart() + codeOffset);

    // Final sp validation just before entry
    if (reinterpret_cast<uint64_t>(state.sp) < 0x10000) {
        fprintf(stderr, "[JIT] BUG: sp=%p just before stencil entry (bc=%u code=%u)\n",
                (void*)state.sp, bcOffset, codeOffset);
        return false;
    }

    // Final entry-address sanity: must be within OUR code zone, not in
    // an evicted/freed area or asmjit's allocator zone.  Bug 11b layer 4:
    // a stale `jm` from a previous activation slipped through methodMap
    // checks and `entry` lands at a JITMethod-shaped slot whose code
    // body was never properly populated (zero bytes → ARM64 UDF).
    uint8_t* entryByte = reinterpret_cast<uint8_t*>(entry);
    if (!codeZone_.contains(entryByte)) {
        fprintf(stderr, "[JIT] BUG: tryResume entry %p outside code zone (bc=%u code=%u jm=%p)\n",
                (void*)entry, bcOffset, codeOffset, (void*)jm);
        return false;
    }
    // Defensive findMethodByPC scan was 93% of tryResume CPU on Linux ARM
    // (Pi 5 perf profile, bench3 block(500K)).  The check is redundant:
    // - methodMap.lookup(compiledMethodOop) above already dereferenced jm
    //   and verified jm->compiledMethodOop matches the lookup key.
    // - entry = jm->codeStart() + codeOffset is mathematically inside jm
    //   by construction.
    // The scan is paranoia from "Bug 11b layer 4" — keep it available
    // under PHARO_JIT_VALIDATE_ENTRY=1 for diagnosis when that bug
    // shape recurs, but skip on the hot path.  Removing it took
    // block(500K) on Pi 5 from 1980ms → expected 100-200ms.
    static const bool validateEntry =
        std::getenv("PHARO_JIT_VALIDATE_ENTRY") != nullptr;
    if (validateEntry) {
        JITMethod* entryMethod = codeZone_.findMethodByPC(reinterpret_cast<uint64_t>(entry));
        if (entryMethod != jm) {
            fprintf(stderr, "[JIT] BUG: tryResume entry %p in wrong method (expected jm=%p, got %p) bc=%u code=%u\n",
                    (void*)entry, (void*)jm, (void*)entryMethod, bcOffset, codeOffset);
            return false;
        }
    }

    // PHARO_BC5_DUMP=1: dump tryResume state at every resume site of
    // a *>>scanFor:.  Used to find which resume corrupts state.
    {
        static bool bc5dump = std::getenv("PHARO_BC5_DUMP") != nullptr;
        static int bc5count = 0;
        if (bc5dump && interp_ && bc5count < 80) {
            std::string sel = interp_->memory().selectorOf(compiledMethod);
            if (sel == "scanFor:") {
                bc5count++;
                Oop methodClass = interp_->methodClassOf(compiledMethod);
                std::string cls = interp_->memory().classNameOf(methodClass);
                Oop tos = state.sp > state.tempBase
                    ? state.sp[-1] : Oop::fromRawBits(0);
                Oop t3 = state.tempBase[3];
                Oop t5 = state.tempBase[5];
                auto desc = [&](Oop o) -> std::string {
                    if (o.isSmallInteger()) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "SmI(%lld)",
                            (long long)o.asSmallInteger());
                        return buf;
                    }
                    if (o.isNil()) return "nil";
                    if (o.isObject() && o.rawBits() > 0x10000) {
                        return interp_->memory().classNameOf(
                            interp_->memory().classOf(o));
                    }
                    char buf[32];
                    snprintf(buf, sizeof(buf), "raw=0x%llx",
                        (unsigned long long)o.rawBits());
                    return buf;
                };
                Oop t2 = state.tempBase[2];
                Oop t4 = state.tempBase[4];
                fprintf(stderr,
                    "[BC5-DUMP #%d] %s>>scanFor: bc=%u sp=%p tempBase=%p\n"
                    "  TOS = 0x%llx %s\n"
                    "  temp2 (=start) = 0x%llx %s\n"
                    "  temp3 (=finish) = 0x%llx %s\n"
                    "  temp4 (=loop limit) = 0x%llx %s\n"
                    "  temp5 (=loop index) = 0x%llx %s\n",
                    bc5count, cls.c_str(), bcOffset,
                    (void*)state.sp, (void*)state.tempBase,
                    (unsigned long long)tos.rawBits(), desc(tos).c_str(),
                    (unsigned long long)t2.rawBits(), desc(t2).c_str(),
                    (unsigned long long)t3.rawBits(), desc(t3).c_str(),
                    (unsigned long long)t4.rawBits(), desc(t4).c_str(),
                    (unsigned long long)t5.rawBits(), desc(t5).c_str());
                // Dump receiver's instVar 1 (= array for HashedCollection)
                // and its actual slot count vs what temp3 thinks.
                if (state.receiver.isObject() && state.receiver.rawBits() > 0x10000) {
                    ObjectHeader* rh = state.receiver.asObjectPtr();
                    if (rh->slotCount() >= 2) {
                        Oop arr = rh->slots()[1];
                        fprintf(stderr,
                            "  receiver.iv[1] (= array) = 0x%llx %s\n",
                            (unsigned long long)arr.rawBits(), desc(arr).c_str());
                        if (arr.isObject() && arr.rawBits() > 0x10000) {
                            ObjectHeader* ah = arr.asObjectPtr();
                            fprintf(stderr,
                                "  array.slotCount = %zu (vs temp3=%s)\n",
                                ah->slotCount(), desc(t3).c_str());
                        }
                    }
                }
                // sp-relative dump (a few stack slots below TOS)
                for (int k = 0; k < 5 && state.sp - k - 1 >= state.tempBase; k++) {
                    Oop v = state.sp[-(k + 1)];
                    fprintf(stderr,
                        "  sp[-%d] = 0x%llx %s\n", k + 1,
                        (unsigned long long)v.rawBits(), desc(v).c_str());
                }
            }
        }
    }

    // 2026-05-07 A1: optional sp-delta trace across each tryResume.
    // PHARO_TRACE_RESUME_SP=1 enables; gated to keep zero overhead off.
    static const bool traceResume =
        std::getenv("PHARO_TRACE_RESUME_SP") != nullptr;
    Oop* spBefore = state.sp;

    entry(&state);

    if (__builtin_expect(traceResume, 0)) {
        ptrdiff_t delta = state.sp - spBefore;
        std::string sel = interp_ ? interp_->memory().selectorOf(compiledMethod) : "?";
        static size_t cnt = 0;
        cnt++;
        if (cnt <= 30 || (delta != 0 && cnt < 200)) {
            fprintf(stderr,
                "[RESUME-SP] #%zu method=#%s bcOff=%u "
                "delta=%+lld exit=%u\n",
                cnt, sel.c_str(), bcOffset,
                (long long)delta, (unsigned)state.exitReason);
        }
    }
    // Stay in X — codebase invariant.  W^X audit 2026-04-26.
    return true;
}

bool JITRuntime::tryResumeFast(JITMethod* jm, uint32_t bcOffset, JITState& state) {
    // Fast resume: caller guarantees jm is the same JITMethod we just exited
    // from, so we skip method map lookup, IC validation, receiver bounds
    // check, and LRU touch.  No JITMethod-field writes happen here, and
    // the codebase invariant (W^X audit 2026-04-26) is that the thread
    // is in EXECUTABLE mode by default — no flips needed.
    if (!jm->isExecutable()) return false;

    uint32_t codeOffset = jm->codeOffsetForBC(bcOffset);
    if (codeOffset == 0 || codeOffset >= jm->codeSize) return false;

    // Set up JIT state
    // Safety: refuse register-reading (_N) entry offsets — see tryResume.
    if (getBcEntryState(jm, bcOffset) != 0) return false;

    state.jitMethod = jm;
    state.exitReason = ExitNone;

    StencilFunc entry = reinterpret_cast<StencilFunc>(jm->codeStart() + codeOffset);
    entry(&state);
    return true;
}

// Walk every compiled method's IC sites, classify each by polymorphism,
// and print a histogram.  Uses the same IC layout constants as the
// stencil (IC_ENTRIES_PER_SITE, IC_BYTES_PER_ENTRY).
//
// Sista inlining Phase 1 diagnostic: if the bulk of sites are
// monomorphic or 2-way, speculative inlining is worth implementing.
// If most hot sites are megamorphic, inlining won't help and the
// whole project premise is wrong.
void JITRuntime::dumpICHistogram() const {
    if (!initialized_) {
        fprintf(stderr, "[IC-HIST] runtime not initialized\n");
        return;
    }

    size_t totalSites   = 0;
    size_t emptySites   = 0;
    size_t monoSites    = 0;
    size_t poly2Sites   = 0;
    size_t poly3Sites   = 0;
    size_t poly4plus    = 0;
    size_t compiledMethods = 0;

    JITMethod* m = codeZone_.firstMethod();
    while (m) {
        if (m->state == MethodState::Compiled && m->numICEntries > 0
                && m->icBuffer) {
            compiledMethods++;
            uint8_t* icStart = m->icZoneStart();
            for (uint32_t s = 0; s < m->numICEntries; s++) {
                totalSites++;
                uint64_t* slots = reinterpret_cast<uint64_t*>(
                    icStart + s * IC_BYTES_PER_SITE);
                // Count non-empty entries.  Entry layout:
                //   slots[3*i + 0] = classKey
                //   slots[3*i + 1] = method
                //   slots[3*i + 2] = extra
                // Key == 0 → empty entry.
                uint32_t populated = 0;
                for (uint32_t i = 0; i < IC_ENTRIES_PER_SITE; i++) {
                    if (slots[i * 3] != 0) populated++;
                }
                switch (populated) {
                    case 0: emptySites++; break;
                    case 1: monoSites++;  break;
                    case 2: poly2Sites++; break;
                    case 3: poly3Sites++; break;
                    default: poly4plus++; break;
                }
            }
        }
        m = m->nextInZone;
    }

    fprintf(stderr, "[IC-HIST] compiledMethods=%zu totalSites=%zu\n",
            compiledMethods, totalSites);
    if (totalSites == 0) {
        fprintf(stderr, "[IC-HIST]   (no sites)\n");
        return;
    }
    auto pct = [&](size_t n) { return 100.0 * n / (double)totalSites; };
    fprintf(stderr, "[IC-HIST]   empty:        %zu (%.1f%%)\n", emptySites, pct(emptySites));
    fprintf(stderr, "[IC-HIST]   monomorphic:  %zu (%.1f%%)\n", monoSites,  pct(monoSites));
    fprintf(stderr, "[IC-HIST]   2-way poly:   %zu (%.1f%%)\n", poly2Sites, pct(poly2Sites));
    fprintf(stderr, "[IC-HIST]   3-way poly:   %zu (%.1f%%)\n", poly3Sites, pct(poly3Sites));
    fprintf(stderr, "[IC-HIST]   4+-way poly:  %zu (%.1f%%)\n", poly4plus,  pct(poly4plus));
    // Key ratio for Sista: sites with exactly 1 or 2 observed classes
    // are candidates for speculative inlining.
    size_t inlinable = monoSites + poly2Sites;
    fprintf(stderr, "[IC-HIST] inlinable (mono or 2-way) / non-empty: %.1f%%\n",
            100.0 * inlinable / (double)(totalSites - emptySites));
}

void JITRuntime::flushCaches() {
    if (!initialized_) return;

    // Clear mega cache
    std::memset(megaCache_, 0, sizeof(megaCache_));

    // Clear IC entries but preserve selectorBits (slot 18 of each 19-slot IC).
    // selectorBits is written once at compile time and never re-patched,
    // so zeroing it would permanently disable megamorphic cache probes.
    // Layout per IC site defined by IC_* constants in JITMethod.hpp.
    //
    // 2026-05-03: IC zone moved to heap; no W^X flip required.
    JITMethod* m = codeZone_.firstMethod();
    while (m) {
        if (m->numICEntries > 0 && m->icBuffer) {
            uint8_t* icStart = m->icZoneStart();
            for (uint32_t i = 0; i < m->numICEntries; i++) {
                uint64_t* slots = reinterpret_cast<uint64_t*>(
                    icStart + i * IC_BYTES_PER_SITE);
                // Zero the entries but keep selectorBits at IC_SELBITS_SLOT
                std::memset(slots, 0, IC_SELBITS_SLOT * sizeof(uint64_t));
            }
        }
        m = m->nextInZone;
    }
}

JITRuntime::T2ICSlot* JITRuntime::allocT2ICSlots(int count) {
    if (t2ICNextSlot_ + count > MaxT2ICSlots) return nullptr;
    T2ICSlot* result = &t2ICPool_[t2ICNextSlot_];
    t2ICNextSlot_ += count;
    std::memset(result, 0, count * sizeof(T2ICSlot));
    return result;
}

void JITRuntime::flushT2ICs() {
    // Zero all allocated IC slots (classIndex=0 forces miss on next access)
    if (t2ICNextSlot_ > 0) {
        std::memset(t2ICPool_, 0, t2ICNextSlot_ * sizeof(T2ICSlot));
    }
}

void JITRuntime::recoverAfterGC(ObjectMemory& memory) {
    if (!initialized_) return;

    // Flush all IC entries: GC compaction moves method and selector oops.
    // forEachRoot visits IC slots, but there are edge cases (recompiled
    // methods, timing between patch and GC) where oops go stale.
    // Flushing is cheap — ICs re-fill on the next few misses.
    // 2026-05-03: IC zone moved to heap; no W^X flip required.
    {
        JITMethod* m = codeZone_.firstMethod();
        // Diagnostic: PHARO_JIT_STALE_LOG=1 logs each stale slot-18 oop
        // at GC-recovery time.  Helps identify which IC writes aren't
        // visited by forEachRoot (Task #41 staleness-source hunt).
        static bool staleLog = []() {
            const char* v = std::getenv("PHARO_JIT_STALE_LOG");
            return v && v[0] == '1';
        }();
        size_t totalSites = 0, staleSlot18 = 0, staleEntries = 0;
        while (m) {
            if (m->numICEntries > 0 && m->icBuffer) {
                uint8_t* icStart = m->icZoneStart();
                if (staleLog && interp_) {
                    for (uint32_t i = 0; i < m->numICEntries; i++) {
                        uint64_t* slots = reinterpret_cast<uint64_t*>(
                            icStart + i * IC_BYTES_PER_SITE);
                        totalSites++;
                        uint64_t selBits = slots[IC_SELBITS_SLOT];
                        if (selBits != 0) {
                            Oop sel = Oop::fromRawBits(selBits);
                            if (!interp_->memory().isValidPointer(sel)) {
                                staleSlot18++;
                                fprintf(stderr,
                                    "[STALE-SELBITS] m=%p oop=0x%llx state=%d "
                                    "site=%u/%u selBits=0x%llx (not-in-heap)\n",
                                    (void*)m,
                                    (unsigned long long)m->compiledMethodOop,
                                    (int)m->state, i, m->numICEntries,
                                    (unsigned long long)selBits);
                            } else {
                                // In-heap check: is it still a Symbol?
                                uint32_t ci = sel.asObjectPtr()->classIndex();
                                // Symbol classIndex varies but should be stable
                                // across runs; we just check for wildly
                                // different (0 or huge) as a sanity.
                                if (ci == 0 || ci > 100000) {
                                    staleSlot18++;
                                    fprintf(stderr,
                                        "[STALE-SELBITS-CI] m=%p oop=0x%llx "
                                        "state=%d site=%u/%u selBits=0x%llx "
                                        "classIndex=%u\n",
                                        (void*)m,
                                        (unsigned long long)m->compiledMethodOop,
                                        (int)m->state, i, m->numICEntries,
                                        (unsigned long long)selBits, ci);
                                }
                            }
                        }
                        uint64_t methBits0 = slots[1];
                        if (methBits0 != 0) {
                            Oop meth = Oop::fromRawBits(methBits0);
                            if (!interp_->memory().isValidPointer(meth)) {
                                staleEntries++;
                            }
                        }
                    }
                }
                // DIAGNOSTIC MODE PHARO_JIT_KEEP_ICS=1: preserve slot 18
                // (selectorBits) to study the flaky crash symptom.
                // Don't merge this in; it causes 1/3 crashes.
                static bool keepICs = []() {
                    const char* v = std::getenv("PHARO_JIT_KEEP_ICS");
                    return v && v[0] == '1';
                }();
                if (keepICs) {
                    constexpr size_t entriesBytes = IC_SELBITS_SLOT * sizeof(uint64_t);
                    for (uint32_t i = 0; i < m->numICEntries; i++) {
                        std::memset(icStart + i * IC_BYTES_PER_SITE, 0, entriesBytes);
                    }
                } else {
                    std::memset(icStart, 0, m->numICEntries * IC_BYTES_PER_SITE);
                }
            }
            m = m->nextInZone;
        }
        if (staleLog && totalSites > 0) {
            fprintf(stderr,
                "[STALE-SUMMARY] sites=%zu staleSlot18=%zu staleEntries=%zu\n",
                totalSites, staleSlot18, staleEntries);
        }
    }
    // Clear mega cache: keyed by selectorBits which changed.
    std::memset(megaCache_, 0, sizeof(megaCache_));

    // Flush T2 monomorphic IC slots (resolvedBits are stale after compaction)
    flushT2ICs();

    // Flush asmjit-T2 IC buffers too — each T2-compiled send has its own
    // 152-byte IC storing methodBits Oops that can become stale post-GC.
    if (tier2Compiler_) tier2Compiler_->flushAllICs();

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

    // Count map: keys were updated in-place by forEachRoot during compaction,
    // but hash positions are stale (same issue as methodMap and megaCache).
    // Rehash to fix positions. Methods that are already compiled (in methodMap)
    // don't need counting anymore — drop them to reduce future probe overhead.
    {
        // Heap-allocate temp because CountMapSize*sizeof(CountEntry) = 196KB
        auto* temp = new CountEntry[CountMapSize];
        std::memcpy(temp, countMap_, sizeof(countMap_));
        std::memset(countMap_, 0, sizeof(countMap_));
        for (size_t i = 0; i < CountMapSize; i++) {
            if (temp[i].key == 0) continue;
            // Skip entries for methods already compiled — no need to count them
            if (methodMap_.lookup(temp[i].key)) continue;
            size_t idx = (temp[i].key >> 3) % CountMapSize;
            for (size_t probe = 0; probe < 8; probe++) {
                size_t slot = (idx + probe) % CountMapSize;
                if (countMap_[slot].key == 0) {
                    countMap_[slot] = temp[i];
                    break;
                }
            }
        }
        delete[] temp;
    }

    // Tier 2 map: keys were updated in-place by forEachRoot, but hash positions
    // are stale. Rehash to fix.
    {
        Tier2Entry temp[Tier2MapSize];
        std::memcpy(temp, tier2Map_, sizeof(tier2Map_));
        std::memset(tier2Map_, 0, sizeof(tier2Map_));
        size_t mask = Tier2MapSize - 1;
        for (size_t i = 0; i < Tier2MapSize; i++) {
            if (temp[i].key == 0) continue;
            tier2Insert(temp[i].key, temp[i].func);
        }
    }
    // 2026-05-03: previous flushCaches/recoverAfterGC IC clear paths
    // both needed code-zone makeWritable/makeExecutable; with IC moved
    // to heap that round-trip is gone.
}

} // namespace jit
} // namespace pharo

#endif // PHARO_JIT_ENABLED
