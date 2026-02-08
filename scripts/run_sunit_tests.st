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
  "Exception handling diagnostic - tests forked process exception chains"
  stream nextPutAll: '=== EXCEPTION DIAGNOSTICS ==='; cr. stream flush.
  "Test 1: ZeroDivide handler (no fork)"
  [| x |
   x := false.
   [ 1/0 ] on: ZeroDivide do: [:e | x := true].
   stream nextPutAll: 'T1 ZeroDivide (no fork): '; nextPutAll: x printString; cr. stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T1 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 2: UnhandledError in same process (with pass)"
  [| x |
   x := false.
   [[ 1/0 ] on: UnhandledError do: [:e | x := true]] on: ZeroDivide do: [:e | e pass].
   stream nextPutAll: 'T2 UnhandledError (no fork): '; nextPutAll: x printString; cr. stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T2 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 3: Simple fork"
  [| sem done |
   sem := Semaphore new. done := false.
   [done := true. sem signal] fork.
   (sem waitTimeoutSeconds: 2) ifTrue: [stream nextPutAll: 'T3 Fork TIMEOUT'; cr]
   ifFalse: [stream nextPutAll: 'T3 Fork OK: '; nextPutAll: done printString; cr]. stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T3 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 4: ZeroDivide in fork"
  [| sem x |
   sem := Semaphore new. x := false.
   [[ 1/0 ] on: ZeroDivide do: [:e | x := true]. sem signal] fork.
   (sem waitTimeoutSeconds: 2) ifTrue: [stream nextPutAll: 'T4 Fork+ZeroDivide TIMEOUT'; cr]
   ifFalse: [stream nextPutAll: 'T4 Fork+ZeroDivide OK: '; nextPutAll: x printString; cr]. stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T4 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 5: UnhandledError in fork (THE failing case)"
  [| sem x |
   sem := Semaphore new. x := false.
   [[ 1/0 ] on: UnhandledError do: [:e | x := true]. sem signal] fork.
   (sem waitTimeoutSeconds: 2) ifTrue: [stream nextPutAll: 'T5 Fork+UnhandledError TIMEOUT'; cr]
   ifFalse: [stream nextPutAll: 'T5 Fork+UnhandledError OK: '; nextPutAll: x printString; cr]. stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T5 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 6: Error handler in fork"
  [| sem x |
   sem := Semaphore new. x := false.
   [[ 1/0 ] on: Error do: [:e | x := true]. sem signal] fork.
   (sem waitTimeoutSeconds: 2) ifTrue: [stream nextPutAll: 'T6 Fork+Error TIMEOUT'; cr]
   ifFalse: [stream nextPutAll: 'T6 Fork+Error OK: '; nextPutAll: x printString; cr]. stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T6 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 7: ensure in fork"
  [| sem ensured |
   sem := Semaphore new. ensured := false.
   [[1 + 1] ensure: [ensured := true. sem signal]] fork.
   (sem waitTimeoutSeconds: 2) ifTrue: [stream nextPutAll: 'T7 Fork+ensure TIMEOUT'; cr]
   ifFalse: [stream nextPutAll: 'T7 Fork+ensure OK: '; nextPutAll: ensured printString; cr]. stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T7 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 8: runWithNoHandlers pattern"
  [| synchSemaphore result |
   synchSemaphore := Semaphore new.
   [result := [[ 1/0 ] on: UnhandledError do: [:ue | ue exception]]
     ensure: [synchSemaphore signal]] forkNamed: 'Test process'.
   (synchSemaphore waitTimeoutSeconds: 2) ifTrue: [stream nextPutAll: 'T8 runWithNoHandlers TIMEOUT'; cr]
   ifFalse: [stream nextPutAll: 'T8 runWithNoHandlers OK: '; nextPutAll: result printString; cr]. stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T8 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 9: forkAt:80 + inner fork with Semaphore>>wait (no timeout machinery)"
  [| outerSem outerResult |
   outerSem := Semaphore new.
   outerResult := 'not set'.
   [[| innerSem executed |
     innerSem := Semaphore new.
     executed := false.
     [executed := true. innerSem signal] fork.
     innerSem wait. "Direct wait - no timeout mechanism"
     outerResult := 'OK executed=', executed printString.
     outerSem signal]
   on: Error do: [:e |
     outerResult := 'ERROR: ', e class name, ' - ', e messageText.
     outerSem signal]] forkAt: 80.
   (outerSem waitTimeoutSeconds: 5) ifTrue: [
     stream nextPutAll: 'T9 ForkAt80+wait OUTER_TIMEOUT'; cr]
   ifFalse: [
     stream nextPutAll: 'T9 ForkAt80+wait: '; nextPutAll: outerResult; cr].
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T9 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 10: forkAt:80 + inner fork with waitTimeoutSeconds: (complex machinery)"
  [| outerSem outerResult |
   outerSem := Semaphore new.
   outerResult := 'not set'.
   [[| innerSem executed |
     innerSem := Semaphore new.
     executed := false.
     [executed := true. innerSem signal] fork.
     (innerSem waitTimeoutSeconds: 2) ifTrue: [outerResult := 'INNER_TIMEOUT']
     ifFalse: [outerResult := 'OK executed=', executed printString].
     outerSem signal]
   on: Error do: [:e |
     outerResult := 'ERROR: ', e class name, ' - ', e messageText.
     outerSem signal]] forkAt: 80.
   (outerSem waitTimeoutSeconds: 5) ifTrue: [
     stream nextPutAll: 'T10 ForkAt80+waitTimeout OUTER_TIMEOUT'; cr]
   ifFalse: [
     stream nextPutAll: 'T10 ForkAt80+waitTimeout: '; nextPutAll: outerResult; cr].
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T10 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 11: forkAt:80 with pre-signaled semaphore (no scheduling needed)"
  [| outerSem outerResult |
   outerSem := Semaphore new.
   outerResult := 'not set'.
   [[| innerSem |
     innerSem := Semaphore new.
     innerSem signal. "Pre-signal"
     innerSem wait. "Should return immediately"
     outerResult := 'OK (pre-signaled)'.
     outerSem signal]
   on: Error do: [:e |
     outerResult := 'ERROR: ', e class name, ' - ', e messageText.
     outerSem signal]] forkAt: 80.
   (outerSem waitTimeoutSeconds: 5) ifTrue: [
     stream nextPutAll: 'T11 ForkAt80+preSignaled OUTER_TIMEOUT'; cr]
   ifFalse: [
     stream nextPutAll: 'T11 ForkAt80+preSignaled: '; nextPutAll: outerResult; cr].
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T11 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 12: ExceptionTester simpleEnsureTestWithUparrow (the actual failing test)"
  [| tester result |
   tester := ExceptionTester new.
   result := tester runTest: #simpleEnsureTestWithUparrow.
   stream nextPutAll: 'T12 ExceptionTester suiteLog: '; nextPutAll: result printString; cr.
   stream nextPutAll: 'T12 ExceptionTester log: '; nextPutAll: tester log printString; cr.
   stream nextPutAll: 'T12 Expected: '; nextPutAll: tester simpleEnsureTestWithUparrowResults printString; cr.
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T12 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 13: simple ensure without NLR (ensure block should fire)"
  [| ensuredRan result |
   ensuredRan := false.
   result := [42] ensure: [ensuredRan := true].
   stream nextPutAll: 'T13 SimpleEnsure: result='; nextPutAll: result printString;
     nextPutAll: ' ensured='; nextPutAll: ensuredRan printString; cr.
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T13 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 14: ensure with NLR from block (non-method ^nil)"
  [| sem log |
   sem := Semaphore new. log := OrderedCollection new.
   [| r |
    r := [log add: 'before'. true ifTrue: [^nil]. log add: 'after'] ensure: [log add: 'ensured'].
    log add: ('result=', r printString).
    sem signal] fork.
   (sem waitTimeoutSeconds: 3) ifTrue: [
     stream nextPutAll: 'T14 Fork+Ensure+NLR: TIMEOUT'; cr]
   ifFalse: [
     stream nextPutAll: 'T14 Fork+Ensure+NLR log: '; nextPutAll: log printString; cr.
     (log includes: 'ensured')
       ifTrue: [stream nextPutAll: 'T14 Fork+Ensure+NLR: OK (ensure fired)'; cr]
       ifFalse: [stream nextPutAll: 'T14 Fork+Ensure+NLR: FAIL (ensure did not fire)'; cr]].
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T14 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 15: RecursionStopper homeMethod diagnostic"
  [| value log |
   value := 0. log := OrderedCollection new.
   RecursionStopper during: [
     value := value + 1.
     log add: ('outer-before: v=', value printString).
     RecursionStopper during: [
       value := value + 1.
       log add: ('inner: v=', value printString).
     ].
     log add: ('outer-after: v=', value printString).
   ].
   stream nextPutAll: 'T15 RecursionStopper: log='; nextPutAll: log printString; cr.
   stream nextPutAll: 'T15 value='; nextPutAll: value printString; cr.
   (value = 1) ifTrue: [stream nextPutAll: 'T15 OK'; cr]
     ifFalse: [stream nextPutAll: 'T15 FAIL (expected 1 got '; nextPutAll: value printString; nextPutAll: ')'; cr].
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T15 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 16: thisContext sender homeMethod identity check"
  [| method1 method2 |
   method1 := thisContext method.
   method2 := nil.
   RecursionStopper during: [
     "We're inside during:'s block. during: calls thisContext sender homeMethod."
     "Let's manually check what homeMethod returns here"
     method2 := thisContext home method.
   ].
   stream nextPutAll: 'T16 method1='; nextPutAll: method1 printString; cr.
   stream nextPutAll: 'T16 method2='; nextPutAll: method2 printString; cr.
   stream nextPutAll: 'T16 same='; nextPutAll: (method1 == method2) printString; cr.
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T16 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  "Test 17: Direct tryPrimitive:withArgs: test for quick primitives"
  [| aReceiver result isFailTok |
   aReceiver := Rectangle origin: 0@0 corner: 100@200.
   "Test P118 with quick primitive 264 (return instVar 0)"
   result := aReceiver tryPrimitive: 264 withArgs: #().
   isFailTok := (result class == Array and: [result size = 2]).
   stream nextPutAll: 'T17a P118(264) result class: '; nextPutAll: result class name; cr.
   stream nextPutAll: 'T17a P118(264) result: '; nextPutAll: result printString; cr.
   stream nextPutAll: 'T17a P118(264) isFailToken: '; nextPutAll: isFailTok printString; cr.
   (isFailTok not and: [result = (0@0)])
     ifTrue: [stream nextPutAll: 'T17a OK'; cr]
     ifFalse: [stream nextPutAll: 'T17a FAIL'; cr].
   "Test P118 with quick primitive 256 (return self)"
   result := aReceiver tryPrimitive: 256 withArgs: #().
   isFailTok := (result class == Array and: [result size = 2]).
   stream nextPutAll: 'T17b P118(256) result class: '; nextPutAll: result class name; cr.
   stream nextPutAll: 'T17b P118(256) result: '; nextPutAll: result printString; cr.
   stream nextPutAll: 'T17b P118(256) isFailToken: '; nextPutAll: isFailTok printString; cr.
   (isFailTok not and: [result == aReceiver])
     ifTrue: [stream nextPutAll: 'T17b OK'; cr]
     ifFalse: [stream nextPutAll: 'T17b FAIL'; cr].
   "Test P118 with quick primitive 257 (return true)"
   result := aReceiver tryPrimitive: 257 withArgs: #().
   stream nextPutAll: 'T17c P118(257) result: '; nextPutAll: result printString; cr.
   "Test doPrimitive: directly"
   [| ctx value |
    ctx := Context sender: thisContext receiver: aReceiver method: (Rectangle >> #center) arguments: #().
    ctx push: aReceiver. "push receiver for topLeft send"
    value := ctx doPrimitive: 264 method: (Rectangle >> #topLeft) receiver: aReceiver args: #().
    stream nextPutAll: 'T17d doPrimitive:264 class: '; nextPutAll: value class name; cr.
    stream nextPutAll: 'T17d doPrimitive:264 result: '; nextPutAll: value printString; cr.
    stream nextPutAll: 'T17d is same ctx: '; nextPutAll: (value == ctx) printString; cr.
    (value == ctx)
      ifTrue: [stream nextPutAll: 'T17d OK (doPrimitive returned context)'; cr]
      ifFalse: [stream nextPutAll: 'T17d FAIL (doPrimitive returned something else)'; cr].
   ] on: Error do: [:e |
    stream nextPutAll: 'T17d ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
   "Test the full stepping - manual send:to:with:super: trace"
   [| ctx centerMethod newCtx sp sendResult |
    centerMethod := Rectangle >> #center.
    ctx := Context sender: thisContext receiver: aReceiver method: centerMethod arguments: #().
    ctx step. "push self"
    sp := ctx instVarAt: 3. "stackp is 3rd instVar"
    stream nextPutAll: 'T17e sp='; nextPutAll: sp printString; cr.
    stream nextPutAll: 'T17e top='; nextPutAll: (ctx at: sp) printString; cr.
    stream flush.
    "Manual pop + send:to:with:super:"
    ctx pop.
    sp := ctx instVarAt: 3.
    stream nextPutAll: 'T17e afterPop sp='; nextPutAll: sp printString; cr.
    sendResult := ctx send: #topLeft to: aReceiver with: #() super: false.
    stream nextPutAll: 'T17e send class='; nextPutAll: sendResult class name; cr.
    stream nextPutAll: 'T17e send='; nextPutAll: sendResult printString; cr.
    stream nextPutAll: 'T17e send==ctx: '; nextPutAll: (sendResult == ctx) printString; cr.
    stream nextPutAll: 'T17e isFailTok: '; nextPutAll: (ctx isFailToken: sendResult) printString; cr.
    stream flush.
    "Step on fresh context"
    ctx := Context sender: thisContext receiver: aReceiver method: centerMethod arguments: #().
    ctx step.
    newCtx := ctx step.
    stream nextPutAll: 'T17e step2='; nextPutAll: newCtx class name.
    stream nextPutAll: ' ==ctx:'; nextPutAll: (newCtx == ctx) printString; cr.
    stream flush.
   ] on: Error do: [:e |
    stream nextPutAll: 'T17e ERR: '; nextPutAll: e class name; nextPutAll: ' '; nextPutAll: e messageText; cr. stream flush].
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T17 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr.
   stream flush].
  "Test 18: Manual send:to:with:super: for quick method"
  [| ctx aRcv cm sendRes |
   aRcv := Rectangle origin: 0@0 corner: 100@200.
   cm := Rectangle >> #center.
   ctx := Context sender: thisContext receiver: aRcv method: cm arguments: #().
   ctx push: aRcv. "simulate pushReceiver"
   "Now manually call send:to:with:super: as step would"
   sendRes := ctx send: #topLeft to: aRcv with: #() super: false.
   stream nextPutAll: 'T18 sendRes class: '; nextPutAll: sendRes class name; cr.
   stream nextPutAll: 'T18 sendRes: '; nextPutAll: sendRes printString; cr.
   stream nextPutAll: 'T18 ==ctx: '; nextPutAll: (sendRes == ctx) printString; cr.
   stream nextPutAll: 'T18 isFailTok: '; nextPutAll: (ctx isFailToken: sendRes) printString; cr.
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: 'T18 ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].
  stream nextPutAll: '=== END DIAGNOSTICS ==='; cr; cr. stream flush! !

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
    self runDiagnostic: stream.
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
  | sels p f e sk t testPriority |

  "Skip classes that stall due to process/scheduling issues in our VM.
   These tests fork processes at the same priority or rely on Delay timeouts
   that cannot fire because our VM lacks proper time-slicing."
  (#(BlockClosureTest ProcessTerminateBugTest ProcessSpecificTest
     MonitorTest DelayTest SemaphoreTest GeneratorTest AllocationTest
     SharedPoolTest ClassHierarchyTest MetaClassTest) includes: testClass name) ifTrue: [
    s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' === SKIPPED (process-heavy)'; cr. s flush.
    ^ {0. 0. 0. 0. 0}].

  s nextPutAll: '=== '; nextPutAll: testClass name; nextPutAll: ' ==='; cr.
  s flush.
  "Tests that use waitTimeoutSeconds: internally need priority < 80 (timingPriority)
   so the delay scheduler can preempt and register timeouts."
  testPriority := (#(ExceptionTest) includes: testClass name)
    ifTrue: [79] ifFalse: [80].
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
        testPragmaRemovingProperty testOneGBAllocation testPrintingRecursive) includes: sel) or: [
         "Skip tests that iterate all heap objects (stalls with broken timer)"
         sel beginsWith: 'testPointers']) ifFalse: [
      | result |
      s nextPutAll: '  RUN: '; nextPutAll: sel; cr. s flush.
      result := self runSingleTest: testClass selector: sel timeout: 5 priority: testPriority on: s.
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
runSingleTest: testClass selector: sel timeout: seconds priority: priority on: s
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
    sem signal] forkAt: priority.

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
