"Test Metaclass>>isMetaclassOfClassOrNil"!

Object subclass: #MetaclassCheckTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!MetaclassCheckTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/metaclass_check.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Metaclass Check Test ==='; cr; cr.

    "Test isMetaclassOfClassOrNil for various classes"
    stream nextPutAll: '--- isMetaclassOfClassOrNil ---'; cr.
    [#(Object Class Metaclass OSWindowDriver OSSDL2Driver OSiOSDriver) do: [:name |
       | cls mc |
       cls := Smalltalk at: name ifAbsent: [nil].
       cls ifNil: [
         stream nextPutAll: name; nextPutAll: ' not found'; cr]
       ifNotNil: [
         mc := cls class.
         stream nextPutAll: name; nextPutAll: ' class = '; print: mc; cr.
         stream nextPutAll: '  isMetaclassOfClassOrNil: '.
         [stream print: mc isMetaclassOfClassOrNil] on: Error do: [:e |
           stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
         stream cr]]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test subclassesDo: for OSWindowDriver class"
    stream cr; nextPutAll: '--- OSWindowDriver class subclassesDo: ---'; cr.
    [stream nextPutAll: 'OSWindowDriver class isMetaclassOfClassOrNil: '.
     [stream print: OSWindowDriver class isMetaclassOfClassOrNil] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr.
     stream nextPutAll: 'Iterating:'; cr.
     OSWindowDriver class subclassesDo: [:aClass |
       stream nextPutAll: '  '; print: aClass; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test what instanceSide returns"
    stream cr; nextPutAll: '--- instanceSide ---'; cr.
    [stream nextPutAll: 'OSWindowDriver class instanceSide: '.
     stream print: OSWindowDriver class instanceSide; cr.
     stream nextPutAll: 'OSWindowDriver class instanceSide subclasses: '.
     OSWindowDriver class instanceSide subclasses do: [:c |
       stream print: c name; space].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!MetaclassCheckTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!MetaclassCheckTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #MetaclassCheckTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
