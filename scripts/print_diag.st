"LargeInteger printing diagnostic"!

Object subclass: #PrintDiag
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

UndefinedObject compile: 'findNextHandlerContext
  ^ nil' classified: 'private'!

!PrintDiag class methodsFor: 'running'!
run
  | stream n q r |
  stream := '/tmp/print_diag.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== LargeInteger Printing ==='; cr.

    "Hex printing tests"
    stream nextPutAll: '--- Hex (base 16) ---'; cr.
    {16rFF. 16r100. 16rFFFF. 16r10000. 16rFFFFFFFF. 16rDEADBEEF.
     16rFEDCBA9876543210} do: [:each |
      | hex |
      hex := each printStringBase: 16.
      stream nextPutAll: each printString; nextPutAll: ' -> '; nextPutAll: hex; cr].
    stream flush.

    "Binary printing tests"
    stream cr; nextPutAll: '--- Binary (base 2) ---'; cr.
    {255. 256. 1023. 1024. (2 raisedTo: 89) - 1} do: [:each |
      | bin |
      bin := each printStringBase: 2.
      stream nextPutAll: each printString; nextPutAll: ' -> ';
        nextPutAll: bin size printString; nextPutAll: ' digits: ';
        nextPutAll: (bin copyFrom: 1 to: (bin size min: 20)).
      bin size > 20 ifTrue: [stream nextPutAll: '...'].
      stream cr].
    stream flush.

    "numberOfDigitsInBase tests"
    stream cr; nextPutAll: '--- numberOfDigitsInBase ---'; cr.
    {255. 256. 16rFFFFFFFF. 16rFEDCBA9876543210. (2 raisedTo: 89) - 1} do: [:each |
      stream nextPutAll: each printString;
        nextPutAll: ' base2='; nextPutAll: (each numberOfDigitsInBase: 2) printString;
        nextPutAll: ' base10='; nextPutAll: (each numberOfDigitsInBase: 10) printString;
        nextPutAll: ' base16='; nextPutAll: (each numberOfDigitsInBase: 16) printString; cr].
    stream flush.

    "Direct division test - what does 16rFEDCBA9876543210 // 16 give?"
    stream cr; nextPutAll: '--- Division by base ---'; cr.
    n := 16rFEDCBA9876543210.
    1 to: 5 do: [:i |
      q := n // 16.
      r := n \\ 16.
      stream nextPutAll: 'step '; nextPutAll: i printString;
        nextPutAll: ': n='; nextPutAll: n printString;
        nextPutAll: ' n//16='; nextPutAll: q printString;
        nextPutAll: ' n\\16='; nextPutAll: r printString;
        nextPutAll: ' digit='; nextPutAll: ('0123456789ABCDEF' at: r + 1) printString; cr.
      n := q].
    stream flush]
  ensure: [stream close].
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!PrintDiag class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #PrintDiag)
  inCategory: SessionManager default userCategory
  atPriority: 90!
