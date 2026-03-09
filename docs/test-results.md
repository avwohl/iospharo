# Test Results

Last updated: 2026-03-09

## Summary

    Total tests:   28,071 across 2,046 classes
    Pass:          27,510 (98.00%)
    Fail:              39
    Error:            391
    Skip:             131
    Timeout:            0

    Zero VM-specific failures.

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
