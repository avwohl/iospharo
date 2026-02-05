"Test Random state to find when it becomes nil"!

Object subclass: #RandomStateTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!RandomStateTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/random_state.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Random State Test ==='; cr; cr.

    "Test 1: Examine state array"
    stream nextPutAll: '--- Test 1: Random state structure ---'; cr.
    [| rnd |
     rnd := Random new.
     stream nextPutAll: 'state class: '; nextPutAll: rnd state class name; cr.
     stream nextPutAll: 'state size: '; nextPutAll: rnd state size printString; cr.
     stream nextPutAll: 'state at: 1 = '; nextPutAll: (rnd state at: 1) printString; cr.
     stream nextPutAll: 'Test 1: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 1 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 2: Track state through iterations"
    stream cr; nextPutAll: '--- Test 2: State tracking (100 iters) ---'; cr.
    [| rnd |
     rnd := Random new.
     1 to: 100 do: [:i |
       | val stateVal |
       stateVal := rnd state at: 1.
       val := rnd next.
       (i <= 5 or: [i \\ 20 = 0]) ifTrue: [
         stream nextPutAll: i printString; nextPutAll: ': state='; nextPutAll: stateVal printString;
           nextPutAll: ' next='; nextPutAll: val printString; cr.
         stateVal isNil ifTrue: [stream nextPutAll: ' STATE IS NIL'; cr]].
       stream flush].
     stream nextPutAll: 'Test 2: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 3: Run 1000 nextInteger: operations"
    stream cr; nextPutAll: '--- Test 3: 1000 nextInteger: operations ---'; cr.
    [| rnd |
     rnd := Random new.
     1 to: 1000 do: [:i |
       | stateVal val |
       stateVal := rnd state at: 1.
       stateVal isNil ifTrue: [
         stream nextPutAll: 'STATE NIL at iteration '; nextPutAll: i printString; cr.
         self error: 'State is nil'].
       val := rnd nextInteger: 1000.
       (i <= 3 or: [i \\ 100 = 0]) ifTrue: [
         stream nextPutAll: i printString; nextPutAll: ': nextInteger: 1000 = '; nextPutAll: val printString; cr].
       stream flush].
     stream nextPutAll: 'Test 3: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 4: DoubleWordArray at:put: test"
    stream cr; nextPutAll: '--- Test 4: DoubleWordArray test ---'; cr.
    [| arr bigNum |
     arr := DoubleWordArray new: 1.
     stream nextPutAll: 'Created DoubleWordArray'; cr.
     bigNum := 16r123456789ABCDEF0.
     arr at: 1 put: bigNum.
     stream nextPutAll: 'Stored: '; nextPutAll: bigNum printString; cr.
     stream nextPutAll: 'Retrieved: '; nextPutAll: (arr at: 1) printString; cr.
     (arr at: 1) = bigNum
       ifTrue: [stream nextPutAll: 'Test 4: PASS'; cr]
       ifFalse: [stream nextPutAll: 'Test 4: FAIL - values differ'; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 5: Large number operations"
    stream cr; nextPutAll: '--- Test 5: Large number operations ---'; cr.
    [| n result |
     n := 16rFFFFFFFFFFFFFFFF.
     stream nextPutAll: 'n = 16rFFFFFFFFFFFFFFFF'; cr.
     stream nextPutAll: 'n class: '; nextPutAll: n class name; cr.
     result := n >> 59.
     stream nextPutAll: 'n >> 59 = '; nextPutAll: result printString; cr.
     result := n * 6364136223846793005.
     stream nextPutAll: 'n * big = '; nextPutAll: result printString; cr.
     result := result bitAnd: 16rFFFFFFFFFFFFFFFF.
     stream nextPutAll: 'result bitAnd: mask = '; nextPutAll: result printString; cr.
     stream nextPutAll: 'Test 5: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 5 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 6: Track through 2000 iterations looking for nil"
    stream cr; nextPutAll: '--- Test 6: 2000 iterations looking for nil state ---'; cr.
    [| rnd nilFound |
     rnd := Random new.
     nilFound := false.
     1 to: 2000 do: [:i |
       | stateVal |
       stateVal := rnd state at: 1.
       stateVal isNil ifTrue: [
         nilFound := true.
         stream nextPutAll: 'NIL STATE at iteration '; nextPutAll: i printString; cr.
         stream flush.
         i > 1 ifTrue: [self error: 'State became nil']].
       rnd next].
     nilFound
       ifTrue: [stream nextPutAll: 'Test 6: FAIL - found nil state'; cr]
       ifFalse: [stream nextPutAll: 'Test 6: PASS'; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 6 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!RandomStateTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!RandomStateTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #RandomStateTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
