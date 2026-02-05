"Test frame depth during exception handling"!

Object subclass: #FrameDepthTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!FrameDepthTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/framedepth.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Frame Depth Test ==='; cr; cr.

    "Test A: Simple NLR without exception"
    stream nextPutAll: '--- Test A: Simple NLR (no exception) ---'; cr.
    [stream nextPutAll: 'testA = '; print: self testA; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test B: NLR after exception in do:"
    stream cr; nextPutAll: '--- Test B: NLR after exception in do: ---'; cr.
    [stream nextPutAll: 'testB = '; print: self testB; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test C: Ensure frame depth matches"
    stream cr; nextPutAll: '--- Test C: Context depth analysis ---'; cr.
    [self testC: stream]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!FrameDepthTest class methodsFor: 'tests'!
testA
  "Simple NLR"
  #(1 2 3) do: [:x | x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!FrameDepthTest class methodsFor: 'tests'!
testB
  "NLR after exception"
  #(1 2 3) do: [:x |
    [self error: 'test'] on: Error do: [:e | ].
    x = 2 ifTrue: [^ #found]].
  ^ #notFound! !

!FrameDepthTest class methodsFor: 'tests'!
testC: stream
  "Analyze context chain"
  | ctx count |
  stream nextPutAll: 'Direct context chain from thisContext:'; cr.
  ctx := thisContext.
  count := 0.
  [ctx notNil and: [count < 20]] whileTrue: [
    stream nextPutAll: '  '; print: count; nextPutAll: ': '; print: ctx selector; cr.
    ctx := ctx sender.
    count := count + 1].

  stream cr; nextPutAll: 'Context chain INSIDE do: block:'; cr.
  #(1 2 3) do: [:x |
    ctx := thisContext.
    count := 0.
    [ctx notNil and: [count < 10]] whileTrue: [
      stream nextPutAll: '  '; print: count; nextPutAll: ': '; print: ctx selector; cr.
      ctx := ctx sender.
      count := count + 1].
    stream nextPutAll: '  (showing for x='; print: x; nextPutAll: ')'; cr.
    x = 1 ifTrue: [
      stream cr; nextPutAll: 'Context chain AFTER exception in do:'; cr.
      [self error: 'test'] on: Error do: [:e | ].
      ctx := thisContext.
      count := 0.
      [ctx notNil and: [count < 10]] whileTrue: [
        stream nextPutAll: '  '; print: count; nextPutAll: ': '; print: ctx selector; cr.
        ctx := ctx sender.
        count := count + 1].
      stream nextPutAll: '  (after exception)'; cr]].
  stream nextPutAll: 'After do: loop completed'; cr! !

!FrameDepthTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!FrameDepthTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #FrameDepthTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
