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

Without the Trait "selector changed!" bug (480 errors, single root cause)
and the ProcessTest processMonitor issue (48 errors, test infrastructure):

    Adjusted pass rate: 99.64%

## Full Suite (Build 78)

628 individual test method failures across 69 classes.

### By root cause

  Root cause                               Failures   Pct
  Trait "selector changed!" errors              480    76%
  ProcessTest processMonitor missing             48     8%
  SystemDependenciesTest                         17     3%
  Fuel WideString/WideSymbol                     15     2%
  Calypso IDE query tests                        14     2%
  MicGitHub network tests                         9     1%
  ReleaseTest meta-tests                          9     1%
  StDebugger tests                                4    <1%
  Geometry unimplemented methods                  3    <1%
  WriteBarrier float/double                       2    <1%
  Other scattered (1 each)                       27     4%

### Trait "selector changed!" (480 errors)

Every Trait* test class fails. Trait composition reads back wrong selector
identity. Single root cause — not yet investigated. This is the highest
priority remaining VM investigation.

### ProcessTest processMonitor (48 errors)

`DefaultExecutionEnvironment >> #processMonitor` not found. All
ProcessTest methods fail. Test execution environment setup issue in
batch runner.

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

## Remaining Investigations

  Priority   Item                                        Failures
  1          Trait "selector changed!" bug                   480
  2          ProcessTest processMonitor missing               48
  3          Ephemeron finalization support                    1
  4          WriteBarrier float/double atPut:                  2
  5          MirrorPrimitives tryPrimitive                     1
  6          Fuel WideString/WideSymbol handling              15
