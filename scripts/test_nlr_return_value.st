"Test NLR returning captured variables"!

Object subclass: #NLRReturnValueTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!NLRReturnValueTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/nlr_return_value.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== NLR Return Value Test ==='; cr; cr.

    "Test 1: Return literal"
    stream nextPutAll: '--- Test 1: Return literal ---'; cr.
    [| result |
     result := self testReturnLiteral.
     stream nextPutAll: 'testReturnLiteral: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 2: Return captured variable"
    stream cr; nextPutAll: '--- Test 2: Return captured variable ---'; cr.
    [| result |
     result := self testReturnCaptured.
     stream nextPutAll: 'testReturnCaptured: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 3: Return from nested block (like subclassesDo:)"
    stream cr; nextPutAll: '--- Test 3: Return from nested block ---'; cr.
    [| result |
     result := self testReturnFromNestedBlock.
     stream nextPutAll: 'testReturnFromNestedBlock: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 4: Exact replica of pickDriverClass"
    stream cr; nextPutAll: '--- Test 4: Exact replica ---'; cr.
    [| result |
     result := self exactPickDriverClass.
     stream nextPutAll: 'exactPickDriverClass: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    "Test 5: Inline subclassesDo: body"
    stream cr; nextPutAll: '--- Test 5: Inline body ---'; cr.
    [| result |
     result := self inlinePickDriverClass.
     stream nextPutAll: 'inlinePickDriverClass: '; print: result; cr]
    on: Error do: [:ex |
      stream nextPutAll: 'ERROR: '; print: ex messageText; cr].
    stream flush.

    stream cr; nextPutAll: '=== Done ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!NLRReturnValueTest class methodsFor: 'tests'!
testReturnLiteral
  #(1 2 3 4 5) do: [:n | n = 3 ifTrue: [^ 'literal']].
  ^ 'notfound'! !

!NLRReturnValueTest class methodsFor: 'tests'!
testReturnCaptured
  #(1 2 3 4 5) do: [:n | n = 3 ifTrue: [^ n]].
  ^ 0! !

!NLRReturnValueTest class methodsFor: 'tests'!
testReturnFromNestedBlock
  | outerBlock |
  outerBlock := [:n | n = 3 ifTrue: [^ n]].
  #(1 2 3 4 5) do: [:each | outerBlock value: each].
  ^ 0! !

!NLRReturnValueTest class methodsFor: 'tests'!
exactPickDriverClass
  OSWindowDriver class subclassesDo: [:aClass |
    aClass isSuitable ifTrue: [^ aClass]].
  ^ OSNullWindowDriver! !

!NLRReturnValueTest class methodsFor: 'tests'!
inlinePickDriverClass
  "Inline what subclassesDo: does"
  | aBlock |
  aBlock := [:aClass | aClass isSuitable ifTrue: [^ aClass]].
  OSWindowDriver class instanceSide subclasses do: [:each |
    aBlock value: each classSide].
  ^ OSNullWindowDriver! !

!NLRReturnValueTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!NLRReturnValueTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #NLRReturnValueTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
