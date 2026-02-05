"Test individual heap operations to find SubscriptOutOfBounds source"!

Object subclass: #HeapOpsTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!HeapOpsTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/heap_ops.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Heap Operations Test ==='; cr; cr.

    "Test 1: bitShift: -1 with large numbers"
    stream nextPutAll: '--- Test 1: bitShift: -1 ---'; cr.
    [{5000. 5001. 4999. 10000. 1} do: [:n |
       | result |
       result := n bitShift: -1.
       stream nextPutAll: n printString; nextPutAll: ' >> 1 = '; nextPutAll: result printString; cr].
     stream nextPutAll: 'Test 1: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 1 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 2: Array access with index 5000"
    stream cr; nextPutAll: '--- Test 2: Large array access ---'; cr.
    [| arr |
     arr := Array new: 5000.
     stream nextPutAll: 'Created array size='; nextPutAll: arr size printString; cr.
     arr at: 1 put: #first.
     arr at: 5000 put: #last.
     arr at: 2500 put: #middle.
     stream nextPutAll: 'arr at: 1 = '; nextPutAll: (arr at: 1) printString; cr.
     stream nextPutAll: 'arr at: 2500 = '; nextPutAll: (arr at: 2500) printString; cr.
     stream nextPutAll: 'arr at: 5000 = '; nextPutAll: (arr at: 5000) printString; cr.
     stream nextPutAll: 'Test 2: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 3: tally // 2 to: 1 by: -1 iteration"
    stream cr; nextPutAll: '--- Test 3: Loop iteration 2500 to 1 ---'; cr.
    [| count tally |
     tally := 5000.
     count := 0.
     tally // 2 to: 1 by: -1 do: [:i | count := count + 1].
     stream nextPutAll: 'Iterations: '; nextPutAll: count printString; cr.
     count = 2500
       ifTrue: [stream nextPutAll: 'Test 3: PASS'; cr]
       ifFalse: [stream nextPutAll: 'Test 3 FAIL: expected 2500 iterations'; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 4: downHeap simulation - just the array accesses"
    stream cr; nextPutAll: '--- Test 4: downHeap index calculations ---'; cr.
    [| arr tally n k j accesses |
     tally := 5000.
     arr := Array new: tally.
     1 to: tally do: [:i | arr at: i put: i].
     n := tally bitShift: -1.
     stream nextPutAll: 'tally='; nextPutAll: tally printString;
       nextPutAll: ' n='; nextPutAll: n printString; cr.
     accesses := 0.
     "Simulate downHeap: 1 which goes deepest"
     k := 1.
     [k <= n] whileTrue: [
       j := k + k.
       "Access array at: j"
       arr at: j.
       accesses := accesses + 1.
       "If j < tally, also access j+1"
       j < tally ifTrue: [
         arr at: j + 1.
         accesses := accesses + 1].
       "Move to child"
       k := j].
     stream nextPutAll: 'Accesses: '; nextPutAll: accesses printString; cr.
     stream nextPutAll: 'Final k='; nextPutAll: k printString; cr.
     stream nextPutAll: 'Test 4: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 5: Full downHeap simulation with sorts:"
    stream cr; nextPutAll: '--- Test 5: Full downHeap: simulation ---'; cr.
    [| arr tally n k j value |
     tally := 5000.
     arr := (1 to: tally) asArray shuffled.
     n := tally bitShift: -1.
     stream nextPutAll: 'Starting downHeap: 1 simulation'; cr.
     k := 1.
     value := arr at: 1.
     [k <= n] whileTrue: [
       j := k + k.
       "Compare j and j+1 to find which child is smaller"
       (j < tally and: [(arr at: j + 1) <= (arr at: j)])
         ifTrue: [j := j + 1].
       "Check if position k is ok"
       (value <= (arr at: j))
         ifTrue: [n := k - 1]  "break loop"
         ifFalse: [
           arr at: k put: (arr at: j).
           k := j]].
     arr at: k put: value.
     stream nextPutAll: 'Test 5: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 5 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 6: Full reSort simulation"
    stream cr; nextPutAll: '--- Test 6: Full reSort simulation ---'; cr.
    [| arr tally |
     tally := 5000.
     arr := (1 to: tally) asArray shuffled.
     stream nextPutAll: 'Array created, size='; nextPutAll: arr size printString; cr.
     stream nextPutAll: 'Starting reSort loop (2500 to 1)...'; cr.
     tally // 2 to: 1 by: -1 do: [:startIndex |
       | n k j value |
       n := tally bitShift: -1.
       k := startIndex.
       value := arr at: startIndex.
       [k <= n] whileTrue: [
         j := k + k.
         (j < tally and: [(arr at: j + 1) <= (arr at: j)])
           ifTrue: [j := j + 1].
         (value <= (arr at: j))
           ifTrue: [n := k - 1]
           ifFalse: [
             arr at: k put: (arr at: j).
             k := j]].
       arr at: k put: value.
       startIndex \\ 500 = 0 ifTrue: [
         stream nextPutAll: 'Processed index '; nextPutAll: startIndex printString; cr. stream flush]].
     stream nextPutAll: 'Test 6: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 6 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 7: Actual Heap withAll:"
    stream cr; nextPutAll: '--- Test 7: Heap withAll: 5000 elements ---'; cr.
    [| arr heap |
     arr := (1 to: 5000) asArray shuffled.
     stream nextPutAll: 'Array created size='; nextPutAll: arr size printString; cr.
     heap := Heap withAll: arr.
     stream nextPutAll: 'Heap created size='; nextPutAll: heap size printString; cr.
     stream nextPutAll: 'Test 7: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 7 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!HeapOpsTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!HeapOpsTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #HeapOpsTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
