"Test classSide returns"!

Object subclass: #ClassSideTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!ClassSideTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/classside.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== ClassSide Test ==='; cr; cr.

    "Check all OSWindowDriver subclasses"
    stream nextPutAll: '--- OSWindowDriver subclasses ---'; cr.
    [OSWindowDriver subclasses do: [:each |
       stream nextPutAll: each name; nextPutAll: ' -> classSide = '.
       [| cs |
        cs := each classSide.
        cs ifNil: [stream nextPutAll: 'NIL'] ifNotNil: [stream print: cs]]
       on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Check what instanceSide subclasses returns"
    stream cr; nextPutAll: '--- OSWindowDriver class instanceSide subclasses ---'; cr.
    [| is |
     is := OSWindowDriver class instanceSide.
     stream nextPutAll: 'instanceSide: '; print: is; cr.
     stream nextPutAll: 'instanceSide subclasses:'; cr.
     is subclasses do: [:each |
       stream nextPutAll: '  '; print: each name; nextPutAll: ' -> classSide = '.
       [| cs |
        cs := each classSide.
        cs ifNil: [stream nextPutAll: 'NIL'] ifNotNil: [stream print: cs]]
       on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Check Object classSide for comparison"
    stream cr; nextPutAll: '--- Comparison checks ---'; cr.
    [stream nextPutAll: 'Object classSide: '; print: Object classSide; cr.
     stream nextPutAll: 'Array classSide: '; print: Array classSide; cr.
     stream nextPutAll: 'OSWindowDriver classSide: '; print: OSWindowDriver classSide; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!ClassSideTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!ClassSideTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #ClassSideTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
