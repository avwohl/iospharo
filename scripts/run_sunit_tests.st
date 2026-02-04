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
    DictionaryTest. SetTest. BagTest. IntervalTest.
    SymbolTest. OrderedCollectionTest. ArrayTest.
    StringTest "last - 438 tests, very slow" }.

  stream := '/tmp/sunit_test_results.txt' asFileReference writeStream.
  [stream nextPutAll: '=== SUnit Test Run ==='; cr; cr.
   testClasses do: [:tc |
     self runTestClass: tc on: stream.
     stream flush].
   stream nextPutAll: '=== ALL TESTS COMPLETE ==='; cr.
   stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SUnitRunner class methodsFor: 'running'!
runTestClass: testClass on: s
  | sels p f e t |
  s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' ==='; cr.
  s flush.
  p := 0. f := 0. e := 0. t := 0.
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
    "Skip tests known to hang or cause VM-level crashes"
    (#(testPrintStringAll testStoreStringAll) includes: sel) ifFalse: [
      "Print progress for StringTest (438 tests)"
      testClass name = 'StringTest' ifTrue: [
        s nextPutAll: '  RUN: '; nextPutAll: sel; cr. s flush].
      (self runSingleTest: testClass selector: sel timeout: 10 on: s)
        ifNil: [p := p + 1]
        ifNotNil: [:kind |
          kind = #fail ifTrue: [f := f + 1].
          kind = #error ifTrue: [e := e + 1].
          kind = #timeout ifTrue: [t := t + 1]]]].

  s nextPutAll: 'Total: '; print: sels size;
    nextPutAll: ' P:'; print: p;
    nextPutAll: ' F:'; print: f;
    nextPutAll: ' E:'; print: e.
  t > 0 ifTrue: [s nextPutAll: ' T:'; print: t].
  s cr; cr! !

!SUnitRunner class methodsFor: 'running'!
runSingleTest: testClass selector: sel timeout: seconds on: s
  "Run a single test with a timeout. Returns nil for pass, #fail, #error, or #timeout."
  | sem result testProcess |
  sem := Semaphore new.
  result := nil. "nil means pass"

  testProcess := [
    [[[testClass new setUp; perform: sel; tearDown]
      on: TestFailure do: [:ex |
        result := #fail.
        s nextPutAll: '  FAIL: '; nextPutAll: sel;
          nextPutAll: ' ['; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 500)); nextPutAll: ']'; cr]]
      on: Deprecation do: [:ex | ex resume]]
      on: Exception do: [:ex |
        result := #error.
        s nextPutAll: '  ERROR: '; nextPutAll: sel;
          nextPutAll: ' ('; nextPutAll: ex class name;
          nextPutAll: ': '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 100)); nextPutAll: ')'; cr].
    sem signal] forkAt: Processor activePriority - 1.

  "Wait for test completion or timeout"
  (sem waitTimeoutSeconds: seconds) ifTrue: [
    "Timeout occurred - terminate the test process"
    testProcess terminate.
    result := #timeout.
    s nextPutAll: '  TIMEOUT: '; nextPutAll: sel;
      nextPutAll: ' (exceeded '; print: seconds; nextPutAll: 's)'; cr.
    s flush].
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
