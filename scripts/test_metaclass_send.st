"Test sending messages to metaclasses"!

Object subclass: #MetaclassSendTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!MetaclassSendTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/metaclass_send.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Metaclass Send Test ==='; cr; cr.

    "Test 1: Send to class vs metaclass"
    stream nextPutAll: '--- Test 1: Class vs Metaclass ---'; cr.
    [| cls mc |
     cls := OSiOSDriver.
     mc := OSiOSDriver class.
     stream nextPutAll: 'cls = '; print: cls; cr.
     stream nextPutAll: 'mc = '; print: mc; cr.
     stream nextPutAll: 'cls == mc: '; print: (cls == mc); cr.

     stream nextPutAll: 'cls isSuitable: '.
     [stream print: cls isSuitable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr.

     stream nextPutAll: 'mc isSuitable: '.
     [stream print: mc isSuitable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: Method lookup on metaclass"
    stream cr; nextPutAll: '--- Test 2: Method lookup ---'; cr.
    [| mc |
     mc := OSiOSDriver class.
     stream nextPutAll: 'mc = '; print: mc; cr.
     stream nextPutAll: 'mc class = '; print: mc class; cr.
     stream nextPutAll: 'mc canUnderstand: #isSuitable: '.
     [stream print: (mc canUnderstand: #isSuitable)] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr.
     stream nextPutAll: 'mc lookupSelector: #isSuitable: '.
     [stream print: (mc lookupSelector: #isSuitable)] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: What pickDriverClass expects"
    stream cr; nextPutAll: '--- Test 3: What pickDriverClass expects ---'; cr.
    [| source |
     source := (OSWindowDriver class >> #pickDriverClass) sourceCode.
     stream nextPutAll: 'pickDriverClass source:'; cr.
     stream nextPutAll: source; cr.
     stream nextPutAll: 'pickDriverClass sends isSuitable to the class, not the metaclass'; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: What Metaclass>>subclassesDo: returns"
    stream cr; nextPutAll: '--- Test 4: What subclassesDo: returns ---'; cr.
    [stream nextPutAll: 'OSWindowDriver subclasses do:'; cr.
     OSWindowDriver subclasses do: [:each |
       stream nextPutAll: '  '; print: each; cr].
     stream nextPutAll: 'OSWindowDriver class subclassesDo: (which calls instanceSide subclasses do:)'; cr.
     OSWindowDriver class subclassesDo: [:each |
       stream nextPutAll: '  '; print: each; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!MetaclassSendTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!MetaclassSendTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #MetaclassSendTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
