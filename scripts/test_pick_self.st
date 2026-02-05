"Test what self is in pickDriverClass"!

Object subclass: #PickSelfTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!PickSelfTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/pick_self.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Pick Self Test ==='; cr; cr.

    "Test 1: What is self in pickDriverClass context?"
    stream nextPutAll: '--- Test 1: Check self in pickDriverClass ---'; cr.
    [| result |
     result := self testPickDriverClassSelf.
     stream nextPutAll: 'testPickDriverClassSelf result: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: What does OSWindowDriver subclassesDo: iterate?"
    stream cr; nextPutAll: '--- Test 2: OSWindowDriver subclassesDo: ---'; cr.
    [stream nextPutAll: 'Calling OSWindowDriver subclassesDo:...'; cr.
     OSWindowDriver subclassesDo: [:c |
       stream nextPutAll: '  '; print: c; nextPutAll: ' (class: '; print: c class; nextPutAll: ')'; cr.
       stream flush]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: Which subclassesDo: method is used?"
    stream cr; nextPutAll: '--- Test 3: Which subclassesDo: ---'; cr.
    [| method |
     method := OSWindowDriver lookupSelector: #subclassesDo:.
     stream nextPutAll: 'Method: '; print: method; cr.
     stream nextPutAll: 'methodClass: '; print: method methodClass; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: Direct pickDriverClass"
    stream cr; nextPutAll: '--- Test 4: pickDriverClass ---'; cr.
    [| result |
     result := OSWindowDriver pickDriverClass.
     stream nextPutAll: 'pickDriverClass: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!PickSelfTest class methodsFor: 'tests'!
testPickDriverClassSelf
  "Check what self would be in the context of pickDriverClass"
  ^ OSWindowDriver class name, ' vs ', OSWindowDriver name! !

!PickSelfTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!PickSelfTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #PickSelfTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
