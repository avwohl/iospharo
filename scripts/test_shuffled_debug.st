"Debug shuffled method to find SubscriptOutOfBounds"!

Object subclass: #ShuffledDebugTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!ShuffledDebugTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/shuffled_debug.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Shuffled Debug Test ==='; cr; cr.

    "Test 1: Check copy first"
    stream nextPutAll: '--- Test 1: Array copy ---'; cr.
    [| arr copy |
     arr := (1 to: 5000) asArray.
     stream nextPutAll: 'Original size: '; nextPutAll: arr size printString; cr.
     copy := arr copy.
     stream nextPutAll: 'Copy size: '; nextPutAll: copy size printString; cr.
     stream nextPutAll: 'Same class: '; nextPutAll: (copy class = arr class) printString; cr.
     stream nextPutAll: 'Test 1: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 1 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 2: Check swap method"
    stream cr; nextPutAll: '--- Test 2: Array swap ---'; cr.
    [| arr |
     arr := (1 to: 100) asArray.
     stream nextPutAll: 'Before swap: arr at: 1 = '; nextPutAll: (arr at: 1) printString;
       nextPutAll: ' arr at: 50 = '; nextPutAll: (arr at: 50) printString; cr.
     arr swap: 1 with: 50.
     stream nextPutAll: 'After swap: arr at: 1 = '; nextPutAll: (arr at: 1) printString;
       nextPutAll: ' arr at: 50 = '; nextPutAll: (arr at: 50) printString; cr.
     stream nextPutAll: 'Test 2: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 3: Check shuffleBy: step by step"
    stream cr; nextPutAll: '--- Test 3: shuffleBy: step by step on copy ---'; cr.
    [| arr rnd |
     arr := (1 to: 5000) asArray copy.
     rnd := Random new.
     stream nextPutAll: 'Array size: '; nextPutAll: arr size printString; cr. stream flush.
     arr size to: 2 by: -1 do: [:i |
       | j |
       [j := rnd nextInteger: i] on: Error do: [:ex |
         stream nextPutAll: 'nextInteger: error at i='; nextPutAll: i printString;
           nextPutAll: ' - '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr.
         stream flush.
         ex pass].
       [arr swap: i with: j] on: Error do: [:ex |
         stream nextPutAll: 'swap error at i='; nextPutAll: i printString; nextPutAll: ' j='; nextPutAll: j printString;
           nextPutAll: ' arrSize='; nextPutAll: arr size printString;
           nextPutAll: ' - '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr.
         stream flush.
         ex pass].
       i \\ 1000 = 0 ifTrue: [
         stream nextPutAll: 'Progress: i='; nextPutAll: i printString; cr. stream flush]].
     stream nextPutAll: 'Test 3: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3 FAIL (outer): '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 4: Call shuffle directly"
    stream cr; nextPutAll: '--- Test 4: Direct shuffle call ---'; cr.
    [| arr |
     arr := (1 to: 5000) asArray copy.
     stream nextPutAll: 'Array size before shuffle: '; nextPutAll: arr size printString; cr. stream flush.
     arr shuffle.
     stream nextPutAll: 'Array size after shuffle: '; nextPutAll: arr size printString; cr.
     stream nextPutAll: 'Test 4: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    "Test 5: Test shuffled on smaller sizes to find threshold"
    stream cr; nextPutAll: '--- Test 5: Find shuffle threshold ---'; cr.
    {100. 500. 1000. 2000. 3000. 4000. 5000} do: [:size |
      stream nextPutAll: 'Testing size='; nextPutAll: size printString; nextPutAll: '... '.
      [| arr result |
       arr := (1 to: size) asArray.
       result := arr shuffled.
       stream nextPutAll: 'PASS'; cr]
      on: Error do: [:ex |
        stream nextPutAll: 'FAIL - '; nextPutAll: ex class name; cr].
      stream flush].
    stream nextPutAll: 'Test 5: done'; cr.
    stream flush.

    "Test 6: Check Random new in shuffleBy: context"
    stream cr; nextPutAll: '--- Test 6: Random in shuffle context ---'; cr.
    [| arr |
     arr := (1 to: 5000) asArray copy.
     stream nextPutAll: 'About to call shuffleBy: Random new'; cr. stream flush.
     arr shuffleBy: Random new.
     stream nextPutAll: 'shuffleBy: completed'; cr.
     stream nextPutAll: 'Test 6: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 6 FAIL: '; nextPutAll: ex class name; nextPutAll: ': '; nextPutAll: ex messageText; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!ShuffledDebugTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!ShuffledDebugTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #ShuffledDebugTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
