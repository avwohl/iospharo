"Test block argument handling"!

Object subclass: #BlockArgTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!BlockArgTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/block_arg.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Block Argument Test ==='; cr; cr.

    "Test 1: Simple block argument"
    stream nextPutAll: '--- Test 1: Simple block arg ---'; cr.
    [#(1 2 3) do: [:n |
       stream nextPutAll: 'n = '; print: n; cr.
       stream nextPutAll: 'n class = '; print: n class; cr.
       stream nextPutAll: 'n + 1 = '; print: n + 1; cr.
       stream flush]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: Block arg with message send"
    stream cr; nextPutAll: '--- Test 2: Block arg message send ---'; cr.
    [#(Object Array String) do: [:cls |
       stream nextPutAll: 'cls = '; print: cls; cr.
       stream nextPutAll: 'cls name = '; print: cls name; cr.
       stream flush]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: Block arg with class method"
    stream cr; nextPutAll: '--- Test 3: Block arg class method ---'; cr.
    [{Object. Array. String} do: [:cls |
       stream nextPutAll: 'cls = '; print: cls; cr.
       stream nextPutAll: 'cls class = '; print: cls class; cr.
       stream nextPutAll: 'Calling cls new...'; cr.
       stream flush.
       [| inst |
        inst := cls new.
        stream nextPutAll: 'cls new = '; print: inst; cr]
       on: Error do: [:e |
         stream nextPutAll: 'ERROR: '; print: e messageText; cr].
       stream flush]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: Nested block with captured variable"
    stream cr; nextPutAll: '--- Test 4: Nested block ---'; cr.
    [| outerVar |
     outerVar := #(10 20 30).
     outerVar do: [:n |
       stream nextPutAll: 'Outer n = '; print: n; cr.
       [stream nextPutAll: 'Inner n = '; print: n; cr] value.
       stream flush]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: subclassesDo: with simple message"
    stream cr; nextPutAll: '--- Test 5: subclassesDo: ---'; cr.
    [OSWindowDriver class subclassesDo: [:aClass |
       stream nextPutAll: 'aClass = '; print: aClass; cr.
       stream nextPutAll: 'aClass class = '.
       [stream print: aClass class] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr.
       stream nextPutAll: 'aClass name = '.
       [stream print: aClass name] on: Error do: [:e |
         stream nextPutAll: '(ERROR: '; print: e messageText; nextPutAll: ')'].
       stream cr.
       stream flush]]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!BlockArgTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!BlockArgTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #BlockArgTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
