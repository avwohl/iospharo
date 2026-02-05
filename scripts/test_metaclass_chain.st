"Test metaclass superclass chain"!

Object subclass: #MetaclassChainTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!MetaclassChainTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/metaclass_chain.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Metaclass Chain Test ==='; cr; cr.

    "Test 1: OSWindowDriver class superclass chain"
    stream nextPutAll: '--- Test 1: OSWindowDriver class superclass chain ---'; cr.
    [| current depth |
     current := OSWindowDriver class.
     depth := 0.
     [current notNil and: [depth < 15]] whileTrue: [
       stream nextPutAll: '  '; print: depth; nextPutAll: ': '; print: current; cr.
       current := current superclass.
       depth := depth + 1]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: Check where subclassesDo: is in the chain"
    stream cr; nextPutAll: '--- Test 2: Where is subclassesDo: in metaclass chain? ---'; cr.
    [| current depth |
     current := OSWindowDriver class.
     depth := 0.
     [current notNil and: [depth < 15]] whileTrue: [
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

    "Test 3: Class methodDict"
    stream cr; nextPutAll: '--- Test 3: Class methodDict ---'; cr.
    [| md |
     md := Class methodDict.
     stream nextPutAll: 'Class methodDict size: '; print: md size; cr.
     stream nextPutAll: 'Class methodDict includesKey: #subclassesDo: = '; print: (md includesKey: #subclassesDo:); cr.
     (md includesKey: #subclassesDo:) ifTrue: [
       stream nextPutAll: 'Class >> #subclassesDo: = '; print: (md at: #subclassesDo:); cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: What is Class superclass?"
    stream cr; nextPutAll: '--- Test 4: Class superclass chain ---'; cr.
    [| current depth |
     current := Class.
     depth := 0.
     [current notNil and: [depth < 10]] whileTrue: [
       stream nextPutAll: '  '; print: depth; nextPutAll: ': '; print: current; cr.
       current := current superclass.
       depth := depth + 1]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: What is ProtoObject class superclass?"
    stream cr; nextPutAll: '--- Test 5: ProtoObject class superclass ---'; cr.
    [stream nextPutAll: 'ProtoObject class = '; print: ProtoObject class; cr.
     stream nextPutAll: 'ProtoObject class superclass = '; print: ProtoObject class superclass; cr.
     stream nextPutAll: 'ProtoObject class superclass == Class: '; print: (ProtoObject class superclass == Class); cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: Send subclassesDo: to OSWindowDriver"
    stream cr; nextPutAll: '--- Test 6: OSWindowDriver subclassesDo: ---'; cr.
    [| count |
     count := 0.
     OSWindowDriver subclassesDo: [:each |
       count := count + 1.
       stream nextPutAll: '  subclass: '; print: each; cr].
     stream nextPutAll: 'Total subclasses: '; print: count; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!MetaclassChainTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!MetaclassChainTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #MetaclassChainTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
