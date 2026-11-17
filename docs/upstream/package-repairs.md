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
- NetworkExtras, multicast sockets.  The 14 tests that failed on our VM only
  showed a defect of our VM: the socket option primitives knew 6 option
  names and gave a false success for each other name
  ([vm-compat-bugs.md](../vm-compat-bugs.md)).  The tests also showed two
  defects of the package:
  - `NeMulticastSocket>>group:port:` binds the group port and sets no reuse
    option, and the tests make two sockets on one group port.  The stock VM
    does not report the failed bind of the second socket
    (`scripts/cogdiff/socket_bind_in_use.st`), so the tests passed there.
    The method now sets `SO_REUSEADDR` and `SO_REUSEPORT` before the bind.
  - `NeMulticastSocketTest>>testInterface` was an expected failure that ends
    with `self fail: #todo`.  The fixes file has the finished test and the
    two accessors of `IP_MULTICAST_IF` that the test needs.
- Tasks, `TkThreadTest>>testPeriodFixedDuration`: the cause is a defect of
  the Pharo image, `Duration>>asDelay`.  See
  [pharo-image-repairs-2026-11-17.md](pharo-image-repairs-2026-11-17.md).
- State of the row with 12 baselines (2026-11-17, in work): our VM 360 of
  361 pass.  Stock Cog stops with a segmentation fault in its garbage
  collector after the multicast tests.  Open: that fault, and
  `testRestartAfterStop` of the multicast server tests on the two VMs.

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
  `run-one-package.sh`).  State: 197 tests, 196 pass, 1 fail on the two VMs.
  The failure is open.

## PharoWiki (`chicoary/PharoWiki`, row 26)

- Defect: the directory `src` has the file `.filetree` and no file
  `.properties`.  The code is in Tonel format, and Iceberg finds no package.
- Effect: the load stops with `KeyNotFound: key 'BaselineOfPharoWiki'`.
- Repair: `patches/chicoary-pharowiki.patch` adds `src/.properties` with the
  Tonel format and removes `src/.filetree`.
- State with the patch: the load works.  77 tests, 70 pass, 7 skip on the two
  VMs.  The 7 skips are open.

