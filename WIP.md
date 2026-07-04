# WIP — session snapshot before reboot (2026-07-03 late PM)

## UPDATE 2026-07-03 ~22:50 — #14 TRUE ROOT CAUSE FOUND + FIXED (testing)

The exactly-once hand-out (dcacc401) killed the re-read storm but the x4
wedge STILL reproduced (gauntlet3-6.log in /c/tmp/probe-img).  Two
root-cause iterations:

ITERATION A (aging back-append — WRONG for this wedge, but real hazard,
fix kept): handleForceYield's aging displacement back-appended the active
process.  Principled Cog-parity fix applied (see below) but gauntlet5
STILL wedged and gauntlet4/6 show ZERO [AGING] fires — aging was not the
displacer.

ITERATION B (the actual mechanism, gauntlet6.log pc=99-vs-101 proof):
- Same-thread callback return: primitiveCallbackReturn defers the longjmp;
  enterInterpreterFromCallback's nested loop then runs a FIXED 500-step
  cooldown "so Smalltalk finishes cleanup", then YANKS whatever process
  is active (materialize + putToSleep BACK-APPEND) and restores the
  C caller.
- The executor's cleanup is much longer than 500 steps: stackProtect
  critical:-release -> valueUnpreemptively ensure -> Process>>priority: ->
  interpriorityYield: `[p resume] fork. p suspend`.  The yank regularly
  lands INSIDE the fork->suspend window.  The resumer is forked at the
  executor's NEW priority (instvar updated before the dance) = same
  queue; back-append puts the yanked executor BEHIND its own resumer.
  Resumer runs first, its resume is spent on a ready process, the
  executor's later `suspend` parks it FOREVER while holding stackProtect
  (sem ..dd3c0/..bd470) -> P70 callback queue blocks on the mutex ->
  total idle wedge (only the 1s Delay tick runs).
- Trace signature: healthy executors resume at interpriorityYield: pc=101
  (post-suspend); the wedged one resumes at pc=99 (pre-suspend) then
  idle forever.  Step-count-boundary timing also explains why probes
  heal it and why the wedge iteration varies.

FINAL CHANGE SET (uncommitted, built 23:18):
1. Interpreter.cpp nested-loop requeue (enterInterpreterFromCallback):
   putToSleep -> putToSleepPreempted (front; involuntary displacement is
   order-preserving).  THE wedge fix — verified: gauntlet7/8/9 all
   4x13/13 + exit 0 (3/3 runs; ~75s total vs wedging forever), full
   battery green (InCallbacks 2x36/36, Derived 12/12, Weak* clean,
   ProcessTest 46/46, SemaphoreTest 18/18, StepOver 10/10, Delay 5/5,
   FFICallback* 13/13).
2. Primitives.cpp primitiveExitCriticalSection: Cog parity — preempt only
   on STRICTLY higher priority; preempted active goes to FRONT (was
   yield + back-append on >=; same hazard class via Mutex prims).
3. handleForceYield/step(): aging front-append TRIED and REVERTED — it
   fixed nothing observed (zero [AGING] fires in every wedge trace) and
   cost ~40% on TFUFFIConcurrencyTest worker round-trips (front-append
   lets the aged CPU-hog bounce back each grace window, starving the P39
   workers; core-loop x4: baseline 7.0/7.9/14.1/15.0s vs front-append
   16.6/12.1/19.3/15.9s).  Comment in handleForceYield documents the
   theoretical aging-in-fork/suspend-window hazard + the fix if ever seen.
4. Comment fix in primitiveReadNextCallback (scan order wording).

Baseline-compared loose ends (stash-bisected, all PRE-EXISTING, not
regressions): TFCallbacksTest 1/8+3F+4E identical on baseline (baseline
even segfaults at exit; fixed binary exits clean);
testSingleCalloutDuringCallback 1F on both; benchFib ~36-45ms on both
(the "~12ms" note was from another config).  TFUFFIConcurrencyTest
UsingWorker is MARGINAL by design here: core loop 7-22s vs ~10s SUnit
limit — worker round-trip pacing (~20-40ms/round) is the pre-existing
disease; flakes either way.

Verification in flight: conc-final.log (CORE x4 + CONC x2) +
gauntletFA/FB.log, then commit.

## UPDATE 2026-07-03 ~23:45 — #14 COMMITTED (e9a7e984); #11 IMPLEMENTED (testing)

#14 done-check passed: ObsoleteTest x4 completes cleanly (no timeout
wedge, exit 0) on e9a7e984.

#11 weak-root GC treatment IMPLEMENTED per the deferred.md plan:
- Interpreter::RootScope{All,StrongOnly} on forEachRoot; StrongOnly
  skips the weak-cache group: methodCache_, JITMethod header oops,
  IC entries + selBitsArray, countMap/failedMap/tier2 keys.
- markPhase marks StrongOnly when !skipEphemerons (true fullGC + sweepGC);
  the scavenge-emulating skipEphemerons path and the copying scavenge
  keep all roots strong (young cache targets tenure, per plan step 3).
- New Interpreter::purgeDeadCacheRoots() runs at end of markPhase (mark
  bits final, before plan/compact): voids dead method-cache entries,
  invalidates JITMethods whose CompiledMethod/selector died (zeroing
  their IC buffers + selBitsArray so the All-scope update pass never
  walks dead oops), clears dead IC target entries in live methods,
  zeroes dead count/failed/tier2 keys, and rebuildMethodMap() so no
  stale key can false-hit a recycled address (sweepGC has no
  recoverAfterGC tail).
- PHARO_GC_PURGE_LOG=1 knob (debug_vars.h) prints per-GC purge counts.

RESULT: ObsoleteTest 3/3 x4 (was 1-2 pass + 1F + 1-2E every iteration;
suite is 3 selectors — the old "4 ran" was itself an artifact);
testFixObsoleteSharedPools passes standalone (was ours-2/3 vs stock-3/3).
Payoff: dead classes/methods are no longer pinned by VM caches at all
(IDE class-redefinition leak).

In flight: extended battery (battery4.log: Weak*/Finalization/Ephemeron
+ TFFI + Process/Delay/Semaphore/StepOver + benchFib) + gauntletG, then
commit #11.

(Previous 2026-07-02 tally superseded; that work is all committed and
documented in docs/deferred.md + memory files.)

Goal (`/goal finish the fix list`): 2 items remain — #14 repeat-run wedge
(in flight, UNCOMMITTED changes below) and #11 ObsoleteTest one-cycle pin
(designed, not started; plan in docs/deferred.md).

## UNCOMMITTED changes in the tree (compile-UNTESTED as a set)

The last build+test cycle (x4 callback suite) was interrupted before it
ran. Files modified beyond HEAD (`de284345`):

1. `src/vm/Interpreter.hpp`
   - `callbackHandedOut_[MaxCallbackDepth]` parallel flag array added next
     to `callbackContextStack_` (exactly-once hand-out; comment explains).

2. `src/vm/Interpreter.cpp`
   - `enterInterpreterFromCallback` push site sets
     `callbackHandedOut_[callbackDepth_] = false;`
   - [STATE-DUMP] now prints top-5 `[SD-FRAME]` caller chain (this part
     WAS built+tested earlier; it produced the livelock evidence).

3. `src/vm/Primitives.cpp`
   - `xtcb::g_dead` set added (timed-out worker callbacks; worker
     deregisters from queue+active and inserts into g_dead — an earlier
     purgeCallbackContext-from-worker-thread idea was REJECTED as a
     cross-thread mutation; this lazy design replaced it).
   - `adoptPendingWorkerCallbacks` push site sets handedOut=false.
   - `primitiveReadNextCallback` REWRITTEN: (a) lazily pops g_dead
     entries ([XTCB-DEAD-POP]); (b) hands each stack entry out EXACTLY
     ONCE (currently shallowest-un-handed-first — CHECK nesting order,
     may need deepest-first), extra reads answer nil. The
     [CALLBACK-REREAD] detector this replaces CONFIRMED the image reads
     the same invocation repeatedly during the wedge.
   - `primitiveCallbackReturn` pop site clears handedOut flag.
   - XTCB-TIMEOUT path: deregisters pending from queue/active, inserts
     vmcc into g_dead (no cross-thread stack mutation).

## Where the #14 investigation stands (evidence committed in deferred.md)

- Repro: TFUFFICallbackTest x4 in ONE VM. Pre-aging-fix it wedged at
  iteration 2; after 27186475 + d91c0df8 at iteration 3+ (flaky).
- wedge5.log (REREAD detector build): CB1..4 all 13/13 + X4-COMPLETE +
  exit 0 WITH 10 [CALLBACK-REREAD] warnings — run-to-run variance is
  large; wedge6A wedged again (~600s, killed).
- [SD-FRAME] livelock stack (wedge4.log): P70 spinning in
  TFCallbackQueue>>nextPendingCallback -> FFIExternalReference class>>
  fromHandle: -> Behavior>>new under executeCallback:/on:do:/on:fork: —
  image reads a NON-NIL invocation over and over (each read allocates)
  without completing it.
- Root-cause theory the uncommitted code implements: our
  primitiveReadNextCallback returned the TOP stack entry on EVERY read,
  while the image contract is queue-like (one signal == one item; nil
  when empty). Duplicate/early callback-semaphore signals (excessSignals
  accumulated across suite churn) make TFCallbackQueue loop: wait ->
  read (same still-executing invocation, non-nil) -> fork ANOTHER
  executor -> livelock.

## NEXT STEPS (in order)

1. Build: `/c/temp/src/iospharo-jit/scripts/build-windows.sh` (absolute
   path, MSYS2 CLANG64). Expect clean compile.
2. Repro gauntlet 2-3x (timeout 800; kill test_load_image.exe between).
   EXACT invocation (the `eval` keyword is REQUIRED — without it the
   harness boots the image bare and DELETES the staged startup.st;
   two runs were wasted on this 2026-07-03):
   `cd /c/tmp/probe-img && timeout 800 .../build-win/test_load_image.exe \
      /c/tmp/probe-img/Pharo.image eval "1 to: 4 do: [:i | | r | \
      r := TFUFFICallbackTest buildSuite run. Stdio stderr nextPutAll: \
      'CB', i printString, ': ', r printString; lf; flush]. Stdio stderr \
      nextPutAll: 'X4-COMPLETE'; lf; flush. 'x'"`
   Want: all 13/13 + X4-COMPLETE + exit 0. If wedge recurs:
   PHARO_STATE_DUMP_PERIOD_MS=800 sampler, grep [XTCB-DEAD-POP]/[SD-FRAME].
3. Regression battery: TFUFFICallbackTest 13/13, InCallbacks 36/36,
   Derived 12/12, TFWorkerTest, WeakSetTest 50/50, WeakAnnouncerTest
   33+1EF, StepOverTest, ProcessTest, DelayTest, SemaphoreTest,
   [30 benchFib] ~12ms. ALSO nested-callback depth>1:
   TFCallbacksTest>>testSingleCalloutDuringCallback — verify the
   exactly-once loop order works for nesting.
4. Commit ("tffi: exactly-once callback hand-out + dead-invocation
   reaping — repeat-run livelock") + push.
5. #14 done-check: ObsoleteTest x4 in one VM (the other wedge shape).
6. #11: weak-root GC treatment — full plan in docs/deferred.md
   ("weak-root treatment, concrete plan"). Step 0: capture what the
   reverted naive pre-mark-flush experiment's ERRORS actually were.
7. Full 2047-class suite before declaring goal done (cd
   /c/temp/pharo-win-test; timeout 12000 eval "(Smalltalk at:
   #SUnitRunner) runAllTests"; baseline run #9 = 27441/27967 = 98.1%,
   exit 0).

## Environment notes

- Probe image: /c/tmp/probe-img/Pharo.image (isolated copy).
- Suite env: /c/temp/pharo-win-test (Pharo-sunit.image; refvm/ = stock
  PharoConsole.exe baseline).
- TestLibrary.dll: `ninja TestLibrary` in build-win (build script skips
  it). FreeType+Cairo DLLs staged by CMake POST_BUILD from
  third_party/windows-runtime-dlls/.
- Kill stray test_load_image.exe before rebuild (link EPERM).
- git: branch jit @ de284345 pushed; only the 3 files above dirty.
