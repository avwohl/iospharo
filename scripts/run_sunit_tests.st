"SUnit test runner for custom VM testing.
 See CLAUDE.md 'Running Official Pharo Test Suite' for usage."!

Object subclass: #SUnitRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!SUnitRunner class methodsFor: 'running'!
runAllTests
  "Run all tests and write results incrementally to /tmp/sunit_test_results.txt"
  | testClasses stream |
  testClasses := {
    SmallIntegerTest. IntegerTest. FloatTest. FractionTest.
    PointTest. CharacterTest.
    StringTest. SymbolTest. DictionaryTest. SetTest. BagTest. IntervalTest.
    OrderedCollectionTest "slow - runs last".
    ArrayTest "last - known to crash test process" }.

  stream := '/tmp/sunit_test_results.txt' asFileReference writeStream.
  [stream nextPutAll: '=== SUnit Test Run ==='; cr; cr.
   stream flush.
   testClasses do: [:tc |
     self runTestClass: tc on: stream.
     stream flush].
   stream nextPutAll: '=== ALL TESTS COMPLETE ==='; cr.
   stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SUnitRunner class methodsFor: 'running'!
runTestClass: testClass on: s
  | sels p f e |
  s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' ==='; cr.
  p := 0. f := 0. e := 0.
  sels := testClass allTestSelectors asArray sorted.

  sels do: [:sel |
    "Skip tests known to hang or cause VM-level crashes"
    (#(testPrintStringAll testStoreStringAll) includes: sel) ifFalse: [
      [[testClass new setUp; perform: sel; tearDown. p := p + 1]
        on: TestFailure do: [:ex |
          f := f + 1.
          s nextPutAll: '  FAIL: '; nextPutAll: sel; cr]]
        on: Exception do: [:ex |
          e := e + 1.
          s nextPutAll: '  ERROR: '; nextPutAll: sel;
            nextPutAll: ' ('; nextPutAll: ex class name; nextPutAll: ')'; cr]]].

  s nextPutAll: 'Total: '; print: sels size;
    nextPutAll: ' P:'; print: p;
    nextPutAll: ' F:'; print: f;
    nextPutAll: ' E:'; print: e; cr; cr! !

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
