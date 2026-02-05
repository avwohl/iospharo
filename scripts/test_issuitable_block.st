"Test isSuitable behavior inside blocks"!

Object subclass: #IsSuitableBlockTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!IsSuitableBlockTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/issuitable_block.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== isSuitable in Block Test ==='; cr; cr.

    "Test 1: Direct isSuitable"
    stream nextPutAll: '--- Test 1: Direct isSuitable ---'; cr.
    [stream nextPutAll: 'OSiOSDriver isSuitable = '; print: OSiOSDriver isSuitable; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: isSuitable in simple block"
    stream cr; nextPutAll: '--- Test 2: isSuitable in simple block ---'; cr.
    [[| result |
      result := OSiOSDriver isSuitable.
      stream nextPutAll: '[OSiOSDriver isSuitable] = '; print: result; cr] value]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: isSuitable on block parameter"
    stream cr; nextPutAll: '--- Test 3: isSuitable on block parameter ---'; cr.
    [[| cls result |
      cls := OSiOSDriver.
      stream nextPutAll: 'cls = '; print: cls; cr.
      stream nextPutAll: 'cls == OSiOSDriver: '; print: (cls == OSiOSDriver); cr.
      result := cls isSuitable.
      stream nextPutAll: 'cls isSuitable = '; print: result; cr] value]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: isSuitable on each element"
    stream cr; nextPutAll: '--- Test 4: isSuitable on each element ---'; cr.
    [{OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:aClass |
      stream nextPutAll: aClass name; nextPutAll: ' isSuitable = '.
      [stream print: aClass isSuitable] on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e messageText].
      stream cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: subclassesDo: with isSuitable tracing"
    stream cr; nextPutAll: '--- Test 5: subclassesDo: with isSuitable ---'; cr.
    [OSWindowDriver subclassesDo: [:aClass |
      stream nextPutAll: 'aClass = '; print: aClass; cr.
      stream nextPutAll: 'aClass class = '; print: aClass class; cr.
      stream nextPutAll: 'aClass == OSiOSDriver: '; print: (aClass == OSiOSDriver); cr.
      stream nextPutAll: 'aClass isSuitable = '.
      [stream print: aClass isSuitable] on: Error do: [:e | stream nextPutAll: 'ERROR: '; print: e messageText].
      stream cr; cr]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 6: Check if NLR fails after isSuitable"
    stream cr; nextPutAll: '--- Test 6: NLR after isSuitable check ---'; cr.
    [| found |
     found := nil.
     OSWindowDriver subclassesDo: [:aClass |
       | suitable |
       suitable := aClass isSuitable.
       stream nextPutAll: aClass name; nextPutAll: ' suitable='; print: suitable; cr.
       suitable ifTrue: [
         stream nextPutAll: '  Setting found := '; print: aClass; cr.
         found := aClass.
         stream nextPutAll: '  About to return'; cr.
         found notNil ifTrue: [
           stream nextPutAll: '  Returning '; print: found; cr]]].
     stream nextPutAll: 'After loop, found = '; print: found; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 7: Explicit NLR test"
    stream cr; nextPutAll: '--- Test 7: Explicit NLR with isSuitable ---'; cr.
    [| result |
     result := self testExplicitNLR.
     stream nextPutAll: 'testExplicitNLR = '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!IsSuitableBlockTest class methodsFor: 'tests'!
testExplicitNLR
  "Explicit test of NLR after isSuitable"
  OSWindowDriver subclassesDo: [:aClass |
    | suitable |
    suitable := aClass isSuitable.
    suitable ifTrue: [^ aClass]].
  ^ #notFound! !

!IsSuitableBlockTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!IsSuitableBlockTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #IsSuitableBlockTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
