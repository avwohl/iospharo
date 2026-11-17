# Repairs of packages of the 200-package run, for review by the owner

This file is the record of each package of
`scripts/pkg-jit-test/packages-200.tsv` that needed a change to load or to
pass its tests on Pharo 14 (image build 791).  Each defect here shows on
stock Cog and on our VM.  Nothing here was sent to the owner of a package.

A repair has one of these forms:

- `scripts/pkg-jit-test/patches/<label>.patch`: a git diff of the repository
  of the package.  The package run loads the package from a local clone with
  the patch.  The patch file is the change to send to the package.
- `scripts/pkg-jit-test/fixes/<label>.st`: entries that the package run files
  in after the load.  Each entry names the method and has the new source, so
  the entry is also the text of the change for the package.
- A changed row of the manifest (a defect of our list, not of the package).

How to run one row again: `scripts/vmrun.sh pkgs --redo <outdir> <row>`.

## Glorp (`pharo-rdbms/glorp`, row 3)

- Defect: `BaselineOfGlorp` loads the package `Glorp-Pharo10` for
  `pharo10.x` to `pharo13.x` only.  The package has one method,
  `String>>convertToEncoding:`.  On Pharo 14 the method is absent.
- Effect: `GlorpDatabasePlatformTest>>testTrimString1` and `testTrimString2`
  stop with `MessageNotUnderstood` (2 errors of 262 tests).
- Repair here: `fixes/pharo-rdbms-glorp.st` adds the method.
- Change for the package: add `#'pharo14.x'` to the platform list in
  `BaselineOfGlorp>>baseline:`.
- Result with the repair: 262 of 262 pass on the two VMs.

## Buoy (`ba-st/Buoy`, row 10)

- Defect: the package `Buoy-Metaprogramming-Pharo-Extensions` has an
  extension `Symbol>>argumentCount ^ self numArgs`.  Pharo 14 has its own
  `Symbol>>argumentCount ^ self numArgs + 1` (the receiver is the first
  argument when a symbol stands for a block), and
  `Symbol>>valueWithEnoughArguments:` depends on that value.  The load of
  Buoy replaces the method of the image.
- Effect: Metacello calls the post-load action of the baseline with
  `valueWithEnoughArguments:`.  With the method of Buoy the argument array is
  empty, and the load stops with `SubscriptOutOfBounds: 1 in #()`.  Each
  later use of a symbol as a block in the image has the same defect.
- Repair: `patches/ba-st-buoy.patch` removes the extension and changes the
  one sender in Buoy (`KeywordMessageSendingCollector`) to `numArgs`.
- State with the patch: the load works.  363 tests, 362 pass on the two VMs.
- Second defect: `OrderedDictionaryExtensionsTest>>testEquals` is a copy of
  an old test of Pharo.  It fills two dictionaries in opposite orders and
  expects that the two are equal.  `OrderedDictionary>>=` of Pharo 14
  compares the index of each key, so the order matters.  The test of Pharo
  14 itself fills the two in the same order.
- Repair here: `fixes/ba-st-buoy.st` gives the test the form of the Pharo 14
  test, with one more check that the opposite order is not equal.  This
  repair has no run yet.

## Pharo-Advent-Of-Code (`terryc321/Pharo-Advent-Of-Code`, row 8)

- Defect: `BaselineOfPharoAdventOfCode` names a package `AdventOfCode`.  The
  repository has no package with that name, and its history never had one.
- Effect: the load stops with `KeyNotFound: key 'AdventOfCode'`.
- Repair: `patches/terryc321-pharo-advent-of-code.patch` names four packages
  of the repository: `Advent-Of-Code-2015` (needs PetitParser2),
  `Advent-Of-Code-2016`, `Advent-Of-Code-2020` and `AdventCode2025`.
- Not in the patch, for the owner of the package to decide:
  `Advent-Of-Code-2018` needs Bloc.  `AdventCode` and `AdventCode2015` have
  the same class names as `Advent-Of-Code-2015` (60 and 19 class files), so
  the three packages cannot be in one image.

## MuTalk (`pharo-contributions/mutalk`, row 4)

- Defect: `MTAnalysisTest>>testExecutingInfiniteRecursionAnalysis` and
  `MTMethodMutationTest>>testMutationInfiniteRecursion` run a mutant that is
  a recursion with no end.  Only the SUnit time limit of the fixture test
  `MTFakeInfiniteLoopsTest` stops the mutant, after 10 seconds.
- Effect on stock Cog: a recursion has no depth limit, and the frames become
  heap Contexts at about 1 GB each second on this machine.  With no
  old-space limit the VM used 13 GB.  With a limit of 6144 MB the VM stops
  with `Could not allocate new object in the old space`.
- Effect on our VM: the two tests pass in 25 ms, for another cause.  Our VM
  signals an error at a depth of 56000 frames, which Cog does not do
  (`docs/vm-compat-bugs.md`, open item of 2026-11-17).
- Repair here: `fixes/pharo-contributions-mutalk.st` gives the fixture test
  a time limit of 1 second (`MTFakeInfiniteLoopsTest>>defaultTimeLimit`).
- State with the repair: 337 tests, 336 pass and 1 skip on the two VMs.
  Stock Cog ends the class list in 12 seconds.
- Second defect: `MTCITonelJsonExporterTest>>testWorking` starts with
  `self skip` since 2024-06-27 ("a lots of strong dependencies").  The test
  needs the Iceberg repository of mutalk and its clone, and the package run
  has the two.  The code under test names `RPackageOrganizer`, which Pharo
  14 does not have.  The fixes file takes the skip out and changes the class
  name to `PackageOrganizer`.  This repair has no run yet.
- The classes of the packages `MuTalk-TestResources` and
  `MuTalk-TestResourcesForExtensionMethods` are fixtures that MuTalk tests
  run, and some fail on purpose.  The class list of the package run leaves
  out a test class in a package with `TestResources` in its name.

## XML-XMLParser (`pharo-contributions/XML-XMLParser`, row 1)

- State before: 110 of 6027 tests skip.  `XMLSkippableTest` has a class-side
  flag `isSkipping` that is true when nobody sets it.
- Repair here: `fixes/pharo-contributions-xml-xmlparser.st`.
  - `XMLFileHandleTest stopSkippingAll` (90 tests with files in the working
    directory) and `XMLExternalEntityTest stopSkipping` (7 file tests and 9
    tests that use HTTP).
  - `XMLConformanceExpatTest>>testnotwfsa140xml` and `testnotwfsa141xml`
    expected a parser error for names with U+309A and U+0E5C.  The parser
    has the name characters of XML 1.0 edition 5, where the two documents
    are well-formed.  The tests now expect no parser error.
  - Two test documents named a DTD address that answers 404 Not Found
    (checked 2026-11-17).  `docBookArticleSource` now names
    `http://www.oasis-open.org/docbook/xml/4.5/docbookx.dtd`, which is the
    system identifier of DocBook XML V4.5.  `musicXMLDocument` now names the
    DTD in the repository of the W3C Music Notation group
    (`https://raw.githubusercontent.com/w3c/musicxml/gh-pages/schema/partwise.dtd`).
  - `XMLConformanceSunTest>>testinvalidutf16bxml` and `testinvalidutf16lxml`
    were in the list of the tests to skip.  The two documents have no DTD,
    and the test suite lists them as invalid.  The parser of the tests
    validates and does not require a DTD, so it raised no error.  The two
    tests now set `requiresSchema: true`.  This repair has no run yet.
- State with the repairs before the last one: 6025 of 6027 pass on the two
  VMs, 2 skip, and the two VMs agree on each test.  The HTTP tests ran on the
  two VMs.

## PharoMisc (`bouraqadi/PharoMisc`, row 6)

- Defect of our manifest: the row loaded the baseline `PharoMisc` only.
  That baseline has one class and no test.  The repository has one baseline
  for each of its projects.
- Repair: the row now loads nine baselines (`PharoMisc`, `StateMachine`,
  `Concurrency`, `BooleanExpressions`, `BaselineAnalyzer`, `CodeMetrics`,
  `LightweightObserver`, `PharoExtra`, `SimpleMiddleware`).  Not in the row:
  `Tasks`, `NetworkExtras` and `EasyUI`, whose baselines name the repository
  without its `Pharo` directory.
- State: 225 tests, 160 pass, 41 fail, 24 errors, the same on the two VMs.
  Open: the cause of the 65 tests that do not pass.
