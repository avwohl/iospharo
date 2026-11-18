# Tests that do not pass on our VM and on Cog (state of 2026-11-18)

On 2026-11-17 the owner set the focus on the tests that do not pass.  This
file lists each such test of the Pharo test suites, on our VM and on stock
Cog.  For each test the file gives the state now.  The data comes from saved
results.  No VM ran to make this file.

Sources:

- The repair record:
  [upstream/pharo-image-repairs-2026-11-17.md](upstream/pharo-image-repairs-2026-11-17.md).
  The column "Group in the record" names the section of the record with the
  cause of each test.
- The last full sweep of each VM, saved in `~/pharo-work-save/2026-11-17/sweeps/`:
  `sweep14-new14` (our VM, `build-rel`, image fixes 1 to 35) and
  `sweepcogp14-new10` (patched Cog, image fixes 1 to 35).
- The package run of rows 1 to 163, saved in `~/pharo-work-save/2026-11-17/pkgs1/`.
- The full list in one file, one line for each test:
  [results/nonpassing-tests-2026-11-18.tsv](results/nonpassing-tests-2026-11-18.tsv).

"Patched Cog" is stock Cog v12.0.6 (arm64) with the Cog changes of
[upstream/](upstream/).  "Stock Cog" is Cog v12.0.6 with no change.  The
sweeps use the runner `scripts/pharo-headless-test/run_sunit.st`, which runs
each parameter set of a parametrized test.  A count such as "PASS 2x" is a
test with two parameter sets.

## Summary

| Test set | Tests | Did not pass on 2026-11-17 | Do not pass now | Of these, open |
|---|---|---|---|---|
| Pharo 14 suite (build 791) | 28413 (old runner), 39465 (new runner) | 220 | 80 skip in the sweeps (A3, A4) | 12 (A3), and 4 that need Cog changes (A2) |
| Tests found during the work (part B) | - | 18 | 1 | 1 |
| Tests that failed at times before 2026-11-17 (part C) | - | 7 | 0 in the last sweeps | 7, no fix entry |
| Pharo 13 suite | 28077 | 183 | not measured again | 183, no per-test list |
| Package run, rows 1 to 163 (155 rows ran) | 46419 | not recorded per test | 291 in 22 rows, and 8 rows that do not load | 256 (79 wait on a decision of the owner) |

### Pharo 14: the 220 tests

On 2026-11-17, before the repairs, 220 of 28413 tests did not pass.  The result was
the same on our VM and on stock Cog: 170 skips, 49 tests with the
expected-failure mark, and 1 failure.  The state of the 220 tests in the last
sweeps:

| Part | Tests | State in the last sweeps |
|---|---|---|
| A1 | 132 | repaired: pass on our VM and on patched Cog |
| A2 | 4 | pass on our VM and on patched Cog.  Stock Cog fails the 4 tests until the Cog changes are in Cog |
| A3 | 56 | skip on the two VMs.  The test cannot apply on macOS arm64, with a proof in the record.  12 of the 56 have a gap in the proof (see "Open points") |
| A4 | 28 | need an interactive session.  4 pass in the sweeps now.  24 skip in the sweeps and pass in the interactive pass of our VM (135 of 135 tests) |

The last sweep of our VM: 39376 of 39465 tests pass, 1 failure, 87 skips, 1
expected failure.  The last sweep of the patched Cog: 39377 pass, 0 failures,
87 skips, 1 expected failure.

The 87 skips and the expected failure are the same tests on the two VMs: the
56 tests of A3 and the 24 tests of A4.  The 56 tests of A3 give 64 lines,
because 8 of them run with two parameter sets.  The 1 failure of our VM is
`TKTNewProcessTaskRunnerTest>>testNewProcessTaskRunnerRunsInNewProcessEveryTime`
(part B, open).

### Open points

1. `TKTNewProcessTaskRunnerTest>>testNewProcessTaskRunnerRunsInNewProcessEveryTime`
   failed 1 time in `sweep14-new14` ("Got 9 instead of 10").  The draft fix
   `scripts/image-fixes/36-taskit-newprocess-runner-tests.st` is in the tree,
   not committed and not validated.
2. `AllTestFailTest` (4 tests) and `DependencyTest` (6 tests): fixtures, but
   no other test runs them.  The rule of the owner accepts a skip for "a
   fixture that another test runs".  These 10 tests do not meet the rule yet.
3. `TFFunctionCallTest>>testCallingFunctionWithW64CallingConvention` and
   `TFUFFIDifferentCallingConventionFunctionCallTest>>testCallingFunctionWithW64CallingConvention`:
   the tests run only on an x86_64 VM on macOS.  This Mac runs an x86_64 VM
   under Rosetta (`build-x86`, the x86_64 stock Cog).  The proof is only the
   architecture name on arm64.  A run on an x86_64 VM is not done.
4. A2: the 4 tests pass on stock Cog only with the Cog changes.  The patches
   are in [upstream/](upstream/) for the owner to review.
5. Group `Debugger-kernel-skips` (A1: the 3 `DebugInfoTest>>testNotStop*`
   tests, A2: `CodeSimulationTest>>testErrorCodeNotFound`): the review was
   not accepted in round 2.  The 4 tests pass in the last sweeps.  The
   open requests of the reviewer: the debugger still steps with the old stop
   points (`shouldStepStopAt:`), because 11 tests of the external Sindarin
   package fail with the new stop points.  The owner must decide whether
   Sindarin changes.
6. A4: the 24 tests pass only in an interactive session.  The interactive
   pass ran on our VM only.  The records have no interactive pass on Cog.
7. Part C: 7 tests failed at times in sweeps before 2026-11-17 and have no
   fix entry.  The 6 saved sweeps of 2026-11-17 pass them.
8. Pharo 13: the work did not start.  The first sweep with the old runner
   gave 27894 passes of 28077: 2 failures, 131 skips, 50 expected failures.
   The saved copy and the repository have no per-test list of that sweep.
   Partial lists:
   `results/sweep-arm-2026-10-11/failing-selectors.txt` (Pharo 13.1, 19
   failures and errors, no skips), and the 7 Pharo 13 test defects of
   `scripts/image-fixes/10-pharo13-test-defects.st`
   ([image_issues.md](image_issues.md)).  The entries of
   `scripts/image-fixes/` check for each defect before the change.  No
   Pharo 13 run with the entries is recorded.  See "The 132 repairs and
   Pharo 13" below.
9. The package run: 256 tests are open.  See the package part.

### The 132 repairs and Pharo 13

The 132 repairs change classes of the standard Pharo 14 image (build 791 from
`get.pharo.org`), not our harness.  Each repair was written and measured on
Pharo 14 only.  A comparison of the source files (no VM run, 2026-11-18) of
Pharo 13.1 (`Pharo13.1-64bit-81f6149.sources`) and Pharo 14
(`Pharo14.0-64bit-42a90c7.sources`) gives:

- 121 of the 132 test methods have the same source in Pharo 13.  2 more
  differ only in a name and keep the same skip
  (`BlockClosureTest>>testOnForkErrorTakesLessThanOneSecond`,
  `ClapHelloTest>>testHelloBothShoutingAndWhispering`).
- 8 tests are not in Pharo 13: the classes `DebugInfoTest`, `OCRengineTest`,
  `RunnerLongDurationTest` and `StDebuggerClientModelTest` are new in
  Pharo 14, and `OCArrayLiteralTest>>testLiteralArrayLongHavingAnotherArrayLong`
  is a new test.
- 1 test has the skip only in Pharo 14:
  `ReflectivityOnStackTest>>testExecuteWithWaitingWithLinkInstalledInOuterMethodAfterExecutionOfMethodAfterExecutionOfEmbeddedBlockChangeMethodManually`
  (Pharo issue 20072).
- Of the methods that the entries 20 to 35 replace, 126 have the same source
  in Pharo 13, 10 differ and 16 are not in Pharo 13.  An entry checks the
  old source text first, so an entry for a method that differs makes no
  change on Pharo 13.  The 10 methods that differ:
  `BlockClosureTest>>testOnForkErrorTakesLessThanOneSecond`,
  `ClapHelloTest>>testHelloBothShoutingAndWhispering`,
  `ClyBrowserMorph>>spawnQueryBrowserOn:withState:`,
  `OCASTTranslator>>visitArrayNode:`,
  `ReflectivityOnStackTest>>testExecuteWithWaiting...ChangeMethodManually`,
  `OCCodeReparator>>openMenu`, `NetNameResolver class>>localHostAddress`,
  `OCCodeSnippet class>>badExpressions`,
  `RBNamespace>>renameClass:to:around:`,
  `RBTransformationsTest>>testRenameClassTransform`.

### Where the repairs go upstream

The Pharo 14 image is built from `pharo-project/pharo` and from projects
that `BaselineOfPharo` loads at a fixed version: Spec `v2.0.6`, NewTools
`v0.11.0`, NewTools-DocumentBrowser `v1.1.1`, Microdown `v2.11.0`, Roassal
`v1.7.7` (Roassal loads Geometry `v1.2.1`).  A repair in such a project gets
into the image only when Pharo moves the version in `BaselineOfPharo`.  The
repository of each class comes from the class files of `~/src/pharo`
(branch `Pharo14`) and of each project at its version (2026-11-18).

The development branch is `Pharo15` in `pharo-project/pharo`, Spec,
NewTools and NewTools-DocumentBrowser, `dev` in Microdown and `master` in
Geometry.  Each project also has a `Pharo14` branch, except Geometry.  The
repairs were measured on Pharo 14 only.

Groups of the record (A1 and A2 tests in brackets) by repository:

- `pharo-project/pharo` (88: 84 of A1 and the 4 of A2): BlockClosureTest
  (2), ClapHelloTest (1), Cly-async-results (2), Cly-misc-queries (5),
  Cly-wrapper-queries (14), Debugger-kernel-skips (4), FloatTest (1),
  Fuel-cleanblock (9), Fuel-widestring-globals (15), NA-platform-windows (3),
  NavigationInteractionTest (1), OCArrayLiteralTest (1), OCParserTest (2),
  OCProgramNodeTest (1), OCRengineTest (1), RGMCClassTest (1),
  ReflectivityOnStackTest (1), ReleaseTest (1), Ring2ChunkImporterTest (2),
  Rubric-anchors (3), RunnerLongDurationTest (2), SettingsStonReaderTest
  (1), SlotErrorsTest (1), TFFI-disabled-tests (4), WeakAnnouncerTest (1),
  WriteBarrierTest (2), network-tests (4), platform-unix-resolver (3).
  The 4 tests of A2 also need the Cog patches of [upstream/](upstream/) in
  `pharo-project/pharo-vm`.
- `pharo-spec/Spec` (22): SpCodePresenterTest (3), SpEasy-skips (10),
  SpMorphPresenterTest (1), SpRadioButtonInteractionTest (2),
  SpTree-selection-skips (6).
- `pharo-spec/NewTools` (12): CavWindowStrategyTest (1), StDebugger-skips
  (9), StObjectTranscriptPresenterTest (1), StSettingsBrowserTest (1).
- `pharo-spec/Spec` and `pharo-spec/NewTools-DocumentBrowser`:
  presenter-examples (8).
- `pharo-project/pharo` and `pharo-spec/NewTools`: St-evaluate-declare-global
  (2).
- `pharo-contributions/Geometry`: Geometry-intersections (3).
- `pillar-markup/Microdown`: MicMetaDataBlockTest (1).
- `pharo-project/pharo` and `pharo-spec/Spec`: the interactive-session
  entries of A4 (`TaskbarMorph`, `SimulateMouseTest`, `SpTextPresenterTest`).

## Part A1: repaired, 132 tests

| # | Test | Before | Defect in | Group in the record | Fix entry | Our VM | Patched Cog |
|---|---|---|---|---|---|---|---|
| 1 | `BlockClosureTest>>testOnForkErrorTakesLessThanOneSecond` | SKIP | test | BlockClosureTest | 20-BlockClosureTest.st | PASS | PASS |
| 2 | `BlockClosureTest>>testOnForkSplit` | SKIP | test | BlockClosureTest | 20-BlockClosureTest.st | PASS | PASS |
| 3 | `CavWindowStrategyTest>>testExactReplaceStrategyDoesNotChangeExtentAndPosition` | SKIP | test | CavWindowStrategyTest | 20-CavWindowStrategyTest.st | PASS | PASS |
| 4 | `ClapHelloTest>>testHelloBothShoutingAndWhispering` | SKIP | image-code | ClapHelloTest | 20-ClapHelloTest.st | PASS | PASS |
| 5 | `ClyAsyncQueryResultTest>>testItemsChangedNotificationShouldResetItems` | XFAIL | test | Cly-async-results | 20-Cly-async-results.st | PASS | PASS |
| 6 | `ClyAsyncQueryTest>>testHasCompositeScopeFromSubqueries` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 7 | `ClyAsyncQueryTest>>testIsExecutedFromMultipleScopeWhenSubqueriesAreFromScopesOfDifferentObjects` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 8 | `ClyAsyncQueryTest>>testIsExecutedFromSingleScopeWhenSubqueriesAreFromDifferentScopesOfSameObject` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 9 | `ClyAsyncQueryTest>>testNotExecutedFromMultipleScopeWhenAllSubqueriesAreFromEmptyScope` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 10 | `ClyAsyncQueryTest>>testNotExecutedFromMultipleScopeWhenSubqueriesAreFromDifferentScopesOfSameObject` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 11 | `ClyAsyncQueryTest>>testNotExecutedFromSingleScopeWhenAllSubqueriesAreFromEmptyScope` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 12 | `ClyAsyncQueryTest>>testNotExecutedFromSingleScopeWhenSubqueriesAreFromScopesOfDifferentObjects` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 13 | `ClyConstantMethodQueryTest>>testIsEmptyFromEmptyMethodScope` | XFAIL | test | Cly-misc-queries | 20-Cly-misc-queries.st | PASS | PASS |
| 14 | `ClyExtendingPackagesQueryTest>>testFromClassWhichTraitIsExtendedButNotItself` | XFAIL | test | Cly-misc-queries | 20-Cly-misc-queries.st | PASS | PASS |
| 15 | `ClyFilterQueryTest>>testHasCompositeScopeFromSubqueries` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 16 | `ClyFilterQueryTest>>testIsExecutedFromMultipleScopeWhenSubqueriesAreFromScopesOfDifferentObjects` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 17 | `ClyFilterQueryTest>>testIsExecutedFromSingleScopeWhenSubqueriesAreFromDifferentScopesOfSameObject` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 18 | `ClyFilterQueryTest>>testNotExecutedFromMultipleScopeWhenAllSubqueriesAreFromEmptyScope` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 19 | `ClyFilterQueryTest>>testNotExecutedFromMultipleScopeWhenSubqueriesAreFromDifferentScopesOfSameObject` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 20 | `ClyFilterQueryTest>>testNotExecutedFromSingleScopeWhenAllSubqueriesAreFromEmptyScope` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 21 | `ClyFilterQueryTest>>testNotExecutedFromSingleScopeWhenSubqueriesAreFromScopesOfDifferentObjects` | XFAIL | test | Cly-wrapper-queries | 20-Cly-wrapper-queries.st | PASS | PASS |
| 22 | `ClyRestUntaggedClassesQueryTest>>testIsEmptyFromEmptyClassScope` | XFAIL | test | Cly-misc-queries | 20-Cly-misc-queries.st | PASS | PASS |
| 23 | `ClySemiAsyncQueryResultTest>>testItemsChangedNotificationShouldResetItems` | XFAIL | test | Cly-async-results | 20-Cly-async-results.st | PASS | PASS |
| 24 | `ClyUnimplementedMethodsQueryTest>>testIsEmptyFromEmptyMethodScope` | XFAIL | test | Cly-misc-queries | 20-Cly-misc-queries.st | PASS | PASS |
| 25 | `ClyUnknownQueryTest>>testIsBoundToEnvironmentWhenScopeIs` | XFAIL | test | Cly-misc-queries | 20-Cly-misc-queries.st | PASS | PASS |
| 26 | `DebugInfoTest>>testNotStopAtAllButFirstBytecodeOfOptimizedSend` | SKIP | image-code | Debugger-kernel-skips | 20-Debugger-kernel-skips.st | PASS 2x | PASS 2x |
| 27 | `DebugInfoTest>>testNotStopAtConstantBlockCreation` | SKIP | image-code | Debugger-kernel-skips | 20-Debugger-kernel-skips.st | PASS 2x | PASS 2x |
| 28 | `DebugInfoTest>>testNotStopAtFullBlockCreation` | SKIP | image-code | Debugger-kernel-skips | 20-Debugger-kernel-skips.st | PASS 2x | PASS 2x |
| 29 | `FFICallbackParametersTest>>testIdentityStruct` | SKIP | test | TFFI-disabled-tests | 20-TFFI-disabled-tests.st | PASS | PASS |
| 30 | `FLBinaryFileStreamBasicSerializationTest>>testConsiderCustomWideSymbolGlobal` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 31 | `FLBinaryFileStreamBasicSerializationTest>>testWideStringClassName` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 32 | `FLBinaryFileStreamBasicSerializationTest>>testWideStringGlobal` | XFAIL | test | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 33 | `FLByteArrayBasicSerializationTest>>testConsiderCustomWideSymbolGlobal` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 34 | `FLByteArrayBasicSerializationTest>>testWideStringClassName` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 35 | `FLByteArrayBasicSerializationTest>>testWideStringGlobal` | XFAIL | test | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 36 | `FLFileReferenceStreamBasicSerializationTest>>testConsiderCustomWideSymbolGlobal` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 37 | `FLFileReferenceStreamBasicSerializationTest>>testWideStringClassName` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 38 | `FLFileReferenceStreamBasicSerializationTest>>testWideStringGlobal` | XFAIL | test | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 39 | `FLFullBasicSerializationTest>>testConsiderCustomWideSymbolGlobal` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 40 | `FLFullBasicSerializationTest>>testWideStringClassName` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 41 | `FLFullBasicSerializationTest>>testWideStringGlobal` | XFAIL | test | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 42 | `FLFullHeaderSerializationTest>>testPostMaterializationActions` | SKIP | test | Fuel-cleanblock | 20-Fuel-cleanblock.st | PASS | PASS |
| 43 | `FLFullHeaderSerializationTest>>testPreMaterializationActions` | SKIP | test | Fuel-cleanblock | 20-Fuel-cleanblock.st | PASS | PASS |
| 44 | `FLGZippedBasicSerializationTest>>testConsiderCustomWideSymbolGlobal` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 45 | `FLGZippedBasicSerializationTest>>testWideStringClassName` | XFAIL | image-code | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 46 | `FLGZippedBasicSerializationTest>>testWideStringGlobal` | XFAIL | test | Fuel-widestring-globals | 20-Fuel-widestring-globals.st | PASS | PASS |
| 47 | `FLGlobalEnvironmentTest>>testCompiledMethodChanged` | SKIP | test | Fuel-cleanblock | 20-Fuel-cleanblock.st | PASS | PASS |
| 48 | `FLHeaderSerializationTest>>testPostMaterializationActions` | SKIP | test | Fuel-cleanblock | 20-Fuel-cleanblock.st | PASS | PASS |
| 49 | `FLHeaderSerializationTest>>testPreMaterializationActions` | SKIP | test | Fuel-cleanblock | 20-Fuel-cleanblock.st | PASS | PASS |
| 50 | `FLSortedCollectionSerializationTest>>testSortedCollection` | SKIP | test | Fuel-cleanblock | 20-Fuel-cleanblock.st | PASS | PASS |
| 51 | `FLSortedCollectionSerializationTest>>testSortedCollectionWithClassVariableChanges` | SKIP | test | Fuel-cleanblock | 20-Fuel-cleanblock.st | PASS | PASS |
| 52 | `FLSortedCollectionSerializationTest>>testSortedCollectionWithClassVariableReference` | SKIP | test | Fuel-cleanblock | 20-Fuel-cleanblock.st | PASS | PASS |
| 53 | `FLUserGuidesTest>>testDemo` | SKIP | test | Fuel-cleanblock | 20-Fuel-cleanblock.st | PASS | PASS |
| 54 | `FloatTest>>testNaNCompare` | SKIP | test | FloatTest | 20-FloatTest.st | PASS | PASS |
| 55 | `GArcTest>>testIntersectionsWithArc` | XFAIL | image-code | Geometry-intersections | 20-Geometry-intersections.st | PASS | PASS |
| 56 | `GArcTest>>testIntersectionsWithEllipse` | XFAIL | image-code | Geometry-intersections | 20-Geometry-intersections.st | PASS | PASS |
| 57 | `GEllipseTest>>testIntersectionsWithArc` | XFAIL | image-code | Geometry-intersections | 20-Geometry-intersections.st | PASS | PASS |
| 58 | `MicMetaDataBlockTest>>testCreatingAnInstanceInitializeTheMetadatDictionary` | SKIP | test | MicMetaDataBlockTest | 20-MicMetaDataBlockTest.st | PASS | PASS |
| 59 | `MicTextPresenterTest>>testExample` | SKIP | image-code | presenter-examples | 20-presenter-examples.st | PASS | PASS |
| 60 | `MicrodownPresenterTest>>testExample` | SKIP | image-code | presenter-examples | 20-presenter-examples.st | PASS | PASS |
| 61 | `NavigationInteractionTest>>testBrowseMethodViaImplementorActuallyBrowseTheClass` | SKIP | image-code | NavigationInteractionTest | 20-NavigationInteractionTest.st | PASS | PASS |
| 62 | `NetNameResolverTest>>testLocalHostAddress` | SKIP | image-code | network-tests | 20-network-tests.st | PASS | PASS |
| 63 | `OCArrayLiteralTest>>testLiteralArrayLongHavingAnotherArrayLong` | SKIP | image-code | OCArrayLiteralTest | 20-OCArrayLiteralTest.st | PASS | PASS |
| 64 | `OCParserTest>>testParseExpressionDontFreeze` | XFAIL | test | OCParserTest | 20-OCParserTest.st | PASS | PASS |
| 65 | `OCParserTest>>testUnclosedTemporariesErrorNodeContainsRightValue` | SKIP | test | OCParserTest | 20-OCParserTest.st | PASS | PASS |
| 66 | `OCProgramNodeTest>>testBestNodeForReturnAStatementWhenIntervalInStatementWithoutLeftPart` | XFAIL | image-code | OCProgramNodeTest | 20-OCProgramNodeTest.st | PASS | PASS |
| 67 | `OCRengineTest>>testWhichUNodesDefinesBlockArg` | SKIP | test | OCRengineTest | 20-OCRengineTest.st | PASS | PASS |
| 68 | `RGMCClassTest>>testClassesWithTraits` | SKIP | image-code | RGMCClassTest | 20-RGMCClassTest.st | PASS | PASS |
| 69 | `ReflectivityOnStackTest>>testExecuteWithWaitingWithLinkInstalledInOuterMethodAfterExecutionOfMethodAfterExecutionOfEmbeddedBlockChangeMethodManually` | SKIP | image-code | ReflectivityOnStackTest | 20-ReflectivityOnStackTest.st | PASS | PASS |
| 70 | `ReleaseTest>>testRatioOfCommentedClasses` | SKIP | test | ReleaseTest | 20-ReleaseTest.st | PASS | PASS |
| 71 | `Ring2ChunkImporterTest>>testMultipleMixedChunksInStrangeOrderWithMetaclassTrait` | XFAIL | image-code | Ring2ChunkImporterTest | 20-Ring2ChunkImporterTest.st | PASS | PASS |
| 72 | `Ring2ChunkImporterTest>>testStandardMethodInNewMetaclassTrait` | XFAIL | both-vms | Ring2ChunkImporterTest | 20-Ring2ChunkImporterTest.st | PASS | PASS |
| 73 | `RubEditingAreaTest>>testEmbeddedAnchorInTextAreDrawn` | SKIP | test | Rubric-anchors | 20-Rubric-anchors.st | PASS | PASS |
| 74 | `RubEditingAreaTest>>testEmbeddedAnchorInTextAreDrawnAtRightPosition` | SKIP | test | Rubric-anchors | 20-Rubric-anchors.st | PASS | PASS |
| 75 | `RubScrolledTextMorphTest>>testEmbeddedAnchorInTextAreDrawnAtRightPosition` | SKIP | test | Rubric-anchors | 20-Rubric-anchors.st | PASS | PASS |
| 76 | `RunnerLongDurationTest>>testRunningFromAClass` | SKIP | test | RunnerLongDurationTest | 20-RunnerLongDurationTest.st | PASS | PASS |
| 77 | `RunnerLongDurationTest>>testRunningFromACollectionOfClasses` | SKIP | test | RunnerLongDurationTest | 20-RunnerLongDurationTest.st | PASS | PASS |
| 78 | `SettingsStonReaderTest>>testNonExistingStoredSettingVariable` | XFAIL | image-code | SettingsStonReaderTest | 20-SettingsStonReaderTest.st | PASS | PASS |
| 79 | `SlotErrorsTest>>testCannotBeRecompiled` | XFAIL | test | SlotErrorsTest | 20-SlotErrorsTest.st | PASS | PASS |
| 80 | `SpCodeEditorPresenterTest>>testExample` | SKIP | image-code | presenter-examples | 20-presenter-examples.st | PASS | PASS |
| 81 | `SpCodePresenterTest>>testDoBrowseClassNotFound` | SKIP | image-code | SpCodePresenterTest | 20-SpCodePresenterTest.st | PASS | PASS |
| 82 | `SpCodePresenterTest>>testDoBrowseMethodReferences` | SKIP | test | SpCodePresenterTest | 20-SpCodePresenterTest.st | PASS | PASS |
| 83 | `SpCodePresenterTest>>testEnvironment` | SKIP | image-code | SpCodePresenterTest | 20-SpCodePresenterTest.st | PASS | PASS |
| 84 | `SpDemoTest>>testExample` | SKIP | image-code | presenter-examples | 20-presenter-examples.st | PASS | PASS |
| 85 | `SpEasyColumnViewPresenterTest>>testDisableActivationDuring` | SKIP | test | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 86 | `SpEasyColumnViewPresenterTest>>testSetSortingBlockBeforeItems` | SKIP | image-code | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 87 | `SpEasyColumnViewPresenterTest>>testSortingBlock` | SKIP | image-code | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 88 | `SpEasyListViewPresenterTest>>testDisableActivationDuring` | SKIP | test | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 89 | `SpEasyTreeColumnViewPresenterTest>>testDisableActivationDuring` | SKIP | test | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 90 | `SpEasyTreeColumnViewPresenterTest>>testPathIndexOf` | SKIP | image-code | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 91 | `SpEasyTreeColumnViewPresenterTest>>testPathItemOf` | SKIP | image-code | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 92 | `SpEasyTreeListViewPresenterTest>>testDisableActivationDuring` | SKIP | test | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 93 | `SpEasyTreeListViewPresenterTest>>testPathIndexOf` | SKIP | image-code | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 94 | `SpEasyTreeListViewPresenterTest>>testPathItemOf` | SKIP | image-code | SpEasy-skips | 20-SpEasy-skips.st | PASS | PASS |
| 95 | `SpMenuItemPresenterTest>>testExample` | SKIP | image-code | presenter-examples | 20-presenter-examples.st | PASS | PASS |
| 96 | `SpMenuPresenterTest>>testExample` | SKIP | image-code | presenter-examples | 20-presenter-examples.st | PASS | PASS |
| 97 | `SpMorphPresenterTest>>testChangingHelpAffectTheWidget` | SKIP | image-code | SpMorphPresenterTest | 20-SpMorphPresenterTest.st | PASS 2x | PASS 2x |
| 98 | `SpRadioButtonInteractionTest>>testHidePresenterHidesWidget` | SKIP | test | SpRadioButtonInteractionTest | 20-SpRadioButtonInteractionTest.st | PASS 2x | PASS 2x |
| 99 | `SpRadioButtonInteractionTest>>testShowPresenterShowsWidget` | SKIP | test | SpRadioButtonInteractionTest | 20-SpRadioButtonInteractionTest.st | PASS 2x | PASS 2x |
| 100 | `SpToolbarToggleButtonMorphTest>>testExample` | SKIP | image-code | presenter-examples | 20-presenter-examples.st | PASS | PASS |
| 101 | `SpTransmissionWithComponentListTest>>testExample` | SKIP | image-code | presenter-examples | 20-presenter-examples.st | PASS | PASS |
| 102 | `SpTreeAdapterMultipleSelectionTest>>testSelectPresenterItemSetsSelectedIndexInWidget` | SKIP | test | SpTree-selection-skips | 20-SpTree-selection-skips.st | PASS 2x | PASS 2x |
| 103 | `SpTreeAdapterSingleSelectionTest>>testSelectPresenterItemSetsSelectedIndexInWidget` | SKIP | test | SpTree-selection-skips | 20-SpTree-selection-skips.st | PASS 2x | PASS 2x |
| 104 | `SpTreeAdapterSingleSelectionTest>>testUnselectPresenterItemUnselectsIndexInWidget` | SKIP | test | SpTree-selection-skips | 20-SpTree-selection-skips.st | PASS 2x | PASS 2x |
| 105 | `SpTreeTableAdapterMultipleSelectionTest>>testSelectPresenterItemSetsSelectedIndexInWidget` | SKIP | test | SpTree-selection-skips | 20-SpTree-selection-skips.st | PASS 2x | PASS 2x |
| 106 | `SpTreeTableAdapterSingleSelectionTest>>testSelectPresenterItemSetsSelectedIndexInWidget` | SKIP | test | SpTree-selection-skips | 20-SpTree-selection-skips.st | PASS 2x | PASS 2x |
| 107 | `SpTreeTableAdapterSingleSelectionTest>>testUnselectPresenterItemUnselectsIndexInWidget` | SKIP | test | SpTree-selection-skips | 20-SpTree-selection-skips.st | PASS 2x | PASS 2x |
| 108 | `StDebuggerClientModelTest>>testComputeInitialTopContext` | SKIP | test | StDebugger-skips | 20-StDebugger-skips.st | PASS | PASS |
| 109 | `StDebuggerContextPredicateTest>>testPrintDescription` | SKIP | test | StDebugger-skips | 20-StDebugger-skips.st | PASS | PASS |
| 110 | `StDebuggerContextPredicateTest>>testPrintPostMortemDescription` | SKIP | test | StDebugger-skips | 20-StDebugger-skips.st | PASS | PASS |
| 111 | `StDebuggerTest>>testGenerateAndProceed` | SKIP | test | StDebugger-skips | 20-StDebugger-skips.st | PASS | PASS |
| 112 | `StDebuggerTest>>testRestoreReceiverRawInspectionSelection` | SKIP | test | StDebugger-skips | 20-StDebugger-skips.st | PASS | PASS |
| 113 | `StDebuggerTest>>testRestoreReceiverRawInspectionSelectionAfterContextChange` | SKIP | test | StDebugger-skips | 20-StDebugger-skips.st | PASS | PASS |
| 114 | `StDebuggerTest>>testReturnEnteredValue` | SKIP | test | StDebugger-skips | 20-StDebugger-skips.st | PASS | PASS |
| 115 | `StDebuggerTest>>testSaveReceiverRawInspectionSelection` | SKIP | test | StDebugger-skips | 20-StDebugger-skips.st | PASS | PASS |
| 116 | `StDebuggerTest>>testSaveReceiverRawInspectionSelectionAfterContextChange` | SKIP | test | StDebugger-skips | 20-StDebugger-skips.st | PASS | PASS |
| 117 | `StObjectContextPresenterTest>>testDoEvaluateDeclareGlobal` | SKIP | test | St-evaluate-declare-global | 20-St-evaluate-declare-global.st | PASS | PASS |
| 118 | `StObjectTranscriptPresenterTest>>testTrueShowInspectorShowsInspector` | SKIP | test | StObjectTranscriptPresenterTest | 20-StObjectTranscriptPresenterTest.st | PASS | PASS |
| 119 | `StPlaygroundPagePresenterTest>>testDoEvaluateDeclareGlobal` | SKIP | test | St-evaluate-declare-global | 20-St-evaluate-declare-global.st | PASS | PASS |
| 120 | `StSettingsBrowserTest>>testSimulateCategoryPopulation` | SKIP | test | StSettingsBrowserTest | 20-StSettingsBrowserTest.st | PASS | PASS |
| 121 | `TFCallbacksTest>>testCallbackNotRespectingLIFOOrderFailsReturn` | SKIP | test | TFFI-disabled-tests | 20-TFFI-disabled-tests.st | PASS 2x | PASS 2x |
| 122 | `TFUFFIFunctionCallTest>>testLongCalloutDoesNotBlockImage` | SKIP | test | TFFI-disabled-tests | 20-TFFI-disabled-tests.st | PASS 2x | PASS 2x |
| 123 | `UnixResolverTest>>testGetTempFromTMPDIR` | SKIP | test | platform-unix-resolver | 20-platform-unix-resolver.st | PASS | PASS |
| 124 | `UnixResolverTest>>testXdgParseUserDirLineDocuments` | SKIP | test | platform-unix-resolver | 20-platform-unix-resolver.st | PASS | PASS |
| 125 | `UnixResolverTest>>testXdgParseUserDirLineNotRoot` | SKIP | test | platform-unix-resolver | 20-platform-unix-resolver.st | PASS | PASS |
| 126 | `WeakAnnouncerTest>>testNoWeakBlock` | XFAIL | image-code | WeakAnnouncerTest | 20-WeakAnnouncerTest.st | PASS | PASS |
| 127 | `Win32EnvironmentTest>>testExistingEnvironmentVariableLongerThanBufferShouldReturnValue` | SKIP | test | NA-platform-windows | 20-NA-platform-windows.st | PASS | PASS |
| 128 | `Win32EnvironmentTest>>testExistingEnvironmentVariableShouldReturnValue` | SKIP | test | NA-platform-windows | 20-NA-platform-windows.st | PASS | PASS |
| 129 | `Win32EnvironmentTest>>testNonExistingEnvironmentVariableShouldGenerateException` | SKIP | test | NA-platform-windows | 20-NA-platform-windows.st | PASS | PASS |
| 130 | `ZnClientTest>>testQueryGoogle` | FAIL | test | network-tests | 20-network-tests.st | PASS | PASS |
| 131 | `ZnHTTPSTest>>testGetPharoVersion` | SKIP | test | network-tests | 20-network-tests.st | PASS | PASS |
| 132 | `ZnHTTPSTest>>testSNIPage` | SKIP | test | network-tests | 20-network-tests.st | PASS | PASS |

## Part A2: pass on our VM and on patched Cog, 4 tests

Stock Cog fails these 4 tests until the Cog changes are in Cog.

- `CodeSimulationTest>>testErrorCodeNotFound`: primitive 218 must give the
  error code `#'not found'`.  The Cog change is one line in
  `primitiveDoNamedPrimitiveWithArgs`.  The review of the group was not
  accepted in round 2: the reviewer asked for a run on a patched Cog, which
  the last sweep now has.
- `TFUFFICallbackTest>>testCallbacksInOtherThread`: stock Cog stops with a
  segmentation fault for a callback from another thread with the same-thread
  runner
  ([upstream/cog-same-thread-runner-callback-from-other-thread.patch](upstream/cog-same-thread-runner-callback-from-other-thread.patch)).
- The two `WriteBarrierTest` tests: Cog primitives 628 and 629 write into a
  read-only `ByteArray`
  ([upstream/cog-write-barrier-typed-stores.patch](upstream/cog-write-barrier-typed-stores.patch)).

| # | Test | Before | Defect in | Group in the record | Fix entry | Our VM | Patched Cog | Stock Cog |
|---|---|---|---|---|---|---|---|---|
| 1 | `CodeSimulationTest>>testErrorCodeNotFound` | SKIP | both-vms | Debugger-kernel-skips | 20-Debugger-kernel-skips.st | PASS | PASS | FAIL |
| 2 | `TFUFFICallbackTest>>testCallbacksInOtherThread` | SKIP | cog-vm | TFFI-disabled-tests | 30-ffi-callback-from-other-thread.st | PASS 2x | PASS 2x | SKIP with the same-thread runner (without the Cog change the VM stops) |
| 3 | `WriteBarrierTest>>testMutateByteArrayUsingDoubleAtPut` | XFAIL | cog-vm | WriteBarrierTest | 20-WriteBarrierTest.st | PASS | PASS | FAIL |
| 4 | `WriteBarrierTest>>testMutateByteArrayUsingFloatAtPut` | XFAIL | cog-vm | WriteBarrierTest | 20-WriteBarrierTest.st | PASS | PASS | FAIL |

## Part A3: cannot apply on macOS arm64, 56 tests

35 fixtures that other tests run, 16 tests for Windows or for x86_64, and 5
tests for a 32-bit VM.  The proof of each test is in the record, groups
`NA-fixture-tests`, `NA-platform-windows` and `NA-platform-32bit`.  See
"Open points" 2 and 3 for the 12 tests with a gap in the proof.

| # | Test | Before | Defect in | Group in the record | Fix entry | Our VM | Patched Cog |
|---|---|---|---|---|---|---|---|
| 1 | `AllTestFailTest>>testFail1` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 2 | `AllTestFailTest>>testFail2` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 3 | `AllTestFailTest>>testFail3` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 4 | `AllTestFailTest>>testFail4` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 5 | `ClyTestedClassMockTest>>testExpectedFailure` | XFAIL | none | NA-fixture-tests | - | XFAIL | XFAIL |
| 6 | `DependencyTest>>testDataIs10ThenIncrement` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 7 | `DependencyTest>>testDataIs11ThenIncrement` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 8 | `DependencyTest>>testDataIs12ThenIncrement` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 9 | `DependencyTest>>testDataIs13ThenIncrement` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 10 | `DependencyTest>>testDataIs14ThenIncrement` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 11 | `DependencyTest>>testDataIs15ThenIncrement` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 12 | `EmptyTest>>test1` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 13 | `EmptyTest>>test2` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 14 | `EmptyTest>>test3` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 15 | `EmptyTest>>test4` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 16 | `FFIExternalStructurePlatformTest>>testStructureHasCorrectOffsets32bits` | SKIP | none | NA-platform-32bit | - | SKIP | SKIP |
| 17 | `FFIExternalStructurePlatformTest>>testStructureHasCorrectSize32bits` | SKIP | none | NA-platform-32bit | - | SKIP | SKIP |
| 18 | `FailThenPassTest>>testFailAtFirstRunning` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 19 | `FailThenPassTest>>testPass` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 20 | `GreenTest>>testPass` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 21 | `GreenTest>>testPassAgain` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 22 | `IntegerTest>>testCreationFromBytes1` | SKIP | none | NA-platform-32bit | - | SKIP | SKIP |
| 23 | `IntegerTest>>testCreationFromBytes2` | SKIP | none | NA-platform-32bit | - | SKIP | SKIP |
| 24 | `IntegerTest>>testCreationFromBytes3` | SKIP | none | NA-platform-32bit | - | SKIP | SKIP |
| 25 | `KMCombinationTest>>testWindowsAndUnixMetaShouldBeCtrl` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP | SKIP |
| 26 | `LongTest>>testIsTakingTooLong` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 27 | `RTFFakeTestClassTest>>testCallingSuperHelper` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 28 | `RTFFakeTestClassTest>>testFailingButNotConsideredAsRotten` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 29 | `RTFFakeTestClassTest>>testHelperHelperAssertHelper` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 30 | `RTFFakeTestClassTest>>testWithAssertionInHelperRun` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 31 | `RTFFakeTestClassTest>>testWithAssertionInSuperHelperRun` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 32 | `RTFFakeTestClassTest>>testWithAssertionRun` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 33 | `RTFFakeTestClassTest>>testWithHelperHelper` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 34 | `RTFFakeTestClassTest>>testWithNoAssertionInHelperRun` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 35 | `RTFFakeTestClassTest>>testWithNoAssertionInSuperHelperRun` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 36 | `RTFFakeTestClassTest>>testWithoutAssertionRun` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 37 | `RedTest>>testFail` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 38 | `RedTest>>testPass` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 39 | `RedTest>>testWithError` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 40 | `TFBasicTypeSizeTest>>testSizeSignedLongWindows` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP 2x | SKIP 2x |
| 41 | `TFBasicTypeSizeTest>>testSizeUnsignedLongWindows` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP 2x | SKIP 2x |
| 42 | `TFBasicTypeTest>>testSignedLongWindows` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP 2x | SKIP 2x |
| 43 | `TFBasicTypeTest>>testUnsignedLongWindows` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP 2x | SKIP 2x |
| 44 | `TFFunctionCallTest>>testCallingFunctionWithW64CallingConvention` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP 2x | SKIP 2x |
| 45 | `TFUFFIBasicTypeSizeTest>>testSizeSignedLongWindows` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP 2x | SKIP 2x |
| 46 | `TFUFFIBasicTypeSizeTest>>testSizeUnsignedLongWindows` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP 2x | SKIP 2x |
| 47 | `TFUFFIDifferentCallingConventionFunctionCallTest>>testCallingFunctionWithW64CallingConvention` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP 2x | SKIP 2x |
| 48 | `Win32WideStringTest>>testCharactersAreEncodedInUnicode16Bits` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP | SKIP |
| 49 | `Win32WideStringTest>>testConvertingInBothDirectionsGaveSameString` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP | SKIP |
| 50 | `Win32WideStringTest>>testHandleIsAByteArray` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP | SKIP |
| 51 | `Win32WideStringTest>>testUnderlayingByteArrayEndsInTwoZeros` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP | SKIP |
| 52 | `Win32WideStringTest>>testUnderlayingByteArrayIsMultipleOf2` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP | SKIP |
| 53 | `WinPlatformTest>>testGetTempPathFromTMP` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP | SKIP |
| 54 | `WindowsResolverTest>>testResolveTempPathFromTMP` | SKIP | none | NA-platform-windows | 20-NA-platform-windows.st | SKIP | SKIP |
| 55 | `YellowTest>>testFail` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |
| 56 | `YellowTest>>testPass` | SKIP | none | NA-fixture-tests | - | SKIP | SKIP |

## Part A4: need an interactive session, 28 tests

The fixes are in `scripts/image-fixes/22-interactive-session-tests.st`.  The
interactive pass is the run of the six classes with `PHARO_INTERACTIVE=1` at
the end of each sweep of our VM.

| # | Test | Before | Last sweep, our VM | Last sweep, patched Cog | Interactive pass, our VM |
|---|---|---|---|---|---|
| 1 | `MorphicNativeWindowTest>>testMaximizeWindow` | SKIP | SKIP | SKIP | PASS |
| 2 | `MorphicNativeWindowTest>>testMinimizeWindow` | SKIP | SKIP | SKIP | PASS |
| 3 | `MorphicNativeWindowTest>>testNewWindowDefaultsIsNotMaximize` | SKIP | SKIP | SKIP | PASS |
| 4 | `MorphicNativeWindowTest>>testNewWindowDefaultsIsNotMinimized` | SKIP | SKIP | SKIP | PASS |
| 5 | `MorphicNativeWindowTest>>testRestoreWindow` | SKIP | SKIP | SKIP | PASS |
| 6 | `MorphicWindowManagerTest>>testDeleteAllWindowToLeftOfAWindow` | SKIP | SKIP | SKIP | PASS |
| 7 | `MorphicWindowManagerTest>>testDeleteAllWindowToRightOfAWindow` | SKIP | SKIP | SKIP | PASS |
| 8 | `MorphicWindowManagerTest>>testDeleteAWindowAndTaskBarActualized` | SKIP | SKIP | SKIP | PASS |
| 9 | `OpenToolTest>>testImplementorsOfAt` | SKIP | SKIP | SKIP | PASS |
| 10 | `OpenToolTest>>testInspectArray` | SKIP | SKIP | SKIP | PASS |
| 11 | `OpenToolTest>>testInspectClass` | SKIP | SKIP | SKIP | PASS |
| 12 | `OpenToolTest>>testInspectDictionary` | SKIP | SKIP | SKIP | PASS |
| 13 | `OpenToolTest>>testInspectInteger` | SKIP | SKIP | SKIP | PASS |
| 14 | `OpenToolTest>>testInspectTraitClass` | SKIP | SKIP | SKIP | PASS |
| 15 | `OpenToolTest>>testOpenBrowseOnClass` | SKIP | SKIP | SKIP | PASS |
| 16 | `OpenToolTest>>testOpenBrowseOnInstalledTraitMethod` | SKIP | SKIP | SKIP | PASS |
| 17 | `OpenToolTest>>testOpenBrowseOnMethod` | SKIP | SKIP | SKIP | PASS |
| 18 | `OpenToolTest>>testOpenBrowseOnTraitMethod` | SKIP | SKIP | SKIP | PASS |
| 19 | `OpenToolTest>>testSendersOfAt` | SKIP | SKIP | SKIP | PASS |
| 20 | `SimulateMouseTest>>testSimulateClick` | SKIP | SKIP | SKIP | PASS |
| 21 | `SimulateMouseTest>>testSimulateMiddleClick` | SKIP | SKIP | SKIP | PASS |
| 22 | `SimulateMouseTest>>testSimulateRightClick` | SKIP | SKIP | SKIP | PASS |
| 23 | `SpTextPresenterTest>>testPropagateNaturalHeight` | SKIP | PASS | PASS | PASS |
| 24 | `SpTextPresenterTest>>testPropagateNaturalHeightWithMultipleLines` | SKIP | SKIP | SKIP | PASS |
| 25 | `SpTextPresenterTest>>testPropagateNaturalWidth` | SKIP | PASS | PASS | PASS |
| 26 | `SpCodePresenterTest>>testPropagateNaturalHeight` | SKIP | PASS | PASS | PASS |
| 27 | `SpCodePresenterTest>>testPropagateNaturalHeightWithMultipleLines` | SKIP | SKIP | SKIP | PASS |
| 28 | `SpCodePresenterTest>>testPropagateNaturalWidth` | SKIP | PASS | PASS | PASS |

## Part B: tests found during the work, 18 tests

The sweeps and the package run found these tests.  Some are new tests that a
fix entry adds.  Two tests are in packages, not in the suite of the image, so
the sweeps do not run them ("not run").

| # | Test | Group in the record | Our VM | Patched Cog |
|---|---|---|---|---|
| 1 | `TKTCommonQueueWorkerPoolTest>>testWorkerPoolDoesNotExceedPoolSizeWhenSchedulingTasksInParallel` | TaskIt-worker-pool-race (added after the repair run) | PASS | PASS |
| 2 | `TKTFutureTest>>testFutureFirstCompleteOfSuccessIfFirstSuccessFaster` | TaskIt-worker-pool-race (added after the repair run) | PASS | PASS |
| 3 | `AtomicSharedQueueTest>>testSecondReaderGetsItemWhenFirstReaderHadTheLock` | AtomicSharedQueue-wakeups (added after the repair run) | PASS | PASS |
| 4 | `AtomicSharedQueueTest>>testReaderGetsItemThatCameWhileItResetTheSignals` | AtomicSharedQueue-wakeups (added after the repair run) | PASS | PASS |
| 5 | `AtomicSharedQueueTest>>testQueueIsFreeAfterReaderIsTerminatedInARead` | AtomicSharedQueue-wakeups (added after the repair run) | PASS | PASS |
| 6 | `OCParseCodeSnippetTest>>testSimpleFormattedCode` (23 snippets) | Simple-formatter-comments (added after the repair run) | PASS 353x | PASS 353x |
| 7 | `CodeImporterCodeSnippetTest>>testCodeImporter` (3 snippets) | Code-importer-snippets (added after the repair run) | PASS 353x | PASS 353x |
| 8 | `ChanelTestCaseNameCleanerTest>>testRenameTestCaseEndingWithTestsUpdateReferences` (package Chanel) | Rename-class-changes (found by the package run) | not run | not run |
| 9 | `RBTransformationsTest>>testRenameClassTransform` | Rename-class-changes (found by the package run) | PASS | PASS |
| 10 | `ReRenameClassCommandTest>>testRenameClassCommandSuccess` | Rename-class-changes (found by the package run) | PASS | PASS |
| 11 | `DurationTest>>testAsDelayDoesNotRoundDown` (new) | Duration-wait-rounds-up (found by the package run) | PASS | PASS |
| 12 | `TkThreadTest>>testPeriodFixedDuration` (package PharoMisc, Tasks) | Duration-wait-rounds-up (found by the package run) | not run | not run |
| 13 | `LinkInstallerTest>>testUninstallVisitsEveryNodeWhenTheNodeSetCollides` (new) | Metalink-uninstall-order (found in a sweep of our VM) | PASS | PASS |
| 14 | `LinkInstallerTest>>testUninstallDoesNotRemoveTheMethodTwiceAfterASkippedNode` (new) | Metalink-uninstall-order (found in a sweep of our VM) | PASS | PASS |
| 15 | `LinkInstallerTest>>testLinkOnTempVarForObject` and each later test of the class in that run | Metalink-uninstall-order (found in a sweep of our VM) | PASS | PASS |
| 16 | `ZnUrlTest>>testEqualsWithQueryFieldsInAnotherOrder` (new, 8 cases; each case has the answer of Pharo 13) | Znurl-query-order (found by the package run) | PASS | PASS |
| 17 | `TKTFutureTest>>testFutureAndThenExecutesInOrder` | Taskit-andthen-order (found by a sweep) | PASS | PASS |
| 18 | `TKTNewProcessTaskRunnerTest>>testNewProcessTaskRunnerRunsInNewProcessEveryTime` | open: draft entry 36, not validated | FAIL | PASS |

## Part C: tests that failed at times before 2026-11-17, 7 tests

These tests are not in the 220.  Each one failed in one or more sweeps before
2026-11-17, on our VM and in most cases on Cog too.  Each one passes in the 6
saved sweeps of 2026-11-17 (4 of our VM, 2 of the patched Cog).  These
sweeps use the JIT build: no sweep of `build-nojit` ran after 2026-11-15.  No
fix entry exists.  The rule of the owner counts a test that fails at times as a test
that fails.

| # | Test | Where it failed | Source |
|---|---|---|---|
| 1 | `StSpotterTest>>testOpenSpotterRefreshesPreviewOnce` | Pharo 14 `build-nojit`, 1 of 4 sweeps, and 1 of 4 sweeps before that; stock Cog too | [WIP.md](WIP.md) ("Stack-depth fix and interpreter-only sweeps"), [vm-compat-bugs.md](vm-compat-bugs.md) |
| 2 | `StPlaygroundPageTest>>testContents` | Pharo 14, earlier sweeps, timing-dependent | [WIP.md](WIP.md), the sweeps at `d5323560` |
| 3 | `FreeTypeCacheTest>>testGlyphAccessIsThreadSafe` | Pharo 14 arm64: our VM 11 of 50 runs, stock Cog 9 of 50 runs (a race in the image on the lazy FreeType face); Pharo 13 too | [WIP.md](WIP.md), [test-results.md](test-results.md) |
| 4 | `FreeTypeCacheTest>>testGlyphAccessIsThreadSafeWhenAskingWidth` | timeouts in earlier sweeps | [test-results.md](test-results.md), [vm-compat-bugs.md](vm-compat-bugs.md) #19 |
| 5 | `StObjectTranscriptPresenterTest>>testOpenedLogIsSelectedLog` | Pharo 14 x86_64: about 3 of 1000 runs on our VM and on Cog (two processes run `TabSelectorMorph>>updateTabs` on one morph) | [WIP.md](WIP.md); the gate in the fake GUI is the repair in test |
| 6 | `NoUnusedVariablesLeftTest>>testNoUnusedTemporaryVariablesLeft` | `build-nojit` (JIT off, the iOS configuration): timeout, 73 s at the 80 s limit of the old runner | [WIP.md](WIP.md) ("Stack-depth fix and interpreter-only sweeps") |
| 7 | `ReleaseTest>>testNoShadowedVariablesInMethods` | `build-nojit`: timeout, 270 s | same |

## Package run: tests that do not pass, rows 1 to 163

The package run loads each package of `scripts/pkg-jit-test/packages-200.tsv`
into Pharo 14 and runs the tests of the package on stock Cog and on our VM.
Rows 164 to 200 did not run: the reboot of 2026-11-17 stopped the run at row
164.  The package run started after the measurement before the repairs, so no list of
the package tests that failed before the repairs exists in one place.  The
repairs of each row are in
[upstream/package-repairs.md](upstream/package-repairs.md).

Of the 291 tests in the list, 35 cannot apply (with a proof in
`scripts/pkg-jit-test/not-applicable/`), and 256 are open.  79 of the 256
wait on a decision of the owner (rows 57, 71, 75 and 113).  The state of
each row:

| Row | Package | Not passing | Cannot apply | Open | Owner decision | State | Next step |
|---|---|---|---|---|---|---|---|
| 57 | `fouziray-escapeanalysispharo` | 1 | 0 | 1 | yes | The author skips the test with self skip; the analyzer says two objects escape, the test says they do not, and the test is right by definition. A repair changes the answer of testTempInBlock, testReturnTempInBlock and testArgBlockReturnsTemp, which assert the careful answer. | The package author decides which answer is right; until then the skip is not accepted (row not clean). Under the owner's rule a fix entry could change the three sibling tests too, but no entry for this was made. |
| 70 | `hernanmd-biosmalltalk` | 7 | 7 | 0 | - | The class tests an adapter for a Glamorous Toolkit class; the setUp skips with that reason. Proven in not-applicable/hernanmd-biosmalltalk.txt (7 lines). | None. Row is clean: 1198 pass, 7 skips on both VMs. |
| 71 | `pillar-markup-pillar` | 4 | 0 | 4 | yes | The 4 tests start with self skip and run latexmk with lualatex; the machine has no TeX. Behind them the baseline pins OSSubprocess v2.0.0, which calls OSEnvironment>>defaultEncoding, missing in Pharo 14 (master has the repair). No not-applicable file exists, so none counts as proven. | Owner decides whether TeX goes on the machine; then repair the OSSubprocess pin and the 4 tests. |
| 75 | `punt-labs-anthropic-sdk-pharo` | 1 | 0 | 1 | yes | On macOS the guard asks isUnix and skips; with the right guard the test writes a secret into the owner's login keychain and removes it in tearDown. Not repaired, because writing into a keychain needs the word of the owner. | Owner decides if the test may write to a keychain (or a fixture keychain is made). |
| 82 | `evref-bl-mcp` | 1 | 1 | 0 | - | Pharo 14 WorldState>>runStepMethodsIn: does not send stepGlobal, so the World no longer steps the Transcript; the test skips itself. Proven in not-applicable/evref-bl-mcp.txt. | None. Row is clean on both VMs (1166 pass, 1 skip). |
| 84 | `apptivegrid-soil` | 17 | 14 | 3 | - | 14 are proven not applicable (8 SoilWindowsFileLockTest with kernel32 FFI, 6 SoilIndexIteratorTest parametrized cases with no meaning). The 3 testCodeCoverage tests (SoilCleanCodeTest, SoilResidentCleanCodeTest, SoilSerializerCleanCodeTest) ask 93 percent; measured 86, 85, 81 percent. | Write tests for about 210 methods to reach 93 percent in Soil-Core, Soil-Resident and Soil-Serializer, or the package author decides on the limit. A round-2 file exists in scratch/ag/round2/apptivegrid-soil.st (not checked against the repo). |
| 89 | `zeroflag-teapot` | 1 | 0 | 1 | - | The test needs an HTTPS server in the image; the SSL plugin of both VMs takes a server certificate only from a keychain on macOS. WIP table line 380 says 'Not started', but the reboot table says branch ssl-pem-identity has 1 commit (8b79c33d, verified in git) and an uncommitted review change of sqMacSSL.c is only in the saved copy. | Read scratch/ssl89 (report.md is NOT in the saved copy: UNKNOWN), apply the saved diff, finish review, build, run row 89, merge; then give the Cog patch scratch/ssl89/cog-squeakssl-mac-pem-identity.patch to the owner. |
| 113 | `svenvc-p3` | 73 | 0 | 73 | yes | 73 tests need a PostgreSQL server; P3ClientTest class>>url names host 'postgres' (a CI service container) and the lookup fails. No not-applicable file is in the repo (a draft svenvc-p3.not-applicable.txt is only in scratch/ag/pkgfinal). The 3 timeouts are a 10 x 5 s retry of the name lookup failure. One package defect (retryDelay:) is repaired. | Owner decides on a PostgreSQL server on 127.0.0.1:5432 (database, user, password postgres); then tests need host 127.0.0.1 in their URL. A stand-in was rejected as not an honest test. |
| 119 | `shnarazk-aoc-in-pharo` | 6 | 6 | 0 | - | The tests read puzzle input files of the author under a data directory; the repository has none and Advent of Code forbids copying inputs. Proven in not-applicable/shnarazk-aoc-in-pharo.txt. The author's day 8 part2 answers 0, so testPart2 could not pass even with the input. | None. Row is clean: 14 pass, 6 skips on both VMs. |
| 121 | `erikonbike-codeparadise` | 7 | 7 | 0 | - | The code under test is client side and runs in a small browser image; the package refuses it in an image with the server classes. Proof for each is in not-applicable/erikonbike-codeparadise.txt (67 lines). | None. Row is clean: 26 pass, 7 skips on both VMs (commit f5822a29). Note: testSortedCollection passes with a stand-in environment and is listed as not applicable only because the repair uses no stand-in. |
| 124 | `opensmock-iconography` | 1 | 0 | 1 | - | Defect in our VM, measured: primitiveWarpBits does not step and pick pixels as the stock BitBltSimulation>>warpLoop (138 of 992 words differ); stock Cog passes 32 of 32. A second defect: alpha blend of rules 24 and 34 rounds differently. | Restart the workflow vm-warpblt-repair-wf_34a48e2b-34f.js on branch warpblt-stock-algorithm (1058-line port, not built; branch exists, no commit); build, then run scripts/cogdiff/bitblt_warp_pixels.st and the row. |
| 126 | `moosetechnology-famix-cpp` | 10 | 0 | 10 | - | The CSV parser and importer (Famix-Cpp-Parser) name classes the metamodel no longer has (FamixCppType, FamixCppFileAnchor, FamixCppSourcedEntity); the tests are marked expected failure. Same on both VMs. Not counted as accepted. | Finish the port of the parser and importer to the present metamodel; the port is said to be in work but no file of it is in the saved copy (UNKNOWN where). Rerun with the second-round workflow package-rows-second-round-wf_99e9fd7b-897.js. |
| 138 | `moosetechnology-fast` | 2 | 0 | 2 | - | Famix commit bcc80b10 removed the <derived> pragma from the fixture; the FAST tests are stale. A round-2 draft (scratch/ag/round2/moosetechnology-fast.st) forces isDerived: true in the two tests; its probe passes 57 of 57 on both VMs, but it is NOT in scripts/pkg-jit-test/fixes (the repo file is the round-1 one) and was not run as a full row. | Copy round2/moosetechnology-fast.st to scripts/pkg-jit-test/fixes/moosetechnology-fast.st, rerun row 138 with scripts/vmrun.sh pkgs --redo; give the FAST/Famix change upstream to the owner for review. |
| 146 | `jordanmontt-pharo-instrumentation` | 5 | 0 | 5 | - | The first repair (action source from block, onProfiler:, unproxifiedMethod) took 13 away. The 5 heavy GraphCallerTest tests give a count of 1. A round-2 draft (scratch/ag/round2/jordanmontt-pharo-instrumentation.st, adds resetSharedState to clear the Microdown resource cache and a warm-up run) is not in the repo and has no report .md; cause is a draft, not proven. | Validate the round-2 draft: copy it to scripts/pkg-jit-test/fixes/jordanmontt-pharo-instrumentation.st, rerun row 146 on both VMs; if the VMs differ, treat as a VM defect. |
| 147 | `svenvc-neoconsole` | 2 | 0 | 2 | - | Our VM only: ConnectionClosed to the package's telnet server on port 4999; stock Cog passes 7 of 7, ours 5. The eval-mode change did not repair it. Cause not found; no round-2 file exists. | Do this row first (a VM difference): reproduce under vmrun.sh on our VM and read the socket/server side; start the second-round workflow for row 147. |
| 151 | `kentbeck-smalltalkgenie` | 2 | 0 | 2 | - | Same on both VMs; package uses Pharo 13 API: Context>>debugStack:on: is gone, and the stale RBTemporaryToInstanceVariableTransformation breaks under the Pharo 14 compiler (OCCompilationError). Repair fixes/kentbeck-smalltalkgenie.st is committed (f433b127) but NOT run; confidence from reading only. | scripts/vmrun.sh pkgs --redo /tmp/pharo-work/pkgs1 151 151; later steps of the refactoring test were never reached and can show a second defect. |
| 153 | `reugalabf-openweatherapi4pharo` | 3 | 0 | 3 | - | The baseline lacks the dependency DatasetUtilities (CSV2JSONStream etc.); both VMs error identically. Patch patches/reugalabf-openweatherapi4pharo.patch is committed (f433b127) but NOT run; the date comparison in testCSVOneRecordAsHistoryFlatBulk is only inferred. | scripts/vmrun.sh pkgs --redo /tmp/pharo-work/pkgs1 153 153; expect 11 of 11. |
| 154 | `tomooda-viennatalk` | 1 | 0 | 1 | - | ViennaConstrainedType>>name takes the block source from printString; Pharo 14 BlockClosure>>printOn: prints '[ ] in Class>>selector'. Repair fixes/tomooda-viennatalk.st (name uses sourceNode sourceCode) is committed but NOT run. | scripts/vmrun.sh pkgs --redo /tmp/pharo-work/pkgs1 154 154; expect 1430 of 1430. |
| 158 | `opensmock-pyramid` | 5 | 0 | 5 | - | No analysis was made; cause UNKNOWN. The 5 tests are PyramidCopyPastePluginTest>>testCopyAsStonMenu, 3 PyramidEditorTest and PyramidPluginEditOnRunningTest>>testBlSpaceShortcutAddAndRemove, skipped on both VMs. No not-applicable file. | Run the analyse workflow package-rows-analyse-verify-wf_8c002123-bea.js for row 158 (read the skip reasons; then repair, or prove not applicable). |
| 159 | `pharo-vcs-iceberg` | 3 | 0 | 3 | - | No analysis; cause UNKNOWN. State BOTH-INCOMPLETE: the run ends after 305 tests on both VMs (cog_exit 1), so tests after the stop were never run and the true count of non-passing tests is UNKNOWN (3 is only what ran). | Run the analyse workflow for row 159: first find why the run stops after 305 tests (read pharo-vcs-iceberg.cog.status and the logs), then the 3 tests. |
| 160 | `moosetechnology-mooseide` | 104 | 0 | 104 | - | No analysis; cause UNKNOWN. 104 tests in 23 Mi*BrowserTest classes skip on both VMs; no not-applicable file. Whether they skip on a missing World or by the author's self skip is UNKNOWN. | Run the analyse workflow for row 160: read the skip texts, then repair or prove not applicable per test. |
| 163 | `alamvic-druid` | 35 | 0 | 35 | - | No analysis; cause UNKNOWN. 34 errors in 13 DR*Test classes (DRSCCP*, DRSSAPRE*, DRDeadCodeEliminationTest and others) and 1 expected failure, same on both VMs. | Run the analyse workflow for row 163: read the error messages, then repair; the expected failure also needs a fix by the owner's rule. |

Rows that did not load, so no test ran:

- Rows 51, 62, 66 and 135 (`georghagn-tsf-scheduler`, `-filerotator`,
  `-logger`, `-nexio`): the four GitHub repositories are not public any more
  ("Repository not found").  The rows can run if the owner makes the
  repositories public ([upstream/package-repairs.md](upstream/package-repairs.md)).
- Row 150 (`pharo-llm-pharo-agent`): the repository is a TypeScript program
  with no Smalltalk package.  The owner can name another package for the row.
- Row 155 (`pharo-spec-spec`): the manifest loaded the branch `Pharo13` into a
  Pharo 14 image.  The repair (branch `Pharo14`) is committed in `f433b127`
  and did not run.
- Row 156 (`pharo-spec-newtools`): the branch `Pharo13` has no package
  `NewTools-Refactoring-Commands`.  The repair (tag `v0.11.0`, the tag of the
  image) is committed in `f433b127` and did not run.
- Row 157 (`pharo-graphics-bloc`): the baseline has no group `tests`.  The
  repair (`load: #('default')`) is committed in `f433b127` and did not run.
- Rows 155 to 157 need `LOAD_TIMEOUT=2400 TEST_TIMEOUT=3600`.

The tests of each row (one result if the two VMs agree, otherwise both):

### Row 57: fouziray-escapeanalysispharo (1 test)

- `EscapeTest` (1): `testComplexWeighterGraph` SKIP

### Row 70: hernanmd-biosmalltalk (7 tests)

- `BioGtUnixSubprocessTest` (7): `testExecuteCommandChained` SKIP, `testExecuteCommandEcho` SKIP, `testExecuteCommandFailureReturnsErrorOutput` SKIP, `testExecuteCommandParameter` SKIP, `testExecuteCommandPipeline` SKIP, `testExecuteCommandQuotedParameter` SKIP, `testExecuteCommandStderrOutput` SKIP

### Row 71: pillar-markup-pillar (4 tests)

- `PRIntegrationTest` (4): `testBasicLatexPdf` SKIP, `testBookPdf` SKIP, `testPresentationPdf` SKIP, `testWelcomePdf` SKIP

### Row 75: punt-labs-anthropic-sdk-pharo (1 test)

- `PharoKeyringIntegrationTest` (1): `testLookupProvisionedSecret` SKIP

### Row 82: evref-bl-mcp (1 test)

- `MCPToolStructuredOutputTest` (1): `testEvaluateTakesTheStepTheWorldGivesTheTranscript` SKIP

### Row 84: apptivegrid-soil (17 tests)

- `SoilCleanCodeTest` (1): `testCodeCoverage` SKIP
- `SoilIndexIteratorTest` (6): `testMoveKey(#classToTest->SoilBTree_#uniqueKeys->false)` SKIP, `testMoveKey(#classToTest->SoilSkipList_#uniqueKeys->false)` SKIP, `testMoveKeyOnNonUniqueIndexIsRefused(#classToTest->SoilBTree_#uniqueKeys->true)` SKIP, `testMoveKeyOnNonUniqueIndexIsRefused(#classToTest->SoilSkipList_#uniqueKeys->true)` SKIP, `testMoveKeyOverwritesTheTarget(#classToTest->SoilBTree_#uniqueKeys->false)` SKIP, `testMoveKeyOverwritesTheTarget(#classToTest->SoilSkipList_#uniqueKeys->false)` SKIP
- `SoilResidentCleanCodeTest` (1): `testCodeCoverage` SKIP
- `SoilSerializerCleanCodeTest` (1): `testCodeCoverage` SKIP
- `SoilWindowsFileLockTest` (8): `testFlushFileBuffersDirect` SKIP, `testFsyncEntireFile` SKIP, `testLockAndUnlockEntireFile` SKIP, `testLockAndUnlockRange` SKIP, `testLockExclusive` SKIP, `testLockOrErrorSuccess` SKIP, `testLockShared` SKIP, `testMultipleLocks` SKIP

### Row 89: zeroflag-teapot (1 test)

- `TeapotSslTest` (1): `testHttpsGet` SKIP

### Row 113: svenvc-p3 (73 tests)

- `P3ClientTest` (42): `testArrayTypes` ERROR, `testBitStringConversion` ERROR, `testBooleanConversion` ERROR, `testBoxConversion` ERROR, `testByteArrayConversion` ERROR, `testChronology` ERROR, `testChronologyWithTimezone` ERROR, `testCircleConversion` ERROR, `testConnection` ERROR, `testConstraints` ERROR, `testConvenienceMetaAccess` ERROR, `testCustomCompositeType` ERROR, `testEmptyString` ERROR, `testEnum` ERROR, `testError` ERROR, `testIntegerConversion` ERROR, `testIntegerVector` ERROR, `testIntervalConversion` ERROR, `testJsonConversion` ERROR, `testLineConversion` ERROR, `testLineSegmentConversion` ERROR, `testMultiStatement` ERROR, `testMultipleCommandsInOneRequest` ERROR, `testNonAsciiStrings` ERROR, `testNotification` ERROR, `testPathConversion` ERROR, `testPointConversion` ERROR, `testPolygonConversion` ERROR, `testQueryCatalog` ERROR, `testSession` ERROR, `testSimpleTable` ERROR, `testSqlFunction` ERROR, `testStringEscaping` ERROR, `testTimeConversion` ERROR, `testTimeout` ERROR, `testTimezone` ERROR, `testTimezoneZTimestamp` ERROR, `testUuid` ERROR, `testValuesArray1` ERROR, `testValuesArray2` ERROR, `testZTimestamp` ERROR, `testZTimestampWithTimezone` ERROR
- `P3ConnectionPoolTest` (3): `testError` TIMEOUT, `testOne` TIMEOUT, `testWarmUp` TIMEOUT
- `P3FormattedStatementTest` (11): `testBitString` ERROR, `testByteArray` ERROR, `testChronology` ERROR, `testChronologyWithTimezone` ERROR, `testGeometricTypes` ERROR, `testInClause` ERROR, `testInterval` ERROR, `testNull` ERROR, `testPrintOn` ERROR, `testSimple` ERROR, `testStrings` ERROR
- `P3PreparedStatementTest` (17): `testBatchInsertStatement` ERROR, `testBinaryColumn` ERROR, `testBitString` ERROR, `testBulkInsertAndSelect` ERROR, `testChronology` ERROR, `testChronologyWithTimezone` ERROR, `testChronologyZTimestamp` ERROR, `testGeometricTypes` ERROR, `testInClause` ERROR, `testInsertNull` ERROR, `testInterval` ERROR, `testLongQueries` ERROR, `testMultipleInsertStatements` ERROR, `testNamedPreparedStatement` ERROR, `testScaledDecimal` ERROR, `testSelectStatement` ERROR, `testSimpleInsertStatement` ERROR

### Row 119: shnarazk-aoc-in-pharo (6 tests)

- `Y2025Day05Test` (3): `testParser` SKIP, `testPart1` SKIP, `testPart2` SKIP
- `Y2025Day08Test` (3): `testParser` SKIP, `testPart1` SKIP, `testPart2` SKIP

### Row 121: erikonbike-codeparadise (7 tests)

- `CpClientEnvironmentTest` (4): `testInstallClass` SKIP, `testInstallMethod` SKIP, `testInstallMethodWithClassVarAccess` SKIP, `testInstallMethodWithInstVarAccess` SKIP
- `CpEncodeDecodeObjectTest` (3): `testCompiledMethod` SKIP, `testCompiledMethodWithAdditionalMethodState` SKIP, `testSortedCollection` SKIP

### Row 124: opensmock-iconography (1 test)

- `AntDesignIconsLibraryTest` (1): `testSerializeAsFormMaxWidthMaxHeight` Cog PASS, our VM FAIL

### Row 126: moosetechnology-famix-cpp (10 tests)

- `FamixCppCSVImporterTest` (1): `testOnlyDeclarations` XFAIL
- `FamixCppCSVParserTest` (9): `testClassDataLine` XFAIL, `testFunctionCreationDependingOnParent` XFAIL, `testFunctionCreationWithBadConstructor` XFAIL, `testFunctionCreationWithBadDestructor` XFAIL, `testFunctionCreationWithCommaAroundName` XFAIL, `testFunctionWithLOCandCyclo` XFAIL, `testImportTypesInNamspaces` XFAIL, `testImportTypesPointerReference` XFAIL, `testMultiFileAnchorCreation` XFAIL

### Row 138: moosetechnology-fast (2 tests)

- `FASTCopyVisitorCodeGeneratorTest` (2): `testShouldCopyMultiValuedDerivedProperty` FAIL, `testShouldNotCopyMultivaluedDerivedProperty` FAIL

### Row 146: jordanmontt-pharo-instrumentation (5 tests)

- `GraphCallerTest` (5): `testFuzzyMatcher` FAIL, `testGraphAlgos` FAIL, `testMicrodown` FAIL, `testProfStef` FAIL, `testRewriteTool` FAIL

### Row 147: svenvc-neoconsole (2 tests)

- `NeoConsoleTelnetServerTests` (2): `testGetSystemTimestamp` Cog PASS, our VM ERROR, `testSumMultiline` Cog PASS, our VM ERROR

### Row 151: kentbeck-smalltalkgenie (2 tests)

- `GenieActionTest` (1): `testVariableRefactoringToolsTemporaryRefactorings` FAIL
- `GenieProtocolTest` (1): `testToolErrorsExposeDebugDetailsOnlyWhenExplicitlyEnabled` ERROR

### Row 153: reugalabf-openweatherapi4pharo (3 tests)

- `OpenWeatherHistoryFlatBulkTest` (3): `testCSVOneRecord` ERROR, `testCSVOneRecordAsDictionary` ERROR, `testCSVOneRecordAsHistoryFlatBulk` ERROR

### Row 154: tomooda-viennatalk (1 test)

- `ViennaConstrainedTypeTest` (1): `testName` FAIL

### Row 158: opensmock-pyramid (5 tests)

- `PyramidCopyPastePluginTest` (1): `testCopyAsStonMenu` SKIP
- `PyramidEditorTest` (3): `testOpenFromBlSpace` SKIP, `testOpenFromBlSpaceWithSpacePluginUninstalled` SKIP, `testOpenNewEditor` SKIP
- `PyramidPluginEditOnRunningTest` (1): `testBlSpaceShortcutAddAndRemove` SKIP

### Row 159: pharo-vcs-iceberg (3 tests)

- `IceCherryPickBrowserTest` (1): `testWindowOnMethodConflict` SKIP
- `IceClassesCherryPickingTest` (1): `testAddClassDependingInATrait` FAIL
- `IceMemoryRepositoryWithSingleRemoteTest` (1): `testPushPushesTags` SKIP

### Row 160: moosetechnology-mooseide (104 tests)

- `MiAccumulatorBrowserTest` (5): `testCanOpenSettings` SKIP, `testHighlightEntity` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP
- `MiArchitecturalMapBrowserTest` (2): `testFollowEntity` SKIP, `testHighlightEntity` SKIP
- `MiBusLogBrowserTest` (9): `testCanOpenSettings` SKIP, `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testPropagateToOtherBrowser` SKIP, `testSelectedTagSetAndGet` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP, `testTagMenuListsTags` SKIP
- `MiButterflyMapBrowserTest` (2): `testFollowEntity` SKIP, `testHighlightEntity` SKIP
- `MiCoUsageMapBrowserTest` (5): `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testSelectedTagSetAndGet` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickReset` SKIP
- `MiCriticBrowserTest` (5): `testCanOpenSettings` SKIP, `testHighlightEntity` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP
- `MiDSMBrowserTest` (4): `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testSelectedTagSetAndGet` SKIP, `testTagMenuListsTags` SKIP
- `MiDeadCodeBrowserTest` (3): `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testPropagateToOtherBrowser` SKIP
- `MiDistributionMapBrowserTest` (2): `testFollowEntity` SKIP, `testHighlightEntity` SKIP
- `MiDuplicationBrowserTest` (1): `testHighlightEntity` SKIP
- `MiExportBrowserTest` (6): `testCanOpenSettings` SKIP, `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP
- `MiFilesBrowserTest` (8): `testCanOpenSettings` SKIP, `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testSelectedTagSetAndGet` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP, `testTagMenuListsTags` SKIP
- `MiHeatMapBrowserTest` (2): `testFollowEntity` SKIP, `testHighlightEntity` SKIP
- `MiInspectorBrowserTest` (6): `testCanOpenSettings` SKIP, `testHighlightEntity` SKIP, `testNavigationTabIsSorted` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP
- `MiLogsBrowserTest` (9): `testCanOpenSettings` SKIP, `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testPropagateToOtherBrowser` SKIP, `testSelectedTagSetAndGet` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP, `testTagMenuListsTags` SKIP
- `MiModelsBrowserTest` (4): `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testSelectedTagSetAndGet` SKIP, `testTagMenuListsTags` SKIP
- `MiNotebookBrowserTest` (5): `testCanOpenSettings` SKIP, `testHighlightEntity` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP
- `MiQueriesBrowserTest` (7): `testCanOpenSettings` SKIP, `testHighlightEntity` SKIP, `testSelectedTagSetAndGet` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP, `testTagMenuListsTags` SKIP
- `MiSourceTextBrowserTest` (2): `testFollowEntity` SKIP, `testHighlightEntity` SKIP
- `MiSystemComplexityBrowserTest` (4): `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testSelectedTagSetAndGet` SKIP, `testTagMenuListsTags` SKIP
- `MiTagBrowserTest` (8): `testCanOpenSettings` SKIP, `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testSelectedTagSetAndGet` SKIP, `testSettingsClickCancel` SKIP, `testSettingsClickOK` SKIP, `testSettingsClickReset` SKIP, `testTagMenuListsTags` SKIP
- `MiTagCreationFormTest` (1): `testIsConsumerOfFQAbstractQuery` SKIP
- `MiUMLBrowserTest` (4): `testFollowEntity` SKIP, `testHighlightEntity` SKIP, `testSelectedTagSetAndGet` SKIP, `testTagMenuListsTags` SKIP

### Row 163: alamvic-druid (35 tests)

- `DRConditionalConstraintTest` (1): `testNotSupportedCases` XFAIL
- `DRConstantPhiInsertionTest` (1): `testPhiInsertionInConditionalCase5` ERROR
- `DRCopyPropagationTest` (1): `testCopyIsPropagatedInSingleBlockToManyInstructions` ERROR
- `DRDeadCodeEliminationTest` (4): `testDCEOnConditionalJump` ERROR, `testDCEOnDeadEndInstruction` ERROR, `testDCEOnSeveralBlocs` ERROR, `testNotRemoveUnusedMandatoryInstruction` ERROR
- `DRGlobalValueNumberingTest` (1): `testLoadInstructionOperandsAreNotDeteleted` ERROR
- `DRPhiSimplicationTest` (1): `testNotSimplifyPhiWithNoResultInstruction` ERROR
- `DRRedundantCodeEliminationTest` (1): `testRedundantCopyIsEliminated` ERROR
- `DRSCCPConditionalConstantFoldingTest` (4): `testConditionalEvaluationOnNilObject` ERROR, `testConditionalEvaluationOnNullValue` ERROR, `testSCCPPropagationBothWithPhiFunction` ERROR, `testSCCPPropagationWithFarAwayPhiFunctionFinishWithValidCFG` ERROR
- `DRSCCPConstantFoldingTest` (10): `testConstantFoldingAddOfLeftNonConstants` ERROR, `testConstantFoldingAddOfRightNonConstants` ERROR, `testConstantFoldingLoadConstant` ERROR, `testConstantFoldingLoadJitCompileExpression` ERROR, `testConstantFoldingLoadNonConstant` ERROR, `testConstantFoldingMultiplyByZeroLeft` ERROR, `testConstantFoldingMultiplyByZeroRight` ERROR, `testFoldBitShiftWithNonLeftConstant` ERROR, `testFoldingLoadWithConstant` ERROR, `testFoldingLoadWithOperation` ERROR
- `DRSCCPConstantFoldingWithStagingTest` (3): `testConstantFoldingLoadNonConstant` ERROR, `testFoldingLoadWithConstant` ERROR, `testFoldingLoadWithOperation` ERROR
- `DRSCCPConstantPropagationTest` (4): `testDoNotPropagateInPhiFunctions` ERROR, `testPropagateToAddOperand` ERROR, `testPropagateToMulOperand` ERROR, `testPropagateWhenResultIsPhisicalRegister` ERROR
- `DRSSAPREConstantLinearCFGTest` (1): `testPartialRedundancyWithSameConstExpressInConditionCF` ERROR
- `DRSSAPRETest` (3): `testPartialRedundancyWithSameConstIn124BlockInConditionCF` ERROR, `testPartialRedundancyWithSameConstIn12BlockInConditionCF` ERROR, `testPhiInsertionInConditionalCase5` ERROR
