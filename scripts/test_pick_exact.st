"Test exact structure of pickDriverClass"!

Object subclass: #PickExactTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!PickExactTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/pick_exact.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Pick Exact Test ==='; cr; cr.

    "Test with tracing"
    stream nextPutAll: '--- Traced pickDriverClass ---'; cr.
    [| result |
     result := self tracedPickDriverClass: stream.
     stream nextPutAll: 'RESULT: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex class name; nextPutAll: ' - '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!PickExactTest class methodsFor: 'tests'!
tracedPickDriverClass: stream
  "Exact replica of OSWindowDriver class>>pickDriverClass with tracing"
  stream nextPutAll: 'About to call subclassesDo:...'; cr.
  stream flush.
  OSWindowDriver class subclassesDo: [:aClass |
    stream nextPutAll: '  In block, aClass = '; print: aClass; cr.
    stream nextPutAll: '  Checking isSuitable...'; cr.
    stream flush.
    [| suitable |
     suitable := aClass isSuitable.
     stream nextPutAll: '  isSuitable returned: '; print: suitable; cr.
     stream flush.
     suitable ifTrue: [
       stream nextPutAll: '  ** RETURNING aClass **'; cr.
       stream flush.
       ^ aClass]]
    on: Error do: [:e |
      stream nextPutAll: '  isSuitable ERROR: '; print: e messageText; cr.
      stream flush]].
  stream nextPutAll: 'After loop, returning OSNullWindowDriver'; cr.
  stream flush.
  ^ OSNullWindowDriver! !

!PickExactTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!PickExactTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #PickExactTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
