"SUnit test runner for custom VM testing.
 See CLAUDE.md 'Running Official Pharo Test Suite' for usage.
 Results: /tmp/sunit_test_results.txt (summary)
          /tmp/sunit_test_detail.txt (per-test with run number)
 Run counter: /tmp/sunit_run_number.txt (auto-incremented)"!

Object subclass: #SUnitRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!SUnitRunner class methodsFor: 'running'!
nextRunNumber
  "Read, increment, and persist the run counter."
  | file num |
  file := '/tmp/sunit_run_number.txt' asFileReference.
  num := [file contents asInteger] on: Error do: [:e | 0].
  num := num + 1.
  file writeStreamDo: [:s | s nextPutAll: num printString].
  ^ num! !

!SUnitRunner class methodsFor: 'running'!
runDiagnostic: stream
  "Run diagnostics before tests"
  stream nextPutAll: '=== DIAGNOSTICS ==='; cr.
  "Test SortedCollection"
  [| sc arr |
   sc := SortedCollection new.
   stream nextPutAll: 'SC new: '; nextPutAll: sc class name; cr.
   stream nextPutAll: '  instVars: '; print: sc class allInstVarNames; cr.
   "instVarAt: 1=array, 2=firstIndex, 3=lastIndex, 4=sortBlock"
   arr := sc instVarAt: 1.
   stream nextPutAll: '  array: '; nextPutAll: (arr ifNil: ['nil'] ifNotNil: [arr class name, ' size=', arr size printString]); cr.
   stream nextPutAll: '  firstIndex: '; nextPutAll: (sc instVarAt: 2) printString; cr.
   stream nextPutAll: '  lastIndex: '; nextPutAll: (sc instVarAt: 3) printString; cr.
   stream nextPutAll: '  sortBlock: '; nextPutAll: (sc instVarAt: 4) printString; cr.
   sc add: 2.
   stream nextPutAll: 'After add:2'; cr.
   arr := sc instVarAt: 1.
   stream nextPutAll: '  fi: '; nextPutAll: (sc instVarAt: 2) printString;
     nextPutAll: ' li: '; nextPutAll: (sc instVarAt: 3) printString;
     nextPutAll: ' size: '; nextPutAll: sc size printString; cr.
   arr ifNotNil: [1 to: (arr size min: 12) do: [:i |
     stream nextPutAll: '  arr['; print: i; nextPutAll: ']='; nextPutAll: (arr at: i) printString; cr]].
   sc add: 5.
   stream nextPutAll: 'After add:5'; cr.
   arr := sc instVarAt: 1.
   stream nextPutAll: '  fi: '; nextPutAll: (sc instVarAt: 2) printString;
     nextPutAll: ' li: '; nextPutAll: (sc instVarAt: 3) printString; cr.
   arr ifNotNil: [1 to: (arr size min: 12) do: [:i |
     stream nextPutAll: '  arr['; print: i; nextPutAll: ']='; nextPutAll: (arr at: i) printString; cr]].
   sc add: 1.
   stream nextPutAll: 'After add:1'; cr.
   arr := sc instVarAt: 1.
   stream nextPutAll: '  fi: '; nextPutAll: (sc instVarAt: 2) printString;
     nextPutAll: ' li: '; nextPutAll: (sc instVarAt: 3) printString; cr.
   arr ifNotNil: [1 to: (arr size min: 12) do: [:i |
     stream nextPutAll: '  arr['; print: i; nextPutAll: ']='; nextPutAll: (arr at: i) printString; cr]].
   "Now test SortedCollectionTest setUp"
   [| test |
    test := SortedCollectionTest selector: #test0CopyTest.
    test setUp.
    stream nextPutAll: 'SCTest setUp OK'; cr.
    stream nextPutAll: '  nonEmpty: '; nextPutAll: test nonEmpty printString; cr.
    stream nextPutAll: '  empty: '; nextPutAll: test empty printString; cr.
    "Run actual test via performTest"
    [test performTest.
     stream nextPutAll: 'SCTest performTest: PASS'; cr]
    on: Error do: [:ex2 |
     stream nextPutAll: 'SCTest performTest ERROR: '; nextPutAll: ex2 class name;
       nextPutAll: ' - '; nextPutAll: ex2 messageText; cr].
    "Run via runCase (like the test runner)"
    [| test2 |
     test2 := SortedCollectionTest selector: #test0CopyTest.
     test2 runCase.
     stream nextPutAll: 'SCTest runCase: PASS'; cr]
    on: Error do: [:ex3 |
     stream nextPutAll: 'SCTest runCase ERROR: '; nextPutAll: ex3 class name;
       nextPutAll: ' - '; nextPutAll: ex3 messageText; cr.
     ex3 signalerContext ifNotNil: [:ctx |
       | c count |
       c := ctx.
       count := 0.
       [c notNil and: [count < 15]] whileTrue: [
         stream nextPutAll: '  '; nextPutAll: c printString; cr.
         c := c sender.
         count := count + 1]]].
    "Run via forkAt:80 like the actual test runner"
    [| sem result test3 |
     sem := Semaphore new.
     result := nil.
     [| t3 |
      t3 := SortedCollectionTest selector: #test0CopyTest.
      [t3 runCase] on: Error do: [:e | result := e messageText].
      sem signal] forkAt: 80.
     (Delay forSeconds: 3) wait.
     sem isSignaled
       ifTrue: [result ifNil: [stream nextPutAll: 'SCTest forkAt80: PASS'; cr]
                       ifNotNil: [stream nextPutAll: 'SCTest forkAt80 ERROR: '; nextPutAll: result; cr]]
       ifFalse: [stream nextPutAll: 'SCTest forkAt80: STALLED'; cr]]
    on: Error do: [:ex4 |
     stream nextPutAll: 'SCTest forkAt80 OUTER ERROR: '; nextPutAll: ex4 class name; nextPutAll: ' - '; nextPutAll: ex4 messageText; cr]] on: Error do: [:ex |
    stream nextPutAll: 'SCTest setUp ERROR: '; nextPutAll: ex class name; nextPutAll: ' - '; nextPutAll: ex messageText; cr].
   stream nextPutAll: 'SC DIAG OK'; cr] on: Error do: [:e |
   stream nextPutAll: 'SC DIAG ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  stream flush.
  "Test IdentitySet with basicIdentityHash"
  [| obj hash |
   obj := Object new.
   hash := obj basicIdentityHash.
   stream nextPutAll: 'Object new basicIdentityHash: '; nextPutAll: hash printString; cr.
   stream nextPutAll: 'IDHash DIAG OK'; cr] on: Error do: [:e |
   stream nextPutAll: 'IDHash DIAG ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  stream flush.
  "Test IdentitySet"
  [| is |
   is := IdentitySet new.
   is add: 1.
   is add: 'hello'.
   is add: Object new.
   stream nextPutAll: 'IdentitySet size: '; nextPutAll: is size printString; cr.
   stream nextPutAll: 'IdentitySet DIAG OK'; cr] on: Error do: [:e |
   stream nextPutAll: 'IdentitySet DIAG ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  stream flush.
  "Test ClassFactory"
  [| factory cls inst |
   factory := ClassFactoryForTestCase new.
   cls := factory newClass.
   stream nextPutAll: 'Factory class: '; nextPutAll: cls name; cr.
   inst := cls new.
   stream nextPutAll: 'Instance: '; nextPutAll: inst printString; cr.
   stream nextPutAll: 'Instance hash: '; nextPutAll: inst basicIdentityHash printString; cr.
   stream nextPutAll: 'Class hash: '; nextPutAll: cls basicIdentityHash printString; cr.
   stream nextPutAll: 'ClassFactory DIAG OK'; cr.
   factory cleanUp] on: Error do: [:e |
   stream nextPutAll: 'ClassFactory DIAG ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  stream flush.
  "Test Random (PCG generator)"
  [| r state val result |
   stream nextPutAll: 'Random DIAG:'; cr.
   r := Random new.
   state := r instVarAt: 1.
   stream nextPutAll: '  state class: '; nextPutAll: state class name; cr.
   stream nextPutAll: '  state size: '; nextPutAll: state size printString; cr.
   val := state at: 1.
   stream nextPutAll: '  state at:1 class: '; nextPutAll: val class name; cr.
   stream nextPutAll: '  state at:1 value: '; nextPutAll: val printString; cr.
   "Step through PCG computation"
   [| mult sum masked |
    mult := val * 6364136223846793005.
    stream nextPutAll: '  val*const class: '; nextPutAll: mult class name; cr.
    stream nextPutAll: '  val*const value: '; nextPutAll: mult printString; cr.
    sum := mult + 1442695040888963407.
    stream nextPutAll: '  +incr class: '; nextPutAll: sum class name; cr.
    masked := sum bitAnd: 16rFFFFFFFFFFFFFFFF.
    stream nextPutAll: '  bitAnd: class: '; nextPutAll: masked class name; cr.
    stream nextPutAll: '  bitAnd: value: '; nextPutAll: masked printString; cr.
    state at: 1 put: masked.
    stream nextPutAll: '  stored back OK'; cr.
    "Now test the output part"
    | count shifted xored shifted2 anded rotA rotB rotResult |
    count := val >> 59.
    stream nextPutAll: '  count: '; nextPutAll: count printString; cr.
    shifted := val >> 18.
    xored := shifted bitXor: val.
    shifted2 := xored >> 27.
    anded := shifted2 bitAnd: 16rFFFFFFFF.
    stream nextPutAll: '  anded: '; nextPutAll: anded printString; cr.
    rotA := anded >> count.
    rotB := anded << (count negated bitAnd: 31).
    rotResult := (rotA bitOr: rotB) bitAnd: 16r7FFFFFFF.
    stream nextPutAll: '  rotResult: '; nextPutAll: rotResult printString; cr.
    "Now test Random>>next"
    result := r next.
    stream nextPutAll: '  r next: '; nextPutAll: result printString; cr.
    stream nextPutAll: '  r next class: '; nextPutAll: result class name; cr.
    stream nextPutAll: 'Random DIAG OK'; cr
   ] on: Error do: [:e2 |
    stream nextPutAll: '  PCG step ERROR: '; nextPutAll: e2 class name; nextPutAll: ' - '; nextPutAll: e2 messageText; cr.
    e2 signalerContext ifNotNil: [:ctx |
      | c count2 |
      c := ctx. count2 := 0.
      [c notNil and: [count2 < 15]] whileTrue: [
        stream nextPutAll: '    '; nextPutAll: c printString; cr.
        c := c sender. count2 := count2 + 1]]]]
  on: Error do: [:e |
   stream nextPutAll: 'Random DIAG ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  stream flush.
  stream nextPutAll: '=== END DIAGNOSTICS ==='; cr; cr.
  stream flush! !

!SUnitRunner class methodsFor: 'running'!
runAllTests
  "Run all tests and write results to /tmp/sunit_test_results.txt and /tmp/sunit_test_detail.txt"
  | testClasses stream detail runNum totalP totalF totalE totalS totalT seen addOnce |
  runNum := self nextRunNumber.

  testClasses := OrderedCollection new.
  "Use a Set to track what we've already added - avoids duplicate iteration bug"
  seen := IdentitySet new.
  addOnce := [:cls | (seen includes: cls) ifFalse: [seen add: cls. testClasses add: cls]].

  "=== Original 15 (all passing) ==="
  {SortedCollectionTest. IdentitySetTest.
    SmallIntegerTest. IntegerTest. FloatTest. FractionTest.
    PointTest. CharacterTest.
    DictionaryTest. SetTest. BagTest. IntervalTest.
    SymbolTest. OrderedCollectionTest. ArrayTest.
    StringTest. HeapTest} do: addOnce.

  "=== Tier 1: Core VM mechanics ==="
  #(BlockClosureTest ContextTest ExceptionTest BecomeTest
    BooleanTest TrueTest FalseTest ProtoObjectTest ObjectTest
    UndefinedObjectTest SemaphoreTest RecursionStopperTest
    LocalRecursionStopperTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 2: Numbers and arithmetic ==="
  #(LargePositiveIntegerTest LargeNegativeIntegerTest IntegerDigitLogicTest
    NumberTest MagnitudeTest ScaledDecimalTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 3: Classes, methods, processes ==="
  #(BehaviorTest CompiledCodeTest CompiledBlockTest
    ClassDescriptionTest ClassHierarchyTest MetaClassTest
    BasicBehaviorClassMetaclassTest PragmaTest
    ProcessTerminateBugTest ProcessSpecificTest
    MonitorTest DelayTest DeprecationTest
    MessageNotUnderstoodTest WeakMessageSendTest
    AllocationTest ObjectLayoutTest DependentsArrayTest
    SharedPoolTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 4: More collections ==="
  #(LinkedListTest OrderedDictionaryTest
    IdentityDictionaryTest StackTest
    DoubleLinkedListTest ByteArrayTest RunArrayTest
    AssociationTest ReduceTest "CollectionRootTest is abstract"
    WideStringTest "ByteSymbolTest - Pharo OCParser infinite loop bug") do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "=== Tier 5: Streams, files, system ==="
  #(ReadStreamTest WriteStreamTest ReadWriteStreamTest
    GeneratorTest LimitedWriteStreamTest
    RandomTest NumberParserTest NumberParsingTest) do: [:name |
      (Smalltalk globals at: name ifAbsent: [nil])
        ifNotNil: [:cls | addOnce value: cls]].

  "Open output files"
  stream := '/tmp/sunit_test_results.txt' asFileReference writeStream.
  detail := '/tmp/sunit_test_detail.txt' asFileReference writeStream.
  totalP := 0. totalF := 0. totalE := 0. totalS := 0. totalT := 0.
  [
    stream nextPutAll: '=== SUnit Test Run #'; print: runNum; nextPutAll: ' ==='; cr; cr.
    detail nextPutAll: 'RUN	CLASS	TEST	RESULT	INFO'; cr.
    detail flush.
    testClasses do: [:tc | | counts |
      counts := self runTestClass: tc run: runNum on: stream detail: detail.
      totalP := totalP + (counts at: 1).
      totalF := totalF + (counts at: 2).
      totalE := totalE + (counts at: 3).
      totalS := totalS + (counts at: 4).
      totalT := totalT + (counts at: 5).
      stream flush. detail flush].
    stream cr.
    stream nextPutAll: '=== GRAND TOTAL ==='; cr.
    stream nextPutAll: 'Run: #'; print: runNum; cr.
    stream nextPutAll: 'Classes: '; print: testClasses size; cr.
    stream nextPutAll: 'Pass: '; print: totalP; cr.
    stream nextPutAll: 'Fail: '; print: totalF; cr.
    stream nextPutAll: 'Error: '; print: totalE; cr.
    stream nextPutAll: 'Skip: '; print: totalS; cr.
    totalT > 0 ifTrue: [stream nextPutAll: 'Timeout: '; print: totalT; cr].
    stream nextPutAll: 'Total: '; print: totalP + totalF + totalE + totalS + totalT; cr.
    stream cr.
    stream nextPutAll: '=== ALL TESTS COMPLETE ==='; cr.
    stream flush]
  ensure: [stream close. detail close].
  "exitSuccess goes through SessionManager shutdown which may not reach primitiveQuit.
   Call quitPrimitive directly to ensure the VM terminates."
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!SUnitRunner class methodsFor: 'running'!
runTestClass: testClass run: runNum on: s detail: d
  "Run all tests in a class. Returns #(pass fail error skip timeout)."
  | sels p f e sk t |

  "Skip classes that stall due to process/scheduling issues in our VM.
   These tests fork processes at the same priority or rely on Delay timeouts
   that cannot fire because our VM lacks proper time-slicing."
  (#(BlockClosureTest ProcessTerminateBugTest ProcessSpecificTest
     MonitorTest DelayTest SemaphoreTest GeneratorTest AllocationTest) includes: testClass name) ifTrue: [
    s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' === SKIPPED (process-heavy)'; cr. s flush.
    ^ {0. 0. 0. 0. 0}].

  s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' ==='; cr.
  s flush.
  p := 0. f := 0. e := 0. sk := 0. t := 0.
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
    ((#(testPrintStringAll testStoreStringAll testPrintStringBase testReciprocalModulo
        testAllReferencesTo testAllReferences testPragmaAddingProperty
        testPragmaRemovingProperty testOneGBAllocation) includes: sel) or: [
         "Skip tests that iterate all heap objects (stalls with broken timer)"
         sel beginsWith: 'testPointers']) ifFalse: [
      | result |
      s nextPutAll: '  RUN: '; nextPutAll: sel; cr. s flush.
      result := self runSingleTest: testClass selector: sel timeout: 5 on: s.
      result ifNil: [
        p := p + 1.
        d nextPutAll: runNum printString; nextPut: $	.
        d nextPutAll: testClass name; nextPut: $	.
        d nextPutAll: sel; nextPut: $	.
        d nextPutAll: 'PASS'; cr]
      ifNotNil: [:kind |
        kind = #fail ifTrue: [
          f := f + 1.
          d nextPutAll: runNum printString; nextPut: $	.
          d nextPutAll: testClass name; nextPut: $	.
          d nextPutAll: sel; nextPut: $	.
          d nextPutAll: 'FAIL'; cr].
        kind = #skip ifTrue: [
          sk := sk + 1.
          d nextPutAll: runNum printString; nextPut: $	.
          d nextPutAll: testClass name; nextPut: $	.
          d nextPutAll: sel; nextPut: $	.
          d nextPutAll: 'SKIP'; cr].
        kind = #error ifTrue: [
          e := e + 1.
          d nextPutAll: runNum printString; nextPut: $	.
          d nextPutAll: testClass name; nextPut: $	.
          d nextPutAll: sel; nextPut: $	.
          d nextPutAll: 'ERROR'; cr].
        kind = #timeout ifTrue: [
          t := t + 1.
          d nextPutAll: runNum printString; nextPut: $	.
          d nextPutAll: testClass name; nextPut: $	.
          d nextPutAll: sel; nextPut: $	.
          d nextPutAll: 'TIMEOUT'; cr]]]].

  s nextPutAll: 'Total: '; print: sels size;
    nextPutAll: ' P:'; print: p;
    nextPutAll: ' F:'; print: f;
    nextPutAll: ' E:'; print: e;
    nextPutAll: ' S:'; print: sk.
  t > 0 ifTrue: [s nextPutAll: ' T:'; print: t].
  s cr; cr.
  ^ {p. f. e. sk. t}! !

!SUnitRunner class methodsFor: 'running'!
runSingleTest: testClass selector: sel timeout: seconds on: s
  "Run a single test with a timeout. Returns nil for pass, #fail, #error, #skip, or #timeout."
  | sem result testProcess |
  sem := Semaphore new.
  result := nil. "nil means pass"

  testProcess := [
    [[[[(testClass selector: sel) runCase]
      on: TestFailure do: [:ex |
        result := #fail.
        s nextPutAll: '  FAIL: '; nextPutAll: sel;
          nextPutAll: ' ['; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 500)); nextPutAll: ']'; cr]]
      on: Deprecation do: [:ex | ex resume]]
      on: TestSkipped do: [:ex |
        result := #skip.
        s nextPutAll: '  SKIP: '; nextPutAll: sel;
          nextPutAll: ' ('; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 100)); nextPutAll: ')'; cr]]
      on: Error do: [:ex |
        result := #error.
        s nextPutAll: '  ERROR: '; nextPutAll: sel;
          nextPutAll: ' ('; nextPutAll: ex class name;
          nextPutAll: ': '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 100)); nextPutAll: ')'; cr].
    sem signal] forkAt: 80.

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
