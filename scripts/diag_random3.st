"Diagnostic: test Random via same fork mechanism as SUnit runner"!

Object subclass: #RandomDiag3
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Diag'!

!RandomDiag3 class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/random_diag3.txt' asFileReference writeStream.
  [self runDiag: stream] ensure: [stream close].
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!RandomDiag3 class methodsFor: 'running'!
runDiag: s
  s nextPutAll: '=== Random Diagnostic 3 (fork test) ==='; cr.

  "Step 1: Run RandomTest directly (no fork)"
  s nextPutAll: '--- Direct (no fork) ---'; cr.
  #(testNext testDistribution testWithFloatAsSeed testUnixRandomGeneratorSeed) do: [:sel |
    | test |
    test := RandomTest selector: sel.
    [test runCase.
     s nextPutAll: sel; nextPutAll: ': PASS'; cr]
    on: Error do: [:e |
     s nextPutAll: sel; nextPutAll: ': ERROR - '; nextPutAll: e class name;
       nextPutAll: ' - '; nextPutAll: e messageText; cr]].
  s flush.

  "Step 2: Run RandomTest via fork at 80 (same as SUnit runner)"
  s nextPutAll: '--- Fork at 80 ---'; cr.
  #(testNext testDistribution testWithFloatAsSeed testUnixRandomGeneratorSeed) do: [:sel |
    | sem result |
    sem := Semaphore new.
    result := nil.
    [[[[(RandomTest selector: sel) runCase]
      on: TestFailure do: [:ex |
        result := 'FAIL: ', (ex messageText copyFrom: 1 to: (ex messageText size min: 200))]]
      on: Deprecation do: [:ex | ex resume]]
      on: Error do: [:ex |
        result := 'ERROR: ', ex class name, ' - ', (ex messageText copyFrom: 1 to: (ex messageText size min: 200))].
    sem signal] forkAt: 80.
    (sem waitTimeoutSeconds: 10) ifTrue: [
      result := 'TIMEOUT'].
    result ifNil: [
      s nextPutAll: sel; nextPutAll: ': PASS'; cr]
    ifNotNil: [
      s nextPutAll: sel; nextPutAll: ': '; nextPutAll: result; cr]].
  s flush.

  "Step 3: Run Random computation in fork at 80"
  s nextPutAll: '--- Raw Random in fork ---'; cr.
  [| sem result2 |
   sem := Semaphore new.
   result2 := nil.
   [| r val |
    [r := Random new.
     val := r next.
     result2 := 'OK: ', val printString, ' (', val class name, ')']
    on: Error do: [:e |
     result2 := 'ERROR: ', e class name, ' - ', e messageText].
    sem signal] forkAt: 80.
   (sem waitTimeoutSeconds: 10) ifTrue: [
     result2 := 'TIMEOUT'].
   s nextPutAll: 'Random in fork: '; nextPutAll: result2; cr] on: Error do: [:e |
   s nextPutAll: 'Fork ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  s flush.

  s nextPutAll: '=== Done ==='; cr.
  s flush! !

!RandomDiag3 class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!RandomDiag3 class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #RandomDiag3)
  inCategory: SessionManager default userCategory
  atPriority: 90!
