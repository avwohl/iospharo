"Exception handling diagnostic for forked processes.
Registers as startup handler so it runs when our custom VM loads the image.
Output goes to /tmp/exception_diag.txt"!

Object subclass: #ExceptionDiag
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Diag'!

!ExceptionDiag class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/exception_diag.txt' asFileReference writeStream.
  [
  stream nextPutAll: '=== Exception Handling Diagnostic ==='; cr.
  stream flush.

  "Test 1: ZeroDivide handler in same process"
  stream nextPutAll: 'TEST 1: ZeroDivide handler (no fork)'; cr. stream flush.
  [| x |
   x := false.
   [ 1/0 ] on: ZeroDivide do: [:e | x := true].
   stream nextPutAll: '  RESULT: '; nextPutAll: x printString; cr. stream flush]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  "Test 2: UnhandledError handler in same process (needs pass)"
  stream nextPutAll: 'TEST 2: UnhandledError handler (no fork, with pass)'; cr. stream flush.
  [| x |
   x := false.
   [[ 1/0 ] on: UnhandledError do: [:e | x := true]]
     on: ZeroDivide do: [:e | e pass].
   stream nextPutAll: '  RESULT: '; nextPutAll: x printString; cr. stream flush]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  "Test 3: Simple fork with semaphore"
  stream nextPutAll: 'TEST 3: Simple fork + semaphore'; cr. stream flush.
  [| sem done |
   sem := Semaphore new.
   done := false.
   [done := true. sem signal] fork.
   (sem waitTimeoutSeconds: 3) ifTrue: [
     stream nextPutAll: '  TIMEOUT'; cr. stream flush]
   ifFalse: [
     stream nextPutAll: '  OK done='; nextPutAll: done printString; cr. stream flush]]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  "Test 4: ZeroDivide handler in forked process"
  stream nextPutAll: 'TEST 4: ZeroDivide handler in fork'; cr. stream flush.
  [| sem x |
   sem := Semaphore new.
   x := false.
   [[ 1/0 ] on: ZeroDivide do: [:e | x := true]. sem signal] fork.
   (sem waitTimeoutSeconds: 3) ifTrue: [
     stream nextPutAll: '  TIMEOUT'; cr. stream flush]
   ifFalse: [
     stream nextPutAll: '  OK x='; nextPutAll: x printString; cr. stream flush]]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  "Test 5: UnhandledError handler in fork (THE failing case)"
  stream nextPutAll: 'TEST 5: UnhandledError handler in fork'; cr. stream flush.
  [| sem x |
   sem := Semaphore new.
   x := false.
   [[ 1/0 ] on: UnhandledError do: [:e | x := true]. sem signal] fork.
   (sem waitTimeoutSeconds: 3) ifTrue: [
     stream nextPutAll: '  TIMEOUT (the bug!)'; cr. stream flush]
   ifFalse: [
     stream nextPutAll: '  OK x='; nextPutAll: x printString; cr. stream flush]]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  "Test 6: Error handler in fork"
  stream nextPutAll: 'TEST 6: Error handler in fork'; cr. stream flush.
  [| sem x |
   sem := Semaphore new.
   x := false.
   [[ 1/0 ] on: Error do: [:e | x := true]. sem signal] fork.
   (sem waitTimeoutSeconds: 3) ifTrue: [
     stream nextPutAll: '  TIMEOUT'; cr. stream flush]
   ifFalse: [
     stream nextPutAll: '  OK x='; nextPutAll: x printString; cr. stream flush]]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  "Test 7: ExceptionTest runWithNoHandlers pattern"
  stream nextPutAll: 'TEST 7: runWithNoHandlers pattern'; cr. stream flush.
  [| synchSemaphore result |
   synchSemaphore := Semaphore new.
   [result := [[ 1/0 ] on: UnhandledError do: [:ue | ue exception]]
     ensure: [synchSemaphore signal]] forkNamed: 'Test process'.
   (synchSemaphore waitTimeoutSeconds: 3) ifTrue: [
     stream nextPutAll: '  TIMEOUT'; cr. stream flush]
   ifFalse: [
     stream nextPutAll: '  OK result: '; nextPutAll: result printString; cr. stream flush]]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  "Test 8: Check if defaultAction works at all"
  stream nextPutAll: 'TEST 8: Signal with no handler (no fork)'; cr. stream flush.
  [| result |
   result := 'not run'.
   [[Error signal: 'test error']
     on: UnhandledError do: [:ue | result := 'caught: ', ue exception messageText. ue return: nil]]
   on: Error do: [:e | e pass].
   stream nextPutAll: '  result: '; nextPutAll: result; cr. stream flush]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  "Test 9: Check process errorHandler"
  stream nextPutAll: 'TEST 9: Process errorHandler'; cr. stream flush.
  [
   stream nextPutAll: '  active errorHandler: '.
   Processor activeProcess errorHandler
     ifNil: [stream nextPutAll: 'nil'; cr]
     ifNotNil: [:h | stream nextPutAll: h printString; cr].
   stream flush]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  "Test 10: Ensure in fork"
  stream nextPutAll: 'TEST 10: ensure in fork'; cr. stream flush.
  [| sem ensured |
   sem := Semaphore new.
   ensured := false.
   [[1 + 1] ensure: [ensured := true. sem signal]] fork.
   (sem waitTimeoutSeconds: 3) ifTrue: [
     stream nextPutAll: '  TIMEOUT'; cr. stream flush]
   ifFalse: [
     stream nextPutAll: '  OK ensured='; nextPutAll: ensured printString; cr. stream flush]]
  on: Error do: [:e |
   stream nextPutAll: '  ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. stream flush].

  stream cr.
  stream nextPutAll: '=== DONE ==='; cr.
  stream flush]
  ensure: [stream close].

  "Exit"
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!ExceptionDiag class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!ExceptionDiag class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #ExceptionDiag)
  inCategory: SessionManager default userCategory
  atPriority: 90!
