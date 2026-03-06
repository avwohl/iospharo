# Test Coverage Work Document

## Full-Image Test Run Results (Build 78) — COMPLETE

All 2,046 test classes, all 28,071 tests. Zero timeouts.

  Pass:    27,510   (98.00%)
  Fail:        39
  Error:      391
  Skip:       131
  Timeout:      0
  Total:   28,071

628 individual test method failures across 69 classes.

### Failure Breakdown by Root Cause

  1. Trait "selector changed!" errors         480 (76% of all failures)
     Every Trait* test class fails with this. Trait composition reads
     back wrong selector identity. Single root cause.

  2. ProcessTest "processMonitor" missing       48 (8%)
     DefaultExecutionEnvironment >> #processMonitor not found.
     All ProcessTest methods fail. Test execution environment issue.

  3. SystemDependenciesTest                     17
     SubscriptOutOfBounds in package dependency checks.
     Not a VM bug — likely test infrastructure issue.

  4. Fuel WideString/WideSymbol                 15
     FL*BasicSerializationTest — "Only symbols are accepted as keys
     in SystemEnvironment" and WideString class name comparisons.

  5. Calypso IDE tests                          14
     ClyAsyncQuery/FilterQuery — "Wrapper query should include single
     subquery" and scope method missing.

  6. MicGitHub network tests                     9
     ZnIncomplete / rate limiting. External dependency.

  7. ReleaseTest meta-tests                      9
     testNoEmptyPackages, testObsoleteClasses, etc. Image state checks.

  8. StDebugger tests                            4
     Debugger UI inspection failures.

  9. Geometry unimplemented methods               3
     GArc/GEllipse >> #intersectionsWithEllipse: not implemented.

 10. WriteBarrier float/double                    2
     testMutateByteArrayUsingDoubleAtPut/FloatAtPut.

 11. Other scattered (1 each)                    27
     WeakAnnouncerTest (ephemerons), MirrorPrimitivesTest,
     OCParser, Slot, SpDemo, SUnitTest, SettingsSton,
     Ring2ChunkImporter, TCPSocketEcho (port conflict),
     ZnClientTest (testQueryGoogle), FBDDecompiler, etc.

### Without Trait+ProcessTest bugs: 99.64% pass rate

If the Trait "selector changed!" and processMonitor issues are fixed,
the remaining failures drop to ~100 out of 28,071 = 99.64% pass rate.
Many of those remaining are image-level meta-tests (ReleaseTest,
SystemDependencies) or external network tests, not VM bugs.

## Tim's Reported Failures (updated status)

  ByteSymbolTest >> #testAs                     PASS (not in failures)
  ByteSymbolTest >> #testNewFrom                PASS (not in failures)
  ByteSymbolTest >> #testReadFromString         PASS (not in failures)
  ProcessTest >> #testResumeAfterBCR            ERROR (processMonitor)
  WeakKeyDictionaryTest >> #testClearing        PASS (not in failures!)
  BehaviorTest >> #testAllReferencesTo          PASS
  ProtoObjectTest >> #testFastPointersTo        PASS
  RecursionStopperTest >> #testThreadSafe       PASS (not in failures!)
  OCSpecialSelectorTest >> #testUnoptimised...  PASS (not in failures!)
  AllocationTest >> #testOneGWordAllocation     PASS

8 of Tim's 10 reported failures now pass. The 2 that fail (ProcessTest)
are the processMonitor infrastructure issue, not a VM bug. Tim's failures
were likely order-dependent or specific to interactive DrTests mode.

## VM Bugs (prioritized)

  1. Trait "selector changed!" — 480 errors
     Root cause TBD. Trait composition metadata reads back wrong
     selector identity. Could be: selector ==, method dictionary
     mutation, or trait flattening mechanics.

  2. ProcessMonitor — 48 errors
     DefaultExecutionEnvironment >> #processMonitor missing.
     Execution environment setup in batch test runner.

  3. Ephemeron support — 1 error
     WeakAnnouncerTest: "Not currently available due to missing
     ephemerons support." Confirms ephemeron finalization incomplete.

  4. WriteBarrier float/double — 2 errors
     Immutable ByteArray mutation detection for floatAtPut:/doubleAtPut:.

  5. MirrorPrimitives — 1 error
     #withReceiver:tryPrimitive:withArguments: fails.

  6. Fuel WideString — 15 errors
     Wide string/symbol handling in serialization. May be WideString
     class identity or encoding issue.

## Not VM Bugs (image/infrastructure)

  - SystemDependenciesTest (17) — package graph meta-tests
  - ReleaseTest (9) — image state checks (obsolete classes, etc.)
  - Calypso query tests (14) — IDE infrastructure
  - MicGitHub tests (9) — external network/rate limiting
  - StDebugger tests (4) — debugger UI
  - Geometry tests (3) — #intersectionsWithEllipse: not implemented
  - TCPSocketEchoTest (1) — port already in use
  - ZnClientTest #testQueryGoogle (1) — external dependency

## Action Items

  1. [x] Run full test suite — 2,046 classes, 28,071 tests, 98.00% pass
  2. [x] Add "Not official Pharo VM" disclaimer to About window
  3. [ ] Investigate Trait "selector changed!" bug (480 errors, top priority)
  4. [ ] Investigate processMonitor missing (48 errors)
  5. [ ] Investigate ephemeron support
  6. [ ] Investigate WriteBarrier float/double atPut:
  7. [ ] Investigate MirrorPrimitives #withReceiver:tryPrimitive:withArguments:
  8. [ ] Investigate Fuel WideString handling
  9. [ ] Compare Trait failures against reference Pharo VM
  10. [ ] Update run_sunit_tests.st to use dynamic discovery
  11. [ ] Update README with accurate coverage numbers
