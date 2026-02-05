"Test isSuitable specifically"!

Object subclass: #IsSuitableTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!IsSuitableTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/is_suitable.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== isSuitable Test ==='; cr; cr.

    "Test 1: Direct calls to isSuitable"
    stream nextPutAll: '--- Test 1: Direct isSuitable calls ---'; cr.
    [stream nextPutAll: 'OSNullWindowDriver isSuitable: '.
     [stream print: OSNullWindowDriver isSuitable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr.

     stream nextPutAll: 'OSSDL2Driver isSuitable: '.
     [stream print: OSSDL2Driver isSuitable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr.

     stream nextPutAll: 'OSiOSDriver isSuitable: '.
     [stream print: OSiOSDriver isSuitable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: Call via variable"
    stream cr; nextPutAll: '--- Test 2: Call via variable ---'; cr.
    [| cls |
     cls := OSiOSDriver.
     stream nextPutAll: 'cls = '; print: cls; cr.
     stream nextPutAll: 'cls isSuitable: '.
     [stream print: cls isSuitable] on: Error do: [:e |
       stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
     stream cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: Call inside block"
    stream cr; nextPutAll: '--- Test 3: Call inside block ---'; cr.
    [[| cls |
      cls := OSiOSDriver.
      stream nextPutAll: 'Inside block, cls = '; print: cls; cr.
      stream nextPutAll: 'cls isSuitable: '.
      [stream print: cls isSuitable] on: Error do: [:e |
        stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
      stream cr] value]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: Call inside do: block"
    stream cr; nextPutAll: '--- Test 4: Call inside do: block ---'; cr.
    [{OSNullWindowDriver. OSSDL2Driver. OSiOSDriver} do: [:cls |
       stream nextPutAll: 'cls = '; print: cls; cr.
       stream nextPutAll: 'cls isSuitable: '.
       [stream print: cls isSuitable] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr.
       stream flush]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: Call inside subclassesDo: block"
    stream cr; nextPutAll: '--- Test 5: Call inside subclassesDo: block ---'; cr.
    [OSWindowDriver class subclassesDo: [:aClass |
       stream nextPutAll: 'aClass = '; print: aClass; cr.
       stream nextPutAll: 'aClass isSuitable: '.
       [stream print: aClass isSuitable] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr.
       stream flush]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!IsSuitableTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!IsSuitableTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #IsSuitableTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
