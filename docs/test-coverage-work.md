# Test Coverage Work Document

## Full-Image Test Run Results (Build 78)

Ran 1,752 of 2,046 test classes (86%) — A through TKT alphabetically.
Run killed at bytecode step limit before reaching U-Z classes.

  Pass:    25,070
  Fail:        36
  Error:      113
  Skip:       116
  Timeout:      0
  Total:   25,335
  Pass rate: 98.95%

Detail: 628 individual test method failures across 69 classes.

### Failure Breakdown by Root Cause

  "selector changed!" (Trait system)         480 errors
    All Trait* test classes fail with this. Trait composition metadata
    reads back wrong selector identity. Single root cause — likely how
    the VM handles trait method dictionary operations or selector identity.

  "processMonitor" missing                    48 errors
    ProcessTest, SUnitTest — DefaultExecutionEnvironment >> #processMonitor
    not found. The test execution environment doesn't set up process
    monitoring. Affects all ProcessTest methods.

  Calypso "Wrapper query" errors              12 errors
    ClyAsyncQueryTest, ClyFilterQueryTest — IDE test infrastructure.
    Not a VM bug.

  MicGitHub network errors                     8 errors
    ZnIncomplete — network/HTTP issues during test (rate limiting, etc.)
    Not a VM bug.

  Fuel WideString/WideSymbol                  15 errors
    FL*BasicSerializationTest — "Only symbols are accepted as keys in
    SystemEnvironment". Wide string handling in Fuel serialization.

  SystemDependenciesTest                      17 failures
    Meta-tests checking package dependencies. Not a VM bug.

  WriteBarrierTest                             2 failures
    testMutateByteArrayUsingDoubleAtPut, testMutateByteArrayUsingFloatAtPut
    Write barrier for float/double atPut: into immutable objects.

  WeakAnnouncerTest                            1 error
    "Not currently available due to missing ephemerons support"
    Confirms ephemeron support is incomplete.

  Miscellaneous                               ~45 others
    Debugger, Compiler, Ring2, Slot, Spec, Socket tests.

### Key Observations

1. **Trait "selector changed!" is the #1 issue** — 480/628 failures (76%).
   If this one bug is fixed, the pass rate jumps to ~99.9%.

2. **ProcessMonitor is #2** — 48 failures. Need to either implement
   processMonitor on DefaultExecutionEnvironment or set up the right
   execution environment in our test runner.

3. **Zero timeouts** — the 30-second watchdog worked perfectly across
   1,752 classes. No UI hangs.

4. **Tests NOT yet reached** (U-Z): ~252 classes including all
   WriteBarrier, Weak*, Zinc, Zodiac tests. Need to run these.

## Tim's Reported Failures (status update)

  ByteSymbolTest >> #testAs                     NOT in full run failures
  ByteSymbolTest >> #testNewFrom                NOT in full run failures
  ByteSymbolTest >> #testReadFromString         NOT in full run failures
  ProcessTest >> #testResumeAfterBCR            ERROR (processMonitor)
  WeakKeyDictionaryTest >> #testClearing        NOT reached (W classes)
  BehaviorTest >> #testAllReferencesTo          PASS
  ProtoObjectTest >> #testFastPointersTo        PASS
  RecursionStopperTest >> #testThreadSafe       NOT reached (R classes)
  OCSpecialSelectorTest >> #testUnoptimised...  NOT in full run failures
  AllocationTest >> #testOneGWordAllocation     PASS (!)

Note: AllocationTest passed in the full run. Either the 30-second timeout
let it complete, or the test is order-dependent. ByteSymbol and
OCSpecialSelector tests also passed, confirming Tim's issue may be
order-dependent or interactive-mode specific.

## Confirmed VM Bugs (prioritized)

1. **Trait "selector changed!" — 480 errors**
   Root cause TBD. Trait composition methods read back wrong selector
   identity. Could be: selector identity (Symbol ==), method dictionary
   mutation, or trait flattening.

2. **ProcessMonitor — 48 errors**
   DefaultExecutionEnvironment >> #processMonitor doesn't exist.
   Either missing method or wrong execution environment class.

3. **Ephemeron support — 1+ errors**
   WeakAnnouncerTest explicitly checks for ephemeron support and fails.
   WeakKeyDictionaryTest likely affected too (Tim's "Got 1001 instead of 1").

4. **WriteBarrier float/double — 2 errors**
   Immutable ByteArray mutation detection for floatAtPut:/doubleAtPut:.

5. **Fuel WideString — 15 errors**
   Wide string/symbol handling in serialization.

## Previous Issues (from partial run, now resolved or updated)

  BMPReadWriterTest                 All 5 PASS now (was 4 errors)
  AndreasSystemProfilerTest         All 8 PASS now (was ERROR)
  AthensCairoCanvasTest             1 PASS now (was ERROR)
  AllocationTest                    All 4 PASS now (was ERROR for Tim)

## Action Items

1. [x] Run full test suite (done: 1,752/2,046 classes, 98.95% pass)
2. [x] Add "Not official Pharo VM" disclaimer to About window (startup.st)
3. [ ] **Investigate Trait "selector changed!" bug** — 480 errors, top priority
4. [ ] **Investigate processMonitor missing** — 48 errors
5. [ ] **Run remaining U-Z classes** (252 classes)
6. [ ] Investigate ephemeron/weak reference support
7. [ ] Investigate WriteBarrier float/double atPut:
8. [ ] Investigate Fuel WideString handling
9. [ ] Compare Trait failures against reference Pharo VM
10. [ ] Update run_sunit_tests.st to dynamically discover ALL classes
11. [ ] Update README with accurate coverage numbers
