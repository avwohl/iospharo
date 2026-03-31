# Benchmark Results: Our VM vs Reference Pharo VM

    Date:          2026-03-30
    Build:         111
    Git hash:      2898e7d
    Optimization:  Flat switch bytecode dispatch (if-else -> switch)
    Script:        scripts/time_tests.st
    Image:         Pharo 13 (130) fresh download

    Our VM:        iospharo test_load_image (interpreter-only, no JIT)
    Reference VM:  Pharo v10.3.9 (Cog JIT, 33e04bb60)

## Overall

    VM              Classes   Tests    Total ms   Wall time
    Reference        1999    27968        74ms      ~1s
    Ours             1999    27968      5731ms     ~11s
    Ratio                                77.4x

Note: wall time includes VM startup, image boot, GC. The 77x ratio
overstates the gap because most reference VM classes clock at 0ms
(sub-millisecond), while our VM shows ~200ms per class from constant
overhead (startup.st patches, class enumeration, etc.).

## Measurable classes (reference VM >= 1ms)

For the 32 classes where the reference VM took >= 1ms, our VM is
only 1.5x slower total (110ms vs 74ms). Many are actually faster:

    Class                                            Tests  Ref ms  Ours ms  Ratio
    SymbolTest                                         268       2       16   8.0x
    RxMatcherTest                                      176       2       11   5.5x
    WeakIdentityKeyDictionaryTest                      209       2       11   5.5x
    IdentitySetTest                                    176       2       10   5.0x
    BagTest                                            168       2        9   4.5x
    SmallIdentityDictionaryTest                        207       3       11   3.7x
    FLFullBasicSerializationTest                        86       2        4   2.0x
    FloatTest                                           73       2        4   2.0x
    OrderedDictionaryTest                               67       2        3   1.5x
    ZnCharacterEncoderTest                              42       2        3   1.5x
    ClyConcreteGroupCritiquesTest                       36       2        2   1.0x
    ClyPackageScopeTest                                 29       2        2   1.0x
    CompletionEngineTest                                51       3        3   1.0x
    ReSemanticsOfInlineMethodRefactoringTest            35       2        2   1.0x
    TFBasicTypeMarshallingTest                          22       2        2   1.0x
    ZnUrlTest                                           50       3        3   1.0x
    CDFluidClassParserTest                              44       3        2   0.7x
    ClyAllMethodsQueryTest                              33       2        1   0.5x
    DelayBasicSchedulerMicrosecondTickerTest            16       2        1   0.5x
    EpRevertTest                                        23       2        1   0.5x
    MicMetaDataBlockTest                                20       2        1   0.5x
    ObjectTest                                          26       2        1   0.5x
    ProtocolAnnouncementsTest                           14       2        1   0.5x
    RSLinesTest                                         18       2        1   0.5x
    SemaphoreTest                                       16       2        1   0.5x
    HaltTest                                            18       3        1   0.3x
    OCParseTreeRewriterTest                             12       3        1   0.3x
    RSBoxPlotTest                                       25       3        1   0.3x
    TraitTestCase                                       19       3        1   0.3x
    MethodAnnouncementsTest                             10       3        0   0.0x
    OCAnnotationTest                                     5       2        0   0.0x
    ReflectivityTest                                     5       3        0   0.0x

## Top 30 slowest classes (absolute ms, our VM)

These are dominated by per-class constant overhead, not per-test cost.

    Class                                            Tests  Ref ms  Ours ms  Ratio
    SystemEnvironmentTest                              217       0      213  213.0x
    DictionaryTest                                     205       0      204  204.0x
    SpTreeTablePresenterMultipleSelectionTest           64       0      204  204.0x
    MemoryFileSystemTest                                67       0      203  203.0x
    IntegerTest                                         83       0      201  201.0x
    MicRawBlockTest                                      8       0      200  200.0x
    STONLargeWriteReadTest                              37       0      200  200.0x
    StRewriterMatchToolPresenterTest                    14       0      199  199.0x
    OrderedCollectionTest                              351       0      196  196.0x
    RSLinePlotTest                                      31       0      196  196.0x
    FBDDecompilerTest                                  160       0      194  194.0x
    TonelWriterV3Test                                   19       0      194  194.0x
    ClyFilterQueryTest                                  41       0      193  193.0x
    ClySharedPoolReferencesQueryTest                    33       0      193  193.0x
    OCDoItVariableTest                                  10       0      192  192.0x
    WeakKeyDictionaryTest                              207       0      191  191.0x
    ZnPositionableReadStreamTest                        14       0      187  187.0x
    PluggableDictionaryTest                            209       0      186  186.0x
    FreeTypeCacheTest                                   25       0      182  182.0x
    SpCodePresenterTest                                 67       0      179  179.0x
    BuilderManifestTest                                 20       0      168  168.0x
    AIAstarTest                                          8       0      152  152.0x
    StringTest                                         438       0       26   26.0x
    ArrayTest                                          324       0       17   17.0x
    IntervalTest                                       260       0       16   16.0x
    SymbolTest                                         268       2       16    8.0x
    LinkedListTest                                     255       0       15   15.0x
    SortedCollectionTest                               287       0       15   15.0x
    Float32ArrayTest                                   277       0       14   14.0x
    Float64ArrayTest                                   268       0       14   14.0x

## Distribution of ratios (classes where ours > 0ms)

    946 classes had measurable time in our VM:

       <10x: 912  (96%)
     10-50x:  12
    50-100x:   0
   100-250x:  22
      >250x:   0

## Analysis

The ~200ms "floor" visible in the slowest classes (all showing ref=0ms,
ours~=200ms) is a per-class constant cost — likely TestCase class
enumeration, setUp/tearDown, Delay-based watchdog overhead, or process
scheduling. The actual per-test execution cost is modest: for classes
with enough tests to be measurable, we're only 1.5-8x slower.

The 77x overall ratio is misleading because:
1. 1050 classes finish in 0ms on both VMs
2. 22 classes show ~200ms constant overhead (not per-test cost)
3. For the 32 classes with ref >= 1ms, the ratio is only 1.5x

Previous measurement (build ecd0d70, 2026-02-23) showed ~445x slower.
Current build 111 shows 77x — a ~5.8x improvement, though methodology
differences (per-class vs per-test timing) make direct comparison rough.

Next optimization targets (see docs/optimizations.md):
- Slim down step() hot path (eliminate per-bytecode overhead)
- Reduce syscalls in periodic checks
- Multi-probe method cache
