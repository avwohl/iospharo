# Test Failures — Feb 17, 2026 (commit d4d4499)

Batch run: **7636 pass, 52 fail, 637 error, 19 skip, 16 timeout** out of 8360 tests.
Note: ~4140 tests not reached due to batch timeouts (batches 4, 5, 11 hung).
WeakKeyDict tests not in batch run — targeted run below.

## Targeted WeakKeyDict Results (post-finalization fix)

| Class | Pass | Fail | Error | Total |
|---|---|---|---|---|
| WeakKeyDictionaryTest | 197 | 1 | 9 | 207 |
| WeakIdentityKeyDictionaryTest | 201 | 2 | 6 | 209 |

Remaining WeakKeyDict failures:
- `WeakKeyDictionaryTest>>#testClearing` — FAIL
- `WeakKeyDictionaryTest>>#testAssociationAtError` — ERROR
- `WeakKeyDictionaryTest>>#testNoneSatisfy` — ERROR
- `WeakKeyDictionaryTest>>#testIncludesAnyNoneThere` — ERROR
- `WeakKeyDictionaryTest>>#testIncludesElementIsThere` — ERROR
- `WeakKeyDictionaryTest>>#testDoSeparatedBy` — ERROR
- `WeakKeyDictionaryTest>>#testIntersectionBasic` — ERROR
- `WeakKeyDictionaryTest>>#testCollect` — ERROR
- `WeakKeyDictionaryTest>>#testIsHealthy` — ERROR
- `WeakKeyDictionaryTest>>#testAssociationsDo` — ERROR
- `WeakIdentityKeyDictionaryTest>>#testFinalizeValuesWhenLastChainContinuesAtFront` — FAIL
- `WeakIdentityKeyDictionaryTest>>#testClearing` — FAIL
- `WeakIdentityKeyDictionaryTest>>#testEqualSignIsTrueForNonIdenticalButEqualCollections` — ERROR
- `WeakIdentityKeyDictionaryTest>>#testAnySastify` — ERROR
- `WeakIdentityKeyDictionaryTest>>#testIntersection` — ERROR
- `WeakIdentityKeyDictionaryTest>>#testBasicCollectEmpty` — ERROR
- `WeakIdentityKeyDictionaryTest>>#testDictionaryPublicProtocolCompatibility` — ERROR
- `WeakIdentityKeyDictionaryTest>>#testKeyAtValue` — ERROR

Reference Pharo VM gets 207/207 + 209/209 (100%).

---

## Batch Run Failures by Category

### SystemEnvironmentTest (40 errors)
Root cause: `#>` DNU — SystemEnvironment doesn't implement comparison selectors.
```
testAddWithKeyAlreadyIn, testAnySastify, testAsBag, testAsByteArray,
testAsOrderedCollection, testAsSortedCollectionWithSortBlock, testAt,
testAtIfAbsentPut, testClassOrTraitNamedReturnsClassForClasses, testCollect,
testCollectAsWithoutParenthesis, testCopyCreatesNewObject, testCopyEmpty,
testCopyEmptyWith, testCopyEmptyWithoutAll, testCopyWithAll,
testDetectIfFoundWhenNobodyIsFound, testDetectIfFoundWhenSomethingIsFound,
testDictionaryConcatenationWithCommonKeysDifferentValues,
testDifferenceWithNonNullIntersection, testFlatCollect,
testHasBindingThatBeginsWith, testIncludeAssociation,
testIncludesAllNoneThere, testIncludesComportementForDictionnary,
testIncludesIdentitySpecificComportement, testIntersectionBasic,
testKeyAtIdentityValueIfAbsent, testPrintOn, testRejectAllThenCollect,
testRejectEmpty, testRejectThenCollect, testSelect,
testSelectThenDoOnEmpty, testSize, testStoreOnWithNegativeInteger,
testValues, test0FixtureAsStringCommaAndDelimiterTest,
test0FixtureCloneTest, test0FixtureOccurrencesForMultiplinessTest
```

### TraitTest (45 errors)
Root cause: class creation/modification primitives. Tests create new classes at runtime.
```
testAddingATraitToAClassWithSubclasses, testClassHavingAnInstanceVariableUsersDifferenThanUsers,
testClassTraitThatHasAPragmaHasCorrectTraitSourceAfterRecompile,
testClassTraits, testClassUsesTrait (TIMEOUT),
testClassUsingTraitsDoesNotHaveUsers, testDefinedMethods, testDefinedSelectors,
testEmptyCompositionManagesTEmpty, testErrorClassCreation, testIndirectSequence,
testIsUsed, testLocalMethodWithSameCodeInTrait, testMethodsAddedInMetaclass,
testMethodsAddedInMetaclassNotPresentInSubclasses,
testMethodsAddedInMetaclassPresentInSubclassesAfterChangingSuperclass,
testOrigin, testOriginWithRequiredMethod,
testOriginWithRequiredMethodInTraitChain, testPackageIsUpdatedInClassSide,
testPackageIsUpdatedInInstanceSide,
testRecompilingTraitClassMethodRecompilesTheMethodInTheUsers,
testRecompilingTraitMethodRecompilesTheMethodInTheUsers,
testRedefiningATraitAsAClassShouldRaiseError,
testRemakingATraitUsedByAnAnonymousClassKeepItAnonymous,
testRemoveFromSystem, testRemovingTraitsDoesNotModifiyTraitedSubclasses,
testRemovingTraitsRemoveTraitedClassMethods,
testRemovingTraitsRemoveTraitedClassMethodsWithSubclasses,
testRemovingTraitsUpdatesCategories, testSelectorsWithExplicitOrigin,
testSelectorsWithExplicitOriginNoTrait, testSequence,
testSettingAClassInAClassTraitCompositionShouldRaiseAnError,
testSettingEmptyTraitCompositionDoesNotModifiyTraitedSubclasses,
testSettingEmptyTraitCompositionUpdatesMetaclass (TIMEOUT),
testSlotsAreNotDuplicated, testSubclasses,
testTraitHaveUsersInstanceVariable, testTraitRemoval,
testTraitSourceIsPersistedWithRecompilation,
testTraitThatHasAPragmaHasCorrectTraitSourceAfterRecompile,
testTraitUsingTraitsPreserveSourceCode,
testTraitUsingTraitsPreserveSourceCodeOnClassSide,
testUsingTraitInAnonymousSubClassAndRedefiningIt
```

### FBDDecompilerTest (42 errors)
Root cause: bytecode decompiler relies on source code access / compilation.
```
testBlockArgument, testBlockNumCopied, testCascade4, testCascadeIfFalse,
testCascadeIntoBlockWithTempIfTrueIfFalse,
testCascadeIntoBlockWithTempIntoCascade, testCascadeNested, testCaseOf,
testCaseOf4, testCaseOf8, testCaseOfOtherwise, testCopyingBlock,
testDoubleRemoteAnidatedBlocks, testFullBlock, testIfNilClosure,
testIfTrue2, testIfTrueForEffectNested, testIfTrueIfFalse,
testIfTrueIfFalseNested2, testIfTrueWithOr,
testInlineBlockCollectionLR3, testNoRemoteBlockTemp,
testPrimitiveErrorCodeModule2, testPushBigArray, testSend,
testSimpleBlockArgument3, testSimpleBlockLocal, testSimpleBlockNested,
testToByDo, testToByDoNegativeLoop, testToDo4, testToDoArgument,
testToDoArgumentLimitIsExpression, testToDoInsideTemp,
testToDoInsideTempNotInlined, testWhileFalse,
testWhileModificationAfterNotInlined, testWhileModificationBeforeNotInlined,
testWhileTrue, testWhileTrue3, testWhileTrueSameJumpTarget2,
testWhileWithTempNotInlined
```

### Package/Class System (80+ errors)
Includes: PackageTest(20), PackageOnModelTest(16), PackageOrganizerTest(10),
PackageTagTest(7), PackageAnnouncementsTest(4), PackageAndClassesTest(3),
PackageAndMethodsTest(3), ClassFactoryForTestCaseTest(6),
ClassFactoryWithNonDefaultEnvironmentTest(6)
Root cause: class/package creation and modification at runtime.

### ClassAnnotation (25 errors)
ClassAnnotationTest(14) + RegisteredClassAnnotationsTest(11).
Root cause: annotation cache relies on class system manipulation.

### Class Definition Parsers (CD*) (30+ errors)
CDTraitCompositionClassParserTest(16), CDDoubleWordClassParserTest(6),
CDEphemeronClassParserTest(4), CDImmediateClassParserTest(4), etc.
Root cause: class definition parsing uses compilation infrastructure.

### OpalCompiler / OC* (35+ errors)
OCParserTest(16), OCASTClosureAnalyzerTest(6), OCClassBuilderTest(12),
OCCompilerTest(1), OCContextTempMappingTest(4), etc.
Root cause: compiler tests create/modify methods at runtime.

### FFI Tests (20 errors)
FFICalloutAPITest(17), FFICompilerPluginTest(3).
Root cause: FFI type resolution broken (ByteSymbol vs ExternalType).

### ClassTest (18 errors)
Root cause: class manipulation (addSlot, removeSlot, compile, subclass creation).

### BuilderManifestTest (17 errors)
Root cause: class/manifest creation at runtime.

### EpLogTest (13 errors)
Root cause: Epicea change log file I/O.

### Slot Tests (20+ errors)
SlotIntegrationTest(5), SlotTraitsTest(7), SlotClassVariableTest(4),
PropertySlotTest(4), SlotBasicTest(3), SlotMigrationTest(2).
Root cause: slot manipulation requires class reshaping.

### Ring/Reflectivity (10+ errors)
RGReadOnlyImageBackendTest(5), RGClassDefinitionTest(4),
RGMetaclassDefinitionTest(2), etc.

---

## Timeouts (16)

```
IntegerTest>>testPrintStringBase
IntegerTest>>testReciprocalModulo
IntegerTest>>testSlowFactorial
NumberParserTest>>testFloatPrintString
MonitorTest>>testExample2
TraitTest>>testClassUsesTrait
TraitTest>>testSettingEmptyTraitCompositionUpdatesMetaclass
RGReadOnlyImageBackendTest>>testTraitExclusions
ClassFactoryForTestCaseTest>>testTraitCreationInDifferentCategories
ContinuationTest>>testRemoveOneStar
SuperVariableTest>>testUsingMethods
CodeSimulationTest>>testTranscriptPrintingWithOpenedTranscriptExists
TestCaseTest>>testAnnouncement
GlobalIdentifierWithDefaultConfigurationTest>>testBackwardCompatibility
GlobalIdentifierWithDefaultConfigurationTest>>testBackwardCompatibility2
ObjectWithPrintingRaisingHaltTest>>testInspectingObjectWithPrintOnWithHaltOpenInspector
```

## Skips (19)

```
IntegerTest>>testCreationFromBytes1 (3 tests — platform-specific)
FloatTest>>testNaNCompare
OCParserTest>>testUnclosedTemporariesErrorNodeContainsRightValue
OCCodeReparatorTest>>testdefineClass
OCCodeReparatorTest>>testdefineTrait
Win32WideStringTest (5 tests — Windows-only)
RGMCClassTest>>testClassesWithTraits
FFIExternalStructurePlatformTest>>testStructureHasCorrectOffsets32bits
FFIExternalStructurePlatformTest>>testStructureHasCorrectSize32bits
ProcessMonitorTestServiceTest (3 tests)
CodeSimulationTest>>testErrorCodeNotFound
```

## Explicit Failures (52)

```
ObjectTest>>testBasicSizeNotOverwritten
LargePositiveIntegerTest>>testReciprocalModulo
IntegerDigitLogicTest>>testLargeShift
BehaviorTest>>testAllInstVarNames
BehaviorTest>>testAllMethods
ClassHierarchyTest>>testSubclassInstVar
DeprecationTest>>testTransformingDeprecation
SharedPoolTest>>testPoolUsers
ByteArrayTest>>testHexDumponmax
RandomTest>>testDistribution
NumberParserTest>>testFloatmin
SlotErrorsTest>>testCannotBeRecompiled
PluggableSetTest>>testGrow
CompiledMethodTest>>testMethodInDeprecatedPackageIsDeprecated (?)
InstructionStreamTest>>testSimulatingAMethodWithHaltHasCorrectContext
WriteBarrierTest>>testMutateByteArrayUsingDoubleAtPut
WriteBarrierTest>>testMutateByteArrayUsingFloatAtPut
ClassTest>>testComment
SlotTest>>testSlotUsers
RGMetaclassDefinitionTest>>testArrayStringForManifest
RGBehaviorTest>>testOldDefinition
RGReadOnlyImageBackendTest>>testBehaviorLocalMethods
OCClassBuilderTest>>testCreateEphemeronClassNamed
OCClassBuilderTest>>testCreateNormalClassWithSharedPools
OCClassBuilderTest>>testCreateVariableByteClassWithAll
OCProgramNodeTest>>testBestNodeForReturnAStatementWhenIntervalInStatementWithoutLeftPart
OCProgramNodeTest>>testReplaceMessageReceiver
OCParserTest>>testBinarySelectors
OCSpecialSelectorTest>>testUnoptimisedValueSpecialSendsMessageCapturesSend
OCASTSingleBranchConditionalTranslatorTest>>testNotNilIfNilDoesNotEvaluateBlock
Base64Test>>testWikipediaExampleQuote
BlockClosuresTestCase>>testNestedLoopsExample1
AITarjanTest>>testNestedCycle
ByteSymbolTest>>testAs
ByteSymbolTest>>testNewFrom
ByteSymbolTest>>testReadFromString
MethodClassifierTest>>testProtocolForKnownPrefixOfSelector
UndefinedClassTest>>testCreateSubclassOfArbitraryExpressionReturningNilThrowsError
OCCodeReparatorTest>>testDeclareTempAndPasteBlock
OCCodeReparatorTest>>testPossibleVariablesForBlock
OCCodeReparatorTest>>testSubstituteVariableAtIntervalBlock
EFMessageExpressionTest>>testKeywordOnTheSameLine2
EFMethodExpressionTest>>testIndentCascade
SystemNavigationTest>>testAllExistingProtocolsFor
SystemBuildInfoTest>>testSystemPackageNames
FFIAutoReleaseOptionCalloutTest>>testNotYetImplementedOnString
EquivalentTreeTest>>testMethodsToBeCheckedExceptSelector
FastStepThroughTest>>testStepIntoBlockArgumentInMessageSend
FastStepThroughTest>>testStepThrough
FastStepThroughTest>>testStepThroughInABlockInATemporary
FastStepThroughTest>>testStepThroughLonger
BitBltTest>>testAlphaCompositing
BitBltTest>>testAlphaCompositing2
```
