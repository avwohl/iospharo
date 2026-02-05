"Run testSlowFactorial directly without SUnit framework"!

Object subclass: #SlowFactorialDirect
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!SlowFactorialDirect class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/slowfactorial_direct.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== testSlowFactorial Direct ==='; cr; cr.

    "Exact test from testSlowFactorial"
    stream nextPutAll: '--- Check known values ---'; cr.
    [
      "Check some known values"
      (0 slowFactorial = 1) ifFalse: [stream nextPutAll: 'FAIL: 0 slowFactorial'; cr].
      (1 slowFactorial = 1) ifFalse: [stream nextPutAll: 'FAIL: 1 slowFactorial'; cr].
      (2 slowFactorial = 2) ifFalse: [stream nextPutAll: 'FAIL: 2 slowFactorial'; cr].
      (3 slowFactorial = 6) ifFalse: [stream nextPutAll: 'FAIL: 3 slowFactorial'; cr].
      (4 slowFactorial = 24) ifFalse: [stream nextPutAll: 'FAIL: 4 slowFactorial'; cr].
      (5 slowFactorial = 120) ifFalse: [stream nextPutAll: 'FAIL: 5 slowFactorial'; cr].
      (6 slowFactorial = 720) ifFalse: [stream nextPutAll: 'FAIL: 6 slowFactorial'; cr].
      stream nextPutAll: 'Known values: PASS'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'Known values ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    "Compare fast implementation with existing one for 0-1000"
    stream cr; nextPutAll: '--- Compare with factorial (0-1000) ---'; cr.
    [| failCount |
     failCount := 0.
     0 to: 1000 do: [:idx |
       (idx slowFactorial = idx factorial) ifFalse: [
         failCount := failCount + 1.
         failCount <= 3 ifTrue: [
           stream nextPutAll: 'MISMATCH at idx='; print: idx; cr]].
       idx \\ 200 = 0 ifTrue: [
         stream nextPutAll: 'Progress: '; print: idx; cr. stream flush]].
     failCount = 0
       ifTrue: [stream nextPutAll: 'All 1001 comparisons: PASS'; cr]
       ifFalse: [stream nextPutAll: 'FAIL count: '; print: failCount; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'Comparison ERROR: '; print: ex class name; nextPutAll: ': '; print: ex messageText; cr].
    stream flush.

    stream cr.
    stream nextPutAll: '=== DONE ==='; cr.
    stream flush]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!SlowFactorialDirect class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!SlowFactorialDirect class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #SlowFactorialDirect)
  inCategory: SessionManager default userCategory
  atPriority: 90!
