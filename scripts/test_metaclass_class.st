"Test metaclass class hierarchy"!

Object subclass: #MetaclassClassTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!MetaclassClassTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/metaclass_class.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Metaclass Class Hierarchy Test ==='; cr; cr.

    "Test 1: Class hierarchy"
    stream nextPutAll: '--- Test 1: Class hierarchy ---'; cr.
    [| cls mc |
     cls := OSiOSDriver.
     mc := OSiOSDriver class.

     stream nextPutAll: 'OSiOSDriver:'; cr.
     stream nextPutAll: '  class: '; print: cls class; cr.
     stream nextPutAll: '  class class: '; print: cls class class; cr.

     stream nextPutAll: 'OSiOSDriver class:'; cr.
     stream nextPutAll: '  class: '; print: mc class; cr.
     stream nextPutAll: '  class class: '; print: mc class class; cr.

     stream nextPutAll: 'Are they the same object? '; cr.
     stream nextPutAll: '  OSiOSDriver class == OSiOSDriver class: '; print: (mc == cls class); cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: Send to class of class"
    stream cr; nextPutAll: '--- Test 2: Send to class of class ---'; cr.
    [| clsClass |
     clsClass := OSiOSDriver class.
     stream nextPutAll: 'clsClass = '; print: clsClass; cr.
     stream nextPutAll: 'clsClass class = '; print: clsClass class; cr.
     stream nextPutAll: 'Sending name to clsClass: '.
     [stream print: clsClass name] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr.
     stream nextPutAll: 'Sending class to clsClass: '.
     [stream print: clsClass class] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr.
     stream nextPutAll: 'Sending yourself to clsClass: '.
     [stream print: clsClass yourself] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: Find where isSuitable is"
    stream cr; nextPutAll: '--- Test 3: isSuitable lookup ---'; cr.
    [| mc method |
     mc := OSiOSDriver class.
     method := mc lookupSelector: #isSuitable.
     stream nextPutAll: 'Method: '; print: method; cr.
     method ifNotNil: [
       stream nextPutAll: 'methodClass: '; print: method methodClass; cr.
       stream nextPutAll: 'selector: '; print: method selector; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: Try perform: instead of direct send"
    stream cr; nextPutAll: '--- Test 4: perform: ---'; cr.
    [| mc |
     mc := OSiOSDriver class.
     stream nextPutAll: 'mc = '; print: mc; cr.
     stream nextPutAll: 'mc perform: #isSuitable: '.
     [stream print: (mc perform: #isSuitable)] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr.
     stream nextPutAll: 'mc perform: #name: '.
     [stream print: (mc perform: #name)] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!MetaclassClassTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!MetaclassClassTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #MetaclassClassTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
