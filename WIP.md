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
  -> 4/4 runs lose the eval silently (clean exit, no DNU, no EVAL-RESULT);
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
- **First-compile fail thrash (suite-scale):** SUnit batch shows 698K
  failed compile attempts vs 5.8K successes — initialCompileFailed_
  (the first-compile negative cache, JITRuntime.cpp ~4045) is CLEARED on
  every recoverAfterGC (i.e. every scavenge), so each failing method
  re-runs the asmjit pipeline once per GC.  Same family as the fixed
  kRecompileFailed thrash (memory jit-recompile-fail-thrash).  Fix
  sketch: make it a key-array visited by forEachRoot (like countMap_)
  + rehash in recoverAfterGC, instead of clear; or remap through the
  scavenge forwarding table.  Measure cost share first (failed-compile
  bail reasons breakdown: compilationsFailed_ bump sites
  JITCompiler.cpp 1542-1701).

## Workflow artifacts (multi-agent investigation maps, this session)

- /private/tmp/claude-501/-Users-wohl-src-iospharo/a6ed987d-559a-4f18-a43d-7bf2905adc05/tasks/wqrxey8av.output
  (send-path map: bit consumers, compile triggers, ExitSendCached flow, gates)
- .../tasks/wn7p9ib48.output (save-stack lifecycle: GC, recompile/eviction,
  protocol diff — contains the remaining suspect list w/ likelihoods+tests:
  matRetSlot-ignoring ExitReturn pops at ~22693 & jit_loop_exit, stale
  bcStartCache after GC, recoverAfterGC ghost-version methodMap rebuild)

## Rules reminders
- EVAL-RESULT prints to stderr — capture with 2>&1.
- grep -o "ms=..." matches the arg echo first; grep EVAL-RESULT first.
- New env knobs -> src/vm/debug_vars.h (DEBUG_BOOL + GET_DEBUG_BOOL).
- JITMethod field reads in emit code: offsetof() ONLY, never raw ints.
