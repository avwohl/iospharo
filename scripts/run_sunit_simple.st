"Simple SUnit test runner - no forking, no timeouts.
 Runs each test directly in the main process."!

Object subclass: #SUnitRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!SUnitRunner class methodsFor: 'running'!
runAllTests
  "Run all tests and write results to /tmp/sunit_test_results.txt"
  | testClasses stream |
  testClasses := {
    "Kernel-Tests: Numbers"
    SmallIntegerTest. IntegerTest. FloatTest. FractionTest.
    PointTest. CharacterTest.
    "Kernel-Tests: Collections"
    DictionaryTest. SetTest. BagTest. IntervalTest.
    SymbolTest. OrderedCollectionTest. ArrayTest.
    StringTest.
    "Collections-Tests"
    LinkedListTest. HeapTest. SortedCollectionTest. WeakSetTest.
    "Compiler-Tests"
    OpalCompilerTest }.

  stream := '/tmp/sunit_test_results.txt' asFileReference writeStream.
  [stream nextPutAll: '=== SUnit Test Run ==='; lf; lf.
   testClasses do: [:tc |
     self runTestClass: tc on: stream.
     stream flush].
   stream nextPutAll: '=== ALL TESTS COMPLETE ==='; lf.
   stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SUnitRunner class methodsFor: 'running'!
runTestClass: testClass on: s
  | sels p f e |
  s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' ==='; lf.
  s flush.
  p := 0. f := 0. e := 0.
  sels := testClass allTestSelectors asArray.
  "Sort with comparison counter"
  [| count |
   count := 0.
   sels := sels sort: [:a :b |
     count := count + 1.
     count > 200000 ifTrue: [
       s nextPutAll: '  SORT-ABORT: '; print: count; lf. s flush.
       Error signal: 'Sort infinite loop detected'].
     a <= b].
   s nextPutAll: '  SORT-OK: '; print: count; nextPutAll: ' comparisons for '; print: sels size; nextPutAll: ' items'; lf. s flush]
  on: Error do: [:ex |
    s nextPutAll: '  SORT-ERROR: '; nextPutAll: ex class name; lf. s flush.
    sels := testClass allTestSelectors asArray].

  sels do: [:sel |
    "Skip known problematic tests"
    (#(testPrintStringAll testStoreStringAll) includes: sel) ifFalse: [
      s nextPutAll: '  RUN: '; nextPutAll: sel; lf. s flush.
      [[[testClass new setUp; perform: sel; tearDown.
         p := p + 1.
         s nextPutAll: '  PASS: '; nextPutAll: sel; lf]
        on: TestFailure do: [:ex |
          f := f + 1.
          s nextPutAll: '  FAIL: '; nextPutAll: sel;
            nextPutAll: ' ['; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 200)); nextPutAll: ']'; lf]]
        on: Deprecation do: [:ex | ex resume]]
        on: Exception do: [:ex |
          e := e + 1.
          s nextPutAll: '  ERROR: '; nextPutAll: sel;
            nextPutAll: ' ('; nextPutAll: ex class name;
            nextPutAll: ': '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 100)); nextPutAll: ')'; lf].
      s flush]].

  s nextPutAll: 'Total: '; print: sels size;
    nextPutAll: ' P:'; print: p;
    nextPutAll: ' F:'; print: f;
    nextPutAll: ' E:'; print: e.
  s lf; lf! !

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

----End fileIn----!
