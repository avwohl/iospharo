# WIP — session snapshot before reboot (2026-07-03 late PM)

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
2. Repro gauntlet 2-3x (timeout 800; kill test_load_image.exe between):
   eval `1 to: 4 do: [:i | ... TFUFFICallbackTest buildSuite run ...]`
   on /c/tmp/probe-img/Pharo.image. Want: all 13/13 + X4-COMPLETE +
   exit 0. If wedge recurs: PHARO_STATE_DUMP_PERIOD_MS=800 sampler,
   grep [XTCB-DEAD-POP]/[SD-FRAME].
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
