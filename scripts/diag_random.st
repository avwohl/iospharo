"Standalone Random diagnostic - traces PCG computation step by step"!

Object subclass: #RandomDiag
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Diag'!

!RandomDiag class methodsFor: 'running'!
run
  | stream |
  stream := '/tmp/random_diag.txt' asFileReference writeStream.
  [self runDiag: stream] ensure: [stream close].
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!RandomDiag class methodsFor: 'running'!
runDiag: s
  | r state val |
  s nextPutAll: '=== Random Diagnostic ==='; cr.

  "Test basic DoubleWordArray"
  [| dwa |
   dwa := DoubleWordArray new: 1.
   s nextPutAll: 'DWA class: '; nextPutAll: dwa class name; cr.
   s nextPutAll: 'DWA size: '; nextPutAll: dwa size printString; cr.
   dwa at: 1 put: 42.
   s nextPutAll: 'DWA at:1 after put:42 = '; nextPutAll: (dwa at: 1) printString; cr.
   dwa at: 1 put: 16rFFFFFFFFFFFFFFFF.
   s nextPutAll: 'DWA at:1 after put:max64 = '; nextPutAll: (dwa at: 1) printString; cr.
   s nextPutAll: 'DWA at:1 class: '; nextPutAll: (dwa at: 1) class name; cr.
   s nextPutAll: 'DWA OK'; cr] on: Error do: [:e |
   s nextPutAll: 'DWA ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  s flush.

  "Test LargeInteger arithmetic"
  [| a b c |
   a := 6364136223846793005.
   s nextPutAll: 'Const a class: '; nextPutAll: a class name; cr.
   s nextPutAll: 'Const a value: '; nextPutAll: a printString; cr.
   b := 12345678901234567890.
   s nextPutAll: 'Const b class: '; nextPutAll: b class name; cr.
   c := a * b.
   s nextPutAll: 'a*b class: '; nextPutAll: c class name; cr.
   s nextPutAll: 'a*b = '; nextPutAll: c printString; cr.
   c := c bitAnd: 16rFFFFFFFFFFFFFFFF.
   s nextPutAll: 'masked class: '; nextPutAll: c class name; cr.
   s nextPutAll: 'masked = '; nextPutAll: c printString; cr.
   s nextPutAll: 'LargeInt arithmetic OK'; cr] on: Error do: [:e |
   s nextPutAll: 'LargeInt ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr.
   e signalerContext ifNotNil: [:ctx |
     | c2 cnt |
     c2 := ctx. cnt := 0.
     [c2 notNil and: [cnt < 15]] whileTrue: [
       s nextPutAll: '  '; nextPutAll: c2 printString; cr.
       c2 := c2 sender. cnt := cnt + 1]]].
  s flush.

  "Test Random new"
  [r := Random new.
   s nextPutAll: 'Random new OK'; cr.
   state := r instVarAt: 1.
   s nextPutAll: 'State class: '; nextPutAll: state class name; cr.
   s nextPutAll: 'State size: '; nextPutAll: state size printString; cr.
   val := state at: 1.
   s nextPutAll: 'State value class: '; nextPutAll: val class name; cr.
   s nextPutAll: 'State value: '; nextPutAll: val printString; cr] on: Error do: [:e |
   s nextPutAll: 'Random new ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr.
   ^ self].
  s flush.

  "Test PCG step by step"
  [| mult sum masked count shifted xored shifted2 anded rotA rotB rotResult |
   mult := val * 6364136223846793005.
   s nextPutAll: 'mult class: '; nextPutAll: mult class name; cr.
   s nextPutAll: 'mult = '; nextPutAll: mult printString; cr.
   sum := mult + 1442695040888963407.
   s nextPutAll: 'sum class: '; nextPutAll: sum class name; cr.
   masked := sum bitAnd: 16rFFFFFFFFFFFFFFFF.
   s nextPutAll: 'masked class: '; nextPutAll: masked class name; cr.
   s nextPutAll: 'masked = '; nextPutAll: masked printString; cr.
   state at: 1 put: masked.
   s nextPutAll: 'stored back OK'; cr.
   count := val >> 59.
   s nextPutAll: 'count = '; nextPutAll: count printString; cr.
   shifted := val >> 18.
   xored := shifted bitXor: val.
   shifted2 := xored >> 27.
   anded := shifted2 bitAnd: 16rFFFFFFFF.
   s nextPutAll: 'anded = '; nextPutAll: anded printString; cr.
   rotA := anded >> count.
   rotB := anded << (count negated bitAnd: 31).
   rotResult := (rotA bitOr: rotB) bitAnd: 16r7FFFFFFF.
   s nextPutAll: 'rotResult = '; nextPutAll: rotResult printString; cr.
   s nextPutAll: 'PCG step OK'; cr] on: Error do: [:e |
   s nextPutAll: 'PCG ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr.
   e signalerContext ifNotNil: [:ctx |
     | c2 cnt |
     c2 := ctx. cnt := 0.
     [c2 notNil and: [cnt < 15]] whileTrue: [
       s nextPutAll: '  '; nextPutAll: c2 printString; cr.
       c2 := c2 sender. cnt := cnt + 1]]].
  s flush.

  "Test Random>>next"
  [| result |
   result := r next.
   s nextPutAll: 'r next class: '; nextPutAll: result class name; cr.
   s nextPutAll: 'r next = '; nextPutAll: result printString; cr.
   s nextPutAll: 'Random next OK'; cr] on: Error do: [:e |
   s nextPutAll: 'Random next ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr.
   e signalerContext ifNotNil: [:ctx |
     | c2 cnt |
     c2 := ctx. cnt := 0.
     [c2 notNil and: [cnt < 15]] whileTrue: [
       s nextPutAll: '  '; nextPutAll: c2 printString; cr.
       c2 := c2 sender. cnt := cnt + 1]]].
  s flush.

  "Test division"
  [| divResult |
   divResult := 42 / 16r7FFFFFFF.
   s nextPutAll: '42/max class: '; nextPutAll: divResult class name; cr.
   s nextPutAll: '42/max = '; nextPutAll: divResult printString; cr.
   s nextPutAll: 'Division OK'; cr] on: Error do: [:e |
   s nextPutAll: 'Division ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr].
  s flush.

  s nextPutAll: '=== Done ==='; cr.
  s flush! !

!RandomDiag class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

!RandomDiag class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #RandomDiag)
  inCategory: SessionManager default userCategory
  atPriority: 90!
