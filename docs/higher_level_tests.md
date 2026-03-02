# Higher-Level Test Packages

Pure-Smalltalk packages loadable via Metacello that provide substantial
test suites for VM validation beyond the built-in Kernel-Tests.

## Results (2026-03-02, Build 62)

  Package      Tests   Pass   Fail  Error  Rate    Notes
  NeoJSON        116    116      0      0  100%    JSON parsing, writing, Unicode
  Mustache        47     47      0      0  100%    Template expansion, closures
  XMLParser     5978   5978      0      0  100%    SAX, DOM, conformance suites
  Grease           0      -      -      -    -     No test packages in baseline
  PolyMath      1168   1162      5      1  99.5%   See failures below
  DataFrame      665    651     14      0  97.9%   See failures below
  Fuel             -      -      -      -    -     Timed out (too slow for interpreter)

  Total tested: 7986 pass / 8000 total = 99.8% pass rate

### PolyMath failures

Pre-existing bugs (also fail on stock Pharo VM):
  PMMatrixTest >> testMatrixCloseTo
  PMVectorTest >> testVectorCloseTo
  PMQRTest >> testDecompositionOfMatrixCausingErraticFailure
  PMClusterFinderTest >> testClusterEuclidean (flaky convergence)
  PMClusterFinderTest >> testClusterCovariance (flaky convergence)

Timeout (also times out on reference VM):
  PMArbitraryPrecisionFloatTest >> testPrintAndEvaluate
    Iterates ~32K compiler evaluations, exceeds 50s timeout on both VMs

### DataFrame failures

All pre-existing (also fail on stock Pharo VM):
  DataSeriesTest: testMathLn, testMathLog, testMathExp, testMathSqrt,
    testMathTan, testMathLog2, testStatsVariance, testStatsStdev
  DataFrameStatsTest: testVariance, testCorrelationMatrix, testAverage, testStdev
  DataFrameTypeDetectorTest: testDateAndTimeColumn, testDateAndTimeColumnWithNils

### Fuel

Timed out at 10 minutes (16 billion bytecodes). Fuel exercises heavy
object graph traversal, closure serialization, and become: which are
extremely slow on an interpreter VM without JIT. Not suitable for
routine testing.

## Loading Order (safest first, increasing VM stress)

### 1. NeoJSON — JSON Framework
- GitHub: https://github.com/svenvc/NeoJSON
- Tests: 116
- VM areas: streams, number parsing, dictionaries, Unicode
- Load:
      Metacello new
          repository: 'github://svenvc/NeoJSON/repository';
          baseline: 'NeoJSON';
          load.

### 2. Mustache — Template Engine
- GitHub: https://github.com/noha/mustache
- Tests: 47 (standard Mustache spec suite)
- VM areas: string processing, recursive expansion, closures, dictionaries
- Load:
      Metacello new
          baseline: 'Mustache';
          repository: 'github://noha/mustache:v1.4/repository';
          load.

### 3. XML-XMLParser — SAX/DOM XML Parser
- GitHub: https://github.com/pharo-contributions/XML-XMLParser
- Tests: 5978 (includes XML conformance suite — Expat, OASIS, Sun)
- VM areas: streams, string/character processing, DOM tree building, exceptions
- Load:
      Metacello new
          baseline: 'XMLParser';
          repository: 'github://pharo-contributions/XML-XMLParser/src';
          load.

### 4. Grease — Portability Library
- GitHub: https://github.com/SeasideSt/Grease
- Tests: 0 (baseline does not include test packages for Pharo 13)
- Load:
      Metacello new
          baseline: 'Grease';
          repository: 'github://SeasideSt/Grease:master/repository';
          load.

### 5. PolyMath — Scientific Computing
- GitHub: https://github.com/PolyMathOrg/PolyMath
- Tests: 1168
- VM areas: float/integer math, matrices, closures, large numbers, ODE solvers
- Load:
      Metacello new
          repository: 'github://PolyMathOrg/PolyMath';
          baseline: 'PolyMath';
          load.

### 6. DataFrame — Tabular Data
- GitHub: https://github.com/PolyMathOrg/DataFrame
- Tests: 665
- VM areas: collections, sorting, numeric computation, closures
- Load:
      Metacello new
          baseline: 'DataFrame';
          repository: 'github://PolyMathOrg/DataFrame/src';
          load.

### 7. Fuel — Object Serializer
- GitHub: https://github.com/theseion/Fuel
- Tests: ~100-200 (not runnable — too slow for interpreter)
- VM areas: object graph traversal, closure serialization, compiled methods,
  reflection, become:
- Load:
      Metacello new
          repository: 'github://theseion/Fuel:Pharo12/repository';
          baseline: 'Fuel';
          load.
- Note: Very VM-sensitive — exercises object memory layout directly.
  Too slow for interpreter VM; needs JIT to complete in reasonable time.

## Other Candidates

  Package              Tests   Notes
  NeoCSV               ~30-60  Streams, type conversion
  Microdown            ~100+   Recursive parsing, AST building (already in image)
  Containers-OrderedSet ~30-80 Set/collection protocol, hashing
  Exercism track       ~200+   Diverse algorithms
  SmaCC                ~100-300 Parser generator (older, may need patching)
  Magritte             ~100+   Reflection, meta-programming

## Running Tests

    ./scripts/run_package_tests.sh              # all packages
    ./scripts/run_package_tests.sh NeoJSON      # single package
    ./scripts/run_package_tests.sh NeoJSON XMLParser  # multiple packages

Results are written to:
  /tmp/pkg_test_results_all.txt — combined summary
  /tmp/pkg_tests/<package>/pkg_test_results.txt — per-package results
  /tmp/pkg_tests/<package>/pkg_test_detail.txt — per-test failure detail
