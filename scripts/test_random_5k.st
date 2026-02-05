"Test Random with 5000+ iterations to match heap example"!

Object subclass: #Random5KTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!Random5KTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/random_5k.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Random 5K Test ==='; cr; cr.

    "Test 1: 5000 next calls"
    stream nextPutAll: '--- Test 1: 5000 next calls ---'; cr.
    [| rnd |
     rnd := Random new.
     stream nextPutAll: 'Starting 5000 next calls...'; cr. stream flush.
     1 to: 5000 do: [:i |
       rnd next.
       i \\ 1000 = 0 ifTrue: [
         stream nextPutAll: 'Iteration '; nextPutAll: i printString; cr. stream flush]].
     stream nextPutAll: 'Test 1: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 1 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 2: 5000 nextInteger: with varying sizes"
    stream cr; nextPutAll: '--- Test 2: nextInteger: with sizes 5000 down to 1 ---'; cr.
    [| rnd |
     rnd := Random new.
     stream nextPutAll: 'Starting 5000 nextInteger: calls (5000 down to 1)...'; cr. stream flush.
     5000 to: 1 by: -1 do: [:i |
       rnd nextInteger: i.
       i \\ 1000 = 0 ifTrue: [
         stream nextPutAll: 'nextInteger: '; nextPutAll: i printString; cr. stream flush]].
     stream nextPutAll: 'Test 2: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 3: Direct shuffle simulation"
    stream cr; nextPutAll: '--- Test 3: Shuffle simulation (5000 elements) ---'; cr.
    [| rnd arr |
     arr := (1 to: 5000) asArray.
     rnd := Random new.
     stream nextPutAll: 'Array size: '; nextPutAll: arr size printString; cr. stream flush.
     5000 to: 2 by: -1 do: [:i |
       | j |
       j := rnd nextInteger: i.
       arr swap: i with: j.
       i \\ 1000 = 0 ifTrue: [
         stream nextPutAll: 'Shuffled through index '; nextPutAll: i printString; cr. stream flush]].
     stream nextPutAll: 'Shuffle complete'; cr.
     stream nextPutAll: 'Test 3: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 4: Actual shuffled method"
    stream cr; nextPutAll: '--- Test 4: Actual shuffled on 5000 array ---'; cr.
    [| arr shuffled |
     arr := (1 to: 5000) asArray.
     stream nextPutAll: 'Calling shuffled...'; cr. stream flush.
     shuffled := arr shuffled.
     stream nextPutAll: 'Shuffled size: '; nextPutAll: shuffled size printString; cr.
     (shuffled includes: nil)
       ifTrue: [stream nextPutAll: 'ERROR: contains nil'; cr]
       ifFalse: [stream nextPutAll: 'No nils in result'; cr].
     stream nextPutAll: 'Test 4: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 5: heapExample simulation with n=5000"
    stream cr; nextPutAll: '--- Test 5: heapExample simulation n=5000 ---'; cr.
    [| n rnd array sorted |
     n := 5000.
     rnd := Random new.
     stream nextPutAll: 'Creating array with collect...'; cr. stream flush.
     array := (1 to: n) collect: [:i | rnd next].
     stream nextPutAll: 'Array size: '; nextPutAll: array size printString; cr.
     stream nextPutAll: 'First 3: '.
     1 to: 3 do: [:i | stream nextPutAll: (array at: i) printString; nextPutAll: ' '].
     stream cr. stream flush.
     (array includes: nil)
       ifTrue: [stream nextPutAll: 'ERROR: array contains nil'; cr. self error: 'nil in array']
       ifFalse: [stream nextPutAll: 'No nils in array'; cr].
     stream nextPutAll: 'Creating Heap withAll...'; cr. stream flush.
     sorted := Heap withAll: array.
     stream nextPutAll: 'Heap created, size='; nextPutAll: sorted size printString; cr.
     stream nextPutAll: 'Test 5: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 5 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!Random5KTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!Random5KTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #Random5KTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
