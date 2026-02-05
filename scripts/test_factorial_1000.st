"Test factorial up to 1000 to match testSlowFactorial"!

Object subclass: #Factorial1000Test
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!Factorial1000Test class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/factorial_1000.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Factorial 1000 Test ==='; cr; cr.

    "Test: Compare slowFactorial vs factorial for 0-1000"
    stream nextPutAll: '--- Comparing slowFactorial vs factorial (0-1000) ---'; cr.
    [| mismatchCount |
     mismatchCount := 0.
     0 to: 1000 do: [:n |
       | slow fast |
       slow := n slowFactorial.
       fast := n factorial.
       slow = fast ifFalse: [
         mismatchCount := mismatchCount + 1.
         mismatchCount <= 5 ifTrue: [
           stream nextPutAll: 'MISMATCH at n='; print: n; cr.
           stream nextPutAll: '  slow digits='; print: slow printString size; cr.
           stream nextPutAll: '  fast digits='; print: fast printString size; cr]].
       n \\ 100 = 0 ifTrue: [
         stream nextPutAll: 'n='; print: n; nextPutAll: ' checked ('; print: slow printString size; nextPutAll: ' digits)'; cr.
         stream flush]].
     mismatchCount = 0
       ifTrue: [stream nextPutAll: 'ALL 1001 FACTORIALS MATCH'; cr]
       ifFalse: [stream nextPutAll: 'TOTAL MISMATCHES: '; print: mismatchCount; cr].
     stream nextPutAll: 'Test: done'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Test ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!Factorial1000Test class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!Factorial1000Test class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #Factorial1000Test)
  inCategory: SessionManager default userCategory
  atPriority: 90!
