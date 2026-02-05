"Test non-local return from nested blocks"!

Object subclass: #NonLocalReturnTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests'!

!NonLocalReturnTest class methodsFor: 'testing'!
testSimpleBlockReturn
  "Should return 42, not 99"
  [^ 42] value.
  ^ 99! !

!NonLocalReturnTest class methodsFor: 'testing'!
testNestedBlockReturn
  "Should return 42, not 99"
  #(1 2 3) do: [:each |
    each = 2 ifTrue: [^ 42]].
  ^ 99! !

!NonLocalReturnTest class methodsFor: 'testing'!
testDoIfPresentReturn
  "Like classAndMethodFor:do:ifAbsent:"
  | found |
  found := false.
  #(a b c) do: [:sym |
    sym = #b ifTrue: [
      found := true.
      ^ #foundIt]].
  ^ #notFound! !

!NonLocalReturnTest class methodsFor: 'testing'!
testExactStructure
  "Exactly like classAndMethodFor:do:ifAbsent: structure"
  | dict |
  dict := Dictionary new.
  dict at: #key put: #value.
  #(key other) do: [:sym |
    dict at: sym ifPresent: [:val |
      ^ val]].
  ^ #absent! !

!NonLocalReturnTest class methodsFor: 'testing'!
testWithCollection
  "Test with a real collection iteration"
  | coll |
  coll := OrderedCollection with: 1 with: 2 with: 3.
  coll do: [:each |
    each = 2 ifTrue: [^ #found2]].
  ^ #notFound! !

!NonLocalReturnTest class methodsFor: 'helpers'!
helperWithDo: aBlock
  "Helper that wraps iteration"
  #(a b c) do: [:each | aBlock value: each]! !

!NonLocalReturnTest class methodsFor: 'testing'!
testHelperWithReturn
  "Test non-local return through helper"
  self helperWithDo: [:each |
    each = #b ifTrue: [^ #foundB]].
  ^ #notFound! !

!NonLocalReturnTest class methodsFor: 'testing'!
testCull
  "Test non-local return through cull:"
  | block |
  block := [:x | ^ x * 2].
  block cull: 21.
  ^ 99! !

!NonLocalReturnTest class methodsFor: 'testing'!
testValueColon
  "Test non-local return through value:"
  | block |
  block := [:x | ^ x * 2].
  block value: 21.
  ^ 99! !

!NonLocalReturnTest class methodsFor: 'testing'!
testNestedCull
  "Test non-local return through nested cull"
  | outer inner |
  inner := [:v | ^ v].
  outer := [:dict :key |
    dict at: key ifPresent: [:val | inner cull: val] ifAbsent: [nil]].
  outer value: (Dictionary new at: #k put: #found; yourself) value: #k.
  ^ #absent! !

!NonLocalReturnTest class methodsFor: 'helpers'!
myIfPresent: aBlock for: dict key: key
  "Simple at:ifPresent: implementation"
  | v |
  v := dict at: key ifAbsent: [^ nil].
  ^ aBlock cull: v! !

!NonLocalReturnTest class methodsFor: 'testing'!
testMyIfPresent
  "Test with custom ifPresent"
  | dict |
  dict := Dictionary new at: #key put: #value; yourself.
  self myIfPresent: [:v | ^ v] for: dict key: #key.
  ^ #absent! !

!NonLocalReturnTest class methodsFor: 'testing'!
testDictAtIfPresent
  "Test Dictionary at:ifPresent: directly (not in loop)"
  | dict |
  dict := Dictionary new at: #key put: #value; yourself.
  dict at: #key ifPresent: [:v | ^ v].
  ^ #absent! !

!NonLocalReturnTest class methodsFor: 'testing'!
testIterateWithMyIfPresent
  "Test iteration with custom ifPresent"
  | dict |
  dict := Dictionary new at: #key put: #value; yourself.
  #(key other) do: [:sym |
    self myIfPresent: [:v | ^ v] for: dict key: sym].
  ^ #absent! !

!NonLocalReturnTest class methodsFor: 'testing'!
testIterateWithDictAt
  "Test iteration with real Dictionary at:ifPresent:"
  | dict |
  dict := Dictionary new at: #key put: #value; yourself.
  #(key other) do: [:sym |
    dict at: sym ifPresent: [:v | ^ v]].
  ^ #absent! !

!NonLocalReturnTest class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/nonlocal_return_test.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== Non-Local Return Tests ==='; cr; cr.

    stream nextPutAll: '1. Simple block return: '.
    stream print: self testSimpleBlockReturn; cr.
    stream nextPutAll: '   Expected: 42'; cr.
    stream flush.

    stream cr; nextPutAll: '2. Nested block return (do:ifTrue:): '.
    stream print: self testNestedBlockReturn; cr.
    stream nextPutAll: '   Expected: 42'; cr.
    stream flush.

    stream cr; nextPutAll: '3. do:ifTrue: with intermediate assignment: '.
    stream print: self testDoIfPresentReturn; cr.
    stream nextPutAll: '   Expected: #foundIt'; cr.
    stream flush.

    stream cr; nextPutAll: '4. Exact classAndMethodFor structure: '.
    stream print: self testExactStructure; cr.
    stream nextPutAll: '   Expected: #value'; cr.
    stream flush.

    stream cr; nextPutAll: '5. With OrderedCollection: '.
    stream print: self testWithCollection; cr.
    stream nextPutAll: '   Expected: #found2'; cr.
    stream flush.

    stream cr; nextPutAll: '6. Through helper method: '.
    stream print: self testHelperWithReturn; cr.
    stream nextPutAll: '   Expected: #foundB'; cr.
    stream flush.

    stream cr; nextPutAll: '7. Through cull: '.
    stream print: self testCull; cr.
    stream nextPutAll: '   Expected: 42'; cr.
    stream flush.

    stream cr; nextPutAll: '8. Through value: '.
    stream print: self testValueColon; cr.
    stream nextPutAll: '   Expected: 42'; cr.
    stream flush.

    stream cr; nextPutAll: '9. Nested cull: '.
    stream print: self testNestedCull; cr.
    stream nextPutAll: '   Expected: #found'; cr.
    stream flush.

    stream cr; nextPutAll: '10. Custom myIfPresent: '.
    stream print: self testMyIfPresent; cr.
    stream nextPutAll: '   Expected: #value'; cr.
    stream flush.

    stream cr; nextPutAll: '11. Dict at:ifPresent: directly: '.
    stream print: self testDictAtIfPresent; cr.
    stream nextPutAll: '   Expected: #value'; cr.
    stream flush.

    stream cr; nextPutAll: '12. Iterate with myIfPresent: '.
    stream print: self testIterateWithMyIfPresent; cr.
    stream nextPutAll: '   Expected: #value'; cr.
    stream flush.

    stream cr; nextPutAll: '13. Iterate with dict at:ifPresent:: '.
    stream print: self testIterateWithDictAt; cr.
    stream nextPutAll: '   Expected: #value'; cr.
    stream flush.

    stream cr; nextPutAll: '=== DONE ==='; cr]
  ensure: [stream close].
  Smalltalk exitSuccess! !

!NonLocalReturnTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!NonLocalReturnTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #NonLocalReturnTest)
  inCategory: SessionManager default userCategory
  atPriority: 90!
