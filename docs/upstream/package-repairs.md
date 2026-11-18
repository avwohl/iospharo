# Repairs of packages of the 200-package run, for review by the owner

This file is the record of each package of
`scripts/pkg-jit-test/packages-200.tsv` that needed a change to load or to
pass its tests on Pharo 14 (image build 791).  Each defect here shows on
stock Cog and on our VM.  Nothing here was sent to the owner of a package.

A repair has one of these forms:

- `scripts/pkg-jit-test/patches/<label>.patch`: a git diff of the repository
  of the package.  The package run loads the package from a local clone with
  the patch.  The patch file is the change to send to the package.
- `scripts/pkg-jit-test/locks/<label>.txt`: the package has a dependency
  that needs the patch of another row.  Each line has the baseline name of
  the dependency and the label of that row.
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
  test, with one more check that the opposite order is not equal.
- Result with the two repairs: 616 of 616 tests pass on the two VMs (the
  class list now has each loaded package of the repository).
- Packages that load Buoy as a dependency get the same defect:
  `ba-st/Launchpad` (row 14) and `ba-st/Hyperspace` (row 15) stopped in the
  load with `SubscriptOutOfBounds: 1 in #()`.  The files
  `locks/ba-st-launchpad.txt` and `locks/ba-st-hyperspace.txt` make the load
  take Buoy from a local clone with the patch (a Metacello lock).

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
- State with the patch only: 98 tests.  Stock Cog: 72 pass, 7 fail, 18
  errors, 1 timeout.  Our VM: 73 pass, 7 fail, 18 errors.
- Result with `fixes/terryc321-pharo-advent-of-code.st`: 96 of 96 pass on the
  two VMs.  The causes of the 26 tests, each from the sources:
  - `String>>asSymbolOrInteger` is in the package `AdventCode`, which cannot
    load with `Advent-Of-Code-2015` (11 tests of `AOC2015Day7Test`).  The
    fixes file adds the method as an extension of `Advent-Of-Code-2015`.
  - `AOC2015Day13>>parse:` and `AOC2015Day15>>split:` split a text at
    `Character lf`.  The text is a string literal of a method, and the load
    gives the literal CR line ends (5 tests).  The two methods now use
    `String>>lines`.
  - `AOC2015Day8>>repoDir` is a directory in the home directory of the
    author.  The input file is in the repository.  The method now asks the
    Iceberg repository for its directory.
  - `AOC2025Day6>>solution1Part2` and `solution2Part2` used the result of
    `puzzleInputParsed2` in a form that a later change of the author broke.
  - Tests that cannot pass as written: a test with the number of another
    day, a test that calls a selector that the class does not have, a test
    that asserts in a forked process and ends with `true = false`, and a
    test that fails on purpose (`AOC2016Day1Test>>testFailing01`).  Each
    test now makes the check that its name and its comment describe.
  - `AOC2015DayTestTemplate` is a template to copy.  Its two tests call
    `notYetImplemented`.  The class is now abstract, so the test count is 96.
  - `AOC2015Day25Test>>testPart1` makes 18 million steps of a Generator.
    The test took 7.5 s on our VM and more than the 10 s default time limit
    on stock Cog.  The test class now has a time limit of 2 minutes.

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
- Result with the repairs of this section: 337 of 337 pass on the two VMs.
  Stock Cog ends the class list in 12 seconds.
- Second defect: `MTCITonelJsonExporterTest>>testWorking` starts with
  `self skip` since 2024-06-27 ("a lots of strong dependencies").  The test
  needs the Iceberg repository of mutalk and its clone, and the package run
  has the two.  The code under test names `RPackageOrganizer`, which Pharo
  14 does not have.  The fixes file takes the skip out and changes the class
  name to `PackageOrganizer`.
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
    tests now set `requiresSchema: true`.
- Result with the repairs: 6027 of 6027 pass on the two VMs.  The HTTP tests
  ran on the two VMs.

## PharoMisc (`bouraqadi/PharoMisc`, row 6)

- Defect of our manifest: the row loaded the baseline `PharoMisc` only.
  That baseline has one class and no test.  The repository has one baseline
  for each of its projects.
- Repair: the row now loads nine baselines (`PharoMisc`, `StateMachine`,
  `Concurrency`, `BooleanExpressions`, `BaselineAnalyzer`, `CodeMetrics`,
  `LightweightObserver`, `PharoExtra`, `SimpleMiddleware`).  Not in the row:
  `Tasks`, `NetworkExtras` and `EasyUI`, whose baselines name the repository
  without its `Pharo` directory.
- State before the repairs: 225 tests, 160 pass, 41 fail, 24 errors, the
  same on the two VMs.  The branch of the package is `pharo12`: the code is
  for Pharo 12.
- Result with `fixes/bouraqadi-pharomisc.st`: 225 of 225 pass on the two VMs.
- CodeMetrics (`CmCodeAnalyzerTest`, 5 errors): `CmCodeAnalyzer` asks the
  global `SystemOrganizer` for category names and for the classes of a
  category.  Pharo 14 has no such global.  The two methods now read the
  package tags (`PackageTag>>categoryName`).  The tests need a category with
  no class, which a Tonel repository cannot hold, so `setUp` makes the tag
  and `tearDown` removes it.
- LightweightObserver (57 tests).  The fixes file has a port to Pharo 14:
  - `LoSubject class` wrapped each method that writes an instance variable
    through the Pharo 12 hook
    `ClassDescription>>addAndClassifySelector:withMethod:inProtocol:`.  Pharo
    14 installs a method with `addSelectorSilently:withMethod:`, and the
    port puts the hook there.
  - The source of a wrapper method is the source of its basic method.  The
    Pharo 14 compiler keeps the generated source in the `#source` property
    of the wrapper, and a file-out of the class then had the generated
    source (`testFileOutIngnoresGeneratedMethods`).
  - The tests made their classes with the ST80 class creation messages of
    `ClassFactoryForTestCase`.  They now use the class builder.
  - `Array>>asSubject` sends `becomeForward:` to the receiver.  A literal
    array is read-only in Pharo 14, so the method makes the receiver
    writable first.
  - `LoSubjectSortedCollection>>addLast:` came from a trait and called
    `SortedCollection>>addLast:`, which is `shouldNotImplement`.
  - Three presenter classes are Spec 1 code.  The port has Spec 2 versions.
- SimpleMiddleware (`SmSingleMiddlewareTest>>testCommunication`,
  `ConnectionClosed` after 8 ms): the test uses the default port of
  `SmServer`, 54321.  On the test machine another program listens on
  `127.0.0.1:54321`.  `SmServer` listens on the wildcard address with the
  address reuse option, and macOS lets that bind go through.  The proxy
  connects to `127.0.0.1:54321`, and the other program gets the connection
  and closes it.  The test now asks the system for a free port.  The change
  for the package: do not use a fixed port in a test.
- Three more baselines (`Tasks`, `NetworkExtras`, `EasyUI`) name the
  repository as `github://bouraqadi/PharoMisc:pharo12`, and the code is in
  the directory `Pharo`.  `patches/bouraqadi-pharomisc.patch` adds `/Pharo`
  to that repository line in the three baselines, and the row now loads
  them: 361 tests.
- NetworkExtras, multicast sockets.  13 tests failed on our VM only.  They
  showed a defect of our VM: the socket option primitives knew 6 option
  names and gave a false success for each other name
  ([vm-compat-bugs.md](../vm-compat-bugs.md)).  The tests also showed these
  defects of the package:
  - Two sockets on one port.  The tests make the sender as a second
    `NeMulticastSocket` on the port of the receiver, and
    `NeMulticastSocket>>group:port:` always binds the port.  The stock VM
    does not report the failed bind of the second socket
    (`scripts/cogdiff/socket_bind_in_use.st`), so the sender stayed unbound
    there with no error, and the tests passed.  Our VM reports
    `Address already in use`.  The fixes file adds a socket that only sends
    (`NeMulticastSocket class>>sendToGroup:port:`: no bind, no group
    membership), and the tests use it for the sender.  A first repair (the
    reuse options before the bind) was wrong: three sockets then listen on
    one port, and the system gives a unicast datagram to only one of them.
  - `NeUdpUnicastSocketTest>>testCloseWhileReceiving` sends a datagram,
    closes the receiver, and expects that the reader gets no data.  The
    datagram is in the buffer of the receiver before the close, so the
    reader can get it.  The test passed on the stock VM in each run and
    failed on our VM in some runs.  The test now sends no datagram: a reader
    that waits gets `ConnectionClosed` when the socket closes.
  - `NeMulticastSocketTest>>testInterface` was an expected failure that ends
    with `self fail: #todo`.  The fixes file has the finished test and the
    two accessors of `IP_MULTICAST_IF` that the test needs.
- Tasks, `TkThreadTest>>testPeriodFixedDuration`: the cause is a defect of
  the Pharo image, `Duration>>asDelay`.  See
  [pharo-image-repairs-2026-11-17.md](pharo-image-repairs-2026-11-17.md).
- Result of the row with 12 baselines: 361 of 361 pass on the two VMs.
- Open, in stock Cog: a segmentation fault in its garbage collector.  The
  fault came with the first repair of the multicast tests (the reuse
  options, three sockets on one port).  Stock Cog stopped in
  `TkThreadTest>>testFinalization`, in 3 runs of 3.  The fixes file of commit
  `5053c46f` gives that state.  With the repair of this section the fault
  does not show.  The cause is not known.

## Chanel (`jecisc/Chanel`, row 13)

- State before: 369 tests, 329 pass, 40 errors on the two VMs.
- Result with `fixes/jecisc-chanel.st`: 369 of 369 pass on the two VMs.
- 33 errors (`OCCompilationError`, undeclared variable): the tests compile
  method sources that use the names `test`, `test2`, `test3`, `test4`,
  `notTest` and `x` with no declaration.  `Behavior>>compile:` of Pharo 14
  does not permit an undeclared variable.  The helpers of
  `ChanelAbstractCleanerTest` that make a class or a trait now give the
  names as instance variables.  The change for the package is the same.
- 6 errors (`ByteSymbol>>#parent:` not understood): the extension
  `OCMessageNode class>>superSendTo:` of Chanel gives a Symbol to
  `selector:`.  `OCMessageNode>>selector:` of Pharo 14 takes an
  `OCSelectorNode`.  The cleaner `ChanelEnsureSuperIsCalledCleaner` itself
  has the defect, not only its tests.
- 1 error (`testRenameTestCaseEndingWithTestsUpdateReferences`): a defect of
  the Pharo image, `RBNamespace>>renameClass:to:around:`.  See
  [pharo-image-repairs-2026-11-17.md](pharo-image-repairs-2026-11-17.md).

## Fame (`moosetechnology/Fame`, row 16)

- State before: 273 tests, 271 pass, 2 skip on the two VMs.  The two tests
  of `FMLazyRelationTest` start with `self skip` and the mark
  `#currentLimitation`.
- Result with `fixes/moosetechnology-fame.st`: 273 of 273 pass on the two
  VMs.
- `testManyToOneWithlazyOne`: the fixture method `RPGLazyHero>>talismans:`
  assigns the slot (`talismans := anObject`).  `FMMany` then stores the
  Array and does not update the other side of the relation.  The method now
  sends `value:` to the slot, as `RPGLazyHero>>kills:` does.
- `testOneToOneRelationReverse`: `FMLazyOne>>materializeFor:slot:` loads a
  value whose inverse slot has another object from a later write, and the
  old lazy value replaces that write.  The method now drops the loaded value
  in that case.  The owner of the package can prefer another design.

## Ficus (`tinchodias/ficus`, row 17)

- State before: 77 tests, 75 pass, 1 skip, 1 expected failure on the two
  VMs.
- Result with `fixes/tinchodias-ficus.st`: 77 of 77 pass on the two VMs.
- `FiSetSimpleChangesTest>>testUpdate` (expected failure):
  `FiSet>>editFromSimpleChange:` answers a `FiSubEdit` for an update, and
  `FiSetUpdate>>edit` answers a `FiInclude`.  The test
  `FiSetChangesTest>>testUpdate` passes and needs the `FiInclude`.  The
  converter now
  answers the same edit, and the mark `<expectedFailure>` goes.
- `FiBuilderTest>>testErrorWhenSettingAnExcludedVariable` (skip, "Decided to
  optimize and remove this validation"): the check of the excluded instance
  variable names is back in `FiFixedObject>>initializeFromStepsAndValues:`,
  and the skip goes.  The owner of the package can delete the test in place
  of that.

## CSSParser (`pharo-contributions/CSSParser`, row 20)

- State before: 80 tests, 78 pass, 2 skip on the two VMs.  Two tests of
  `CSSFontFormatterTest` start with `self skip:` ("probably due to use of a
  non existing font in the CI").
- Cause: the two tests compare the font family with `TextStyle named:` for
  the fonts Verdana and NimbusRomanNo9L.  `TextStyle named:` answers nil on a
  machine that does not have the font, and the formatter then keeps
  `CSSFontFormatter defaultStyle`.  One test also compares a Float size with
  the Integer that the formatter stores.
- Repair here: `fixes/pharo-contributions-cssparser.st`.  The expected value
  is the font, or the default style of the package when the machine does not
  have the font.  The expected size is an Integer, as in the formatter.
- Result: 80 of 80 pass on the two VMs.

## JavaScriptGenerator (`feenkcom/JavaScriptGenerator`, row 24)

- State before: 45 tests, 43 pass, 2 fail on the two VMs.
- Cause: two tests of `JSGExtensionsTest` compare the output for a
  Dictionary and for a Set with a fixed string.  The string has the elements
  in the order of the image of the author.  The two collections enumerate in
  hash order.
- Repair here: `fixes/feenkcom-javascriptgenerator.st`.  The tests compare
  the elements and not their order.
- Result: 45 of 45 pass on the two VMs.

## pharo-beacon (`pharo-project/pharo-beacon`, row 29)

- State before: 20 tests, 17 pass, 3 errors on the two VMs
  (`CompiledBlock>>#asRingDefinition` not understood).
- Cause: `MethodStackSignal>>currentStack` sends `method asRingDefinition`
  to each context.  `Context>>method` of Pharo 14 answers the `CompiledBlock`
  for a block context, and only `CompiledMethod` has `asRingDefinition`.
- Repair here: `fixes/pharo-project-pharo-beacon.st` uses
  `Context>>homeMethod`.
- Result: 20 of 20 pass on the two VMs.

## ParametrizedTests (`tesonep/ParametrizedTests`, row 32)

- State before: 139 tests, 136 pass, 3 errors on the two VMs.
- Cause: Pharo 14 has this package in `SUnit-Core` and `SUnit-Tests`, in a
  newer version.  The load puts the old `ParametrizedTestMatrix>>addCase:`
  in place of the method of the image.  Three tests of `PaSuiteTest` that
  only Pharo 14 has then fail (`ShouldBeImplemented`, `#key`).
- Repair here: `fixes/tesonep-parametrizedtests.st` puts the Pharo 14 source
  of the method back.
- Result: 139 of 139 pass on the two VMs.
- For the owner of the package: the package is a part of Pharo 14, and a
  load of it on Pharo 14 replaces newer code.

## Sauco (`tinchodias/Sauco`, row 36)

- Defect of our manifest: the row loaded the packages `Sauco-Core` and
  `Sauco-Tests` only.  `SaAdaptiveLabelsTest` (in `Sauco-Tests`) uses
  `SaReport>>asRSCanvas` of the package `Sauco-Roassal` (2 errors).
- Repair: the row loads the baseline with no package list.
- Result: 19 of 19 pass on the two VMs.
- For the owner of the package: in the baseline, `Sauco-Tests` requires
  `Sauco-Core` only.  The package needs `Sauco-Roassal` too.

## Mole (`ba-st/Mole`, row 37) and Hyperspace (`ba-st/Hyperspace`, row 15)

- The two packages load Buoy.  See the Buoy section: the files
  `locks/ba-st-mole.txt` and `locks/ba-st-hyperspace.txt`.
- Mole: 74 of 74 pass on the two VMs.
- Hyperspace also loads Zinc from GitHub, and the image has Zinc.  The load
  stopped at a dialog ("merge or load") until the time limit.  The package
  run now answers that warning with "load" (`headless_load` in
  `run-one-package.sh`).
- Hyperspace, `ZnUrlHyperspaceExtensionsTest>>testStartLimit`: the test
  expects that one URL is equal to `?limit=10&start=1` and to
  `?start=1&limit=10`.  See "URLs with the same query fields in another
  order" below: the defect is in the image, and image fix 34 repairs it.
  The package has no fixes file any more.
- Hyperspace result: 197 of 197 pass on the two VMs, with image fix 34 and
  no fixes file.

## PharoWiki (`chicoary/PharoWiki`, row 26)

- Defect: the directory `src` has the file `.filetree` and no file
  `.properties`.  The code is in Tonel format, and Iceberg finds no package.
- Effect: the load stops with `KeyNotFound: key 'BaselineOfPharoWiki'`.
- Repair: `patches/chicoary-pharowiki.patch` adds `src/.properties` with the
  Tonel format and removes `src/.filetree`.
- State with the patch: the load works.  77 tests, 70 pass, 7 skip on the two
  VMs.
- 2 skips (`PWikiGeneratorRegistryTest`): the tests skip when the Epicea log
  of the session has no class addition, which is the state of a new image.
  The tests now install an Epicea monitor with one class addition for the
  time of the test.
- 5 skips (`PWikiGeneratorTest`): each test starts with `self skip`, with
  no reason in the source.  The tests make the page of `Point` with a scan
  of the image for the senders and implementors of each selector.
  `fixes/chicoary-pharowiki.st` gives `PWikiGenerator` a setter for its
  extractor, the tests use an extractor with no scan, and the skips go.
- Result: 77 of 77 pass on the two VMs.

## Launchpad (`ba-st/Launchpad`, row 14)

- The package loads Buoy.  See the Buoy section
  (`locks/ba-st-launchpad.txt`).
- State with the load repair only: 123 tests, 48 pass, 1 fail, 74 errors on
  the two VMs.
- Cause of 71 tests: Launchpad calls `CommandLineArguments>>copySubcommand`,
  `CommandLineArguments>>optionAt:ifPresent:ifAbsent:` and
  `CommandLineHandler class>>prepareSubcommand:`.  Pharo 13 had the three
  methods.  Pharo 14 has a new design of the command line handler and does
  not have them.  The repository has its own copies of the methods for
  GemStone.
- Cause of 2 tests: `BaselineOfLaunchpad` has branches for `pharo10.x` to
  `pharo13.x` only, so the package `Launchpad-Tracing-Fuel` does not load on
  Pharo 14, and its test class does.
- Repair: `patches/ba-st-launchpad.patch` adds a package
  `Launchpad-Pharo14-Compatibility` with the three methods and a
  `pharo14.x` branch in the baseline.
- Cause of 1 test: three helper methods of Buoy
  (`OSPlatform>>environmentAt:put:` and two more) call the deprecated
  `OSPlatform>>environment`.  `fixes/ba-st-launchpad.st` makes them use
  `OSEnvironment current`.  The same change belongs in Buoy.
- Result: 123 of 123 pass on the two VMs.

## linear-models (`pharo-ai/linear-models`, row 41)

- State before: 52 tests, 49 pass, 1 error, 2 skip on the two VMs.
- The error (`Cannot locate any of #('liblapack.dylib')`):
  `LapackLibrary>>macLibraryName` of the dependency `pharo-ai/lapack` asks
  `FFIMacLibraryFinder`, which accepts a path only when a file is there.
  Since macOS 11 the system libraries are in the shared cache of the dynamic
  loader and have no file.  The loader opens `/usr/lib/liblapack.dylib` on
  the two VMs.  `fixes/pharo-ai-linear-models.st` makes the method answer
  that path when the finder finds no file.
- The 2 skips: the two tests of `AILinearRegressionLeastSquaresVanillaTest`
  start with `self skip` ("least squares vanilla is still broken").  The
  fixes file gives the tests their bodies back, and the two tests pass.
- Result: 52 of 52 pass on the two VMs.
- For the Pharo image: `FFILibraryFinder>>findAnyLibrary:` has the same file
  test for each system library on macOS 11 and later.

## TSF-Scheduler, TSF-FileRotator, TSF-Logger, TSF-NexIO (`georghagn`, rows 51, 62, 66, 135)

- The four repositories do not exist as public repositories.  Measured on
  2026-11-17:
  - `git ls-remote https://github.com/georghagn/<name>.git` answers
    "Repository not found" for each of the four names.
  - `https://api.github.com/users/georghagn/repos` answers an empty list:
    the account has no public repository.
  - A search of GitHub for a repository with one of the four names
    (`search/repositories?q=<name>+in:name`) finds no copy and no fork.
- GitHub asks for a name and a password for a repository that does not
  exist, and Iceberg opened a dialog in the headless image.  The package run
  now uses the non-interactive credentials provider of Iceberg, so the load
  stops with an error (state `LOAD-FAIL`).
- These four rows cannot run on any machine: there is no source to load.
  They are the only rows that the run leaves out, and the reason is the
  measurement above.  If the owner of the repositories makes them public
  again, the rows run with no change.

## pharo-newfile (`ronsaldo/pharo-newfile`, row 50)

- State before: stock Cog 42 of 42 pass.  Our VM: 34 pass, 8 skip.  The 8
  tests of `NewFileTest` start with
  `NewFile primitiveIsAvailable ifFalse: [ ^ self skip ]`.
- Cause: a defect of our VM.  The package calls the plugin `NewFilePlugin`
  of the stock VM, and our VM did not have the plugin.
- Repair: `src/vm/plugins/NewFilePlugin.cpp` (20 primitives, the POSIX
  side).  The package needs no change.
- Result: 42 of 42 pass on the two VMs.
- The tests of the package reach 14 of the 20 primitives.
  `scripts/cogdiff/newfile_plugin_checks.st` has 36 more checks (directories,
  closed handles, buffers that are not ByteArrays, wrong arguments).  Our VM
  and stock Cog give the same answer for 33 of them.  The 3 others are
  defects of the stock plugin:
  - A read of 3 bytes into a ByteArray of 2 bytes answers 3: the stock
    plugin writes past the buffer.  Our VM fails the primitive.
  - A read into an Array answers 2: the stock plugin writes bytes into the
    pointer slots of the Array.  Our VM fails the primitive.
  - A read into a read-only ByteArray answers 2.  Our VM fails the
    primitive.
- `scripts/cogdiff/newfile_plugin_ours_checks.st` is for our VM only.  Stock
  Cog stopped with `Abort trap: 6` in `primitiveClose:` when the first form
  of the script had the check that gives a copy of the handle of a closed
  file to the close primitive (the stock plugin keeps a C pointer in the
  handle).  Our VM keeps an id that is never used again, and fails the
  primitive.
- Found in the package, not repaired, and no test shows the defect:
  `NewFile>>close` does not release the memory views of the file, and sets
  the handle to null.  A later `unmap` of a view then gives a null handle to
  the primitive, and the mapping stays until the VM exits.  The stock VM
  has the same result.

## Comix2 (`Ducasse/Comix2`, row 40)

- State before: 113 tests, 110 pass, 2 fail, 1 skip on the two VMs.
- `CXXMLItemSaverLoaderTest>>testVisitorXML`: the expected string has the
  date 24 January 2026, and `CXVisitor` writes the date of the day.  The
  expected string also had LF line ends, and the visitor writes CR.  The
  test now makes the expected string with the date of the day and
  `String cr`.
- `CXXMLItemSaverLoaderTest>>testXMLImporter` ("Got 0 instead of 2"):
  `CXXMLImporter>>import:` makes each item and does not add it to the
  collection that it answers.  The method now adds each item.
- `CBXMLImporterTest>>testImportRealCollection` (skip): the test reads the
  file `BD.362.xml` from the image directory.  The file is the collection of
  the author and is not in the repository (`git ls-files` and the history
  have no such name).  The test now writes a file of 50 series with 20
  albums each in the same format and imports that file.
- Result: 113 of 113 pass on the two VMs.

## OSC (`Ducasse/OSC`, row 43)

- State before: 17 tests, 16 pass, 1 fail on the two VMs.
- `OSCParserTest>>testParseTimestamp` compares the time tag of a bundle with
  a second read of the clock, so the fraction bytes differ in each run.  The
  test also compares an Array with a ByteArray.  The test now checks that
  the time tag is between the time before and the time after the call.
- Found in the same code, with no failing test:
  `DateAndTime>>addLatency:` of the package adds to the nanoseconds with no
  carry.  In the last 50 milliseconds of a second the fraction is too large
  for its 4 bytes.  The fixes file makes the method add a Duration.
- Result: 17 of 17 pass on the two VMs.

## aws-sdk-smalltalk (`newapplesho/aws-sdk-smalltalk`, row 42)

- State before: 59 tests, 39 pass, 20 errors on the two VMs
  (`ConnectionClosed` to `127.0.0.1:8000` and `127.0.0.1:4566`).
- Cause: 20 tests are live tests.  They need DynamoDB Local on port 8000 (19
  tests) and an S3 emulator on port 4566 (1 test).  The class comments and
  the `docker-compose.yml` of the package say so.  The test machine runs
  neither, and no download of an emulator is permitted.
- Repair here: `fixes/newapplesho-aws-sdk-smalltalk.st` adds the class
  `AWSLocalEmulatorDouble`: two `ZnServer` instances in the image that
  answer the requests of the tests (tables and items with their state, and
  buckets).  The `setUp` of the three live test classes starts the two
  servers.  No assertion has a change.
- Limit: the 20 tests now check the client side (the request format, the
  JSON mapping, the response handling, the sockets and the HTTP server of
  the VM).  They do not check the request signature, and they are not a
  check against the real services.
- Result: 59 of 59 pass on the two VMs.

## opentelemetry-pharo (`Gabriel-Darbord/opentelemetry-pharo`, row 44)

- State before: the manifest row loaded the baseline with no group, and the
  image had no test class.  With the group `tests`: 1034 tests, 1033 pass,
  1 skip on the two VMs.  The group `integration-tests` (3 tests) was not
  loaded.
- The skip: `OTOtlpGrpcSpanExporterTest>>testExportUsesGrpcTransportStackEndToEnd`
  runs only when 9 classes of grpc-pharo are in the image.  One of them,
  `GRPCHTTP2MockStream`, is in the package `GRPC-Tests`.  The baseline of
  the package loads grpc-pharo with its default group, and that group does
  not have `GRPC-Tests`.
- The 3 integration tests send one gRPC call each to an OpenTelemetry
  collector on `127.0.0.1:4317`, and then read the file that the collector
  writes.  The test machine runs no collector.
- Repair here:
  - The manifest row loads the groups `tests` and `integration-tests`.
  - `fixes/gabriel-darbord-opentelemetry-pharo.st` defines
    `GRPCHTTP2MockStream` (a copy of the class of grpc-pharo v0.1.0).
  - The same file adds the class `OTLPGrpcCollectorDouble`: a receiver for
    gRPC on HTTP/2 in the image.  The receiver decodes each request and
    writes the JSON file that the collector writes.  The `setUp` of
    `OTOtlpGrpcIntegrationTest` starts the receiver and sets the environment
    variables that the tests read.  No assertion has a change.
- Limits: the receiver uses the HTTP/2 code of the GRPC package for the
  server side.  A defect that the reader and the writer of that code share
  does not show.  The receiver does not check the request headers or the
  rules of the OTLP data.  The 3 tests are thus a check of the client
  against a receiver in the image, and not against the real collector.
- Result: 1037 of 1037 pass on the two VMs.

## DatasetUtilities4Pharo (`reugalabf/DatasetUtilities4Pharo`, row 56)

- State before: 37 tests, 26 pass, 1 failure, 10 errors on the two VMs.
- Causes, all in the package:
  - `DatasetAggregator` calls `Number readFrom:` on the value of a row.  A
    row from JSON has a Number there, and `Number readFrom:` needs a String
    or a stream (2 errors).  The repair parses a String and passes a Number
    on.
  - `DictionaryAggregatorTest>>testSingleValueDataset` asserts 3 rows for a
    text with 4 data rows, and then calls `inspect` (1 failure).  The repair
    asserts 4 rows and the hourly minimum and maximum.  One more test ends
    with `inspect`; the repair removes that line.
  - `ObjectAggregatorTest` uses the classes `FlareSenseDevelopment` and
    `FlareSenseRawEndpointClient` (8 errors).  No repository of the run and
    no baseline has these classes.  The repair makes the two classes from
    the JSON text and the expected values of the tests.
- Limit: the two classes are stand-ins that we wrote.  The classes of the
  author can differ.  The author must add the classes to the test package,
  or add their project to the baseline.
- Result: 37 of 37 pass on the two VMs.

## PharoAthleteProject (`RomaShmyhelskyi/PharoAthleteProject`, row 61)

- State before: `NO-TEST-CLASSES`.  The load loaded only the baseline class.
- Cause: on the branch `main`, `BaselineOfProjectAthlete` has the method
  `baseline:` on the class side.  Metacello calls `baseline:` on an
  instance, so the load has no package.  The branch `master` has the method
  on the instance side, and the README names that branch.
- Repair here: the manifest row loads the branch `master`.
  `fixes/romashmyhelskyi-pharoathleteproject.st` repairs two defects of the
  package code on that branch:
  - `TennisPlayer>>addPrizeMoney:` added a prize and removed the last
    element at once, so the list of prizes stayed empty.  The repair removes
    the last element only when the list has more than 3.
  - `Footballer>>goalsScored:` refused 0, the value that `initialize` sets,
    and the error text joined a String and an Integer.
- Result: 34 of 34 pass on the two VMs.

## Conduit (`NathanFrund/Conduit`, row 64)

- State before: `LOAD-FAIL` (`KeyNotFound: key 'ConduitCore-Tests'`).
- Cause: the baseline names the package `ConduitCore-Tests`.  The
  repository has the package `Conduit-Tests`.
- Repair here: `patches/nathanfrund-conduit.patch` corrects the name in the
  two places of the baseline.
- Result: 14 of 14 pass on the two VMs.

## Geosphere (`akgrant43/Geosphere`, row 67)

- State before: 11 tests, 6 pass, 5 failures on the two VMs.
- Causes, all in the tests of the package:
  - 4 tests compare Floats with `assert:closeTo:`.  The expected values have
    4 or 5 decimal places, and Pharo 14 compares with a precision of
    1.49e-8.  The repair gives each comparison its precision (0.0001 for
    degrees, 0.01 for metres).
  - 1 test expects the text `0.0` for the seconds.  The code prints 2
    decimal places (`0.00`).
- Repair here: `fixes/akgrant43-geosphere.st` (5 test methods).
- Result: 11 of 11 pass on the two VMs.

## anthropic-sdk-pharo (`punt-labs/anthropic-sdk-pharo`, row 75)

- State before: 932 tests, 883 pass, 1 failure, 43 errors, 3 skips, 2
  timeouts on the two VMs.
- Cause: 46 tests of 16 integration test classes call the real service
  `api.anthropic.com`.  `ClaudeSDKExampleSupport class>>resolveClient` reads
  the key from `ANTHROPIC_PHARO_API_KEY`, `ANTHROPIC_API_KEY` or the
  keychain.  The shell that started the run had `ANTHROPIC_API_KEY`, so the
  tests sent requests with that value.  The service answered "invalid
  x-api-key".  The two timeouts are the same answer on the streaming socket:
  the client waits for a line end that does not come.
- What we changed in our tools because of this row: the VMs of the package
  run start with an environment that has no key or token of the user
  (`scripts/pkg-jit-test/run-one-package.sh` and `scripts/vmrun.sh`).
- Repair here: `fixes/punt-labs-anthropic-sdk-pharo.st` adds the class
  `ClaudeAnthropicDouble`: a `ZnServer` on `127.0.0.1` that accepts one fixed
  test key and answers the requests of the tests (models, messages, token
  counts, streaming, files, skills).  `resolveClient` answers a client for
  that server.  Three tests that opened a TLS socket to the real host take a
  plain socket to the server.  No assertion has a change, no real key is
  read, and no request leaves the machine.
- Limit: the tests now check the client code of the package against a
  server in the image.  The server does not run a model, and it is not a
  check against the real service.
- 2 skips were tests of `isAvailable` that could not fail.  They now assert
  that the answer agrees with a search of `PATH` for the tool.
- Not repaired, and the owner must decide: `PharoKeyringIntegrationTest`
  skips on macOS because its guard asks `isUnix`.  With the right guard the
  test writes a secret into the login keychain of the user and removes the
  secret in `tearDown`.  We do not write into a keychain without the word of
  the owner.
- Result: 932 tests, 931 pass and 1 skip (the keychain test) on the two
  VMs.  The run had the environment with no key.

## FAST-Java (`moosetechnology/FAST-JAVA`, row 76)

- State before: 914 tests, 902 pass, 1 failure, 8 errors, 3 skips on the two
  VMs.
- Causes:
  - 9 tests: the dependency FAST (branch `v3`) sends `#children`,
    `#childrenSelectors` and `#parentSelectors`.  Famix renamed these methods
    (`#containedEntities`, `#containedEntitySelectors`,
    `#containerSelectors`).  The old names are only in the package
    `Famix-Deprecated`, and the baseline of FAST-Java does not load that
    package.
  - 2 skips: `JavaSmaCCParamsMethodTest>>testBlockScope` and `testScope`
    start with `self skip`, and their code was a comment that calls
    `#fastScope`, a method that does not exist.  The scope code is now in
    `FASTJavaLocalResolverVisitor`.
  - 1 skip: `JavaSmaCCReturnAttributeAccessTest>>testReturnExpressionType`
    (skipped in 2023 "to let CI pass") expects `FASTJavaQualifiedName` for
    `a.b` in a method.  The importer makes a `FASTJavaClassProperty` there,
    and two other tests of the package assert that.
- Repair here: `fixes/moosetechnology-fast-java.st`.  6 methods of FAST use
  the new names.  The 3 tests have a body for the code as it is.
- Result: 914 of 914 pass on the two VMs.

## Hera (`koendehondt/hera-for-pharo`, row 78)

- State before: 672 tests, 582 pass, 4 failures, 86 errors on the two VMs
  (`UndeclaredVariableRead: HeraSyntaxDarkTheme`).
- Cause: the commit `6bf7551` of the package uses the classes
  `HeraSyntaxLightTheme` and `HeraSyntaxDarkTheme`.  No file of the
  repository defines them.  `HeraSettings>>syntaxTheme` reads the dark theme
  class when the UI theme of the image is dark, as in the image of the run.
- Repair here: `fixes/koendehondt-hera-for-pharo.st` makes the three theme
  classes (`HeraSyntaxTheme` and the two subclasses) with the six colours
  that `HeraScriptGenerator` asks for.  The light colours are the values in
  the expected texts of the tests.  Each test of `HeraUnitTestCase` runs with
  the light theme, because the tests expect the light colours.
- Limit: the dark colours are ours (the GitHub dark palette).  The author
  must add the class files to the repository.
- Result: 672 of 672 pass on the two VMs.

## PolyMath (`PolyMathOrg/PolyMath`, row 80)

- State before: 777 tests, 776 pass, 1 error on the two VMs
  (`PMKDTreeTest>>testBenchmark`, `UndeclaredVariableRead:
  PMKDTreeBenchmark`).
- Cause: the test uses a class of the package `Math-Benchmarks-KDTree`.  The
  baseline has that package only in the group `Benchmarks`, and the group
  `Tests` does not ask for it.
- Repair here: the manifest row loads `Math-Benchmarks-KDTree` too.
- Result: 777 of 777 pass on the two VMs.

## MCP (`Evref-BL/MCP`, row 82)

- State before: 1167 tests.  Stock Cog: 947 pass, 188 failures, 31 errors, 1
  skip.  Our VM: 946 pass, 189 failures.
- Cause of 218 tests: the manifest row loaded the groups `Core` and `Tests`.
  The helper selectors that the tests need (`mcp...`) are in the package
  `MCP-IDE-Compatibility-From12`, and the baseline has that package only in
  the group `IDE`.
- Repair here: the manifest row loads `Core`, `IDE` and `Tests`.
- Result: stock Cog 1166 pass and 1 skip.  Our VM 1165 pass, 1 failure and
  1 skip.
- The failure on our VM only:
  `MCPToolContractsTest>>testEvaluateStopsAnEndlessRecursionWithinASecondOrTwo`.
  The evaluator of the package stops an endless recursion at a depth of
  1000000 frames.  Our VM gives its own error at 56000 frames first.  This
  is a defect of our VM (`docs/vm-compat-bugs.md`, the first entry), and the
  repair is in work.
- The skip: the test needs the World to step the Transcript.  Pharo 14 does
  not do that any more (`WorldState>>runStepMethodsIn:` does not send
  `stepGlobal`), so the test cannot apply on this image.

## mongotalk (`pharo-nosql/mongotalk`, row 88)

- State before: 88 tests, 23 pass, 65 errors on the two VMs.
- Cause: the 65 tests need a MongoDB server on `127.0.0.1:27017`.  41 tests
  stop in `setUp` with `ConnectionClosed`.  24 tests stop in `tearDown`,
  because `setUp` did not make the database object.  The test machine runs
  no MongoDB, and we install none.
- Repair here: `fixes/pharo-nosql-mongotalk.st` adds a stand-in for the
  server in the image (the classes `MongoServerDouble`,
  `MongoDoubleCollection`, `MongoDoubleMatcher`, `MongoDoubleBSON` and
  `MongoDoubleError`).  The stand-in listens on `127.0.0.1:27017`, reads the
  wire messages `OP_MSG` that the client sends, and keeps databases and
  collections in memory.  The `setUp` of the five test classes starts the
  stand-in first.  No assertion has a change.
- Three test defects that the missing server hid:
  - `MongoTest>>testAddExistingCollection` and
    `MongoAuthTest>>testSCRAMSHA1AuthMechanism` start with `self skip`.  The
    two tests run now.  The stand-in answers the error for a collection that
    exists, and has the server side of the login `SCRAM-SHA-1`.
  - `MQQueryTest>>testWhere` sends the JavaScript text `this.key1 =
    "value1"`.  A single `=` is an assignment, so a real server matches each
    document.  The repair writes `==`.
- Limits: the tests are a check of the client of the package against a
  server in the image, and not against MongoDB.  The stand-in has only the
  commands and the query operators that the tests use.  It has no
  JavaScript, no aggregation, no transactions and no check of access rights.
- Result: 88 of 88 pass on the two VMs.

## Iris (`koendehondt/iris-for-pharo`, row 96)

- State before: 131 tests, 128 pass, 2 failures, 1 error on the two VMs.
- Cause: the 3 tests are in `IrisExampleTest`, a class with one test that
  passes, two that fail and one that signals an error.  The class is data:
  `IrisMCPRunTestsToolTest` runs it and asserts the text of the report
  ("1 passed, 2 failed, 1 errored" and the three messages).  The package run
  listed the class as a test class.
- Repair here: `fixtures/koendehondt-iris-for-pharo.txt` names the class as
  a fixture.  Its 4 tests leave the list.
- Result: 127 of 127 pass on the two VMs.

## pharo-gsoc2025 (`Pakarati/pharo-gsoc2025`, row 97)

- State before: 1456 tests, 1455 pass, 1 failure on the two VMs
  (`DTTestCoverageTest>>testDTTestCoverageResultHas2Nodes`, "Got 2 instead
  of 3").
- Cause: `DTTestCoverageResult>>buildTreeForUI` of the package makes 2
  nodes, and the name of the test says 2.  The assertion says 3.  The first
  commit of the repository has the same code and the same assertion.
- Repair here: `fixes/pakarati-pharo-gsoc2025.st` corrects the assertion.
- Result: 1456 of 1456 pass on the two VMs.

## Carrefour (`moosetechnology/Carrefour`, row 85)

- State before: `LOAD-FAIL` (`Class not found: #FAMIX2JavaVisitor`).
- Cause of the load failure: no baseline of the chain loads the Famix Java
  metamodel.  FAST-Java loads the Famix groups `Core` and `Basic` only, and
  the baseline of Famix2Java has no dependencies.  The binder also needs
  TinyLogger.
- Repair of the load: the manifest row loads Famix with the group
  `ModelJava`, then TinyLogger, then Carrefour.
- State after the load: 77 tests, 63 pass, 9 failures, 5 errors on the two
  VMs.  The package and its tests are for the Famix Java metamodel of 2023.
  Causes, each found with a probe:
  - A defect of Famix (development branch):
    `FamixJavaMethod>>isConstructor` answers `false` for each method.  The
    binder keeps an invocation of a `new` expression only when the candidate
    is a constructor, so no constructor call was bound (9 tests).  The
    repair answers true when the kind of the method is `#constructor`, as
    `FamixTHasKind>>isConstructor` does.
  - The JSON model of the tests (`CRFTest>>famixModelString`) has the
    properties of 2023.  Famix removed `children`, `declaredType` as a
    property, the element `ParameterType` and others.  The repair migrates
    the model: typing elements for 84 declared types, `TypeParameter`,
    `typeParameters`, `referredEntity`, and `candidates` of an access.
  - Carrefour, FAMIX2Java and FAST (branch `v2`) call selectors that Famix
    renamed (`atScope:`, `allAtScope:`, `children`, `childrenSelectors`,
    `parentSelectors` and others).  The repair uses the new names.
  - 2 tests call the setter `declaredType:` that Famix removed.  They make a
    typing element now.  3 tests look for an entity by a name with `::`
    between the package and the class.  Famix writes a period there now.
- Repair here: `fixes/moosetechnology-carrefour.st`.
- Result: 77 of 77 pass on the two VMs, in a run from a new load.
- For the owners of the packages: the defect of
  `FamixJavaMethod>>isConstructor` is in Famix.  Carrefour needs a release
  for the present Famix.

## EscapeAnalysisPharo (`fouziray/EscapeAnalysisPharo`, row 57)

- State before: the two VMs stopped in the middle of the row.  Three
  `1 halt` statements were in the code and in the tests, and a `Halt` with
  no handler ends a headless image.  Of the 80 tests that ran, 62 passed.
- The package is a research prototype of an escape analysis for Pharo
  methods.  An object escapes a method when a reference to it can stay
  after the activation ends.
- Repairs here (`fixes/fouziray-escapeanalysispharo.st`, 33 entries; the
  manifest row also loads `tomooda/HoneyGinger`, which 4 tests use and no
  baseline names):
  - The three `1 halt` statements are gone.  The runner now also records a
    `Halt` as an error of the test.
  - Three tests use the AST API of an older Pharo (`selector:` with a
    String, `loadProject:`).
  - The analyzer (`EscapeAnalyzerVisitor`) answered "escapes" for objects
    that do not escape.  The causes, each shown with a probe:
    - A variable in a block was an escape, also in a block that the compiler
      inlines (`ifTrue:`, `timesRepeat:`).
    - A recursive send made a new context down to the depth limit, and the
      limit marks the receiver as an escape.
    - A store into an instance variable was an escape at once, also when the
      object that gets the value does not escape.
    - The result of a message was taken for the receiver of the message.
    - The flag "the result is returned" stayed set after the last return of
      the root method.
    - A primitive that only reads its receiver (`ProtoObject>>class`) was an
      escape.
    - The name `self` of a callee was looked up in the context of the
      caller.  That tied objects together that have no relation, and one
      test made 21779 contexts.
  - Two tests had wrong expected values (a method name copied from another
    test, and a count of contexts).  One test passed before only because its
    list was empty; the code that it runs is repaired.
  - `ObjectInlining>>assignObjectCreatedInlineToTmp:` was a stub.  It is
    written now, as small as its test needs.
- Result: 82 tests, 81 pass and 1 skip on the two VMs, in a run from a new
  load.
- Open: `EscapeTest>>testComplexWeighterGraph`.  The author skips the test
  (`self skip`, with the flag `#false`): the analyzer says that two objects
  escape, and the test says that they do not.  The test is right by the
  definition.  A repair must analyse the body of a block at the place where
  the block is made.  That change gives another answer for three tests of
  the same class (`testTempInBlock`, `testReturnTempInBlock`,
  `testArgBlockReturnsTemp`), which assert the careful answer.  The author
  of the package must decide which answer is right.  The test is not
  accepted: the row is not clean.

## Famix (`moosetechnology/Famix`, row 73)

- State before: 1477 tests, 1472 pass, 4 skips, 1 expected failure on the
  two VMs.
- Causes:
  - `FamixMetricsTest>>testLCOM2` and `testLCOM3` start with `self skip`.
    Without the skip: `FamixStClass` does not understand `lcom2`.  The
    generator of the Smalltalk metamodel
    (`FamixPharoSmalltalkGenerator>>defineHierarchy`) does not give the class
    the trait `FamixTLCOMMetrics`; the Java generator does.  The last line
    of `testLCOM2` also asks for `lcom3` where it means `lcom2`.
  - `testMartinCohesionFamixPackage` starts with `self skip`.  Without the
    skip the first value is 5.0, and the test expects 1.0.
    `FamixTCohesionCouplingMetrics>>martinCohesion` counts the classes in
    the same namespace.  In Smalltalk all classes are in one namespace, so
    classes of other packages count too.  The expected values of the test
    agree with the definition (H = (R + 1) / N for the classes of the
    package).  The Java package class has the same method with
    `FamixTPackage`.
  - `FamixTest2ModelTest>>testRootUniqueness` is a skip for an old issue.
    The test of the superclass passes for the class.
  - `FamixPackageNavigationChefTest>>testStaticProviderPackages` has the
    mark of an expected failure.  Its expected set lacks one package that
    the sibling test has.
- Repair here: `fixes/moosetechnology-famix.st` adds the trait in the
  generator and in the class, uses `FamixTPackage` in `martinCohesion`, and
  corrects the four tests.
- Result: 1477 of 1477 pass on the two VMs, in a run from a new load.
- For the owners of Famix: the two code defects are in Famix.

## ReStore (`rko281/ReStoreForPharo`, row 74)

- State before: 2358 tests.  Stock Cog: 2355 pass, 1 failure, 1 error, 1
  expected failure.  Our VM: 2356 pass, 1 failure, 1 expected failure.
- Causes:
  - `SSWReStoreTypeTest>>testVeryLargeString` (error on stock Cog only).
    The dependency Pharo-SQLite3 gives the bytes of a text to
    `sqlite3_bind_text` with the destructor `SQLITE_STATIC`.  With that
    value SQLite keeps the pointer and reads the bytes at the step.  The
    bytes are a ByteArray that nothing holds after the call.  The test
    allocates 10 MB between the bind and the step, the collector of Cog uses
    the memory again, and SQLite stores other bytes.  Our VM did not use the
    memory again in that time.  The defect is in Pharo-SQLite3, and each
    text bind has it.  The repair gives `SQLITE_TRANSIENT`, so SQLite makes
    a copy.
  - `SSWReStoreBlockIfNilQueryTest>>testSelectSimpleObjectWithFunctionIfNil`
    has the mark of an expected failure, and the test passes (an unexpected
    pass is a failure).  The mark goes.
  - `SSWReStoreGeneralTest>>test01Events` (expected failure).  The test
    needs the events of a list model of Dolphin Smalltalk.  The Pharo port
    uses an OrderedCollection, which sends no event.  The test now sends the
    event that the list model sends.  The mark goes.
  - `SSWReStoreDefaultWriteTest>>testRefreshExistingObjects` failed on
    stock Cog in one run of two.  ReStore keeps the objects that it reads in
    a cache with weak values.  The test asserts that an object still shows
    an old value, and the address of the object is a proxy that loads
    later.  When the collector runs between the setUp and the test, the
    object is read again and its proxy loads the new value.  With a process
    that forces a scavenge each millisecond the test failed in 13 of 30 runs
    on stock Cog.  The repair reads the value before the other session
    writes.  A sibling test gets the same line.  After the repair: 0
    failures in 30 such runs on each VM.
- Repair here: `fixes/rko281-restoreforpharo.st` (7 entries).
- Result: 2358 of 2358 pass on the two VMs, in a run from a new load.
- For the owners: the bind defect is in `rko281/Pharo-SQLite3` and in
  `pharo-rdbms/Pharo-SQLite3`, which has the same code.

## pharo-test-analysis (`JavierLarre/pharo-test-analysis`, row 90)

- State before: `LOAD-FAIL` (`Name not found: FirstToFix-Tests`).
- Causes:
  - The group `Tests` of the baseline names a package that the repository
    does not have.  `patches/javierlarre-pharo-test-analysis.patch` names
    the two test packages that exist.
  - `TSDFakeTests` and `TSDSubFakeTests` are data for other tests, and their
    `setUp` skips on purpose.  `fixtures/javierlarre-pharo-test-analysis.txt`
    names them.
  - 4 tests: `RTDHelperVisitor` does not understand `messages`.  A commit of
    the package made the class a subclass of Object, and the class lost the
    method that it had from its old superclass.
    `fixes/javierlarre-pharo-test-analysis.st` adds the method.
- Result: 71 of 71 pass on the two VMs, in a run from a new load.

## BioSmalltalk (`hernanmd/BioSmalltalk`, row 70)

- State before: 1205 tests, 1147 pass, 2 failures, 56 errors on the two VMs.
- Causes and repairs (`fixes/hernanmd-biosmalltalk.st`):
  - 11 tests read test files that are only in the archive
    `resources/BioSmalltalkResourceFiles.zip` of the repository.  Nothing
    takes the files out.  The repair takes the archive of the clone apart in
    the working directory one time.
  - 18 tests open files that no repository has (`/tmp/bed/*.bed`,
    `/tmp/maf_test/test.maf`, two GFF3 files of a download).  The `setUp` of
    the tests writes small files with the content that the assertions
    describe, and the `tearDown` removes them.
  - 12 tests send `metabolite:`, `protein:` and `gene:` to `BioSpecies`.
    The class of the pathway code has the name `BioPathwaySpecies` now.
  - 8 tests of the Phylip parser use tokenizers of `BioPhylipParser`.  A
    rewrite moved them to `BioPhylipPetitParser`.
  - 1 test: `BioReaction>>enzymes` did not answer the enzymes of the
    catalyses of the reaction.
  - 1 test needs the program `cutadapt`.  The test uses the program when
    the machine has it, and a small `awk` program with the same options and
    summary text when not.
- 7 tests cannot apply on Pharo 14 (`not-applicable/hernanmd-biosmalltalk.txt`):
  `BioGtUnixSubprocessTest` tests an adapter for a class of Glamorous
  Toolkit.  Their `setUp` now skips with a text that says so.  A first
  repair made a stand-in for the Glamorous Toolkit class.  The package then
  chose that adapter for each external program, and 12 other tests broke.
  The stand-in is gone.
- Limits: the GFF3, BED and MAF files are ours, made from the assertions.
  The `awk` program is not `cutadapt`.
- Result: 1205 tests, 1198 pass and 7 skips on the two VMs, in a run from a
  new load.

## Microdown (`pillar-markup/Microdown`, row 72)

- State before: 1605 tests, 1584 pass, 1 failure, 3 errors, 17 skips on the
  two VMs.
- Causes and repairs (`fixes/pillar-markup-microdown.st`):
  - `MicMetaDataBlockTest`: our image fix 20 used `at:`, which the newer
    Microdown of the repository does not have.  The image fix uses `atKey:`
    now, which the two versions have.
  - 2 errors: `MicSyncTesterStrategy>>verify:` sends
    `Smalltalk image lastUpdateString`, which Pharo 14 does not have.
  - 1 failure: a test expects the text of an old compiler error for an
    empty source.
  - 8 skips of `MicBlogCreatorTest` "for the CI server": the tests copy CSS
    files from the clone of the repository, which the package run has.  The
    skips go.
  - 5 skips of `MicReferenceCheckerTest`: two tests use an old API.
    `MicAnalysisReportWriter>>buildReportSTONFormatOn:` wrote each result
    under each group and put file objects into the STON text.
    `MicUnreferencedFigureResult` had no `errorType` and no
    `stonFormatExplanation`, which its sibling classes have.  One test was a
    copy of another test with no figure in its document.
  - `MicDumperTest>>testTable`: `MicRealDumper>>visitTable:` was a stub.
  - `MicHTMLConfigurationTest>>testWriteDocumentCSS` had no body.
  - `MicParsesAndRendersAllCommentsTest>>testSmokeSignalAndChoke` renders
    the comment of each class and needs more than the 10 s of SUnit.  The
    class has a time limit of 15 minutes now.
- Result: 1605 of 1605 pass on the two VMs, in a run from a new load.

## Turbo (`pavel-krivanek/turbo`, row 91)

- State before: 270 tests, 265 pass, 5 expected failures on the two VMs.
- Causes:
  - The 5 expected pictures of `TbFlowLayoutTest` (two children in a
    vertical flow, in the centre) are one cell off.  The free space is odd,
    so the layout gives a position with a half cell.  `TbCanvas>>at:write:`
    rounds a half up.  The pictures were drawn for a half that is cut off.
    The layout code is right, and the other tests of the class agree with
    it.
  - `TbBounds>>atRandom` could answer a point on the right or bottom edge,
    which `containsPoint:` does not count as inside.  `TbBoundsTest>>testAtRandom`
    failed in about 1 run of 10 (372 of 4000 points outside on stock Cog
    before the repair, 0 after).
- Repair here: `fixes/pavel-krivanek-turbo.st`: the 5 pictures, with the
  line ends of the package (CR), no mark of an expected failure, and
  `atRandom` with an offset from 0.
- Result: 270 of 270 pass on the two VMs, in a run from a new load.

## Pillar (`pillar-markup/pillar`, row 71)

- State before: 66 tests, 37 pass, 24 errors, 5 skips on the two VMs.
- Causes:
  - 20 errors (`UndeclaredVariableRead: MicPathRelativizer`) and, behind
    them, 4 errors of the archetype tests.  Pillar (branch `dev`) uses
    classes of the package `Microdown-Toc`, which only the branch `dev` of
    Microdown has.  The baseline of Pillar names Microdown `v2.11.2`.  And
    Pharo 14 has a class `BaselineOfMicrodown` in the image: Metacello does
    not load a baseline whose class is in the image, so the load kept the
    old package list of the image for each repository that the baseline
    named.
  - `PRArchetype>>directory` looks for the archetypes beside the image
    only.  The CI of the package copies them there.  Three tests name
    archetypes that the repository renamed or removed.
  - `PRIntegrationTest>>testBookEpub` is a skip.  It sends `outputEPub`,
    which has no implementor.  The package `Pillar-ExporterEPub` of the
    repository cannot load: its superclasses and writers are defined
    nowhere.
- Repair here:
  - `patches/pillar-markup-pillar.patch`: the baseline names Microdown
    `dev`.  The manifest row has `ignoreImage`, so Metacello loads the
    baseline of the repository.
  - `fixes/pillar-markup-pillar.st`: `PRArchetype>>directory` also looks in
    the clone of the repository; the three archetype names; and a small
    EPub exporter (`PREPubOutput`, `PREPubDocument`) in the style of the
    HTML exporter, so that `testBookEpub` runs with its assertions.
- Limit: the EPub exporter is ours and is as small as the test needs (the
  figures are not in the archive, and the navigation has one entry).
- Result: 66 tests, 62 pass and 4 skips on the two VMs, in a run from a new
  load.
- Open, the 4 skips: `testBasicLatexPdf`, `testBookPdf`,
  `testPresentationPdf` and `testWelcomePdf` start with `self skip`.  They
  run `latexmk` with `lualatex`.  The test machine has no TeX (no
  `latexmk`, `lualatex`, `pdflatex` or `xelatex` on the PATH, and no
  `/Library/TeX`).  A probe without the skip wrote `index.tex` and then
  stopped in OSSubprocess: the baseline pins OSSubprocess `v2.0.0`, which
  calls `OSEnvironment>>defaultEncoding`, a method that Pharo 14 does not
  have.  OSSubprocess `master` has the repair.  The owner must decide if
  TeX goes on the machine; then the pin and the four tests get their
  repair.  The row is not clean.

## MyPrecious (`SmalltalkWeb/MyPrecious`, row 101)

- State before: stock Cog stopped in the middle of the row (72 tests run, 1
  error).  Our VM ran the 109 tests: 108 pass, 1 error
  (`testReconnectsIfOtherSideIsDisconnected`, `TestFailedByForkedProcess`).
- Cause, one defect of the package: a connection that is closed makes the
  receive service of `MpConnection` loop with no end.
  `SocketStream>>next:` answers an empty ByteArray at the end of a stream,
  with no signal.  `MpConnection>>receiveObject` then makes a task for 0
  bytes.  The task fails in a TaskIt worker, the service has a step delay
  of 0 ms, and nothing stops it.
  - On our VM the failed tasks make the error of the test.
  - On stock Cog the loop keeps the processor from the other processes of
    its priority, the queue of failed tasks grows, and old space reaches
    its limit.  Stock Cog then stops the VM (the low-space defect of Cog,
    `docs/upstream/cog-changes.md`).
- Repair here (`fixes/smalltalkweb-myprecious.st`): a short read signals
  `MpConnectionFailed`, `receiveObject` stops the receive service when the
  connection is closed, and a new test
  (`testStopsReceivingWhenOtherSideClosesConnection`, in the two middleware
  test classes) checks that.
- Result: 111 of 111 pass on the two VMs, and stock Cog runs the full row.

## Stardust (`ba-st/Stardust`, row 102) and the Buoy patch

- State before: 71 tests, 48 pass, 23 errors on the two VMs
  (`InstanceCreationFailed: The amount of model creators (3) should match
  the amount of arguments (4)`).
- Cause: `GenericModelCreator class>>assertSizeOf:equalsArgumentCountOf:`
  sends `argumentCount` to a Symbol.  Stardust relied on the extension
  `Symbol>>argumentCount` of Buoy, which answers `numArgs`.  That extension
  replaces a method of Pharo 14, and our Buoy patch removes it so that Buoy
  loads.  The method of Pharo 14 answers `numArgs + 1` (a Symbol in the
  place of a block).
- Repair here: `patches/ba-st-stardust.patch` uses `numArgs` in the two
  places.
- Result: 71 of 71 pass on the two VMs.

## GitHub-Pharo-API (`Evref-BL/GitHub-Pharo-API`, row 104)

- State before: 50 tests, 46 pass, 4 errors on the two VMs
  (`UndeclaredVariableRead: GitlabApi`).
- Cause: three tests of `GithubUsersTest` are copies of tests of the GitLab
  project and make a `GitlabApi`.  The baseline rightly does not load that
  class.  A fourth test calls one of the three.
- Repair here: `fixes/evref-bl-github-pharo-api.st` uses `GithubApi`.  The
  tests replace the HTTP client with a mock, so no request leaves the
  machine.
- Result: 50 of 50 pass on the two VMs.

## FamixTagging (`moosetechnology/FamixTagging`, row 105)

- State before: 115 tests, 0 pass, 114 errors, 1 expected failure on the two
  VMs (`UndeclaredVariableRead` of the model classes).
- Causes:
  - The model classes need `MooseEntity` and `MooseModel`.  Nothing loaded
    Famix.  The baseline has `spec for: #needsFamix`, and its
    `customProjectAttributes` answers `#NeedsFamix`: Metacello compares the
    case, so the Famix part of the baseline never applies.  The CI of the
    package loads Famix first.
  - The expected failure `testProviderMethodToTags` hides a gap:
    `FamixTag>>isLinkedTo:dependancyDirection:in:` does not look at the
    children of the tagged entities.
- Repair here: the manifest row loads Famix (groups `Core` and
  `TestModels`) first.  `fixes/moosetechnology-famixtagging.st` repairs the
  method and removes the mark.
- Result: 115 of 115 pass on the two VMs.

## SmaCC (`j-brant/SmaCC`, row 106)

- State before: 565 tests, 540 pass, 25 errors on the two VMs.
- Causes, all in the code generator of SmaCC and its tests, which are for
  an older Pharo:
  - 12 errors: `SmaCCIncrementalParser class>>incrementalParserFrom:` makes
    a subclass with no class-side slot, and then compiles a class-side
    method that uses the variable.
  - 5 errors: the generator gives a Symbol to `OCMessageNode>>selector:`,
    which wants a selector node in Pharo 14.
  - 6 errors: tests make classes with
    `subclass:instanceVariableNames:classVariableNames:category:`.
  - 2 errors: `asClass` on a name.
- Repair here: `fixes/j-brant-smacc.st`.
- Result: 565 of 565 pass on the two VMs.

## Sagan (`ba-st/Sagan`, row 100)

- State before: 133 tests, 37 pass, 61 errors, 35 timeouts on the two VMs,
  and 14 tests with another result on each VM (an error on one VM and a
  timeout on the other).
- Causes:
  - The tests of the relational repositories choose a PostgreSQL server on
    `127.0.0.1:5432` when the environment variable `RDBMS` is absent.  The
    test machine has no server.  The login code tries again with waits, so
    a test ends with an error or with the time limit.  The CI of the
    repository runs the same tests with `RDBMS=SQLite3`.
  - With SQLite: 68 errors "database is locked".  The cause is in the
    dependency Pharo-SQLite3: `SQLite3PreparedStatement>>stringAt:` sends
    `SQLite3Library>>stringFrom:at:`, and only the packages `SQLite3-Pharo8`
    and `SQLite3-Pharo9` have that method.  The baseline of Pharo-SQLite3
    loads `SQLite3-Pharo9` for Pharo 9 to 13 and not for Pharo 14.  A read
    of a text column fails, the statement stays open and keeps a read lock
    on the file, and the `DROP TABLE` of the `tearDown` then gets "database
    is locked", which hides the first error.
  - The text bind of Pharo-SQLite3 has the defect of the ReStore row
    (`SQLITE_STATIC` for bytes that nothing holds).
- Repair here (`fixes/ba-st-sagan.st`): SQLite3 is the login when `RDBMS` is
  absent; `SQLite3Library>>stringFrom:at:` with the body of
  `SQLite3-Pharo9`; the text bind with `SQLITE_TRANSIENT`.
- Limit: the tests run on SQLite, not on PostgreSQL.  They do not check the
  PostgreSQL driver.
- Result: 133 of 133 pass on the two VMs, in a run from a new load.  No test
  differs between the VMs.
- For the owners: the baseline of `pharo-rdbms/Pharo-SQLite3` must name
  Pharo 14.

## P3 (`svenvc/P3`, row 113)

- State: 96 tests, 23 pass, 70 errors (`NameLookupFailure: cannot resolve
  'postgres'`), 3 timeouts on the two VMs.
- Cause: 73 tests need a PostgreSQL server.  `P3ClientTest class>>url`
  answers `psql://postgres:postgres@postgres:5432/postgres`.  The host
  `postgres` is the name of the service container of the CI of the
  repository, so the tests stop at the name lookup on this machine.  No
  environment variable changes the URL.
- No stand-in: the tests send real SQL and check what the server does
  (tables and types, the catalogs, notices, a statement timeout, time
  zones).  A stand-in with replies that we write would only check the
  decoder of P3 against our own text.
- The 3 timeouts: `P3Client>>withConnection:` tries a network error again
  10 times and waits 5 s each time.  A name lookup failure is a network
  error, so a test of the connection pool waits for 50 s.
- One package defect, repaired in `fixes/svenvc-p3.st`:
  `P3Client>>retryDelay:` did not store its argument.
- Open, for the owner: the row needs a PostgreSQL server (version 9.4 or
  later) on `127.0.0.1:5432` with the database `postgres`, the user
  `postgres` and the password `postgres`, and the tests need the host
  `127.0.0.1` in their URL.  We install nothing without the word of the
  owner.  The row is not clean.

## Kepler (`ba-st/Kepler`, row 107)

- State before: 41 tests, 37 pass, 4 errors on the two VMs.
- Cause: `Subscription class>>of:to:toBeNotifiedSending:` and
  `Subscription>>notifyAbout:` send `argumentCount` to a Symbol, as Stardust
  does (see the Stardust section).
- Repair here: `patches/ba-st-kepler.patch` uses `numArgs` in the two places.
- Result: 41 of 41 pass on the two VMs.

## MicroUML (`olekscode/MicroUML`, row 110)

- State before: 83 tests, 78 pass, 1 failure, 4 errors on the two VMs
  (`Error: Package names should be uppercase`).
- Cause: the `setUp` of `MicroUMLCodeGeneratorTest` makes classes in a
  package with the name `__MicroUML__Dummy-Package__`.
  `PackageOrganizer>>addPackage:` of Pharo 14 refuses a name that does not
  start with an upper-case letter.  The class is in the system before the
  error comes, so a later test found classes of an earlier test (the
  failure).
- Repair here: `fixes/olekscode-microuml.st` uses the name
  `Z_MicroUML__Dummy-Package__`.
- Result: 83 of 83 pass on the two VMs.

## pharo-openapi-generator (`alkalinan/pharo-openapi-generator`, row 112)

- State before: `LOAD-FAIL` (`KeyNotFound: key 'BaselineOfOpenAPI'`).
- Causes, all in the repository, which is an unfinished state:
  - The repository has no `.project` file and no `src/.properties`, so
    Iceberg finds no package in `src`.
  - The classes `OAContact`, `OAInfo`, `OAComponents` and `OAOperation`,
    which the code and the tests use, are not in the repository.
  - One test has a literal `#{...}` that Pharo 14 does not read, and one
    test uses a mock that does not pass `visit:` on.
  - The baseline pins a version of Mocketry of 2019.
- Repair here: `patches/alkalinan-pharo-openapi-generator.patch` adds the
  two project files and the four classes (as small as the tests need),
  nests the JSON mapping of the info object, corrects the two tests and
  names the present Mocketry.
- Limit: the four classes are ours.  The author must add the real ones.
- Result: 16 of 16 pass on the two VMs.

## OSSubprocess (`pharo-contributions/OSSubprocess`, row 115)

- State before: 126 tests, 98 pass, 2 failures, 26 errors on the two VMs.
- Causes, all API changes of Pharo 14 (the row loads the branch `master`):
  - 18 errors and 2 failures: the old file stream classes of the package
    send `SessionManager default currentSession isReadOnlyAccessMode`.
    `WorkingSession` of Pharo 14 has no such method.
  - 4 errors: a test helper sends `UUIDGenerator next`.
  - 4 tests send `Smalltalk image changesName`.
- Repair here: `fixes/pharo-contributions-ossubprocess.st` adds
  `WorkingSession>>isReadOnlyAccessMode` (it answers false), and the tests
  use `UUID new` and `FileLocator changes`.
- Result: 126 of 126 pass on the two VMs.

## Postern (`punt-labs/postern`, row 116)

- State before: 88 tests, 82 pass, 2 failures, 1 error, 3 skips on the two
  VMs.
- Causes, all in the package, which is for Pharo 12:
  - `testCodeFencesParse`: the test catches `CodeError`.  Pharo 14 signals
    `OCCodeError`, the superclass.
  - `testPharoVersionPin` asks for a review of the help content when the
    Pharo version changes.  The review found names that Pharo 14 does not
    have (`RPackageOrganizer`, `RBRenameMethodRefactoring`, `TestRunner`,
    `IceWorkingCopy>>changes`).  The help content and the version text are
    for Pharo 14 now.
  - `testDelegateNormalizesLfToCr`: the input of the test has no period
    after a statement, so the code that it evaluates is not the code that
    the test means.
  - 2 skips "test requires a running eval server": no test starts the
    server.  Each of the two tests now starts a server on the loopback
    interface and stops it.
  - 1 skip: the test looks for the Iceberg repository of another project of
    the author.  It uses the repository of the package now.
- Repair here: `fixes/punt-labs-postern.st`.
- Result: 88 of 88 pass on the two VMs.

## Blenny (`NathanFrund/blenny`, row 118)

- State before: `LOAD-FAIL` (an Array index error in the load).
- Causes:
  - The load brings Buoy in (through JSONWebToken and NeoJSON).  Buoy
    replaces `Symbol>>argumentCount` of Pharo 14, and the post-load step of
    Buoy then fails.  The row gets the locked Buoy with its patch
    (`locks/nathanfrund-blenny.txt`), as the ba-st rows.
  - The baseline of Blenny has a bracket at the wrong place, so three of its
    groups are outside the platform block
    (`patches/nathanfrund-blenny.patch`).
  - 3 tests start with `self skip`, and the reasons in their comments do not
    hold.  They have a body now.  One of them uses a small object in the
    place of a WebSocket, which records the messages that it gets.
  - 2 tests subtract an Integer from a `DateAndTime`, which Pharo 14 does
    not do.  They subtract a Duration.
  - Three defects of the package code that no test shows (a `join:` with
    the receiver and the argument in the wrong places, a parse of an
    argument with no `=`, the name of an environment variable).
- Repair here: the lock, the patch and `fixes/nathanfrund-blenny.st`.
- Result: 35 of 35 pass on the two VMs.  No test of the row needs a
  PostgreSQL server.

## Soil (`ApptiveGrid/Soil`, row 84)

- State before: 1139 tests, 1109 pass, 30 skips on the two VMs.  Each of the
  30 is a `self skip` of the package.
- Defects of the package that the skips hid, each repaired in
  `fixes/apptivegrid-soil.st`:
  - `SoilBTreePage>>cleanRemovedIn:` removed each removed entry by its key.
    With two entries of one key the search can find the live entry, so the
    live entry went and the removed one stayed.
  - `SoilRestoringIndexIterator` had no `size` for an older transaction: a
    transaction saw the count of a later commit.
  - `SoilObjectProxy` had no `class` method (a commit removed it), so
    `class` answered the proxy class and not the class of the object.
  - `SoilObjectRepository>>cacheSegmentAt:using:` gave its block the segment
    where the block needs the cache.
  - `SoilBehaviorRegistry>>recreateIndex` did a commit and then an abort of
    the same transaction.
  - A method with no sender in the image
    (`checkDescriptionsOf:referencedBy:in:`) is gone, and a class with no
    use (`SoilObjectVisitor`) too.
- Tests:
  - 3 tests were stale and have a body for the code as it is.
  - The lint tests of the package (no class with no use, no call with no
    implementor, no selector with no sender) run now.  For them:
    `SoilJournalConverter`, a tool that a user starts by hand, got a test;
    44 new tests (`SoilApiSurfaceTest`) call the API that no test called;
    and a reader of the method format of Pharo 11 has the lint mark for the
    selector that Pharo 14 does not have.
- Result: 1184 tests, 1167 pass and 17 skips on the two VMs, in a run from a
  new load.
  - 14 skips cannot apply (`not-applicable/apptivegrid-soil.txt`): 8 tests
    of file locks of Windows, and 6 cases of a parametrized test that have
    no meaning for their parameter.
  - Open, 3 skips: the three `testCodeCoverage` tests ask for a coverage of
    93 percent.  The measured coverage is 86 percent (Soil-Core), 81 percent
    (Soil-Serializer) and 85 percent (Soil-Resident).  The package needs
    more tests for about 210 methods.  The row is not clean.
- For the owner of Soil: `SoilObjectProxy>>class` is back, which the author
  removed on purpose or by accident; the review must decide.

## Superluminal (`ba-st/Superluminal`, row 87)

- State before: no load (the baseline of Teapot `v4` has no branch for
  Pharo 14, as for Stargate).  With the patched Teapot and Launchpad as
  locked dependencies: 203 tests, 150 pass, 4 failures, 49 errors on the two
  VMs.
- Causes:
  - 43 errors: the tests call an httpbin service.  The CI of the package
    starts a container and gives its name in `CI_HTTPBIN_HOSTNAME`.  With no
    variable the tests go to `localhost:80`.
  - 8 tests need a memcached server on `127.0.0.1:11211`.  The memcached
    client of the dependency drops a connection error, so a stored item is
    gone with no error.
  - 4 tests compare a URL with a text whose query fields are in another
    order than the code makes them (see "URLs with the same query fields in
    another order" below).
- Repair here (`fixes/ba-st-superluminal.st`): two stand-ins in the image,
  `SuperluminalHttpbinStandIn` (a `ZnServer` on `127.0.0.1` with the routes
  that the tests use) and `SuperluminalMemcachedStandIn` (the text protocol
  of memcached).  The tests find them when the CI variables are absent.  The
  URL order has its repair in the image (image fix 34).  No assertion is
  gone.
- Limits: the tests check the client code of the package against the
  stand-ins, not against httpbin and memcached.  The route `/cache` of the
  stand-in takes the method `QUERY`, because one test sends it; we did not
  read the httpbin fork of the CI.  Port 11211 must be free.
- Result: 203 of 203 pass on the two VMs, in a run from a new load.

## Kendrick (`KendrickOrg/kendrick`, row 83)

- State before: `LOAD-FAIL` (`Conflicting repositories!`).
- Causes of the load failure, in the baseline: the baseline names its own
  repository with another address than the load expression, and it gives
  the dependencies of `Kendrick-Core` only for Pharo 8 to 10.
  `patches/kendrickorg-kendrick.patch` removes the repository line and names
  PolyMath, DataFrame, PetitParser and the AI graph algorithms for each
  Pharo version.
- State after the load: 240 tests, 219 pass, 1 failure, 16 errors, 4
  expected failures on the two VMs.
- Causes, each found with a probe (`fixes/kendrickorg-kendrick.st`):
  - The chart code sends `RSChart>>addPlot:`.  Roassal of Pharo 14 has
    `add:`.
  - `Composition>>doesNotUnderstand:` looks for a parameter in its own
    parameters, which are empty.  The parameters are in the model of the
    composition.
  - `KEComponent>>attributes:` did not refuse two attributes with one name
    (1 failure), and did not take a Dictionary.
  - Four defects behind the four marks of an expected failure: a model made
    from transitions had its equations in the wrong kind of collection;
    `KEUnaryExpression>>removeVariable:` changed the receiver; a compartment
    that only a transition names had no key; and one test asked a model of
    transitions for events, which only exist between equations and
    transitions.  That test now asks for the transitions and keeps its
    expected value of 4.  The four marks are gone.
  - `KendrickEquations` reads the print text of an Array of symbols.  Pharo
    14 prints `#'S:t'` with quotes, so the text was cut.
  - Tests: `contents max` on a DataFrame (the value is `max first`);
    `asArray` on a number; tests that compared data frames of another
    shape; a class name that the last commit of the repository changed.
- 10 simulation tests need 3 to 25 s, and SUnit gives a test 10 s.  The
  profile is flat (the work is the steps of the simulation, no slow lookup).
  The test classes of the simulations have a time limit of 3 minutes of
  their own, with the measured times in the comment.
- Result: 240 of 240 pass on the two VMs, in a run from a new load.

## URLs with the same query fields in another order (rows 15, 81, 87)

- Tests of three ba-st packages compare a URL with a text whose query
  fields are in another order than the code makes them:
  - Hyperspace, `ZnUrlHyperspaceExtensionsTest>>testStartLimit` (the test
    asserts that one URL is equal to the two orders);
  - Stargate, `PetsRESTfulControllerTest>>testComplexPagination` ("Got
    `...pets?limit=2&start=3` instead of `...pets?start=3&limit=2`");
  - Superluminal, `CompositeHttpRequestCommandTest>>testApplyOn` and
    `testShortcutCreation`, `HttpRequestTest>>testComposite` and
    `testQueryString`.
- The tests pass on Pharo 13.  The defect is in the Pharo 14 image:
  `ZnUrl>>equals:` compares the queries with `=`, the query is an
  OrderedDictionary, and `OrderedDictionary>>=` of Pharo 14 compares the
  position of each key.  Measured: the same two URLs are equal on Pharo 13
  and not equal on Pharo 14 (stock Cog and our VM).
- Repair: `scripts/image-fixes/34-znurl-query-order.st` (see
  [../image_issues.md](../image_issues.md)).  The packages need no change
  for it, and the changes of test texts that the first repairs made are
  gone.
- Result with image fix 34 and no package change for the URL order:
  Hyperspace 197 of 197, Stargate 520 of 520 and Superluminal 203 of 203 on
  the two VMs.

## Stargate (`ba-st/Stargate`, row 81)

- State before: `LOAD-FAIL`.  The baseline of the dependency Teapot `v4`
  (`ba-st-dependencies/Teapot`) defines the package `Teapot-Tests` for Pharo
  7 to 13 only, so the load stops for Pharo 14.
- Repair of the load: `patches/ba-st-dependencies-teapot.patch` adds Pharo
  14 to the list.  `locks/ba-st-stargate.txt` loads the patched Teapot, the
  patched Buoy and the patched Launchpad first and locks them.
- `fixes/ba-st-stargate.st`: three methods of
  `PharoOperatingSystemInformationProvider` use `Smalltalk os`, which Pharo
  14 deprecates.  They use `OSPlatform current` and `OSEnvironment current`.
- A defect of our VM that the tests of the row showed: VM parameter 11 (the
  count of objects that scavenges moved to old space) was always 0.
  `GarbageCollectionMetricsTest` asserts a positive value.  Repaired in the
  VM.
- With the load: 520 tests, 519 pass, 1 failure on the two VMs
  (`testComplexPagination`, the URL order above).
- Result with image fix 34: 520 of 520 pass on the two VMs.

## Esope (`moosetechnology/Esope`, row 109)

- State before: `LOAD-FAIL` (`Class not found: #FamixFortranModel`).
- Causes of the load failure and of the first errors:
  - The baseline of the dependency Famix-Fortran has no "requires" line
    from `Famix-FortranUDT-Entities` to `Famix-Fortran77-Entities`, so
    Metacello loads the first package before the package that it needs.
    `patches/moosetechnology-famix-fortran.patch` adds the lines, and
    `locks/moosetechnology-esope.txt` loads the patched Famix-Fortran first.
  - No baseline of the chain names NeoJSON, which the importer uses (83
    errors).  The manifest row loads NeoJSON first.
- State after the load: 331 tests, 325 pass, 1 failure, 5 skips on the two
  VMs; before the first repair of the tests 14 tests were skips.
- Repairs (`fixes/moosetechnology-esope.st`), each test with its assertions:
  - `PPEsopeRewriter>>applySlashTransformation:` put its marker before a
    blank and not at the slash (2 skips).
  - 5 skips were left from a time when the importer lacked the feature; the
    feature is there now.
  - Features that the importer lacked, written in its style: an argument
    count for an invocation (the Fortran model of Famix has no arguments, so
    the count is in the intermediate tree); the command "segini" with a
    copy; a pointer in a segment; a DATA statement; a comment line with an
    IF before an Esope command.
  - `visitBlock_data:` stored a raw Array as the source anchor, so each
    BLOCK DATA stopped the conversion to Famix.
  - One test used `target` of an access, which Famix replaced by `variable`.
  - One test starts an outside parser program.  It uses a stand-in that
    gives the JSON text of the test.
- Two expected values are ours and need the review of the owner of the
  package: the count of body entries of a BLOCK DATA with one COMMON and one
  DATA statement (3 became 2: one statement gives one entry in the sibling
  test), and the end column of an IF comment (29 became 27: the line has 26
  characters, and the sibling test shows that the end is the last column
  plus 1).
- Limit of the model, not repaired: a BLOCK DATA entity of the Fortran model
  has no accesses, so the values of a DATA statement in a BLOCK DATA are not
  in the Famix model.
- Result: 331 of 331 pass on the two VMs, in a run from a new load.

## Iconography (`OpenSmock/Iconography`, row 124)

- State before: 32 tests, 6 pass, 26 errors on the two VMs
  (`KeyNotFound: key #iconCheckCircleFilled32x32 not found in
  MethodDictionary`).
- Cause: the setUp and the tearDown of two test classes
  (`AntDesignIconsLibraryTest`, `MaterialDesignIcons4LibraryTest`) call
  `removeGeneratedMethods`, which sends `removeSelector:` for three
  selectors.  Only two tests make these methods, so in each other test the
  selectors are absent.  `removeSelector:` of Pharo 14 signals an error for
  an absent selector, so the setUp stops before the test starts.
- Repair (`fixes/opensmock-iconography.st`): the two methods remove a
  selector only when the class has it.
- Result on stock Cog: 32 of 32 pass.
- Result on our VM: 31 pass, 1 failure that Cog does not have:
  `AntDesignIconsLibraryTest>>testSerializeAsFormMaxWidthMaxHeight` ("Got a
  Bitmap of length 992 instead of a Bitmap of length 992": the same size,
  other pixel values).  The cause is in work.  An old note said "a library
  difference" (our VM loads the Cairo of Homebrew, Cog loads the Cairo of
  its bundle); nobody measured that.

## DebuggingSpy (`Pharo-XP-Tools/DebuggingSpy`, row 122)

- State before: 113 tests, 71 pass, 2 failures, 38 errors, 2 skips on the
  two VMs (`UndeclaredVariableRead` of `properties` and of `Breakpoint`,
  compile errors).
- Cause: the manifest row loaded the branch `P12`, the port of the package
  for Pharo 12.  That code uses classes and variables that Pharo 14 does not
  have.  The repository has a branch `P14`, its default branch, which the CI
  of the repository runs on Pharo 14.
- Repair: the manifest row loads the branch `P14` (and NeoJSON first, which
  the tests use).  No change of the package.
- Result: 227 of 227 pass on the two VMs.

## Famix-ExecutionFlow (`moosetechnology/Famix-ExecutionFlow`, row 130)

- State before: 8 tests, 8 errors on the two VMs (`UndeclaredVariableRead`
  of `FamixEFDirectMethodCall`).
- Cause: the baseline of the package does not name Famix.  The load gives
  only warnings ("depends on the following classes": `MooseModel`,
  `MooseEntity`, `FamixTMethod`), does not define the classes of three
  packages, and ends with success.  The CI of the package uses a Moose
  image, which has Famix.
- Repair: the manifest row loads Famix (`Core`, `Basic`,
  `EntitiesSmalltalk`) first.  For the owner of the package: the baseline
  must name Famix.
- Result: 8 of 8 pass on the two VMs.

## gaussian-mixture-model (`pharo-ai/gaussian-mixture-model`, row 131)

- State before: 9 tests, 5 pass, 4 errors on the two VMs
  (`PMSingularMatrixError: lupInverse encountered a singular matrix`).
- Causes, all in the package:
  - The fit (the EM algorithm) does not regularise the covariance matrices.
    In the data of the tests each cluster has the same x value for all its
    points, so the covariance matrix of a cluster has a variance of 0 and no
    inverse.  The tests passed in 2022 by the start points that the Random
    generator of Pharo 9 gave for the seed 3.  Pharo 14 has another
    generator.
  - With a small variance the density of a far point is 0.0 for the two
    clusters, and the responsibilities are 0.0 / 0.0 (NaN).  `labels` then
    answers 0, which is no cluster.  The expected values of the tests
    (`#(1 1 1 0 0 0)`, `#(1 0)`) have this 0.
  - `labels` answered an OrderedCollection, and the test compares with an
    Array.  The two are never equal, so `testLabels` could not pass.
- Repair (`fixes/pharo-ai-gaussian-mixture-model.st`):
  - `updateParametersFor:` adds 1e-6 to the diagonal of each covariance
    matrix (the default of scikit-learn for the same algorithm).
  - `calculateResponsibilitiesFor:` computes with logarithms.
  - `labels` answers an Array.
  - Three tests assert what the model must give and what does not depend on
    the start points: the first three points have one label, the last three
    have the other label, each label is 1 or 2, `predict:` of the fit data
    gives the labels of the fit, and two new points get the label of their
    cluster.  `testFitX` has two more checks.
- For the owner of the package: the expected values of three tests changed.
- Result: 9 of 9 pass on the two VMs.

## PharoCodex (`StefanKrecher/PharoCodex`, row 132)

- State before: 16 tests, 11 pass, 5 errors on the two VMs
  (`UndeclaredVariableRead` of `NeoJSONWriter` and `NeoJSONReader`; one
  "only integers should be used as indices" that comes from the same cause).
- Cause: the baseline of the package does not name NeoJSON.
- Repair: the manifest row loads NeoJSON first.  For the owner of the
  package: the baseline must name NeoJSON.
- The tests use no network service and no key: the transports of the tests
  are stand-ins in memory.
- Result: 16 of 16 pass on the two VMs.

## AoC-in-Pharo (`shnarazk/AoC-in-Pharo`, row 119)

- State before: 20 tests, 7 pass, 13 errors on the two VMs (`OSEnvironment`,
  `Dictionary` and `Array` do not understand `#atGrid:`; `SmallInteger` does
  not understand `#y`).
- Causes, in the package:
  - The last commit of the repository (20b89cf) renamed `at:` to `atGrid:`
    also in code whose receiver is not a `GridArray`.  Only `GridArray` has
    `atGrid:`.
  - `GridArray>>x:y:` and `x:y:put:` gave an Integer to `atGrid:`, which
    takes a Point.
- Repair (`fixes/shnarazk-aoc-in-pharo.st`): the 10 methods send `at:`
  again, and the two `GridArray` methods use `at:` and `at:put:`.
- 6 tests cannot apply on this machine (`Y2025Day05Test`, `Y2025Day08Test`).
  They read puzzle input files from a directory `data` under the home
  directory.  The repository has no such file, and Advent of Code asks that
  the puzzle text and the inputs are not copied.  The setUp of the two
  classes skips the test with that reason when the file is absent.
  `not-applicable/shnarazk-aoc-in-pharo.txt` has the proof.
- For the owner of the package: the solution of day 8 of 2025 is not
  complete (`part2` answers 0), so `testPart2` cannot pass with the input
  file either.
- Result: 14 pass and 6 skips on the two VMs; each skip is in the
  not-applicable file.

## Famix-Cpp (`moosetechnology/Famix-Cpp`, row 126)

- State before: `LOAD-FAIL` (an `OCUndeclaredVariableNotice` for `FMOne` in a
  slot of a trait).
- Cause: the baseline of the package does not name Famix and TinyLogger.
- Repair of the load: the manifest row loads Famix (`Core`, `Basic`) and
  TinyLogger first.  For the owner of the package: the baseline must name
  the two.
- With the load: 30 tests, 20 pass, 10 expected failures on the two VMs.
  The 10 are tests of a parser and an importer for CSV exports
  (`Famix-Cpp-Parser`).  That code names classes that the metamodel of the
  package no longer has (`FamixCppType`, `FamixCppFileAnchor`,
  `FamixCppSourcedEntity`).  The test class marks the tests as expected
  failures.
- Open: a port of the parser and the importer to the present metamodel is
  in work.

## PharoSmalltalkInteropServer (`mumez/PharoSmalltalkInteropServer`, row 137)

- State before: 44 tests.  Stock Cog passes 44.  Our VM passes 9, with 34
  errors (`ConnectionClosed: Connection aborted to 127.0.0.1:8086`) and 1
  failure (`testStatusStringWhenRunning`).
- No defect of our VM.  Probes on the two VMs show that the socket plugin,
  Zinc and Teapot work on our VM when the server runs.
- Cause of the difference, in our test runner: the runner starts the tests
  inside the image start, before the deferred start actions of the image.
  `SisServer class>>startUp:` makes the server in such an action.  On stock
  Cog the tests start after these actions, so the server runs before the
  first test.  On our VM the first test that sends `SisServer current`
  starts the server inside the test.  SUnit ends each process that a test
  started, so the server stops at the end of that test, and each later
  request has no server.  The change of the runner is in work (see
  [../WIP.md](../WIP.md)).
- Defects of the package that the difference showed.  They are there on
  each VM, when the first `SisServer current` is in a test or when one test
  runs alone:
  - `SisTest` has no setUp, so the tests depend on a server that something
    else started.
  - `testStatusStringWhenRunning` passed on stock Cog only after an earlier
    test put a setting into the server.  `SisServer>>statusString` printed
    an empty list of settings as `#()` and a list with entries as `{...}`.
- Repair (`fixes/mumez-pharosmalltalkinteropserver.st`):
  - a new `SisTest>>setUp` starts the server when it does not run.  The
    start is inside `DefaultExecutionEnvironment beActiveDuring:`, so the
    server processes do not belong to the test and SUnit does not end them.
  - `statusString` prints `{}` for no settings.  The text for a list with
    entries is the same as before.
- Result: 44 of 44 pass on the two VMs.

## HeuristicCompletion-Benchmarks (`omarabedelkader/HeuristicCompletion-Benchmarks`, row 142)

- State before: the load gave 2 packages and no test class, so the row ran
  no test.
- Cause: the baseline of the repository has the package
  `ExtendedHeuristicCompletion-Benchmarks-Tests` inside a comment, and does
  not name the package `ExtendedHeuristicCompletion-Model`.
- Repair (`patches/omarabedelkader-heuristiccompletion-benchmarks.patch`,
  for the owner of the package): the baseline names the tests package and a
  group `default` with the benchmarks and the tests.  The package `Model`
  is in a group of its own: it uses names that Pharo 14 does not have, and
  no test needs it.
- Result: 14 of 14 pass on the two VMs.

## pharo-dataset (`pharo-llm/pharo-dataset`, row 143)

- State before: 14 tests, 13 pass, 1 error on the two VMs
  (`CooCompletionDatasetExporter` does not understand
  `#entriesAsCompletionJsonString`).
- Cause: commit ff24157 of the repository deleted the method and left its
  two senders (`writeToFileNamed:`, `writeToFileReference:`) and its test.
- Repair (`fixes/pharo-llm-pharo-dataset.st`): the method is back, with the
  body of the deleted method.
- The tests use no network service and no key.
- Result: 14 of 14 pass on the two VMs.

## Pharotales_Books (`neAnasteisha/Pharotales_Books`, row 144)

- State before: 22 tests, 21 pass, 1 failure on the two VMs ("Got 2026
  instead of 2025").
- Cause: `FairyTaleTest>>testInitialize` compares the year of a new object
  with the fixed value 2025.  `FairyTale>>initialize` uses the year of
  today.  The test passed only in 2025.
- Repair (`fixes/neanasteisha-pharotales-books.st`): the test compares with
  `Date today year`, as the test `testYearLimits` of the same class does.
- Result: 22 of 22 pass on the two VMs.

## Sindarin-DAP (`Pharo-XP-Tools/Sindarin-DAP`, row 145)

- State before: 18 tests.  Our VM passed 18 in the run of the row.  Stock
  Cog passed 17 and had 1 failure:
  `DAPHistoryTest>>testMessagesShouldReturnInReverseChronologicalOrder`
  ("Got 7 instead of 8").
- No defect of a VM.  The test fails on the two VMs in repeated runs: 171 of
  300 runs on stock Cog and 67 of 300 on our VM.  Our VM passed the run of
  the row by accident.
- Cause, in the package: `DAPHistory>>messages` sorts the messages by their
  time stamp (`DateAndTime now`) with the block `[ tA >= tB ]`.  Messages
  that arrive in one tick of the clock have the same time stamp, and the
  sort then gives them in any order.  Measured: of 2000 time stamps taken
  in a loop, 655 are distinct on stock Cog and 912 on our VM.
- Repair (`fixes/pharo-xp-tools-sindarin-dap.st`): `addRequest:`,
  `addEvent:` and `addResponse:` give each message an arrival number, and
  `messages` sorts by the time stamp and then by the arrival number.  The
  test is not changed.
- Result: the test passes 300 of 300 runs on the two VMs, and the row
  passes 18 of 18 on the two VMs.

