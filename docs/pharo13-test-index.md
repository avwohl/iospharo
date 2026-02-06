# Pharo 13 Test Suite Index

**Generated:** 2026-02-06
**Image:** Pharo 13.1 (build: 0, commit: ad5c9e4) 64-bit
**Total:** 246 packages, 2092 test classes, 29407 tests

This document provides a comprehensive index of all test classes in the Pharo 13 image, organized for systematic VM testing.

---

## Currently Running Tests

These 15 test classes are currently executed by our custom VM:

| Test Class | Tests | Pass | Fail | Error | Notes |
|-----------|-------|------|------|-------|-------|
| SmallIntegerTest | 29 | 29 | 0 | 0 | ✓ |
| FloatTest | 75 | 75 | 0 | 0 | ✓ |
| FractionTest | 32 | 32 | 0 | 0 | ✓ |
| IntegerTest | 83 | 83 | 0 | 0 | ✓ |
| ArrayTest | 324 | 324 | 0 | 0 | ✓ |
| OrderedCollectionTest | 351 | 351 | 0 | 0 | ✓ |
| DictionaryTest | 205 | 205 | 0 | 0 | ✓ |
| CharacterTest | 19 | 19 | 0 | 0 | ✓ |
| StringTest | 438 | 438 | 0 | 0 | ✓ |
| SymbolTest | 268 | 268 | 0 | 0 | ✓ |
| SetTest | 174 | 174 | 0 | 0 | ✓ |
| IntervalTest | 260 | 260 | 0 | 0 | ✓ |
| BagTest | 168 | 168 | 0 | 0 | ✓ |
| PointTest | 36 | 36 | 0 | 0 | ✓ |
| RectangleTest | 52 | 52 | 0 | 0 | ✓ |

**Current Status:** 2604 pass, 0 fail, 4 errors (99.85% pass rate)

**Known Errors (4):**
- `ArrayTest >> testIsHealthy` - uses reflectivity/metalinks, outside core scope
- `DictionaryTest >> testIsHealthy` - uses reflectivity/metalinks, outside core scope
- `OrderedCollectionTest >> testIsHealthy` - uses reflectivity/metalinks, outside core scope
- `SetTest >> testIsHealthy` - uses reflectivity/metalinks, outside core scope

---

## Priority Additions

The following test classes are recommended for addition, organized by priority tier. These build progressively on VM fundamentals while avoiding dependencies on missing features.

### Tier 1: Core VM Mechanics (179 tests)
Exercise bytecodes, contexts, closures, exceptions directly.

- **BlockClosureTest** (50) - block closures, temps, non-local returns
- **ContextTest** (34) - execution contexts, stack frames
- **ExceptionTest** (47) - exception handling, ensure, on:do:
- **BecomeTest** (8) - object mutation, become: primitive
- **BooleanTest** (5) - boolean protocol
- **TrueTest** (17) - true singleton
- **FalseTest** (17) - false singleton
- **ProtoObjectTest** (17) - absolute minimal object protocol
- **ObjectTest** (28) - core object protocol (=, ==, hash, copy)
- **UndefinedObjectTest** (19) - nil singleton
- **SemaphoreTest** (18) - semaphore synchronization

### Tier 2: Numbers and Arithmetic (113 tests)
Exercise primitive arithmetic operations.

- **LargePositiveIntegerTest** (19) - big integer math
- **LargeNegativeIntegerTest** (15) - negative big integers
- **IntegerDigitLogicTest** (7) - bitwise operations
- **NumberTest** (23) - abstract number protocol
- **MagnitudeTest** (7) - comparison protocol
- **ScaledDecimalTest** (36) - fixed-point decimals

### Tier 3: Classes and Methods (307 tests)
Exercise method lookup, class hierarchy, compilation.

- **BehaviorTest** (45) - class behavior protocol
- **CompiledCodeTest** (32) - compiled method structure
- **CompiledMethodTest** (75) - method compilation, literals, execution
- **ClassDescriptionTest** (29) - class descriptions
- **ClassTest** (46) - class protocol, hierarchy
- **ProcessTest** (46) - process scheduling, priority
- **ProcessTerminateBugTest** (12) - process termination edge cases

### Tier 4: More Collections (1531 tests)
Broader collection coverage beyond current 15 classes.

- **LinkedListTest** (255) - doubly-linked list
- **SortedCollectionTest** (287) - sorted collections
- **IdentityDictionaryTest** (206) - identity-based dictionaries
- **IdentitySetTest** (176) - identity-based sets
- **WeakSetTest** (50) - weak reference sets
- **ByteArrayTest** (12) - byte arrays
- **Float32ArrayTest** (277) - 32-bit float arrays
- **Float64ArrayTest** (268) - 64-bit float arrays

### Tier 5: FFI (303 tests)
Validates our ThreadedFFI implementation.

**ThreadedFFI-Tests (101 tests):**
- TFBasicTypeTest (15) - basic type marshalling
- TFBasicTypeSizeTest (26) - type size validation
- TFBasicTypeMarshallingTest (22) - marshalling correctness
- TFFunctionCallTest (8) - function call mechanics
- TFStructTest (3) - structure handling

**ThreadedFFI-UFFI-Tests (110 tests):**
- TFUFFIBasicTypeMarshallingInCallbacksTest (18)
- TFUFFIBasicTypeMarshallingTest (18)
- TFUFFIBasicTypeSizeTest (26)
- TFUFFICallbackTest (7)
- TFUFFIConcurrencyTest (1)
- TFUFFIDerivedTypeMarshallingInCallbackTest (6)
- TFUFFIDerivedTypeMarshallingTest (17)
- TFUFFIDerivedTypeSizeTest (1)
- TFUFFIDifferentCallingConventionFunctionCallTest (1)
- TFUFFIFunctionCallTest (3)
- TFUFFIMethodRegistryTest (1)
- TFUFFIStructuresTest (11)

**UnifiedFFI-Tests (92 tests):**
- FFIArchitectureTest (3)
- FFIAutoReleaseOptionCalloutTest (4)
- FFICallbackParametersTest (12)
- FFICallbackTest (2)
- FFICalloutAPITest (18)
- FFICalloutMethodBuilderTest (10)
- FFICalloutTest (6)
- FFICompilerPluginTest (5)
- FFIConstantHandleTest (2)
- FFIExternalArrayTest (7)
- FFIExternalEnumerationTest (6)
- FFIExternalPackedStructureTest (2)
- FFIExternalStructureFieldParserTest (1)
- FFIExternalStructurePlatformTest (4)
- FFIExternalStructureTest (12)
- FFIExternalUnionTest (1)
- FFIExternalValueHolderTest (2)
- FFIFunctionParserTest (45)
- FFIFunctionResolutionTest (19)
- FFIOpaqueObjectTest (3)
- FFIStringCalloutTest (13)
- FFITypeArrayTest (5)
- FFITypesTest (15)
- LibTTYTest (5) - excluded, requires TTY

---

## Full Package Index

All 246 packages with their test classes, organized by category.

### Core Kernel & Collections (13 packages, 170 classes, 5919 tests)

**Kernel-Tests** (57 classes, 981 tests)
- AllocationTest (4), BasicBehaviorClassMetaclassTest (9), BecomeTest (8), BehaviorTest (45)
- BlockClosureTest (50), BlockClosuresTestCase (13), BooleanTest (5), CharacterTest (19)
- ClassAnnouncementsTest (1), ClassDescriptionProtocolsTest (41), ClassDescriptionTest (29), ClassHierarchyTest (3)
- CompiledBlockTest (2), CompiledCodeTest (32), ContextTest (34)
- DelayBasicSchedulerMicrosecondTickerTest (16), DelayBasicSchedulerMillisecondTickerTest (16)
- DelayMutexSchedulerMicrosecondTickerTest (16), DelayMutexSchedulerMillisecondTickerTest (16)
- DelaySemaphoreSchedulerMicrosecondTickerTest (16), DelaySemaphoreSchedulerMillisecondTickerTest (16)
- DelayTest (5), DependentsArrayTest (1), DeprecationTest (2), ExceptionTest (47)
- FalseTest (17), FloatTest (75), FractionTest (32), IVsAndClassVarNamesConflictTest (2)
- IntegerDigitLogicTest (7), IntegerTest (83), LargeNegativeIntegerTest (15), LargePositiveIntegerTest (19)
- LocalRecursionStopperTest (4), MagnitudeTest (7), MessageNotUnderstoodTest (2), MetaClassTest (3)
- MethodAnnouncementsTest (10), MonitorTest (3), NumberTest (23), ObjectFinalizerTest (1)
- ObjectLayoutTest (1), ObjectTest (28), OutOfMemoryTest (2), PragmaTest (10)
- ProcessSpecificTest (8), ProcessTerminateBugTest (12), ProtoObjectTest (17), ProtocolAnnouncementsTest (14)
- RecursionStopperTest (4), ScaledDecimalTest (36), SemaphoreTest (18), SharedPoolTest (6)
- SmallIntegerTest (29), TrueTest (17), UndefinedObjectTest (19), WeakMessageSendTest (11)

**Kernel-Extended-Tests** (12 classes, 305 tests)
- AdditionalMethodStateTest (3), AsciiCharsetTest (32), ClassTest (46), CodeSimulationTest (9)
- CompiledMethodTest (75), InstructionClientTest (1), InstructionStreamTest (15), MutexTest (7)
- ProcessTest (46), SizeInMemoryTest (5), UnicodeTest (35), WriteBarrierTest (31)

**Kernel-CodeModel-Tests** (14 classes, 198 tests)
- PackageAndClassesTest (29), PackageAndMethodsTest (26), PackageAndTraitOnModelTest (6), PackageAndTraitsTest (5)
- PackageAnnouncementsTest (11), PackageObsoleteTest (2), PackageOnModelTest (19), PackageOrganizerTest (30)
- PackageRenameTest (6), PackageTagTest (10), PackageTest (41), UndeclaredVariableTest (4)
- UndefinedPackageTagTest (4), UndefinedPackageTest (5)

**Kernel-ExtraUtils** (1 class, 10 tests)
- ClassHierarchyPrinterTest (10)

**Kernel-Tests-WithCompiler** (3 classes, 10 tests)
- BehaviorWithCompilerTest (5), RuntimeSyntaxErrorTest (3), SelfEvaluatingObjectTest (2)

**Collections-Abstract-Tests** (2 classes, 82 tests)
- CollectionRootTest (56), SplitJoinTest (26)

**Collections-Sequenceable-Tests** (9 classes, 1762 tests)
- ArrayTest (324), HeapTest (148), IntervalTest (260), LinkedListTest (255)
- OrderedCollectionTest (351), OrderedDictionaryTest (67), OrderedIdentityDictionaryTest (67)
- SharedQueueTest (3), SortedCollectionTest (287)

**Collections-Unordered-Tests** (14 classes, 1781 tests)
- BagTest (168), DictionaryTest (205), HashTableSizesTest (1), IdentityBagTest (3)
- IdentityDictionaryTest (206), IdentitySetTest (176), KeyedTreeTest (9), MethodDictionaryTest (36)
- NestedDictionaryTest (6), PluggableDictionaryTest (209), PluggableSetTest (174), SetTest (174)
- SmallDictionaryTest (207), SmallIdentityDictionaryTest (207)

**Collections-Weak-Tests** (7 classes, 954 tests)
- WeakIdentityKeyDictionaryTest (209), WeakIdentitySetTest (51), WeakIdentityValueDictionaryTest (218)
- WeakKeyDictionaryTest (207), WeakOrderedCollectionTest (2), WeakSetTest (50), WeakValueDictionaryTest (217)

**Collections-Strings-Tests** (6 classes, 738 tests)
- ByteSymbolTest (4), StringInitializationTest (6), StringLineEndingsTest (3)
- StringTest (438), SymbolTest (268), WideStringTest (19)

**Collections-Support-Tests** (6 classes, 62 tests)
- AssociationTest (13), CharacterSetTest (1), CollectionTest (7), RunArrayTest (35)
- ValueLinkTest (3), WideCharacterSetTest (3)

**Collections-Streams-Tests** (7 classes, 95 tests)
- GeneratorTest (13), LimitedWriteStreamTest (23), NullStreamTest (8), ReadStreamTest (12)
- ReadWriteStreamTest (19), StreamBugsTest (1), WriteStreamTest (19)

**Collections-Native-Tests** (5 classes, 566 tests)
- ByteArrayTest (12), Float32ArrayTest (277), Float64ArrayTest (268), IntegerArrayTest (3), NativeArrayTest (6)

**Collections-Arithmetic-Tests** (1 class, 20 tests)
- CollectionArithmeticTest (20)

**Collections-Atomic-Tests** (3 classes, 17 tests)
- FIFOQueueTest (9), LIFOQueueTest (3), WaitfreeQueueTest (5)

**Collections-DoubleLinkedList-Tests** (1 class, 22 tests)
- DoubleLinkedListTest (22)

**Collections-Stack-Tests** (1 class, 13 tests)
- StackTest (13)

**Collections-Tests** (1 class, 8 tests)
- ReduceTest (8)

### Compiler & AST (5 packages, 133 classes, 2400 tests)

**OpalCompiler-Tests** (55 classes, 668 tests)
- MethodMapTest (30), MethodPragmaTest (37), OCASTAndOrTranslatorTest (8), OCASTBasicTranslatorTest (3)
- OCASTBlockTranslatorTest (17), OCASTCheckerTest (25), OCASTClosureAnalyzerTest (33)
- OCASTDoubleBlockTranslatorTest (4), OCASTDoubleBranchConditionalTranslatorTest (12)
- OCASTLiteralTranslatorTest (3), OCASTRepeatTranslatorTest (3), OCASTSemanticAnalyzerTest (1)
- OCASTSingleBlockTranslatorTest (4), OCASTSingleBranchConditionalTranslatorTest (28)
- OCASTSpecialLiteralTranslatorTest (5), OCASTTimesRepeatTranslatorTest (7), OCASTToDoTranslatorTest (5)
- OCASTTranslatorMappingForFullBlockClosuresTest (2), OCASTTranslatorTest (4)
- OCASTVariableTranslatorTest (16), OCASTWhileFalseTranslatorTest (9), OCASTWhileTrueTranslatorTest (9)
- OCAnnotationTest (5), OCArrayLiteralTest (8), OCBytecodeGeneratorTest (2)
- OCBytecodeToASTCacheTest (14), OCClosureCompilerTest (8), OCClosureTest (24)
- OCCompilationContextTest (6), OCCompileWithFailureTest (3), OCCompiledMethodIntegrityTest (8)
- OCCompilerNotifyingTest (31), OCCompilerSyntaxErrorNotifyingTest (31), OCCompilerTest (17)
- OCContextTempMappingTest (11), OCDoItVariableTest (10), OCDoitTest (13)
- OCDynamicASTCompilerPluginTest (2), OCEnvironmentScopeTest (2), OCIRBuilderTest (34)
- OCIRPrinterTest (18), OCIRTransformTest (10), OCIRVisitorTest (18)
- OCIfNotNilTest (13), OCLiteralTest (14), OCNewCompilerWithChangesFunctionalTest (1)
- OCPerformTest (2), OCScanner2Test (2), OCScopeTest (4)
- OCSourceCode2BytecodeTest (58), OCSpecialSelectorTest (4), OCStaticASTCompilerPluginTest (4)
- OCTargetCompilerTest (4), OCVariableNodeNameResolutionTest (3), OpalCompilerTest (19)

**OpalCompiler-ToolFeatures-Tests** (2 classes, 12 tests)
- OCReadBeforeWrittenTesterTest (2), OCReturnNodeAdderVisitorTest (10)

**OpalCompiler-UI-Tests** (1 class, 15 tests)
- OCCodeReparatorTest (15)

**AST-Core-Tests** (23 classes, 624 tests)
- OCBlockNodeTest (5), OCCodeSnippetScriptingTest (4), OCCodeSnippetTest (19), OCCommentNodeTest (2)
- OCCommentNodeVisitorTest (3), OCDumpVisitorTest (20), OCEOFTokenTest (4), OCErrorNodeParserTest (29)
- OCEvaluationTest (3), OCGenericNodeVisitorTest (5), OCMessageNodeTest (16), OCMethodNodeTest (31)
- OCParseErrorNodeTest (1), OCParseTreeRewriterTest (12), OCParseTreeSearcherTest (46)
- OCParserTest (142), OCPatternParserTest (8), OCProgramNodeTest (121), OCScannerTest (138)
- OCSequenceNodeTest (2), OCSimpleFormatterTest (6), OCTypingVisitorTest (5), OCVariableNodeTest (2)

**Flashback-Decompiler-Tests** (3 classes, 244 tests)
- FBDBytecodeDecompilerExamplesTest (51), FBDDecompilerTest (160), FBIRBytecodeDecompilerTest (33)

**ClassParser-Tests** (30 classes, 448 tests)
- CDBehaviorParserTest (7), CDClassDefinitionParserTest (15), CDClassWithPoolDictionaryParserTest (15)
- CDClassWithTraitAliasParserTest (19), CDClassWithTraitCompositionSequenceParserTest (17)
- CDClassWithTraitExclusionParserTest (17), CDClassWithTraitParserTest (17)
- CDCompiledBlockClassParserTest (16), CDCompiledMethodClassParserTest (16)
- CDDoubleByteClassParserTest (16), CDDoubleWordClassParserTest (16), CDEphemeronClassParserTest (16)
- CDExistingClassDefinitionTest (13), CDExistingClassSideDefinitionTest (1), CDFluidClassParserTest (44)
- CDImmediateClassParserTest (16), CDMetaclassParserTest (8), CDMetaclassWithTraitParserTest (9)
- CDNilSubclassParserTest (1), CDNormalClassCategoryParserTest (15), CDNormalClassParserTest (16)
- CDNormalMetaclassParserTest (9), CDPointClassParserTest (16), CDTraitCompositionClassParserTest (16)
- CDTraitParserTest (7), CDVariableByteClassParserTest (16), CDVariableClassParserTest (16)
- CDVariableWordClassParserTest (16), CDWeakClassParserTest (16), OCClassBuilderTest (26)

**ClassDefinitionPrinters-Tests** (4 classes, 47 tests)
- ClassDefinitionPrinterConfigurationTest (3), FluidClassDefinitionPrinterTest (24)
- LegacyClassDefinitionPrinterTest (10), OldClassDefinitionPrinterTest (10)

### System & Files (16 packages, 121 classes, 1511 tests)

**System-Support-Tests** (7 classes, 261 tests)
- ClassQueryTest (3), MethodQueryTest (1), SmalltalkImageTest (9), SystemBuildInfoTest (3)
- SystemEnvironmentTest (217), SystemNavigationTest (11), SystemVersionTest (17)

**System-Time-Tests** (21 classes, 659 tests)
- BlockClosureValueWithinDurationTest (5), BlockClosureValueWithinTest (5)
- DateAndTimeDosEpochTest (63), DateAndTimeEpochTest (64), DateAndTimeLeapTest (43), DateAndTimeTest (59)
- DateAndTimeUnixEpochTest (63), DateParsingTest (20), DateTest (54), DosTimestampTest (3)
- DurationTest (71), MonthTest (18), ScheduleTest (15), StopwatchTest (13), TimeTest (55)
- TimespanDoSpanAYearTest (4), TimespanDoTest (8), TimespanTest (62), WeekTest (14)
- YearMonthWeekTest (8), YearTest (12)

**FileSystem-Core-Tests** (18 classes, 337 tests)
- BreadthFirstGuideTest (1), CollectVisitorTest (3), CopyVisitorTest (1), DeleteVisitorTest (2)
- DirectoryEntryTest (12), FileLocatorTest (38), FileReferenceTest (112), FileSystemHandleTest (15)
- FileSystemTest (53), InteractiveResolverTest (2), PathTest (76), PlatformResolverTest (3)
- PostorderGuideTest (1), PreorderGuideTest (1), SelectVisitorTest (6), SystemResolverTest (7)
- UnixResolverTest (3), WindowsResolverTest (1)

**FileSystem-Disk-Tests** (3 classes, 81 tests)
- DiskFileSystemTest (59), FileHandleTest (15), WindowsStoreTest (7)

**FileSystem-Memory-Tests** (2 classes, 82 tests)
- MemoryFileSystemTest (67), MemoryHandleTest (15)

**FileSystem-Tests-Attributes** (3 classes, 49 tests)
- DiskFileAttributesTest (24), FileAttributesPluginPrimsTest (6), FileReferenceAttributeTest (19)

**Files-Tests** (4 classes, 33 tests)
- BinaryFileStreamTest (15), FileRegistryTest (2), FileTest (9), StdioStreamTest (7)

**System-BasicCommandLineHandler-Tests** (4 classes, 30 tests)
- CommandLineArgumentsTest (8), CommandLineHandlerTest (5), CommandLinePasswordManagerTest (8)
- STCommandLineHandlerTest (9)

**System-Benchmark-Tests** (1 class, 4 tests)
- BenchmarkResultTest (4)

**System-Caching-Tests** (2 classes, 54 tests)
- LRUCacheTest (26), TTLCacheTest (28)

**System-Dependencies-Tests** (1 class, 17 tests)
- SystemDependenciesTest (17)

**System-Finalization-Tests** (1 class, 6 tests)
- FinalizationRegistryTest (6)

**System-Hashing-Tests** (4 classes, 51 tests)
- BitRegisterTest (21), MD5Test (9), SHA1Test (9), SHA256Test (12)

**System-History-Tests** (3 classes, 22 tests)
- ConfigurableHistoryIteratorTest (8), HistoryIteratorTest (6), HistoryNodeTest (8)

**System-Identification-Tests** (8 classes, 74 tests)
- GlobalIdentifierFuelPersistenceTest (10), GlobalIdentifierMergerTest (8), GlobalIdentifierPersistenceTest (10)
- GlobalIdentifierStonPersistenceTest (10), GlobalIdentifierTest (9), GlobalIdentifierWithDefaultConfigurationTest (9)
- GlobalIdentifierWithFuelTest (9), GlobalIdentifierWithStonTest (9)

**System-Installers-Tests** (1 class, 2 tests)
- MCMczInstallerTest (2)

**System-Localization-Tests** (5 classes, 14 tests)
- ISOLanguageDefinitionTest (2), LocaleChangedTest (1), LocaleIDTest (8), LocaleTest (1)
- NaturalLanguageTranslatorTest (2)

**System-OSEnvironments-Tests** (1 class, 9 tests)
- OSEnvironmentTest (9)

**System-Object Events-Tests** (1 class, 25 tests)
- EventManagerTest (25)

**System-Platforms-Tests** (5 classes, 17 tests)
- KeyboardKeyTest (3), OSPlatformTest (5), Win32EnvironmentTest (3), Win32WideStringTest (5), WinPlatformTest (1)

**System-SessionManager-Tests** (5 classes, 23 tests)
- SessionCreationTest (3), SessionErrorHandlingTest (4), SessionManagerRegistrationOrderTest (8)
- SessionManagerRegistrationTest (6), SessionManagerUnregistrationTest (2)

**System-Settings-Tests** (6 classes, 46 tests)
- AbstractStoredSettingTest (4), SettingBrowserTest (2), SettingsStonReaderTest (7)
- SettingsStonWriterTest (5), StoredSettingsMergerTest (2), SystemSettingsPersistenceTest (26)

**System-Sources-Tests** (2 classes, 32 tests)
- SourceFileArrayTest (18), SourceFileBufferedReadWriteStreamTest (14)

**System-Utilities-Tests** (2 classes, 18 tests)
- ClipboardTest (4), ContinuationTest (14)

### FFI & External Interface (3 packages, 44 classes, 413 tests)

**ThreadedFFI-Tests** (8 classes, 101 tests)
- TFBasicTypeMarshallingInCallbacksTest (18), TFBasicTypeMarshallingTest (22)
- TFBasicTypeSizeTest (26), TFBasicTypeTest (15), TFCallbacksTest (5)
- TFFunctionCallTest (8), TFPoolTest (4), TFStructTest (3)

**ThreadedFFI-UFFI-Tests** (12 classes, 110 tests)
- TFUFFIBasicTypeMarshallingInCallbacksTest (18), TFUFFIBasicTypeMarshallingTest (18)
- TFUFFIBasicTypeSizeTest (26), TFUFFICallbackTest (7), TFUFFIConcurrencyTest (1)
- TFUFFIDerivedTypeMarshallingInCallbackTest (6), TFUFFIDerivedTypeMarshallingTest (17)
- TFUFFIDerivedTypeSizeTest (1), TFUFFIDifferentCallingConventionFunctionCallTest (1)
- TFUFFIFunctionCallTest (3), TFUFFIMethodRegistryTest (1), TFUFFIStructuresTest (11)

**UnifiedFFI-Tests** (24 classes, 202 tests)
- FFIArchitectureTest (3), FFIAutoReleaseOptionCalloutTest (4), FFICallbackParametersTest (12)
- FFICallbackTest (2), FFICalloutAPITest (18), FFICalloutMethodBuilderTest (10)
- FFICalloutTest (6), FFICompilerPluginTest (5), FFIConstantHandleTest (2)
- FFIExternalArrayTest (7), FFIExternalEnumerationTest (6), FFIExternalPackedStructureTest (2)
- FFIExternalStructureFieldParserTest (1), FFIExternalStructurePlatformTest (4)
- FFIExternalStructureTest (12), FFIExternalUnionTest (1), FFIExternalValueHolderTest (2)
- FFIFunctionParserTest (45), FFIFunctionResolutionTest (19), FFIOpaqueObjectTest (3)
- FFIStringCalloutTest (13), FFITypeArrayTest (5), FFITypesTest (15), LibTTYTest (5)

### Graphics & UI Morphic (12 packages, 74 classes, 621 tests)

**Graphics-Tests** (12 classes, 213 tests)
- BMPReadWriterTest (5), BitBltClipBugsTest (9), BitBltTest (9), ColorTest (16)
- FormSetTest (13), FormTest (8), GIFReadWriterTest (3), ImageReadWriterTest (12)
- MarginTest (8), PNGReadWriterTest (42), PointTest (36), RectangleTest (52)

**Morphic-Tests** (20 classes, 115 tests)
- CircleMorphTest (9), HaloMorphTest (3), LayoutFrameTest (9), MCPTest (1)
- MorphTest (9), MorphTreeMorphTest (5), MorphicEventHandlerTest (14), MorphicNativeWindowTest (5)
- MorphicWindowManagerTest (13), MouseClickStateTest (4), NullWorldRendererTest (1)
- PolygonMorphTest (1), ScrollbarTest (9), SliderTest (9), SupplyAnswerTest (3)
- TableLayoutTest (1), TextAnchorTest (3), TextLineTest (2), TextMorphTest (6), WindowAnnouncementTest (8)

**Morphic-Deprecated** (1 class, 1 test)
- PaginatedMorphTreeMorphTest (1)

**Morphic-Widgets-FastTable-Tests** (13 classes, 67 tests)
- FTAbstractColumnSortingStrategyTest (1), FTAbstractSortingStateTest (2), FTAscendingSortingStateTest (2)
- FTCellSelectionModeStrategyTest (15), FTColumnTest (4), FTDescendingSortingStateTest (2)
- FTNullColumnSortingStrategyTest (1), FTPropertyColumnSortingStrategyTest (3)
- FTRowSelectionModeStrategyTest (15), FTSelectionModeStrategyTest (15)
- FTSortFunctionColumnSortingStrategyTest (4), FTTableMorphTest (1), FTUnsortedSortingStateTest (2)

**Morphic-Widgets-Taskbar-Tests** (2 classes, 18 tests)
- TaskListMorphTest (3), TaskbarMorphTest (15)

**Athens-Core-Tests** (3 classes, 45 tests)
- AthensAffineTransformTest (15), AthensProjectiveTransformTest (15), AthensTransformTest (15)

**Athens-Cairo-Tests** (7 classes, 34 tests)
- AthensCairoCanvasTest (1), AthensCairoExportSurfaceTest (4), AthensCairoMatrixTest (17)
- AthensCairoPDFSurfaceTest (4), AthensCairoSVGSurfaceTest (4), CairoLibraryTest (1)
- CairoUTF8ConverterTest (3)

**FormCanvas-Tests** (3 classes, 16 tests)
- BalloonEngineTest (1), PointArrayTest (9), ShortIntegerArrayTest (6)

**FreeType-Tests** (3 classes, 28 tests)
- FreeTypeCacheTest (25), FreeTypeFontFamilyMemberTest (2), FreeTypeFontTest (1)

**Fonts-Infrastructure-Tests** (4 classes, 4 tests)
- AbstractFontFamilyMemberTest (1), AbstractFontFamilyTest (1), AbstractFontProviderTest (1)
- LogicalFontManagerTest (1)

**EmbeddedFreeType-Tests** (1 class, 2 tests)
- EmbeddedFreeTypeFontInstallerTest (2)

**OSWindow-Tests** (2 classes, 5 tests)
- OSWindowAttributesTest (1), OSWindowTest (4)

**Geometry-Tests** (17 classes, 228 tests)
- GAngleTest (23), GArcTest (16), GCircleTest (11), GCoordinatesTest (11)
- GElementTestCase (11), GEllipseTest (23), GLineTest (15), GMatrixTest (5)
- GPointTest (20), GPolygonTest (17), GRayTest (13), GRectangleTest (14)
- GSegmentTest (18), GShapeTestCase (14), GTestCase (2), GTriangleTest (7), GVectorTest (8)

### Spec2 UI Framework (14 packages, 240 classes, 3127 tests)

**Spec2-Tests** (85 classes, 1305 tests)
- SpAbstractButtonPresenterTest (12), SpAbstractListPresenterTest (26), SpAbstractSelectionModeTest (1)
- SpAbstractTextPresenterTest (27), SpAbstractTreePresenterTest (21), SpAbstractWidgetLayoutTest (4)
- SpAbstractWidgetPresenterDeferringActionTest (2), SpActionModifierTest (1), SpApplicationTest (8)
- SpApplicationWithToolbarTest (11), SpBasePresenterTest (9), SpBoxLayoutTest (15)
- SpButtonPresenterTest (20), SpCheckBoxExampleTest (11), SpCheckboxPresenterTest (22)
- SpClassMethodBrowserTest (11), SpCodeCommandTest (1), SpComponentListPresenterTest (32)
- SpComposablePresenterWithAdditionalSubpresentersTest (1), SpComposablePresenterWithModelTest (10)
- SpDemoTest (12), SpDropListPresenterTest (20), SpDynamicWidgetChangeTest (11)
- SpEditableListPresenterTest (17), SpEventHandlerTest (3), SpFrameLayoutTest (11)
- SpGridLayoutBuilderTest (3), SpGridLayoutTest (1), SpHorizontalBoxLayoutTest (21)
- SpHorizontalPanedLayoutTest (24), SpImagePresenterTest (14), SpLabelPresenterTest (14)
- SpLayoutTest (10), SpLinkPresenterTest (14), SpListPresenterMultipleSelectionTest (63)
- SpListPresenterSingleSelectionTest (45), SpListPresenterTest (31), SpListSelectionPresenterTest (11)
- SpMenuBarPresenterTest (13), SpMenuButtonPresenterTest (13), SpMenuItemPresenterTest (13)
- SpMenuPresenterTest (13), SpMillerColumnPresenterTest (21), SpMultipleSelectionModeTest (4)
- SpNotebookPresenterTest (21), SpNotificationCenterTest (4), SpNotificationTest (1)
- SpNumberInputFieldPresenterTest (47), SpOpenOnIntExampleTest (11), SpOpenOnNilExampleTest (11)
- SpOpenOnStringExampleTest (11), SpOptionListPresenterTest (11), SpOptionPresenterTest (2)
- SpOverlayLayoutTest (15), SpPanedLayoutTest (24), SpPopoverPresenterTest (14)
- SpPresenterTest (13), SpRadioButtonExampleTest (11), SpRadioButtonPresenterTest (11)
- SpRequiredFieldValidationTest (4), SpSearchInputFieldOptionsPresenterTest (15)
- SpSingleSelectionModeTest (6), SpSliderInputPresenterTest (11), SpSliderPresenterTest (15)
- SpSmokeTest (11), SpSpecTest (9), SpStringTableColumnTest (3), SpTablePresenterTest (35)
- SpTextFieldExampleTest (11), SpTextInputFieldPresenterTest (40)
- SpTextInputFieldWithValidationPresenterTest (3), SpTextPresenterTest (35)
- SpToggleSplitPresenterTest (2), SpToolbarPresenterTest (15), SpToolbarToggleButtonPresenterTest (9)
- SpTransmissionWithComponentListTest (12), SpTreePresenterTest (21)
- SpTreeTablePresenterMultipleSelectionTest (64), SpTreeTablePresenterSingleSelectionTest (41)
- SpTreeTablePresenterTest (22), SpValidationReportTest (1), SpVerticalBoxLayoutTest (19)
- SpVerticalPanedLayoutTest (24), SpWindowPresenterTest (18), SpWindowTest (5)

**Spec2-Backend-Tests** (56 classes, 790 tests)
- SpAbstractAdapterTest (4), SpAbstractListAdapterMultipleSelectionTest (19)
- SpAbstractListAdapterSingleSelectionTest (21), SpAbstractListCommonPropertiestTest (13)
- SpAbstractSearchTest (8), SpAbstractTextAdapterTest (12), SpAbstractTreeTableAdapterTest (19)
- SpAbstractWidgetAdapterTest (6), SpAthensAdapterTest (9), SpBoxLayoutAdapterTest (12)
- SpButtonAdapterTest (13), SpCheckboxAdapterTest (14), SpComponentListAdapterMultipleSelectionTest (19)
- SpComponentListAdapterSingleSelectionTest (21), SpComponentListAdapterTest (8)
- SpDropListAdapterTest (15), SpDropListWithoutInitialSelectionAdapterTest (2)
- SpExecutableLayoutTest (2), SpGridLayoutAdapterTest (19), SpHorizontalBoxLayoutAdapterTest (12)
- SpHorizontalPanedLayoutAdapterTest (18), SpLabelAdapterTest (8), SpLayoutAdapterTest (5)
- SpListAdapterMultipleSelectionTest (19), SpListAdapterSingleSelectionTest (21)
- SpListCommonPropertiestTest (23), SpListPresenterHeaderTest (1), SpListSearchTest (8)
- SpMillerColumnAdapterTest (10), SpNotebookAdapterTest (18), SpOverlayLayoutAdapterTest (14)
- SpPanedLayoutAdapterTest (18), SpRadioButtonAdapterTest (8), SpRadioButtonInteractionTest (11)
- SpScrollableLayoutAdapterTest (11), SpSliderAdapterTest (13), SpTableAdapterMultipleSelectionTest (19)
- SpTableAdapterSingleSelectionTest (21), SpTableCommonPropertiestTest (17), SpTableSearchTest (8)
- SpTextAdapterTest (20), SpTextInputFieldAdapterTest (17), SpToggleButtonAdapterTest (14)
- SpToolbarAdapterTest (7), SpTreeAdapterMultipleSelectionTest (21), SpTreeAdapterSingleSelectionTest (22)
- SpTreePresenterExpandTest (11), SpTreeTableAdapterMultiColumnMultiSelectionTest (20)
- SpTreeTableAdapterMultiColumnTest (20), SpTreeTableAdapterMultipleSelectionTest (19)
- SpTreeTableAdapterSingleColumnMultiSelectionTest (20), SpTreeTableAdapterSingleColumnTest (19)
- SpTreeTableAdapterSingleSelectionTest (23), SpTreeTableSearchTest (8)
- SpVerticalBoxLayoutAdapterTest (12), SpVerticalPanedLayoutAdapterTest (18)

**Spec2-Code-Tests** (9 classes, 102 tests)
- SpCodeBehaviorInteractionModelTest (4), SpCodeInteractionModelTest (4)
- SpCodeMethodInteractionModelTest (4), SpCodeNullInteractionModelTest (4)
- SpCodeObjectInteractionModelTest (5), SpCodePopoverPrintPresenterTest (2)
- SpCodePresenterTest (67), SpCodeScriptingInteractionModelTest (7), SpContextInteractionModelTest (5)

**Spec2-Code-Backend-Tests** (1 class, 23 tests)
- SpCodeAdapterTest (23)

**Spec2-Code-Diff-Tests** (2 classes, 6 tests)
- DiffBuilderTest (4), DiffPatchTest (2)

**Spec2-Morphic-Tests** (11 classes, 107 tests)
- SpDatePresenterTest (12), SpDropListExampleTest (11), SpImageAdapterTest (10)
- SpJobListPresenterTest (14), SpMorphPresenterTest (7), SpPaginatorMorphTest (3)
- SpRGBSlidersPresenterTest (13), SpRGBWidgetTest (11), SpScrollSyncExampleTest (11)
- SpToolbarToggleButtonMorphTest (12), SpWorldPresenterTest (3)

**Spec2-Morphic-Backend-Tests** (10 classes, 52 tests)
- SpApplicationWithLocaleTest (1), SpMorphicBoxLayoutTest (9), SpMorphicFrameLayoutTest (3)
- SpMorphicGridLayoutTest (11), SpMorphicNumberInputFieldAdapterTest (13)
- SpMorphicPanedLayoutTest (2), SpMorphicWindowAdapterTest (3), SpPresenterBuildTest (3)
- SpPresenterFocusOrderTest (4), SpUIThemeDecoratorTest (3)

**Spec2-Adapters-Morphic-Tests** (11 classes, 28 tests)
- SpDrawStyleTest (1), SpFontStyleTest (1), SpMergeStyleTest (5), SpMorphStyleTest (7)
- SpMorphicListAdapterTest (2), SpPropertyStyleTest (4), SpStyleEnvironmentColorProxyTest (1)
- SpStyleTest (1), SpStyleVariableTest (4), SpTextInputFieldPresenterStyleTest (1)
- SpWindowSimulateOpenModalTest (1)

**Spec2-Commander2-Tests** (6 classes, 29 tests)
- CmUILeftPositionStrategyExtensionsTest (2), CmUIRightPositionStrategyExtensionsTest (2)
- SpCommandTest (16), SpMenuBarPresenterBuilderTest (4), SpMenuPresenterBuilderTest (4)
- SpRecursiveContextSetterTest (1)

**Spec2-Dialogs-Tests** (1 class, 8 tests)
- SpInformUserDialogTest (8)

**Spec2-ListView-Tests** (6 classes, 151 tests)
- SpColumnViewPresenterTest (26), SpEasyColumnViewPresenterTest (26), SpEasyListViewPresenterTest (31)
- SpEasyTreeColumnViewPresenterTest (21), SpEasyTreeListViewPresenterTest (21), SpListViewPresenterTest (26)

**ColorPicker-Tests** (1 class, 2 tests)
- SpColorPickerTest (2)

**NewValueHolder-Tests** (2 classes, 10 tests)
- CollectionValueHolderTest (8), NewValueHolderTest (2)

**NewTools-FontChooser** (1 class, 10 tests)
- StFontChooserPresenterTest (10)

**NewTools-FontChooser-Tests** (1 class, 1 test)
- FontChooserTest (1)

### NewTools & IDE (27 packages, 211 classes, 2257 tests)

**NewTools-Debugger-Tests** (23 classes, 302 tests)
- HaltInCompiledBlockPrintOnTest (1), ObjectWithPrintingRaisingHaltTest (1)
- StDebuggerActionModelAnnouncementTest (1), StDebuggerActionModelTest (55)
- StDebuggerAssertionContextTest (2), StDebuggerCodeCommandTreeBuilderTest (7)
- StDebuggerCommandTest (25), StDebuggerCommandTreeBuilderTest (5)
- StDebuggerConfigurationCommandTreeBuilderTest (5), StDebuggerContextInteractionModelTest (11)
- StDebuggerContextPredicateTest (6), StDebuggerContextTest (14), StDebuggerExceptionExtensionsTest (4)
- StDebuggerExtensionMechanismTest (15), StDebuggerInspectorTest (11), StDebuggerMNUExtensionTest (3)
- StDebuggerRawObjectInspectorTest (1), StDebuggerStackCommandTreeBuilderTest (33)
- StDebuggerTest (67), StDebuggerToolbarCommandTreeBuilderTest (20)
- StDebuggerTreeTablePresenterTest (8), StFailingAssertionInspectorTest (3), TStDebuggerExtensionTest (4)

**NewTools-Debugger-Breakpoints-Tools** (5 classes, 36 tests)
- StHaltAndBreakpointControlTest (1), StHaltAndBreakpointControllerTest (11)
- StHaltBreakpointInspectionItemTest (8), StHaltCacheTest (13), StObjectBreakpointInspectionTest (3)

**NewTools-Debugger-Fuel-Tests** (1 class, 3 tests)
- FLDebuggerStackSerializerTest (3)

**NewTools-Debugger-Morphic** (1 class, 2 tests)
- StMorphicDebugSessionTest (2)

**NewTools-Inspector-Tests** (5 classes, 22 tests)
- StInspectorTest (12), StMetaInspectionTest (4), StObjectContextModelTest (1)
- StObjectContextPresenterTest (2), StObjectPrinterTest (3)

**NewTools-Playground-Tests** (6 classes, 29 tests)
- StPlaygroundInteractionModelTest (9), StPlaygroundPagePresenterTest (7)
- StPlaygroundPageSummaryPresenterTest (2), StPlaygroundPageTest (6)
- StPlaygroundPagesPresenterTest (1), StPlaygroundTest (4)

**NewTools-Spotter-Tests** (2 classes, 5 tests)
- StSpotterModelTest (2), StSpotterTest (3)

**NewTools-Spotter-Processors-Tests** (12 classes, 102 tests)
- StAbstractProcessorTest (1), StCamelCaseSplitTest (5), StClassProcessorTest (6)
- StGeneratorBlockIteratorTest (5), StGeneratorIteratorTest (6), StHistoryProcessorTest (4)
- StImplementorsProcessorTest (5), StIteratorsTest (28), StPackageProcessorTest (5)
- StUnifiedProcessorTest (24), StWindowsProcessorTest (6), StWorldMenuProcessorTest (7)

**NewTools-Transcript-Tests** (2 classes, 19 tests)
- StThreadSafeTranscriptTest (14), StTranscriptPresenterTest (5)

**NewTools-ObjectTranscript** (1 class, 4 tests)
- StObjectTranscriptPresenterTest (4)

**NewTools-Core-Tests** (2 classes, 9 tests)
- StPharoApplicationTest (3), StProtocolNameChooserPresenterTest (6)

**NewTools-FileBrowser-Tests** (17 classes, 82 tests)
- StAbstractFilterTest (1), StBitmapFilterTest (1), StBreadcrumbPresenterTest (1)
- StDirectoryFilterTest (1), StExtensionsFilterTest (1), StFileBrowserOpenTerminalCommandTest (4)
- StFileBrowserPreviewTest (8), StFileFilterTest (5), StFilePresenterTest (7)
- StGIFFilterTest (1), StJPEGFilterTest (1), StNavigationSystemTest (13), StNilFilterTest (1)
- StOpenDirectoryPresenterTest (11), StOpenFilePresenterTest (16), StPNGFilterTest (1)
- StSaveFilePresenterTest (9)

**NewTools-Finder-Tests** (4 classes, 27 tests)
- StFinderClassTest (7), StFinderExampleTest (4), StFinderPackageTest (7), StFinderSelectorTest (9)

**NewTools-SettingsBrowser-Tests** (1 class, 22 tests)
- StSettingsBrowserTest (22)

**NewTools-DocumentBrowser-Tests** (6 classes, 50 tests)
- MicDocumentBrowserLayoutModelTest (9), MicDocumentBrowserModelTest (8)
- MicPharoClassCommentResourceReferenceTest (4), MicPharoCommentResourceReferenceTest (4)
- MicPharoPackageCommentResourceReferenceTest (16), MicSectionBlockTest (9)

**NewTools-DocumentBrowser-GitHubResource-Tests** (2 classes, 15 tests)
- MicGitHubAPITest (5), MicGitHubRessourceReferenceTest (10)

**NewTools-DependencyAnalyser-Tests** (10 classes, 89 tests)
- StDependencyCheckerTest (2), StMessageSendAnalyzerTest (4), StPackageCycleDetectorTest (14)
- StPackageCycleTest (5), StPackageDependencyTest (10), StPackageDependencyWrapperTest (1)
- StPackageRelationGraphDiffTest (11), StPackageRelationGraphTest (26), StPackageTest (14)
- StTarjanAlgorithmTest (2)

**NewTools-RewriterTools-Tests** (10 classes, 45 tests)
- StRewriterAbstractToolTest (3), StRewriterExpressionFinderPresenterTest (5)
- StRewriterMatchToolPresenterTest (14), StRewriterOccurrencesBrowserPresenterTest (2)
- StRewriterReplaceWithPanelTest (1), StRewriterRuleEditorPresenterTest (6)
- StRewriterRuleLoaderPresenterTest (3), StRewriterRulesHelpPresenterTest (5)
- StRewriterScopeSelectorPresenterTest (5), StRewriterSearchForPanelTest (1)

**NewTools-RewriterTools-Backend-Tests** (1 class, 3 tests)
- RTPatternMatcherTest (3)

**NewTools-CodeCritiques-Tests** (5 classes, 18 tests)
- StCritiqueBrowserPresenterTest (3), StCritiquePackageSelectorPresenterTest (2)
- StCritiqueRuleSelectorPresenterTest (8), StCritiqueToolbarPresenterTest (2), StResetWindowPresenterTest (3)

**NewTools-DebugPointsBrowser-Tests** (1 class, 11 tests)
- DebugPointBrowserPresenterTest (11)

**NewTools-Scopes-Tests** (2 classes, 15 tests)
- ScopesManagerTest (13), ScopesTest (2)

**NewTools-Sindarin-Commands-Tests** (1 class, 9 tests)
- SindarinCommandsTest (9)

**NewTools-Sindarin-Tools** (1 class, 2 tests)
- StSindarinContextInteractionModelTest (2)

**NewTools-SpTextPresenterDecorators** (2 classes, 6 tests)
- SpTextPresenterDecoratorMorphicAdapterTest (4), SpTextPresenterDecoratorTest (2)

**NewTools-WindowManager-Tests** (1 class, 7 tests)
- SpClosedWindowListPresenterTest (7)

**NewTools-Window-Profiles** (4 classes, 15 tests)
- CavWindowProfileTest (4), CavWindowStrategyTest (3), CavroisWindowManagerTest (6)
- CavroisWindowPlaceHolderTest (2)

**Debugging-Utils-Tests** (3 classes, 22 tests)
- CodeSimulationWithHaltTest (2), ContextDebuggingTest (2), HaltTest (18)

**DebugPoints-Tests** (2 classes, 50 tests)
- DebugPointObserverTest (5), DebugPointTest (45)

**Debugger-Model-Tests** (16 classes, 90 tests)
- ArgumentNamesTest (8), AssignmentAndLiteralDebuggerTest (6), DebugSessionContexts2Test (3)
- DebugSessionContextsTest (2), DebugSessionExceptionTest (3), DebuggerModelTest (6)
- DebuggerTestCaseForRestartTest (2), DynamicMessageImplementorTest (9), FastStepThroughTest (11)
- IsContextPostMortemTest (1), RecompileTest (3), RestartTest (3), StepIntoTest (7)
- StepOverTest (10), StepThroughTest (11), TDebuggerTest (5)

**Debugger-Oups-Tests** (5 classes, 31 tests)
- OupsDebugRequestTest (7), OupsDebuggerSelectionStrategyTest (1), OupsDebuggerSelectorTest (13)
- OupsDebuggerSystemTest (5), OupsSingleDebuggerSelectorTest (5)

### Calypso Browser (17 packages, 241 classes, 5054 tests)

**Calypso-SystemQueries-Tests** (71 classes, 1808 tests)
- ClyAbstractClassScopeTest (29), ClyAllClassVariablesQueryTest (33), ClyAllClassesQueryTest (33)
- ClyAllExtensionMethodsQueryTest (33), ClyAllInstanceVariablesQueryTest (33)
- ClyAllMethodGroupsQueryTest (33), ClyAllMethodsQueryTest (33), ClyAllPackageTagsQueryTest (32)
- ClyAllPackagesQueryTest (33), ClyAllVariablesQueryTest (33), ClyBothMetaLevelClassScopeTest (40)
- ClyClassBindingsTest (12), ClyClassCommentsQueryTest (35), ClyClassExternalReferencesQueryTest (32)
- ClyClassGroupProviderTest (1), ClyClassHierarchyScopeTest (35), ClyClassQueryTest (31)
- ClyClassReferencesQueryTest (41), ClyClassScopeTest (48), ClyClassSideScopeTest (41)
- ClyClassVariableTest (6), ClyConstantMethodQueryTest (40), ClyExtendedClassGroupProviderTest (1)
- ClyExtendedMethodGroupProviderTest (1), ClyExtendingPackagesQueryTest (36)
- ClyExtensionLastSortedClassesTest (18), ClyGroupedClassVariablesTest (15)
- ClyGroupedExtendingPackagesTest (13), ClyGroupedInstanceVariablesTest (19)
- ClyGroupedVariablesTest (12), ClyHierarchicalSystemItemsTest (15)
- ClyHierarchicallySortedClassesTest (19), ClyInheritedMethodGroupProviderTest (1)
- ClyInstanceSideScopeTest (41), ClyInterestingSuperclassScopeTest (40), ClyLocalClassScopeTest (36)
- ClyMessageImplementorsQueryTest (38), ClyMessageSendersQueryTest (38)
- ClyMethodGroupProviderTest (1), ClyMethodQueryTest (31), ClyMethodScopeTest (26)
- ClyMethodSourcesQueryTest (35), ClyMethodVisibilityGroupsTest (15)
- ClyMethodsInProtocolGroupProviderTest (2), ClyMethodsInProtocolQueryTest (34)
- ClyMultipleClassRelationScopeTest (36), ClyNoTagClassGroupProviderTest (1)
- ClyPackageExtensionMethodsQueryTest (34), ClyPackageExtensionScopeTest (28)
- ClyPackageQueryTest (31), ClyPackageScopeTest (29), ClyQueryBrowserFilterTest (11)
- ClyRestUntaggedClassesQueryTest (38), ClySharedPoolReferencesQueryTest (33)
- ClySortByDefiningClassFunctionTest (1), ClySortMethodByPackageFunctionTest (6)
- ClySortMethodBySelectorFunctionTest (6), ClySortSystemItemFunctionTest (3)
- ClySubclassScopeTest (44), ClySuperclassScopeTest (47), ClySystemEnvironmentTest (3)
- ClyTaggedClassGroupProviderTest (3), ClyTaggedClassesQueryTest (34)
- ClyUnclassifiedMethodGroupProviderTest (1), ClyUnclassifiedMethodsQueryTest (33)
- ClyUntaggedClassesQueryTest (35), ClyVariableQueryTest (31), ClyVariableReadersQueryTest (37)
- ClyVariableReferencesQueryTest (39), ClyVariableReferencesTest (33), ClyVariableWritersQueryTest (37)

**Calypso-NavigationModel-Tests** (33 classes, 637 tests)
- ClyAsyncBrowserQueryCursorTest (14), ClyAsyncQueryResultTest (20), ClyAsyncQueryTest (40)
- ClyAsyncRawQueryCursorTest (14), ClyBrowserItemCursorTest (14), ClyBrowserQueryCursorTest (12)
- ClyBrowserQueryResultTest (11), ClyCompositeQueryTest (35), ClyCompositeScopeTest (39)
- ClyConstantQueryTest (33), ClyFilterQueryTest (41), ClyItemNameFilterTest (4)
- ClyNavigationEnvironmentTest (11), ClyQueryExampleTest (32), ClyQueryNavigationResultTest (10)
- ClyQueryTest (14), ClyRawItemCursorTest (14), ClyRawQueryResultTest (16), ClyRegexPatternTest (5)
- ClyScopeExampleTest (28), ClyScopeTest (7), ClySemiAsyncQueryResultTest (21)
- ClySortBrowserItemFunctionTest (2), ClySortByNameFunctionTest (2), ClySortItemGroupFunctionTest (2)
- ClySortedQueryResultTest (12), ClySubstringPatternTest (4), ClyTypedQueryTest (30)
- ClyTypedScopeTest (25), ClyUnionQueryTest (52), ClyUnknownQueryTest (20), ClyUnknownScopeTest (16)
- ClyWrapQueryTest (37)

**Calypso-Browser-Tests** (4 classes, 37 tests)
- ClyBrowserToolValidityTest (25), ClyNotebookPageRecyclerTest (8), ClyTextEditorToolMorphTest (1)
- NavigationInteractionTest (3)

**Calypso-SystemTools-FullBrowser-Tests** (5 classes, 54 tests)
- ClyAccrossWindowNavigationStateTest (8), ClyBrowserStateTest (5), ClyFullBrowserStateTest (11)
- ClyQueryViewMorphShouldExpandTest (14), ClyQueryViewStateTest (16)

**Calypso-SystemTools-QueryBrowser-Tests** (1 class, 13 tests)
- ClyQueryBrowserStateTest (13)

**Calypso-SystemPlugins-SUnit-Queries-Tests** (4 classes, 50 tests)
- ClyExpectedFailedTestMethodsQueryTest (35), ClyExpectedFailureMethodGroupProviderTest (1)
- ClyTestedClassMockTest (4), ClyTestedEnvironmentPluginTest (10)

**Calypso-SystemPlugins-Critic-Queries-Tests** (8 classes, 212 tests)
- ClyAllBasisCritiquesTest (32), ClyAllMethodCritiquesTest (32), ClyAllProblemMethodsTest (36)
- ClyConcreteGroupCritiquesTest (36), ClyCriticMethodGroupProviderTest (1), ClyCritiqueQueryTest (30)
- ClyFilteringCritiqueQueryTest (32), ClyGroupedCritiquesTest (13)

**Calypso-SystemPlugins-Deprecation-Queries-Tests** (2 classes, 36 tests)
- ClyDeprecatedMethodGroupProviderTest (1), ClyDeprecatedMethodsQueryTest (35)

**Calypso-SystemPlugins-FFI-Queries-Tests** (2 classes, 36 tests)
- ClyFFIMethodGroupProviderTest (1), ClyFFIMethodsTest (35)

**Calypso-SystemPlugins-Flags-Queries-Tests** (2 classes, 36 tests)
- ClyFlagMethodGroupProviderTest (1), ClyFlaggingMethodsQueryTest (35)

**Calypso-SystemPlugins-InheritanceAnalysis-Queries-Tests** (8 classes, 162 tests)
- ClyAbstractMethodGroupProviderTest (1), ClyAbstractMethodsQueryTest (35)
- ClyOverriddenMethodGroupProviderTest (1), ClyOverriddenMethodsQueryTest (40)
- ClyOverridingMethodGroupProviderTest (1), ClyOverridingMethodsQueryTest (39)
- ClyRequiredMethodGroupProviderTest (1), ClyUnimplementedMethodsQueryTest (44)

**Calypso-SystemPlugins-Reflectivity-Browser-Tests** (1 class, 9 tests)
- ClyInstallMetaLinkPresenterTest (9)

**Calypso-SystemPlugins-Reflectivity-Queries-Tests** (6 classes, 114 tests)
- ClyActiveBreakpointsQueryTest (39), ClyBreakpointMethodGroupProviderTest (1)
- ClyMethodCallCountersQueryTest (36), ClyMethodCounterGroupProviderTest (1)
- ClyMethodWatchQueryTest (36), ClyWatchMethodGroupProviderTest (1)

**Calypso-SystemPlugins-Traits-Queries-Tests** (6 classes, 114 tests)
- ClyInheritedTraitsHierarchyTest (18), ClyMergedSubclassesAndInheritedTraitsHierarchyTest (22)
- ClyMergedSuperclassesAndInheritedTraitsHierarchyTest (19), ClyTraitFirstSortFunctionTest (1)
- ClyTraitUserScopeTest (38), ClyTraitUsersHierarchyTest (16)

**Calypso-SystemPlugins-Undeclared-Queries-Tests** (2 classes, 37 tests)
- ClyUndeclaredMethodGroupProviderTest (1), ClyUndeclaredMethodsQueryTest (36)

**HeuristicCompletion-Tests** (27 classes, 281 tests)
- CoASTResultSetBuilderTest (19), CoAvoidRepeatedFetcherTest (8), CoBasicFetcherTest (2)
- CoBasicFetcherWithElementsTest (8), CoClassVariableFetcherTest (8)
- CoCompletionEngineCodeSnippetTest (1), CoCompletionEngineTest (65), CoFetcherWithNoResultsTest (3)
- CoFilterFetcherTest (8), CoFilterNarrowingComparisonTest (10), CoGlobalSelectorFetcherTest (8)
- CoGlobalVariableFetcherTest (8), CoHierarchyClassVariableFetcherTest (8)
- CoHierarchyImplementedSelectorsFetcherTest (8), CoHierarchyInstanceVariableFetcherTest (8)
- CoImplementedSelectorsFetcherTest (8), CoInitializeTypeInferenceTest (4)
- CoInstanceVariableFetcherTest (8), CoMethodVariablesFetcherTest (8), CoNarrowHistoryFetcherTest (11)
- CoRepeatedHierarchyImplementedSelectorsFetcherTest (8), CoResultSetResettingTest (2)
- CoResultSetTest (13), CoSequenceFetcherTest (11), CoSharedPoolVariableFetcherTest (8)
- CoStatisticsTest (23), CoSuperMessageHeuristicTest (5)

**HeuristicCompletion-Benchmarks-Tests** (1 class, 24 tests)
- CoStaticBenchmarksTest (24)

**NECompletion-Tests** (2 classes, 58 tests)
- CompletionContextTest (7), CompletionEngineTest (51)

### Refactoring & Tools (8 packages, 182 classes, 1143 tests)

**Refactoring-Transformations-Tests** (104 classes, 568 tests)
- EquivalentTreeTest (6), RBAbstractClassVariableParametrizedTest (7)
- RBAbstractInstanceVariableParametrizedTest (6), RBAbstractRefactoringTest (1)
- RBAbstractTransformationTest (6), RBAddAccessorsForClassTransformationTest (2)
- RBAddAssignmentTransformationTest (5), RBAddClassVariableRefactoringTest (2)
- RBAddClassVariableTransformationParametrizedTest (5), RBAddInstanceVariableParametrizedTest (3)
- RBAddInstanceVariableRefactoringTest (5), RBAddMessageSendTransformationTest (5)
- RBAddMethodRefactoringTest (3), RBAddMethodTransformationParametrizedTest (3)
- RBAddParameterParametrizedTest (15), RBAddPragmaTransformationTest (5)
- RBAddReturnStatementTransformationTest (6), RBAddSubtreeTransformationTest (4)
- RBAddTemporaryVariableParametrizedTest (3), RBAddVariableAccessorsParametrizedTest (6)
- RBAddVariableAccessorsWithLazyInitializationParametrizedTest (7)
- RBChildrenToSiblingsParametrizedTest (5), RBCopyPackageParametrizedTest (4)
- RBDeprecateMethodParametrizedTest (6), RBExtractMethodAndOccurrencesTest (3)
- RBExtractMethodRefactoringTest (13), RBExtractMethodToComponentRefactoringTest (5)
- RBExtractMethodTransformationTest (14), RBExtractSetUpMethodAndOccurrencesParametrizedTest (4)
- RBExtractSetUpMethodParametrizedTest (7), RBExtractToTemporaryParametrizedTest (9)
- RBFindAndReplaceParametrizedTest (5), RBFindAndReplaceSetUpParametrizedTest (2)
- RBInlineAllMethodParametrizedTest (5), RBInlineMethodFromComponentParametrizedTest (7)
- RBInlineMethodParametrizedTest (15), RBInlineParameterParametrizedTest (3)
- RBInlineTemporaryParametrizedTest (4), RBInsertClassParametrizedTest (10)
- RBMakeClassAbstractParametrizedTest (2), RBMergeInstanceVariableIntoAnotherParametrizedTest (4)
- RBMethodProtocolTransformationTest (4), RBMoveClassTransformationTest (3)
- RBMoveInstanceVariableToClassParametrizedTest (3), RBMoveMethodParametrizedTest (6)
- RBMoveMethodToClassParametrizedTest (1), RBMoveMethodToClassSideParametrizedTest (3)
- RBMoveMethodToClassSideRefactoringTest (2), RBMoveTemporaryVariableDefinitionTransformationTest (5)
- RBMoveVariableDefinitionParametrizedTest (5), RBProtectInstanceVariableParametrizedTest (2)
- RBProtectVariableTransformationTest (11), RBPullUpClassVariableParametrizedTest (3)
- RBPullUpInstanceVariableParametrizedTest (2), RBPullUpMethodParametrizedTest (9)
- RBPullUpMethodRefactoringTest (2), RBPushDownClassVariableParametrizedTest (7)
- RBPushDownInstanceVariableParametrizedTest (5), RBPushDownMethodParametrizedTest (3)
- RBPushDownMethodRefactoringTest (2), RBRealizeClassParametrizedTest (4), RBRegexTest (2)
- RBRemoveAllMessageSendsTransformationTest (8), RBRemoveAllSendersParametrizedTest (1)
- RBRemoveAssignmentTransformationTest (6), RBRemoveClassPushStateToSubclassesTest (2)
- RBRemoveClassRefactoringTest (13), RBRemoveClassTransformationTest (3)
- RBRemoveClassVariableParametrizedTest (2), RBRemoveClassVariableRefactoringTest (1)
- RBRemoveDirectAccessToVariableTransformationTest (6), RBRemoveInstanceVariable2ParametrizedTest (5)
- RBRemoveMethodParametrizedTest (2), RBRemoveMethodRefactoringTest (7)
- RBRemoveMethodsInHierarchyRefactoringTest (3), RBRemoveMethodsRefactoringTest (4)
- RBRemoveParameterParametrizedTest (4), RBRemovePragmaTransformationTest (5)
- RBRemoveProtocolTransformationTest (2), RBRemoveReturnStatementTransformationTest (5)
- RBRemoveSenderMethodParametrizedTest (10), RBRemoveSubtreeTransformationTest (6)
- RBRemoveTemporaryVariableTransformationTest (4), RBRenameAndDeprecateClassTransformationTest (1)
- RBRenameClassRefactoringTest (8), RBRenameClassVariableParametrizedTest (5)
- RBRenameInstanceVariableParametrizedTest (6), RBRenameMethodParametrizedTest (8)
- RBRenameMethodRefactoringTest (1), RBRenamePackageParametrizedTest (3)
- RBRenameTemporaryParametrizedTest (7), RBRenameVariableParametrizedTest (1)
- RBReplaceMessageSendParametrizedTest (9), RBReplaceSubtreeTransformationTest (7)
- RBSplitCascadeParametrizedTest (3), RBTemporaryToInstanceVariableParametrizedTest (4)
- RBTemporaryToInstanceVariableRefactoringTest (1), RBTransformationsTest (21)
- RBWithDifferentConstructorsParametrizedTest (1), RBWithDifferentsArgumentsParametrizedTest (1)
- ReRemoveUnusedTemporaryVariableRefactoringTest (3)
- ReSemanticsOfCompositeExtractMethodRefactoringTest (24)
- ReSemanticsOfExtractSetUpMethodRefactoringTest (4), ReSemanticsOfInlineMethodRefactoringTest (35)

**Refactoring-Core-Tests** (25 classes, 155 tests)
- LowLevelReflectiveAPITest (6), OCIsEssentialTest (16), RBClassTest (25), RBConditionTest (18)
- RBMaxOneAssignmentWithReferencesConditionTest (4), RBMethodNameTest (2), RBNamespaceTest (22)
- RBNotInCascadedMessageConditionTest (2), RBSearchTest (1), RBSharedPoolTest (5)
- RBSubtreeDoesNotContainReturnConditionTest (2), RBVariableTypeTest (4)
- RBVariablesNotReadBeforeWrittenConditionTest (4), ReClassHasSubclassesTest (3)
- ReClassesAreNotMetaClassConditionTest (4), ReDefinesSelectorsConditionTest (4)
- ReHierarchyDefinesMethodTest (4), ReMethodsDontReferToInstVarsTest (4)
- ReMethodsDontReferToSharedVarsTest (4), ReMethodsHaveNoSuperCallInSiblingsConditionTest (3)
- ReMethodsReceiveNoSupersendsTest (3), ReMethodsSendNoSupersendsTest (2)
- ReSafeMethodNameForbasedOnTest (6), ReSingleAssignmentConditionTest (2), ReUpToRootDefinesMethodTest (5)

**Refactoring-Changes-Tests** (3 classes, 52 tests)
- RBRefactoringChangeManagerPerformChangesTest (3), RBRefactoringChangeManagerTest (8)
- RBRefactoringChangeTest (41)

**Refactoring-Environment-Tests** (1 class, 27 tests)
- RBBrowserEnvironmentTest (27)

**Refactoring-DataForTesting** (1 class, 8 tests)
- RBTestAsDataForExtractSetupTransformationTest (8)

**Refactoring-UI-Tests** (15 classes, 42 tests)
- ReAddSubclassDriverTest (2), ReDuplicateClassDriverTest (2), ReExtractTempDriverTest (2)
- ReGenerateEqualAndHashDriverTest (2), ReGeneratePrintOnDriverTest (1), RePullUpMethodDriverTest (1)
- RePushDownMethodDriverTest (3), ReRemoveClassDriverTest (4), ReRemoveInstanceVariablesDriverTest (3)
- ReRenameInstanceVariableDriverTest (4), ReRenameMethodDriverTest (2)
- ReRenameSharedVariableDriverTest (4), StClassAndMethodsSelectionPresenterTest (4)
- StRefactoringAddClassPresenterTest (4), StRequestClassPresenterTest (4)

**SystemCommands-RefactoringSupport-Tests** (4 classes, 43 tests)
- StMethodNameEditorPresenterTest (18), SycMethodNameEditorTest (18), SycRefactoringPreviewTest (5)
- SycRefactoringStoreOnTest (2)

**SystemCommands-MethodCommands-Tests** (3 classes, 6 tests)
- SycConvertTempToinstVarCommandTest (2), SycInlineTempCommandTest (2), SycRenameArgOrTempCommandTest (2)

**SystemCommands-MessageCommands-Tests** (1 class, 1 test)
- SycHierarchicalSendersCommandTest (1)

**SystemCommands-PackageCommands-Tests** (2 classes, 3 tests)
- SycAddNewPackageCommandTest (2), SycRemoveNewPackageCommandTest (1)

**Tools-CodeNavigation-Tests** (3 classes, 50 tests)
- CNSelectorExtractionOnPositionTest (36), CNSelectorExtractionOnSelectionTest (12)
- CNSelectorExtractorTest (2)

**Tools-Tests** (3 classes, 18 tests)
- MethodClassifierTest (5), OpenToolTest (11), ProjectManagerTest (2)

**Tool-Profilers-Tests** (3 classes, 16 tests)
- AndreasSystemProfilerTest (8), MessageTallyTest (5), SpaceTallyTest (3)

**Tool-ExternalBrowser-Tests** (1 class, 2 tests)
- ExternalBrowserTest (2)

**Tool-ImageCleaner-Tests** (1 class, 1 test)
- ImageCleanerTest (1)

**NautilusRefactoring-Tests** (1 class, 4 tests)
- ChangesBrowserTest (4)

### SUnit Testing Framework (7 packages, 48 classes, 324 tests)

**SUnit-Tests** (26 classes, 207 tests)
- ClassFactoryForTestCaseTest (7), ClassFactoryWithNonDefaultEnvironmentTest (7), ExampleSetTest (6)
- FailingTearDownTest (2), FailingTestResourceTestCase (5), HashTesterTest (1)
- ManyTestResourceTestCase (6), PaAbstractExampleTest (1), PaCasesMatrixTest (1)
- PaCombinedMatrixExampleTest (1), PaMatrix3ExampleTest (1), PaMatrixExampleTest (1)
- PaSelectedCasesExampleTest (1), PaSimpleMatrixExampleTest (3), PaSuiteTest (11)
- ProcessMonitorTestServiceTest (47), ResumableTestFailureTestCase (1), SUnitExtensionsTest (18)
- SUnitTest (35), SimpleTestResourceTestCase (5), TestAsserterTest (2), TestCaseTest (2)
- TestExecutionEnvironmentTest (38), TestExecutionEnvironmentTestCase (2), TestFailureTestCase (1)
- TestResourceWithForkedProcessTestCase (2)

**SUnit-Core** (2 classes, 7 tests)
- ClassTestCase (5), HashAndEqualsTestCase (2)

**SUnit-MockObjects-Tests** (5 classes, 16 tests)
- MockBasicAPITest (6), MockMessageArgumentTest (3), MockMessageSendTest (4)
- MockMessageSequenceTest (1), MockVerifyMessageTest (2)

**SUnit-Rules-Tests** (3 classes, 3 tests)
- ReTestClassNameShouldEndWithTestTest (1), ReTestClassNameShouldNotEndWithTestsTest (1)
- ReTestClassNotInPackageWithTestEndingNameTest (1)

**SUnit-Support-UITesting-Tests** (2 classes, 6 tests)
- SimulateKeystrokesTest (3), SimulateMouseTest (3)

**SUnit-Visitor-Tests** (2 classes, 5 tests)
- SUnitSuiteBuilderTest (4), SUnitTestsCounterTest (1)

**DrTests-Tests** (6 classes, 29 tests)
- DTCoveragePluginPresenterTest (2), DTFilterableListPresenterTest (3), DTMockPluginTest (1)
- DrTestsTestRunnerTest (6), DrTestsTestRunnerUITest (1), DrTestsUITest (16)

**DrTests-TestCoverage-Tests** (2 classes, 13 tests)
- DTCoverageCollectorTest (2), DTTestCoverageTest (11)

**DrTests-TestCoverage-Tests-Mocks** (1 class, 3 tests)
- DTCoverageMockTest (3)

**DrTests-CommentsToTests-Tests** (1 class, 4 tests)
- CommentsToTestsTest (4)

**DrTests-TestsProfiling-Tests** (1 class, 8 tests)
- DTTestProfilingTest (8)

### Traits (2 packages, 29 classes, 281 tests)

**Traits-Tests** (29 classes, 281 tests)
- ClassTraitTest (5), MOPTraitTest (3), ShTraitBuilderTest (1), ShTraitInstallerTest (4)
- TraitChangesTest (3), TraitCompositionTest (13), TraitFileOutTest (4)
- TraitFluidClassDefinitionPrinterTest (22), TraitInTraitClassTest (2)
- TraitLegacyPharoClassDefinitionPrinterTest (11), TraitMCDefinitionsTest (16)
- TraitMethodDescriptionTest (4), TraitObsoleteClassTest (3), TraitOldPharoClassDefinitionPrinterTest (10)
- TraitOverloadingOfMethodsInTraitedClassTest (2), TraitPrecedenceCompositionTest (9)
- TraitPropagatingSlotChangesTest (2), TraitPureBehaviorTest (19), TraitSlotScopeTest (12)
- TraitSubclassingTraitedClassTest (12), TraitTest (54), TraitTestCase (19)
- TraitUsingTraitsWithSlotsTest (1), TraitWithAliasTest (8), TraitWithComplexSlotsTest (10)
- TraitWithConflictsTest (7), TraitWithMethodsInProtocolsTest (6), TraitWithPackagesTest (10)
- TraitWithSlotsTest (9)

### Slots & Variables (2 packages, 41 classes, 291 tests)

**Slot-Tests** (31 classes, 239 tests)
- AccessorInstanceVariableSlotTest (1), ArgumentVariableTest (2), BooleanSlotTest (3)
- ClassVariableTest (17), ExampleClassVariableTest (3), ExampleSlotWithFluidAPITest (1)
- ExampleSlotWithStateTest (3), GlobalVariableTest (6), LiteralVariableTest (3)
- PropertySlotTest (8), RelationSetTest (2), RelationSlotTest (5), SelfVariableTest (5)
- SlotAnnouncementsTest (23), SlotBasicTest (20), SlotClassVariableTest (5), SlotEnvironmentTest (1)
- SlotErrorsTest (12), SlotExampleMovieAndPersonTest (13), SlotIntegrationTest (17)
- SlotLayoutEqualityTest (4), SlotLayoutExtensionTest (17), SlotMethodRecompilationTest (4)
- SlotMigrationTest (20), SlotTest (14), SlotTraitsTest (8), SuperVariableTest (3)
- TemporaryVariableTest (11), ThisContextVariableTest (2), UnlimitedInstanceVariableSlotTest (2)
- WriteOnceSlotTest (4)

**VariablesLibrary-Tests** (10 classes, 52 tests)
- ComputedSlotTest (4), HistorySlotTest (7), InitializedClassVariableTest (4)
- InitializedSlotTest (6), LazyClassVariableTest (4), LazySlotTest (4), ObservableSlotTest (11)
- ProcessLocalSlotTest (4), WeakClassVariableTest (4), WeakSlotTest (4)

### Shift Class Builder (1 package, 16 classes, 102 tests)

**Shift-ClassBuilder-Tests** (16 classes, 102 tests)
- ShAnonymousClassInstallerTest (2), ShClassInstallerAnnouncementsTest (2)
- ShClassInstallerTest (28), ShClassSlotChangeDetectorTest (2), ShCreateClassTest (11)
- ShLayoutChangeDetectorTest (2), ShMetaclassChangeDetectorTest (2), ShModifyClassTest (4)
- ShSharedPoolChangeDetectorTest (2), ShSharedVariablesChangeDetectorTest (2)
- ShSlotChangeDetectorTest (2), ShSuperclassChangeDetectorTest (2), ShiftClassBuilderTest (26)
- ShiftClassSideClassBuilderTest (4), ShiftClassSideTraitBuilderTest (1), ShiftTraitBuilderTest (10)

### Reflectivity & Metalinks (2 packages, 19 classes, 431 tests)

**Reflectivity-Tests** (12 classes, 322 tests)
- CoverageDemoTest (3), LinkInstallerTest (40), MetaLinkAnonymousClassBuilderTest (14)
- MetaLinkObjectAPITest (18), MetaLinkRegistryTest (2), MetaLinkTest (11), OCCacheResetTest (1)
- ReflectiveMethodTest (22), ReflectivityControlTest (71), ReflectivityOnStackTest (23)
- ReflectivityReificationTest (112), ReflectivityTest (5)

**Reflectivity-Tools-Tests** (7 classes, 109 tests)
- BreakpointObserverTest (3), BreakpointTest (32), ExecutionCounterTest (10)
- MethodConstantTest (5), RuntimeTyperTest (1), VariableBreakpointTest (46), WatchTest (12)

**ReflectionMirrors-Primitives-Tests** (1 class, 40 tests)
- MirrorPrimitivesTest (40)

**Coverage-Tests** (1 class, 9 tests)
- CoverageCollectorTest (9)

### Monticello & Version Control (5 packages, 51 classes, 293 tests)

**Monticello-Tests** (28 classes, 148 tests)
- MCAncestryTest (2), MCChangeNotificationTest (3), MCClassDefinitionTest (13), MCDataStreamTest (3)
- MCDependencySorterTest (7), MCDictionaryRepositoryTest (15), MCDirectoryRepositoryTest (15)
- MCFileInTest (1), MCMergingTest (12), MCMethodDefinitionTest (7), MCOrganizationTest (1)
- MCPackageLoaderTest (1), MCPackageTest (2), MCPatchTest (1), MCReleaseTest (1)
- MCRepositoryAuthorizationTest (5), MCRepositoryTest (15), MCScannerTest (6)
- MCSerializationTest (2), MCSmalltalkhubRepositoryTest (1), MCSnapshotTest (5), MCSortingTest (2)
- MCStReaderTest (3), MCStWriterTest (9), MCWorkingCopyForExtensionsTest (1)
- MCWorkingCopyManagementTest (2), MCWorkingCopyTest (9), RPackageMonticelloSynchronisationTest (4)

**MonticelloTonel-Tests** (10 classes, 120 tests)
- TonelParserTest (19), TonelReaderTest (7), TonelReaderTraitCompositionTest (8)
- TonelRepositoryTest (2), TonelScannerTest (1), TonelSourceScannerTest (7), TonelWriterTest (19)
- TonelWriterV1Test (19), TonelWriterV2Test (19), TonelWriterV3Test (19)

**CodeImport-Tests** (1 class, 11 tests)
- ChunkImportTestCase (11)

**Epicea-Tests** (5 classes, 57 tests)
- EpAnnouncementsTest (4), EpCodeChangeIntegrationTest (32), EpDisabledIntegrationTest (7)
- EpLogTest (13), EpTriggeringIntegrationTest (1)

**EpiceaBrowsers-Tests** (8 classes, 129 tests)
- EpApplyPreviewerTest (40), EpApplyTest (28), EpCommentTest (1), EpFileOutModificationsTest (17)
- EpFilterTest (5), EpHasImpactFilterTest (10), EpLostChangesDetectorTest (5), EpRevertTest (23)

**Ombu-Tests** (9 classes, 91 tests)
- OmBlockFileStoreTest (2), OmDeferrerTest (2), OmFileStoreTest (24), OmMemoryStoreTest (18)
- OmRandomSuffixStrategyTest (3), OmSessionStoreNameStrategyTest (2), OmSessionStoreTest (22)
- OmStoreFactoryTest (2), OmStoreTest (16)

### Metacello (4 packages, 25 classes, 187 tests)

**Metacello-TestsCore** (13 classes, 122 tests)
- MetacelloCommonVersionNumberTestCase (13), MetacelloCoreIssue125TestCase (4)
- MetacelloCoreSymbolicVersionTest (4), MetacelloCoreVersionQueryTestCase (2)
- MetacelloGroupSpecTestCase (2), MetacelloLockTest (2), MetacelloPackagesSpecTestCase (15)
- MetacelloProjectReferenceSpecTestCase (2), MetacelloProjectSpecTestCase (2)
- MetacelloSemanticVersionNumberTestCase (33), MetacelloValueHolderSpecTestCase (2)
- MetacelloVersionNumberTestCase (38), MetacelloVersionSpecTestCase (3)

**Metacello-TestsMCCore** (11 classes, 46 tests)
- MCGitBasedNetworkRepositoryTest (1), MetacelloMCGroupSpecTestCase (2)
- MetacelloMCPackagesSpecTestCase (12), MetacelloMCProjectReferenceSpecTestCase (2)
- MetacelloMCProjectSpecTestCase (2), MetacelloMCValueHolderSpecTestCase (2)
- MetacelloMCVersionSpecTestCase (2), MetacelloPackageSpecTestCase (2)
- MetacelloRepositoriesSpecTestCase (13), MetacelloRepositorySpecTestCase (2)
- MetacelloRepositorySqueakCommonTestCase (6)

**Metacello-TestsReference** (1 class, 2 tests)
- MetacelloReferenceTestCase (2)

**Metacello-Gitlab-Tests** (1 class, 4 tests)
- MCGitlabRepositoryTest (4)

**MetacelloCommandLineHandler-Tests** (1 class, 13 tests)
- MetacelloCommandLineHandlerTest (13)

### Ring & Code Model (4 packages, 63 classes, 408 tests)

**Ring-Core-Tests** (31 classes, 290 tests)
- RGAnnouncementsTest (9), RGBehaviorTest (8), RGClassDescripitonStrategyTest (23)
- RGClassStrategyTest (10), RGClassTest (31), RGClassVariableTest (3), RGCommentTest (6)
- RGEnsureTraitTest (6), RGEnvironmentBackendTest (8), RGEnvironmentQueryInterfaceTest (1)
- RGEnvironmentTest (18), RGGlobalVariableTest (3), RGLayoutDefinitionTest (15)
- RGMCTraitCompositionVisitorTest (11), RGMetaclassStrategyTest (8), RGMetaclassTraitStrategyTest (3)
- RGMetaclassTraitTest (18), RGMethodTest (11), RGObjectTest (10), RGPackageTest (12)
- RGPoolVariableTest (2), RGReadOnlyBackendTest (1), RGReadOnlyImageBackendTest (19)
- RGSlotTest (5), RGStampParserTest (2), RGTraitAliasTest (5), RGTraitCompositionTest (6)
- RGTraitExclusionTest (7), RGTraitStrategyTest (10), RGTraitTest (17), RGUnresolvedValueTest (2)

**Ring-Definitions-Core-Tests** (8 classes, 77 tests)
- RGClassDefinitionTest (23), RGCommentDefinitionTest (6), RGElementDefinitionTest (1)
- RGMetaclassDefinitionTest (3), RGMetatraitDefinitionTest (1), RGMethodDefinitionTest (32)
- RGTraitDefinitionTest (6), RGVariableDefinitionTest (5)

**Ring-Definitions-Monticello-Tests** (1 class, 4 tests)
- RGMonticelloTest (4)

**Ring-Definitions-Tests-Containers** (1 class, 2 tests)
- RGPackageDefinitionTest (2)

**Ring-Monticello-Tests** (2 classes, 5 tests)
- RGMCClassTest (4), RGMCPackageTest (1)

**Ring-ChunkImporter-Tests** (1 class, 34 tests)
- Ring2ChunkImporterTest (34)

### Text & Microdown (7 packages, 76 classes, 856 tests)

**Text-Tests** (11 classes, 83 tests)
- FontTest (2), TextAlignmentTest (4), TextColorTest (2), TextEmphasisTest (16)
- TextFontChangeTest (2), TextFontReferenceTest (3), TextIndentTest (3), TextKernTest (2)
- TextLineEndingsTest (9), TextStreamTest (5), TextTest (35)

**Text-Diff-Tests** (1 class, 13 tests)
- TextDiffBuilderTest (13)

**Microdown-Tests** (51 classes, 643 tests)
- MicAnchorBlockTest (7), MicAnchorLinkerTest (3), MicAnnotatedParagraphBlockTest (15)
- MicArgumentListTest (30), MicBlockQuoteBlockTest (13), MicBlockTest (6), MicCitationBlockTest (6)
- MicCodeBlockTest (37), MicColumnsBlockTest (2), MicCommentTest (7), MicEnvironmentBlockTest (28)
- MicFigureBlockTest (24), MicFileResourceReferenceTest (16), MicFootnoteBlockTest (3)
- MicFormatBlockTest (23), MicHTTPResourceReferenceTest (9), MicHeaderBlockTest (16)
- MicHorizontalLineBlockTest (6), MicInlineBlockTest (2), MicInlineDelimiterTest (4)
- MicInlineExtenedSyntaxTest (8), MicInlineParserTest (36), MicInlineSpaceBlockTest (8)
- MicInlineTokenStreamTest (22), MicInputfileBlockTest (4), MicLinkBlockTest (17)
- MicMathBlockTest (17), MicMetaDataBlockTest (20), MicMicrodownTextualBuilderTest (43)
- MicNoteBlockTest (3), MicOrderedListBlockTest (17), MicParagraphBlockTest (15)
- MicParserTest (9), MicPharoEvaluatortBlockTest (7), MicPharoImageResourceReferenceTest (3)
- MicPharoScriptBlockTest (7), MicRawBlockTest (8), MicRawParagraphBlockTest (11)
- MicRelativeResourceReferenceTest (4), MicResourceReferenceTest (9), MicResourceSettingsTest (9)
- MicRootBlockTest (10), MicScriptBlockExtensionTest (4), MicSlideBlockTest (8)
- MicStringExtensionTest (2), MicUnknownResourceUriTest (3), MicUnorderedListBlockTest (25)
- MicZincPathResolverTest (5), MicrodownParserTest (41), MicrodownTest (10), MicrodownVisitorTest (1)

**Microdown-RichTextPresenter-Tests** (3 classes, 49 tests)
- MicTextPresenterTest (22), MicrodownPresenterTest (26), MicrodownSpecComponentTest (1)

**Rubric-Tests** (10 classes, 135 tests)
- RubEditingAreaTest (7), RubEditingStateTest (2), RubFindReplaceServiceTest (3)
- RubScrolledTextMorphTest (2), RubSelectionTest (6), RubSmalltalkEditorTest (63)
- RubTextEditorLocalHistoryTest (5), RubTextEditorTest (34), RubTextFieldAreaTest (10)
- RubTextSegmentMorphTest (3)

**Shout-Tests** (2 classes, 66 tests)
- SHRBStyleAttributionTest (64), SHStyleElementTest (2)

**PharoDocComment-Tests** (1 class, 12 tests)
- PharoDocCommentTest (12)

**EnlumineurFormatter-Tests** (20 classes, 298 tests)
- EFArrayExpressionTest (18), EFAssignmentExpressionTest (11), EFBlockExpressionOnlyTest (4)
- EFBlockExpressionTest (30), EFCascadeExpressionTest (19), EFContextTest (4), EFFFICallTest (6)
- EFFormatterTest (4), EFInternalTest (7), EFLiteralArrayExpressionTest (9)
- EFLiteralValueExpressionTest (6), EFMessageExpressionTest (83), EFMethodExpressionTest (24)
- EFParseErrorExpressionTest (2), EFPatternBlockExpressionTest (30), EFPragmaExpressionTest (3)
- EFReturnExpressionTest (7), EFSequenceExpressionTest (16), EFTemporariesExpressionTest (13)
- EFVariableExpressionTest (2)

### Roassal Visualization (9 packages, 98 classes, 796 tests)

**Roassal-Global-Tests** (22 classes, 159 tests)
- RSAdjacencyMatrixBuilderTest (13), RSAttachPointTest (5), RSCameraTest (6), RSChannelTest (4)
- RSCircleVennDiagramTest (5), RSCollectionTest (11), RSColoredTreePaletteTest (3)
- RSConnectionTest (3), RSDSMTest (10), RSDependencyTest (1), RSExamplesTest (2)
- RSForceBasedLayoutTest (5), RSHeatmapTest (6), RSMondrianTest (20), RSMonitorEventsTest (3)
- RSObjectWithPropertyTest (2), RSRTreeTest (26), RSRoassalTest (6), RSShapeBuilderTest (8)
- RSShapeTest (18), RSSunburstBuilderTest (1), RSWrapLabelTest (1)

**Roassal-Chart-Tests** (21 classes, 256 tests)
- RSAbstractPlotTest (22), RSBarPlotTest (16), RSBoxPlotShapeTest (16), RSBoxPlotTest (25)
- RSChartTickTest (6), RSChartTitleDecorationTest (2), RSClusterChartTest (3)
- RSCompositeChartTest (6), RSDensityPlotTest (10), RSHistogramPlotTest (8)
- RSKernelDensityTest (14), RSKiviatTest (5), RSLinePlotTest (31)
- RSMarkerDecorationParametrizedTest (4), RSMarkerTest (5), RSQuantileTest (2)
- RSScatterPlotTest (23), RSStatisticalMeasuresTest (13), RSTickLocatorTest (30)
- RSViolinPlotShapeTest (2), RSViolinPlotTest (13)

**Roassal-Layouts-Tests** (17 classes, 83 tests)
- RSAlignmentTest (10), RSAngleLineLayoutTest (6), RSCircularAroundAVertexLayoutTest (4)
- RSClusteringLayoutTest (4), RSDominanceTreeLayoutTest (1), RSFlowLayoutTest (3)
- RSGridLayoutTest (6), RSHorizontalDominanceTreeLayoutTest (1), RSHorizontalTreeLayoutTest (7)
- RSLayoutBuilderTest (4), RSLayoutTest (1), RSLocationTest (13), RSOvalLayoutTest (2)
- RSResizeTest (8), RSSugiyamaLayoutTest (2), RSTreeLayoutTest (9), RSVerticalGridLayoutTest (2)

**Roassal-Shapes-Tests** (12 classes, 148 tests)
- RSAthensRendererTest (1), RSBoundingTest (10), RSCanvasTest (34), RSCircleTest (2)
- RSCompositeTest (12), RSGroupTest (12), RSLabelTest (11), RSLineBuilderTest (41)
- RSLinesTest (18), RSNormalizerTest (1), RSPBoundingTest (3), RSPLinesTest (3)

**Roassal-Interaction-Tests** (12 classes, 72 tests)
- RSCanvasControllerTest (4), RSDraggableCanvasTest (4), RSDraggableTest (3), RSElasticBoxTest (3)
- RSGhostDraggableTest (2), RSHighlightableTest (8), RSLabeledTest (1), RSPopupTest (8)
- RSSearchInCanvasInteractionTest (13), RSSelectionInCanvasInteractionTest (12)
- RSShadowInteractionTest (8), RSTransformableTest (6)

**Roassal-Animation-Tests** (5 classes, 20 tests)
- RSEasingInterpolatorTest (1), RSPAnimationTest (4), RSParallelAnimationTest (5)
- RSSequentialAnimationTest (5), RSTransitionAnimationTest (5)

**Roassal-BaselineMap-Tests** (1 class, 3 tests)
- RSMapBuilderTest (3)

**Roassal-Inspector-Tests** (2 classes, 5 tests)
- RSInspectorShapeTest (1), RSSelectionPresentationInteractionTest (4)

**Roassal-Spec-Tests** (2 classes, 3 tests)
- RSSpecExamplesTest (1), RoassalSpecTest (2)

**Roassal-UML-Tests** (1 class, 2 tests)
- RSUMLClassBuilderTest (2)

**Roassal-SVG-Tests** (1 class, 3 tests)
- RSSVGTest (3)

**Roassal-Mondrian** (1 class, 3 tests)
- RSFlowCanvasTest (3)

**Hiedra-Tests** (4 classes, 16 tests)
- HiFastTableExampleTest (1), HiRulerBuilderTest (12), HiRulerTest (2), HiSpecExampleTest (1)

### Network & Protocols (6 packages, 64 classes, 549 tests)

**Zinc-Tests** (24 classes, 233 tests)
- ZnBivalentWriteStreamTest (2), ZnChunkedStreamTest (10), ZnClientTest (50)
- ZnDigestAuthenticatorTest (2), ZnDispatcherDelegateTest (1), ZnEasyTest (10)
- ZnEntityReaderTest (4), ZnEntityTest (17), ZnEntityWriterTest (4), ZnHeadersTest (9)
- ZnHtmlOutputStreamTest (14), ZnLimitedReadStreamTest (10), ZnLineReaderTest (4)
- ZnMagicCookieJarTest (3), ZnMagicCookieTest (3), ZnMessageBenchmarkTest (2), ZnOptionsTest (6)
- ZnRequestLineTest (4), ZnRequestTest (8), ZnResponseTest (12), ZnServerTest (31)
- ZnStatusLineTest (7), ZnUserAgentSessionTest (3), ZnUtilsTest (17)

**Zinc-Character-Encoding-Tests** (12 classes, 121 tests)
- ZnBase64EncoderTest (10), ZnBufferedReadStreamTest (9), ZnBufferedReadWriteStreamTest (11)
- ZnBufferedStreamByteTest (5), ZnBufferedWriteStreamTest (4), ZnCRLFReadStreamTest (3)
- ZnCharacterEncoderTest (42), ZnCharacterStreamTest (12), ZnFastLineReaderTest (2)
- ZnNewLineWriterStreamTest (2), ZnPercentEncoderTest (7), ZnPositionableReadStreamTest (14)

**Zinc-Resource-Meta-Tests** (5 classes, 81 tests)
- ZnFileUrlTest (12), ZnMimeTypeTest (12), ZnMultiValueDictionaryTest (4)
- ZnResourceMetaUtilsTest (3), ZnUrlTest (50)

**Zinc-HTTP-Examples** (7 classes, 18 tests)
- ZnImageExampleDelegateTest (3), ZnKeyValueStoreTest (1), ZnPrefixMappingDelegateTest (3)
- ZnReadEvalPrintDelegateTest (1), ZnStaticFileDecoratorDelegateTest (1)
- ZnStaticFileServerDelegateTest (6), ZnUrlShortnerDelegateTest (3)

**Zinc-Zodiac-Tests** (1 class, 10 tests)
- ZnHTTPSTest (10)

**Zodiac-Tests** (10 classes, 95 tests)
- ZdcAbstractSocketStreamTest (15), ZdcByteArrayManagerTest (2), ZdcIOBufferTest (13)
- ZdcOptimizedSocketStreamTest (15), ZdcPluginSSLSessionFinalizationTest (1)
- ZdcPluginSSLSessionTest (2), ZdcReferenceSocketStreamTest (15), ZdcSecureSocketStreamTest (2)
- ZdcSimpleSocketStreamTest (15), ZdcSocketStreamTest (15)

**Network-Tests** (15 classes, 103 tests)
- Base64MimeConverterTest (3), Base64Test (4), NetNameResolverTest (2), NetworkIPv6StringTest (8)
- QuotedPrintableMimeConverterTest (16), SMTPClientTest (1), SocketAddressTest (5)
- SocketStreamTest (24), TCPSocketEchoTest (1), TCPSocketTest (9), UDPSocketEchoTest (1)
- UDPSocketTest (2), UUIDGeneratorTest (5), UUIDPrimitivesTest (13), UUIDTest (9)

**Network-Mail-Tests** (3 classes, 12 tests)
- MailAddressParserTest (3), MailAddressTokenizerTest (3), MailMessageTest (6)

### Serialization & Data (5 packages, 59 classes, 1256 tests)

**Fuel-Core-Tests** (44 classes, 827 tests)
- FLBasicSerializationTest (79), FLBinaryFileStreamBasicSerializationTest (79)
- FLBlockClosureSerializationTest (18), FLByteArrayBasicSerializationTest (79)
- FLCompiledMethodSerializationTest (6), FLConfigurationTest (25), FLContextSerializationTest (9)
- FLConvenienceExtensionTest (3), FLCreateClassSerializationTest (41), FLCreateTraitSerializationTest (12)
- FLDecoderTest (8), FLEncoderDecoderTest (5), FLEncoderTest (16), FLEphemeronTest (7)
- FLFileReferenceStreamBasicSerializationTest (79), FLFullBasicSerializationTest (86)
- FLFullBlockClosureSerializationTest (8), FLFullHeaderSerializationTest (6)
- FLGZippedBasicSerializationTest (82), FLGlobalClassSerializationTest (11)
- FLGlobalEnvironmentTest (13), FLGlobalSendSerializationTest (2), FLGlobalTraitSerializationTest (11)
- FLHashedCollectionSerializationTest (2), FLHeaderSerializationTest (6), FLHookedSubstitutionTest (10)
- FLIgnoredVariablesTest (4), FLIndexStreamTest (3), FLLimitingSerializationTest (5)
- FLMaterializerConvenienceMethodsTest (6), FLMaterializerTest (12), FLMigrationTest (10)
- FLPluggableSubstitutionTest (12), FLProcessSerializationTest (1), FLSequencedSerializationTest (2)
- FLSerializerConvenienceMethodsTest (3), FLSerializerTest (34), FLSignatureTest (2)
- FLSimpleStackTest (5), FLSingletonTest (3), FLSortedCollectionSerializationTest (5)
- FLUserGuidesTest (6), FLVersionTest (7), FLWeakObjectsTest (4)

**STON-Tests** (10 classes, 310 tests)
- STONCStyleCommentsSkipStreamTest (6), STONJSONTest (11), STONLargeWriteReadTest (37)
- STONReaderTest (54), STONTest (9), STONWriteAsciiOnlyReadTest (37)
- STONWritePrettyPrinterReadTest (37), STONWriteReadCommentsTest (37), STONWriteReadTest (37)
- STONWriterTest (45)

**Compression-Tests** (5 classes, 29 tests)
- GZipReadStreamTest (1), ZipArchiveTest (15), ZipCrcTest (9), ZipExtensionTest (2), ZipWriteStreamTest (2)

### Other Utilities & Libraries (26 packages, 185 classes, 1482 tests)

**Announcements-Core-Tests** (3 classes, 65 tests)
- AnnouncementSetTest (2), AnnouncerTest (29), WeakAnnouncerTest (34)

**Beacon-Core-Tests** (5 classes, 16 tests)
- MemoryLoggerTest (8), SignalLoggerTest (3), SignalTest (2), StackSignalTest (1), WrapperSignalTest (2)

**BeautifulComments** (1 class, 5 tests)
- BCBeautifulCommentsSettingsTest (5)

**ClassAnnotation-Tests** (7 classes, 66 tests)
- ActiveClassAnnotationsTest (5), ClassAnnotationTest (28), CompositeAnnotationContextTest (5)
- QueryAnnotationsFromClassTest (10), RegisteredClassAnnotationsTest (11), SimpleAnnotationContextTest (1)
- VisibleClassAnnotationsTest (6)

**Clap-Tests** (16 classes, 110 tests)
- ClapApplicationTest (3), ClapCodeEvaluatorTest (3), ClapCommandSpecTest (15), ClapCommandTest (12)
- ClapContextTest (9), ClapDocumentationTest (2), ClapFlagTest (11), ClapHelloTest (8)
- ClapHelloWorldTest (6), ClapMatchesTest (9), ClapMeaningsTest (4), ClapParameterTest (4)
- ClapParameterizedTest (7), ClapPharoVersionTest (9), ClapPositionalTest (5), ClapValidationTest (3)

**Commander-Core-Tests** (3 classes, 10 tests)
- CmdCommandActivationStrategyTest (2), CmdCommandActivatorTest (3), CmdMenuTest (5)

**Commander2-Tests** (3 classes, 26 tests)
- CmCommandDecoratorTest (7), CmCommandTest (7), CmCommandsGroupTest (12)

**Commander2-UI-Tests** (1 class, 7 tests)
- CmUICommandTest (7)

**Equals-Tests** (3 classes, 17 tests)
- EqFruitComparisonTest (4), EqPersonComparisonTest (5), TEqualityTest (8)

**FuzzyMatcher-Tests** (1 class, 9 tests)
- FuzzyMatcherTest (9)

**Jobs-Tests** (1 class, 8 tests)
- JobTest (8)

**Keymapping-Tests** (10 classes, 46 tests)
- CharacterKeyCombinationTest (2), KMCategoryTest (3), KMCombinationTest (9), KMDispatchChainTest (3)
- KMDispatcherTest (5), KMKeymapBuilderTest (3), KMKeymapTest (2), KMPerInstanceTest (1)
- KMShortcutPrinterTest (2), KMShortcutTest (16)

**Manifest-Tests** (2 classes, 24 tests)
- BuilderManifestTest (20), SmalllintManifestCheckerTest (4)

**Math-Operations-Extensions-Tests** (2 classes, 3 tests)
- MathOperationnsExtensionsTest (1), MathOperationsExtensionsTest (2)

**NumberParser-Tests** (2 classes, 38 tests)
- NumberParserTest (25), NumberParsingTest (13)

**NumericInterpolator-Tests** (12 classes, 43 tests)
- NSClampTest (3), NSDomainAndRangeTest (2), NSLinearScaleTest (7), NSLogScaleTest (5)
- NSNiceLinearTicksGeneratorTest (2), NSNumberTest (4), NSOrdinalScaleTest (5)
- NSPolylinearScaleTest (1), NSPowScaleTest (3), NSSLnScaleTest (3), NSScaleTest (5), NSSymLogScaleTest (3)

**ProfStef-Tests** (10 classes, 45 tests)
- AbstractTutorialTest (4), HowToMakeYourOwnTutorialTest (6), LessonInstanciationTest (2)
- PharoSyntaxTutorialTest (6), PharoTutorialGoOnMockTutorialTest (5), PharoTutorialGoTest (5)
- PharoTutorialNavigationTest (6), TutorialPlayerTutorialAccessorTest (2)
- TutorialPlayerWithMockTutorialTest (4), TutorialTest (5)

**Random-Tests** (1 class, 16 tests)
- RandomTest (16)

**Regex-Core-Tests** (3 classes, 196 tests)
- RxExtensionsTest (2), RxMatcherTest (176), RxParserTest (18)

**RTree-Tests** (2 classes, 35 tests)
- RTCollectionTest (8), RTreeTest (27)

**SortFunctions-Tests** (3 classes, 21 tests)
- ChainedSortFunctionTest (2), SortFunctionTest (15), ThreeWayComparisonTest (4)

**TaskIt-Tests** (15 classes, 108 tests)
- TKTBasicTaskTest (6), TKTCommonQueueWorkerPoolTest (7), TKTFutureTest (49)
- TKTLocalProcessTaskRunnerTest (2), TKTNewProcessTaskRunnerTest (3), TKTParameterizedServiceTest (3)
- TKTPharoProcessProviderTest (1), TKTServiceManagerTest (3), TKTServiceMemoryLeakTest (4)
- TKTServiceTest (4), TKTSubclassServiceTest (3), TKTTaskTimeoutTest (2), TKTWorkerMemoryLeakTest (5)
- TKTWorkerPoolTest (9), TKTWorkerTest (7)

**Transcript-NonInteractive-Tests** (1 class, 4 tests)
- NonInteractiveTranscriptTest (4)

**UndefinedClasses-Tests** (1 class, 13 tests)
- UndefinedClassTest (13)

**EmergencyDebugger-Tests** (2 classes, 63 tests)
- EDDebuggingAPITest (27), EDEmergencyDebuggerTest (36)

**Sindarin-Tests** (2 classes, 88 tests)
- SindarinDebugSessionTest (3), SindarinDebuggerTest (85)

**RottenTestsFinder-Tests** (5 classes, 9 tests)
- RTFLeadsToAssertPrimitiveCallCheckerTest (1), RTFMethodCallsCollectorTest (1)
- RTFSelfCallInterpreterTest (5), RTFSelfCallTreeCleanerTest (1), RottenTestsFinderTest (1)

**ReleaseTests** (7 classes, 85 tests)
- NoUnusedVariablesLeftTest (3), ObsoleteTest (3), ProperMethodCategorizationTest (20)
- ProperPackagesTest (4), ProperlyImplementedSUnitClassesTest (3), ProtocolConventionsTest (8)
- ReleaseTest (44)

**General-Rules-Tests** (73 classes, 223 tests)
- FloatReferencesRuleTest (3), OverridesDeprecatedMethodRuleTest (3), ReAsClassRuleTest (3)
- ReAsOrderedCollectionNotNeededRuleTest (3), ReAssertWithBooleanEqualtiyRuleTest (5)
- ReAssignmentInIfTrueRuleTest (2), ReAssignmentWithoutEffectRuleTest (2)
- ReBaselineProperlyPackagedRuleTest (2), ReBaselineWithProperSuperclassRuleTest (2)
- ReBetweenAndRuleTest (2), ReClassNameInSelectorRuleTest (2), ReClassNotCategorizedRuleTest (3)
- ReClassVariableCapitalizationRuleTest (2), ReClassVariableNeitherReadNorWrittenRuleTest (2)
- ReCollectSelectNotUsedRuleTest (3), ReCollectionProtocolRuleTest (3), ReCyclomaticComplexityRuleTest (2)
- ReDeadBlockRuleTest (4), ReEqualsTrueRuleTest (3), ReEquivalentSuperclassMethodsRuleTest (2)
- ReExcessiveArgumentsRuleTest (2), ReExcessiveInheritanceRuleTest (2), ReExcessiveVariablesRuleTest (2)
- ReExtraBlockRuleTest (2), ReGlobalVariablesUsageRuleTest (1), ReGuardClauseRuleTest (3)
- ReIfTrueIfFalseUselessRuleTest (2), ReImplementedNotSentRuleTest (3)
- ReInconsistentMethodClassificationRuleTest (2), ReInstanceVariableCapitalizationRuleTest (4)
- ReJustSendsSuperRuleTest (1), ReKeysDoRuleTest (4), ReLiteralArrayCharactersRuleTest (2)
- ReLocalMethodsSameThanTraitRuleTest (2), ReLongMethodsRuleTest (2)
- ReMethodSelectorKeywordCasingRuleTest (2), ReMethodSourceContainsLinefeedsRuleTest (1)
- ReMultiplePeriodsTerminatingStatementRuleTest (1), ReNilBranchRuleTest (3)
- ReNoNilAssignationInInitializeRuleTest (2), ReNoUnusedInstanceVariableRuleTest (1)
- ReNotEliminationRuleTest (3), ReNotOptimizedIfNilRuleTest (2), ReNotOptimizedIfRuleTest (2)
- RePackageManifestShouldBePackagedInManifestTagRuleTest (2), RePointRuleTest (2)
- ReRefersToClassRuleTest (2), ReSearchingLiteralRuleTest (2), ReSelfSentNotImplementedRuleTest (1)
- ReSentButNotUnderstoodBySuperRuleTest (1), ReSentNotImplementedRuleTest (1)
- ReShouldSendSuperInitializeAsFirstMessageTest (4), ReSizeCheckRuleTest (4), ReSmalllintTest (58)
- ReStatementsAfterReturnConditionalRuleTest (3), ReStringConcatenationRuleTest (2)
- ReSuperWithoutSendTest (1), ReTempVarOverridesInstVarRuleTest (1)
- ReTemporaryNeitherReadNorWrittenRuleTest (2), ReTemporaryVariableCapitalizationRuleTest (4)
- ReTempsReadBeforeWrittenRuleTest (1), ReTestCaseShouldNotUseInitializeRuleTest (2)
- ReThemeAccessRuleTest (4), ReTrueFalseDuplicationRuleTest (2), ReUnclassifiedMethodsRuleTest (2)
- ReUncommonMessageSendRuleTest (1), ReUnoptimizedAndOrRuleTest (2), ReUnoptimizedToDoRuleTest (2)
- ReUnwindBlocksRuleTest (4), ReUseSetUpRuleTest (2), ReUsesTrueRuleTest (1)
- ReYourselfNotUsedRuleTest (2), SendsDeprecatedMethodToGlobalRuleTest (4)

**Renraku-Tests** (28 classes, 102 tests)
- ReAbstractRuleTest (1), ReBasicScenarioExceptionStrategyTest (3)
- ReClassSideInitializeMethodProtocolRuleTest (4), ReClassSideResetMethodProtocolRuleTest (4)
- ReCompactSourceCodeRuleTest (1), ReCritiqueTest (6), ReExceptionStrategyTest (1)
- ReInstanceSideBaselineMethodProtocolRuleTest (4), ReInstanceSideEqualsMethodProtocolRuleTest (4)
- ReInstanceSideFinalizeMethodProtocolRuleTest (4), ReInstanceSideHashMethodProtocolRuleTest (4)
- ReInstanceSideInitializeMethodProtocolRuleTest (4), ReInstanceSidePrintOnMethodProtocolRuleTest (4)
- ReInstanceSideSpeciesMethodProtocolRuleTest (4), ReInstanceSideValueMethodProtocolRuleTest (4)
- ReMethodSourceCleanerTest (8), ReNoPrintStringInPrintOnRuleTest (2)
- ReProperClassMethodProtocolRuleTest (4), ReProperInstanceMethodProtocolRuleTest (4)
- ReProperMethodProtocolRuleTest (4), ReRuleManagerTest (2), ReSmalllintCheckerTest (1)
- ReSmokeExceptionStrategyTest (3), ReSourceCodeLineTest (4), ReVarSearchSourceAnchorTest (3)
- RenrakuExtensionsTest (3), RenrakuGlobalBanningTest (5), RenrakuTest (7)

**Specific-Rules-Tests** (1 class, 3 tests)
- PharoBootstrapRuleTest (3)

**ReferenceFinder-Core** (1 class, 1 test)
- ReferenceFinderTest (1)

**AI-Algorithms-Graph-Tests** (16 classes, 84 tests)
- AIAstarTest (8), AIBFSTest (5), AIBellmanFordTest (4), AIDijkstraTest (8), AIDinicTest (2)
- AIGraphAlgorithmTest (4), AIGraphReducerTest (12), AIHitsTest (7), AIKruskalTest (5)
- AILongestPathInDAGTest (2), AILongestPathInDCGTest (4), AIPrimTest (3)
- AIShortestPathInDAGTest (2), AITarjanTest (7), AITopologicalSortingTest (6), AIWeightedHitsTest (5)

---

## Summary Statistics

**By Category:**
- Core Kernel & Collections: 170 classes, 5919 tests
- Compiler & AST: 133 classes, 2400 tests
- System & Files: 121 classes, 1511 tests
- FFI: 44 classes, 413 tests
- Graphics & Morphic: 74 classes, 621 tests
- Spec2 UI: 240 classes, 3127 tests
- NewTools & IDE: 211 classes, 2257 tests
- Calypso Browser: 241 classes, 5054 tests
- Refactoring & Tools: 182 classes, 1143 tests
- SUnit Framework: 48 classes, 324 tests
- Traits: 29 classes, 281 tests
- Slots & Variables: 41 classes, 291 tests
- Shift Class Builder: 16 classes, 102 tests
- Reflectivity: 19 classes, 431 tests
- Monticello & VCS: 51 classes, 293 tests
- Metacello: 25 classes, 187 tests
- Ring: 63 classes, 408 tests
- Text & Microdown: 76 classes, 856 tests
- Roassal: 98 classes, 796 tests
- Network: 64 classes, 549 tests
- Serialization: 59 classes, 1256 tests
- Other Utilities: 185 classes, 1482 tests

**Total:** 2092 test classes, 29407 tests across 246 packages

---

## Usage Notes

### Testing Strategy
1. Start with Tier 1 (Core VM mechanics) - these test fundamental bytecode execution
2. Add Tier 2 (Numbers) - exercises primitive operations
3. Add Tier 3 (Classes/Methods) - validates method lookup and compilation
4. Add Tier 4 (Collections) - broader collection coverage
5. Add Tier 5 (FFI) - validates external function interface

### Running Tests
Currently, tests are run by injecting `run_sunit_tests.st` into a fresh image:
```bash
/Users/wohl/Downloads/pharo /tmp/Pharo.image eval --save \
  "'/Users/wohl/src/iospharo/scripts/run_sunit_tests.st' asFileReference fileIn"
./build/test_load_image /tmp/Pharo.image
cat /tmp/sunit_test_results.txt
```

### Known Limitations
- Our VM currently lacks full FFI support, so FFI-heavy tests may fail
- Graphics/UI tests require display driver functionality
- Some tests depend on features outside core VM scope (reflectivity, metalinks)

### Next Steps
1. Add Tier 1 tests to `scripts/run_sunit_tests.st`
2. Fix any failures revealed
3. Progressively work through Tiers 2-5
4. Document any systematic test exclusions with rationale

---

**Document Maintained By:** iospharo VM development
**Last Updated:** 2026-02-06
**Image Version:** Pharo 13.1 64-bit (ad5c9e4)
