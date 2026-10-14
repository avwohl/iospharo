"Machine-code semantics checks for the arm64 JIT (machine-code frames).

 Each test runs its expression in a loop so the methods involved are compiled
 and the compiled path is the one being checked. Every case below differed from
 the interpreter (and from stock Cog) on some build:

 - the inline `=` for byte objects compared any two byte objects, so large
   integers of opposite sign and 'ab' = #[97 98] answered true (2026-10-14);
 - lazy outer contexts must read live values: a block's outerContext answers
   its sender, temps and stack as image code would see them;
 - blocks with a non-local return are called natively and must still return
   to the right activation, and their local returns must deliver their value;
 - super sends and 3+ argument sends exit or call natively through a helper.

 Written 2026-10-14."!

TestCase subclass: #JITSemanticsTest
  instanceVariableNames: 'counter'
  classVariableNames: ''
  package: 'Tests-Kernel'!

!JITSemanticsTest methodsFor: 'helpers'!
repeat: aBlock
  "Evaluate aBlock often enough that the methods it runs are compiled,
   and answer the last value."
  | last |
  1 to: 3000 do: [ :i | last := aBlock value ].
  ^ last!

found: anObject in: aCollection
  "includes: compares with = in compiled code."
  ^ aCollection includes: anObject!

firstOver: n in: aCollection
  "A non-local return from a block called natively."
  aCollection do: [ :each | each > n ifTrue: [ ^ each ] ].
  ^ nil!

outerOf: x
  "A block whose outerContext is this activation."
  | t b |
  t := x * 2.
  b := [ ^ t ].
  ^ b outerContext!

three: a with: b and: c
  ^ a + b + c! !

!JITSemanticsTest methodsFor: 'tests'!
testByteObjectEqualityNeedsStrings
  self deny: (self repeat: [ self found: (2 raisedTo: 100) in: (Array with: (2 raisedTo: 100) negated) ]).
  self deny: (self repeat: [ self found: #[97 98] in: (Array with: 'ab') ]).
  self assert: (self repeat: [ self found: 'ab' in: (Array with: #[97 98]) ]).
  self assert: (self repeat: [ self found: #abc in: (Array with: 'abc') ]).
  self assert: (self repeat: [ self found: 'abc' copy in: (Array with: 'abc') ]).
  self deny: (self repeat: [ self found: 'abd' in: (Array with: 'abc') ])!

testNonLocalReturnFromNativeBlock
  self assert: (self repeat: [ self firstOver: 5 in: (1 to: 10) ]) equals: 6.
  self assert: (self repeat: [ self firstOver: 50 in: (1 to: 10) ]) isNil!

testBlockLocalReturns
  self assert: (self repeat: [ (1 to: 5) collect: [ :x | x even ifTrue: [ x ] ifFalse: [ nil ] ] ])
    equals: #(nil 2 nil 4 nil).
  self assert: (self repeat: [ [ :x | ] value: 3 ]) isNil.
  self assert: (self repeat: [ (1 to: 30) inject: 0 into: [ :s :e | s + ([ :q | q * 2 ] value: e) ] ])
    equals: 930!

testLazyOuterContextReadsLiveValues
  | ctx |
  ctx := self repeat: [ self outerOf: 7 ].
  self assert: (ctx tempAt: 1) equals: 7.
  self assert: (ctx tempAt: 2) equals: 14.
  self assert: ctx isDead.
  ctx := self repeat: [ [ :x | | t b | t := x * 2. b := [ ^ t ]. b outerContext sender ] value: 3 ].
  self assert: ctx notNil!

testSuperAndLongSends
  self assert: (self repeat: [ self three: 1 with: 2 and: 3 ]) equals: 6.
  self assert: (self repeat: [ (WriteStream on: String new) nextPutAll: 'ab'; nextPutAll: 'cd'; contents ])
    equals: 'abcd'! !
