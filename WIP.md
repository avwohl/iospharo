# WIP — JIT Cog-speed: cross-method inline-J2J unblocked (2026-06-09 session 2)

Goal (active /goal): **fix this jit to work and be as fast as cog.**
Branch: `jit`. Build: `cmake --build build-opt` (optimized; the plain `build/` is -O0).
Test VM: `./build-opt/test_load_image /tmp/harness/Pharo.image eval "<expr>"`.
Stock Cog baseline: `cd /tmp/harness && ./pharo Pharo.image eval "<expr>"`.

## Headline: cfib(30) 344ms -> 41-43ms (Cog: 8ms). 43x gap -> ~5x.

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
  **CULPRIT PINNED (2026-06-10):** deterministic bisect on
  PHARO_T1_INLINE_J2J_XMETHOD_MAX (pass at 26704, fail at 26705) +
  lldb watchpoint on g_xgate_bail_cap in the PASS run -> fire #26705 is
  caller=#handle:offset: (CM 0x300976370) callee=#initializeHandle:offset:
  (CM 0x300976530, bcLen=10, 2 args, >=1 IC site) — FFI ExternalAddress
  family.  RULED OUT at the fire (lldb raw-memory checks): tempCount setup (the
  xmethod path reads callee JM[35] dynamically; offsets 34/35 verified
  vs offsetof) and stale bcStartCache (JM[104] == compiledMethodOop+40,
  consistent -> CM had not moved by fire #26705).  NEXT: single-step
  the callee's FIRST mid-method C++ exit (its send sites are cold ->
  ExitSendCached) and its return path — the corruption mechanism lives
  in that exit/materialize/resume sequence, since leaf callees (which
  never exit mid-method) are immune.  Callee initializeHandle:offset:
  stores into receiver ivars (write-barrier path) — check the
  popStoreRecvVar interaction with a J2J-entered frame too.
  ALSO FOUND: PHARO_T1_INLINE_J2J_XMETHOD_LOG's emit wrapper does NOT
  save x1-x6 across its blr -> the LOGGER ITSELF corrupts the receiver
  (instant crash in callee #header) — likely the origin of the
  historical "xmethod corrupts state, lldb-only" lore.  Fix the wrapper
  before trusting any XMETHOD_LOG run.
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
- Self-rec saveless path (PHARO_T1_CAN_SKIP_J2J_SAVE) exists but is
  self-rec-only; cross-method saveless for canSkipJ2JSave callees (incc
  qualifies) would shave the save-push cost — needs the state-update
  extension noted in the Eδ.2c comment.
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
- EVAL-RESULT prints to stderr — capture with 2>&1.
- grep -o "ms=..." matches the arg echo first; grep EVAL-RESULT first.
- New env knobs -> src/vm/debug_vars.h (DEBUG_BOOL + GET_DEBUG_BOOL).
- JITMethod field reads in emit code: offsetof() ONLY, never raw ints.
