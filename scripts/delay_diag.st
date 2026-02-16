"Delay diagnostic"!

Object subclass: #DelayDiag
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

UndefinedObject compile: 'findNextHandlerContext
  ^ nil' classified: 'private'!

!DelayDiag class methodsFor: 'running'!
run
  | stream t1 t2 sem done stuck result watchdog |
  stream := '/tmp/delay_diag.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Delay Diagnostic ==='; cr.

    "Test 1: Does Delay class exist and respond?"
    stream nextPutAll: 'Delay class: '; nextPutAll: Delay name; cr.
    stream flush.

    "Test 2: Simple delay in main process"
    stream nextPutAll: 'Before delay... '; flush.
    t1 := DateAndTime now.
    (Delay forMilliseconds: 100) wait.
    t2 := DateAndTime now.
    stream nextPutAll: 'After delay. Duration: '; nextPutAll: (t2 - t1) asMilliSeconds printString; nextPutAll: 'ms'; cr.
    stream flush.

    "Test 3: Delay with forked process"
    sem := Semaphore new. done := false.
    [
      (Delay forMilliseconds: 200) wait.
      done := true.
      sem signal] fork.
    stream nextPutAll: 'Forked delay process...'; cr. stream flush.
    sem wait.
    stream nextPutAll: 'Forked delay done: '; nextPutAll: done printString; cr.
    stream flush.

    "Test 4: Timeout preemption - stuck loop at 79, watchdog at 90"
    result := 'none'.
    stuck := [1 to: 100000000000 do: [:i | ]] forkAt: 79.
    watchdog := [(Delay forMilliseconds: 2000) wait. result := 'timeout fired'.
      [stuck suspend] on: Error do: [:e |]] forkAt: 90.
    stream nextPutAll: 'Waiting 3s for timeout test...'; cr. stream flush.
    (Delay forMilliseconds: 3000) wait.
    stream nextPutAll: 'Timeout result: '; nextPutAll: result; cr.
    stream flush]
  ensure: [stream close].
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!DelayDiag class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #DelayDiag)
  inCategory: SessionManager default userCategory
  atPriority: 90!
