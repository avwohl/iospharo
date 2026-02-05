"Test Random class to find the SubscriptOutOfBounds source"!

Object subclass: #RandomTest2
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!RandomTest2 class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/random_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Random Test ==='; cr; cr.

    "Test 1: Random new"
    stream nextPutAll: '--- Test 1: Random new ---'; cr.
    [| rnd |
     rnd := Random new.
     stream nextPutAll: 'Random new = '; nextPutAll: rnd class name; cr.
     rnd isNil ifTrue: [stream nextPutAll: 'ERROR: Random new returned nil'; cr].
     stream nextPutAll: 'Test 1: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 1 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 2: Random next"
    stream cr; nextPutAll: '--- Test 2: Random next ---'; cr.
    [| rnd val |
     rnd := Random new.
     stream nextPutAll: 'Random created, class='; nextPutAll: rnd class name; cr.
     val := rnd next.
     stream nextPutAll: 'Random next = '; nextPutAll: val printString; cr.
     val isNil ifTrue: [stream nextPutAll: 'ERROR: next returned nil'; cr].
     stream nextPutAll: 'Test 2: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 3: Random nextInteger:"
    stream cr; nextPutAll: '--- Test 3: Random nextInteger: 100 ---'; cr.
    [| rnd val |
     rnd := Random new.
     val := rnd nextInteger: 100.
     stream nextPutAll: 'Random nextInteger: 100 = '; nextPutAll: val printString; cr.
     val isNil ifTrue: [stream nextPutAll: 'ERROR: nextInteger: returned nil'; cr].
     stream nextPutAll: 'Test 3: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 4: Generate 10 random numbers"
    stream cr; nextPutAll: '--- Test 4: Generate 10 randoms ---'; cr.
    [| rnd |
     rnd := Random new.
     1 to: 10 do: [:i |
       | val |
       val := rnd next.
       stream nextPutAll: i printString; nextPutAll: ': '; nextPutAll: val printString.
       val isNil ifTrue: [stream nextPutAll: ' NIL'].
       stream cr].
     stream nextPutAll: 'Test 4: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 5: collect with Random"
    stream cr; nextPutAll: '--- Test 5: collect: with Random ---'; cr.
    [| rnd arr |
     rnd := Random new.
     arr := (1 to: 10) collect: [:i | rnd next].
     stream nextPutAll: 'Collected: '.
     arr do: [:each |
       stream nextPutAll: each printString; nextPutAll: ' '.
       each isNil ifTrue: [stream nextPutAll: 'NIL ']].
     stream cr.
     stream nextPutAll: 'Test 5: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 5 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 6: shuffled on small array"
    stream cr; nextPutAll: '--- Test 6: shuffled on small array ---'; cr.
    [| arr shuffled |
     arr := #(1 2 3 4 5).
     shuffled := arr shuffled.
     stream nextPutAll: 'Original: '; nextPutAll: arr printString; cr.
     stream nextPutAll: 'Shuffled: '; nextPutAll: shuffled printString; cr.
     (shuffled includes: nil) ifTrue: [stream nextPutAll: 'ERROR: shuffled contains nil'; cr].
     stream nextPutAll: 'Test 6: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 6 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 7: shuffled on 100 element array"
    stream cr; nextPutAll: '--- Test 7: shuffled on 100 element array ---'; cr.
    [| arr shuffled |
     arr := (1 to: 100) asArray.
     shuffled := arr shuffled.
     stream nextPutAll: 'Array size: '; nextPutAll: arr size printString; cr.
     stream nextPutAll: 'Shuffled size: '; nextPutAll: shuffled size printString; cr.
     (shuffled includes: nil)
       ifTrue: [stream nextPutAll: 'ERROR: shuffled contains nil at: '.
                1 to: shuffled size do: [:i |
                  (shuffled at: i) isNil ifTrue: [stream nextPutAll: i printString; nextPutAll: ' ']].
                stream cr]
       ifFalse: [stream nextPutAll: 'No nils in shuffled'; cr].
     stream nextPutAll: 'Test 7: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 7 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 8: shuffled on 1000 element array"
    stream cr; nextPutAll: '--- Test 8: shuffled on 1000 element array ---'; cr.
    [| arr shuffled |
     arr := (1 to: 1000) asArray.
     shuffled := arr shuffled.
     stream nextPutAll: 'Array size: '; nextPutAll: arr size printString; cr.
     stream nextPutAll: 'Shuffled size: '; nextPutAll: shuffled size printString; cr.
     (shuffled includes: nil)
       ifTrue: [stream nextPutAll: 'ERROR: shuffled contains nil'; cr]
       ifFalse: [stream nextPutAll: 'No nils in shuffled'; cr].
     stream nextPutAll: 'Test 8: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 8 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 9: Heap withAll with manually created random array (no shuffled)"
    stream cr; nextPutAll: '--- Test 9: Heap withAll with manual random array ---'; cr.
    [| rnd arr heap |
     rnd := Random new.
     arr := Array new: 100.
     1 to: 100 do: [:i | arr at: i put: (rnd nextInteger: 10000)].
     stream nextPutAll: 'Created array size='; nextPutAll: arr size printString; cr.
     stream nextPutAll: 'First 5: '.
     1 to: 5 do: [:i | stream nextPutAll: (arr at: i) printString; nextPutAll: ' '].
     stream cr.
     (arr includes: nil) ifTrue: [stream nextPutAll: 'ERROR: Array has nils'; cr].
     heap := Heap withAll: arr.
     stream nextPutAll: 'Heap size='; nextPutAll: heap size printString; cr.
     stream nextPutAll: 'Test 9: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 9 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!RandomTest2 class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!RandomTest2 class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #RandomTest2)
  inCategory: SessionManager default userCategory
  atPriority: 90!
