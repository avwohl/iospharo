"Test method lookup for subclassesDo:"!

Object subclass: #LookupTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!LookupTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/lookup_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Method Lookup Test ==='; cr; cr.

    "Test 1: lookupSelector on OSWindowDriver"
    stream nextPutAll: '--- Test 1: OSWindowDriver lookupSelector: ---'; cr.
    [| method |
     method := OSWindowDriver lookupSelector: #subclassesDo:.
     stream nextPutAll: 'OSWindowDriver lookupSelector: #subclassesDo: = '; print: method; cr.
     method ifNotNil: [
       stream nextPutAll: '  methodClass: '; print: method methodClass; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: lookupSelector on Behavior"
    stream cr; nextPutAll: '--- Test 2: Behavior lookupSelector: ---'; cr.
    [| method |
     method := Behavior lookupSelector: #subclassesDo:.
     stream nextPutAll: 'Behavior lookupSelector: #subclassesDo: = '; print: method; cr.
     method ifNotNil: [
       stream nextPutAll: '  methodClass: '; print: method methodClass; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: Check the superclass chain of OSWindowDriver"
    stream cr; nextPutAll: '--- Test 3: OSWindowDriver superclass chain ---'; cr.
    [| current depth |
     current := OSWindowDriver.
     depth := 0.
     [current notNil and: [depth < 20]] whileTrue: [
       stream nextPutAll: '  '; print: depth; nextPutAll: ': '; print: current; cr.
       current := current superclass.
       depth := depth + 1]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: Check where subclassesDo: is defined"
    stream cr; nextPutAll: '--- Test 4: Where is subclassesDo: defined? ---'; cr.
    [| current depth |
     current := OSWindowDriver.
     depth := 0.
     [current notNil and: [depth < 20]] whileTrue: [
       | md |
       md := current methodDict.
       (md includesKey: #subclassesDo:) ifTrue: [
         stream nextPutAll: '  FOUND at depth '; print: depth; nextPutAll: ' in '; print: current; cr.
         stream nextPutAll: '  method: '; print: (md at: #subclassesDo:); cr].
       current := current superclass.
       depth := depth + 1]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: Check Behavior methodDict directly"
    stream cr; nextPutAll: '--- Test 5: Behavior methodDict ---'; cr.
    [| md |
     md := Behavior methodDict.
     stream nextPutAll: 'Behavior methodDict size: '; print: md size; cr.
     stream nextPutAll: 'Behavior methodDict includesKey: #subclassesDo: = '; print: (md includesKey: #subclassesDo:); cr.
     (md includesKey: #subclassesDo:) ifTrue: [
       stream nextPutAll: 'Behavior >> #subclassesDo: = '; print: (md at: #subclassesDo:); cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: Is OSWindowDriver a subclass of Behavior?"
    stream cr; nextPutAll: '--- Test 6: Class hierarchy checks ---'; cr.
    [stream nextPutAll: 'OSWindowDriver isKindOf: Behavior = '; print: (OSWindowDriver isKindOf: Behavior); cr.
     stream nextPutAll: 'OSWindowDriver inheritsFrom: Behavior = '; print: (OSWindowDriver inheritsFrom: Behavior); cr.
     stream nextPutAll: 'OSWindowDriver class = '; print: OSWindowDriver class; cr.
     stream nextPutAll: 'OSWindowDriver class inheritsFrom: Behavior = '; print: (OSWindowDriver class inheritsFrom: Behavior); cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 7: What does lookupSelector: do?"
    stream cr; nextPutAll: '--- Test 7: lookupSelector: implementation ---'; cr.
    [| method |
     method := Behavior lookupSelector: #lookupSelector:.
     stream nextPutAll: 'Behavior >> #lookupSelector: defined in: '; print: method methodClass; cr.
     stream nextPutAll: 'Source: '; cr.
     stream nextPutAll: method sourceCode; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!LookupTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!LookupTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #LookupTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
