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
  `?start=1&limit=10`.  The query of a `ZnUrl` is an ordered dictionary, and
  `OrderedDictionary>>=` of Pharo 14 compares the order of the keys (the
  cause of the Buoy `testEquals` repair too).  `fixes/ba-st-hyperspace.st`
  compares the first form as a set of associations and keeps the second
  assertion.
- Hyperspace result: 197 of 197 pass on the two VMs.

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

