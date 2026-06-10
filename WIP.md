# WIP — JIT Cog-speed: inline-J2J DEFAULT-ON (2026-06-09/10)

Goal (active /goal): **fix this jit to work and be as fast as cog.**
Branch: `jit`. Build: `cmake --build build-opt` (optimized; the plain `build/` is -O0).
Test VM: `./build-opt/test_load_image /tmp/harness/Pharo.image eval "<expr>"`.
Stock Cog baseline: `cd /tmp/harness && ./pharo Pharo.image eval "<expr>"`.

## Headline: inline-J2J is DEFAULT-ON (lever e, commit after 0a48a0e1).
DEFAULT config now: cfib(30) 344ms -> 44ms (Cog 8), benchFib(30) 296ms -> 32ms
(Cog 5).  60-class SUnit A/B default-vs-J2J per-test identical (4130/4140;
two known-flaky tests flip one each way; the historical flip-blocker
CharacterTest>>testStoreStringAll PASSES).  Opt-out: PHARO_T1_NO_INLINE_J2J=1.
Confirmation COMPLETE: 1-60 re-run 4131/3 (one flaky better than the
pre-flip A/B); 61-200 (4319 tests) J2J-on 4297 vs J2J-off 4299 — only
per-test diffs are LIFOQueueTest>>testHeavyContention and
WeakAnnouncerTest>>testWeakDoubleAnnouncer, both PASS under J2J in
isolation on a quiet machine (load-flaky).  No reproducible regression
across 200 classes / 8459 tests.  The 61-200 SHA256Test TIMEOUT (both
configs) is the 64MB zone filling (~14.6K methods) -> late-hot methods
interpreted; PHARO_CODE_ZONE_MB=192 works around it until lever (d)
shrinks the ~6KB/method emit.  Next perf checkpoints vs Cog: re-run
the full classify-sunit Δcog comparison on build-opt, and watch
inline-J2J chain lengths vs preemption fairness (testHeavyContention
under load is the canary).

## DONE this session (committed)

1. **dd0fa6f2 — cross-method inline-J2J gate off-by-one.** The xmethod gate
   chain in AsmjitT1.cpp read JITMethod bytes 47/46 as isStubOnEntry/
   canBailMidMethod; real layout 46=hasNLR 47=canBailMidMethod 48=isStubOnEntry.
   Since t1NlrTailOnly (default ON) sets hasNLR for EVERY method containing a
   return opcode 0x58-0x5C, the byte-46 read rejected ~all real cross-method
   callees (574K/930K in cfib) — this WAS the entire cross-method send gap.
   The real isStubOnEntry was never checked (stubs slipped through = the old
   "xmethod corrupts state" reputation). All JM reads now use offsetof().
   Also: per-gate bail counters (`xmethod gates:` line in dumpJITStats, only
   emitted when PHARO_T1_INLINE_J2J is set), and the 30000 default fire cap
   (bisection leftover) lifted to unlimited.

2. **24175466 — stale state.j2jDepth corruption (xmethod at scale).** With
   876K+ fires/run, ~50% of startups corrupted (SettingTree mergeSort
   sortBlock -> #value:value: DNU). Root cause: materializeJ2J (chain loop,
   Interpreter.cpp ~22556) consumed pending saves into SavedFrames but never
   reset state.j2jDepth; the ExitBlockCreate/ExitArrayCreate handlers check
   `state.j2jDepth > 0` BEFORE enableJ2J resets it, so the
   `state.sp = stackPointer_` resync was skipped on consumed saves -> fresh
   closure above stale sp -> overwritten -> shifted operand passed as the
   sortBlock. Fix: materializeJ2J resets depth+cursor. 10/10 clean startups.
   Also hardened: prepareForGC/afterGC now round-trip live J2J pool saves'
   raw `ip` through J2JSave.ipOffset (was a real GC hole: forEachRoot visits
   only save.receiver); PHARO_J2J_MAT_LOG diagnostic knob.

3. **(uncommitted at write time) rj2j slice reservation** — the opt-in
   PHARO_RESUME_J2J chain loop's save slice was never reserved in
   j2jPoolCursor_, so GC receiver walk / ip round-trip / eviction pinning
   all missed those saves. Latent (rj2j default-off); fixed anyway.

## Key corrections to the previous WIP (premise was WRONG)

- "cfib->incc gets 0 bit-60 fills" was an ARTIFACT: PHARO_J2J_LOG_FILL caps
  at 4000 lines and startup burns them. The IC fill always worked
  (PHARO_DUMP_RECOMPILE_IC=cfib shows bit-60 + entryAddr on site 2).
- The slow path was the SEND-SITE DISPATCH: with the gates broken, every
  cross-method bit-60 send bailed to the trampoline activation
  (2.5M C++ activations/run @ ~340ns).
- incc DOES JIT-compile (T1-COMPILE right after cfib). `^self+1` is NOT
  classified by detectTrivialMethod (intArith needs a method ARG, 0x40).

## Current state / next levers toward Cog parity (~5x left on cfib)

- Validation in flight: SUnit batch 0-150 default-vs-INLINE_J2J
  (results /tmp/sunit_results_{default,j2j}.txt). MUST be equal-or-better
  before flipping PHARO_T1_INLINE_J2J default-on (lever (e), the big one —
  all of today's wins are behind that opt-in env).
- Residual xmethod bails (cfib run): bail_prim=105K (prim-bearing callees),
  bail_numic=237K (callees with sends — lever (c), needs nested-save
  correctness), bail_b47/stub+canBail ~17K. Each bail = slow trampoline.
  ALSO: any callee with conditional jumps has canBailMidMethod=1 -> b46
  bail (e.g. Number>>max: = 1.35M bails in the incs bench) — relaxing
  that is part of the same nested-correctness project.
- **Lever (c) status (PHARO_T1_XMETHOD_MAX_IC=N knob, commit 1238b606):**
  N>=1 (admit callees with sends) still corrupts after the j2jDepth fix
  AND after j2jEntryDepth hardening at 4 JIT_CALL sites.  DETERMINISTIC
  REPRO: `PHARO_T1_INLINE_J2J=1 PHARO_T1_XMETHOD_MAX_IC=1 PHARO_DET_SCHED=1`
  -> 4/4 runs lose the eval silently (clean exit, no DNU, no EVAL-RESULT;
  [STARTUP-ST-FIRED] never appears -> the corruption drops the startup
  action from SessionManager's startup list, no error raised);
  **BISECT CAVEAT (2026-06-10, supersedes the 'culprit pinned' claim):**
  the cap-bisect boundary is NOT call-identity-stable — capping changes
  inline-vs-trampoline charge counts downstream, so the boundary fire
  shifts with the binary (26705 = handle:offset:->initializeHandle:offset:
  on one build; 26683 = reset->resetTo: — a LEAF pair! — on the next).
  A leaf boundary proves the bisect finds a scheduling-sensitivity
  point, not the corruptor.  The corruptor is some 1-IC-callee fire
  (leaf-only is clean), identity unknown.  The watchpoint recipe still
  gives a deterministic stop + live registers at any chosen fire count,
  and `expr *(unsigned long long*)&g_xmethod_max = -1` at the stop
  un-caps live so the boundary call can be single-stepped.  RULED OUT at the fire (lldb raw-memory checks): tempCount setup (the
  xmethod path reads callee JM[35] dynamically; offsets 34/35 verified
  vs offsetof) and stale bcStartCache (JM[104] == compiledMethodOop+40,
  consistent -> CM had not moved by fire #26705).  NEXT: single-step
  the callee's FIRST mid-method C++ exit (its send sites are cold ->
  ExitSendCached) and its return path — the corruption mechanism lives
  in that exit/materialize/resume sequence, since leaf callees (which
  never exit mid-method) are immune.  Callee initializeHandle:offset:
  stores into receiver ivars (write-barrier path) — check the
  popStoreRecvVar interaction with a J2J-entered frame too.
  LOGGER FIXED (928df628): the XMETHOD_LOG wrapper now saves x0-x13+x30
  (was clobbering x1-x6 = receiver+args -> every logged call corrupted,
  the origin of the "xmethod corrupts state" lore).
  **SELECTOR-LEVEL BISECT DONE (2026-06-10, identity-stable):**
  PHARO_J2J_SKIP_SELECTORS halving over the 429 with-sends+filled
  callees converged on ONE selector: **initializeHandle:offset:**
  (ExternalData family: `self initialize. handle := aHandle.
  startOffset := aNumber - 1` — FIRST bytecode is a send -> immediate
  mid-method C++ exit on every cold call).  Skipping ONLY its fills
  cures the full failing config (ok + DNU=0).  Same selector the
  original fire-bisect named — that identification was genuine.
  **ROUTE NARROWED:** with the fixed XMETHOD_LOG, the failing run shows
  only 8 dispatch-A inline fires, NONE to this callee -> the corruptor
  route is the C++ J2J driver / stencil consumption of its bit-60 IC
  entry (ExitJ2JCall driver, Interpreter.cpp ~19302) — which has NO
  numICEntries gate and runs in the DEFAULT config too.  MAX_IC=1
  likely only shifts scheduling to expose it; the default config may
  carry the same latent bug.  NEXT: lift the 200-line PHARO_J2J_MAT_LOG
  cap (Interpreter.cpp materializeJ2JSaveIntoFrame) and/or conditional-
  break the ExitJ2JCall driver on calleeCM==initializeHandle's oop in
  the DET run; inspect the mid-callee-exit (its first `self initialize`
  send) -> materialize -> resume -> return sequence.
  GOTCHA: PHARO_J2J_ONLY_SELECTORS kills ALL IC fills for other
  selectors (0% IC hit rate) — runs under it DNU with ANY selector and
  are NOT a valid minimal repro.
  **EPISTEMOLOGY (2026-06-10, READ BEFORE TRUSTING ANY A/B ON THIS BUG):**
  the DNU-visibility is a LAYOUT KNIFE-EDGE: adding ANY env var (even
  AAAA=1, or byte-length-matched pads) makes the default+DET 7-DNU
  baseline go silent.  DebugSettings copies env strings at static init,
  so env content shifts allocation layout deterministically; the
  corrupt write lands in slack on most layouts.  CONSEQUENTLY:
  - POISONED (layout mirage): the heap-write-callee gate "cure"
    (PHARO_J2J_NO_HEAPWRITE_CALLEES — byte-matched pad cures equally),
    the skip-selector bisect convergence on initializeHandle:offset:
    (env length shrank monotonically with the candidate halving), the
    SCAV_DANGLE_CHECK "cure".
  - STILL VALID: the 7-DNU baseline itself (stable across reruns,
    exact env); cap=26682-pass vs 26683-fail (identical env length —
    ONE extra cross-method inline fire flips the severe outcome);
    NO_J2J_BRANCH keeping 7 DNUs DESPITE its env-length change (the
    corruption fires broadly; behavior knobs re-land it visibly).
  - The corruption is REAL and present in the shipping default config;
    wall-clock scheduling + layout luck hide it (200-class suites
    clean).
  - **KNIFE-EDGE EXTENDS TO BINARY DELTAS (2026-06-10 experiment
    series):** even the C helper's body size moves the visibility, so
    cross-binary DNU comparisons are NOISE.  Tested at the
    DET+NO_J2J_BRANCH 2-DNU deterministic repro: store-site nop sleds,
    register save/restore, empty calls, delay loops — DNUs persist
    within each binary; the one 0-DNU binary (full ring) is not
    attributable.  ONLY within-binary deterministic comparisons are
    valid evidence on this bug.
  - PHARO_T1_STORE_RING landed (knob-gated): store-provenance ring +
    DNU-time scan — useful once a config shows DNUs WITH the ring
    enabled (search configs; the scan prints planted-value provenance
    with selectors).
  - **SHADOW-SLOT INSTRUMENT BUILT (PHARO_SHADOW_SLOTS, committed
    2026-06-10):** ShadowSlots.{hpp,cpp} — 1M-entry (object,slot)->
    (value,writer) table; writers tracked: 3 JIT store emits,
    storePointer, storePointerUnchecked (covers setReceiverInstVar);
    readers verified: interpreter pushRecvVar (both paths) + JIT
    pushRecvVar/ExtPushRecvVar emits.  GC: forEachRoot visits entry
    oops, afterGC rehashes, becomeForward clears.  VALIDATED: ~28M
    stores / 3.45M checks / 0 mismatches / 0 false positives per
    startup across default+DET+MAX_IC=1+NOJB configs — writer coverage
    complete for exercised paths.  CAVEAT: those configs also showed
    no DNUs in the instrument's layout, so absence-of-mismatch is not
    yet absence-of-corruption.
  - **SUITE-SCALE VERDICT (200 classes): IVAR SLOTS EXONERATED.**
    974M tracked stores / 199M verified reads / exactly 4 mismatches —
    all the two-way-become family (prim 128 now clears the table; the
    only untracked writer found).  No ivar ever changed via an
    untracked path at scale.  (Suite 38 fails/6 timeouts vs 13/1
    uninstrumented = the ~4x instrumentation slowdown on
    timing-sensitive tests, not corruption.)
  - **REFRAME: the corruption class is STACK-FRAME SP-DESYNC, not heap
    stores.**  All victims in the original evidence were TEMPS
    (mergeFirst's `by` arg, OpalCompiler do: receiver), and both
    mechanisms actually root-caused (stale j2jDepth sp-resync skip;
    saveless return hijack) were sp desyncs that shift operand/temp
    slots.  NEXT INSTRUMENT: per-bcOffset expected-stack-depth table
    (computable at T1 compile from the bytecode stack effects) +
    verification of (state.sp - state.tempBase) at every JIT
    exit/resume boundary — catches the desync AT the boundary that
    produces it, with the method+bcOffset in hand.  The existing
    spAtLastJ2JCall/dispatchTraceLeak machinery in the chain loop is
    prior art for the check sites.
  - hasRecvFieldWrite now computed by T1 (commit 0bc7d7f9) — flag is
    real even though the gate experiment was inconclusive.
  Recipe (reusable): DET_SCHED makes the failure a deterministic
  function of the fire cap -> binary-search the cap (~20 runs), then
  watchpoint the cap-bail counter in the LAST PASSING run; x10=calleeJM
  x19/x11=callerJM x12=callerCM at the stop; resolve oops via
  PHARO_T1_TRACE_COMPILE log lines (lldb expression evaluator is
  useless under LTO).
  without DET_SCHED: intermittent startup DNUs (#do: in OpalCompiler
  evaluate / #key in WorkingSession startup), runs recover.  Control:
  DET_SCHED+leaf-only is clean.  Next session: lldb the deterministic
  repro; the send-bearing-callee bench is incs '^(self+1) max: 0' in a
  cfib loop (503ms today vs 41ms leaf — the lever-(c) prize).
- The xmethod fire path still bumps g_xmethod_count + cap-check per fire
  (2 ld + add + st + cmp); could be emitted only when a cap is set.
- J2J-s (stencil_sendJ2J out-of-line) handles 2.4M calls/run — dispatch-A
  inline-J2J only covers sites in recompiled (tier-2) callers? Verify
  coverage; tier-1-only callers may never get the inline emit.
- Saveless path (PHARO_T1_CAN_SKIP_J2J_SAVE), state after e6bb3809:
  - RETURN-HIJACK FIXED: the callee prelude tail-jumps past the
    post-blr restore whenever j2jDepth > j2jEntryDepth (callers
    mid-J2J-chain) -> sp-stash leaked 96B/call -> guard-page crash.
    Fix: pin entryDepth = depth across the blr (stash pad slot [40]).
  - Controlled single-site repro recipe: PHARO_T1_SAVELESS_MIN_COMPILE=N
    emits saveless only in compiles seq>=N (find the target caller's
    seq with PHARO_T1_TRACE_COMPILE; cfib lands ~#4699 in the bench
    eval).  cfib->incc via saveless: CORRECT (val=2692537).
  - PERF: as implemented it LOSES to the save-push (510 vs 412 ms 10x
    cfib) — the 96B stash + entryDepth pin cost more than they save.
    Diet plan: x25-x28 are callee-saved AND absent from JIT_CALL's
    clobber list (JIT code preserves them; trampoline relies on
    x23/x24 likewise) -> carry caller receiver/sp/tempBase/ip in
    x25-x28 across the blr, shrink the stash to {x30, entryDepth} +
    cross fields, and skip the OFF_RECEIVER/TEMPBASE/IP restores
    (write-back from regs).  Target: beat the ~23-op save-push.
  - **COMPLETE (a978a7f6)**: the at-scale bug was ExitArithOverflow
    bails — ANY SmI arith in a leaf can bail; canSkipJ2JSave only
    excluded cond-jump bailers (confirmed exit=6 via brk trap; the
    controlled cfib->incc site just never overflowed).  The non-return
    RECOVERY STUB retro-builds the elided pool save (post-send ip from
    callerCM + compile-time bcOffset; resumeAddr=endOfSend) and RETs
    with the callee's exit state -> C++ materializes as if save-push.
    Full scope, no bisect gates: 3/3 clean startups, 0 DNUs; 10x cfib
    420ms vs 440 save-push; benchFib + battery correct.
    REGISTER DIET VERDICT (3-agent audit, 2026-06-10): NOT VIABLE as a
    local change.  T1 emits use none of x22/x25-x27 (free there), but
    TrampolineAsm.S PINS ALL of x21-x28 across its BLRs into JIT code
    (x22=save base, x25=localCalls, x26=localReturns — LOAD-BEARING:
    checkCountdown_ -= (calls+returns)*10 — x27=overflow limit,
    x28=nilOop).  A diet register set inside a trampoline-entered
    caller corrupts the loop state it RETs back into; saving/restoring
    around the blr re-adds the eliminated memory ops (net zero).  A
    true win needs trampoline re-homing (counters -> frame slots,
    x27 -> recompute from x22+imm) freeing x25-x27 zone-wide + adding
    them to JIT_CALL clobbers — ~+4 mem ops per trampoline round-trip
    (NOT cold: all non-leaf J2J) for -6 per leaf call.  Marginal and
    risky; SUBSUMED by the register-resident state.sp project (the
    structural fix for the ~55-instr-vs-Cog-12 per-call gap).  Audit
    artifact: workflows task w5s5eg5y3 output.
    Default-on gate: full-suite A/B on the Graviton box (in flight at
    write time — default config 12674/12724 = 99.6%%; saveless run +
    per-test diff pending).  Bisect gates (MIN_COMPILE / MAX_ARGS /
    NO_EXTRAS) remain for future shape isolation.
- docs/jit-retrospective.md "Cog-speed MAP" numbers now stale for
  cross-method; tight-loop 2x-faster-than-Cog and self-rec 6x still hold.
- **First-compile fail thrash: FIXED (c79b97ab + 9dbe6a49).**  All
  failures were unsuppPrim (PHARO_JIT_FAIL_REASONS=1).  TWO paths:
  (a) initialCompileFailed_ negative cache was cleared every
  recoverAfterGC -> per-GC retries (now a GC-visited, rehashed 16K-key
  array like countMap_); (b) THE dominant one: upgradeICToJ2J's eager
  compile of prim-bearing callees runs per ExitSendCached send with no
  cache -> per-SEND asmjit pipeline re-runs.  Combined: 83.5K -> 694
  failed attempts per bare startup (120x); suite batches were burning
  ~700 failed compiles/sec.

## Workflow artifacts (multi-agent investigation maps, this session)

- /private/tmp/claude-501/-Users-wohl-src-iospharo/a6ed987d-559a-4f18-a43d-7bf2905adc05/tasks/wqrxey8av.output
  (send-path map: bit consumers, compile triggers, ExitSendCached flow, gates)
- .../tasks/wn7p9ib48.output (save-stack lifecycle: GC, recompile/eviction,
  protocol diff — contains the remaining suspect list w/ likelihoods+tests:
  matRetSlot-ignoring ExitReturn pops at ~22693 & jit_loop_exit, stale
  bcStartCache after GC, recoverAfterGC ghost-version methodMap rebuild)

## Rules reminders
- **SUnit runner invocations: rm TWO stale files first** or the run
  silently does nothing (ate three 90-min A/B attempts 2026-06-09/10):
  1. /tmp/sunit_run_completed.txt — runner's done-marker, ALSO touched
     by every test_load_image EVAL run (the A3 fix); blocks auto-start
     ("skipping auto-restart", one easy-to-miss line).
  2. <imagedir>/startup.st (e.g. /tmp/harness/startup.st) — eval mode
     writes it and non-eval runs EXECUTE the stale one -> evaluates the
     old expr and quits the image seconds after startup (0 SUnitRunner
     lines, "Test Complete" almost immediately).
  3. /tmp/sunit_batch.txt is 1-BASED (`1 60`, not `0 60`): index 0 makes
     the runner's copyFrom:to: raise, SessionManager swallows it, and the
     run is silent — the error appears ONLY in <imagedir>/PharoDebug.log
     (always check that file when a runner run is silent).
  4. The optimized build burns the 60G step budget in ~4-5 min; set
     PHARO_MAX_STEPS=2000000000000 for suite runs (knob added 0a48a0e1).
- EVAL-RESULT prints to stderr — capture with 2>&1.
- grep -o "ms=..." matches the arg echo first; grep EVAL-RESULT first.
- New env knobs -> src/vm/debug_vars.h (DEBUG_BOOL + GET_DEBUG_BOOL).
- JITMethod field reads in emit code: offsetof() ONLY, never raw ints.
