"Diagnostic: reproduce RandomTest failure after ClassFactory creates TemporaryMockClass"!

Object subclass: #RandomDiag2
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Diag'!

!RandomDiag2 class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/random_diag2.txt' asFileReference writeStream.
  [self runDiag: stream] ensure: [stream close].
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!RandomDiag2 class methodsFor: 'running'!
runDiag: s
  s nextPutAll: '=== Random Diagnostic 2 ==='; cr.

  "Step 1: Test Random before ClassFactory"
  s nextPutAll: '--- Before ClassFactory ---'; cr.
  [| r |
   r := Random new.
   s nextPutAll: 'r next = '; nextPutAll: r next printString; cr.
   s nextPutAll: 'r next class = '; nextPutAll: r next class name; cr.
   s nextPutAll: 'Random BEFORE: OK'; cr] on: Error do: [:e |
   s nextPutAll: 'Random BEFORE ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  s flush.

  "Step 2: Create a TemporaryMockClass using ClassFactory (like BehaviorTest does)"
  s nextPutAll: '--- Creating ClassFactory classes ---'; cr.
  [| factory cls1 cls2 |
   factory := ClassFactoryForTestCase new.
   cls1 := factory newClass.
   s nextPutAll: 'Created class: '; nextPutAll: cls1 name; cr.
   cls2 := factory newClass.
   s nextPutAll: 'Created class: '; nextPutAll: cls2 name; cr.
   "Run some BehaviorTest-like operations"
   s nextPutAll: 'cls1 new: '; nextPutAll: cls1 new printString; cr.
   s nextPutAll: 'cls1 methods: '; nextPutAll: cls1 methods size printString; cr.
   "Clean up"
   factory cleanUp.
   s nextPutAll: 'ClassFactory OK'; cr] on: Error do: [:e |
   s nextPutAll: 'ClassFactory ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  s flush.

  "Step 3: Test Random AFTER ClassFactory"
  s nextPutAll: '--- After ClassFactory ---'; cr.
  [| r result |
   r := Random new.
   result := r next.
   s nextPutAll: 'r next = '; nextPutAll: result printString; cr.
   s nextPutAll: 'r next class = '; nextPutAll: result class name; cr.
   s nextPutAll: 'Random AFTER: OK'; cr] on: Error do: [:e |
   s nextPutAll: 'Random AFTER ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr.
   e signalerContext ifNotNil: [:ctx |
     | c2 cnt |
     c2 := ctx. cnt := 0.
     [c2 notNil and: [cnt < 15]] whileTrue: [
       s nextPutAll: '  '; nextPutAll: c2 printString; cr.
       c2 := c2 sender. cnt := cnt + 1]]].
  s flush.

  "Step 4: Run actual RandomTest tests"
  s nextPutAll: '--- Running RandomTest tests ---'; cr.
  #(testNext testNextBetweenAnd testNextInto testDistribution testWithFloatAsSeed testUnixRandomGeneratorSeed) do: [:sel |
    | test |
    test := RandomTest selector: sel.
    [test setUp.
     test performTest.
     s nextPutAll: sel; nextPutAll: ': PASS'; cr]
    on: Error do: [:e |
     s nextPutAll: sel; nextPutAll: ': ERROR - '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr.
     e signalerContext ifNotNil: [:ctx |
       | c2 cnt |
       c2 := ctx. cnt := 0.
       [c2 notNil and: [cnt < 10]] whileTrue: [
         s nextPutAll: '  '; nextPutAll: c2 printString; cr.
         c2 := c2 sender. cnt := cnt + 1]]]].
  s flush.

  s nextPutAll: '=== Done ==='; cr.
  s flush! !

!RandomDiag2 class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!RandomDiag2 class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #RandomDiag2)
  inCategory: SessionManager default userCategory
  atPriority: 90!
