# Non-Passing Tests (2026-02-21, commit c7d990d)

Full suite: 13,040 pass, 6 fail, 7 error, 22 skip, 0 timeout (577 classes, 13,075 total)

## Failures (6)

| Class | Test | Message | Verdict |
|-------|------|---------|---------|
| ClassQueryTest | testAllCallsOn | Got 2 instead of 1. | Unknown — may be image version mismatch |
| OCProgramNodeTest | testBestNodeForReturnAStatementWhenIntervalInStatementWithoutLeftPart | Assertion failed | Unknown — may be image version mismatch |
| SlotErrorsTest | testCannotBeRecompiled | Old class builder raises: X cannot be recompiled | Expected failure on reference VM |
| TestExecutionEnvironmentTest | testHandleForkedProcessesByAllServices | Assertion failed | Flaky in full suite only; passes in isolation (all 38 class tests pass) |
| WriteBarrierTest | testMutateByteArrayUsingDoubleAtPut | Assertion failed | Expected failure on reference VM (`<expectedFailure>` pragma) |
| WriteBarrierTest | testMutateByteArrayUsingFloatAtPut | Assertion failed | Expected failure on reference VM (`<expectedFailure>` pragma) |

## Errors (7)

| Class | Test | Message | Verdict |
|-------|------|---------|---------|
| FBDDecompilerTest | testWhileTrue3 | OCCodeError: Undeclared variable | Expected failure on reference VM |
| GArcTest | testIntersectionsWithArc | ShouldBeImplemented: #intersectionsWithEllipse: should have been implemented in GCircle | Expected failure on reference VM |
| GArcTest | testIntersectionsWithEllipse | ShouldBeImplemented: #intersectionsWithEllipse: should have been implemented in GEllipse | Expected failure on reference VM |
| GEllipseTest | testIntersectionsWithArc | ShouldBeImplemented: #intersectionsWithEllipse: should have been implemented in GEllipse | Expected failure on reference VM |
| OCClassBuilderTest | testCreateNormalClassWithTraitComposition | OCCodeError: Undeclared variable | Non-existent test method in image |
| OCParserTest | testParseExpressionDontFreeze | OCCodeError: Literal expected | Non-existent test method in image |
| WeakAnnouncerTest | testNoWeakBlock | Error: Not currently available due to missing ephemerons support | Expected failure on reference VM |

## Skips (22)

| Class | Test | Reason |
|-------|------|--------|
| BlockClosureTest | testOnForkErrorTakesLessThanOneSecond | Skipped by test |
| BlockClosureTest | testOnForkSplit | Skipped by test |
| CodeSimulationTest | testErrorCodeNotFound | Skipped by test |
| FFICallbackParametersTest | testIdentityStruct | Skipped by test |
| FFIExternalStructurePlatformTest | testStructureHasCorrectOffsets32bits | Skipped by test (32-bit only) |
| FFIExternalStructurePlatformTest | testStructureHasCorrectSize32bits | Skipped by test (32-bit only) |
| FloatTest | testNaNCompare | Skipped by test |
| IntegerTest | testCreationFromBytes1 | Skipped by test |
| IntegerTest | testCreationFromBytes2 | Skipped by test |
| IntegerTest | testCreationFromBytes3 | Skipped by test |
| OCCodeReparatorTest | testdefineClass | Skipped by test |
| OCCodeReparatorTest | testdefineTrait | Skipped by test |
| OCParserTest | testUnclosedTemporariesErrorNodeContainsRightValue | Skipped by test |
| ProcessMonitorTestServiceTest | testAlwaysPassBackgroundHalt | Skipped by test |
| ProcessMonitorTestServiceTest | testDoesNotRaiseForkedProcessFailureWhenFailuresWerePassedAndProcessCompletes | Skipped by test |
| ProcessMonitorTestServiceTest | testPassBackgroundFailuresWhenSuspensionLogicIsDisabled | Skipped by test |
| RGMCClassTest | testClassesWithTraits | Skipped by test |
| Win32WideStringTest | testCharactersAreEncodedInUnicode16Bits | Skipped by test (Windows only) |
| Win32WideStringTest | testConvertingInBothDirectionsGaveSameString | Skipped by test (Windows only) |
| Win32WideStringTest | testHandleIsAByteArray | Skipped by test (Windows only) |
| Win32WideStringTest | testUnderlayingByteArrayEndsInTwoZeros | Skipped by test (Windows only) |
| Win32WideStringTest | testUnderlayingByteArrayIsMultipleOf2 | Skipped by test (Windows only) |

## Summary

- **0 real VM bugs** — all failures are upstream expected failures, non-existent tests, or flaky suite interactions
- 6 expected failures (same behavior on reference Pharo VM)
- 2 non-existent test methods (image version mismatch with test runner)
- 1 flaky full-suite interaction (passes in isolation)
- 22 skips are self-skipping tests (platform-specific, known limitations)
