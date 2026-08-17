"Non-local return must reach the right ACTIVATION, not just the right method.

 A `^` inside a block returns from the activation that lexically created the
 block. The VM used to find that activation by walking the frame stack top-down
 looking for one running the block's home method. Method identity cannot
 distinguish two activations of the same method, and the top-down walk picks the
 innermost — so whenever the home was an OUTER activation of a method that is
 also on the stack deeper in, the return went to the wrong frame.

 This is not an exotic shape. `BlockClosure>>valueWithExit` is

     valueWithExit
         ^self value: [ ^nil ]

 so the exit block's home is the valueWithExit activation, and any nested use
 puts two activations of that one method on the stack. The outer exit then
 returned from the inner activation, which simply does not stop the outer loop.

 It surfaced as RSLinePlotTest never finishing. NSNiceLinearTicksGenerator is
 built from four nested valueWithExit blocks; its `break` never fired, so the
 tick search spun forever on any ordinary axis range and took the whole test run
 down with it. The generator returned no ticks at all for 0..10, 0..1, 1..100 or
 anything else — only the degenerate 0..0 case terminated.

 The fix matches on the closure's outerContext, which names the activation
 exactly. Written 2026-08-16."!

TestCase subclass: #NonLocalReturnActivationTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Kernel'!

!NonLocalReturnActivationTest methodsFor: 'helpers'!
runWithLimit: aBlock
  "Evaluate aBlock, answering #timeout rather than hanging the whole run.
   These tests fail by not terminating, so they need a bound of their own."
  ^ aBlock valueWithin: 5 seconds onTimeout: [ #timeout ]! !

!NonLocalReturnActivationTest methodsFor: 'tests'!
testExitFromOuterValueWithExitStopsBoundedLoop
  "The clearest statement of the bug, and it needs no loop that can hang: the
   outer exit should stop the iteration at 3, and under the bug it ran all 100
   times because `break value` returned from the inner valueWithExit instead."
  | z seen |
  z := 0.
  seen := OrderedCollection new.
  [ :break |
    1 to: 100 do: [ :i |
      [ :continue |
        z := z + 1.
        seen add: z.
        z >= 3 ifTrue: [ break value ].
        continue value.
        seen add: 999 ] valueWithExit ] ] valueWithExit.
  self assert: seen asArray equals: #(1 2 3)! !

!NonLocalReturnActivationTest methodsFor: 'tests'!
testExitFromOuterValueWithExitTerminatesUnboundedLoop
  "The same shape around [ true ] whileTrue:, which is what the tick generator
   actually uses. Under the bug this does not fail, it never returns."
  | result |
  result := self runWithLimit: [
    | z seen |
    z := 0.
    seen := OrderedCollection new.
    [ :break |
      [ true ] whileTrue: [
        [ :continue |
          z := z + 1.
          seen add: z.
          z >= 3 ifTrue: [ break value ].
          continue value ] valueWithExit ] ] valueWithExit.
    seen asArray ].
  self assert: result equals: #(1 2 3)! !

!NonLocalReturnActivationTest methodsFor: 'tests'!
testExitWorksWithNoInnerBlockPresent
  "Guards the simple case, so a fix for the nested one cannot break it."
  | z seen |
  z := 0.
  seen := OrderedCollection new.
  [ :break |
    [ true ] whileTrue: [
      z := z + 1.
      seen add: z.
      z >= 3 ifTrue: [ break value ] ] ] valueWithExit.
  self assert: seen asArray equals: #(1 2 3)! !

!NonLocalReturnActivationTest methodsFor: 'tests'!
testThreeLevelsOfValueWithExit
  "Each exit unwinds exactly its own level and no more."
  | seen |
  seen := OrderedCollection new.
  [ :a |
    seen add: 1.
    [ :b |
      seen add: 2.
      [ :c | seen add: 3. c value. seen add: 97 ] valueWithExit.
      seen add: 4 ] valueWithExit.
    seen add: 5 ] valueWithExit.
  self assert: seen asArray equals: #(1 2 3 4 5)! !

!NonLocalReturnActivationTest methodsFor: 'tests'!
testTickGeneratorTerminatesOnOrdinaryRanges
  "The real-world consequence. NSNiceLinearTicksGenerator is nothing but nested
   valueWithExit blocks; with the bug it returned nothing for any ordinary axis
   range. Guarded by a time limit because the failure mode is non-termination."
  | gen |
  gen := Smalltalk at: #NSNiceLinearTicksGenerator ifAbsent: [ nil ].
  gen ifNil: [ ^ self skip ].
  #( #(0 10) #(0 1) #(1 100) #(-1 1) ) do: [ :pair |
    | result |
    result := self runWithLimit: [
      (gen new searchMin: (pair at: 1) max: (pair at: 2) desired: 5) printString ].
    self
      deny: result = #timeout
      description: 'tick search did not terminate for ', pair printString ]! !
