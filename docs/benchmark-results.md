# Benchmark Results: Our VM vs Reference Pharo VM

## Latest: Build 115 — superinstructions + accessor inlining

    Date:          2026-03-31
    Build:         115 (updated)
    Optimization:  Superinstructions + trivial getter/setter inlining
    Script:        startup.st timing benchmark (via StartupPreferencesLoader)
    Image:         Pharo 13 (130) fresh download

    Method:  3 runs each on identical fresh images, CPU time via `time`.
             primitiveQuit fix allows clean exit (~14s wall) instead of
             600s timeout.

    Baseline (build 114):   17.71  18.18  18.10   avg = 18.00s
    Superinstructions only: 17.52  17.46  17.48   avg = 17.49s  (+2.8%)
    + accessor inlining:    16.36  16.40  16.35   avg = 16.37s  (+6.4%)
    Total improvement:      ~9.1% CPU

    returnsSelf A/B (3 alternating pairs, post quit-fix):
    Baseline:    14.08  13.19  10.32   avg = 12.53s
    returnsSelf: 13.89  12.85  10.55   avg = 12.43s
    Difference:  ~0.8% — within noise (yourself/identity methods too rare)

    Accessor inlining detects trivial getter/setter methods at method cache
    time. On cache hit, bypasses Smalltalk activation entirely — no frame
    push/pop, no IP/method switch. Getters (pushRecvVar + returnTop) and
    setters (popStoreRecvVar + returnReceiver) are handled.

## Build 115 — speculative superinstructions

    Date:          2026-03-31
    Build:         115
    Optimization:  Speculative superinstructions in computed goto handlers
    Script:        scripts/time_tests.st
    Image:         Pharo 13 (130) fresh download

    Our VM:        iospharo test_load_image (interpreter-only, no JIT)
    Reference VM:  Pharo v10.3.9 (Cog JIT, 33e04bb60)

    Method:  3 runs each on identical fresh images, 300s wall timeout,
             CPU time measured via `time`.

    Baseline (build 114):  17.71  18.18  18.10   avg = 18.00s
    Superinstructions:     17.52  17.46  17.48   avg = 17.49s
    Improvement:           ~2.8% CPU

    Superinstructions implemented (profile-guided from 100M bytecodes):
    - SmallInt comparison + conditional jump fusion (5.3M pairs):
      Skip boolean object creation, branch directly on comparison result
    - push1 + arith+ / arith- fusion (2.4M, 65% hit rate):
      Inline x+1 / x-1 without pushing constant
    - pushNil + spec== fusion (1.73M, 46% hit rate):
      Inline nil identity check, fuse with following branch
    - dup + pushNil + spec== + jump fusion (1.45M, 96% hit rate):
      Full "x ifNotNil:" idiom in one dispatch
    - push0 + arith= fusion (356K): Inline "x = 0" check
    - spec== (0x76) fast handler (2.4M total):
      Identity comparison inlined as rawBits==, fused with branch

    The 2.8% improvement is below the Phase 3 estimate (15-30%) because
    Apple M1's branch predictor already handles the dispatch jump table
    efficiently. Eliminating dispatches saves ~11% of dispatch operations
    but dispatch overhead is a smaller fraction of total time than expected.
    The bottleneck is method lookup and stack frame setup, not dispatch.

## Build 114 — computed goto dispatch

    Date:          2026-03-31
    Build:         114
    Optimization:  Computed goto dispatch in interpret()
    Script:        scripts/time_tests.st
    Image:         Pharo 13 (130) fresh download

    Our VM:        iospharo test_load_image (interpreter-only, no JIT)
    Reference VM:  Pharo v10.3.9 (Cog JIT, 33e04bb60)

    Method:  3 runs each on identical fresh images, 300s wall timeout,
             CPU time measured via `time`. Both binaries built from same
             commit tree, only Interpreter.cpp differs.

    Baseline (switch):   18.17  18.51  18.63   avg = 18.44s
    Computed goto:       17.71  18.18  18.10   avg = 18.00s
    Improvement:         ~2.5% CPU

    As predicted by research (docs/pseudo-jit.md), computed goto on Apple M1
    yields only 2-5% due to excellent branch prediction hardware. The benefit
    comes from per-handler branch predictor entries and eliminating the
    running_/bytecodeEnd_ per-bytecode checks.

## Build 113 — sendSelector cleanup + multi-probe cache

    Date:          2026-03-31
    Build:         113
    Optimizations: sendSelector() hot path cleanup + 2-way 4096-entry method cache
    Script:        scripts/time_tests.st
    Image:         Pharo 13 (130) fresh download

    Our VM:        iospharo test_load_image (interpreter-only, no JIT)
    Reference VM:  Pharo v10.3.9 (Cog JIT, 33e04bb60)

    VM              Classes   Tests    CPU time   Smalltalk ms
    Reference        1999    27968      0.18s          58
    Ours             1999    27968     16.88s        ~5400*

    * Smalltalk-measured ms has ~10% run-to-run variance (5002-5708ms across
      runs) due to bimodal test distribution. CPU time is the reliable metric.

    CPU improvement since build 112: 20.81s → 16.88s (18.9% faster)

## Build 112 — interpret() fast loop

    Date:          2026-03-31
    Build:         112
    Git hash:      e9fc0c9
    Optimization:  step() hot path → fast interpret() loop

    VM              Classes   Tests    CPU time   Smalltalk ms
    Reference        1999    27968      0.18s          58
    Ours             1999    27968     20.81s        5002

    Improvement over build 111: 5731ms → 5002ms (12.7% by Smalltalk time)

## Previous: Build 111 (flat switch, step() loop)

    Date:          2026-03-30
    Build:         111
    Git hash:      2898e7d
    Optimization:  Flat switch bytecode dispatch (if-else -> switch)
    Script:        scripts/time_tests.st
    Image:         Pharo 13 (130) fresh download

    Our VM:        iospharo test_load_image (interpreter-only, no JIT)
    Reference VM:  Pharo v10.3.9 (Cog JIT, 33e04bb60)

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

The distribution is bimodal: classes are either 0-5ms or 150-213ms with
nothing in between. This is NOT per-class constant overhead — it reflects
which classes have computationally heavy tests vs trivial ones.

The 22 slow classes (150-213ms) have per-test costs of 1-25ms:

    Class                              Tests   ms/test  Why slow
    MicRawBlockTest                       8     25.0    Markdown parsing, string manipulation
    AIAstarTest                           8     19.0    Graph search with heavy iteration
    IntegerTest                          83      2.4    Factorial, modular arithmetic, primes
    DictionaryTest                      205      1.0    Rehashing, growing, iteration
    OrderedCollectionTest               351      0.6    Collection mutation in loops

Meanwhile "fast" classes do 0.05ms/test (trivial operations like set
membership, identity checks, basic array access) — these are essentially
free on both VMs.

The reference VM (JIT) finishes even the heavy classes in <1ms total
because it compiles hot loops to native code. Our interpreter pays full
dispatch cost for every iteration, which is the expected 100-200x penalty
for loop-heavy Smalltalk code on an interpreter vs JIT.

The 77x overall ratio is dominated by these 22 heavy classes (5.4s of
the 5.7s total). The other 1977 classes total only 0.3s.

Previous measurement (build ecd0d70, 2026-02-23) showed ~445x slower.
Current build 111 shows 77x — a ~5.8x improvement, though methodology
differences (per-class vs per-test timing) make direct comparison rough.

Next optimization targets (see docs/optimizations.md):
- Slim down step() hot path (eliminate per-bytecode overhead)
- Reduce syscalls in periodic checks
- Multi-probe method cache
