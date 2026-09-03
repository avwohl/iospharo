# Pharo SUnit Suite — VM Compatibility Status

> **2026-09-02/03 — both architectures swept, and they agree.**
>
>     arch     classes  tests   PASS    FAIL  ERROR  TIMEOUT  SKIP   rate
>     x86_64    2046    28071   27833    22     26      10     180   99.15%
>     arm64     2005    27682   27453    21     21       5     182   99.17%
>
> **No x86-vs-arm codegen divergence in 28071 tests.**  Every class non-clean
> on one arch and clean on the other is a Rosetta timeout (the x86 build runs
> at roughly half arm64's speed against a fixed 80 s per-test bound), a
> wall-clock assertion, the Windows-only `w64Convention` symbol missing from
> our test dylib, a working-directory artifact, or a class the other arch never
> reached.  Write-ups: `docs/results/sweep-x86-2026-09-03.md` and
> `docs/results/sweep-arm-2026-09-02.md`.
>
> The x86_64 run is the more complete of the two — batch 1901-1950 storms on
> arm64 (defect #23, 39 classes lost) and runs clean on x86_64 in 270 s — so it
> is the first measurement this project has of the trait block:
> `TraitTest` 53 P / 1 F on `testTraitsUsersSanity`, `TraitFileOutTest` 2 E (a
> CWD artifact), `TraitInTraitClassTest` 1 T, the other 24 classes clean.  Cog
> runs all 27 at 270 P / 0 F / 0 E.
>
> **2026-09-02 — arm64 full sweep on the rebuilt host: 99.17%.** 2007 classes,
> 27692 tests, 27461 P / 21 F / 21 E / 7 T / 182 S.  F+E is 42 against the
> 2026-08-22 run's 52 on ~375 fewer tests.  Full attribution per class in
> `docs/results/sweep-arm-2026-09-02.md`; artifacts alongside it.  **This is
> not the whole suite**: one batch died of an allocation storm and took 39
> classes with it — all of the trait tests — so read the figure as "2007 of
> ~2047 classes".  Two more classes were absent from the raw aggregation
> because the runner spliced their `Total:` line into a timeout verdict; that
> is fixed in the submodule, and both are folded into the numbers above.
> x86_64 is running as of this writing.
>
> **2026-09-03 — the package tier now has both arches.**  9376 P / 16 F / 24 E
> on x86_64 against 9326 P / 16 F / 16 E on arm64 over the same 354 classes.
> FAIL is identical on both -- 14 DataFrame + 2 PolyMath, same selectors -- so
> there is no x86-only package failure.  The ERROR delta is XMLParser (5 vs 0)
> and PolyMath (19 vs 16), and PolyMath also ran 55 more passing cases on x86
> with one fewer timeout, so the two runs did not execute the same set of
> cases.  Write-up: `docs/results/packages-x86-2026-09-03.md`.
>
> **Defect #23, the arm64 Context storm that costs 39 classes a sweep, is
> bounded.**  The loop is `Context>>cannotReturn:` -> `error:` -> `signal`
> against a sender chain with no handler in it, and `Context>>freeze` copying
> the whole chain each round is what allocates 2.6M Contexts.  Four of the six
> `cannotReturn:` sites had no rate limit; the VM already terminates the
> process at the fifth, and `cannotReturnStormGuard` now applies that rule to
> the other four.  Measured interleaved under held load: **7 aborts of 10
> become 0**, with the guard firing in 7 of those runs, and the same build
> scores 0 non-clean classes on batch 1-100.
>
> Two attempted root fixes did NOT survive.  One (materializing pending J2J
> saves before building a closure) broke 11 classes on batch 1-100 and is
> reverted.  The other (the materialized-context handover) is kept but is not
> load-bearing.  The hole itself -- a closure created inside a J2J chain
> captures an `outerContext` missing every J2J-hidden caller, 481 times in a
> 20-second run -- is still open, with two inert candidate fixes behind knobs.
> Sweeps on both arches are re-running with the guard; the numbers above
> predate them.
>
> **A Δcog IS obtainable on this host after all** — the stock VM's fixed-address
> `codeZone` abort is arch-specific, and the x86_64 Cog runs under Rosetta.
> The whole residual was measured against Cog v10.3.9 the same evening
> (`docs/results/sweep-arm-2026-09-02/cog-residual-baseline.txt`): six classes
> are at exact parity on the same failing test, everything else is ours, and
> the sixteen classes long bucketed as "the missing display" score 0 F / 0 E on
> **headless** Cog with no prelude.  Fourteen of the sixteen are root-caused
> (defect #24): installing `MorphicUIManager` on Cog and suspending its UI
> process reproduces our exact failure sets, and our runner suspends that
> process because our headless resume restarts `MorphicRenderLoop` into a
> busy-spin of Morphic DNUs (defect #25).  That accounts for 9 of the 21 FAILs
> and 17 of the 21 ERRORs above; three more FAILs are parity with Cog, and
> three are genuinely ours.  Separately, the runner runs one case per selector where parameterised
> suites define several, so the test count understates the suite by ~38% —
> 10567 cases image-wide.  `RSLinesTest`'s
> `BlockCannotReturn` is root-caused as defect #22 (a trait method copy whose
> block still names the trait's method), and the storm that ate the trait
> tests is defect #23 (Cog runs those 27 classes clean).
>
> **2026-08-22 — the package tier was 7x slower than its own baseline, and is
> now faster than it.** Code-zone eviction became reachable on 2026-08-22 and
> cost more than it saved until three defects in it were found and fixed:
> XMLParser went 1055 s (2026-08-17, eviction unreachable) -> TIMEOUT at
> 1800 s -> 369 s, and Grease 50 s -> 365 s -> 48 s.  Two packages that had
> never produced a result on this host now do.  Numbers and method in
> `docs/results/packages-arm-2026-08-22.md`; the defects are written up in
> `docs/WIP.md`.
>
> Read any per-test failure in this file against
> `TestCase>>skipOnPharoCITestingEnvironment`: 17 tests carry that guard and
> Pharo's own CI skips them, while our sweeps do not set
> `PHARO_CI_TESTING_ENVIRONMENT` and therefore run them.  Two of the 17 are
> named individually in this repo's arm-vs-x86 residual list.

> **2026-08-19 — all three tiers, both architectures.** One tally method
> throughout: sum only the `=== BATCH TOTAL ===` blocks (per-class lines use
> `P:/F:/E:/S:` and double-count if included). Earlier figures in this file used
> a different denominator and are not directly comparable.
>
> SUnit, 2047 classes, same image loaded by each VM:
>
>     arm64  RAW       2043 cls  28058 tests  P=27725  F=23  E=22   98.81%
>     x86_64 RAW       2037 cls  27982 tests  P=27380  F=26  E=287  97.85%
>     arm64  NO-CAIRO  1925 cls  27187 tests  P=26855  F=23  E=21   98.78%
>     x86_64 NO-CAIRO  1919 cls  27111 tests  P=26770  F=25  E=27   98.74%
>
> NO-CAIRO excludes RS*/Athens*/Cairo*/FreeType*/FT*/Roassal*. The host has only
> an arm64 `libcairo.2.dylib` and no Intel Homebrew, so a Rosetta x86_64 VM
> cannot load it: 260 of x86's 287 errors are that one missing dylib. Excluding
> it, the two architectures are at parity. Every remaining non-Cairo x86 excess
> is accounted for — 6 more Cairo-backed cases under Sp*/Hi* names, and 2 from
> one Windows-only `w64Convention` test that arm skips and x86 runs.
>
> Package suites (7 packages), after the Grease fix:
>
>     arm64   9383 pass  16 fail  17 err
>     x86_64  9377 pass  16 fail  23 err
>
> x86_64 package LOADS cannot run here — Iceberg resolves `github://` through
> libgit2 and only an arm64 build exists — so x86 tests run against
> arm64-loaded images via `REUSE_FROM` (Spur images are architecture-neutral).
> The arm64 VM loads `github://` fine; the failure is scoped to the x86_64
> binary, not the host.
>
> **No VM defects.** A 9-agent adversarial investigation classified every
> residual package failure. None is the VM computing something wrong. They are:
> 12 DataFrame failures from `Float DefaultComparisonPrecision` tightening
> 1e-4 -> 1.49e-8 (restoring the old value gives 14/14 and 227/227); 2 from
> Pharo 13 `DateAndTime` accepting only `y-mm-dd`, reproducible on a pristine
> base image; 9 from selectors removed in Pharo 13
> (`#whichCategoryIncludesSelector:`, `#newSubclassOf:using:`); 1 from a
> package's own O(n^2) generator; 3 from an intentional SMark demo fixture; and
> 12 SUnit watchdog timeouts.
>
> With `TestCase defaultTimeLimit` raised 10s -> 600s (verified by read-back),
> PolyMath's F+E drops 8 -> 2. The survivors are the `SMarkTest` Pharo 13 API
> removal and `PMArbitraryPrecisionFloatTest>>testPrintAndEvaluate`, which
> carries its own `<timeout: 50>` pragma and so ignores the class default —
> it passes alone and exceeds 50s inside a full class run.
>
> VM C++ tests: green on both arches (`test_class_table` ALL PASSED,
> `test_relaunch` 3/3, `test_asmjit_t1_stub`, `test_sista_ir`,
> `test_sista_survey`). Run WITHOUT an image argument these exit rc=1 with a
> usage line that reads exactly like a failure — it is not one. `test_platform`
> reports 0 pixels on both arches; that predates the 2026-08-19 `setGlobal` fix
> (verified by rebuilding the parent commit) and whether 0 is its expected
> headless result is NOT established.

> **2026-08-13 full suite, macOS-arm64, 2052 classes** (all of this session's
> fixes: auto-GC ephemerons #18, exception-blacklist removal, DNU-intercept
> removal, objectAsMethod super sends, UnixOSProcessPlugin pipes, preemption
> starvation guard #20):
>
>     27726 P / 27 F / 25 E / 7 T / 182 S
>
> measured against a same-session baseline of the identical build minus the
> #20 guard (27720 P / 29 F / 25 E / 11 T).  Per-test diff of the two runs:
> 8 newly passing (2 of them `ReleaseTest` timeouts, `ZnHTTPSTest>>testTransfers`,
> `RBBrowserEnvironmentTest>>testVariableEnvironmentInstVars`,
> `WeakValueDictionaryTest>>testAssociationsWithGarbageCollectedValue`), 2 newly
> failing and both timing-sensitive (`SUnitTest>>testGreenTestThenLongRunningTest`,
> `DTTestProfilingTest>>testDTTestCoverageResulLeafsContentHaveATestCase`).
> Treat single-run deltas of this size as the harness noise floor, not signal.
>
> **2026-08-12.** Headline metrics that used to sit in `docs/deferred.md`
> ("27,760 P / 99.92%", "FINAL 2026-07-07") are the July picture and are
> archived under `docs/history/catalog-state-2026-07.md`. Current numbers
> live here and in `docs/results/`, next to their artifacts. Latest full
> suites (2026-08-11/12, 2055 classes): macOS-arm64 27701 P / 22 F / 12 T /
> 50 E; Linux-aarch64 27647 P / 24 F / 13 T / 102 E — 93 of those ERRORs
> are one open defect (`docs/vm-compat-bugs.md` #1).
>
> **Every column here must carry a defect number, TIMEOUT included**
> (2026-08-13). The T column had been a bare count for months, and two of
> the entries in it were an unfiled Cog divergence:
> `FreeTypeCacheTest>>testGlyphAccessIsThreadSafe` and
> `>>testGlyphAccessIsThreadSafeWhenAskingWidth` are killed at the 80 s
> watchdog on BOTH arches while stock Cog passes them in under 2 s. That is
> now `docs/vm-compat-bugs.md` #19, with the gap measured (BitBlt is at
> parity; `drawString:` is 12x, a full text render 25x). The remaining
> TIMEOUTs are the reflective-scan activation wall, #6. If a run produces a
> timeout that matches neither, FILE IT before publishing the numbers.


## 2026-06-04 full-suite run: custom VM vs stock Cog

**Setup:** full discovery run (every non-abstract `TestCase` subclass — the
2026-04 no-skip policy), Pharo 13 image. Stock Cog (`~/stockpharo/pharo`,
`run_sunit_cog.st`) on the x64 box vs the clean C++ VM (`test_load_image`,
`run_sunit_tests.st`). Custom-VM arm64 = local build; x86 = AWS box.

**Stock Cog — COMPLETES the full suite (baseline):**

```
  classes  2045
  pass     27815      fail 38   error 83   skip 128
  total    28064      pass rate 99.1%
```

**Custom VM (arm64, Sista ON) — BOTH full-suite deadlocks are now FIXED; the
run is bounded only by a wall-clock cap, no longer by any wedge.**

```
  blocker #1 (FIXED)  Sista HELPER_SENDS broke the Context sender chain so a
                      forked test's unhandled error skipped its ensure: →
                      doneSem deadlock. Hung at ~80 classes
                      (CollectionArithmeticTest). Fix: HELPER_SENDS default-off
                      (below). Unblocked 80 → 511 classes (6×).
  blocker #2 (FIXED)  At ~511 classes a deep recursive Context>>copyTo: (freezing
                      an error's signalerContext on a ~4000-deep call chain)
                      overflowed at fd=4096; the overflow termination skipped
                      ensure:/ifCurtailed: and LEAKED a held critical: mutex, so
                      the Delay/timer scheduler blocked on it forever → only-idle
                      deadlock. Two-part fix (commits 73eb8947 + submodule
                      c2cbfa6). See "Blocker #2" below.
```

With both fixes, the full run **sails past the old ~511 wedge to 836 classes /
17,125 tests** before the runner's 2h batch deadline aborts it (0 DELAY-DEATH, 0
overflow hard-kills end-to-end) — i.e. the remaining limit is wall-clock time,
not a deadlock. (The error-heavy Package/Trait/System clusters each emit 25-frame
stack traces, so the run averages ~7 classes/min; the full 2051-class discovery
would need a higher `SUnitMaxBatchSeconds`.) Other configs (for the record):
arm64 Sista OFF stalls earlier at IntegerTest (class 7); x86 Sista ON still DNUs
at startup (SUnitRunner>>nextRunNumber inlined-block wrong-receiver miscompile,
the x86 analog of arm64 fix 4b446bf4, not yet ported).

**The CollectionArithmeticTest hang is NOT a JIT/Sista bug — corrected
2026-06-04.** Originally mislabelled a "Sista miscompile of testRunningAverage".
Investigation (eval-mode repro + runCase) disproved that:
- Every test in the class — testRunningAverage, …WithSubsetSize1…, testStdev,
  testRunningMax/Min — PASSES when run directly via `(Class selector: sel)
  runCase`, under Sista, in a warm VM. The test *logic* is correct.
- The full-suite run hangs in the pure INTERPRETER too (`PHARO_NO_JIT=1`), at a
  *different* test (testStdev-ish) — so it is timing-dependent and not JIT.
- No WhileTrueAccum fold fires on this path (CAND counter = 0).
- At the hang only the idle process (P10) is runnable → a **scheduler
  DEADLOCK**, not a runaway loop.
The hang is in the SUnit **runner's** per-test fork + Delay-based watchdog
machinery (`run_sunit_tests.st`) — not in the tests or the JIT.

**Root cause pinned 2026-06-04: a forked process that hits an UNHANDLED ERROR
terminates WITHOUT running its `ensure:`/unwind blocks.** Minimal repro on our
VM (eval mode):

```
  | sem ran | sem := Semaphore new. ran := false.
  [[nil fooBarBaz] ensure: [ran := true. sem signal]] fork.
  sem waitTimeoutSeconds: 3        "→ ran=false, sem never signals (TIMEOUT)"
```

Explicit `[...terminate] ensure: [...]` DOES unwind (ran=true), and normal
completion + caught errors unwind too — only the unhandled-error termination
path skips unwind. In the runner, each test is
`[[testInstance runCase] on: TestFailure/Deprecation/TestSkipped/Error ...]
ensure: [testDone := true. doneSem signal]`; a watchdog (P60) loops
`relinquishProcessorForMicroseconds:` until `testDone`, and the runner waits on
`doneSem`. When a test raises something that escapes those handlers (a non-Error
exception, a 2nd error inside the Error handler's signalerContext/printString
walk, or a mishandled Deprecation resume), the test process terminates via the
unhandled-error path → its `ensure:` is skipped → `testDone` stays false +
`doneSem` never signals → the watchdog loops to its ≤300s deadline and the runner
blocks → the "deadlock". PROC-DUMP corroborates: ~52 leaked terminated processes
with corrupt `myList=0x300000000` (termination cleanup incomplete on this path).
The DIAG timer was healthy/armed throughout — NOT delay-scheduler death.

**FIXED 2026-06-04 — root cause was Sista HELPER_SENDS.** The escaping exception
is `CollectionIsEmpty` (raised by `Collection>>average` on an empty collection in
the running/average tests). It IS an `Error`, so the runner's `on: Error do:`
*should* catch it — but its signaler stack is only 2 frames (`sum` ← `average`)
with `average`'s **sender = nil** (`0x300000000` is just `nil`, the first special
object — the earlier "corrupt sender" was a red herring). The broken sender chain
came from **HELPER_SENDS** (Sista helper-send activations): a helper-send
activation leaves a broken Context sender chain, so exception **handler search
cannot reach the enclosing `on: Error`/`should:raise:` handler** → the exception
goes unhandled → the forked test process is abandoned without running its
`ensure: [doneSem signal]` → the watchdog loops to its deadline and the runner
blocks. Confirmed by bisect: `PHARO_NO_SISTA_HELPER_SENDS=1` makes the hanging
class pass **20/20 on both interpreter and Sista**; default (HELPER_SENDS on)
hangs both. The materializeFrameStack cycle-break (`0x300000000`/sender=nil) is a
timing-dependent symptom of the same broken chains, not the cause.

**Fix:** HELPER_SENDS is now **default-off** (`DebugSettings.cpp`:
`noSistaHelperSends = !envEq1("PHARO_SISTA_HELPER_SENDS")`), opt back in with
`PHARO_SISTA_HELPER_SENDS=1`. Disabled until the helper-send activation reifies a
correct sender chain. Validated: CollectionArithmeticTest 20/20; an 8-class
diverse subset (incl. the previously-hanging CollectionArithmeticTest +
IntegerTest) completes with **1298 pass / 0 fail / 29 error / 4 skip, no hang**;
the Sista loop bench is unchanged at 19/19 (the counted-loop fusions / to:do:
WhileTrueAccum / kPrimAt work don't use HELPER_SENDS).

**Blocker #2 (FIXED 2026-06-04, commit 73eb8947): stack-overflow termination
leaked a held `critical:` mutex.** ~60 min into the full run (class ~511,
`PackageAndMethodsTest`, after a run of mass-erroring `Package*` tests), a forked
test process overflowed the stack (fd=4096) inside `Context>>freeze` →
`freezeUpTo:` → `copyTo:` (error-handling stack capture) **while a
`Semaphore>>critical:` was on its stack.** The old `handleStackOverflow` →
`terminateAndSwitchProcess` hard-killed the process WITHOUT running its
`ensure:`/`ifCurtailed:` unwind blocks, so the `critical:` mutex was never
released. The Delay/timer scheduler (or any process) entering that critical
section then blocked on the leaked mutex forever → `timerSemaphore_` stays nil,
only P10 idle runnable, total deadlock; the C++ `[DELAY-DEATH]` recovery only
re-signals the timing semaphore (futile — the scheduler is alive-but-blocked, not
dead) and the 2h batch-abort can't fire because it needs the dead timer.

Root cause CONFIRMED with a deterministic ~2s repro (`/tmp/mutex_leak.st`): fork
`mutex critical: [infinite-recursion]` → overflow → terminate; a second process's
`mutex critical:` then blocks forever (`held=false`).

**Fix — TWO parts, both needed:**

1. **VM (commit 73eb8947):** on overflow, `handleStackOverflow` drives the
   image's own `activeProcess terminate` (walks the context chain running the
   unwind blocks, RELEASING the mutex, before ending the process) instead of the
   C++ hard-kill. The unwind itself pushes frames at fd≈4096, so a new
   `inStackOverflowSignal_` flag grants `StackOverflowSignalHeadroom` (8192) extra
   frames while driving recovery, cleared on the next process switch; a second
   overflow past the headroom falls back to hard-kill. (Signalling `#error:`
   instead does NOT work — unhandled in a forked test, and our unhandled-error
   path skips unwinds too; explicit `terminate` unwinds.)
2. **Image (submodule commit c2cbfa6, `run_sunit_tests.st`):** part 1 ALONE was
   INSUFFICIENT — the full run still wedged at fd=12288 because the
   terminate-unwind ITSELF re-triggered a deep `freeze→copyTo:` (a secondary
   `SubscriptOutOfBounds`), re-overflowing past the headroom → hard-kill fallback
   → leaked the mutex again. Root cause: stock `Context>>copyTo:` copies the
   sender chain RECURSIVELY (`tmp1 privSender: (self sender copyTo: aContext)`),
   so ANY freeze/copy of a deep stack recurses ~chain-deep and overflows. Got the
   exact source via `(Context>>#copyTo:) decompile` (no .sources) and replaced it
   with an ITERATIVE copy — so the overflow never happens in the first place.
   (See `docs/image_issues.md`.)

Together they're robust: iterative `copyTo:` prevents overflow on bounded deep
stacks (the actual trigger); the VM terminate fix releases mutexes for genuine
infinite recursion that still overflows. Validated: mutex repro `held=false→true`;
iterative-copyTo: eval (5000-deep recursion that errors caught cleanly, 0
overflows; normal catch + ensure: unaffected); simple overflow recovers + timer
survives; Sista bench completes; 8-class subset BATCH COMPLETE (no wedge).
**End-to-end CONFIRMED:** the full run reached 836 classes / 17,125 tests with 0
DELAY-DEATH and 0 overflow hard-kills (vs the old ~511 wedge), bounded only by the
2h batch deadline. Distinct from blocker #1 (sender chains) and the asmjit RA
crash (bb158a7f). See the `timer-scheduler-wedge` memory for the full
investigation.

**Δcog on the 836 classes the custom VM (Sista, both fixes) reached before the 2h
cap** (17,125 tests, `scripts/classify-sunit.py /tmp/results_cog_full.txt
/tmp/results_custom_fixed.txt`):

```
  custom  pass 15307   fail 213   error 1573   skip 24   timeout 8   (89.4%)
  vs cog: 1776 tests pass on Cog but not here  (1559 error, 209 fail, 8 timeout)
          +10742 cog-tests not run here (the ~1215 classes past the 2h time cap)
```

(For reference, the earlier wedge-limited two-pass run reached only 560 classes /
12,691 tests / 1031 regressions — the jump to 836 is the blocker-#2 fix taking
effect. The regression CLUSTERS below are from that 560-class snapshot but hold
proportionally; the dominant `#SystemOrganization` env-gap grows with the added
Package/System classes.)

Regression clusters (delta file `*.delta-vs-cog.txt`), by count of cog-pass
tests failing here:

```
  SystemEnvironmentTest 96, PackageTest 41, PackageOrganizerTest 30,
  PackageAndClassesTest 25, PackageOnModelTest 19, PackageAndMethodsTest 16,
  PackageAnnouncementsTest 11
      → ONE shared root cause: `#SystemOrganization not found in
        SystemEnvironment` (KeyNotFound). The headless image's
        SystemOrganization/PackageOrganizer isn't fully set up — an
        ENVIRONMENT-setup gap, not VM correctness. ~250+ of the 1031.
  TraitTest 35, ClassDescriptionProtocolsTest 37, TraitPureBehaviorTest 19,
  SlotMigrationTest 17, SlotIntegrationTest 15, TraitCompositionTest 13,
  ClassAnnotationTest 14, SlotAnnouncementsTest 13
      → trait/slot/class-description reflection; partly the same
        class-organization gap, partly real reflection differences.
  OCASTSingleBranchConditionalTranslatorTest 26, EFMessageExpressionTest 24
      → OpalCompiler AST translation.
  WeakIdentityValueDictionaryTest 14, WeakIdentityKeyDictionaryTest 10
      → weak/finalization. HeapTest 14, BagTest 12 → collections.
  7 timeouts (all watchdog-killed at 80s, known-slow): IntegerTest +
  LargePositiveIntegerTest testReciprocalModulo, RandomTest testDistribution,
  SHA256Test testFips180Example3, OCSimpleFormatterTest testCoreSystem,
  OCCodeReparatorTest testDeclareInstVar.
```

157 distinct classes have at least one regression; the Package/SystemEnvironment
cluster (a single environment-setup gap) accounts for the largest share.

**Bottom line:** Cog runs the whole suite at 99.1%. The custom VM reached
**560 classes / 12,691 tests at 91.7%** (6× the prior 80-class stall) after the
HELPER_SENDS fix (blocker #1), then BOTH full-suite deadlocks were root-caused and
fixed: blocker #1 (broken sender chains) and blocker #2 (stack-overflow
termination leaking a `critical:` mutex, commit 73eb8947). Both are distinct from
the asmjit RA crash fixed earlier (bb158a7f — bench-validated). Of the 1031
cog-pass regressions, the largest single share (~250+) is one environment-setup
gap (`#SystemOrganization` missing in the headless image's `SystemEnvironment`),
not a VM defect. Remaining levers for custom-VM SUnit coverage: (a) seed
`SystemOrganization`/`PackageOrganizer` in the test image to clear the
Package/SystemEnvironment cluster (~250 tests); (b) the trait/slot reflection and
OpalCompiler-AST clusters; (c) port the x86 inlined-block wrong-receiver fix
(nextRunNumber, 4b446bf4) so the x86 build can run the suite at all.

## 2026-05-28 first end-to-end run

**Setup:** `scripts/pharo-headless-test/` harness, fresh Pharo 13
image (`/tmp/harness/Pharo.image`) with `setup_fake_gui.st` +
`run_sunit_tests.st` injected via Cog `eval --save` into
`/tmp/harness/Pharo-sunit.image`.

**Result:** Run completes structurally but reports 0 pass / 0 fail /
0 error / 0 skip / 0 timeout for every test class.  2046 test
classes discovered (dynamic fallback); zero of them ran any tests.

The harness is exposing three latent VM bugs that block meaningful
SUnit execution:

### 1. `FileReference >> #lines` DNU

```
outer err: MessageNotUnderstood
msg: Message not understood: FileReference >> #lines
```

The harness tries to read `/tmp/sunit_test_classes.txt` via
`listFile contents lines` to use the curated class order (which
keeps `ClassQueryTest>>testAllCallsOn` counts comparable to Cog).
Our image doesn't have `#lines` on FileReference.

Effect: falls back to dynamic discovery via `TestCase allSubclasses`
— harness still runs, but with a different class iteration order.

### 2. `Character>>isLetter` broken

```
DIAG: Unicode isLetter:$t=false isDigit:$5=true
DIAG: cs isLetter: $t=false
DIAG: $t between:$a and:$z=true
DIAG: isLetter: source=  DIAG error: #Error Attempting to assign selector with wrong number of arguments.
```

`$t isLetter` returns `false`.  `$t between: $a and: $z` returns
`true` (so basic character-range math works).  Unicode classification
tables are either uninitialized or our VM has a primitive bug.

The harness diagnostic also tries to read the source code of
`isLetter` to print it; that throws `Error Attempting to assign
selector with wrong number of arguments.` — secondary bug
suggesting decompiler/reflection has a selector-arity issue.

### 3. `methodDict` doesn't see test methods (the showstopper)

For every test class:

```
DIAG-EMPTY: allTestSelectors=0 for AIAstarTest
DIAG-EMPTY: allSelectors size=58
DIAG-EMPTY: test* in allSelectors=8
DIAG-EMPTY: first5=#(#testSimpleWeightedGraphBacktracking ...)
DIAG-EMPTY: methodDict size=9
DIAG-EMPTY: test* in methodDict=0
```

- `allSelectors` (walks the class hierarchy) reports 58 selectors,
  of which 8 start with `test`.  Correct — those are the actual
  inherited + own selectors.
- `methodDict` reports size 9 — the class's own methods, including
  the 8 tests + a couple of helpers.
- But iterating methodDict and filtering for `test*` selectors
  finds **zero**.

That's the contradiction.  `allSelectors` finds them via the
class hierarchy walk; `methodDict` iteration doesn't.  Either:

- `methodDict` iteration is broken (the underlying primitive
  returns wrong selectors), OR
- Selector identity is broken (the `test*` Symbols in methodDict
  hash differently than the ones the test discovery code creates), OR
- `methodDict` keys aren't actually Symbols in our image

Without working test discovery, `SUnitRunner>>allTestSelectors`
returns empty, so each class runs 0 tests.

## What this means for the VM

The micro-benchmarks (fib, sieve, tinyBench) don't exercise:
- Reflection / `methodDict` iteration
- Symbol-table operations across realistic class hierarchies
- Character classification / Unicode tables
- File I/O selectors past the simplest paths

So we've been "as fast as Cog" on workloads that systematically
miss the parts of the VM that don't actually work.

## Root cause traced via probes (2026-05-28)

A series of probe images (`/tmp/probe_initN.st`) narrowed the bug
chain.  All three SUnit-blocking symptoms reduce to ONE root cause:
**`SparseLargeTable>>size` (or `at:`) is broken in our VM.**

The dependency chain:

1. **`SparseLargeTable>>size`** — VM either crashes or returns a
   wrong value.  Probe stops emitting output after `gc class =
   SparseLargeTable` and before the next `gc size = ...` line,
   suggesting `size` throws an error that the on:do: handler
   can't catch (or it hangs and gets killed at the SDL2 display
   timeout).

2. **`Unicode class>>isLetter:`** uses `(GeneralCategory at:
   charCode + 1)`.  GeneralCategory is a `SparseLargeTable` class
   var.  With the table broken, this returns wrong values (or
   the early-exit `index > GeneralCategory size ifTrue: [^false]`
   path fires unconditionally and returns false for everything).

3. **`Character>>isLetter`** → `self characterSet isLetter: self`
   → `Unicode isLetter: self` — always returns false on our VM.

4. **`Symbol>>numArgs`** checks `firstChar isLetter` first.  With
   `isLetter` broken, falls through to the "not-a-valid-selector"
   branch and returns `-1` for everything.

5. **`TestCase class>>allTestSelectors`** filters by
   `numArgs isZero`.  Returns `false` for every selector (`-1` ≠
   `0`), so the result is empty.

6. **Every test class runs 0 tests.**

Probe data confirming the chain:
- `$t characterSet = Unicode` ✓ (both VMs)
- `Unicode global = Unicode` ✓
- `GeneralCategory class = SparseLargeTable` ✓
- `gc size = 917632` ✓ (both VMs — `size` works fine)
- `gc at: 117` returns **5** (Cog) vs **0** (ours) ❌ — the actual bug
- `gc at: 98`  returns **5** (Cog) vs **0** (ours) ❌
- `(gc instVarAt: 2) = 917632` ✓ — slot reads work
- `base offset (instVarAt: 1) = SmallInteger` ✓ on both — layout matches
- `$t isLetter = false` (ours) vs `true` (Cog) — downstream
- `#testFoo numArgs = -1` (ours) vs `0` (Cog) — downstream

## True root cause: `SparseLargeTable>>at:` returns defaultValue (0)

The `at:` method delegates to `noCheckAt:`:

```smalltalk
noCheckAt: index
    | chunkIndex t |
    chunkIndex := index - base // chunkSize + 1.
    (chunkIndex > self basicSize or: [chunkIndex < 1]) ifTrue: [^ defaultValue].
    t := self basicAt: chunkIndex.
    t ifNil: [^ defaultValue].
    ^ t at: (index - base + 1 - (chunkIndex - 1 * chunkSize))
```

SparseLargeTable is **indexable with fixed slots** (instSpec format
3 — `IndexableWithFixed`).  The four named inst vars (base, size,
chunkSize, defaultValue) occupy fixed slots; the chunk pointers live
in the object's indexable slot area.

Our VM returns 0 (defaultValue), meaning one of:
- `self basicSize` returns wrong value (likely too small or 0)
- `self basicAt: chunkIndex` returns nil for valid indices

Both depend on our `Primitives.cpp` handling of `IndexableWithFixed`
format correctly — specifically computing the indexable slot count
as `slotCount - fixedFields`.  Other primitives we already audited
(primitiveStringReplace at Primitives.cpp:16319) get this right.
The `basicAt:`/`basicSize` family may have a bug specifically for
this format.

## Next step

Add a 1-line probe (in `Interpreter::primitiveSize` or
`primitiveBasicAt`) that logs receiver classIndex + slotCount +
fixedFields + result when the receiver is a SparseLargeTable.
Compare against Cog's expected output (basicSize ≈ 917632 /
chunkSize ≈ 64 → ~14 K chunks).

## Root cause and fix (2026-05-28)

**Bug:** JIT-compiled `basicSize` returned the source-level fallback
`^ 0` for any IndexableWithFixed/Weak (fmt 3/4/5) receiver AND for any
receiver whose header used the overflow slot-count encoding
(slotCount-byte = 0xFF, real count in the word before the header).

Both gaps were present in **two** JIT paths:

1. `stencil_primSize` (stencils.cpp:3786-3795) — fmt 3/4/5 branch
   fell through to bytecode body.
2. `emitPrimProlog_arm64` for prim 62 (AsmjitT1.cpp:2058-2103) — only
   handled fmt 2 / 10-11 / 16-23 directly, bailed on slotCount==255
   overflow header, and treated `fail` as "fall through to bytecode
   body" (= `^ 0`).

For SparseLargeTable, BOTH conditions were true: fmt=3 AND overflow
header (897 indexable chunks + 4 fixed = 901 slots > 254).
basicSize JIT-returned 0.  Downstream:
- `noCheckAt:` checked `chunkIndex > self basicSize` → `1 > 0` true →
  returned defaultValue (0) for every Unicode lookup.
- `Unicode>>isLetter:` indexed GeneralCategory → got 0 → returned false.
- `Character>>isLetter` → false for every char.
- `Symbol>>numArgs` checked firstChar.isLetter → false → returned -1.
- `OpalCompiler` arity check (numArgs vs bytecoded args) → mismatch →
  raised "Attempting to assign selector with wrong number of
  arguments." for every `compile:classified:` call.
- SUnit test discovery filtered by `numArgs isZero` → empty for every
  class → 0 tests run.

**Fix** (commit-pending):
- New runtime helper `jit_rt_primsize_ptr` (JITRuntime.cpp).  Mirrors
  the existing `jit_rt_primat_ptr` class-lookup for fmt 3/4/5; for
  fmt 9 returns slotCount directly.
- Plumbed through helpers struct (JITCompiler.hpp), extract_stencils.py
  (hole ID 20), JITCompiler.cpp (4 patch sites).
- stencil_primSize fmt 3/4/5 branch now calls the helper instead of
  falling through to `^ 0`.
- asmjit T1 prim 62 prologue extended: reads overflow word for
  slotCount==255 receivers, handles fmt 9 inline (size=slotCount),
  calls the new helper for fmt 3/4/5.

Also extended `jit_rt_array_prim`'s primKind=16 branch to handle fmt
3/4/5 (the IC-shortcut path); not strictly required for the chunk
chain but closes the same gap if reached via the IC dispatch.

**Verified:**
`gc at: 117 = 5` (was 0); `$t isLetter = true` (was false);
`#testFoo numArgs = 0` (was -1).  Chain healed end-to-end.

## Original narrowing — JIT inline Array>>at: bug (superseded)

Further probes (probe_initN.st, iterations 10-13) collapsed the
chain to a single root cause:

**`Array>>at:` returns 0 on our VM under JIT.  PHARO_NO_JIT=1
fixes everything.**

Reproduction (probe13 on `/tmp/harness/Pharo-probe13.image`):

```
Operation                                     JIT off       JIT on             Cog
gc basicAt: 1 (returns 1024-slot Array)       Array         Array              Array
chunk size                                    1024          1024               1024
chunk at: 1                                   1 OK          hang/crash FAIL    1
chunk at: 98                                  5 OK          no output  FAIL    5
chunk instVarAt: 98                           5 OK          no output  FAIL    5
```

The probe stops emitting output after `chunk size = 1024` with JIT
on.  The `chunk at: 1` call must be triggering a silent crash or
hang — not catchable by `on: Error do:` (which suggests SIGSEGV
or infinite loop, not a Smalltalk exception).

**Likely culprits:**

1. `stencil_primAt` (stencils.cpp:3614) — the JIT inline Array#at:
   stencil.  Line 3624 reads `slotCount = (header >> 56) & 0xFF`
   and handles overflow at 3625-3630 for slotCount==255.  Our
   1024-slot Array DOES have overflow header, so it should hit
   the overflow path.  Possible bugs:
   - Overflow word read at `rcvr.bits - 8` if `rcvr.bits` doesn't
     point at the header for overflow objects in our scheme
   - Slot pointer `rcvr.bits + 8` if overflow alignment is wrong
2. asmjit T1 inline `at:` emit (around AsmjitT1.cpp:4356) — may
   bypass the stencil entirely for plain-Array receivers.

**Why this matters:**
This bug breaks every Pharo program that uses Arrays with >254
slots — which is most non-trivial programs.  Including:
- `SparseLargeTable` (which uses 1024-slot chunks → blocks all
  Unicode classification → blocks all SUnit)
- Any indexable collection past the basic case
- Lots more

This is THE next bug to fix.  The audit-gap work we've been
chasing is dwarfed in impact by this one JIT correctness issue.

**Diagnostic approach:**

```bash
# Confirm the bug
cp /tmp/harness/Pharo-probe13.image /tmp/test.image
PHARO_NO_JIT=1 ./build/test_load_image /tmp/test.image      # works
                ./build/test_load_image /tmp/test.image      # fails

# lldb attach + breakpoint on stencil_primAt or the asmjit emit
# Compare the slotCount it computes against the actual 1024.

# Alternative: add a printf in stencil_primAt that fires for
# slotCount >= 255 (overflow path).  Run probe13 and inspect.
```

## Next-step priorities

1. **Fix `SparseLargeTable>>size`** — single root cause for the
   entire SUnit blockage.  Probable culprits:
   - `SparseLargeTable` uses an arrayed slot layout that needs a
     specific primitive (perhaps `primitive 70` or `primitiveSize`
     for non-indexable formats).
   - One of our prims returns the wrong slot count for
     `IndexableWithFixed` or similar format.
   - Investigate via lldb: attach, breakpoint on
     `Interpreter::primitiveSize` (or whichever is dispatched for
     `SparseLargeTable>>size`), compare result to expected ~17 K
     (Unicode general-category table covers BMP range).

2. **The "selector with wrong number of arguments" error.**
   Separate but related — appears in scheduler patch, timeout
   patch, and source decompilation paths.  Likely a method-header
   arity-encoding bug.

3. **`FileReference>>#lines`** — minor; harness has fallback.

## How to reproduce

```bash
# Stage curated class list
cp scripts/pharo-headless-test/test_classes.txt /tmp/sunit_test_classes.txt

# Clone image, inject runner via Cog
cp /tmp/harness/Pharo.image /tmp/harness/Pharo-sunit.image
cd /tmp && /tmp/harness/pharo --headless /tmp/harness/Pharo-sunit.image eval --save \
  "'$PWD/scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn.
   '$PWD/scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn"

# Clean stale completion marker (otherwise startup hook short-circuits)
rm -f /tmp/sunit_run_completed.txt /tmp/sunit_test_*.txt /tmp/sunit_batch*.txt

# Run our VM
./build/test_load_image /tmp/harness/Pharo-sunit.image > /tmp/sunit-run.out 2>&1

# Results
cat /tmp/sunit_test_results.txt      # summary
cat /tmp/sunit_test_detail.txt       # per-test detail
cat /tmp/sunit_batch_outer_err.txt   # outer errors
```
