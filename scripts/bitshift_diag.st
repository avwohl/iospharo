"BitShift diagnostic - test all shifts"!

Object subclass: #BitShiftDiag
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

UndefinedObject compile: 'findNextHandlerContext
  ^ nil' classified: 'private'!

!BitShiftDiag class methodsFor: 'running'!
run
  | stream suite totalFail |
  stream := '/tmp/bitshift_diag.txt' asFileReference writeStream.
  totalFail := 0.
  [
    stream nextPutAll: '=== BitShift Diagnostic (ALL shifts) ==='; cr.

    "Same suite as testLargeShift"
    suite := #('101101011101001100110111110110011101101101000001110110011' '1101101001100010011001101110100000111011011010100011101100' '101101101011110011001100110011011101011001111000100011101000' '10101101101000101001111111111100101101011001011000100011100000' '1000101010101001111011101010111001011111110011110001000110000000' '1100101010101000010011101000110010111110110011110000000000000001').
    suite := suite , (suite collect: [:e | '1' , e reversed]).
    suite := suite , (suite collect: [:e | e , e]).

    suite doWithIndex: [:bits :idx |
      | num localFail |
      num := Integer readFrom: bits readStream base: 2.
      localFail := 0.
      0 to: bits size - 1 do: [:shift |
        | ls rs expectedLs expectedRs |
        ls := (num bitShift: shift) printStringBase: 2.
        rs := (num bitShift: 0 - shift) printStringBase: 2.
        expectedLs := bits , (String new: shift withAll: $0).
        expectedRs := bits copyFrom: 1 to: bits size - shift.
        ls = expectedLs ifFalse: [
          localFail := localFail + 1. totalFail := totalFail + 1.
          localFail <= 3 ifTrue: [
            stream nextPutAll: '  #'; nextPutAll: idx printString;
              nextPutAll: ' FAIL-LS shift='; nextPutAll: shift printString;
              nextPutAll: ' got '; nextPutAll: ls size printString; nextPutAll: ' expected ';
              nextPutAll: expectedLs size printString; cr]].
        rs = expectedRs ifFalse: [
          localFail := localFail + 1. totalFail := totalFail + 1.
          localFail <= 3 ifTrue: [
            stream nextPutAll: '  #'; nextPutAll: idx printString;
              nextPutAll: ' FAIL-RS shift='; nextPutAll: shift printString;
              nextPutAll: ' got '; nextPutAll: rs size printString; nextPutAll: ' expected ';
              nextPutAll: expectedRs size printString; cr]]].
      localFail > 0 ifTrue: [
        stream nextPutAll: 'Suite #'; nextPutAll: idx printString;
          nextPutAll: ' ('; nextPutAll: bits size printString;
          nextPutAll: ' bits): '; nextPutAll: localFail printString; nextPutAll: ' failures'; cr]
      ifFalse: [
        stream nextPutAll: 'Suite #'; nextPutAll: idx printString;
          nextPutAll: ' ('; nextPutAll: bits size printString; nextPutAll: ' bits): OK'; cr].
      stream flush].
    stream nextPutAll: 'Total failures: '; nextPutAll: totalFail printString; cr.
    stream nextPutAll: '=== Done ==='; cr.
    stream flush]
  ensure: [stream close].
  [Smalltalk exitSuccess] on: Error do: [:e |].
  Smalltalk quitPrimitive! !

!BitShiftDiag class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self run! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #BitShiftDiag)
  inCategory: SessionManager default userCategory
  atPriority: 90!
