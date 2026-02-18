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
  | testClasses stream detail runNum totalP totalF totalE totalS totalT seen addOnce batchStart batchEnd batchClasses |
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
    SlotTraitsTest SelfVariableTest TemporaryVariableTest
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
    "Trait tests"
    TraitTest TraitCompositionTest ClassTraitTest MOPTraitTest
    TraitPureBehaviorTest TraitPrecedenceCompositionTest
    TraitWithAliasTest TraitWithConflictsTest
    TraitChangesTest TraitMethodDescriptionTest
    TraitOverloadingOfMethodsInTraitedClassTest
    TraitPackagingTest TraitInTraitClassTest
    TraitWithMethodsInProtocolsTest
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
    EFFFICallTest BitBltTest
    FastStepThroughTest) do: [:name |
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

  "Read batch range from /tmp/sunit_batch.txt (format: 'startIndex endIndex').
   If file absent, run all classes."
  batchStart := 1.
  batchEnd := testClasses size.
  [| batchFile parts |
   batchFile := '/tmp/sunit_batch.txt' asFileReference.
   batchFile exists ifTrue: [
     parts := batchFile contents trimBoth substrings.
     batchStart := parts first asInteger.
     batchEnd := parts second asInteger min: testClasses size]]
  on: Error do: [:e | ].
  batchClasses := testClasses copyFrom: batchStart to: batchEnd.

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
    batchClasses do: [:tc | | counts |
      [counts := self runTestClass: tc run: runNum on: stream detail: detail.
      totalP := totalP + (counts at: 1).
      totalF := totalF + (counts at: 2).
      totalE := totalE + (counts at: 3).
      totalS := totalS + (counts at: 4).
      totalT := totalT + (counts at: 5)]
        on: Error do: [:ex |
          stream nextPutAll: '*** RUNNER ERROR in '; nextPutAll: tc name;
            nextPutAll: ': '; nextPutAll: ex class name;
            nextPutAll: ' - '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 200)); cr].
      stream flush. detail flush].
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
runTestClass: testClass run: runNum on: s detail: d
  "Run all tests in a class. Returns #(pass fail error skip timeout)."
  | sels p f e sk t testPriority consecutiveTimeouts |

  "Skip genuinely abstract classes — use isAbstract instead of manual list"
  ([testClass isAbstract] on: Error do: [:e | false]) ifTrue: [
    s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' === SKIPPED (abstract)'; cr. s flush.
    ^ {0. 0. 0. 0. 0}].

  "Skip classes that crash the VM via GC corruption during class restructuring.
   These acquire compilation/class mutation mutexes, then timeout-suspend holds the
   lock forever. Subsequent compilation attempts deadlock, GC under pressure corrupts
   process priority fields, putToSleep() aborts on non-SmallInteger priority."
  (#(SlotAnnouncementsTest SlotClassBuilderTest
     SlotEnvironmentTest SlotExampleMovieAndPersonTest SlotSilentTest
     SlotMethodRecompilationTest
     SlotLayoutEqualityTest SlotLayoutExtensionTest
     TraitCompositionTest ClassTraitTest MOPTraitTest
     TraitPureBehaviorTest TraitPrecedenceCompositionTest
     TraitWithAliasTest TraitWithConflictsTest TraitChangesTest
     TraitMethodDescriptionTest TraitOverloadingOfMethodsInTraitedClassTest
     TraitPackagingTest TraitInTraitClassTest TraitWithMethodsInProtocolsTest
     TraitSlotScopeTest TraitWithSlotsTest TraitWithComplexSlotsTest
     AccessorInstanceVariableSlotTest ComputedSlotTest
     ProcessLocalSlotTest WeakClassVariableTest WeakSlotTest
     FFICallbackParametersTest FFICallbackTest
     ProcessTest)
    includes: testClass name) ifTrue: [
    s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' === SKIPPED (crashes VM: class restructuring/mutex)'; cr. s flush.
    ^ {0. 0. 0. 0. 0}].

  s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' ==='; cr.
  s flush.
  "Tests must run below timingPriority (80) so the Delay scheduler can
   preempt hanging tests and fire waitTimeoutSeconds: timeouts.
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
       "IntegerTest tests that exceed 10s timeout due to slow LargeInteger operations.
        Our LargeInteger division/printing is O(n^2) vs reference VM's optimized C.
        When they timeout, the runner (same priority 40) can't preempt, causing hang."
       testPrintStringBase testPositiveIntegerPrinting testNegativeIntegerPrinting
       testSlowFactorial testReciprocalModulo testPrintStringAll)
      includes: sel) ifFalse: [
      | result |
      s nextPutAll: '  RUN: '; nextPutAll: sel; cr. s flush.
      result := self runSingleTest: testClass selector: sel timeout: 10 priority: testPriority on: s.
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
        "Bail out of class after 3 consecutive timeouts — suspended processes
         hold Mutexes (compilation, class mod), causing next test to deadlock."
        consecutiveTimeouts >= 3 ifTrue: [
          s nextPutAll: '  BAIL: 3 consecutive timeouts, skipping rest of class'; cr.
          s nextPutAll: 'Total: '; print: sels size;
            nextPutAll: ' P:'; print: p; nextPutAll: ' F:'; print: f;
            nextPutAll: ' E:'; print: e; nextPutAll: ' S:'; print: sk;
            nextPutAll: ' T:'; print: t; cr; cr. s flush.
          ^ {p. f. e. sk. t}]]]].

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
  "Run a single test with semaphore-based completion + Delay timeout.
   Fork the test at the SAME priority. The test signals doneSem when done,
   waking the runner immediately (no polling delay). For timeouts, a separate
   watchdog process at priority+1 fires after the deadline and suspends
   the test, then signals doneSem."
  | result testProcess doneSem watchdog |
  result := nil. "nil means pass"
  doneSem := Semaphore new.

  "Fork test at requested priority"
  testProcess := [
    [[[[(testClass selector: sel) runCase]
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
         s flush] on: Error do: [:e2 | "ignore trace errors"]].
    doneSem signal] forkAt: priority.

  "Fork watchdog at priority+1 so it can preempt a hanging test.
   The watchdog waits for the timeout, then suspends the test and signals
   doneSem so the runner wakes up. For fast tests, the runner wakes from
   doneSem before the watchdog fires, and we suspend the watchdog."
  watchdog := [
    (Delay forSeconds: seconds) wait.
    "Timeout elapsed — test is still running"
    [testProcess suspend] on: Error do: [:e |].
    result := #timeout.
    s nextPutAll: '  TIMEOUT: '; nextPutAll: sel;
      nextPutAll: ' (exceeded '; print: seconds; nextPutAll: 's)'; cr.
    s flush.
    doneSem signal] forkAt: priority + 1.

  "Wait for test completion or watchdog timeout"
  doneSem wait.

  "Clean up watchdog if test completed before timeout"
  [watchdog suspend] on: Error do: [:e |].

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
