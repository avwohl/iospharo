# Test Coverage Work Document

## Problem Statement

Our test suite (`scripts/run_sunit_tests.st`) runs **577 of 2,046** non-abstract
TestCase subclasses in the Pharo 13 image — only **28% class coverage**.
The "98% pass rate" is accurate for the classes we run (13,040/13,075) but
misleading because we never tested the other 1,478 classes (~15,000 tests).

Tim (first external TestFlight user) found failures by running DrTests inside
the image, which discovers ALL test classes. Several of his failures are in
classes we DO run — meaning they either regressed or behave differently
when run interactively vs. in our batch runner.

## Tim's Reported Failures

Status key: PASS = passes on our Mac VM, FAIL = fails, ERROR = crashes,
INVESTIGATING = not yet tested, KNOWN = known upstream issue

  ByteSymbolTest >> #testAs                          PASS (on Mac)
  ByteSymbolTest >> #testNewFrom                     PASS (on Mac)
  ByteSymbolTest >> #testReadFromString              FAIL "Got 0 instead of 1"
  ByteSymbolTest >> #testAsFileLocatorOrReferenceReturn  (in StringTest, not ByteSymbolTest)
  ProcessTest >> #testResumeAfterBCR                 ERROR (needs TestExecutionEnvironment processMonitor)
  WeakIdentityKeyDictionaryTest >> #testWeakKeyDictionary  METHOD NOT FOUND (wrong selector?)
  WeakKeyDictionaryTest >> #testClearing             FAIL "Got 1001 instead of 1" (weak refs not collected)
  BehaviorTest >> #testAllReferencesTo               PASS
  ProtoObjectTest >> #testFastPointersTo             PASS
  RecursionStopperTest >> #testThreadSafe            FAIL "Assertion failed"
  OCSpecialSelectorTest >> #testUnoptimised...       FAIL "Got OCOpalExamples instead of #valueToTest"
  AllocationTest >> #testOneGWordAllocation          SKIPPED in our suite (OOM)
  OutOfMemoryTest >> #testErrorProducedBy            NOT IN SUITE

  ProtoObject >> #basicNew PrimitiveFailed           Tim's debugger screenshot shows
                                                     Array basicNew: failing (prim 71)

  Spotter/browse/debug broken in recent builds       Possibly related to symbol equality

### Analysis of Confirmed Failures

1. ByteSymbolTest >> #testReadFromString
   - "Got 0 instead of 1" — likely ReadStream/parsing issue

2. WeakKeyDictionaryTest >> #testClearing
   - "Got 1001 instead of 1" — weak references not being collected by GC
   - Our GC may not be mourning ephemerons/weak refs properly

3. RecursionStopperTest >> #testThreadSafe
   - Multi-threaded recursion stopper test fails
   - May be timing/priority related

4. OCSpecialSelectorTest >> #testUnoptimisedValueSpecialSendsMessageCapturesSend
   - "Got an OCOpalExamples instead of #valueToTest"
   - Special selector optimization issue — #value send not being captured

## Test Class Coverage Gap

  In our suite:    577 classes (~13,000 tests)
  In image:      2,046 classes (~28,000 tests)
  Missing:       1,478 classes (~15,000 tests)

### Largest Missing Packages

  Refactoring-Transformations-Tests          105 classes
  Spec2-Tests                                 74 classes
  General-Rules-Tests                         73 classes
  Calypso-SystemQueries-Tests                 61 classes
  Kernel-Tests (partial)                      59 classes
  OpalCompiler-Tests (partial)                54 classes
  Microdown-Tests                             51 classes
  Fuel-Core-Tests                             46 classes
  Spec2-Backend-Tests                         45 classes
  Ring-Core-Tests                             34 classes
  Slot-Tests                                  33 classes
  Traits-Tests                                31 classes
  Monticello-Tests                            28 classes
  ClassParser-Tests                           26 classes
  Refactoring-Core-Tests                      25 classes
  Renraku-Tests                               25 classes
  HeuristicCompletion-Tests                   25 classes
  Zinc-Tests                                  24 classes
  SUnit-Tests                                 24 classes
  NewTools-Debugger-Tests                     24 classes
  UnifiedFFI-Tests (partial)                  24 classes
  AST-Core-Tests                              23 classes
  Roassal-*-Tests                             ~80 classes (charting/visualization)
  Morphic-Tests                               21 classes
  System-Time-Tests                           21 classes
  Network-Tests                               15 classes
  ... and 150+ more packages

### Categories of Missing Tests

  GUI/Morphic tests     — may need display, may crash headless
  Refactoring tests     — pure Smalltalk, should mostly work
  Calypso/IDE tests     — browser/tools tests, may need UI
  Spec2 tests           — UI framework tests
  Roassal tests         — visualization, may need display
  Debugger tests        — may need interactive debugger
  Network/Zinc tests    — HTTP/WebSocket tests, need network
  Fuel tests            — serialization, should work
  Monticello tests      — version control, may need git/network
  General-Rules         — lint rules, pure computation
  Microdown tests       — markdown parsing, should work

## Full Test Run Status

A full run of all 2,046 classes is in progress.
Results will be at: /tmp/all_test_results.txt and /tmp/all_test_detail.txt

## Action Items

1. Complete full test run and analyze results
2. Categorize all failures as:
   - VM bug (we need to fix)
   - Upstream bug (fails on reference VM too)
   - Environment issue (needs UI/network/etc.)
   - Timing/flaky
3. Fix confirmed VM bugs from Tim's list
4. Update run_sunit_tests.st to include ALL discovered classes
5. Update README with accurate coverage numbers
6. Set up regression testing that catches new failures
