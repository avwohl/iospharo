/*
 * DebugSettings.hpp - Centralized env-var debug/control flags
 *
 * Every PHARO_*, JIT_*, and T2_* debug/control knob that the VM reads
 * from the environment lives here.  The constructor parses getenv()
 * for all of them once at static-initialization time (before main());
 * callers then read g_debug.fieldName directly.  This replaces ~100
 * scattered getenv() calls with a single field access.
 *
 * Why centralize:
 *   - getenv does a linear scan of environ[] each call.  static caching
 *     at each site helps but spreads the knowledge of what flags exist.
 *   - A single struct is greppable (want to know every knob?  read this
 *     file) and dumpable at startup for debugging.
 *   - Tests can set fields directly without touching the process env.
 *
 * Adding a new flag: add a field + initializer in the constructor
 * (DebugSettings.cpp), use g_debug.fieldName at the call site.
 */
#ifndef PHARO_DEBUG_SETTINGS_HPP
#define PHARO_DEBUG_SETTINGS_HPP

namespace pharo {

struct DebugSettings {
    // --- General debug/trace booleans ---
    bool debugFrameLeak = false;      // PHARO_DEBUG_FRAME_LEAK
    bool debugDispLeak = false;       // PHARO_DEBUG_DISP_LEAK (checks == '1')
    bool delayDebug = false;          // PHARO_DELAY_DEBUG
    bool gcEphDebug = false;          // PHARO_GC_EPH_DEBUG
    bool reflectProfile = false;      // PHARO_REFLECT_PROFILE
    bool timerDebug = false;          // PHARO_TIMER_DEBUG
    bool callbackDebug = false;       // PHARO_CALLBACK_DEBUG
    bool bench = false;               // PHARO_BENCH (presence)
    bool debugArithExit = false;      // PHARO_DEBUG_ARITH_EXIT

    // --- JIT on/off switches ---
    bool noJit = false;               // PHARO_NO_JIT or PHARO_NOJIT
    bool noOSR = false;               // PHARO_NO_OSR
    bool noJ2J = false;               // PHARO_NO_J2J
    bool noICFill = false;            // PHARO_NO_IC_FILL
    bool noChain = false;             // PHARO_NO_CHAIN
    bool noEagerCompile = false;      // PHARO_NO_EAGER_COMPILE
    bool noBlocks = false;            // PHARO_JIT_NO_BLOCKS
    bool noResume = false;            // PHARO_NO_RESUME
    bool resumeJ2J = false;           // PHARO_RESUME_J2J (external resume trampoline)

    // --- JIT debug/trace ---
    bool jitDumpBC = false;           // JIT_DUMP_BC
    bool jitSpillWarn = false;        // PHARO_JIT_SPILL_WARN (presence)
    bool icHitDbg = false;            // PHARO_IC_HIT_DBG
    bool icPatchDebug = false;        // PHARO_IC_PATCH_DEBUG
    bool resumeStateDebug = false;    // PHARO_RESUME_STATE_DEBUG
    bool jitRetvalDbg = false;        // PHARO_JIT_RETVAL_DBG
    bool jitMegaScan = false;         // PHARO_JIT_MEGA_SCAN
    bool jitArithOflowTrace = false;  // JIT_ARITH_OFLOW_TRACE (presence)
    bool b5Trace = false;             // PHARO_B5_TRACE
    bool jitSimStack = false;         // PHARO_JIT_SIMSTACK (force on)
    bool jitNoSimStack = false;       // PHARO_JIT_NO_SIMSTACK (force off)

    // --- Tier 2 / asmjit ---
    bool t2Enabled = false;           // PHARO_T2 (any value)
    bool t2A1 = false;                // PHARO_T2_A1
    bool t2Replace = false;           // PHARO_T2_REPLACE
    bool t2ForceMiss = false;         // PHARO_T2_FORCE_MISS
    bool t2Verbose = false;           // PHARO_T2_VERBOSE
    bool t2MbcJumps = false;          // PHARO_T2_MBC_JUMPS
    bool t2MbcSends = false;          // PHARO_T2_MBC_SENDS
    bool t2MbcIC = false;             // PHARO_T2_MBC_IC
    bool t2ZeroargIC = false;         // PHARO_T2_ZEROARG_IC

    // --- Integer-valued tunables.  -1 / sentinel means "unset". ---
    int jitMinSends = -1;             // PHARO_JIT_MIN_SENDS
    int jitMaxCompile = -1;           // JIT_MAX_COMPILE
    int jitDefer = -1;                // PHARO_JIT_DEFER (ms)
    int jitThreshold = -1;            // PHARO_JIT_THRESHOLD
    int t2Limit = 999;                // T2_LIMIT
    int t2Warmup = 3;                 // PHARO_T2_WARMUP
    int fibN = -1;                    // PHARO_FIB_N
    int b5Skip = 0;                   // PHARO_B5_SKIP
    int b5Max = 0;                    // PHARO_B5_MAX

    // --- String-valued.  nullptr if env var unset or empty. ---
    const char* benchType = nullptr;               // PHARO_BENCH (value)
    const char* awfyOnly = nullptr;                // PHARO_AWFY_ONLY
    const char* jitExclude = nullptr;              // JIT_EXCLUDE
    const char* jitExcludeOop = nullptr;           // JIT_EXCLUDE_OOP
    const char* jitSkipSelectors = nullptr;        // PHARO_JIT_SKIP_SELECTORS
    const char* jitNoSimStackSelectors = nullptr;  // PHARO_JIT_NO_SIMSTACK_SELECTORS
    const char* j2jSkipSelectors = nullptr;        // PHARO_J2J_SKIP_SELECTORS
    const char* jitDumpBCPre = nullptr;            // JIT_DUMP_BC_PRE
    const char* jitDumpBCPost = nullptr;           // JIT_DUMP_BC_POST
    const char* jitDumpHex = nullptr;              // JIT_DUMP_HEX
    const char* jitDumpSel = nullptr;              // PHARO_JIT_DUMP_SEL
    const char* b5Focus = nullptr;                 // PHARO_B5_FOCUS
    const char* jitTraceOop = nullptr;             // PHARO_JIT_TRACE_OOP
    const char* jitTop = nullptr;                  // PHARO_JIT_TOP
    bool sistaCompile = false;                     // PHARO_SISTA_COMPILE
    bool sistaDispatch = false;                    // PHARO_SISTA_DISPATCH
    bool sistaVerbose = false;                     // PHARO_SISTA_VERBOSE
    bool sistaAllowSends = false;                  // PHARO_SISTA_ALLOW_SENDS
    bool sistaSend0Only = false;                   // PHARO_SISTA_SEND0_ONLY
    bool sistaUnsafeArith = false;                 // PHARO_SISTA_UNSAFE_ARITH

    // The constructor reads every env var listed above.  C++ guarantees
    // static-storage-duration objects are initialized before main(), and
    // by that time the process environment is fully populated, so this
    // is always safe.
    DebugSettings();

    // Re-read env vars.  Call after any code that mutates the environment
    // via setenv(), e.g., test_load_image auto-setting PHARO_NO_JIT.
    // Equivalent to constructing a fresh DebugSettings and copying over.
    void reload();
};

extern DebugSettings g_debug;

}  // namespace pharo

#endif  // PHARO_DEBUG_SETTINGS_HPP
