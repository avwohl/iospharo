"The stack-depth limit must count the whole depth of a process.

 The VM stops a runaway recursion with a catchable Error when the process is
 about 56000 frames deep. Before the fix of 2026-11-15 the count covered only
 the frames since the last process switch. The older part of the stack lives in the heap
 Context chain, and the count did not include it. A recursion that was switched
 out often never reached the limit. Two things switch a process out: a yield
 partner at the same priority, and a higher priority process that wakes up
 often (a Delay poller). The Context chain then grew until old space was full
 and the VM stopped.

 The SUnit runner has such a poller (priority 79). Because of the poller,
 ProcessTest>>testStackOverflow (Pharo 14) failed on the interpreter-only build:
 the recursion ran until old space was full, and the test got an error that
 was not the overflow. On the JIT build the same recursion ended inside one
 time slice, so that test passed by speed. A recursion with a yield partner
 failed on the JIT build too.

 Every runaway recursion here carries its depth and answers #noOverflow when it
 reaches hardBound (200000 levels, far above any real limit). A VM with the
 defect then fails the test by assertion. Measured on the binaries before the
 fix: the class ends in less than 2 s, and the peak memory is about 500 MB
 (about 350 MB with the fix). The test cannot exhaust memory.

 The tests assert that an Error is signalled and that the recursion stops
 below hardBound. They do not assert on the message text or on the limit
 value, which belong to one VM. The legal recursions are 30000 to 40000 deep,
 which is legal on every VM that has a limit of 56000 or more.

 On an image that has Process>>shouldOverflow: (Pharo 14) the helper process
 uses shouldOverflow: true, as ProcessTest>>testStackOverflow does. Pharo 13
 has no such selector, and the same tests run without it.

 Written 2026-11-15."!

TestCase subclass: #StackDepthLimitTest
  instanceVariableNames: 'processes'
  classVariableNames: ''
  package: 'Tests-Kernel'!

!StackDepthLimitTest methodsFor: 'running'!
setUp
  super setUp.
  processes := OrderedCollection new! !

!StackDepthLimitTest methodsFor: 'running'!
tearDown
  "Every helper process, partner and poller dies here, also after a failure."
  processes ifNotNil: [ :all | all do: [ :each | each terminate ] ].
  processes := nil.
  super tearDown! !

!StackDepthLimitTest methodsFor: 'helpers'!
hardBound
  "Depth at which a runaway helper gives up and answers #noOverflow."
  ^ 200000! !

!StackDepthLimitTest methodsFor: 'helpers'!
waitMilliseconds
  "Longest wait for one helper process. A timeout is a test failure."
  ^ 30000! !

!StackDepthLimitTest methodsFor: 'helpers'!
runaway: n state: st yield: k shape: shape
  "Recurse until the VM signals an Error, or until hardBound. st at: 1 is the
   deepest level seen (updated every 250 levels). The process yields every k
   levels. The shape selects how the next level is entered."
  n >= self hardBound ifTrue: [ ^ #noOverflow ].
  (n \\ 250) = 0 ifTrue: [ st at: 1 put: n ].
  (n \\ k) = 0 ifTrue: [ Processor yield ].
  shape == #this ifTrue: [ thisContext sender ].
  shape == #perform ifTrue: [
    ^ self perform: #runaway:state:yield:shape: with: n + 1 with: st with: k with: shape ].
  shape == #block ifTrue: [
    ^ [ :m | self runaway: m state: st yield: k shape: shape ] value: n + 1 ].
  shape == #nlr ifTrue: [
    #(1) do: [ :x | ^ self runaway: n + 1 state: st yield: k shape: shape ] ].
  ^ self runaway: n + 1 state: st yield: k shape: shape! !

!StackDepthLimitTest methodsFor: 'helpers'!
runawayEnsure: n state: st yield: k
  "Like runaway, with an ensure: block at every tenth level. st at: 2 counts the
   ensure: blocks entered and st at: 3 counts the ones that ran."
  | r |
  n >= self hardBound ifTrue: [ ^ #noOverflow ].
  st at: 1 put: n.
  (n \\ k) = 0 ifTrue: [ Processor yield ].
  (n \\ 10) = 0
    ifTrue: [
      st at: 2 put: (st at: 2) + 1.
      r := [ self runawayEnsure: n + 1 state: st yield: k ]
        ensure: [ st at: 3 put: (st at: 3) + 1 ] ]
    ifFalse: [ r := self runawayEnsure: n + 1 state: st yield: k ].
  ^ r! !

!StackDepthLimitTest methodsFor: 'helpers'!
overflowing: aBlock
  "Evaluate aBlock with shouldOverflow: true on the active process when the
   image has Process>>shouldOverflow: (Pharo 14, as ProcessTest>>testStackOverflow
   does), and reset the flag afterwards. The flag is set only around a runaway
   recursion, because a VM that honours the flag at every stack page (stock Cog)
   signals the Error in a legal deep recursion too."
  | p |
  p := Processor activeProcess.
  (p respondsTo: #shouldOverflow:) ifFalse: [ ^ aBlock value ].
  p shouldOverflow: true.
  ^ [ aBlock value ] ensure: [ p shouldOverflow: false ]! !

!StackDepthLimitTest methodsFor: 'helpers'!
runawayWith: st yield: k shape: shape
  "A runaway recursion from level 0, with the overflow flag set."
  ^ self overflowing: [ self runaway: 0 state: st yield: k shape: shape ]! !

!StackDepthLimitTest methodsFor: 'helpers'!
legal: n yield: k
  "A finite recursion n levels deep that yields every k levels. Answers n."
  n = 0 ifTrue: [ ^ 0 ].
  (n \\ k) = 0 ifTrue: [ Processor yield ].
  ^ (self legal: n - 1 yield: k) + 1! !

!StackDepthLimitTest methodsFor: 'helpers'!
newState
  ^ Array with: 0 with: 0 with: 0! !

!StackDepthLimitTest methodsFor: 'helpers'!
startPartner
  "A process at the active priority that yields forever. A recursion that yields
   is switched out to this process each time."
  processes add: ([ [ Processor yield ] repeat ] fork)! !

!StackDepthLimitTest methodsFor: 'helpers'!
startPoller
  "A process above the helper that wakes up every millisecond, like the poller of
   the SUnit runner."
  processes add: ([ [ (Delay forMilliseconds: 1) wait ] repeat ] forkAt: 79)! !

!StackDepthLimitTest methodsFor: 'helpers'!
spawn: aBlock state: st
  "Run aBlock in a new process at the active priority. Answer a record. An
   Error that reaches the top of aBlock is stored in the record, and the result
   is then #caught."
  | rec p |
  rec := Dictionary new.
  rec at: #done put: Semaphore new.
  rec at: #state put: st.
  p := [
    rec at: #result put: ([ aBlock value ]
      on: Error
      do: [ :e |
        rec at: #error put: e class.
        rec at: #text put: e messageText printString.
        e return: #caught ]).
    (rec at: #done) signal ] newProcess.
  p priority: Processor activePriority.
  processes add: p.
  p resume.
  ^ rec! !

!StackDepthLimitTest methodsFor: 'helpers'!
await: rec label: aString
  "Wait for a record from spawn:state:. A timeout is a failure."
  ((rec at: #done) waitTimeoutMilliseconds: self waitMilliseconds)
    ifTrue: [
      self assert: false description: aString, ': helper process did not finish in ',
        self waitMilliseconds printString, ' ms (reached level ',
        ((rec at: #state) at: 1) printString, ')' ]! !

!StackDepthLimitTest methodsFor: 'helpers'!
assertOverflowed: rec label: aString
  "The helper got an Error from the overflow: the recursion was deep when the
   Error came, it stopped below hardBound, and the Error is no other kind of
   failure."
  | result cls reached |
  result := rec at: #result.
  reached := (rec at: #state) at: 1.
  result == #noOverflow ifTrue: [
    self assert: false description: aString, ': the recursion reached the hard bound of ',
      self hardBound printString, ' levels and no Error was signalled' ].
  self assert: result == #caught
    description: aString, ': result was ', result printString,
      ' and not an Error from the overflow'.
  cls := rec at: #error.
  self assert: (cls == Error or: [ cls inheritsFrom: Error ])
    description: aString, ': the exception was not an Error'.
  self deny: (cls == MessageNotUnderstood or: [ cls inheritsFrom: MessageNotUnderstood ])
    description: aString, ': the Error was a doesNotUnderstand, not the overflow: ',
      (rec at: #text).
  self assert: reached >= 10000
    description: aString, ': the Error came at level ', reached printString,
      ', which is too early for an overflow: ', (rec at: #text).
  self assert: reached < self hardBound
    description: aString, ': the recursion reached the hard bound'! !

!StackDepthLimitTest methodsFor: 'tests'!
testYieldPartnerRecursionOverflows
  "Proves: a recursion that yields to a same-priority partner every 2000 levels
   gets a catchable Error. Before the fix the VM counted only the frames since
   the last switch (2000), so the limit was never reached."
  | st rec |
  st := self newState.
  self startPartner.
  rec := self spawn: [ self runawayWith: st yield: 2000 shape: #plain ] state: st.
  self await: rec label: 'yield partner'.
  self assertOverflowed: rec label: 'yield partner'! !

!StackDepthLimitTest methodsFor: 'tests'!
testPollerRecursionOverflows
  "Proves: a plain recursion that a priority 79 process interrupts every
   millisecond (a Delay poller) gets a catchable Error. The recursion never
   yields by itself. The poller is the only cause of the process switches."
  | st rec |
  st := self newState.
  self startPoller.
  rec := self spawn: [ self runawayWith: st yield: 1000000 shape: #plain ] state: st.
  self await: rec label: 'poller'.
  self assertOverflowed: rec label: 'poller'! !

!StackDepthLimitTest methodsFor: 'tests'!
testTwoRunawayProcessesEachOverflow
  "Proves: two runaway recursions that yield to each other both get their own
   catchable Error. A first version of the fix gave the Error to one process
   and killed the other."
  | st1 st2 rec1 rec2 |
  st1 := self newState.
  st2 := self newState.
  rec1 := self spawn: [ self runawayWith: st1 yield: 2000 shape: #plain ] state: st1.
  rec2 := self spawn: [ self runawayWith: st2 yield: 2000 shape: #plain ] state: st2.
  self await: rec1 label: 'process 1'.
  self await: rec2 label: 'process 2'.
  self assertOverflowed: rec1 label: 'process 1'.
  self assertOverflowed: rec2 label: 'process 2'! !

!StackDepthLimitTest methodsFor: 'tests'!
testLegalRecursionWithManySwitchesDoesNotSignal
  "Proves: a legal recursion (35000 levels, below the limit) that is switched
   out 70 times by a partner returns its value and signals nothing. This guards
   the fix against a false overflow, which would count the depth twice."
  | st rec |
  st := self newState.
  self startPartner.
  rec := self spawn: [ self legal: 35000 yield: 500 ] state: st.
  self await: rec label: 'legal with partner'.
  self assert: (rec at: #result) equals: 35000.
  self deny: (rec includesKey: #error)
    description: 'legal recursion signalled ', (rec at: #text ifAbsent: [ '' ])! !

!StackDepthLimitTest methodsFor: 'tests'!
testLegalRecursionWithPollerDoesNotSignal
  "Proves: legal recursions of 40000 levels that the poller interrupts return
   their value and signal nothing. One recursion ends in a few milliseconds.
   Twenty of them take long enough for the 1 ms poller to interrupt the process
   many times."
  | st rec |
  st := self newState.
  self startPoller.
  rec := self spawn: [
    | sum |
    sum := 0.
    20 timesRepeat: [ sum := sum + (self legal: 40000 yield: 1000000) ].
    sum ] state: st.
  self await: rec label: 'legal with poller'.
  self assert: (rec at: #result) equals: 800000.
  self deny: (rec includesKey: #error)
    description: 'legal recursion signalled ', (rec at: #text ifAbsent: [ '' ])! !

!StackDepthLimitTest methodsFor: 'tests'!
testManyLegalRecursionsDoNotSignal
  "Proves: eight processes with legal recursions of 31000 to 38000 levels, which
   yield to each other, all return their value. The depth of one process must
   not add to the depth of another."
  | recs |
  recs := (1 to: 8) collect: [ :i |
    self spawn: [ self legal: 30000 + (i * 1000) yield: 700 ] state: self newState ].
  recs doWithIndex: [ :rec :i | self await: rec label: 'process ', i printString ].
  recs doWithIndex: [ :rec :i |
    self assert: (rec at: #result) equals: 30000 + (i * 1000).
    self deny: (rec includesKey: #error)
      description: 'process ', i printString, ' signalled ', (rec at: #text ifAbsent: [ '' ]) ]! !

!StackDepthLimitTest methodsFor: 'tests'!
testLegalRecursionAfterCaughtOverflow
  "Proves: after a caught overflow the same process runs a legal recursion of
   35000 levels, with switches, and no false overflow comes. A depth count that
   stays high after the unwind would signal here."
  | st rec result |
  st := self newState.
  self startPartner.
  rec := self spawn: [
    | before overflow after |
    before := self legal: 35000 yield: 500.
    overflow := [ self runawayWith: st yield: 2000 shape: #plain ]
      on: Error do: [ :e | e return: #caught ].
    after := self legal: 35000 yield: 500.
    Array with: before with: overflow with: after ] state: st.
  self await: rec label: 'legal after overflow'.
  self deny: (rec includesKey: #error)
    description: 'the helper process got an Error outside the overflow: ', (rec at: #text ifAbsent: [ '' ]).
  result := rec at: #result.
  self assert: (result at: 1) equals: 35000.
  self assert: (result at: 2) == #caught
    description: 'the runaway recursion was not stopped: ', (result at: 2) printString.
  self assert: (st at: 1) >= 10000 & ((st at: 1) < self hardBound)
    description: 'the overflow came at level ', (st at: 1) printString.
  self assert: (result at: 3) equals: 35000! !

!StackDepthLimitTest methodsFor: 'tests'!
testOverflowTwiceInSameProcess
  "Proves: a process that caught an overflow can overflow and catch again, three
   times in a row. The depth count must restart when the first Error unwinds."
  | states rec result |
  states := (1 to: 3) collect: [ :i | self newState ].
  self startPartner.
  rec := self spawn: [
    states collect: [ :st |
      [ self runawayWith: st yield: 2000 shape: #plain ]
        on: Error do: [ :e | e return: #caught ] ] ] state: states first.
  self await: rec label: 'three overflows'.
  self deny: (rec includesKey: #error)
    description: 'the helper process got an Error outside the overflows: ', (rec at: #text ifAbsent: [ '' ]).
  result := rec at: #result.
  result doWithIndex: [ :each :i |
    self assert: each == #caught
      description: 'overflow ', i printString, ' answered ', each printString, ' and not #caught'.
    self assert: ((states at: i) at: 1) >= 10000 & (((states at: i) at: 1) < self hardBound)
      description: 'overflow ', i printString, ' came at level ', ((states at: i) at: 1) printString ]! !

!StackDepthLimitTest methodsFor: 'tests'!
testEnsureBlocksRunWhenOverflowUnwinds
  "Proves: the ensure: blocks of a recursion that was switched out often run
   when the Error unwinds the recursion. The recursion has an ensure: block at
   every tenth level. The VM may signal the Error in the send of an ensure:
   itself, so one ensure: block may not have been set up. All other blocks run."
  | st rec entered ran |
  st := self newState.
  self startPartner.
  rec := self spawn: [ self overflowing: [ self runawayEnsure: 0 state: st yield: 2000 ] ] state: st.
  self await: rec label: 'ensure'.
  self assertOverflowed: rec label: 'ensure'.
  entered := st at: 2.
  ran := st at: 3.
  self assert: entered > 1000
    description: 'only ', entered printString, ' ensure: blocks were entered'.
  self assert: (ran <= entered and: [ ran >= (entered - 1) ])
    description: entered printString, ' ensure: blocks were entered and ', ran printString, ' ran'! !

!StackDepthLimitTest methodsFor: 'tests'!
testThisContextAtEveryLevelOverflows
  "Proves: a recursion that reads thisContext sender at every level (so every
   frame has a Context object) and yields every 100 levels gets a catchable
   Error."
  | st rec |
  st := self newState.
  self startPartner.
  rec := self spawn: [ self runawayWith: st yield: 100 shape: #this ] state: st.
  self await: rec label: 'thisContext'.
  self assertOverflowed: rec label: 'thisContext'! !

!StackDepthLimitTest methodsFor: 'tests'!
testPerformBlockAndNonLocalReturnRecursionsOverflow
  "Proves: a recursion through perform:, through a block, and through a block
   with a non-local return, each switched out by a partner every 2000 levels,
   gets a catchable Error. These are three other ways to build frames."
  #(#perform #block #nlr) do: [ :shape |
    | st rec |
    st := self newState.
    self startPartner.
    rec := self spawn: [ self runawayWith: st yield: 2000 shape: shape ] state: st.
    self await: rec label: shape.
    self assertOverflowed: rec label: shape ]! !
