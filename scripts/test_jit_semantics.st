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
 - super sends and 3+ argument sends exit or call natively through a helper;
 - primitive 188 (withArgs:executeMethod:, valueWithReceiver:arguments:) ran
   BlockClosure>>value's primitive with method_ set to BlockClosure>>value,
   so the caller's frame was recorded with the wrong method; after a process
   switch it resumed as BlockClosure>>value on a CompiledMethod (DNU
   #numArgsError:).  Method proxies (methodproxies, OCSpecialSelectorTest)
   reach it through valueUnpreemptively;
 - the simStack (deferred operand pushes) must keep a pending read of a
   temp ahead of a store to it, and the IC site numbering of == (2026-10-14);
 - two-way become (primitive 128) moved the identity hash with the contents;
   Spur keeps it with the references (copyHash false), so an IdentitySet
   still finds what its slot now refers to (ReStore's exclusion sets).

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
  ^ a + b + c!

pendingRead: x
  "simStack: the pending read of a takes its value before the store."
  | a |
  a := x.
  ^ a + (a := 10)!

nilOrThree: x
  "simStack: == fused with a conditional jump."
  ^ x == nil ifTrue: [#isNil] ifFalse: [x == 3 ifTrue: [#three] ifFalse: [#other]]!

keep: x
  "simStack: a store that keeps its value on the stack."
  | y z |
  z := (y := x + 1) + 1.
  ^ {y. z}!

sum: n
  "simStack: arithmetic in registers, with overflow into LargeInteger."
  | s |
  s := SmallInteger maxVal - n.
  1 to: n + 2 do: [:i | s := s + 1].
  ^ s! !

!JITSemanticsTest methodsFor: 'tests'!
testByteObjectEqualityNeedsStrings
  self deny: (self repeat: [ self found: (2 raisedTo: 100) in: (Array with: (2 raisedTo: 100) negated) ]).
  self deny: (self repeat: [ self found: #[97 98] in: (Array with: 'ab') ]).
  self assert: (self repeat: [ self found: 'ab' in: (Array with: #[97 98]) ]).
  self assert: (self repeat: [ self found: #abc in: (Array with: 'abc') ]).
  self assert: (self repeat: [ self found: 'abc' copy in: (Array with: 'abc') ]).
  self deny: (self repeat: [ self found: 'abd' in: (Array with: 'abc') ]).
  "Symbol>>=: identical -> true, another Symbol -> false, a String -> compare."
  self assert: (self repeat: [ self found: 'abc' copy in: (Array with: #abc) ]).
  self deny: (self repeat: [ self found: #abd in: (Array with: #abc) ]).
  self deny: (self repeat: [ self found: 'abd' in: (Array with: #abc) ])!

testArithmeticOnNonSmallIntegers
  self assert: (self repeat: [ (2 raisedTo: 100) + 1 ]) equals: 1267650600228229401496703205377.
  self assert: (self repeat: [ (1/2) + (1/3) ]) equals: 5/6.
  self assert: (self repeat: [ (3@4) + 1 ]) equals: 4@5.
  self assert: (self repeat: [ $a < $b ]).
  self deny: (self repeat: [ nil = 3 ])!

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

testExecuteMethodOnBlockAcrossProcessSwitch
  | r |
  r := OrderedCollection new.
  [ 30 timesRepeat: [ Processor yield ] ] forkAt: Processor activePriority.
  1 to: 20 do: [ :i |
    r add: ((BlockClosure >> #value) valueWithReceiver: [ Processor yield. i ] arguments: #()) ].
  self assert: r asArray equals: (1 to: 20) asArray!

testTwoWayBecomeKeepsHashWithReferences
  | a b h set |
  a := Object new. b := Object new. h := a identityHash.
  set := IdentitySet with: a.
  a become: b.
  self assert: a identityHash equals: h.
  self assert: (set includes: a).
  self deny: (set includes: b).
  a := Array new: 3. b := 'xy' copy. h := a identityHash.
  set := IdentitySet with: a.
  a become: b.
  self assert: a identityHash equals: h.
  self assert: (set includes: a).
  self assert: a class equals: ByteString!

testSimStackSemantics
  self assert: (self repeat: [ self pendingRead: 4 ]) equals: 14.
  self assert: (self repeat: [ self nilOrThree: nil ]) equals: #isNil.
  self assert: (self repeat: [ self nilOrThree: 3 ]) equals: #three.
  self assert: (self repeat: [ self nilOrThree: 'x' ]) equals: #other.
  self assert: (self repeat: [ self keep: 10 ]) equals: #(11 12).
  self assert: (self repeat: [ self sum: 5 ]) equals: SmallInteger maxVal + 2.
  self assert: (self repeat: [ self sum: 5 ]) class equals: LargePositiveInteger!

testSuperAndLongSends
  self assert: (self repeat: [ self three: 1 with: 2 and: 3 ]) equals: 6.
  self assert: (self repeat: [ (WriteStream on: String new) nextPutAll: 'ab'; nextPutAll: 'cd'; contents ])
    equals: 'abcd'! !
