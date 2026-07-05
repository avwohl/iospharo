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
    bool delayDebug = false;          // PHARO_DELAY_DEBUG
    bool gcEphDebug = false;          // PHARO_GC_EPH_DEBUG
    bool timerDebug = false;          // PHARO_TIMER_DEBUG

    // --- JIT on/off switches ---
    bool noJit = false;               // PHARO_NO_JIT or PHARO_NOJIT
    bool noJitStrict = false;         // PHARO_NO_JIT set to non-"0" value (true disable)

    // --- asmjit-T1 path (default JIT since phase4b.37) ---
    // The asmjit-T1 emitter replaces the legacy stencil JIT.  See
    // scripts/jit-diff/plan_asmjit_replacement.md.  Default ON;
    // PHARO_NO_ASMJIT_T1=1 falls back to the stencil pipeline (which is
    // 0/37 on the differential fuzzer corpus and only retained for
    // bisection / regression checks).  PHARO_USE_ASMJIT_T1=1 is still
    // accepted as an explicit opt-in (no-op when default is on).
    bool useAsmjitT1 = true;          // PHARO_USE_ASMJIT_T1 / opt-out PHARO_NO_ASMJIT_T1

    // --- asmjit-T1 simplification / bisect knobs ---
    // Splits of the original PHARO_T1_FORCE_SIMPLE which set all three.
    bool t1ForceBailMid = false;      // PHARO_T1_FORCE_BAIL_MID — force canBailMidMethod=true
    // t1CanSkipJ2JSave REMOVED — knob moved to debug_vars.h (PHARO_T1_NO_CAN_SKIP_J2J_SAVE).
    bool t1NoBlockResume = false;     // PHARO_T1_NO_BLOCK_RESUME — skip chain-loop block-resume tryExecute
    bool t1NoPostPrimResume = false;  // PHARO_T1_NO_POST_PRIM_RESUME — skip post-prim tryResume
    // Inline IC probe in JIT send emit (DEFAULT-OFF until correctness
    // is established).  When on, every send site reads icData[0] and
    // exits with ExitSendCached on a match — skipping the chain loop's
    // method-lookup + IC-patch path.  Diagnostic only as of 2026-05-15:
    // empirically yields 37% raw hit rate but introduces correctness
    // bugs (infinite Context>>copyTo: recursion at startup; with an
    // icData[2]==0 guard, downstream DNUs).  See docs/jit-ic-probe.md
    // for the full investigation.  PHARO_T1_IC_PROBE=1 enables for
    // experimentation.
    bool t1ICProbe = true;            // default-on since 2026-05-16 (opt-out PHARO_T1_NO_IC_PROBE)
    int  t1ICProbeMin = -1;           // PHARO_T1_IC_PROBE_MIN — only probe opcodes >= this
    int  t1ICProbeMax = -1;           // PHARO_T1_IC_PROBE_MAX — only probe opcodes <= this
    // Per-specialization opt-outs (default-on when probe enabled).
    // PHARO_T1_NO_INLINE_GETTER / SETTER / RETURNS_SELF to disable
    // individually — bisect tool for debugging path B regressions.
    bool t1InlineGetter = false;      // default OFF 2026-05-28 (correctness); PHARO_T1_INLINE_GETTER opts in
    bool t1InlineSetter = true;       // PHARO_T1_NO_INLINE_SETTER inverts
    bool t1InlineReturnsSelf = true;  // PHARO_T1_NO_INLINE_RETURNS_SELF inverts
    // The six dispatch-A-only "extra" inline specs (TempReturn/IntCmpReturn/
    // IntArithReturn/EvenOdd/MultiSlot/ReturnsLiteral, IC bits 51-58) moved to
    // debug_vars.h as default-off opt-in knobs (PHARO_T1_INLINE_*) read via
    // GET_DEBUG_BOOL — unvalidated + buggy (tryMultiSlot's wild #extent receiver).
    // jit-may23d W7: shortcut for #even/#odd in Sista's call_send
    // helper.  PHARO_SISTA_NO_SHORTCUT_EVEN_ODD=1 disables.
    // Inline-J2J emit (self-recursive case): tail-call callee's JIT entry
    // directly instead of round-tripping through the chain loop.  12× win
    // on benchFib.  Default-on 2026-05-17, default-OFF 2026-05-20 after
    // discovering wrong-result bug, default-ON again 2026-05-21 with
    // pure-J2J gate (AsmjitT1.cpp ~3343) that runtime-checks ALL of
    // caller's IC sites have bit 60 before allowing inline-J2J to fire.
    // PHARO_T1_NO_INLINE_J2J=1 to disable.  See deferred.md A6 N+30k.
    // t1InlineJ2J REMOVED — knob moved to debug_vars.h (PHARO_T1_NO_INLINE_J2J).
    // Pure-J2J gate (AsmjitT1 inline-J2J self-rec emit): runtime-loop
    // that bails inline-J2J unless ALL of caller's IC sites have
    // J2J_ENTRY_BIT.  Shipped default-ON in A6 N+30k as a safety net
    // for a materialize-bail wrong-result bug — without it ~20% of
    // benchFib(17) calls return 5133/5067 instead of 5167 (jit-may20
    // Step 2 investigation, 2026-05-21).  Now superseded by t1WarmJ2JGate
    // (warmth check) — kept as a strict fallback under PHARO_T1_PURE_J2J_GATE=1.
    // Warm-J2J gate (jit-may20 Step 2, 2026-05-21): cheaper, more
    // permissive runtime gate.  Iterates caller's IC sites and bails
    // only if any site's entry0 *key* is zero (= site cold).  Catches
    // the same materialize-bail wrong-result bug that t1PureJ2JGate
    // guards (the bug needs a cold IC mid-flight in the inlined chain),
    // but stays passable for warm prim-only sites that t1PureJ2JGate
    // would bail on for lacking bit 60.  Default-ON.
    // PHARO_T1_NO_WARM_J2J_GATE=1 disables for A-B testing.
    // Warm-J2J gate: iterate caller's IC entries at IC HIT and bail
    // inline-J2J self-rec emit if ANY site has key==0 (cold).  Added
    // 2026-05-20 (commit 6a550da8) as a safety net for a materialize-
    // bail wrong-result bug that made fib(N) return fib(N-2) for
    // N>=17.  Empirically bailed ~96% of fib's IC hits — defeating
    // inline-J2J entirely.  Repeated correctness check 2026-05-26
    // (50/50 runs of fib(17..35) all return correct values with gate
    // OFF) shows the underlying bug was fixed by intervening JIT
    // commits (likely 2d9da6ee state-sync work + dde2903b null-check
    // + 38bf9887 splice-driven pass-3 fix).  Flipped to OFF: closes
    // ~6x of the Cog gap on fib (95ms → 14ms).
    // Opt-in: PHARO_T1_WARM_J2J_GATE=1 to re-enable.
    // jit-may20b Step 6.1: per-caller histogram of inline-J2J gate pass/bail.
    // When set, the gate bail and gate pass sites emit a `bl` into
    // jit_rt_bail_gate_log(callerJM, kind).  The helper maintains
    // unordered_maps keyed on JITMethod*, dumped at VM exit alongside the
    // existing inline-J2J counters.  Default OFF (adds save/restore around
    // the gate exit paths — diagnostic only).
    // PHARO_T1_BAIL_GATE_HISTO=1.
    bool t1BailGateHisto = false;
    // jit-may20b Step 6.2: one-shot per-method dump of icBuffer state at
    // the first gate bail.  Implies t1BailGateHisto.  Prints all
    // numICEntries × IC_ENTRIES_PER_SITE entries of the bailing caller
    // method.  PHARO_T1_BAIL_GATE_TRACE=1.
    // Inline SmI bitwise prims (bitAnd:/bitOr:/bitXor:) at the IC HIT site
    // for nArgs==1 sends.  Saves the chain-loop round-trip for SmI bitwise
    // sends via named-send (bitXor: is not a special-selector bytecode so
    // every send goes through IC).  primKind dispatch added 2026-05-18 for
    // stringHash/dict benches.  PHARO_T1_NO_INLINE_PRIM_BITOPS=1 opt-out.
    bool t1InlinePrimBitOps = true;
    // Inline at: (primKind 14) at IC HIT site for heap receivers with
    // fmt-2 (Array).  Skips chain-loop round-trip for `arr at: i`-style
    // sends.  Mirrors stencils.cpp:1538-1545.  ~30 cycle inline emit
    // vs ~500 cycle chain-loop round-trip.  Could benefit sort/dict/sum.
    // Default-on; PHARO_T1_NO_INLINE_PRIM_AT=1 opt-out.
    bool t1InlinePrimAt = true;
    // jit-may22b Step 4: walk IC slots 0-2 in the inline probe instead
    // of bailing on slot-0 miss.  In theory catches ~12K cold-start
    // polymorphic DUPs.  In practice 1-3% slowdown across most benches
    // because the extra 4 instructions on slot-0-hit's fall-through
    // outweigh the rare polymorphic-hit savings.  Default-OFF until
    // a workload that actually benefits is found.
    // PHARO_T1_IC_POLY_WALK=1 opt-in.
    // jit-may20b Step 10: inline-prim 18 (basicNew:) at the IC HIT site
    // for nArgs == 1 sends.  Routes to a runtime helper that calls
    // primitiveNewWithArg directly from the JIT, skipping the
    // chain-loop's exit→re-entry round-trip.  Targets the 100K
    // allocations bench gap (12 ms ours vs 3 ms Cog).
    // Default-on; PHARO_T1_NO_INLINE_PRIM_BASIC_NEW=1 opt-out.
    bool t1InlinePrimBasicNew = true;
    // jit-may20b Step 8.4: dispatch to Sista's compiled fn from asmjit-T1
    // IC HIT path when the IC patcher set SISTA_BIT (bit 55) + fn ptr in
    // bits 47:0.  Currently dormant — Sista's bail-protocol bug (Step 4)
    // means Sista doesn't compile send-having methods like benchFib, so
    // SISTA_BIT is never set in practice.  Infrastructure in place for
    // future Step 4 follow-up.  Default-off (until Step 4 lands).
    // PHARO_T1_INLINE_SISTA_CALL=1 opt-in.
    // Cross-method inline-J2J emit (callee != caller).  Default OFF —
    // currently corrupts state at MAX>5000 fires; opt-in for lldb work.
    // PHARO_T1_INLINE_J2J_XMETHOD=1 enables.
    bool t1InlineJ2JXmethod = false;
    // Bisect cap on number of cross-method fires.  Default 30000 —
    // empirically the safe boundary on `42 printString` corruption
    // bisection (deferred A6 iter N+9/N+10).  Above this, state
    // corruption resurfaces.  PHARO_T1_INLINE_J2J_XMETHOD_MAX=N
    // overrides; set very high for default-on attempts after the
    // chain-break protocol fix lands.
    int  t1InlineJ2JXmethodMax = 30000;
    // Dump xmethod trace ring buffer at terminateCurrentProcess.
    // PHARO_T1_XMETHOD_LOG=1 enables.  Useful when chain-break corruption
    // surfaces as a P process termination — the last 64 fires are
    // captured in g_xmethod_trace.
    // Opt-in: allow block-value inline for non-leaf blocks (those with
    // inner sends).  PHARO_T1_INLINE_BLOCK_VALUE_NONLEAF=1 enables.
    // See JITRuntime.cpp jit_rt_inline_block_value_prep — leaf-only
    // gate was added 2026-05-19 (iter N+16) to suppress chain-break
    // corruption.  Re-attempt after the later fixes.
    // F3-NL3 bisection cap: max non-leaf inline-block-value fires.
    // PHARO_T1_INLINE_BLOCK_VALUE_MAX=N.  Default -1 = uncapped.
    int  t1InlineBlockValueMax = -1;
    // F3-NL4: max numICEntries for non-leaf BV inline.  PHARO_T1_BV_MAX_IC=N.
    // -1 = no cap.  Tunes the non-leaf threshold.
    int  t1BvMaxIC = -1;
    // F3-NL5: max j2jDepth for non-leaf BV inline.  PHARO_T1_BV_MAX_DEPTH=N.
    // -1 = no cap.  Bisects nested-BV overhead.
    int  t1BvMaxDepth = -1;
    // Per-primitive call counter dumped at exit.  PHARO_PRIM_PROFILE=1.
    // Compile every block on first invocation (instead of waiting for
    // hot-count threshold).  PHARO_T1_EAGER_BLOCK_COMPILE=1 — diagnostic.
    // Sync interp->receiver_ and method_ on chain-loop J2J Return path.
    // Without this, a stale callee-receiver lingers in interp->receiver_
    // after chain-loop bail/resume and can corrupt later interp dispatch.
    // PHARO_T1_J2J_RECEIVER_SYNC=1 enables.  See deferred.md A6 iter N+7.
    // Store post-send IP (state.method + bcOffsetFromMethObj + 1) in
    // inline-J2J save.ip instead of raw state.ip.  Mirrors chain-loop's
    // J2JCall handler which advances state.ip past the send before
    // saving.  Found via lldb that pre-send save.ip causes interp to
    // re-execute the send when materialized.
    //
    // Default-on (2026-05-26): measured fib(28) 9 ms → 8 ms (-10%),
    // correctness verified on fib(17..40), sieve, even, factorial,
    // sista_survey.  Opt-out: PHARO_T1_NO_J2J_POST_SEND_IP=1.
    bool t1J2JPostSendIp = true;                   // PHARO_T1_NO_J2J_POST_SEND_IP
    // jit-may24 Step 1: suppress JIT re-activation inside materialize-bail
    // unwind.  When ANY of the top 4 SavedFrames has materializedRetSlot
    // set, canJITActivate() returns false to prevent nested bails.
    // Default-on safety net.  PHARO_T1_ALLOW_NESTED_JIT_BAIL=1 opts out.
    bool t1NlrTailOnly = false;       // PHARO_T1_NLR_TAIL_ONLY (loosen hasNLR detection)
    // SIGPROF sampling profiler (PHARO_PROFILE=1).  Samples
    // interpreter's activeMethod() every PHARO_PROFILE_INTERVAL_US
    // microseconds of CPU time (default 1000 = 1ms); dumps top-N
    // by sample count at process exit (PHARO_PROFILE_TOP=N, default 30).
    int  profileIntervalUs = 1000;
    int  profileTopN = 30;
    // Interp-side IC table for Sista hint extraction on interp-only
    // methods.  Default-on 2026-05-25 (Session H follow-up) after
    // bench-suite A/B confirmed Session E's lift dispatch is reliable:
    // 1M getter+yourself drops 33 ms → 1 ms (closes the Cog gap).
    // Total bench-suite delta ~-22 ms; mild slowdowns on sort (+6 %)
    // and fib (+3 %) are outweighed by the getter win.
    // PHARO_NO_SISTA_INTERP_HINTS=1 reverts.
    // See docs/sista-ic-promotion-plan.md and deferred.md §A6 iter N+32.
    bool sistaInterpHints = true;
    // Split-pool layout: JIT inline-J2J pushes saves to a separate slice
    // of j2jPool_, distinct from the chain-loop's rj2jSaves slice.  Avoids
    // collision when both chain loop and JIT push during a single tryJIT-
    // Activation call.  PHARO_T1_J2J_SPLIT_POOL=1.  See deferred.md A6
    // iter N+2/N+3.
    // Inline FullBlockClosure>>value/value:/value:value:... at the IC HIT
    // path when BLOCK_VALUE_BIT (bit 59 of extras) is set.  Looks up the
    // compiledBlock's JM via jit_rt_inline_block_value_lookup, then
    // pushes a J2J save + br to the block's entry.  Saves the ~500-cycle
    // chain-loop round-trip per block invocation.  Opt-in (same risk
    // profile as cross-method inline-J2J — uses the same save/return
    // protocol).  PHARO_T1_INLINE_BLOCK_VALUE=1.
    // Diagnostic: force probe to always-miss so the probe COMPUTATION runs
    // but exits via ExitSend regardless of icData[0] match.  Isolates
    // whether bugs are in the probe arithmetic or in the HIT path.
    // Probe HIT path exits via EXIT_SEND instead of EXIT_SEND_CACHED.
    // Diagnostic: isolates whether the bug is in cached-method dispatch
    // or in the probe arithmetic.  Equivalent to always-miss EXCEPT
    // the state setup runs the full HIT branch.
    // Diagnostic: verify cached method's selector matches the IC site's
    // selector AND receiver class matches the IC key on each HIT.  Logs
    // mismatches.  Off by default.
    // PHARO_SORTSTR_WATCH=1: install a slot watcher on sortStructs:into:'s
    // temp 3 (fp+4) right after PopStoreTemp 3 writes it.  Any subsequent
    // change to that slot logs (via checkSortstrWatch()) — pinpoints the
    // dispatched method that corrupts the caller frame.
    // PHARO_DRIFT_CHECK=1: after every joint method_/instructionPointer_
    // update, verify IP is within method_'s bytecode area.  Off by default.
    // PHARO_T1_RESUME_TOS_LOG=1: log state.sp[-1] (what the JIT resume
    // entry will read as TOS) right before JIT_CALL into the precomputed
    // resume.  Pinpoints whether the cached-dispatch's retVal write
    // survives to the resume point.  Off by default.
    // Conditional-jump emit — DEFAULT-OFF.  Two reasons:
    //
    // 1. Correctness flake: in-place real-emit of cond-jump methods
    //    intermittently corrupts downstream callers (cull: is the
    //    canonical case).  Reproduced 2026-05-16: with
    //    PHARO_ASMJIT_T1_ENABLE_JUMPS=1 + PHARO_RECOMPILE_AT=999999
    //    (recompile disabled), fuzzer is 39/39 PASS x3, but with
    //    recompile on (default), fuzzer flakes 5/39 → 39/39.
    //    Disabling recompile globally for T1 is a small perf
    //    regression (sieve doesn't recompile so doesn't matter for
    //    sieve, but other methods regress 15-20% via lost JIT entries).
    //
    // 2. Sieve perf regression: even when fuzzer-stable, real-emit
    //    of cond-jump methods is SLOWER than bail-to-interp on
    //    practical benchmarks — sieve 194ms (stub-on-cond-jump) →
    //    820ms (real emit).  The cond-jump emit works correctly
    //    but doesn't pay off vs the optimized interp path.
    //
    // Default-on 2026-05-17 — short cond jumps + 0xED/0xEE/0xEF long jumps.
    // PHARO_ASMJIT_T1_NO_JUMPS=1 to opt out.  Original opt-in flag
    // PHARO_ASMJIT_T1_ENABLE_JUMPS=1 stays as a no-op alias.
    bool t1EnableJumps = true;
    int  t1JumpsFirstN = -1;          // PHARO_ASMJIT_T1_JUMPS_FIRST_N
    int  t1JumpsOnlyN  = -1;          // PHARO_ASMJIT_T1_JUMPS_ONLY_N
    int  t1JumpsSkipN  = -1;          // PHARO_ASMJIT_T1_JUMPS_SKIP_N
    int  t1JumpsSkipFrom = -1;        // PHARO_ASMJIT_T1_JUMPS_SKIP_FROM
    int  t1JumpsSkipTo   = -1;        // PHARO_ASMJIT_T1_JUMPS_SKIP_TO
    int  t1MaxSendNArgs  = 99;        // PHARO_ASMJIT_T1_MAX_SEND_NARGS

    // Block-compile bisect.
    int  t1BlocksFirstN = -1;         // PHARO_T1_BLOCKS_FIRST_N
    int  t1BlocksOnlyN = -1;          // PHARO_T1_BLOCKS_ONLY_N
    int  t1BlocksSkipFrom = -1;       // PHARO_T1_BLOCKS_SKIP_FROM
    int  t1BlocksSkipTo = -1;         // PHARO_T1_BLOCKS_SKIP_TO
    // Method-dump for objdump inspection.
    const char* t1DumpSel = nullptr;  // PHARO_T1_DUMP_SEL (selector to dump)
    const char* t1SkipSelectors = nullptr; // PHARO_T1_SKIP_SELECTORS — CSV of selectors to reject from real-emit
    // Trace JITRuntime::tryExecute calls for matching selectors.  Helped
    // identify pushLiteral: as the #138 cond-jump bug trigger in May 2026
    // by surfacing receiver class + slot count at each JIT entry.
    const char* traceExecSels = nullptr; // PHARO_TRACE_EXEC_SELS — CSV of selectors to trace
    // Skip JIT re-entry after a method return (returns to interp dispatch).
    // Skip auto-start of the heartbeat thread.

    // --- JIT debug/trace ---
    bool jitSpillWarn = false;        // PHARO_JIT_SPILL_WARN (presence)
    bool jitArithOflowTrace = false;  // JIT_ARITH_OFLOW_TRACE (presence)
    bool jitSimStack = false;         // PHARO_JIT_SIMSTACK (force on, strict =1)
    bool jitNoSimStack = false;       // PHARO_JIT_NO_SIMSTACK (force off, strict =1)

    // --- Tier 2 / asmjit ---
    bool t2MbcJumpsEnabled = true;    // PHARO_T2_MBC_JUMPS=0 disables, else enables

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
    int recompileAt = 500;            // PHARO_RECOMPILE_AT — recompile threshold

    // --- IC specialization opt-outs (default-on; set to disable) ---
    // MONOJ2J default-on attempt #2 (2026-04-27).  First attempt
    // (2026-04-26) crashed on long fib with "JIT method numIC=0" —
    // root cause was the IC-offset bug in applyICSpecialization
    // (fixed 17c8241).  Post-fix re-test: clean 60s bench, fib(30),
    // fib(32), no SIGSEGV.  Default ON; PHARO_NO_MONOJ2J_SPEC=1 to
    // bisect.  ~10% across-the-board win.
    bool monoJ2JSpec = true;          // default-on (post 17c8241)

    // --- String-valued.  nullptr if env var unset or empty. ---
    const char* benchType = nullptr;               // PHARO_BENCH (value)
    const char* awfyOnly = nullptr;                // PHARO_AWFY_ONLY
    const char* jitExclude = nullptr;              // JIT_EXCLUDE
    const char* jitExcludeOop = nullptr;           // JIT_EXCLUDE_OOP
    const char* jitSkipSelectors = nullptr;        // PHARO_JIT_SKIP_SELECTORS
    const char* jitNoSimStackSelectors = nullptr;  // PHARO_JIT_NO_SIMSTACK_SELECTORS
    const char* j2jSkipSelectors = nullptr;        // PHARO_J2J_SKIP_SELECTORS
    const char* j2jOnlySelectors = nullptr;        // PHARO_J2J_ONLY_SELECTORS (inverse skip: fill J2J bit ONLY for these selectors)
    const char* jitDumpBCPre = nullptr;            // JIT_DUMP_BC_PRE
    const char* jitDumpBCPost = nullptr;           // JIT_DUMP_BC_POST
    const char* jitDumpHex = nullptr;              // JIT_DUMP_HEX
    const char* jitDumpSel = nullptr;              // PHARO_JIT_DUMP_SEL
    const char* b5Focus = nullptr;                 // PHARO_B5_FOCUS
    const char* jitTraceOop = nullptr;             // PHARO_JIT_TRACE_OOP
    const char* jitTop = nullptr;                  // PHARO_JIT_TOP
    // Sista tier-up dispatch: on by default (Phase 2.3 MVP ships).
    // Set PHARO_NO_SISTA=1 to opt out; PHARO_SISTA_DISPATCH=1 remains
    // a no-op that also enables it (backward-compatible).
    bool sistaDispatch = true;                     // PHARO_SISTA_DISPATCH / opt-out PHARO_NO_SISTA

    // Generational GC (eden + scavenge).  Default ON (2026-04-24)
    // after class-table identity-hash collision fix.  Bench wins
    // 2-9x on allocation-heavy workloads (factorial beats Cog).
    // Set PHARO_NO_YG=1 to opt out; PHARO_YOUNG_GEN=1 forces ON.
    bool youngGenEnabled = true;                   // PHARO_YOUNG_GEN / opt-out PHARO_NO_YG
    // Skip the per-safe-point scavenge trigger.  Pre-compact scavenge
    // still runs inside fullGC.  Useful for isolating scavenge
    // cadence from correctness.

    // ---------------------------------------------------------------
    // Trace / debug bools migrated from scattered std::getenv() calls.
    // All envPresent semantics unless noted otherwise.
    // ---------------------------------------------------------------
    int  detectErrorsLimit = 30;                   // PHARO_DETECT_ERRORS_LIMIT
    const char* dumpRecompileIC = nullptr;         // PHARO_DUMP_RECOMPILE_IC (substring filter)
    bool jitKeepICs = false;                       // PHARO_JIT_KEEP_ICS (strict =1)
    bool jitStaleLog = false;                      // PHARO_JIT_STALE_LOG (strict =1)
    bool socketDebug = false;                      // SOCK_DEBUG (strict =1): trace the socket accept/listen/data path
    const char* noGetterBitBisect = nullptr;       // PHARO_NO_GETTER_BIT_BISECT
    bool noSistaCollect = false;                   // PHARO_NO_SISTA_COLLECT
    bool noSistaDoSplice = false;                  // PHARO_NO_SISTA_DO_SPLICE
    bool sistaDoSpliceNoHint = false;              // PHARO_SISTA_DO_SPLICE_NO_HINT (strict =1)
    bool noSistaHelperSends = false;               // PHARO_NO_SISTA_HELPER_SENDS (presence)
    bool noSistaHelperSendsStrict = false;         // PHARO_NO_SISTA_HELPER_SENDS (strict =1)
    const char* sistaExcludeSels = nullptr;        // PHARO_SISTA_EXCLUDE_SELS
    int  sistaHelperMaxDepth = 64;                 // PHARO_SISTA_HELPER_MAX_DEPTH
    bool sistaInlinePoly = true;                   // default-on 2026-05-26; PHARO_NO_SISTA_INLINE_POLY=1 reverts
    // NB: per-op x86 Sista disable knobs (AT/SIZE/ATPUT/FLOAT/COUNTED_LOOP)
    // live in debug_vars.h (the single-source X-macro system), not here.
    int  sistaT1Warmup = 100;                      // PHARO_SISTA_T1_WARMUP
    // Self-recursive splice: inline a method's own body at its
    // recursive call sites at Sista compile time.
    //
    // Default-OFF (2026-05-26).  Initially flipped ON earlier today
    // to close the fib(28) Cog gap (-29%), but after the warm-J2J
    // gate flip (same session) the inline-J2J self-rec emit fires
    // at every IC hit and benchFib runs at 9ms (vs 14ms with splice
    // ON, vs 30ms Cog).  The splice adds per-call Sista compile +
    // dispatch overhead that the inline-J2J path doesn't pay.
    //
    // Keep the code in tree gated to opt-in (PHARO_T1_SELF_REC_SPLICE=1)
    // because the splice IR is the foundation for future inline-tail-
    // call lowering — when kSendInlineSelf gets a direct-call emit
    // path, splicing the body becomes a profitable transformation.
    // Until then, the inline-J2J path is faster.
    // Number of nested self-recursive splice levels allowed.
    // 1 = single-level (inline one recursive call); 2 = nested
    // (inline a call that itself contains an inlined call).
    // Code size grows ~2^depth per call site.  Default 1 — higher
    // values regressed fib(28) at depth >= 3 in measurement.
    int t1SelfRecSpliceDepth = 1;                  // PHARO_T1_SELF_REC_SPLICE_DEPTH
    // Bypass the sendNoSplice negative-cache gate for methods that
    // emitted a multi-block splice.  Paired with t1SelfRecSplice;
    // both default-off (see t1SelfRecSplice comment).
    bool t2Strict = false;                         // PHARO_T2 strict =1 form (JITRuntime)
    // String-valued env vars for OS introspection primitives.
    const char* envHome = nullptr;                 // HOME
    const char* envUser = nullptr;                 // USER
    const char* envLogname = nullptr;              // LOGNAME
    const char* envLang = nullptr;                 // LANG
    const char* envLcAll = nullptr;                // LC_ALL
    const char* envTmpdir = nullptr;               // TMPDIR

    // Cog-spec finalization signaling + native C++ mourn drain.
    // Drain fires at method activation (not primitive calls), which
    // matches Cog's stack-limit-trick triggering behavior: Cog's
    // `forceInterruptCheck` sets stackLimit = -1 in fireEphemeron,
    // then the next *real* method activation's stack-overflow check
    // fires and drains via checkForEvents.  Quick primitives (slot
    // accessors like dict.size) return pre-drain values because they
    // never call activateMethod, exactly matching what
    // testClearing expects at assertion B ("Keys are gone but not
    // yet finalized" — tally still 1001 at the time of the read).
    // Default-on since testClearing + testFinalize both pass in
    // stock Cog with this semantic; set PHARO_INLINE_FINALIZE=1 to
    // restore legacy for bisection.
    bool finalizeDeferred = true;                  // PHARO_INLINE_FINALIZE inverts

    // PHARO_SCAV_BIT_AUDIT=1 — verify that the RememberedBit (set by
    // the JIT inline-asm write barrier and storePointer's C++ barrier)
    // is reliably populated.  Scavenge still does its full old-space
    // scan; this mode adds bookkeeping that counts (a) objects with
    // young refs that had the bit set, (b) objects with young refs
    // that had the bit UNSET (audit-gap misses), (c) objects with the
    // bit set but no actual young refs (false positives, harmless).
    // Reported in JIT stats summary.  Low overhead, default off.

    // PHARO_T1_SETTER_BARRIER=1 — opt in to emitting an old→young
    // write-barrier call from the JIT T1 inline setter (AsmjitT1.cpp
    // line 4431 arm64).  Default off: the setter currently writes
    // recv->slot[i] = arg inline without the barrier, relying on
    // scavenge's O(oldSpace) full scan to find missed old→young
    // pointers.  When on, the setter emits a BLR to
    // jit_rt_setter_write_barrier after the inline store.  Audit-gap
    // closer — see memory/jit_remembered_set_dead.md.

    // PHARO_GC_HEADROOM_MB — override ObjectMemory's default 512 MB
    // threshold for the next-fullGC point.  Higher values reduce GC
    // count during allocation-heavy benchmarks; physical-memory cost
    // grows with peak live heap (the 4 GB old-space region is mmap'd
    // lazy-commit, so the headroom only affects WHEN GC fires, not
    // baseline RSS).  0 = use the default.  See docs/WIP.md for
    // the perf-vs-headroom table.
    int  gcHeadroomMB = 0;

    // The constructor reads every env var listed above.  C++ guarantees
    // static-storage-duration objects are initialized before main(), and
    // by that time the process environment is fully populated, so this
    // is always safe.
    DebugSettings();

    // Re-read env vars.  Call after any code that mutates the environment
    // via setenv(), e.g., test_load_image auto-setting PHARO_NO_JIT.
    // Equivalent to constructing a fresh DebugSettings and copying over.
    void reload();

    // Runtime accessor for the SmallTalk-callable `OSEnvironment getenv:`
    // primitive in Primitives.cpp.  Cannot be cached because the variable
    // name is a runtime argument.  All other VM code MUST go through
    // g_debug.<field> instead.
    static const char* envRuntime(const char* name);
};

extern DebugSettings g_debug;

}  // namespace pharo

#endif  // PHARO_DEBUG_SETTINGS_HPP
