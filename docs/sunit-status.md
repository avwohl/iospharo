# SUnit Status

Canonical place for comparing SUnit pass/fail across the three VMs.
Every run updates THIS file in THIS format.  No ad-hoc reports.

Harness:      scripts/pharo-headless-test/run_sunit_tests.st
Class list:   836 test classes (after skip-list filters)
Image:        /tmp/harness/Pharo.image  md5=2db30d745e41d581cb409b2eef27ecad
Per-test TO:  50s (scale=5 × default 10s)
Run watchdog: 45m (VM wall-clock)

**Note 2026-04-21**: JIT-default crash (bug 11, JM_SIZE off-by-8) fixed
in commit 1c3a5a4.  Short sanity run shows 1150+ method compiles with
zero JIT crash (previously crashed at compile #5-#65 across layers).
jit-default row below will be updated with real full-suite numbers
once the 45-min run completes.

## VMs

    Short name   Binary                            Branch / commit
    cog          /tmp/stocktest/pharo              Pharo VM v10.3.9 (2025-11-17)
    main         (not yet built in this matrix)    main @ d324080
    jit          ./build/test_load_image           jit  @ e6cc1a4

## Matrix — last run per (VM, mode)

Status legend:  P pass · F fail · E error · S skip · T timeout · — unrun

`Δcog` (rows below cog only) = count of tests that are NOT pass on this
(VM, mode) but ARE pass on cog.  Requires per-test join with the cog
row; marked `?` until the cog row is populated.

    VM     Mode     Date        Classes   Tests    P       F    E     S    T    Wall     Δcog
    cog    default  2026-04-20  828/836   17258    17195   15   23    25   0    2m       (baseline)
    main   NO_JIT   2026-04-20  34/836    412†     410     0    2     0    0    50m(wd)  2 / ≥?
    main   default  —           —         —        —       —    —     —    —    —        ?
    jit    NO_JIT   2026-04-21  824/836   16927    16864   14   18    25   6    45m(wd)  21
    jit    default  2026-04-21  0/836     —        —       —    —     —    —    45m(wd)  hang‡

`wd` = hit the VM watchdog.  `†` = main NO_JIT is enormously slower per
test than jit NO_JIT: 50 minutes for 34 classes vs jit's 45 minutes for
827.  Root cause not in this file; likely cumulative interpreter-speed
improvements between main `d324080` and jit `ed51a22` (776 commits).
Practically this means a full main NO_JIT comparison needs the watchdog
raised to ~20 hours, or a narrower class list.

Main also requires a `startup.st` workaround to boot headlessly — main's
`test_load_image` has no `eval` mode, so we inject the fileIn via
`StartupPreferencesLoader` by writing `/tmp/harness/startup.st`.

`hang` = image deadlocks in `ProcessorScheduler>>idleProcess` after
~540 method compiles; SUnit harness never starts.  JIT no longer
crashes (bug 11 fixed), but jit-default changes scheduler-visible
behavior vs stock Cog / our NO_JIT path — see `jit-uncovered-bugs.md`
bug 14 (open).

`sig` = crashed with SIGSEGV/SIGBUS.  jit default SEGFAULTs during
SUnit.  Two root causes found and partially fixed in commits
`32e8cda` (eviction-safety pinning) and `147c3a9` (W^X restore):

 * SIGBUS "PC not in any active JIT method (evicted?)" — Apple
   Silicon `pthread_jit_write_protect_np` is a per-thread toggle
   that affects the entire MAP_JIT region.  patchJITICAfterSend,
   forEachRoot (GC), flushCaches, and recoverAfterGC all called
   makeWritable() without a matching makeExecutable() before
   returning to JIT.  **Fixed.**
 * SIGSEGV "activateMethod+2756 → byteSize on null" — J2J save
   path's chain-loop frame could hold a stale JITMethod* after
   eviction, whose compiledMethodOop was zero after free+realloc.
   Pinning prevents the eviction; null-guard skips materialization
   if state.method is nil.  **Fixed.**

Residuals: more SIGBUS crashes at progressively later compile counts.
Progression so far:

    Commit       Crash at   Notes
    baseline     ~5 methods SIGBUS, eviction-related
    32e8cda      ~25       eviction safety pinning
    147c3a9      ~30       W^X restore in patchJITICAfterSend + GC
    dd91d26     same        saveJM null guards (no new crash signature)
    34f39cd      ~28       JIT_CALL macro auto-flips W^X executable

**Bisect**: with `PHARO_NO_SISTA=1` (T1 JIT only, no Sista), 44+
methods compile, **no crash**.  Image hits separate "Improper store
into indexable object" but JIT itself stays up.  Sista is the
trigger of the residual crash, not T1 JIT.

After the W^X bracketing around `fn(&sstate)` (commit `5247ba4`),
the jit-default crash signature flipped from SIGBUS at compile #28
to SIGILL at compile #42.  SIGILL crash dump (added in `eda373b`)
shows PC in the code zone at the END of a JIT method's compiled
region — instructions ahead are all zero (ARM64 UDF).  Method ran
past its own code boundary.  Different bug class:
**JIT codegen, not W^X / eviction**.  Likely a conditional-jump
target overshooting the method, or a missing fall-through
return.  Out of session scope to debug further.

jit default still cannot complete a full SUnit run; jit NO_SISTA
gets further but isn't a real benchmark of the production config.

**Whack-a-mole pattern (8 fixes this session, each uncovers the next):**

    Try               Crash at  Bug exposed
    baseline          ~5        eviction → use-after-free SIGBUS
    +pinning          ~30       W^X mode imbalance SIGBUS
    +W^X RAII         ~28       Sista compile path SIGBUS
    +JIT_CALL macro   ~28       same
    +Sista fn bracket ~42       JIT codegen overrun → SIGILL
    +SIGILL handler   ~42       confirms PC is past method end
    +Sista compile bracket ~15  unmasks tryJITActivation null-deref

**Bisect with Sista gates** (still residual crash, but later):

    Sista config              Crash at  Notes
    NO_SISTA=1                ∞         clean (no JIT crash; image error unrelated)
    SISTA_COMPILE only        ~45       SIGILL — compile alone corrupts state
    SISTA_NO_STORES           ~112      null-deref deep in tryJITActivation
    default (compile+dispatch) ~15      same null-deref as NO_STORES, earlier

The store-blocking gate pushes the crash floor from #15 to #112 —
strong evidence Sista's `kStoreInstVar` lowering (or its temp-store
peer) is one of the corruption sources.  Even with stores blocked
the crash recurs deeper in the suite, so there's at least a second
Sista codegen issue layered underneath.

Each one is a real bug.  Together they suggest Sista's IR
lowering has a fundamental correctness issue that's been masked
by the JIT-default crash floor moving every time a more-shallow
bug was fixed.  Continuing without a focused multi-day Sista
audit will keep revealing the next bug, not converge on green.

Numbers above are the parser's count (`scripts/classify-sunit.py`)
for direct comparability; they differ by a handful from the harness'
own `BATCH TOTAL` line (e.g., harness reported 16465 P / 40 S for jit,
parser sees 16467 P / 23 S — rounding off-by-one on some class
boundaries).  The Δcog column is the authoritative comparison and is
computed from the parser output.

## Shared non-pass (both cog and jit fail the same test)

9 real shared fails/errors + 33 shared skips = 42 tests.  These are
the *non-regressions* — cog has the same problem, so not ours to
fix on this branch:

    Fail   3  ClyAsyncQueryTest/ClyFilterQueryTest>>testHasCompositeScopeFromSubqueries
              ClySemiAsyncQueryResultTest>>testItemsChangedNotificationShouldResetItems
    Error  6  ClyAsyncQueryTest/ClyFilterQueryTest "Wrapper query" (3 selectors × 2 classes)
    Error  1  DebugPointTest>>testTranscriptDebugPoint (NonInteractiveTranscript>>#contents DNU)
    Error  1  OCClassBuilderTest>>testCreateNormalClassWithTraitComposition (OCCodeError)
    Skip  33  Image-level skips (FFI Platform, Win32, OCCodeReparator, etc.)

## Δcog for jit default — partial run (SIGSEGV, 36/836 classes)

With `PHARO_JIT_DEFER=30`, 36 classes / 3410 tests completed before
the JIT-evict crash.  **Zero Δcog regressions in those 36 classes**
(P:3404 + Skip:6 = full parity with cog for the ones tested).  The
crashed class is `BehaviorTest`.

This is a weaker signal than a full run but encouraging: for the
prefix the VM could execute under JIT+Sista, no tests regressed vs
cog.  Need to fix the JIT-evict crash before we can extend this.

## Δcog for main NO_JIT — partial run (50-min watchdog, 34/836 classes)

Only 34 classes completed; 16,783 of cog's 17,258 tests weren't run.
Real Δcog is unknown until main finishes or is run on a narrower list.

Regressions seen within those 34 classes:

    error  2
        AllocationTest>>testOneGWordAllocation
        AndreasSystemProfilerTest>>testSimple

Both look VM-level (allocation limit, profiler timing).  Full list at
`results/sunit-2026-04-20-main-nojit.delta-vs-cog.txt`.

Main-vs-jit comparison is blocked until main can get through more
classes.  Two ways forward: raise the watchdog past 20 h, or run only
the 548 classes that jit flagged as Δcog=non-zero.

## Δcog for jit NO_JIT (548 regressions)

Tests that pass on cog but FAIL/ERROR/TIMEOUT on our VM:

    error    521    (454 of these are C11 FFI DNU cascade)
    fail      15    (path/env + FFI params + a few assertion fails)
    timeout   12    (FinalizationRegistry x5, FFICallback x5, FLBinary x1, misc x1)

Plus 181 cog-tests that jit NO_JIT didn't execute at all (the 9
watchdog-cut classes).  Every one of those 181 is a *potential*
additional regression — unknown until we finish the run.

Full list at `results/sunit-2026-04-20-jit-nojit.delta-vs-cog.txt`.

## Delta vs stock Cog

Each cell = tests that **pass on cog** but **fail/error/timeout on this VM+mode**,
on the SAME image.  Zero means parity.  "—" means the cog column hasn't been
run or this VM+mode hasn't been run.

    Full-suite delta (all 17k+ tests)
    VM+mode           Δ (cog-P → here-not-P)    Source of evidence
    main NO_JIT       —                          not run
    main default      —                          not run
    jit  NO_JIT       ≥454 (likely ≥900)         15-class probe 2026-04-20
    jit  default      —                          not run

"≥454" = our direct full run on jit-NO_JIT shows 454 FFI `C11` DNUs
confirmed passing on cog.  The other 82 non-FFI errors + 18 fails +
13 timeouts are also *suspected* regressions but haven't been checked
against cog yet.  If they all regress, Δ would be ≥536+18+13 = ≥567.

## Failure attribution — 2026-04-20 jit NO_JIT run

Every error/fail/timeout is in one of these buckets until proven
otherwise.  Buckets roll up; re-bucket when cross-checked on cog.

    Attribution                       Count  Cross-checked on cog?
    CONFIRMED_VM_REGRESSION             454  yes — 15-class probe, all pass cog
    SUSPECTED_VM_REGRESSION             113  no — `packageName`/`select:thenDo:`/
                                              `disable`/`outputFileReference` nil
                                              (92 + 59 + 49 + 17 = 217, but some
                                              classes overlap — net 113)
    SUSPECTED_VM_REGRESSION_CALYPSO      24  no — Wrapper/origin errors
    SUSPECTED_VM_REGRESSION_OTHER        12  no — unclassified
    CONFIRMED_VM_FAIL                    18  no — test-level assertion failures;
                                              docs/test-results.md 2026-04-12
                                              listed 6 of these as *known VM path*
                                              failures (testUserLocalDirectory,
                                              testVmBinary, testVmDirectory) —
                                              environment/path expectation, not
                                              VM correctness
    CONFIRMED_VM_TIMEOUT                 13  no — FinalizationRegistry (5×300s),
                                              FFICallback (5×80s), FLBinary (1),
                                              MiscFFI (2).  Some may be genuine
                                              hangs on our VM even where cog
                                              completes; some are interpreter-
                                              speed ceilings (see A00).
    CONFIRMED_IMAGE_BUG                   0  would be: fails on cog too
    TOTAL                               567

(567 is F+E+T; 40 skips excluded, 16465 passes elsewhere.)

## Specific failure signatures seen

For quick grep.  First column is the bucket; second is count; third
is the signature substring.

    VM_REGRESSION       454  MessageNotUnderstood: Message not understood: C11 class >> #current
    VM_REGRESSION?       92  MessageNotUnderstood: receiver of "packageName" is nil
    VM_REGRESSION?       59  MessageNotUnderstood: receiver of "select:thenDo:" is nil
    VM_REGRESSION?       49  MessageNotUnderstood: receiver of "disable" is nil
    VM_REGRESSION?       17  MessageNotUnderstood: receiver of "outputFileReference" is nil
    VM_REGRESSION?       12  Error: Wrapper query should include single subquery
    VM_REGRESSION?       12  Error: Can't find the requested origin
    VM_REGRESSION?        8  MessageNotUnderstood: receiver of "close" is nil
    VM_FAIL_ENV           3  testUserLocalDirectory / testVmBinary / testVmDirectory
    VM_FAIL_KNOWN         2  testFloatParameters / testIntegerParameters (FFI param)
    TIMEOUT               5  FinalizationRegistry suite (300s each)
    TIMEOUT               5  FFICallback suite (80s each)
    TIMEOUT               2  FLBinary + one misc (80s)

## Progress trend (same config, over time)

Each row = one dated run of jit NO_JIT.  Delta columns show change
versus the row immediately above.

    Date        Tests   P       ΔP      F    ΔF   E     ΔE    T    ΔT
    2026-04-12  12576   12531   —        14   —   2      —    —    —   (535 classes, older
                                                                         class list)
    2026-04-20  17072   16465   baseline 18   +4  536   +534  13   +13  (836 classes, expanded
                                                                         list; error explosion
                                                                         concentrated in FFI)
    2026-04-21a 17049   16980   +515     21   +3   17   -519   6    -7  (post-bug-11 layer 5)
    2026-04-21b 17048   16987    +7      12   -9   18    +1    6     0  (post-bucket-15 fixes:
                                                                         NFC/NFD, mkdir errno,
                                                                         readdir stat, exec path)
    2026-04-21c 16927   16864   -123     14   +2   18    0    6     0  (post-revert of
                                                                         finalization queue
                                                                         split; watchdog-truncated
                                                                         at FFICallbackParameters)

Note: 4-21c is a shorter run (DELAY-DEATH truncation cut ~10 classes).
Tests delta is not meaningful vs 21b because of the early exit.

Δcog progress: 548 (4-20) → 27 (4-21a) → 19 (4-21b) → 21 (4-21c).
4-21c is 10 fixes shy of 4-21a+10; it added 2 new entries (TraitTest
and EpDisabledIntegration) from covered classes.  The 10 bucket
15.A-C fixes are preserved:
  testUserLocalDirectory, testVmBinary, testVmDirectory, testIsExecutable
  testFromPlatformPath, testToPlatformPath
  testCreateDirectoryExists, testCreateDirectoryNoParent
  testMoveToFailingMissingDestination, testEntriesHaveAttributes

## What I DON'T know — open deltas

1. Does stock cog complete the full 836-class suite?  How many fail?
   (Not run.  Pharo 10 VM segfaulted on our harness; need simple
   runner.)
2. Does main (d324080) show the FFI C11 cascade too?  If yes, the
   regression is older than the jit branch.  (Not run.)
3. Does jit default (JIT on) differ from jit NO_JIT?  (Not run.)
4. Do the 113+24+12 suspected buckets actually regress on cog?

## How to update this file

After any SUnit run:

1. Save the raw `/tmp/sunit_test_results.txt` somewhere dated, e.g.
   `results/sunit-YYYY-MM-DD-<vm>-<mode>.txt`.
2. Update the matrix row for that (VM, mode) — replace the previous
   row, don't append a new one.
3. Update the Progress-trend row (append, don't replace).
4. Update the Delta-vs-cog cell if cross-checked.
5. Update the Specific-signature table if new failure signatures
   appeared.

Commit.  One commit per run.  Subject: `sunit-status: YYYY-MM-DD <vm> <mode> — Δ…`.
