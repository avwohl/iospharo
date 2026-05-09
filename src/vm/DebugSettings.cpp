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
    // Default ON — override to OFF only if PHARO_NO_SISTA is set.
    // PHARO_SISTA_DISPATCH still forces ON (backward compat).
    {
        const bool noSista = envPresent("PHARO_NO_SISTA");
        const bool explicitOn = envPresent("PHARO_SISTA_DISPATCH");
        sistaDispatch = explicitOn || !noSista;
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
