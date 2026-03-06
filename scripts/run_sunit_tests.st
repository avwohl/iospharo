"SUnit test runner for custom VM testing.
 See CLAUDE.md 'Running Official Pharo Test Suite' for usage.
 Results: /tmp/sunit_test_results.txt (summary)
          /tmp/sunit_test_detail.txt (per-test with run number)
 Run counter: /tmp/sunit_run_number.txt (auto-incremented)"!

Object subclass: #SUnitRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

"UndefinedObject >> findNextHandlerContext terminates the handler context chain.
 Context >> findNextHandlerContext calls sender findNextHandlerContext; when sender
 is nil, UndefinedObject needs this method to return nil (no more handlers) instead
 of triggering an infinite DNU loop. Standard Cog VM avoids this via optimized C
 exception primitives, but our Smalltalk code path requires the method."
UndefinedObject compile: 'findNextHandlerContext
  "Terminate the handler context chain -- no more contexts to search."
  ^ nil' classified: 'private'!

!SUnitRunner class methodsFor: 'running'!
nextRunNumber
  "Read, increment, and persist the run counter."
  | file num |
  file := '/tmp/sunit_run_number.txt' asFileReference.
  num := [file contents asInteger] on: Error do: [:e | 0].
  num := num + 1.
  file writeStreamDo: [:s | s nextPutAll: num printString].
  ^ num! !

!SUnitRunner class methodsFor: 'running'!
runDiagnostic: stream
  "Quick sanity check - verify basic forking and exception handling work"
  stream nextPutAll: '=== DIAGNOSTICS ==='; cr. stream flush.
  [| sem done |
   sem := Semaphore new. done := false.
   [[ 1/0 ] on: ZeroDivide do: [:e | done := true]. sem signal] fork.
   (sem waitTimeoutSeconds: 3) ifTrue: [stream nextPutAll: 'DIAG: fork+exception TIMEOUT'; cr]
   ifFalse: [stream nextPutAll: 'DIAG: fork+exception OK: '; nextPutAll: done printString; cr].
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'DIAG ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  stream nextPutAll: '=== END DIAGNOSTICS ==='; cr; cr. stream flush! !

!SUnitRunner class methodsFor: 'running'!
runAllTests
  "Run all tests and write results to /tmp/sunit_test_results.txt and /tmp/sunit_test_detail.txt"
  | testClasses stream detail runNum totalP totalF totalE totalS totalT seen addOnce batchStart batchEnd batchClasses timeoutScale watchdogTimeout |
  runNum := self nextRunNumber.

  testClasses := OrderedCollection new.
  "Use a Set to track what we've already added - avoids duplicate iteration bug"
  seen := IdentitySet new.
  addOnce := [:cls | (seen includes: cls) ifFalse: [seen add: cls. testClasses add: cls]].

  "=== Original 15 (all passing) ==="
  {SortedCollectionTest. IdentitySetTest.
    SmallIntegerTest. IntegerTest. FloatTest. FractionTest.
    PointTest. CharacterTest.
    DictionaryTest. SetTest. BagTest. IntervalTest.
    SymbolTest. OrderedCollectionTest. ArrayTest.
    StringTest. HeapTest} do: addOnce.

  "=== Tier 1: Core VM mechanics ==="
  #(BlockClosureTest ContextTest ExceptionTest BecomeTest
    BooleanTest TrueTest FalseTest ProtoObjectTest ObjectTest
    UndefinedObjectTest SemaphoreTest RecursionStopperTest
    LocalRecursionStopperTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 2: Numbers and arithmetic ==="
  #(LargePositiveIntegerTest LargeNegativeIntegerTest IntegerDigitLogicTest
    NumberTest MagnitudeTest ScaledDecimalTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 3: Classes, methods, processes ==="
  #(BehaviorTest CompiledCodeTest CompiledBlockTest
    ClassDescriptionTest ClassHierarchyTest MetaClassTest
    BasicBehaviorClassMetaclassTest PragmaTest
    ProcessTerminateBugTest ProcessSpecificTest
    MonitorTest DelayTest DeprecationTest
    MessageNotUnderstoodTest WeakMessageSendTest
    AllocationTest ObjectLayoutTest DependentsArrayTest
    SharedPoolTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 3b: Traits (run early — later class-modifying tests contaminate
     trait invariant checks like testTraitsUsersSanity) ==="
  #(TraitTest TraitCompositionTest ClassTraitTest MOPTraitTest
    TraitPureBehaviorTest TraitPrecedenceCompositionTest
    TraitWithAliasTest TraitWithConflictsTest
    TraitChangesTest TraitMethodDescriptionTest
    TraitOverloadingOfMethodsInTraitedClassTest
    TraitPackagingTest TraitInTraitClassTest
    TraitWithMethodsInProtocolsTest
    SlotTraitsTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 4: More collections ==="
  #(LinkedListTest OrderedDictionaryTest
    IdentityDictionaryTest StackTest
    DoubleLinkedListTest ByteArrayTest RunArrayTest
    AssociationTest ReduceTest "CollectionRootTest is abstract"
    WideStringTest "ByteSymbolTest - Pharo OCParser infinite loop bug") do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 5: Streams, files, system ==="
  #(ReadStreamTest WriteStreamTest ReadWriteStreamTest
    GeneratorTest LimitedWriteStreamTest
    RandomTest NumberParserTest NumberParsingTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 6: Extended coverage ==="
  #(SplitJoinTest CollectionArithmeticTest MethodDictionaryTest
    Float32ArrayTest WordArrayTest IntegerArrayTest
    ClassDescriptionProtocolsTest
    SmallDictionaryTest ValueLinkTest
    ChunkReadStreamTest MultiByteBinaryOrTextStreamTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 7: More collections, weak refs, slots ==="
  #(NullStreamTest StreamBugsTest
    KeyedTreeTest NestedDictionaryTest PluggableDictionaryTest
    IdentityBagTest HashTableSizesTest CharacterSetTest WideCharacterSetTest
    NativeArrayTest StringInitializationTest StringLineEndingsTest
    WeakSetTest WeakValueDictionaryTest WeakIdentitySetTest
    WeakKeyDictionaryTest WeakIdentityKeyDictionaryTest
    WeakOrderedCollectionTest WeakIdentityValueDictionaryTest
    FIFOQueueTest LIFOQueueTest
    SlotBasicTest SlotMigrationTest SlotErrorsTest SuperVariableTest
    BenchmarkResultTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Tier 8: Time, hashing, caching, compiler, more kernel/collections"
  #(DateTest TimeTest DateAndTimeTest DurationTest TimespanTest
    WeekTest YearTest MonthTest ScheduleTest StopwatchTest
    MD5Test SHA1Test SHA256Test
    LRUCacheTest TTLCacheTest
    Float64ArrayTest OrderedIdentityDictionaryTest
    SmallIdentityDictionaryTest PluggableSetTest
    CollectionTest WaitfreeQueueTest SharedQueueTest
    SystemVersionTest SmalltalkImageTest VeryDeepCopyTest
    CompiledMethodTest UnicodeTest SizeInMemoryTest
    AdditionalMethodStateTest InstructionStreamTest
    MutexTest WriteBarrierTest ProcessTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Tier 9: Regex, system, text, announcements, class defs, more kernel/slot"
  #(RxMatcherTest RxParserTest RxExtensionsTest
    EventManagerTest FinalizationRegistryTest
    SessionCreationTest SessionErrorHandlingTest
    SessionManagerRegistrationOrderTest SessionManagerRegistrationTest
    SessionManagerUnregistrationTest
    ISOLanguageDefinitionTest LocaleChangedTest LocaleIDTest
    LocaleTest NaturalLanguageTranslatorTest
    KeyboardKeyTest Win32WideStringTest
    AnnouncementSetTest WeakAnnouncerTest AnnouncerTest
    TextTest TextEmphasisTest TextAlignmentTest TextColorTest
    TextFontChangeTest TextFontReferenceTest TextIndentTest
    TextStreamTest TextKernTest FontTest
    ClassTest AsciiCharsetTest CodeSimulationTest InstructionClientTest
    CollectionRootTest
    FluidClassDefinitionPrinterTest LegacyClassDefinitionPrinterTest
    OldClassDefinitionPrinterTest ClassDefinitionPrinterConfigurationTest
    SlotTest SlotIntegrationTest PropertySlotTest
    SlotAnnouncementsTest SlotClassVariableTest
    ClassVariableTest GlobalVariableTest LiteralVariableTest
    SlotLayoutEqualityTest SlotLayoutExtensionTest
    "SlotTraitsTest moved to Tier 3b"
    SelfVariableTest TemporaryVariableTest
    ThisContextVariableTest BooleanSlotTest WriteOnceSlotTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Tier 10: OpalCompiler tests, AST tests, ClassParser tests"
  #(MethodMapTest MethodPragmaTest
    OCAnnotationTest OCArrayLiteralTest
    OCASTAndOrTranslatorTest OCASTBasicTranslatorTest
    OCASTBlockTranslatorTest OCASTCheckerTest
    OCASTClosureAnalyzerTest OCASTDoubleBlockTranslatorTest
    OCASTDoubleBranchConditionalTranslatorTest OCASTLiteralTranslatorTest
    OCASTRepeatTranslatorTest OCASTSemanticAnalyzerTest
    OCASTSingleBlockTranslatorTest OCASTSingleBranchConditionalTranslatorTest
    OCASTSpecialLiteralTranslatorTest OCASTTimesRepeatTranslatorTest
    OCASTToDoTranslatorTest OCASTTranslatorMappingForFullBlockClosuresTest
    OCASTTranslatorTest OCASTVariableTranslatorTest
    OCASTWhileFalseTranslatorTest OCASTWhileTrueTranslatorTest
    OCBytecodeGeneratorTest OCBytecodeToASTCacheTest
    OCClosureCompilerTest OCClosureTest
    OCCodeSnippetScriptingTest OCCompilationContextTest
    OCCompiledMethodIntegrityTest OCCompilerNotifyingTest
    OCCompilerTest OCCompileWithFailureTest
    OCContextIntegrationTest OCContextTempMappingTest
    OCDoitTest OCDoItVariableTest
    OCDynamicASTCompilerPluginTest OCEnvironmentScopeTest
    OCIfNotNilTest OCIRBuilderTest OCIRPrinterTest
    OCIRTransformTest OCIRVisitorTest
    OCLiteralTest OCPerformTest OCScanner2Test
    OCSourceCode2BytecodeTest OCSpecialSelectorTest
    OCStaticASTCompilerPluginTest OCTargetCompilerTest
    OCVariableNodeNameResolutionTest OpalCompilerTest
    ContextDebuggingInfoTest
    "AST tests"
    OCScannerTest OCParserTest OCProgramNodeTest
    OCMethodNodeTest OCMessageNodeTest
    OCBlockNodeTest OCSequenceNodeTest OCVariableNodeTest
    OCCommentNodeTest OCCommentNodeVisitorTest
    OCDumpVisitorTest OCGenericNodeVisitorTest
    OCTypingVisitorTest OCSimpleFormatterTest
    OCParseTreeRewriterTest OCParseTreeSearcherTest
    OCPatternParserTest OCErrorNodeParserTest
    OCEOFTokenTest OCParseErrorNodeTest
    OCEvaluationTest OCCompileCodeSnippetTest
    OCParseCodeSnippetTest OCCodeSnippetTest
    "ClassParser tests"
    CDBehaviorParserTest CDClassDefinitionParserTest
    CDFluidClassParserTest CDTraitParserTest
    CDExistingClassDefinitionTest CDExistingClassSideDefinitionTest
    OCClassBuilderTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Tier 11: ClassParser, Ring, Traits, ClassAnnotation, more kernel"
  #(CDCompiledBlockClassParserTest CDCompiledMethodClassParserTest
    CDNormalClassParserTest CDVariableClassParserTest
    CDVariableByteClassParserTest CDVariableWordClassParserTest
    CDDoubleByteClassParserTest CDDoubleWordClassParserTest
    CDEphemeronClassParserTest CDImmediateClassParserTest
    CDMetaclassWithTraitParserTest CDNilSubclassParserTest
    CDWeakClassParserTest CDPointClassParserTest
    CDNormalClassCategoryParserTest CDNormalMetaclassParserTest
    CDClassWithPoolDictionaryParserTest CDClassWithTraitParserTest
    CDClassWithTraitAliasParserTest CDClassWithTraitExclusionParserTest
    CDClassWithTraitCompositionSequenceParserTest
    CDTraitCompositionClassParserTest
    "Ring metadata tests"
    RGClassDefinitionTest RGCommentDefinitionTest
    RGElementDefinitionTest RGMethodDefinitionTest
    RGVariableDefinitionTest RGPackageDefinitionTest
    RGMetaclassDefinitionTest RGMetatraitDefinitionTest
    RGTraitDefinitionTest RGObjectTest RGSlotTest
    RGStampParserTest RGUnresolvedValueTest
    RGClassVariableTest RGGlobalVariableTest RGPoolVariableTest
    RGCommentTest RGLayoutDefinitionTest
    RGTraitAliasTest RGTraitCompositionTest RGTraitExclusionTest
    RGBehaviorTest RGClassDescripitonStrategyTest
    RGClassStrategyTest RGMetaclassStrategyTest
    RGMetaclassTraitStrategyTest RGMetaclassTraitTest
    RGTraitStrategyTest RGTraitTest RGClassTest
    RGEnvironmentTest RGBackendTest RGAnnouncementsTest
    RGEnsureTraitTest RGEnvironmentBackendTest
    RGEnvironmentQueryInterfaceTest RGReadOnlyImageBackendTest
    "Trait tests moved to Tier 3b"
    "ClassAnnotation tests"
    ClassAnnotationTest ActiveClassAnnotationsTest
    QueryAnnotationsFromClassTest RegisteredClassAnnotationsTest
    CompositeAnnotationContextTest SimpleAnnotationContextTest
    VisibleClassAnnotationsTest
    "More kernel/compiler"
    Base64Test Base64MimeConverterTest
    ChainedSortFunctionTest ChunkImportTestCase
    ClassQueryTest ClassHierarchyPrinterTest
    MethodQueryTest MethodConstantTest
    DateAndTimeDosEpochTest DateAndTimeEpochTest
    DateAndTimeLeapTest DateAndTimeUnixEpochTest DateParsingTest
    PointArrayTest ArgumentNamesTest
    AbstractClassDescriptionTest ClassAnnouncementsTest
    MethodAnnouncementsTest ProtocolAnnouncementsTest
    OCScopeTest OCCompilerSyntaxErrorNotifyingTest
    OCIsEssentialTest
    BehaviorWithCompilerTest BlockClosuresTestCase
    ObjectFinalizerTest CollectionValueHolderTest
    HashTesterTest ProtocolConventionsTest
    ClassFactoryForTestCaseTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Tier 12: Graph algorithms, slots, continuations, compiler, system"
  #("AI Graph algorithms (pure computation)"
    AIAstarTest AIBellmanFordTest AIBFSTest AIDijkstraTest
    AIDinicTest AIGraphAlgorithmTest AIGraphReducerTest AIHitsTest
    AIKruskalTest AILongestPathInDAGTest AILongestPathInDCGTest
    AIPrimTest AIShortestPathInDAGTest AITarjanTest
    AITopologicalSortingTest AIWeightedHitsTest
    "Slot variants"
    AccessorInstanceVariableSlotTest ComputedSlotTest
    ProcessLocalSlotTest WeakClassVariableTest WeakSlotTest
    ArgumentVariableTest
    "Block/Continuation"
    BlockClosureValueWithinTest BlockClosureValueWithinDurationTest
    ContinuationTest
    "Compiler/definitions"
    ByteSymbolTest CommonClassDefinitionPrinterTest
    RuntimeSyntaxErrorTest MethodClassifierTest
    SortFunctionTest
    "System/misc"
    CommandLineArgumentsTest StackSignalTest
    UndefinedClassTest UndefinedPackageTagTest UndefinedPackageTest
    CoverageCollectorTest CoverageDemoTest
    CharacterKeyCombinationTest
    "More compiler tests"
    OCCacheResetTest OCCodeReparatorTest
    OCNewCompilerWithChangesFunctionalTest
    OCReadBeforeWrittenTesterTest OCReturnNodeAdderVisitorTest
    "Ring additional"
    RGClassDescriptionTest RGMethodTest RGPackageTest RGTest
    "Package tests"
    PackageTest PackageTagTest PackageOrganizerTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Tier 13: Formatters, diff, text, slots, visitors, timespan, misc"
  #("EqualityFormatter tests (code formatting)"
    EFArrayExpressionTest EFAssignmentExpressionTest
    EFBlockExpressionOnlyTest EFBlockExpressionTest
    EFCascadeExpressionTest EFContextTest
    EFFormatterTest EFInternalTest
    EFLiteralArrayExpressionTest EFLiteralValueExpressionTest
    EFMessageExpressionTest EFMethodExpressionTest
    EFParseErrorExpressionTest EFPatternBlockExpressionTest
    EFPragmaExpressionTest EFReturnExpressionTest
    EFSequenceExpressionTest EFTemporariesExpressionTest
    EFTest EFVariableExpressionTest
    "Diff algorithm tests"
    DiffBuilderTest DiffPatchTest TextDiffBuilderTest
    "AST visitors"
    CollectVisitorTest CopyVisitorTest DeleteVisitorTest
    "Text additional"
    TextAnchorTest TextLineTest TextLineEndingsTest
    "Timespan additional"
    TimespanDoTest TimespanDoSpanAYearTest
    "Slot additional"
    SlotClassBuilderTest SlotEnvironmentTest
    SlotExampleMovieAndPersonTest SlotSilentTest
    "Misc kernel/compiler"
    DosTimestampTest ConfigurableHistoryIteratorTest
    DynamicMessageImplementorTest
    CommentTestCase CommentsToTestsTest
    ContextDebuggingTest
    ClassFactoryWithNonDefaultEnvironmentTest
    "Ring/Package additional"
    RGReadOnlyBackendTest RGMCTest RGMCClassTest
    RGMCPackageTest RGMCTraitCompositionVisitorTest
    PackageAnnouncementsTest PackageOnModelTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Tier 14: FFI tests, decompiler, equivalence, examples, misc"
  #("FFI tests (validate FFI implementation)"
    FFIArchitectureTest FFICalloutAPITest FFICalloutMethodBuilderTest
    FFICalloutTest FFICompilerPluginTest FFIConstantHandleTest
    FFIExternalArrayTest FFIExternalEnumerationTest
    FFIExternalPackedStructureTest FFIExternalStructureFieldParserTest
    FFIExternalStructurePlatformTest FFIExternalStructureTest
    FFIExternalUnionTest FFIExternalValueHolderTest
    FFIFunctionParserTest FFIFunctionResolutionTest
    FFIOpaqueObjectTest FFIStringCalloutTest
    FFITypeArrayTest FFITypesTest
    FFICallbackParametersTest FFICallbackTest
    FFIAutoReleaseOptionCalloutTest
    "Bytecode decompiler"
    FBDBytecodeDecompilerExamplesTest FBDDecompilerTest
    FBIRBytecodeDecompilerTest
    "Equivalence/comparison tests"
    EqFruitComparisonTest EqPersonComparisonTest EquivalentTreeTest
    "Example/demo tests"
    ExampleClassVariableTest ExampleSetTest
    ExampleSlotWithFluidAPITest ExampleSlotWithStateTest
    "Bit operations"
    BitRegisterTest
    "Misc safe"
    BreadthFirstGuideTest BuilderManifestTest
    ExecutionCounterTest SlotMethodRecompilationTest
    SortFunctionTest NullWorldRendererTest
    FloatReferencesRuleTest ProcessMonitorTestServiceTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Tier 15: SUnit self-tests, GlobalIdentifier, trait printers, system, commands"
  #("SUnit self-tests"
    TestAsserterTest TestCaseTest TestFailureTestCase
    TestExecutionEnvironmentTest
    "GlobalIdentifier tests"
    GlobalIdentifierMergerTest
    GlobalIdentifierWithDefaultConfigurationTest
    "System tests"
    SystemNavigationTest SystemEnvironmentTest
    SystemBuildInfoTest SystemResolverTest
    "Command line tests"
    CommandLineHandlerTest CommandLinePasswordManagerTest
    "Trait definition printers (read-only, no class modification)"
    TraitCommonClassDefinitionPrinterTest
    TraitFluidClassDefinitionPrinterTest
    TraitLegacyPharoClassDefinitionPrinterTest
    TraitOldPharoClassDefinitionPrinterTest
    "Trait slot tests"
    TraitSlotScopeTest TraitWithSlotsTest
    TraitWithComplexSlotsTest
    "Package tests"
    PackageAndClassesTest PackageAndMethodsTest
    "Misc"
    DocCommentsReleaseTest PharoDocCommentTest
    ObjectWithPrintingRaisingHaltTest
    EFFFICallTest BitBltTest BitBltTranslucentTest
    FastStepThroughTest StepThroughTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Tier 16: Geometry, fuzzy matching, history, commander, misc"
  #("Geometry (pure computation)"
    GAngleTest GArcTest GCircleTest GCoordinatesTest GEllipseTest
    GLineTest GMatrixTest GPointTest GPolygonTest GRayTest
    GRectangleTest GSegmentTest GTriangleTest GVectorTest
    "Compression"
    GZipReadStreamTest
    "Fuzzy matching"
    FuzzyMatcherTest
    "History"
    HistoryIteratorTest HistoryNodeTest HistorySlotTest
    "Commander"
    CmCommandDecoratorTest CmCommandsGroupTest CmCommandTest
    "Epicea (change logging)"
    EpFilterTest EpLogTest EpCommentTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Dynamic discovery: add ALL remaining TestCase subclasses ==="
  "After the hand-curated tiers above, discover every non-abstract TestCase
   subclass in the image and add any we haven't already included. This ensures
   new tests added to the Pharo image are automatically picked up.
   Skip list: classes known to hang, require UI, or have infrastructure issues."
  [| skipNames allTC |
   skipNames := #(
     "UI/Morphic tests that hang in headless mode"
     ClyBrowserToolValidityTest ClyDataSourcePaginationTest
     ClyMethodCalledFromBrowserTest ClyMethodCalledFromMessageBrowserTest
     MorphTest MorphicEventHandlerTest MorphicUIManagerTest
     TextMorphTest SystemWindowTest PluggableTextMorphTest
     WorldMorphTest DisplayScreenTest HandMorphTest PasteUpMorphTest
     SpAbstractWidgetPresenterTest SpApplicationTest
     "Spec2 backend tests (need Morphic/display)"
     SpBoxLayoutTest SpGridLayoutTest SpDropListPresenterTest
     SpListPresenterTest SpTablePresenterTest SpTreeTablePresenterTest
     SpButtonPresenterTest SpTextPresenterTest SpTextInputFieldPresenterTest
     SpWindowPresenterTest SpSliderPresenterTest SpRadioButtonPresenterTest
     SpCheckBoxPresenterTest SpTabPresenterTest SpProgressBarPresenterTest
     SpToolbarPresenterTest SpMenuPresenterTest SpNotebookPresenterTest
   ) asSet.
   allTC := (TestCase allSubclasses reject: [:c |
     [c isAbstract] on: Error do: [:e | true]])
     asSortedCollection: [:a :b | a name <= b name].
   allTC do: [:cls |
     (skipNames includes: cls name) ifFalse: [
       addOnce value: cls]]]
  on: Error do: [:e |].

  "Read batch range from /tmp/sunit_batch.txt (format: 'startIndex endIndex')
   OR class names from /tmp/sunit_class_names.txt (one class name per line).
   If class names file exists, it takes priority. If neither, run all classes."
  batchStart := 1.
  batchEnd := testClasses size.
  [| classNamesFile |
   classNamesFile := '/tmp/sunit_class_names.txt' asFileReference.
   classNamesFile exists ifTrue: [
     | names selected |
     names := classNamesFile contents lines
       collect: [:l | l trimBoth]
       thenSelect: [:l | l notEmpty].
     selected := OrderedCollection new.
     names do: [:name |
       testClasses detect: [:c | c name = name] ifFound: [:c | selected add: c]].
     batchClasses := selected.
     batchStart := 1.
     batchEnd := selected size]
   ifFalse: [
     [| batchFile parts |
      batchFile := '/tmp/sunit_batch.txt' asFileReference.
      batchFile exists ifTrue: [
        parts := batchFile contents trimBoth substrings.
        batchStart := parts first asInteger.
        batchEnd := parts second asInteger min: testClasses size]]
     on: Error do: [:e | ]]]
  on: Error do: [:e | ].
  batchClasses ifNil: [batchClasses := testClasses copyFrom: batchStart to: batchEnd].

  "Open output files — both overwritten per batch.
   Shell script concatenates detail files across batches."
  '/tmp/sunit_test_results.txt' asFileReference ensureDelete.
  stream := '/tmp/sunit_test_results.txt' asFileReference writeStream.
  '/tmp/sunit_test_detail.txt' asFileReference ensureDelete.
  detail := '/tmp/sunit_test_detail.txt' asFileReference writeStream.
  totalP := 0. totalF := 0. totalE := 0. totalS := 0. totalT := 0.

  [
    stream nextPutAll: '=== SUnit Test Run #'; print: runNum;
      nextPutAll: ' (batch '; print: batchStart;
      nextPutAll: '-'; print: batchEnd;
      nextPutAll: ' of '; print: testClasses size;
      nextPutAll: ') ==='; cr; cr.
    self runDiagnostic: stream.
    "Check symbol table for Symbol class corruption"
    [| symCls count |
     symCls := Symbol.
     count := 0.
     (Symbol classVariableNamed: #SymbolTable) read do: [:each |
       each == symCls ifTrue: [count := count + 1]].
     (Symbol classVariableNamed: #NewSymbols) read do: [:each |
       each == symCls ifTrue: [count := count + 1]].
     stream nextPutAll: 'SYMTABLE-CHECK: Symbol class found '; print: count;
       nextPutAll: ' times in symbol tables'; cr. stream flush.
     count > 0 ifTrue: [
       stream nextPutAll: 'WARNING: Symbol class IS in symbol table - this causes corruption!!'; cr.
       stream flush]]
    on: Error do: [:e |
     stream nextPutAll: 'SYMTABLE-CHECK error: '; nextPutAll: e messageText; cr. stream flush].
    "Patch test timeouts for non-JIT VM. Read scale from /tmp/sunit_timeout_scale.txt
     (default 10). Override TestCase>>defaultTimeLimit and remove per-class overrides
     so all tests inherit a generous scaled timeout instead of the 10s JIT default."
    timeoutScale := 10.
    [| f |
     f := '/tmp/sunit_timeout_scale.txt' asFileReference.
     f exists ifTrue: [
       | v | v := f contents trimBoth asInteger.
       v > 0 ifTrue: [timeoutScale := v]]]
    on: Error do: [:e |].
    watchdogTimeout := 10 * timeoutScale + 30. "base timeout + 30s buffer"
    [| patched |
     TestCase compile: ('defaultTimeLimit
  ^ {1} seconds' format: {(10 * timeoutScale) printString})
       classified: 'running'.
     patched := 0.
     TestCase allSubclasses do: [:cls |
       (cls includesSelector: #defaultTimeLimit) ifTrue: [
         cls removeSelector: #defaultTimeLimit. patched := patched + 1]].
     stream nextPutAll: 'Timeout: scale='; print: timeoutScale;
       nextPutAll: 'x, defaultTimeLimit='; print: 10 * timeoutScale;
       nextPutAll: 's, watchdog='; print: watchdogTimeout;
       nextPutAll: 's, '; print: patched; nextPutAll: ' class overrides removed'; cr.
     stream flush]
    on: Error do: [:e |
     stream nextPutAll: 'Timeout patch error: '; nextPutAll: e messageText; cr. stream flush].
    "Wrap in TestExecutionEnvironment so tests that need processMonitor etc. work.
     DefaultExecutionEnvironment>>runTestsBy: activates TestExecutionEnvironment."
    CurrentExecutionEnvironment runTestsBy: [
    batchClasses do: [:tc | | counts |
      [counts := self runTestClass: tc run: runNum on: stream detail: detail timeout: watchdogTimeout.
      totalP := totalP + (counts at: 1).
      totalF := totalF + (counts at: 2).
      totalE := totalE + (counts at: 3).
      totalS := totalS + (counts at: 4).
      totalT := totalT + (counts at: 5)]
        on: Error do: [:ex |
          "TestTookTooMuchTime from TestExecutionEnvironment watchdog — count as timeout, don't abort class"
          ex class name = 'TestTookTooMuchTime'
            ifTrue: [
              totalT := totalT + 1.
              stream nextPutAll: '  TIMEOUT(env-class): '; nextPutAll: tc name; cr.
              "Fix up env state so next test works"
              [| env wds |
               env := CurrentExecutionEnvironment value.
               env instVarNamed: 'testCompleted' put: true.
               wds := env instVarNamed: 'watchDogSemaphore'.
               wds ifNotNil: [wds signal]]
              on: Error do: [:e2 |]]
            ifFalse: [
              stream nextPutAll: '*** RUNNER ERROR in '; nextPutAll: tc name;
                nextPutAll: ': '; nextPutAll: ex class name;
                nextPutAll: ' - '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 200)); cr]].
      stream flush. detail flush]].
    stream nextPutAll: '*** BATCH COMPLETED for '; print: batchClasses size;
      nextPutAll: ' classes ('; print: batchStart; nextPutAll: '-'; print: batchEnd; nextPutAll: ')'; cr.
    stream flush.
    stream cr.
    stream nextPutAll: '=== BATCH TOTAL ==='; cr.
    stream nextPutAll: 'Run: #'; print: runNum; cr.
    stream nextPutAll: 'Batch: '; print: batchStart; nextPutAll: '-'; print: batchEnd; cr.
    stream nextPutAll: 'Classes: '; print: batchClasses size; cr.
    stream nextPutAll: 'Pass: '; print: totalP; cr.
    stream nextPutAll: 'Fail: '; print: totalF; cr.
    stream nextPutAll: 'Error: '; print: totalE; cr.
    stream nextPutAll: 'Skip: '; print: totalS; cr.
    totalT > 0 ifTrue: [stream nextPutAll: 'Timeout: '; print: totalT; cr].
    stream nextPutAll: 'Total: '; print: totalP + totalF + totalE + totalS + totalT; cr.
    stream cr.
    stream nextPutAll: '=== BATCH COMPLETE ==='; cr.
    stream flush]
  ensure: [stream close. detail close].
  "exitSuccess goes through SessionManager shutdown which may not reach primitiveQuit.
   Call quitPrimitive directly to ensure the VM terminates."
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!SUnitRunner class methodsFor: 'running'!
runTestClass: testClass run: runNum on: s detail: d timeout: watchdogSecs
  "Run all tests in a class. Returns #(pass fail error skip timeout)."
  | sels p f e sk t testPriority consecutiveTimeouts |

  "Skip genuinely abstract classes — use isAbstract instead of manual list"
  ([testClass isAbstract] on: Error do: [:e | false]) ifTrue: [
    s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' === SKIPPED (abstract)'; cr. s flush.
    ^ {0. 0. 0. 0. 0}].

  s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' ==='; cr.
  s flush.
  "Check if Delay scheduler is alive before this class"
  [| sem alive startMs |
   alive := false.
   sem := Semaphore new.
   [(Delay forMilliseconds: 50) wait. sem signal] fork.
   "Spin-wait using wall-clock time (not yield count).
    Processor yield is a no-op when no other process is at our priority,
    so we must check real elapsed time. 5 second timeout."
   startMs := Time millisecondClockValue.
   [alive not and: [((Time millisecondClockValue - startMs) bitAnd: 16r1FFFFFFF) < 5000]]
     whileTrue: [sem isSignaled ifTrue: [alive := true]. Processor yield].
   alive ifFalse: [sem isSignaled ifTrue: [alive := true]].
   alive ifFalse: [
     s nextPutAll: '  TIMEOUT:DELAY-DEAD-BEFORE: '; nextPutAll: testClass name; cr.
     [| proc ctx |
      proc := Delay scheduler schedulingProcess.
      s nextPutAll: '  DELAY-DIAG: proc='.
      proc ifNil: [s nextPutAll: 'nil']
        ifNotNil: [
          s nextPutAll: proc class name; nextPutAll: ' pri='; print: proc priority;
            nextPutAll: ' suspended='.
          ctx := proc suspendedContext.
          ctx ifNil: [s nextPutAll: 'nil (ACTIVE or DEAD)']
            ifNotNil: [s nextPutAll: ctx printString].
          s nextPutAll: ' isTerminated='; nextPutAll: proc isTerminated printString;
            nextPutAll: ' isSuspended='; nextPutAll: proc isSuspended printString].
      s cr. s flush] on: Error do: [:e |
        s nextPutAll: '  DELAY-DIAG-ERROR: '; nextPutAll: e printString; cr. s flush]]]
  on: Error do: [:e |].
  "Tests must run below timingPriority (80). Watchdog uses spin-wait
   (relinquishProcessorForMicroseconds:) so it works even if Delay scheduler dies.
   Use priority 40 (userSchedulingPriority) matching standard Pharo behavior.
   This is critical because some tests do forkAt: activeProcess priority + N
   and priority 79+20=99 would exceed Processor highestPriority (80).
   Weak/finalization tests MUST run below mourn priority (51), also at 40."
  testPriority := 40.
  p := 0. f := 0. e := 0. sk := 0. t := 0. consecutiveTimeouts := 0.
  sels := testClass allTestSelectors asArray.
  "Sort with comparison counter to detect infinite loops"
  [| count |
   count := 0.
   sels := sels sort: [:a :b |
     count := count + 1.
     count > 200000 ifTrue: [
       s nextPutAll: '  SORT-ABORT: '; print: count; nextPutAll: ' comparisons exceeded limit'; cr. s flush.
       Error signal: 'Sort infinite loop detected'].
     a <= b].
   s nextPutAll: '  SORT-OK: '; print: count; nextPutAll: ' comparisons for '; print: sels size; nextPutAll: ' items'; cr. s flush]
  on: Error do: [:ex |
    s nextPutAll: '  SORT-ERROR: '; nextPutAll: ex class name; nextPutAll: ' - '; nextPutAll: ex messageText; cr. s flush.
    sels := testClass allTestSelectors asArray "use unsorted on error"].

  sels do: [:sel |
    "Minimal skips: 1GB allocations (OOM) and inherited ClassTestCase lint tests
     that run AST-parsing lint rules on every method in the class hierarchy.
     These timeout on every test class (~640) at 10s each = 100+ min of dead waiting.
     They're static analysis checks, not VM correctness tests."
    (#(testOneGBAllocation testOneGWordAllocation
       testMethodsOfTheClassShouldNotBeRepeatedInItsSuperclasses
       testTraitExplicitRequirementMethodsMustBeImplementedInTheClassOrInASuperclass
       "IntegerTest: O(n^2) LargeInteger division/printing vs reference VM's optimized C.
        These take minutes even with scaled timeouts. When they hang at P40, the runner
        (also P40) can't preempt to fire the watchdog, so they must be skipped."
       testPrintStringBase testPositiveIntegerPrinting testNegativeIntegerPrinting
       testSlowFactorial testReciprocalModulo testPrintStringAll
       "testTransformingDeprecation hangs the VM: Deprecation>>transform triggers
        code rewriting that creates an infinite loop or high-priority process, starving
        the P40 watchdog. The watchdog never fires even after 10+ minutes."
       testTransformingDeprecation)
      includes: sel) ifFalse: [
      | result |
      s nextPutAll: '  RUN: '; nextPutAll: sel; cr. s flush.
      result := self runSingleTest: testClass selector: sel timeout: watchdogSecs priority: testPriority on: s.
      result ifNil: [
        p := p + 1. consecutiveTimeouts := 0.
        d nextPutAll: runNum printString; nextPut: $	.
        d nextPutAll: testClass name; nextPut: $	.
        d nextPutAll: sel; nextPut: $	.
        d nextPutAll: 'PASS'; cr. d flush]
      ifNotNil: [:kind |
        kind = #timeout
          ifTrue: [consecutiveTimeouts := consecutiveTimeouts + 1]
          ifFalse: [consecutiveTimeouts := 0].
        kind = #fail ifTrue: [
          f := f + 1.
          d nextPutAll: runNum printString; nextPut: $	.
          d nextPutAll: testClass name; nextPut: $	.
          d nextPutAll: sel; nextPut: $	.
          d nextPutAll: 'FAIL'; cr].
        kind = #skip ifTrue: [
          sk := sk + 1.
          d nextPutAll: runNum printString; nextPut: $	.
          d nextPutAll: testClass name; nextPut: $	.
          d nextPutAll: sel; nextPut: $	.
          d nextPutAll: 'SKIP'; cr].
        kind = #error ifTrue: [
          e := e + 1.
          d nextPutAll: runNum printString; nextPut: $	.
          d nextPutAll: testClass name; nextPut: $	.
          d nextPutAll: sel; nextPut: $	.
          d nextPutAll: 'ERROR'; cr].
        kind = #timeout ifTrue: [
          t := t + 1.
          d nextPutAll: runNum printString; nextPut: $	.
          d nextPutAll: testClass name; nextPut: $	.
          d nextPutAll: sel; nextPut: $	.
          d nextPutAll: 'TIMEOUT'; cr].
        d flush.
        "Bail out of class after 5 consecutive timeouts to avoid wasting time"
        consecutiveTimeouts >= 5 ifTrue: [
          s nextPutAll: '  BAIL: 5 consecutive timeouts, skipping rest of class'; cr.
          s nextPutAll: 'Total: '; print: sels size;
            nextPutAll: ' P:'; print: p; nextPutAll: ' F:'; print: f;
            nextPutAll: ' E:'; print: e; nextPutAll: ' S:'; print: sk;
            nextPutAll: ' T:'; print: t; cr; cr. s flush.
          ^ {p. f. e. sk. t}]]]].

  "Check if Delay scheduler is alive after this class"
  [| sem alive startMs |
   alive := false.
   sem := Semaphore new.
   [(Delay forMilliseconds: 50) wait. sem signal] fork.
   "Spin-wait using wall-clock time — same fix as BEFORE check"
   startMs := Time millisecondClockValue.
   [alive not and: [((Time millisecondClockValue - startMs) bitAnd: 16r1FFFFFFF) < 5000]]
     whileTrue: [sem isSignaled ifTrue: [alive := true]. Processor yield].
   alive ifFalse: [sem isSignaled ifTrue: [alive := true]].
   alive ifFalse: [
     s nextPutAll: 'DELAY-DEAD-AFTER: '; nextPutAll: testClass name; cr.
     [| proc ctx |
      proc := Delay scheduler schedulingProcess.
      s nextPutAll: '  DELAY-DIAG: proc='.
      proc ifNil: [s nextPutAll: 'nil']
        ifNotNil: [
          s nextPutAll: proc class name; nextPutAll: ' pri='; print: proc priority;
            nextPutAll: ' suspended='.
          ctx := proc suspendedContext.
          ctx ifNil: [s nextPutAll: 'nil (ACTIVE or DEAD)']
            ifNotNil: [s nextPutAll: ctx printString].
          s nextPutAll: ' isTerminated='; nextPutAll: proc isTerminated printString;
            nextPutAll: ' isSuspended='; nextPutAll: proc isSuspended printString].
      s cr. s flush] on: Error do: [:e |
        s nextPutAll: '  DELAY-DIAG-ERROR: '; nextPutAll: e printString; cr. s flush]]]
  on: Error do: [:e |].
  s nextPutAll: 'Total: '; print: sels size;
    nextPutAll: ' P:'; print: p;
    nextPutAll: ' F:'; print: f;
    nextPutAll: ' E:'; print: e;
    nextPutAll: ' S:'; print: sk.
  t > 0 ifTrue: [s nextPutAll: ' T:'; print: t].
  s cr; cr.
  ^ {p. f. e. sk. t}! !

!SUnitRunner class methodsFor: 'running'!
runSingleTest: testClass selector: sel timeout: seconds priority: priority on: s
  "Run a single test with spin-wait watchdog that does NOT depend on Delay scheduler.
   Fork the test at the requested priority. The test signals doneSem when done,
   waking the runner immediately. The watchdog at priority+1 uses
   relinquishProcessorForMicroseconds: to yield, checking elapsed time each cycle.
   The heartbeat forceYield (1s) preempts the test back to the watchdog.
   Timeout: 30s primary, 60s backup."
  | result testProcess doneSem watchdog backupWatchdog testDone |
  result := nil. "nil means pass"
  testDone := false.
  doneSem := Semaphore new.

  "Fork test at requested priority"
  testProcess := [
    | testInstance |
    testInstance := testClass selector: sel.
    "Set testCase on TestExecutionEnvironment so EpMonitor can query shouldLogWithEpicea"
    [CurrentExecutionEnvironment value instVarNamed: 'testCase' put: testInstance] on: Error do: [:e2 |].
    [[[[[testInstance runCase]
      on: TestFailure do: [:ex |
        result := #fail.
        s nextPutAll: '  FAIL: '; nextPutAll: sel;
          nextPutAll: ' ['; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 500)); nextPutAll: ']'; cr]]
      on: Deprecation do: [:ex |
        [ex shouldTransform ifTrue: [ex transform]] on: Error do: [:e | ].
        ex resume]]
      on: TestSkipped do: [:ex |
        result := #skip.
        s nextPutAll: '  SKIP: '; nextPutAll: sel;
          nextPutAll: ' ('; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 100)); nextPutAll: ')'; cr]]
      on: Error do: [:ex |
        result := #error.
        s nextPutAll: '  ERROR: '; nextPutAll: sel;
          nextPutAll: ' ('; nextPutAll: ex class name;
          nextPutAll: ': '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 100)); nextPutAll: ')'; cr.
        "Capture stack trace for diagnosis"
        [| ctx count |
         ctx := ex signalerContext.
         count := 0.
         [ctx notNil and: [count < 25]] whileTrue: [
           s nextPutAll: '    >> '; nextPutAll: ctx printString; cr.
           ctx := ctx sender. count := count + 1].
         s flush] on: Error do: [:e2 | "ignore trace errors"]]]
    "ensure: guarantees doneSem signal even if tearDown throws a second
     error during unwinding that escapes the on: Error handler."
    ensure: [testDone := true. doneSem signal]] forkAt: priority.

  "Two-tier watchdog:
   1. P79 Delay-based (efficient, fires when Delay scheduler works)
   2. P40 spin-wait backup (fires when Delay scheduler is dead/hung, 2x timeout)
   Whichever fires first signals doneSem."
  watchdog := [
    (Delay forMilliseconds: seconds * 1000) wait.
    "Timeout elapsed — check if test already finished (testDone flag)."
    testDone ifFalse: [
      result := #timeout.
      testDone := true.
      s nextPutAll: '  TIMEOUT: '; nextPutAll: sel;
        nextPutAll: ' (exceeded '; print: seconds; nextPutAll: 's)'; cr.
      s flush.
      doneSem signal.
      "Terminate after signaling — best effort cleanup"
      [testProcess terminate] on: Error do: [:e |]]] forkAt: 79.
  "Backup: spin-wait at LOW priority (P10) using Processor yield.
   2x timeout gives primary watchdog time to fire first.
   Uses testDone flag (not result isNil) — result=nil means 'pass', not 'still running'.
   MUST be below userBackgroundPriority (30) so it never starves test-forked
   processes (MonitorTest forks at P30, SemaphoreTest at activePriority-1=P39)."
  backupWatchdog := [| startMs |
   startMs := Time millisecondClockValue.
   [testDone not and: [
     ((Time millisecondClockValue - startMs) bitAnd: 16r1FFFFFFF) < (seconds * 2000)]]
       whileTrue: [Processor yield].
   testDone ifFalse: [
     result := #timeout.
     testDone := true.
     s nextPutAll: '  TIMEOUT(backup): '; nextPutAll: sel;
       nextPutAll: ' (exceeded '; print: seconds * 2; nextPutAll: 's)'; cr.
     s flush.
     doneSem signal.
     [testProcess terminate] on: Error do: [:e |]]] forkAt: 10.

  "Wait for test completion or watchdog timeout.
   Catch TestTookTooMuchTime — the TestExecutionEnvironment watchdog at P79 may
   signal this into our runner process (mainTestProcess) if testCompleted stays false.
   This happens when our 15s watchdog terminates the test process but ensure: blocks
   in runTestCase: don't fire, leaving testCompleted=false."
  [doneSem wait] on: Error do: [:ex |
    ex class name = 'TestTookTooMuchTime'
      ifTrue: [
        result ifNil: [result := #timeout].
        s nextPutAll: '  TIMEOUT(env-watchdog): '; nextPutAll: sel; cr. s flush.
        "Terminate the test process if still running"
        [testProcess terminate] on: Error do: [:e2 |]]
      ifFalse: [ex pass]].

  "Clean up watchdogs if test completed before timeout"
  [watchdog suspend] on: Error do: [:e |].
  [backupWatchdog suspend] on: Error do: [:e |].

  "Fix up TestExecutionEnvironment state: if our watchdog killed the test process,
   the ensure: block in runTestCase: may not have fired, leaving testCompleted=false.
   The env watchdog at P79 is blocked in waitTimeoutMilliseconds: — it won't check
   testCompleted until signaled or timed out. Signal only for timeouts."
  result == #timeout ifTrue: [
    [| env wds |
     env := CurrentExecutionEnvironment value.
     env instVarNamed: 'testCompleted' put: true.
     wds := env instVarNamed: 'watchDogSemaphore'.
     wds ifNotNil: [wds signal]]
    on: Error do: [:e |]].

  ^ result! !

!SUnitRunner class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self runAllTests! !

!SUnitRunner class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup - use user category at priority 90"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SUnitRunner)
  inCategory: SessionManager default userCategory
  atPriority: 90!
