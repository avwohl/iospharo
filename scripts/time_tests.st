"Per-class test timing. Run on both VMs and compare.
 Output: /tmp/sunit_timing.txt (TSV: className, testCount, totalMs)"!

Object subclass: #SUnitTimer
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Timer'!

!SUnitTimer class methodsFor: 'running'!
runAllTimed
  | testClasses stream |
  testClasses := OrderedCollection new.
  TestCase allSubclassesDo: [:cls |
    (cls isAbstract not and: [cls allTestSelectors notEmpty])
      ifTrue: [testClasses add: cls]].
  testClasses := testClasses sorted: [:a :b | a name <= b name].

  stream := '/tmp/sunit_timing.txt' asFileReference writeStream.
  [
    stream nextPutAll: 'class'; nextPut: Character tab.
    stream nextPutAll: 'tests'; nextPut: Character tab.
    stream nextPutAll: 'ms'; cr. stream flush.

    [TestCase defaultTimeLimit: 120 seconds] on: Error do: [:e |].

    testClasses do: [:tc |
      | sels startMs endMs elapsed p f e sk |
      sels := tc allTestSelectors asArray.
      "Skip dangerous/known-hanging tests"
      sels := sels reject: [:sel |
        #(testOneGBAllocation testOneGWordAllocation
          testMethodsOfTheClassShouldNotBeRepeatedInItsSuperclasses
          testTraitExplicitRequirementMethodsMustBeImplementedInTheClassOrInASuperclass
          testTransformingDeprecation) includes: sel].

      p := 0. f := 0. e := 0. sk := 0.
      startMs := Time millisecondClockValue.
      sels do: [:sel |
        | testDone doneSem testProcess watchdog |
        testDone := false.
        doneSem := Semaphore new.
        testProcess := [
          [[[(tc selector: sel) runCase]
            on: TestSkipped do: [:ex | sk := sk + 1]
            on: TestFailure do: [:ex | f := f + 1]]
            on: Error do: [:ex | e := e + 1]]
          ensure: [testDone := true. doneSem signal]] forkAt: 40.
        watchdog := [
          (Delay forMilliseconds: 15000) wait.
          testDone ifFalse: [
            testDone := true. doneSem signal.
            [testProcess terminate] on: Error do: [:e2 |]]] forkAt: 79.
        doneSem wait.
        [watchdog suspend] on: Error do: [:e2 |].
        testDone ifFalse: [testDone := true].
        p := p + 1].
      endMs := Time millisecondClockValue.
      elapsed := (endMs - startMs) bitAnd: 16r1FFFFFFF.

      stream nextPutAll: tc name; nextPut: Character tab.
      stream print: sels size; nextPut: Character tab.
      stream print: elapsed; cr. stream flush.

      Transcript show: tc name, ': ', sels size printString, ' tests in ', elapsed printString, 'ms'; cr]
  ] ensure: [stream close].

  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!SUnitTimer class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self runAllTimed! !

!SUnitTimer class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #SUnitTimer)
  inCategory: SessionManager default userCategory
  atPriority: 90!
