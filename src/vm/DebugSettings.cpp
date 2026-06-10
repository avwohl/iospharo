/*
 * DebugSettings.cpp - Constructor reads all env vars once at static-init.
 *
 * C++ guarantees namespace-scope objects with static storage duration
 * are constructed before main() runs, and the process environment is
 * fully populated before any constructor fires, so reading getenv() in
 * the ctor is safe.  No other global constructor references g_debug,
 * so initialization order across translation units doesn't matter.
 */
#include "DebugSettings.hpp"

#include <cstdlib>
#include <cstring>

namespace pharo {

namespace {

// ============================ FROZEN ============================
// DO NOT ADD envPresent()/envInt()/envStr() CALL SITES.  This file is
// the LEGACY knob surface; the CMake configure step counts envPresent
// call sites and FAILS the build if the count grows (see the
// "envPresent RATCHET" block in CMakeLists.txt).  New knobs — including
// default-ON opt-outs — go in src/vm/debug_vars.h (DEBUG_BOOL/INT/STR)
// and are read with GET_DEBUG_*() at the use site.  envPresent here was
// the slow workaround for the project's getenv ban; frozen 2026-06-10.
// ================================================================
//
// Present and non-empty (any value including "0" counts as set).
// Most call sites historically used `getenv("X") != nullptr`, which
// also treats empty string as set.  Preserve that by only requiring
// non-null.
inline bool envPresent(const char* name) {
    return std::getenv(name) != nullptr;
}

// Non-null and non-empty — matches `v && *v` pattern.
inline bool envTruthy(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0';
}

// Returns raw pointer or nullptr if unset/empty.  Pointer lifetime
// matches the process environment — safe to cache.
inline const char* envStr(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? v : nullptr;
}

// Returns raw pointer or nullptr — matches `const char* v = getenv("X")`
// pattern that lets callers distinguish set-empty from unset.
inline const char* envStrRaw(const char* name) {
    return std::getenv(name);
}

inline int envInt(const char* name, int defaultVal) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::atoi(v) : defaultVal;
}

// Strict equality with "1" — matches `v && v[0] == '1'` pattern used at
// a handful of call sites (PHARO_T2 compile/exec gates, PHARO_JIT_*_LOG
// diagnostics).  Treats unset / empty / non-"1*" values as false.
inline bool envEq1(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] == '1';
}

}  // namespace

void DebugSettings::reload() {
    *this = DebugSettings();
}

DebugSettings::DebugSettings() {
    // --- General debug/trace ---
    delayDebug       = envTruthy("PHARO_DELAY_DEBUG");
    gcEphDebug       = envTruthy("PHARO_GC_EPH_DEBUG");
    reflectProfile   = envPresent("PHARO_REFLECT_PROFILE");
    timerDebug       = envTruthy("PHARO_TIMER_DEBUG");
    callbackDebug    = envPresent("PHARO_CALLBACK_DEBUG");
    bench            = envPresent("PHARO_BENCH");

    // --- JIT on/off switches ---
    noJit            = envPresent("PHARO_NO_JIT") || envPresent("PHARO_NOJIT");
    {
        // PHARO_NO_JIT=0 historically does NOT disable JIT — it's treated as
        // "explicitly enable".  Only non-"0" values count as a real disable.
        const char* v = std::getenv("PHARO_NO_JIT");
        const char* v2 = std::getenv("PHARO_NOJIT");
        bool disable = false;
        if (v && v[0] != '0') disable = true;
        if (v2 && v2[0] != '0') disable = true;
        noJitStrict = disable;
    }
    noOSR            = envPresent("PHARO_NO_OSR");
    noJ2J            = envPresent("PHARO_NO_J2J");
    noICFill         = envPresent("PHARO_NO_IC_FILL");
    noChain          = envPresent("PHARO_NO_CHAIN");
    noEagerCompile   = envPresent("PHARO_NO_EAGER_COMPILE");
    noBlocks         = envPresent("PHARO_JIT_NO_BLOCKS");
    noResume         = envPresent("PHARO_NO_RESUME");
    resumeJ2J        = envPresent("PHARO_RESUME_J2J");

    // --- asmjit-T1 path (default since phase4b.37, 37/37 fuzzer PASS) ---
    {
        const bool noT1 = envPresent("PHARO_NO_ASMJIT_T1");
        const bool explicitOn = envPresent("PHARO_USE_ASMJIT_T1");
        useAsmjitT1 = explicitOn || !noT1;
    }
    useAsmjitT1Trace    = envPresent("PHARO_USE_ASMJIT_T1_TRACE");

    // --- asmjit-T1 simplification / bisect knobs ---
    t1ForceSimple       = envPresent("PHARO_T1_FORCE_SIMPLE");
    t1ForceBailMid      = t1ForceSimple || envPresent("PHARO_T1_FORCE_BAIL_MID");
    t1TraceCompile      = envPresent("PHARO_T1_TRACE_COMPILE");
    // Saveless leaf-call path (Eδ.2d): DEFAULT-ON 2026-06-10, knob lives
    // in debug_vars.h (PHARO_T1_NO_CAN_SKIP_J2J_SAVE, read via
    // GET_DEBUG_BOOL at the emit site).  The t1CanSkipJ2JSave field is
    // gone — see debug_vars.h for the validation summary.
    t1NoBlockResume     = t1ForceSimple || envPresent("PHARO_T1_NO_BLOCK_RESUME");
    t1NoPostPrimResume  = t1ForceSimple || envPresent("PHARO_T1_NO_POST_PRIM_RESUME");
    {
        // Default ON since 2026-05-16 — chain-loop's J2J trampoline
        // conversion (which corrupted the receiver-slot retVal for
        // send-containing callers) is now bypassed when probe is on
        // (see Interpreter.cpp's noJ2JTramp).  39/39 fuzzer PASS,
        // tinyBenchmarks sends +25%, no MUSTBOOL regressions.
        const bool optIn = envPresent("PHARO_T1_IC_PROBE");
        const bool optOut = envPresent("PHARO_T1_NO_IC_PROBE");
        t1ICProbe = optOut ? false : true;
        (void)optIn;  // legacy — was required to opt in; now ignored
    }
    t1ICProbeMin        = envInt("PHARO_T1_IC_PROBE_MIN", -1);
    t1ICProbeMax        = envInt("PHARO_T1_IC_PROBE_MAX", -1);
    // 2026-05-29: default ON (opt-out via PHARO_T1_NO_INLINE_GETTER).  It was
    // turned OFF 2026-05-28 for the AI-Algorithms-Graph AIPrim/AITarjan
    // corruption — but that was NOT a getter bug: the getter always wrote the
    // correct operand slot.  Root cause was the ExitArrayCreate handlers gating
    // their state.ip sync on `tier == 1`, which dropped recompiled (tier 2)
    // asmjit-T1 methods, leaving instructionPointer_ stale so asTuple re-ran
    // from offset 2 and rebuilt {prior-array, ...}.  Fixed 3d787a78 (gate on
    // useAsmjitT1).  Re-validation (docs/results-jitpkg.md) showed all 5 of the
    // det-sched-found "getter regressions" were non-bugs: testJump /
    // testSelfEvaluatingComplexCase are PHARO_DET_SCHED_QUANTUM=1 force-yield
    // artifacts (pass under wall-clock and QUANTUM>=2), testAsArrayKeepsIdentity
    // / testInCriticalWait are flaky (pass on re-run), and
    // testHasBindingThatBeginsWith ERRORs identically with the getter OFF (a
    // pre-existing warmup-masked failure).  So the getter is safe again; this
    // restores the perf optimization.  PHARO_T1_NO_INLINE_GETTER forces off.
    t1InlineGetter      = !envPresent("PHARO_T1_NO_INLINE_GETTER");
    t1InlineSetter      = !envPresent("PHARO_T1_NO_INLINE_SETTER");
    t1InlineReturnsSelf = !envPresent("PHARO_T1_NO_INLINE_RETURNS_SELF");
    // The six dispatch-A-only "extra" inline specs (IC bits 51-58:
    // multiSlot/returnsLiteral/tempReturn/intCmp/intArith/evenOdd) moved to
    // debug_vars.h as DEFAULT-OFF opt-in knobs (PHARO_T1_INLINE_*), read via
    // GET_DEBUG_BOOL at the emit sites — they were unvalidated + buggy
    // (tryMultiSlot's wild #extent receiver). See debug_vars.h / jit-retrospective.md.
    sistaNoShortcutEvenOdd = envPresent("PHARO_SISTA_NO_SHORTCUT_EVEN_ODD");
    // 2026-05-21 (A6 iter N+30k): default-ON.  The pure-J2J gate
    // (PHARO_T1_NO_PURE_J2J_GATE=1 to disable) prevents inline-J2J
    // from firing when any IC site lacks bit 60 (J2J_ENTRY_BIT),
    // which avoids the materialize-bail wrong-result bug that
    // affected benchFib(N>=17).  See AsmjitT1.cpp inline-J2J emit
    // and deferred.md A6 N+30k.  PHARO_T1_NO_INLINE_J2J=1 opts out
    // entirely (legacy workaround).
    //
    // 2026-05-28 PM (final): DEFAULT FLIPPED BACK TO OFF.
    //
    // Bisection found:
    //   INLINE off + BV off (NEW default):     634/634 SUnit PASS (100%),  76 ms fib28
    //   INLINE on, XMETHOD off (old default):  ~297/634 (cull: bug catastrophic)
    //   INLINE on, XMETHOD on, BV off:         633/634 SUnit PASS,         13 ms fib28
    //
    // The 1-test gap with INLINE on is CharacterTest>>testStoreStringAll
    // — deep OCParser/JIT interaction (parseAssignment gets a non-Boolean
    // OCIdentifierToken from a JIT-emitted conditional).  Workarounds
    // tried that don't help: PHARO_T1_PURE_J2J_GATE=1,
    // PHARO_T1_WARM_J2J_GATE=1, PHARO_T1_J2J_RECEIVER_SYNC=1,
    // PHARO_T1_CAN_SKIP_J2J_SAVE=1, PHARO_T1_INLINE_J2J_XMETHOD_MAX=0.
    //
    // Trading 7x fib speedup for 100% test pass rate per the
    // user's "fix the existing fails" directive.  Opt back in via
    // PHARO_T1_INLINE_J2J=1 + PHARO_T1_INLINE_J2J_XMETHOD=1 for
    // bench harnesses that need fib-style perf.
    //
    // 2026-06-10: DEFAULT-ON (lever (e)).  The opt-in era's two blockers
    // are both root-caused and fixed:
    //   - the xmethod gate off-by-one (dd0fa6f2) — read hasNLR as
    //     canBailMidMethod, bouncing ~every real cross-method callee to
    //     the trampoline AND letting stubs through (the state-corruption
    //     lore);
    //   - the stale state.j2jDepth after materializeJ2J (24175466) — the
    //     50%-startup sortBlock corruption at full fire volume.
    // Validation: 60-class SUnit A/B default-vs-INLINE_J2J per-test
    // identical (4130/4140 both; only two known-flaky timing/termination
    // tests flipped, one each way; the historical blocker
    // CharacterTest>>testStoreStringAll PASSES now); 10/10 + 6/6 clean
    // startups; cfib(30) 344ms -> 41ms (Cog 8ms); benchFib(30) 296ms ->
    // 35ms; eval battery matches Cog values.
    // KNOB MOVED to debug_vars.h (PHARO_T1_NO_INLINE_J2J, read via
    // GET_DEBUG_BOOL at the emit sites) per the no-new-envPresent rule;
    // the t1InlineJ2J field is gone.
    // 2026-05-21 (jit-may20 Step 2): pure-J2J gate replaced by warmth
    // gate as the default.  Pure gate (bit-60 check) was too strict —
    // bailed ~96.5% of inline-J2J attempts on fib.  Warmth gate
    // (entry0.key == 0 check) catches the same wrong-result class of
    // bug with a 99%+ catch rate.  Pure gate kept as opt-in via
    // PHARO_T1_PURE_J2J_GATE=1 for bisection; warmth gate disabled via
    // PHARO_T1_NO_WARM_J2J_GATE=1.
    // 2026-05-24: flip pure gate to default-on.  Pure gate checks IC
    // entry's bit 60 (J2J_ENTRY_BIT) which IS preserved across GC
    // (see JITRuntime.cpp:3580 FLAGS_MASK selective clear), unlike
    // warm gate which checks the key (zeroed by GC).  After GC, warm
    // gate bails on EVERY inline-J2J attempt until ICs re-fill,
    // making fib pay full activation cost for the rest of the run.
    // Pure gate stays warm across GC, so it bails LESS often while
    // still guarding the materialize-bail wrong-result bug.
    // PHARO_T1_NO_PURE_J2J_GATE=1 disables.
    //
    // 2026-05-25: pure gate FLIPPED TO DEFAULT-OFF.  The new
    // no-nested-jit-bail check in canJITActivate (DebugSettings.hpp
    // t1AllowNestedJitBail) is a tighter fix for the materialize-bail
    // wrong-result bug — only suppresses JIT entry during active
    // materialize-bail unwind instead of bailing inline-J2J at every
    // push.  Bench impact: fib(28) 125ms → 14ms (8.9x).  Gate kept
    // as PHARO_T1_PURE_J2J_GATE=1 opt-in for bisection.
    t1PureJ2JGate       = envPresent("PHARO_T1_PURE_J2J_GATE");
    // 2026-05-25 Pt 2: warm gate flipped BACK to default-on.  The
    // canJITActivate no-nested-bail check fixes fib's wrong-result
    // scenario (commit 719239f9) but image-startup workloads
    // (snapshot operations, DNU cascade for OSSDL2 #resolve:) still
    // crash without the gate.  Without gates, JIT runs inline-J2J
    // aggressively and hits a different state-corruption (interp
    // +1100 SmI-deref, same signature as the fib bug but a different
    // root cause — needs lldb-driven investigation).  Warm gate alone
    // is sufficient as the safety net (bench-suite runs cleanly).
    // PHARO_T1_NO_WARM_J2J_GATE=1 disables for fib-specific bench.
    t1WarmJ2JGate       = envPresent("PHARO_T1_WARM_J2J_GATE");
    t1BailGateTrace     = envPresent("PHARO_T1_BAIL_GATE_TRACE");
    t1BailGateHisto     = t1BailGateTrace
                         || envPresent("PHARO_T1_BAIL_GATE_HISTO");
    t1InlinePrimBitOps  = !envPresent("PHARO_T1_NO_INLINE_PRIM_BITOPS");
    t1InlinePrimAt      = !envPresent("PHARO_T1_NO_INLINE_PRIM_AT");
    t1InlinePrimBasicNew = !envPresent("PHARO_T1_NO_INLINE_PRIM_BASIC_NEW");
    t1ICPolyWalk        =  envPresent("PHARO_T1_IC_POLY_WALK");
    t1InlineSistaCall   =  envPresent("PHARO_T1_INLINE_SISTA_CALL");
    // F3-NL6: default-on xmethod + non-leaf BV after F3-NL2 fix made
    // them correct.  Bench-suite: 1468 → 1217 ms (-17%) with both on.
    // Opt-out via PHARO_T1_NO_INLINE_J2J_XMETHOD / PHARO_T1_NO_INLINE_BLOCK_VALUE_NONLEAF.
    //
    // E2 2026-05-24: reverted xmethod to default-OFF after discovering
    // it breaks SessionManager startup-handler dispatch under JIT-on.
    // The X+BV SIGSEGV fix earlier this session (commit 0efb2086)
    // resolved the visible crash but the xmethod path still has
    // correctness issues that surface more subtly — handlers fork
    // but never get scheduled.
    //
    // 2026-05-28 PM: FLIPPED BACK TO ON, paired with inline-J2J=ON.
    // Bisection on Pharo-sunit run: INLINE_J2J=on + XMETHOD=on has the
    // EXACT SAME 23-FAIL set as inline-J2J=off — no new test failures
    // introduced, and fib(28) drops from 76 ms → 11 ms.  The
    // SessionManager-handler concern from May 24 doesn't reproduce
    // under the current code (the cleanUpInstanceVariables stress
    // probe fires through SessionManager startup with INLINE+XMETHOD
    // and runs 10000/10000 OK).
    // Opt out via PHARO_T1_NO_INLINE_J2J_XMETHOD=1.
    t1InlineJ2JXmethod  = !envPresent("PHARO_T1_NO_INLINE_J2J_XMETHOD");
    // 2026-06-09: default cap 30000 -> unlimited (-1).  The cap was a
    // bisection leftover; with the JM-offset gate fix (AsmjitT1
    // cross-method gates read the real canBailMidMethod/isStubOnEntry
    // bytes now) cross-method inline-J2J fires ~3.8M times in a cfib
    // bench run, and a 30000 cap silently re-routes everything after
    // the first 30000 calls back to the slow path (357ms vs 39ms).
    // The knob remains for bisection.
    t1InlineJ2JXmethodMax = envInt("PHARO_T1_INLINE_J2J_XMETHOD_MAX", -1);
    t1XmethodLog        = envPresent("PHARO_T1_XMETHOD_LOG");
    // Session H Phase 5 (2026-05-25): default-FLIP to leaf-only BV
    // inline.  Previous default (NonLeaf=ON) allowed BV inline for
    // blocks containing sends — those bail mid-bytecode to interp on
    // IC miss and the J2JSave/SavedFrame stacks diverge, causing
    // intermittent DNU corruption (ipOff=23680 signature documented
    // in memory/project_blocks_never_run_in_t1.md).  Restricting to
    // leaf blocks (numICEntries==0) + tail-only NLR detection (below)
    // gives 100% pass with bench numbers identical to default.
    // Opt-back-in via PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF=1.
    t1InlineBlockValueNonLeaf =
        envPresent("PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF");
    t1InlineBlockValueMax = envInt("PHARO_T1_INLINE_BLOCK_VALUE_MAX", -1);
    t1BvMaxIC = envInt("PHARO_T1_BV_MAX_IC", -1);
    t1BvMaxDepth = envInt("PHARO_T1_BV_MAX_DEPTH", -1);
    primProfile         = envPresent("PHARO_PRIM_PROFILE");
    t1EagerBlockCompile = envPresent("PHARO_T1_EAGER_BLOCK_COMPILE");
    t1J2JReceiverSync   = envPresent("PHARO_T1_J2J_RECEIVER_SYNC");
    t1J2JPostSendIp     = !envPresent("PHARO_T1_NO_J2J_POST_SEND_IP");
    // Session H Phase 5 (2026-05-25): default-ON.  Together with
    // leaf-only BV inline (above), this enables BV inline for leaf
    // blocks whose only 0x5D/0x5E is the natural tail BlockReturnTop.
    // 10/10 pass under the combo + identical bench numbers.
    // Opt-out via PHARO_T1_NO_NLR_TAIL_ONLY=1.
    t1NlrTailOnly        = !envPresent("PHARO_T1_NO_NLR_TAIL_ONLY");
    t1AllowNestedJitBail = envPresent("PHARO_T1_ALLOW_NESTED_JIT_BAIL");
    profile              = envPresent("PHARO_PROFILE");
    profileIntervalUs    = envInt("PHARO_PROFILE_INTERVAL_US", 1000);
    profileTopN          = envInt("PHARO_PROFILE_TOP", 30);
    sistaInterpHints     = !envPresent("PHARO_NO_SISTA_INTERP_HINTS");
    t1J2JSplitPool      = envPresent("PHARO_T1_J2J_SPLIT_POOL");
    // F3-NL6: default-on BV inline after F3-NL2/3/4 fixes.
    // PHARO_T1_NO_INLINE_BLOCK_VALUE=1 disables.
    //
    // 2026-05-28: DEFAULT FLIPPED TO OFF.  The BV-inline path
    // corrupts inner-block iteration when the outer caller is itself
    // doing `arr do:` over the same receiver and the inner block
    // compares captured outer-`each` against the inner-`each`.
    // Reproducer: `#($a $b $a $c $d) do: [:e | print: (arr2 occurrencesOf: e)]`
    // returns 3,1,3,1,1 instead of 2,1,2,1,1 with BV inline on.
    // Costs 22 SUnit FAILs in our focused-4-class run
    // (testAsArray/testAsBag/test0FixtureIterateTest etc. all hit
    // this pattern).  Flipping off lifts SUnit pass from 611/634
    // (96%) to 633/634 (99.8%) — only remaining issue is
    // CharacterTest>>testStoreStringAll (ERROR, unrelated).
    // Perf cost: fib28 10 ms -> 12 ms (small — fib doesn't use
    // blocks heavily).
    // Opt back in via PHARO_T1_INLINE_BLOCK_VALUE=1.
    t1InlineBlockValue  = envPresent("PHARO_T1_INLINE_BLOCK_VALUE");
    t1ProbeAlwaysMiss   = envPresent("PHARO_T1_PROBE_ALWAYS_MISS");
    t1HitAsMiss         = envPresent("PHARO_T1_HIT_AS_MISS");
    t1ICHitVerify       = envPresent("PHARO_T1_IC_HIT_VERIFY");
    t1TraceHit          = envPresent("PHARO_T1_TRACE_HIT");
    sortstrWatch        = envPresent("PHARO_SORTSTR_WATCH");
    driftCheck          = envPresent("PHARO_DRIFT_CHECK");
    t1ResumeTosLog      = envPresent("PHARO_T1_RESUME_TOS_LOG");
    // Default-on since 2026-05-17: short conditional jumps + 0xED/0xEE/0xEF
    // long jumps.  Unblocks benchFib + most other methods with control flow
    // from compiling as stubs.  PHARO_ASMJIT_T1_NO_JUMPS=1 to opt out.
    t1EnableJumps    = !envPresent("PHARO_ASMJIT_T1_NO_JUMPS");
    t1JumpsFirstN    = envInt("PHARO_ASMJIT_T1_JUMPS_FIRST_N", -1);
    t1JumpsOnlyN     = envInt("PHARO_ASMJIT_T1_JUMPS_ONLY_N",  -1);
    t1JumpsSkipN     = envInt("PHARO_ASMJIT_T1_JUMPS_SKIP_N",  -1);
    t1JumpsSkipFrom  = envInt("PHARO_ASMJIT_T1_JUMPS_SKIP_FROM", -1);
    t1JumpsSkipTo    = envInt("PHARO_ASMJIT_T1_JUMPS_SKIP_TO",   -1);
    t1NoSendsBisect  = envPresent("PHARO_ASMJIT_T1_NO_SENDS_BISECT");
    t1MaxSendNArgs   = envInt("PHARO_ASMJIT_T1_MAX_SEND_NARGS", 99);
    t1NoBlocks          = envPresent("PHARO_T1_NO_BLOCKS");
    t1BlocksFirstN      = envInt("PHARO_T1_BLOCKS_FIRST_N", -1);
    t1BlocksOnlyN       = envInt("PHARO_T1_BLOCKS_ONLY_N", -1);
    t1BlocksSkipFrom    = envInt("PHARO_T1_BLOCKS_SKIP_FROM", -1);
    t1BlocksSkipTo      = envInt("PHARO_T1_BLOCKS_SKIP_TO", -1);
    t1BlocksTrace       = envPresent("PHARO_T1_BLOCKS_TRACE");
    t1DumpSel           = envStr("PHARO_T1_DUMP_SEL");
    t1SkipSelectors     = envStr("PHARO_T1_SKIP_SELECTORS");
    traceExecSels       = envStr("PHARO_TRACE_EXEC_SELS");
    noJITResumeAfterReturn = envPresent("PHARO_NO_JIT_RESUME_AFTER_RETURN");
    noHeartbeat         = envPresent("PHARO_NO_HEARTBEAT");

    // --- JIT debug/trace ---
    jitDumpBC        = envPresent("JIT_DUMP_BC");
    jitSpillWarn     = envEq1("PHARO_JIT_SPILL_WARN");
    icHitDbg         = envPresent("PHARO_IC_HIT_DBG");
    icPatchDebug     = envPresent("PHARO_IC_PATCH_DEBUG");
    resumeStateDebug = envPresent("PHARO_RESUME_STATE_DEBUG");
    jitRetvalDbg     = envPresent("PHARO_JIT_RETVAL_DBG");
    jitMegaScan      = envPresent("PHARO_JIT_MEGA_SCAN");
    jitArithOflowTrace = envEq1("JIT_ARITH_OFLOW_TRACE");
    b5Trace          = envPresent("PHARO_B5_TRACE");
    jitSimStack      = envEq1("PHARO_JIT_SIMSTACK");
    jitNoSimStack    = envEq1("PHARO_JIT_NO_SIMSTACK");

    // --- Tier 2 ---
    t2Enabled        = envPresent("PHARO_T2");
    t2A1             = envPresent("PHARO_T2_A1");
    t2Replace        = envPresent("PHARO_T2_REPLACE");
    t2ForceMiss      = envPresent("PHARO_T2_FORCE_MISS");
    t2Verbose        = envPresent("PHARO_T2_VERBOSE");
    t2MbcJumps       = envPresent("PHARO_T2_MBC_JUMPS");
    {
        // PHARO_T2_MBC_JUMPS=0 disables, else (unset or any other val) enables.
        const char* env = std::getenv("PHARO_T2_MBC_JUMPS");
        t2MbcJumpsEnabled = !(env && env[0] == '0');
    }
    t2MbcSends       = envPresent("PHARO_T2_MBC_SENDS");
    t2MbcIC          = envPresent("PHARO_T2_MBC_IC");
    t2ZeroargIC      = envPresent("PHARO_T2_ZEROARG_IC");

    // --- Integer tunables ---
    jitMinSends      = envInt("PHARO_JIT_MIN_SENDS",  -1);
    jitMaxCompile    = envInt("JIT_MAX_COMPILE",      -1);
    jitDefer         = envInt("PHARO_JIT_DEFER",      -1);
    jitThreshold     = envInt("PHARO_JIT_THRESHOLD",  -1);
    t2Limit          = envInt("T2_LIMIT",             999);
    t2Warmup         = envInt("PHARO_T2_WARMUP",      3);
    fibN             = envInt("PHARO_FIB_N",          -1);
    b5Skip           = envInt("PHARO_B5_SKIP",        0);
    b5Max            = envInt("PHARO_B5_MAX",         0);
    recompileAt      = envInt("PHARO_RECOMPILE_AT",   500);

    // --- IC specialization opt-outs ---
    noBlock1Spec     = envPresent("PHARO_NO_BLOCK1_SPEC");
    noMonoJ2JSpec    = envPresent("PHARO_NO_MONOJ2J_SPEC");
    // MONOJ2J default-on; opt-out via PHARO_NO_MONOJ2J_SPEC.  Legacy
    // PHARO_MONOJ2J_SPEC still enables (no-op when default is on).
    monoJ2JSpec      = !noMonoJ2JSpec;

    // --- Strings ---
    benchType              = envStr("PHARO_BENCH");
    awfyOnly               = envStr("PHARO_AWFY_ONLY");
    jitExclude             = envStr("JIT_EXCLUDE");
    jitExcludeOop          = envStr("JIT_EXCLUDE_OOP");
    jitSkipSelectors       = envStr("PHARO_JIT_SKIP_SELECTORS");
    jitNoSimStackSelectors = envStr("PHARO_JIT_NO_SIMSTACK_SELECTORS");
    j2jSkipSelectors       = envStr("PHARO_J2J_SKIP_SELECTORS");
    j2jOnlySelectors       = envStr("PHARO_J2J_ONLY_SELECTORS");
    jitDumpBCPre           = envStr("JIT_DUMP_BC_PRE");
    jitDumpBCPost          = envStr("JIT_DUMP_BC_POST");
    jitDumpHex             = envStr("JIT_DUMP_HEX");
    jitDumpSel             = envStr("PHARO_JIT_DUMP_SEL");
    b5Focus                = envStr("PHARO_B5_FOCUS");
    jitTraceOop            = envStr("PHARO_JIT_TRACE_OOP");
    jitTop                 = envStr("PHARO_JIT_TOP");
    sistaCompile           = envPresent("PHARO_SISTA_COMPILE");
    // Default ON on arm64 hosts (where SistaLowering/Tier2Compiler emit
    // working asmjit::a64 code).  Default OFF elsewhere — the lowering
    // path still hardcodes asmjit::a64 instructions on every host, so
    // running it on x86_64 hits a SIGSEGV at fault 0x358.  PHARO_NO_SISTA
    // forces OFF on any host; PHARO_SISTA_DISPATCH=1 forces ON (useful
    // when bisecting Sista-x86_64 lowering work in progress).
    {
#if defined(__aarch64__)
        constexpr bool kDefaultSistaOn = true;
#else
        constexpr bool kDefaultSistaOn = false;
#endif
        const bool noSista = envPresent("PHARO_NO_SISTA");
        const bool explicitOn = envPresent("PHARO_SISTA_DISPATCH");
        sistaDispatch = explicitOn || (kDefaultSistaOn && !noSista);
    }
    sistaVerbose           = envPresent("PHARO_SISTA_VERBOSE");
    sistaAllowSends        = envPresent("PHARO_SISTA_ALLOW_SENDS");
    sistaSend0Only         = envPresent("PHARO_SISTA_SEND0_ONLY");
    sistaUnsafeArith       = envPresent("PHARO_SISTA_UNSAFE_ARITH");
    sistaAllowBail         = envPresent("PHARO_SISTA_ALLOW_BAIL");
    sistaNoBail            = envPresent("PHARO_SISTA_NO_BAIL");
    // Generational GC default ON (2026-04-24).  Bench wins 2-9x on
    // allocation-heavy workloads (factorial 231→26ms beats Cog).
    // Cuts fullGC samples ~50%.  Override to OFF only if PHARO_NO_YG is
    // set; PHARO_YOUNG_GEN=1 still forces ON for backward compat.
    {
        const bool noYG = envPresent("PHARO_NO_YG") || envPresent("PHARO_NO_YOUNGGEN");
        const bool explicitOn = envPresent("PHARO_YOUNG_GEN");
        youngGenEnabled = explicitOn || !noYG;
    }
    ygNoScavenge           = envPresent("PHARO_YG_NO_SCAVENGE");
    finalizeDeferred       = !envPresent("PHARO_INLINE_FINALIZE");
    gcHeadroomMB           = envInt("PHARO_GC_HEADROOM_MB", 0);
    t1SetterBarrier        = envPresent("PHARO_T1_SETTER_BARRIER");
    scavBitAudit           = envPresent("PHARO_SCAV_BIT_AUDIT");

    // --- Newly migrated trace / debug bools (mass conversion) ---
    a1Trace                          = envPresent("PHARO_A1_TRACE");
    asmjitT1BctocodeZero             = envPresent("PHARO_ASMJIT_T1_BCTOCODE_ZERO");
    asmjitT1ForceResumeForSends      = envPresent("PHARO_ASMJIT_T1_FORCE_RESUME_FOR_SENDS");
    asmjitT1ResumeSendsNoCondjump    = envPresent("PHARO_T1_RESUME_SENDS_NO_CONDJUMP");
    asmjitT1HardcodeStub             = envPresent("PHARO_ASMJIT_T1_HARDCODE_STUB");
    asmjitT1Log                      = envPresent("PHARO_ASMJIT_T1_LOG");
    asmjitT1NoBctocode               = envPresent("PHARO_ASMJIT_T1_NO_BCTOCODE");
    asmjitT1NoNumbc                  = envPresent("PHARO_ASMJIT_T1_NO_NUMBC");
    asmjitT1NoTrim                   = envPresent("PHARO_ASMJIT_T1_NO_TRIM");
    asmjitT1StubOnly                 = envPresent("PHARO_ASMJIT_T1_STUB_ONLY");
    asmjitT1TraceCond                = envPresent("PHARO_ASMJIT_T1_TRACE_COND");
    basicAtTrace                     = envPresent("PHARO_BASICAT_TRACE");
    bc5Dump                          = envPresent("PHARO_BC5_DUMP");
    ctxTempAtDbg                     = envPresent("PHARO_CTX_TEMPAT_DBG");
    detectErrors                     = envPresent("PHARO_DETECT_ERRORS");
    detectErrorsLimit                = envInt("PHARO_DETECT_ERRORS_LIMIT", 30);
    dumpInterpOffsets                = envPresent("PHARO_DUMP_INTERP_OFFSETS");
    dumpRecompileIC                  = envStrRaw("PHARO_DUMP_RECOMPILE_IC");
    gcLog                            = envPresent("PHARO_GC_LOG");
    icHistogram                      = envPresent("PHARO_IC_HISTOGRAM");
    improperStoreTrace               = envPresent("PHARO_IMPROPER_STORE_TRACE");
    inlineActivateNoBailMid          = envPresent("PHARO_INLINE_ACTIVATE_NO_BAIL_MID");
    inlineActivateStubs              = envPresent("PHARO_INLINE_ACTIVATE_STUBS");
    inlinePrimDebug                  = envPresent("PHARO_INLINE_PRIM_DEBUG");
    t1GetterClassifyLog              = envPresent("PHARO_T1_GETTER_CLASSIFY_LOG");
    jitFailReasons                   = envPresent("PHARO_JIT_FAIL_REASONS");
    jitKeepICs                       = envEq1("PHARO_JIT_KEEP_ICS");
    jitStaleLog                      = envEq1("PHARO_JIT_STALE_LOG");
    jitTraceRecompile                = envPresent("PHARO_JIT_TRACE_RECOMPILE");
    jitValidateEntry                 = envPresent("PHARO_JIT_VALIDATE_ENTRY");
    mnuAllocDbg                      = envPresent("PHARO_MNU_ALLOC_DBG");
    noBlockBit                       = envPresent("PHARO_NO_BLOCK_BIT");
    noBlockValueSpec                 = envPresent("PHARO_NO_BLOCK_VALUE_SPEC");
    noCullICFill                     = envPresent("PHARO_NO_CULL_IC_FILL");
    noCullMega                       = envPresent("PHARO_NO_CULL_MEGA");
    noEagerBlockValue                = envPresent("PHARO_NO_EAGER_BLOCK_VALUE");
    noFwdCollapse                    = envPresent("PHARO_NO_FWD_COLLAPSE");
    noGetterBit                      = envPresent("PHARO_NO_GETTER_BIT");
    noGetterBitBisect                = envStrRaw("PHARO_NO_GETTER_BIT_BISECT");
    noHotLoopThreshold               = envPresent("PHARO_NO_HOT_LOOP_THRESHOLD");
    noJ2JCalleeBump                  = envPresent("PHARO_NO_J2J_CALLEE_BUMP");
    noJ2JInlineBump                  = envPresent("PHARO_NO_J2J_INLINE_BUMP");
    noLateSpecRecompile              = envPresent("PHARO_NO_LATE_SPEC_RECOMPILE");
    noMegahitICFill                  = envPresent("PHARO_NO_MEGAHIT_IC_FILL");
    noMultislotGetter                = envPresent("PHARO_NO_MULTISLOT_GETTER");
    noOSRRecompile                   = envPresent("PHARO_NO_OSR_RECOMPILE");
    noQueueCompile                   = envPresent("PHARO_NO_QUEUE_COMPILE");
    noRetLit                         = envPresent("PHARO_NO_RETLIT");
    // DEFAULT-OFF (2026-06-07): the collect:/select: splice elides
    // `recv select: [block]` into kCountedLoopArraySelect, but its deopt/bail
    // back to the interpreter mis-reconstructs the send stack (the spliced-away
    // block is not restored as the arg) -> `Collection>>reject:` sends #select:
    // to a FullBlockClosure -> MNU.  This broke reject:/copyWithout: across ~12
    // collection classes (~100+ SUnit errors; e.g. BagTest 12, HeapTest 14),
    // all of which pass with the splice off.  Disabled by default until the
    // deopt reconstruction is fixed; opt back in with PHARO_SISTA_COLLECT=1.
    // (Same correctness-over-a-broken-Sista-opt pattern as HELPER_SENDS.)
    noSistaCollect                   = !envEq1("PHARO_SISTA_COLLECT");
    noSistaCollectResume             = envPresent("PHARO_NO_SISTA_COLLECT_RESUME");
    noSistaDoSplice                  = envPresent("PHARO_NO_SISTA_DO_SPLICE");
    sistaDoSpliceNoHint              = envEq1("PHARO_SISTA_DO_SPLICE_NO_HINT");
    noSistaDoAccumResume             = envPresent("PHARO_NO_SISTA_DOACCUM_RESUME");
    // 2026-06-04: HELPER_SENDS is now DEFAULT-OFF.  A Sista helper-send
    // activation leaves a broken Context sender chain, so exception handler
    // search cannot find an enclosing handler: an unhandled error in a forked
    // process (e.g. a SUnit test raising CollectionIsEmpty past its on:Error
    // handler) escapes, the process is abandoned WITHOUT running its ensure:,
    // and any waiter on a semaphore that ensure: would signal deadlocks (the
    // SUnit full-suite hang — CollectionArithmeticTest etc.).  Confirmed:
    // PHARO_NO_SISTA_HELPER_SENDS=1 made the hanging class pass 20/20 on both
    // the interpreter and Sista; default-on hung both.  Disabled until the
    // helper-send activation reifies a correct sender chain.  Re-enable with
    // PHARO_SISTA_HELPER_SENDS=1.
    noSistaHelperSends               = !envEq1("PHARO_SISTA_HELPER_SENDS");
    noSistaHelperSendsStrict         = !envEq1("PHARO_SISTA_HELPER_SENDS");
    noSistaInjectResume              = envPresent("PHARO_NO_SISTA_INJECT_RESUME");
    noSistaInlineArithIVar           = envPresent("PHARO_NO_SISTA_INLINE_ARITHIVAR");
    noSistaInlineIdentityEq          = envPresent("PHARO_NO_SISTA_INLINE_IDENTITY_EQ");
    noSistaInlineYourself            = envPresent("PHARO_NO_SISTA_INLINE_YOURSELF");
    noSistaIvDoAccum                 = envPresent("PHARO_NO_SISTA_IV_DO_ACCUM");
    noSistaPerBC                     = envPresent("PHARO_NO_SISTA_PER_BC");
    noSistaPrimAtPut                 = envPresent("PHARO_NO_SISTA_PRIM_AT_PUT");
    noSistaWhileTrue                 = envPresent("PHARO_NO_SISTA_WHILETRUE");
    p63Trace                         = envPresent("PHARO_P63_TRACE");
    primAtDebug                      = envPresent("PHARO_PRIMAT_DEBUG");
    primAtOob                        = envPresent("PHARO_PRIMAT_OOB");
    primSizeDebug                    = envPresent("PHARO_PRIMSIZE_DEBUG");
    primSizeStencilDbg               = envPresent("PHARO_PRIMSIZE_STENCIL_DBG");
    procDump                         = envPresent("PHARO_PROC_DUMP");
    rj2jValidate                     = envPresent("PHARO_RJ2J_VALIDATE");
    sdlTrace                         = envPresent("PHARO_SDL_TRACE");
    semSignalTrace                   = envPresent("PHARO_SEM_SIGNAL_TRACE");
    senderTripwire                   = envPresent("PHARO_SENDER_TRIPWIRE");
    sistaAfterT1                     = envPresent("PHARO_SISTA_AFTER_T1");
    sistaAllocArrayTrace             = envPresent("PHARO_SISTA_ALLOC_ARRAY_TRACE");
    sistaAllowArrayDoHelper          = envPresent("PHARO_SISTA_ALLOW_ARRAYDO_HELPER");
    sistaAsmjitLog                   = envPresent("PHARO_SISTA_ASMJIT_LOG");
    sistaAtPeephole                  = envPresent("PHARO_SISTA_AT_PEEPHOLE");
    sistaBailLog                     = envPresent("PHARO_SISTA_BAIL_LOG");
    sistaBlockBail                   = envPresent("PHARO_SISTA_BLOCK_BAIL");
    sistaBlockHelperTrace            = envPresent("PHARO_SISTA_BLOCK_HELPER_TRACE");
    sistaCollectResumeForceBail      = envPresent("PHARO_SISTA_COLLECT_RESUME_FORCE_BAIL");
    sistaCompileBailOnly             = envPresent("PHARO_SISTA_COMPILE_BAIL_ONLY");
    sistaDoDetect                    = envPresent("PHARO_SISTA_DO_DETECT");
    sistaDoAccumForceBail            = envPresent("PHARO_SISTA_DOACCUM_FORCE_BAIL");
    sistaExcludeSels                 = envStrRaw("PHARO_SISTA_EXCLUDE_SELS");
    sistaHelperForceBail             = envPresent("PHARO_SISTA_HELPER_FORCE_BAIL");
    sistaHelperMaxDepth              = envInt("PHARO_SISTA_HELPER_MAX_DEPTH", 64);
    if (sistaHelperMaxDepth <= 0) sistaHelperMaxDepth = 64;  // preserve old guard
    sistaInjectResumeForceBail       = envPresent("PHARO_SISTA_INJECT_RESUME_FORCE_BAIL");
    sistaInlineDbg                   = envPresent("PHARO_SISTA_INLINE_DBG");
    sistaInlineDump                  = envPresent("PHARO_SISTA_INLINE_DUMP");
    sistaInlinePoly                  = !envPresent("PHARO_NO_SISTA_INLINE_POLY");
    sistaInlineSelf                  = envPresent("PHARO_SISTA_INLINE_SELF");
    sistaInlineStats                 = envPresent("PHARO_SISTA_INLINE_STATS");
    sistaInvariantCheck              = envPresent("PHARO_SISTA_INVARIANT_CHECK");
    sistaNoFullblock                 = envPresent("PHARO_SISTA_NO_FULLBLOCK");
    sistaNoInlineArith               = envPresent("PHARO_SISTA_NO_INLINE_ARITH");
    sistaNoInlineConst               = envPresent("PHARO_SISTA_NO_INLINE_CONST");
    sistaNoInlineSetters             = envPresent("PHARO_SISTA_NO_INLINE_SETTERS");
    sistaNoLower                     = envPresent("PHARO_SISTA_NO_LOWER");
    sistaNoLowerAdd                  = envPresent("PHARO_SISTA_NO_LOWER_ADD");
    sistaNoLowerArith                = envPresent("PHARO_SISTA_NO_LOWER_ARITH");
    sistaNoLowerArithCmp             = envPresent("PHARO_SISTA_NO_LOWER_ARITH_CMP");
    sistaNoLowerArithMath            = envPresent("PHARO_SISTA_NO_LOWER_ARITH_MATH");
    sistaNoLowerBody                 = envPresent("PHARO_SISTA_NO_LOWER_BODY");
    sistaNoLowerBranch               = envPresent("PHARO_SISTA_NO_LOWER_BRANCH");
    sistaNoLowerFuse                 = envPresent("PHARO_SISTA_NO_LOWER_FUSE");
    sistaNoLowerSends                = envPresent("PHARO_SISTA_NO_LOWER_SENDS");
    // x86 per-op disable knobs migrated to debug_vars.h.
    sistaNoRemoteTemp                = envPresent("PHARO_SISTA_NO_REMOTE_TEMP");
    sistaNoRemoteTempRead            = envPresent("PHARO_SISTA_NO_REMOTE_TEMP_READ");
    sistaNoRemoteTempWrite           = envPresent("PHARO_SISTA_NO_REMOTE_TEMP_WRITE");
    sistaNoStores                    = envPresent("PHARO_SISTA_NO_STORES");
    sistaPerBCBailTrace              = envPresent("PHARO_SISTA_PER_BC_BAIL_TRACE");
    sistaPerBCDispatchTrace          = envPresent("PHARO_SISTA_PER_BC_DISPATCH_TRACE");
    sistaPerBCNoDispatch             = envPresent("PHARO_SISTA_PER_BC_NO_DISPATCH");
    sistaPerBCTrace                  = envPresent("PHARO_SISTA_PER_BC_TRACE");
    sistaBjTrace                     = envPresent("PHARO_SISTA_BJ_TRACE");
    sistaRekeyAfterGC                = envPresent("PHARO_SISTA_REKEY_AFTER_GC");
    sistaSizePeephole                = envPresent("PHARO_SISTA_SIZE_PEEPHOLE");
    sistaStackWatch                  = envPresent("PHARO_SISTA_STACK_WATCH");
    sistaT1Warmup                    = envInt("PHARO_SISTA_T1_WARMUP", 100);
    if (sistaT1Warmup <= 0) sistaT1Warmup = 100;  // preserve old guard
    sistaTempWatch                   = envPresent("PHARO_SISTA_TEMP_WATCH");
    sistaTraceAdd                    = envPresent("PHARO_SISTA_TRACE_ADD");
    sistaX86TraceOk                  = envPresent("PHARO_SISTA_X86_TRACE_OK");
    slotTripwire                     = envPresent("PHARO_SLOT_TRIPWIRE");
    spCorruptTrap                    = envPresent("PHARO_SP_CORRUPT_TRAP");
    t1AcceptExtSuperSend             = envPresent("PHARO_T1_ACCEPT_EXTSUPERSEND");
    t1InlineJ2J_Env                  = envPresent("PHARO_T1_INLINE_J2J");
    t1InlineJ2JDumpBC                = envPresent("PHARO_T1_INLINE_J2J_DUMP_BC");
    t1InlineJ2JTraceUnsupp           = envPresent("PHARO_T1_INLINE_J2J_TRACE_UNSUPP");
    t1InlineJ2JXmethodLive           = envPresent("PHARO_T1_INLINE_J2J_XMETHOD_LIVE");
    t1InlineJ2JXmethodLog            = envPresent("PHARO_T1_INLINE_J2J_XMETHOD_LOG");
    t1InlinePrimCounters             = envPresent("PHARO_T1_INLINE_PRIM_COUNTERS");
    t1NoJ2JTramp                     = envPresent("PHARO_T1_NO_J2J_TRAMP");
    t1SistaDispatchAllow             = envPresent("PHARO_T1_SISTA_DISPATCH_ALLOW");
    t1TraceEmit                      = envPresent("PHARO_T1_TRACE_EMIT");
    t1SelfRecSplice                  = envPresent("PHARO_T1_SELF_REC_SPLICE");
    t1SelfRecSpliceHintless          = envPresent("PHARO_T1_SELF_REC_SPLICE_HINTLESS");
    t1SelfRecSpliceDepth             = envInt("PHARO_T1_SELF_REC_SPLICE_DEPTH", 1);
    sistaDispatchMultiBlock          = envPresent("PHARO_SISTA_DISPATCH_MULTIBLOCK");
    t2X86Log                         = envPresent("PHARO_T2_X86_LOG");
    t2X86Trace                       = envPresent("PHARO_T2_X86_TRACE");
    t2Strict                         = envEq1("PHARO_T2");
    termBT                           = envPresent("PHARO_TERM_BT");
    timeGCPhases                     = envPresent("PHARO_TIME_GC_PHASES");
    traceActivateBlock               = envPresent("PHARO_TRACE_ACTIVATE_BLOCK");
    traceBC7A                        = envPresent("PHARO_TRACE_BC_7A");
    traceCullBail                    = envPresent("PHARO_TRACE_CULL_BAIL");
    traceCullEntry                   = envPresent("PHARO_TRACE_CULL_ENTRY");
    traceCullReturn                  = envPresent("PHARO_TRACE_CULL_RETURN");
    traceExceptionSel                = envPresent("PHARO_TRACE_EXCEPTION_SEL");
    traceExecPrim                    = envPresent("PHARO_TRACE_EXEC_PRIM");
    traceExit                        = envPresent("PHARO_TRACE_EXIT");
    traceOpValue1                    = envPresent("PHARO_TRACE_OP_VALUE1");
    tracePerBCSp                     = envPresent("PHARO_TRACE_PER_BC_SP");
    tracePrim207                     = envPresent("PHARO_TRACE_PRIM207");
    traceRecompileFlow               = envPresent("PHARO_TRACE_RECOMPILE_FLOW");
    traceResumeSp                    = envPresent("PHARO_TRACE_RESUME_SP");
    traceRewriteIC                   = envPresent("PHARO_TRACE_REWRITE_IC");
    traceShouldNotImpl               = envPresent("PHARO_TRACE_SHOULDNOTIMPL");
    traceSistaDispatch               = envPresent("PHARO_TRACE_SISTA_DISPATCH");
    traceSistaPerBC                  = envPresent("PHARO_TRACE_SISTA_PERBC");
    traceSpCorrupt                   = envPresent("PHARO_TRACE_SP_CORRUPT");
    traceStackOrigin                 = envPresent("PHARO_TRACE_STACK_ORIGIN");
    traceTotalSteps                  = envPresent("PHARO_TRACE_TOTAL_STEPS");
    traceValueAct                    = envPresent("PHARO_TRACE_VALUE_ACT");
    trapBadDnu                       = envPresent("PHARO_TRAP_BAD_DNU");
    xferTrace                        = envPresent("PHARO_XFER_TRACE");
    // OS environment strings (lifetime tied to process env)
    envHome                          = std::getenv("HOME");
    envUser                          = std::getenv("USER");
    envLogname                       = std::getenv("LOGNAME");
    envLang                          = std::getenv("LANG");
    envLcAll                         = std::getenv("LC_ALL");
    envTmpdir                        = std::getenv("TMPDIR");
}

const char* DebugSettings::envRuntime(const char* name) {
    return std::getenv(name);
}

DebugSettings g_debug;  // static-storage-duration; constructor runs before main().

}  // namespace pharo
