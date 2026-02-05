"Test NLR from nested blocks similar to Metaclass>>subclassesDo:"!

Object subclass: #NLRNestedTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!NLRNestedTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/nlr_nested.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== NLR Nested Block Test ==='; cr; cr.

    "Test 1: Simple NLR from do:ifTrue:"
    stream nextPutAll: '--- Test 1: Simple NLR ---'; cr.
    [| result |
     result := self testSimpleNLR.
     stream nextPutAll: 'testSimpleNLR: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: NLR from wrapper block"
    stream cr; nextPutAll: '--- Test 2: NLR from wrapper block ---'; cr.
    [| result |
     result := self testWrapperNLR.
     stream nextPutAll: 'testWrapperNLR: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: NLR like Metaclass>>subclassesDo:"
    stream cr; nextPutAll: '--- Test 3: NLR like Metaclass>>subclassesDo: ---'; cr.
    [| result |
     result := self testMetaclassStyleNLR.
     stream nextPutAll: 'testMetaclassStyleNLR: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: Actual pickDriverClass"
    stream cr; nextPutAll: '--- Test 4: pickDriverClass ---'; cr.
    [| result |
     result := OSWindowDriver pickDriverClass.
     stream nextPutAll: 'pickDriverClass: '; print: result name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: Manual implementation of pickDriverClass"
    stream cr; nextPutAll: '--- Test 5: Manual pickDriverClass ---'; cr.
    [| result |
     result := self manualPickDriverClass.
     stream nextPutAll: 'manualPickDriverClass: '; print: result name; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!NLRNestedTest class methodsFor: 'tests'!
testSimpleNLR
  "NLR from ifTrue: inside do:"
  #(1 2 3 4 5) do: [:n | n = 3 ifTrue: [^ 'found3']].
  ^ 'notfound'! !

!NLRNestedTest class methodsFor: 'tests'!
testWrapperNLR
  "NLR from aBlock that is called from inside another block"
  | aBlock |
  aBlock := [:n | n = 3 ifTrue: [^ 'found3']].
  #(1 2 3 4 5) do: [:each | aBlock value: each].
  ^ 'notfound'! !

!NLRNestedTest class methodsFor: 'tests'!
testMetaclassStyleNLR
  "NLR exactly like Metaclass>>subclassesDo: does"
  | aBlock items |
  aBlock := [:n | n = 3 ifTrue: [^ 'found3']].
  items := #(1 2 3 4 5).
  items do: [:each | aBlock value: each].
  ^ 'notfound'! !

!NLRNestedTest class methodsFor: 'tests'!
manualPickDriverClass
  "Manual implementation of pickDriverClass"
  | foundClass |
  foundClass := nil.
  OSWindowDriver subclasses do: [:each |
    | classClass |
    classClass := each classSide.
    classClass isSuitable ifTrue: [
      foundClass := classClass.
      ^ classClass  "NLR"]].
  foundClass ifNil: [foundClass := OSNullWindowDriver].
  ^ foundClass! !

!NLRNestedTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!NLRNestedTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #NLRNestedTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
