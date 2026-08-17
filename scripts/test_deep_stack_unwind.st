"Unwind and handler search must not give up on a deep stack.

 Primitives 195 (findNextUnwindContextUpTo:) and 197
 (findNextHandlerOrSignalingContext) walk a context's sender chain. Both stopped
 after 10000 contexts and then answered **nil**, which does not mean 'I stopped
 looking' — it means 'there is no such context', and every caller believes it.

 The consequences are not subtle:

   - Context>>unwindTo: runs no unwind blocks at all, so ensure: and ifCurtailed:
     blocks below the cutoff are silently skipped.
   - Process>>terminate, which terminates another process by inserting an
     ensure: at the bottom of its stack and waiting on the semaphore that ensure
     signals, kills its target and then waits forever. terminate never returns.
   - An exception raised deeper than the cutoff finds no handler and goes
     unhandled even though a handler is sitting right there.

 10000 is not a large stack. StepThroughTest recurses exactly 10000 deep as a
 matter of course, which put its chain at 10009 and one context past the limit —
 so StepThroughTest could never finish under the normal test runner, and
 FastStepThroughTest>>testStepThroughLonger failed its assertion. Both were
 recorded as separate stepping bugs for months; they were this.

 The limit is now a corruption guard far above any real stack, and exceeding it
 fails the primitive so the Smalltalk fallback — which walks with no limit —
 takes over, rather than answering something false.

 Written 2026-08-16."!

TestCase subclass: #DeepStackUnwindTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Kernel'!

!DeepStackUnwindTest methodsFor: 'helpers'!
recurse: n then: aBlock
  "Build a stack n frames deep, then evaluate aBlock at the bottom."
  n <= 0 ifTrue: [ ^ aBlock value ].
  ^ self recurse: n - 1 then: aBlock! !

!DeepStackUnwindTest methodsFor: 'helpers'!
depthPastOldLimit
  "Comfortably past the 10000 the primitives used to stop at. Kept well under
   the VM's frame ceiling so this tests the context walk and nothing else."
  ^ 12000! !

!DeepStackUnwindTest methodsFor: 'tests'!
testEnsureRunsBelowADeepStack
  "An ensure: below a deep stack must still run when the stack unwinds."
  | ran |
  ran := false.
  [ self recurse: self depthPastOldLimit then: [ Error signal: 'unwind me' ] ]
    on: Error
    do: [ :e | e return: nil ].
  [ [ self recurse: self depthPastOldLimit then: [ Error signal: 'unwind me' ] ]
      ensure: [ ran := true ] ]
    on: Error
    do: [ :e | e return: nil ].
  self assert: ran description: 'ensure: block below a deep stack did not run'! !

!DeepStackUnwindTest methodsFor: 'tests'!
testHandlerFoundBelowADeepStack
  "A handler below a deep stack must still be found. Under the bug the search
   answered nil and the exception went unhandled."
  | caught |
  caught := false.
  [ self recurse: self depthPastOldLimit then: [ Error signal: 'find me' ] ]
    on: Error
    do: [ :e | caught := true. e return: nil ].
  self assert: caught description: 'handler below a deep stack was not found'! !

!DeepStackUnwindTest methodsFor: 'tests'!
testTerminateReturnsForADeepProcess
  "Process>>terminate on another process inserts an ensure: at the bottom of the
   target's stack and waits on the semaphore that ensure signals. If the unwind
   search cannot reach that far the ensure never runs, so terminate kills the
   target and never returns to its caller. Guarded by a time limit, because that
   is exactly how it fails."
  | sem target returned |
  sem := Semaphore new.
  returned := false.
  target := [ self recurse: self depthPastOldLimit then: [ sem wait ] ] fork.
  (Delay forMilliseconds: 400) wait.
  [ target terminate. returned := true ] valueWithin: 10 seconds onTimeout: [ nil ].
  self
    assert: returned
    description: 'terminate did not return for a process with a deep stack'.
  self assert: target isTerminated! !

!DeepStackUnwindTest methodsFor: 'tests'!
testUnwindBlockOfTerminatedDeepProcessRuns
  "The same shape, checking the ensure: actually ran rather than merely that
   terminate returned."
  | sem target ran |
  sem := Semaphore new.
  ran := false.
  target := [ [ self recurse: self depthPastOldLimit then: [ sem wait ] ]
                ensure: [ ran := true ] ] fork.
  (Delay forMilliseconds: 400) wait.
  [ target terminate ] valueWithin: 10 seconds onTimeout: [ nil ].
  self assert: ran description: 'ensure: of a terminated deep process did not run'! !
