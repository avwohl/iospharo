"Test factorial to find divergence point"!

Object subclass: #FactorialTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!FactorialTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/factorial_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Factorial Test ==='; cr; cr.

    "Test 1: Small factorials (should all match)"
    stream nextPutAll: '--- Test 1: Small factorials (0-20) ---'; cr.
    0 to: 20 do: [:n |
      | slow fast |
      slow := n slowFactorial.
      fast := n factorial.
      slow = fast
        ifFalse: [stream nextPutAll: 'MISMATCH at n='; print: n; cr.
                  stream nextPutAll: '  slow='; print: slow; cr.
                  stream nextPutAll: '  fast='; print: fast; cr]
        ifTrue: [(n <= 10 or: [n = 20]) ifTrue: [
                  stream nextPutAll: n printString; nextPutAll: ': '; print: slow; cr]]].
    stream nextPutAll: 'Test 1: done'; cr.
    stream flush.

    "Test 2: Find first mismatch up to 200"
    stream cr; nextPutAll: '--- Test 2: Find first mismatch (0-200) ---'; cr.
    [| firstMismatch |
     firstMismatch := nil.
     0 to: 200 do: [:n |
       | slow fast |
       slow := n slowFactorial.
       fast := n factorial.
       slow = fast ifFalse: [
         firstMismatch ifNil: [
           firstMismatch := n.
           stream nextPutAll: 'FIRST MISMATCH at n='; print: n; cr.
           stream nextPutAll: '  slow size='; print: slow printString size; cr.
           stream nextPutAll: '  fast size='; print: fast printString size; cr.
           stream nextPutAll: '  slow class='; print: slow class name; cr.
           stream nextPutAll: '  fast class='; print: fast class name; cr]]].
     firstMismatch
       ifNil: [stream nextPutAll: 'All 0-200 factorials match'; cr]
       ifNotNil: [stream nextPutAll: 'First mismatch at: '; print: firstMismatch; cr].
     stream nextPutAll: 'Test 2: done'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 2 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 3: Check specific multiplication"
    stream cr; nextPutAll: '--- Test 3: Large multiplication check ---'; cr.
    [| a b prod expected |
     "100 factorial is about 158 digits"
     a := 100 factorial.
     b := 101.
     prod := a * b.
     expected := 101 factorial.
     stream nextPutAll: '100 fact * 101 = 101 fact? '.
     prod = expected
       ifTrue: [stream nextPutAll: 'YES'; cr]
       ifFalse: [stream nextPutAll: 'NO'; cr.
                 stream nextPutAll: '  prod size='; print: prod printString size; cr.
                 stream nextPutAll: '  expected size='; print: expected printString size; cr].
     stream nextPutAll: 'Test 3: done'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 3 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Test 4: Incremental factorial check"
    stream cr; nextPutAll: '--- Test 4: Incremental factorial check ---'; cr.
    [| prevSlow prevFast |
     prevSlow := 1.
     prevFast := 1.
     1 to: 300 do: [:n |
       | currSlow currFast |
       currSlow := prevSlow * n.
       currFast := n factorial.
       currSlow = currFast ifFalse: [
         stream nextPutAll: 'Incremental mismatch at n='; print: n; cr.
         stream nextPutAll: '  prev*n = '; print: currSlow printString size; nextPutAll: ' digits'; cr.
         stream nextPutAll: '  n fact = '; print: currFast printString size; nextPutAll: ' digits'; cr.
         stream nextPutAll: '  prevSlow = prevFast? '; print: (prevSlow = prevFast); cr].
       prevSlow := currSlow.
       prevFast := currFast.
       n \\ 50 = 0 ifTrue: [
         stream nextPutAll: 'n='; print: n; nextPutAll: ' OK ('; print: currSlow printString size; nextPutAll: ' digits)'; cr.
         stream flush]].
     stream nextPutAll: 'Test 4: done'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test 4 ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!FactorialTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!FactorialTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #FactorialTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
