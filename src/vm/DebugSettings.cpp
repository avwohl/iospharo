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

inline int envInt(const char* name, int defaultVal) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::atoi(v) : defaultVal;
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
    t1InlineGetter      = !envPresent("PHARO_T1_NO_INLINE_GETTER");
    t1InlineSetter      = !envPresent("PHARO_T1_NO_INLINE_SETTER");
    t1InlineReturnsSelf = !envPresent("PHARO_T1_NO_INLINE_RETURNS_SELF");
    // 2026-05-21 (A6 iter N+30k): default-ON.  The pure-J2J gate
    // (PHARO_T1_NO_PURE_J2J_GATE=1 to disable) prevents inline-J2J
    // from firing when any IC site lacks bit 60 (J2J_ENTRY_BIT),
    // which avoids the materialize-bail wrong-result bug that
    // affected benchFib(N>=17).  See AsmjitT1.cpp inline-J2J emit
    // and deferred.md A6 N+30k.  PHARO_T1_NO_INLINE_J2J=1 opts out
    // entirely (legacy workaround).
    t1InlineJ2J         = !envPresent("PHARO_T1_NO_INLINE_J2J");
    // 2026-05-21 (jit-may20 Step 2): pure-J2J gate replaced by warmth
    // gate as the default.  Pure gate (bit-60 check) was too strict —
    // bailed ~96.5% of inline-J2J attempts on fib.  Warmth gate
    // (entry0.key == 0 check) catches the same wrong-result class of
    // bug with a 99%+ catch rate.  Pure gate kept as opt-in via
    // PHARO_T1_PURE_J2J_GATE=1 for bisection; warmth gate disabled via
    // PHARO_T1_NO_WARM_J2J_GATE=1.
    t1PureJ2JGate       = envPresent("PHARO_T1_PURE_J2J_GATE");
    t1WarmJ2JGate       = !envPresent("PHARO_T1_NO_WARM_J2J_GATE");
    t1InlinePrimBitOps  = !envPresent("PHARO_T1_NO_INLINE_PRIM_BITOPS");
    t1InlinePrimAt      = !envPresent("PHARO_T1_NO_INLINE_PRIM_AT");
    t1InlineJ2JXmethod  = envPresent("PHARO_T1_INLINE_J2J_XMETHOD");
    t1InlineJ2JXmethodMax = envInt("PHARO_T1_INLINE_J2J_XMETHOD_MAX", 30000);
    t1XmethodLog        = envPresent("PHARO_T1_XMETHOD_LOG");
    t1InlineBlockValueNonLeaf =
        envPresent("PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF");
    primProfile         = envPresent("PHARO_PRIM_PROFILE");
    t1EagerBlockCompile = envPresent("PHARO_T1_EAGER_BLOCK_COMPILE");
    t1J2JReceiverSync   = envPresent("PHARO_T1_J2J_RECEIVER_SYNC");
    t1J2JPostSendIp     = envPresent("PHARO_T1_J2J_POST_SEND_IP");
    t1J2JSplitPool      = envPresent("PHARO_T1_J2J_SPLIT_POOL");
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
    jitSpillWarn     = envPresent("PHARO_JIT_SPILL_WARN");
    icHitDbg         = envPresent("PHARO_IC_HIT_DBG");
    icPatchDebug     = envPresent("PHARO_IC_PATCH_DEBUG");
    resumeStateDebug = envPresent("PHARO_RESUME_STATE_DEBUG");
    jitRetvalDbg     = envPresent("PHARO_JIT_RETVAL_DBG");
    jitMegaScan      = envPresent("PHARO_JIT_MEGA_SCAN");
    jitArithOflowTrace = envPresent("JIT_ARITH_OFLOW_TRACE");
    b5Trace          = envPresent("PHARO_B5_TRACE");
    jitSimStack      = envPresent("PHARO_JIT_SIMSTACK");
    jitNoSimStack    = envPresent("PHARO_JIT_NO_SIMSTACK");

    // --- Tier 2 ---
    t2Enabled        = envPresent("PHARO_T2");
    t2A1             = envPresent("PHARO_T2_A1");
    t2Replace        = envPresent("PHARO_T2_REPLACE");
    t2ForceMiss      = envPresent("PHARO_T2_FORCE_MISS");
    t2Verbose        = envPresent("PHARO_T2_VERBOSE");
    t2MbcJumps       = envPresent("PHARO_T2_MBC_JUMPS");
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
}

DebugSettings g_debug;  // static-storage-duration; constructor runs before main().

}  // namespace pharo
