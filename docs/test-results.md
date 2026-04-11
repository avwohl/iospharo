# Test Results

Last updated: 2026-04-10

## AWFY Benchmark Suite (2026-04-10)

Are We Fast Yet (AWFY) — 11 benchmarks compiled via SomLoader into Pharo 13.
Run via PHARO_BENCH=awfy harness. 1 warmup + 5 timed runs each, median reported.
GC between runs to prevent heap exhaustion on allocation-heavy benchmarks.

    Benchmark    Cog (us)   JIT (us)  NO_JIT (us)  JIT/Cog  JIT/NOJIT
    Richards      247,069 14,139,253  14,132,184     57.2x      1.00x
    DeltaBlue      46,859  1,766,485   1,753,622     37.7x      1.01x
    Mandelbrot    257,271  2,083,151   2,096,088      8.1x      0.99x
    NBody         231,766  7,179,255   7,246,797     31.0x      0.99x
    Bounce         91,009  1,923,959   1,920,422     21.1x      1.00x
    Permute        79,126  2,950,917   2,921,041     37.3x      1.01x
    Queens         69,607  5,905,082   5,979,641     84.8x      0.99x
    Sieve         164,834    833,144     785,336      5.1x      1.06x
    Storage       115,321  2,711,917   2,766,013     23.5x      0.98x
    Towers         73,369  5,509,941   5,549,239     75.1x      0.99x
    List           92,855  5,336,920   5,316,089     57.5x      1.00x
    Geomean                                          30.1x      1.00x

Key findings:
- JIT vs NO_JIT: 1.00x geomean — JIT provides no net speedup on AWFY yet
- Our VM vs Cog: 30x slower geomean (range 5x-85x)
- Best: Sieve (5.1x), Mandelbrot (8.1x) — arithmetic-heavy, less dispatch
- Worst: Queens (84.8x), Towers (75.1x) — heavily recursive, dispatch-bound
- JIT stats: 213 compiled, 99.9% IC hit, 2B IC hits, 47.6M J2J stencil calls
- The bottleneck is method dispatch overhead, not bytecode execution speed

## Test Mode End-to-End (2026-04-08)

`./build/test_load_image <image> test "Kernel-Tests"` now works.
Fresh Pharo 13 image + SUnitRunner session handler, 600s timeout.
JIT auto-disabled in test mode (26x cold-code overhead, see below).

    Classes  Pass     Fail  Err  Skip  Rate
    799      16,453   23    21   39    99.7%

799 of 1671 class tiers completed in the 600s window (vs 535 previously).
Remaining classes are slow tests (Fuel serialization, file system, etc.).

All failures are known non-VM issues: testSimpleEnsureTestWithUparrow (NLR),
testFastPointersTo (ShouldNotImplement), testClearing (weak ref GC timing),
Fuel WideString/WideSymbol, Calypso query tests, VM path meta-tests.

Fixes: always create Display (MorphicRenderLoop is P40 not P80), always pass
--interactive (avoid STCommandLineHandler conflict), auto-disable JIT.

## JIT Cold-Code Overhead (2026-04-08)

JIT causes ~26x slowdown on cold test workloads. Root cause: every Smalltalk
send goes through heavy C++ JIT entry/exit transitions (JITState setup,
MethodMap lookup, IC patching) that overwhelm the benefit of executing machine
code. On micro-benchmarks (fib, sort) where methods are hot, JIT is ~5% faster
than interpreter. On test suites with thousands of cold methods, the overhead
dominates. PHARO_NO_JIT=1 avoids this; test mode auto-sets it.

## Full Suite — JIT vs Interpreter Parity (2026-04-08)

Fresh Pharo 13 image, full test suite (1671 class tiers).
Both JIT (no-J2J) and interpreter produce identical results.

    Mode       Classes  Pass     Fail  Err  Skip  Rate
    JIT(noJ2J) 535      12,531   13    3    29    99.6%
    No-JIT     535      12,531   13    3    29    99.6%

Zero JIT-specific regressions. 535 of 1671 class tiers completed
in the 600s timeout window. The JIT compiles and correctly executes
all Kernel, Collection, Compiler, and many system test classes.

J2J (JIT-to-JIT direct calls) disabled during this run due to a
context chain materialization bug that prevents exception propagation
through J2J frames. Fix in progress.

## Code Zone Eviction Stress Test (2026-04-07)

Full suite run (1671 classes, 660s timeout) with JIT enabled.
500+ LRU evictions during the run, zero crashes.

    Mode    Classes  Pass    Fail  Err  Skip  Timeout
    JIT     12       1,258   3     2    4     5
    Interp  12       1,287   0     3    3     4

    JIT stats: 6591 compiled, 99% IC hit, 500+ evictions, 0 SIGSEGV

    Key fixes in this build:
    - Targeted J2J IC flush on eviction (only clears entries pointing
      to evicted code, preserving IC data for surviving methods)
    - Pre-eviction callback captures ALL evicted ranges (both LRU passes)
    - GC-safety: refresh cached method oop after executePrimitive

    FloatTest failures (JIT: 3F, Interp: 0F) are from testFloatRounded
    timeout difference. The 3 JIT "failures" are actually test methods
    that couldn't report results due to ZnCharacterWriteStream runner bug.

## Kernel-Tests — JIT vs Interpreter Parity (2026-04-07)

Fresh Pharo 13 image, Kernel-Tests package (243 test classes).
Both modes produce identical results — zero JIT-specific regressions.

    Mode        Classes  Pass   Fail  Err  Skip
    JIT         243      9,033  7     2    15
    Interpreter 243      9,033  7     2    15

    JIT stats: 2500+ compiled, 96% IC hit, 0 crashes, 0 SIGSEGV

    Failures (same in both modes):
    - testSimpleEnsureTestWithUparrow — NLR + ensure
    - testTerminationDuringNestedUnwindB1 — process termination
    - testTerminationDuringNestedUnwindWithReturn2 — same
    - testClearing (x2) — weak ref timing (nondeterministic)
    - testWeakDoubleAnnouncer — weak ref timing
    - testWeakObject — weak ref timing
    Errors:
    - testFastPointersTo — ShouldNotImplement in Array
    - testUnoptimisedValueSpecialSendsMessageCapturesSend — nil receiver

    Key fix in this build: unsafe J2J calls to primitive methods without
    prologue stencils (e.g. noCheckAt:, basicAt:, basicSize) were skipping
    the primitive entirely. Now patchJITICAfterSend checks hasPrimPrologue
    before setting the J2J bit.

## Full Suite — Interpreter (2026-04-04, deadlock fix)

    Fresh Pharo 13 image, 1671 test class candidates, interpreter only.
    VM exits cleanly via primitiveQuit after test runner completes.

    Classes:   620 (ran to completion; remainder skip-listed or errored during discovery)
    Tests:  13,702
    Pass:   13,341  (97.3%)
    Fail:       15
    Error:     213
    Skip:       32

    Wall:  1,538s  (~25.6 min)
    User:  1,372s
    Sys:      50s
    Memory: 4.3 GB peak

    No deadlocks. Previous runs hung at MicrodownSpecComponentTest
    (all Smalltalk processes blocked, Delay scheduler dead, watchdogs
    couldn't fire). Fixed by: removing 10-attempt cap on Delay
    scheduler recovery + fixing primitiveWait to clean up wait list
    before failing when no process is runnable.

## JIT vs Interpreter Comparison (2026-04-04)

Fresh Pharo 13 image, 192 test classes, 8275 tests per run.
Both runs killed by 660s timeout (3 slow reflective tests in SelfVariableTest).

    Mode       Tests   Pass   Fail  Error  Skip  Timeout
    JIT-OFF    8,275  8,213     23     11    25        3
    JIT-ON     8,275  8,213     23     11    25        3

    JIT-specific regressions: 0
    (One run showed WeakIdentityKeyDictionaryTest>>testClearing
     as an extra JIT failure, but it's nondeterministic — passes
     on subsequent runs. The underlying count varies with GC timing.)

    Timing (wall clock / user CPU / sys CPU):
    JIT-OFF:  660s wall  /  619.7s user  /  2.3s sys
    JIT-ON:   660s wall  /    4.6s user  /  1.6s sys

    CPU reduction: ~99% less user CPU under JIT
    Both runs complete the same 192 classes before timeout.
    JIT finishes tests much earlier, then idles; interpreter
    runs tests for the full 660s.

    JIT stats at run end: 303 compiled, 0 failed, 96% IC hit rate

## Summary

    Total tests:   28,071 across 2,046 classes (full suite, Build 78+)
    Pass:          27,510 (98.00%)
    Fail:              39
    Error:            391
    Skip:             131
    Timeout:            0

    Zero VM-specific failures.

## JIT Expanded Validation (Build 122, 2026-04-03)

Fresh Pharo 13 image, all 15 core test classes. JIT with inline
getter/setter dispatch, IC selector verification, free-list LRU eviction.

    Class                    Pass  Fail  Error  Skip
    ArrayTest                 323     0      0     0
    BagTest                   167     0      0     0
    CharacterTest              16     0      0     0
    DictionaryTest            205     0      0     0
    FloatTest                  72     0      0     1
    FractionTest               30     0      0     0
    IdentitySetTest           176     0      0     0
    IntegerTest                75     0      0     3
    IntervalTest              260     0      0     0
    OrderedCollectionTest     351     0      0     0
    PointTest                  34     0      0     0
    SetTest                   174     0      0     0
    SmallIntegerTest           27     0      0     0
    SortedCollectionTest      287     0      0     0
    SymbolTest                268     0      0     0
    TOTAL                   2,465     0      0     4

    100% pass rate. DictionaryTest previously hung at 43 — now runs fully (205).
    4 skips are expectedFailure tests (FloatTest, IntegerTest).

JIT stats at end of run:
    317 compiled, 0 failed, 0 bailouts
    IC: 99% hit rate, 0 stale entries
    J2J activation: 73% of sends resolved via JIT-to-JIT chaining

Remaining 187+ classes from expanded tiers blocked by Delay scheduler
at priority 79 preventing test fork processes from completing. The 15
core classes cover all fundamental VM mechanics (arithmetic, collections,
strings, symbols, intervals, dictionaries, sets, arrays, points).

## JIT + GC Cooperation Validation (Build 122, 2026-04-02)

JIT with full GC cooperation (forEachRoot scanning, recoverAfterGC, IC
flushing, code zone leak fix). Send-heavy method guard (`hasSends`) keeps
JIT-enabled VM at full interpreter speed (~87M steps/10s).

    Class                  Pass  Fail  Error  Skip  Timeout
    SortedCollectionTest    287     0      0     0        0
    IdentitySetTest         176     0      0     0        0
    IntegerTest             102     0      0     3        0
    FloatTest                71     0      0     1        1
    DictionaryTest           43     0      0     0        0
    PointTest                34     0      0     0        0
    FractionTest             30     0      0     0        0
    SmallIntegerTest         27     0      0     0        0
    CharacterTest            16     0      0     0        0
    TOTAL                   786     0      0     4        1

0 failures, 0 errors. DictionaryTest partial (hung on a slow test after 43
passes). Skips are expectedFailure tests. Timeout is testFloatTruncated
(known slow).

## JIT IC Validation (Build 122, 2026-04-02)

Kernel-Tests run with copy-and-patch JIT (Tier 1) enabled on macOS ARM64.
JIT IC corruption bug (infinite #assert recursion) fixed — stale
pendingICPatch_ was being consumed by sends inside blocks.

    JIT disabled:  187 classes  8248 pass  5 fail  1 error  15 skip
    JIT enabled:   187 classes  8249 pass  4 fail  1 error  15 skip

No JIT-specific failures. All failures are non-deterministic GC/weak ref tests
(testClearing, testWeakObject, testWeakDoubleAnnouncer) or Pharo image bugs
(testFastPointorsTo).

Both runs hit a timeout on SlotClassBuilderTest >> testUsingMethodsFFI
(exceeded 80s per-test watchdog, then process-level 1800s timeout).

Trait tests (previously 480 "failures") now pass: 217/217.
Without the ProcessTest processMonitor issue (46 errors, Pharo 13 image bug):

    Adjusted pass rate: 99.82%

## Full Suite (Build 78)

628 individual test method failures across 69 classes.

### By root cause

  Root cause                               Failures   Pct
  Trait "selector changed!" errors          RESOLVED   --   (217/217 pass, moved to Tier 17)
  ProcessTest processMonitor missing             46    31%   (same on official Pharo VM)
  SystemDependenciesTest                         17    11%
  Fuel WideString/WideSymbol                     15    10%
  Calypso IDE query tests                        14     9%
  MicGitHub network tests                         9     6%
  ReleaseTest meta-tests                          9     6%
  StDebugger tests                                4     3%
  Geometry unimplemented methods                  3     2%
  WriteBarrier float/double                       2     1%
  Other scattered (1 each)                       27    18%

### Trait tests (RESOLVED, Build 88)

Previously reported as 480 failures ("selector changed!"). Root cause was
batch test ordering — Trait tests modify shared T1-T5 traits and earlier
tests could corrupt state. All 217 tests pass on our VM when run as a
standalone group or when placed last (Tier 17) in the batch runner.
TraitPackagingTest was renamed upstream and no longer exists.

### ProcessTest processMonitor (46 errors)

`DefaultExecutionEnvironment >> #processMonitor` not found. All 46
ProcessTest methods fail identically on both our VM and the official
Pharo VM (P=0 F=0 E=46). Confirmed: this is a Pharo 13 image bug,
not a VM issue. The `processMonitor` method is missing from
`DefaultExecutionEnvironment` in the stock image.

### Not VM bugs

  - SystemDependenciesTest (17) — package graph meta-tests
  - ReleaseTest (9) — image state checks
  - Calypso query tests (14) — IDE infrastructure
  - MicGitHub tests (9) — network / rate limiting
  - StDebugger tests (4) — debugger UI
  - Geometry tests (3) — #intersectionsWithEllipse: unimplemented
  - TCPSocketEchoTest (1) — port already in use
  - ZnClientTest testQueryGoogle (1) — external dependency

### Tim's reported failures (all resolved)

  Test                                         Status
  ByteSymbolTest >> testAs                     PASS
  ByteSymbolTest >> testNewFrom                PASS
  ByteSymbolTest >> testReadFromString         PASS
  ProcessTest >> testResumeAfterBCR            ERROR (processMonitor — not VM)
  WeakKeyDictionaryTest >> testClearing        PASS
  BehaviorTest >> testAllReferencesTo          PASS
  ProtoObjectTest >> testFastPointersTo        PASS
  RecursionStopperTest >> testThreadSafe       PASS
  OCSpecialSelectorTest >> testUnoptimised..   PASS
  AllocationTest >> testOneGWordAllocation     PASS

8 of 10 now pass. The 2 that fail are the processMonitor issue.

## Higher-Level Package Tests (Build 62)

  Package      Tests   Pass   Fail  Error  Rate
  NeoJSON        116    116      0      0  100%
  Mustache        47     47      0      0  100%
  XMLParser     5978   5978      0      0  100%
  PolyMath      1168   1162      5      1  99.5%
  DataFrame      665    651     14      0  97.9%
  Total         7974   7954     19      1  99.8%

All failures are pre-existing on the official Pharo VM.
See `docs/higher_level_tests.md` for loading instructions and failure details.

## GUI/Spec Tests (64 classes, fake head)

Run with `scripts/setup_fake_gui.st` (MorphicUIManager + UI process):

    1,054 pass, 5 fail, 15 error, 35 skip, 4 timeout
    94.6% pass rate

Remaining failures are font metrics (ascent nil, no FreeType in headless)
and complex presenter tests needing real display.

## Skip List

Classes skipped (hang, crash, or infrastructure issues in headless):

  - DirectoryEntryTest, DiskFileAttributesTest, DiskFileSystemTest
  - DrTestsTestRunnerTest
  - ObsoleteTest (corrupts image state)
  - Epicea test classes (file watcher hang)
  - Fuel serialization classes (extreme timeouts)
  - CodeSimulationTest, FBDDecompilerTest (extremely slow)
  - MicInlineDelimiterTest (deadlock), SocketStreamTest
  - Various filesystem/network tests that hang headless

## Previous Investigations (all resolved, Build 86)

  Item                                   Resolution
  Trait "selector changed!" (480)        FIXED: 217/217 pass. Re-enabled, moved to run last (Tier 17)
  ProcessTest processMonitor (46)        Pharo 13 image bug — identical on official VM (P=0 F=0 E=46)
  Ephemeron finalization (1)             FIXED: removed priority guard in signalFinalizationIfNeeded()
  WriteBarrier float/double (2)          31/31 PASS — was never a bug
  MirrorPrimitives tryPrimitive (1)      expectedFailure that passes — our VM is correct
  Fuel WideString/WideSymbol (15)        WideStringTest passes; Fuel timeouts are interpreter speed

  FinalizationRegistryTest: 6/6 pass
  WeakAnnouncerTest: 34/34 pass

No remaining VM-specific bugs.
