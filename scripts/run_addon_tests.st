"Add-on test packages: test suites beyond the core 577-class suite.
 These are Pharo packages that exercise areas not covered by the core suite.
 Inject via reference VM, run with our VM.
 Results: /tmp/addon_test_results.txt, /tmp/addon_test_detail.txt"!

Object subclass: #AddonTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Addon'!

!AddonTestRunner class methodsFor: 'running'!
runAllAddons
  "Run all add-on test packages and write results."
  | stream detail packages totalP totalF totalE totalS timeoutScale watchdogTimeout |
  stream := '/tmp/addon_test_results.txt' asFileReference writeStream.
  detail := '/tmp/addon_test_detail.txt' asFileReference writeStream.
  totalP := totalF := totalE := totalS := 0.
  timeoutScale := 10.  "Interpreter VM is ~10x slower than JIT"
  watchdogTimeout := 100 * timeoutScale. "100s default per test"

  [
    stream nextPutAll: '=== Add-on Test Packages ==='; cr.
    stream nextPutAll: 'Date: '; nextPutAll: DateAndTime now printString; cr; cr.
    detail nextPutAll: '=== Add-on Test Detail ==='; cr; cr.

    packages := self addonPackages.
    packages do: [:entry |
      | pkgName classes pass fail err skip |
      pkgName := entry key.
      classes := entry value.
      stream nextPutAll: '--- Package: '; nextPutAll: pkgName; nextPutAll: ' (';
        nextPutAll: classes size printString; nextPutAll: ' classes) ---'; cr.
      stream flush.
      detail nextPutAll: '=== '; nextPutAll: pkgName; nextPutAll: ' ==='; cr.
      pass := fail := err := skip := 0.
      classes do: [:clsName |
        | cls suite result startTime elapsed |
        cls := Smalltalk globals at: clsName ifAbsent: [nil].
        cls ifNil: [
          detail nextPutAll: '  MISSING: '; nextPutAll: clsName; cr.
          skip := skip + 1]
        ifNotNil: [
          detail nextPutAll: '  '; nextPutAll: clsName; nextPutAll: ': '.
          startTime := DateAndTime now.
          [
            | p f e s |
            suite := cls buildSuite.
            result := [suite run] valueWithin: (Duration seconds: watchdogTimeout)
              onTimeout: [nil].
            result ifNil: [
              detail nextPutAll: 'TIMEOUT'; cr.
              err := err + suite tests size]
            ifNotNil: [
              p := result passedCount. f := result failureCount.
              e := result errorCount. s := result skippedCount.
              pass := pass + p. fail := fail + f. err := err + e. skip := skip + s.
              detail nextPutAll: p printString; nextPutAll: ' pass, ';
                nextPutAll: f printString; nextPutAll: ' fail, ';
                nextPutAll: e printString; nextPutAll: ' error, ';
                nextPutAll: s printString; nextPutAll: ' skip'.
              (f > 0 or: [e > 0]) ifTrue: [
                result failures do: [:each |
                  detail cr; nextPutAll: '    FAIL: '; nextPutAll: each class name;
                    nextPutAll: '>>'; nextPutAll: each selector].
                result errors do: [:each |
                  detail cr; nextPutAll: '    ERROR: '; nextPutAll: each class name;
                    nextPutAll: '>>'; nextPutAll: each selector]].
              detail cr]
          ] on: Error do: [:ex |
            detail nextPutAll: 'EXCEPTION: '; nextPutAll: ex class name;
              nextPutAll: ' - '; nextPutAll: ([ex messageText] on: Error do: ['?']); cr.
            err := err + 1].
          detail flush]].
      totalP := totalP + pass. totalF := totalF + fail.
      totalE := totalE + err. totalS := totalS + skip.
      stream nextPutAll: '  Pass: '; nextPutAll: pass printString;
        nextPutAll: '  Fail: '; nextPutAll: fail printString;
        nextPutAll: '  Error: '; nextPutAll: err printString;
        nextPutAll: '  Skip: '; nextPutAll: skip printString; cr; cr.
      stream flush].

    stream cr; nextPutAll: '=== TOTALS ==='; cr.
    stream nextPutAll: 'Pass:  '; nextPutAll: totalP printString; cr.
    stream nextPutAll: 'Fail:  '; nextPutAll: totalF printString; cr.
    stream nextPutAll: 'Error: '; nextPutAll: totalE printString; cr.
    stream nextPutAll: 'Skip:  '; nextPutAll: totalS printString; cr.
    stream nextPutAll: 'Total: '; nextPutAll: (totalP + totalF + totalE + totalS) printString; cr.
  ] ensure: [stream close. detail close].

  Transcript show: 'Add-on results: /tmp/addon_test_results.txt'; cr! !

!AddonTestRunner class methodsFor: 'packages'!
addonPackages
  "Return an OrderedCollection of (packageName -> #(className ...)) pairs.
   These are packages NOT in the core 577-class suite.
   Organized by priority: most likely to pass and most useful first."
  | pkgs add |
  pkgs := OrderedCollection new.
  add := [:name :classes | pkgs add: (name -> (classes collect: [:n |
    n asSymbol]))].

  "--- Priority 1: Graphics and Rendering ---"
  "Would have caught both WarpBlt bugs. Already in image."
  add value: 'Graphics-Tests' value: #(
    FormTest ColorTest BitBltTest PointTest
    FormCanvasTest FormPaintTest WarpBltTest
    GradientFillStyleTest GIFReadWriterTest
    PNGReadWriterTest JPEGReadWriterTest BMPReadWriterTest).

  "--- Priority 2: Collections (beyond core) ---"
  add value: 'Collections-Unordered-Tests' value: #(
    PluggableDictionaryTest PluggableSetTest
    SmallDictionaryTest WeakValueDictionaryTest
    IdentityDictionaryTest OrderPreservingDictionaryTest
    WeakKeyDictionaryTest KeyedTreeTest
    FuzzyMatcherTest DoubleLinkedListTest
    BTreeTest IdentityBagTest
    ShortIntegerArrayTest TrieTest).

  add value: 'Collections-Sequenceable-Tests' value: #(
    LinkedListTest ArrayTest OrderedCollectionTest
    SortedCollectionTest RunArrayTest
    DoubleByteArrayTest IntegerArrayTest
    FloatArrayTest WordArrayTest ByteArrayTest).

  add value: 'Collections-Streams-Tests' value: #(
    ReadWriteStreamTest ReadStreamTest WriteStreamTest
    MultiByteBinaryOrTextStreamTest BinaryFileStreamTest
    ZnCharacterReadStreamTest ZnCharacterWriteStreamTest).

  add value: 'Collections-Weak-Tests' value: #(
    WeakArrayTest WeakSetTest EphemeronTest
    WeakRegistryTest WeakIdentitySetTest
    WeakAnnouncerTest WeakFinalizerTest).

  "--- Priority 3: Text and Formatting ---"
  add value: 'Text-Tests' value: #(
    TextTest RunArrayTest TextLineTest
    TextConverterTest TextStyleTest
    FontTest TextEmphasisTest
    TextActionTest AttributedStringTest
    TextEditorTest TextMorphTest TextPrinterTest).

  add value: 'STON-Tests' value: #(
    STONReaderTest STONWriterTest STONTest
    STONStreamingTest STONLargeDataTest
    STONMaterializerTest STONReferenceTest
    STONJSONTest STONStreamWriterTest STONShortcutTest).

  "--- Priority 4: Compression ---"
  add value: 'Compression-Tests' value: #(
    ZLibReadStreamTest ZLibWriteStreamTest
    GZipReadStreamTest GZipWriteStreamTest).

  "--- Priority 5: File System ---"
  add value: 'FileSystem-Core-Tests' value: #(
    FileSystemTest PathTest FileReferenceTest
    FileLocatorTest FileSystemDirectoryEntryTest
    AbstractFileReferenceTest CopyVisitorTest
    DeleteVisitorTest FileSystemResolverTest
    FileSystemPermissionsTest MemoryFileSystemTest
    DiskFileSystemTest).

  add value: 'FileSystem-Tests-Attributes' value: #(
    FileAttributesTest FileAttributesPluginTest
    DiskFileAttributesTest).

  add value: 'Files-Tests' value: #(
    FileTest SourceFileArrayTest
    SourceFileTest MultiByteBinaryOrTextStreamTest).

  "--- Priority 6: Network ---"
  add value: 'Network-Tests' value: #(
    SocketTest SocketStreamTest
    URITest URLTest NetNameResolverTest
    MIMETypeTest InternetConfigurationTest
    SocketAddressTest HTTPClientTest
    NetworkSystemSettingsTest SocketPrimTest
    HTTPServerTest TCPSocketEchoTest
    TCPSendRecvTest UDPSocketTest
    UDPMulticastTest).

  add value: 'Zinc-Character-Encoding-Tests' value: #(
    ZnCharacterEncoderTest ZnUTF8EncoderTest
    ZnByteStringEncoderTest ZnLatin1EncoderTest
    ZnNullEncoderTest ZnBufferedReadStreamTest
    ZnBufferedWriteStreamTest ZnBufferedReadWriteStreamTest
    ZnCharacterReadStreamTest ZnCharacterWriteStreamTest
    ZnCharacterStreamTest ZnFastLineReaderTest).

  add value: 'Zinc-Tests' value: #(
    ZnClientTest ZnServerTest ZnEntityTest
    ZnHeadersTest ZnMimeTypeTest ZnMultiPartFormDataEntityTest
    ZnMultiValueDictionaryTest ZnRequestTest ZnResponseTest
    ZnStatusLineTest ZnUrlTest ZnSingleThreadedServerTest
    ZnLogEventTest ZnServerSessionManagerTest
    ZnDefaultServerDelegateTest ZnStaticFileServerDelegateTest
    ZnHTTP1Test ZnHTTPSTest ZnProxyTest
    ZnChunkedTest ZnLineReaderTest
    ZnCommonUrlsTest ZnOptimizedTest).

  add value: 'Zinc-Resource-Meta-Tests' value: #(
    ZnMimeTypeTest ZnFileUrlTest
    ZnUrlTest ZnUrlAuthorityTest ZnUrlPathTest).

  "--- Priority 7: Numeric / Math ---"
  add value: 'NumericInterpolator-Tests' value: #(
    NInterpolatorTest NNewtonInterpolatorTest
    NLagrangeInterpolatorTest NLinearInterpolatorTest
    NNearestNeighborInterpolatorTest NCubicInterpolatorTest
    NAkimaInterpolatorTest NSplineInterpolatorTest
    NLoessInterpolatorTest NBulirschStoerInterpolatorTest
    NThieleInterpolatorTest NBarycentric2InterpolatorTest).

  add value: 'Geometry-Tests' value: #(
    GPointTest GSegmentTest GLineTest GRectangleTest
    GEllipseTest GCircleTest GPolygonTest
    GAffineTransformationTest GCoordinatesTest
    GTransformTest GArcTest GBezierTest
    GConvexHullTest GVectorTest).

  "--- Priority 8: Morphic (may need display) ---"
  add value: 'Morphic-Tests' value: #(
    MorphTest MorphLayoutTest HandMorphTest
    MorphTreeMorphTest TransformMorphTest
    BalloonEngineTest PasteUpMorphTest
    StringMorphTest WorldMorphTest
    SketchMorphTest AlignmentMorphTest
    MorphPositionTest
    SystemWindowTest WidgetExampleTest
    MorphEventTest DisplayScreenTest
    LayoutPolicyTest FormCanvasTest
    InnerTextMorphTest ParagraphTest
    TextMorphTest).

  "--- Priority 9: Keymapping ---"
  add value: 'Keymapping-Tests' value: #(
    KMKeyCombinationTest KMDispatcherTest
    KMCategoryTest KMRepositoryTest
    KMGlobalDispatcherTest KMSingleEventDispatcherTest
    KMShortcutTest KMKeyCompareTest
    KMCombinationTest KMKeymapBuilderTest).

  "--- Priority 10: System utilities ---"
  add value: 'System-Support-Tests' value: #(
    AuthorTest SystemNavigationTest
    ChangeSetTest SmalltalkImageTest
    SystemDependencyTest SystemOrganizationTest
    SystemVersionTest).

  add value: 'System-Time-Tests' value: #(
    DateTest TimeTest DateAndTimeTest
    DurationTest TimeZoneTest WeekTest
    MonthTest ScheduleTest TimespanTest
    ChronologyTest YearTest TimeStampTest
    DateAndTimeLeapSecondTest
    DateAndTimeDSTTest TimeZoneDatabaseTest
    DateAndTimeEpochTest StopwatchTest
    HistoryTest DatePrintingTest
    DateAndTimeFormatTest TimeMeasureTest
    DateArithmeticTest).

  "--- Priority 11: Fuel serialization ---"
  add value: 'Fuel-Core-Tests' value: #(
    FLBasicSerializationTest FLGlobalEnvironmentTest
    FLCompiledMethodSerializationTest
    FLBlockClosureSerializationTest
    FLClassSerializationTest FLTraitSerializationTest
    FLClassFactoryForTestCaseTest
    FLCustomObjectSerializationTest
    FLSerializerTest FLMaterializerTest
    FLStreamStrategyTest
    FLFullBlockClosureSerializationTest
    FLProcessSerializationTest FLSemaphoreSerializationTest
    FLContextSerializationTest FLCollectionSerializationTest
    FLNumberSerializationTest FLCharacterSerializationTest
    FLStringSerializationTest FLSymbolSerializationTest
    FLDateAndTimeSerializationTest FLAssociationSerializationTest
    FLDictionarySerializationTest FLArraySerializationTest
    FLByteArraySerializationTest FLCompiledBlockSerializationTest
    FLMetaclassSerializationTest FLBehaviorSerializationTest
    FLObsoleteSerializationTest FLPackageSerializationTest
    FLClassOrganizationSerializationTest
    FLSourceCodeSerializationTest FLGlobalClassSerializationTest
    FLHeaderSerializationTest FLBytecodeSerializationTest
    FLColorSerializationTest FLPointSerializationTest
    FLRectangleSerializationTest FLFormSerializationTest
    FLTrueSerializationTest FLFalseSerializationTest
    FLNilSerializationTest FLFloatSerializationTest
    FLLargeIntegerSerializationTest
    FLSmallIntegerSerializationTest
    FLVariableSerializationTest FLWideStringSerializationTest
    FLPluggableSerializationTest).

  "--- Priority 12: Slots and class builder ---"
  add value: 'Shift-ClassBuilder-Tests' value: #(
    ShiftClassBuilderTest ShiftClassInstallerTest
    ShiftClassBuilderWithTraitsTest
    ShiftClassInstallAndMigrateTest
    SlotAnonymousClassInstallerTest
    ShiftMigrationTest ShiftAnonymousInstallerTest
    ShiftRemoveClassTest ShiftClassBuilderWithPackagesTest
    ShiftSuperclassChangesTest
    ShiftClassBuildAndMigrateFullTest
    ShiftClassBuilderSmokeTest
    ShiftClassBuilderWithSlotsTest
    ShiftInstallerTraitRelatedTest
    ShiftTraitBuilderTest
    ShiftClassBuilderUtilitiesTest
    ShiftPragmaAndPoolTest).

  "--- Priority 13: Monticello ---"
  add value: 'Monticello-Tests' value: #(
    MCChangeNotificationTest MCClassDefinitionTest
    MCDependencySorterTest MCDiffyTextWriterTest
    MCFileInTest MCMczInstallerTest
    MCMethodDefinitionTest MCMockDefinitionVisitor
    MCOrganizationTest MCPackageTest
    MCPatchTest MCSnapshotTest
    MCStReaderTest MCStWriterTest
    MCThreeWayMergerTest MCVersionTest
    MCWorkingCopyTest MCSnapshotBrowserTest
    MCSnapshotResourceTest MCTraitDefinitionTest
    MCMethodDefinitionOrderTest MCRemovalTest
    MCBasicRepositoryTest MCCacheRepositoryTest
    MCDirectoryRepositoryTest MCDictionaryRepositoryTest
    MCPackageManagerTest MCMergerTest
    MCMockPackageInfoTest MCVersionInfoTest
    MCGitBasedNetworkRepositoryTest MCLatestTest).

  "--- Priority 14: Ring metamodel ---"
  add value: 'Ring-Core-Tests' value: #(
    RGBehaviorTest RGClassTest RGCommentTest
    RGEnvironmentTest RGGlobalVariableTest
    RGInstanceVariableSlotTest RGLayoutTest
    RGMetaclassTest RGMethodBodyTest
    RGMethodDefinitionTest RGPackageTest
    RGProtocolTest RGSingletonTest
    RGTraitCompositionTest RGTraitTest
    RGClassVariableTest RGPoolVariableTest
    RGBehaviorResolverTest RGFixedLayoutTest
    RGByteLayoutTest RGCompiledMethodLayoutTest
    RGDoubleByteLayoutTest RGDoubleWordLayoutTest
    RGEphemeronLayoutTest RGImmediateLayoutTest
    RGVariableLayoutTest RGWeakLayoutTest
    RGWordLayoutTest RGReadOnlyTest
    RGMergeTest RGDiffTest
    RGSliceTest RGInheritanceTest).

  "--- Priority 15: Microdown ---"
  add value: 'Microdown-Tests' value: #(
    MicBlockTextTest MicMicrodownSnippetTest
    MicArgumentListTest MicAnnotationBlockTest
    MicAnchorBlockTest MicAnchorReferenceBlockTest
    MicCodeBlockTest MicCommentBlockTest
    MicEnvironmentBlockTest MicFigureBlockTest
    MicFootnoteBlockTest MicHeaderBlockTest
    MicHorizontalLineBlockTest MicInlineBlockTest
    MicInputFileBlockTest MicLinkBlockTest
    MicListBlockTest MicMathBlockTest
    MicMathInlineBlockTest MicOrderedListBlockTest
    MicParagraphBlockTest MicQuoteBlockTest
    MicRootBlockTest MicSectionBlockTest
    MicTableBlockTest MicUnorderedListBlockTest
    MicMicrodownParserTest MicMicrodownInlineParserTest
    MicInlineSplitterTest MicInlineParserTest
    MicPathResolverTest MicArgumentsSplitterTest
    MicFigureTest MicColumnTest
    MicSlideTest MicRawBlockTest
    MicCitationBlockTest MicMicrodownTextualBuilderTest
    MicInlineEmphasisCloserTest MicResourceReferenceTest
    MicFileResourceReferenceTest MicCheckerTest
    MicScriptLanguageTest MicDocumentTest
    MicHTTPResourceReferenceTest MicMicrodownParagraphTest
    MicPhylisopheTest MicConcatenationTest
    MicCounterTest).

  ^ pkgs! !

!AddonTestRunner class methodsFor: 'running'!
run
  self runAllAddons.
  Smalltalk snapshot: false andQuit: true! !

"Auto-run on image start if /tmp/addon_run_flag.txt exists"
'/tmp/addon_run_flag.txt' asFileReference exists
  ifTrue: [AddonTestRunner run]
  ifFalse: [Transcript show: 'AddonTestRunner injected (set /tmp/addon_run_flag.txt to auto-run)'; cr]!
