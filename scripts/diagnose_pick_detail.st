"Diagnose pickDriverClass internals"!

Object subclass: #PickDetailDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!PickDetailDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/pick_detail.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Pick Detail Diagnostic ==='; cr; cr.

    "Step 1: Get pickDriverClass source"
    stream nextPutAll: '--- Step 1: pickDriverClass source ---'; cr.
    [| m |
     m := OSWindowDriver class >> #pickDriverClass.
     stream nextPutAll: m sourceCode; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Implement our own version"
    stream cr; nextPutAll: '--- Step 2: Manual pickDriverClass ---'; cr.
    [| result |
     result := nil.
     OSWindowDriver subclassesDo: [:aClass |
       stream nextPutAll: 'Testing '; print: aClass name; nextPutAll: '... '.
       aClass isSuitable ifTrue: [
         stream nextPutAll: 'SUITABLE, setting result'; cr.
         result := aClass]
       ifFalse: [
         stream nextPutAll: 'not suitable'; cr]].
     stream nextPutAll: 'After loop, result = '.
     result ifNil: [stream nextPutAll: 'nil'] ifNotNil: [stream print: result name].
     stream cr.
     result ifNil: [result := OSNullWindowDriver].
     stream nextPutAll: 'Final result: '; print: result name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Check if non-local return works"
    stream cr; nextPutAll: '--- Step 3: Non-local return test ---'; cr.
    [| result |
     result := self testNonLocalReturn.
     stream nextPutAll: 'testNonLocalReturn result: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!PickDetailDiagnostic class methodsFor: 'testing'!
testNonLocalReturn
  "Test if non-local return from ifTrue: block works"
  #(1 2 3 4 5) do: [:n |
    n = 3 ifTrue: [^ 'found3']].
  ^ 'notfound'! !

!PickDetailDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!PickDetailDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #PickDetailDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
