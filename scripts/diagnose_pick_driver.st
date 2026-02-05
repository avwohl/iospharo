"Diagnose pickDriverClass in detail"!

Object subclass: #PickDriverDiagnostic
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!PickDriverDiagnostic class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/pick_driver.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Pick Driver Diagnostic ==='; cr; cr.

    "Step 1: Check subclasses iteration"
    stream nextPutAll: '--- Step 1: OSWindowDriver subclasses ---'; cr.
    [stream nextPutAll: 'OSWindowDriver subclasses: '.
     OSWindowDriver subclasses do: [:c | stream print: c name; space].
     stream cr.
     stream nextPutAll: 'subclassesDo: order: '.
     OSWindowDriver subclassesDo: [:c | stream print: c name; space].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 2: Manually iterate and check each"
    stream cr; nextPutAll: '--- Step 2: Manual iteration ---'; cr.
    [OSWindowDriver subclassesDo: [:aClass |
       stream nextPutAll: 'Checking '; print: aClass name; nextPutAll: '...'; cr.
       stream nextPutAll: '  class: '; print: aClass; cr.
       stream nextPutAll: '  isSuitable method owner: '.
       [| m | m := aClass class lookupSelector: #isSuitable.
        m ifNil: [stream nextPutAll: 'nil']
           ifNotNil: [stream print: m methodClass name].
        stream cr]
       on: Error do: [:e | stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'; cr].
       stream nextPutAll: '  calling isSuitable: '.
       [| result |
        result := aClass isSuitable.
        stream print: result; cr.
        result ifTrue: [
          stream nextPutAll: '  ** WOULD BE PICKED **'; cr]]
       on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'; cr]]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Step 3: Call pickDriverClass"
    stream cr; nextPutAll: '--- Step 3: Call pickDriverClass ---'; cr.
    [| result |
     result := OSWindowDriver pickDriverClass.
     stream nextPutAll: 'Result: '; print: result name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!PickDriverDiagnostic class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!PickDriverDiagnostic class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #PickDriverDiagnostic)
  inCategory: SessionManager default userCategory
  atPriority: 90!
