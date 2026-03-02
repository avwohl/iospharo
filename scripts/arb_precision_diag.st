"Diagnostic script for PMArbitraryPrecisionFloat test failures.
 Compares specific operations against expected results to identify
 which primitive/operation diverges from the reference VM.
 Results: /tmp/arb_precision_diag.txt
 Usage: pharo Pharo.image eval --save thisFile asFileReference fileIn
        Then run with our VM: ./build/test_load_image Pharo.image"!

Object subclass: #ArbPrecisionDiag
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

!ArbPrecisionDiag class methodsFor: 'running'!
runDiagnostic
| stream |
stream := '/tmp/arb_precision_diag.txt' asFileReference writeStream.
[
stream nextPutAll: '=== ArbitraryPrecisionFloat Diagnostic ==='; cr.
stream nextPutAll: 'Date: '; nextPutAll: DateAndTime now printString; cr; cr.

"=== Section 1: Run each failing test and capture error details ==="
stream nextPutAll: '=== Section 1: Individual test results ==='; cr.
(Smalltalk globals at: #PMArbitraryPrecisionFloatTest ifAbsent: [nil])
  ifNil: [stream nextPutAll: 'ERROR: PMArbitraryPrecisionFloatTest not found'; cr]
  ifNotNil: [:cls |
    #(testIEEEArithmeticVersusIntegerAndFraction testLn testExp testArcSin
      testArTanh testVeryLargeSin testVeryLargeCos testVeryLargeTan
      testSinh testCosh testTanh testPrintAndEvaluate) do: [:sel |
      stream nextPutAll: '--- '; nextPutAll: sel; nextPutAll: ' ---'; cr.
      stream flush.
      [| tc |
       tc := cls selector: sel.
       [tc setUp. tc perform: sel.
        stream nextPutAll: '  PASS'; cr]
         on: TestFailure do: [:ex |
           stream nextPutAll: '  FAIL: '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 500)); cr.
           [| ctx count |
            ctx := ex signalerContext.
            count := 0.
            [ctx notNil and: [count < 20]] whileTrue: [
              stream nextPutAll: '    >> '; nextPutAll: ctx printString; cr.
              ctx := ctx sender. count := count + 1]] on: Error do: [:e2 |]]]
         on: Error do: [:ex |
           stream nextPutAll: '  ERROR: '; nextPutAll: ex class name;
             nextPutAll: ': '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 500)); cr.
           [| ctx count |
            ctx := ex signalerContext.
            count := 0.
            [ctx notNil and: [count < 20]] whileTrue: [
              stream nextPutAll: '    >> '; nextPutAll: ctx printString; cr.
              ctx := ctx sender. count := count + 1]] on: Error do: [:e2 |]]]
      on: Error do: [:ex |
        stream nextPutAll: '  OUTER ERROR: '; nextPutAll: ex class name;
          nextPutAll: ': '; nextPutAll: (ex messageText copyFrom: 1 to: (ex messageText size min: 200)); cr].
      stream flush]].

stream cr.

"=== Section 2: Basic ArbitraryPrecisionFloat operations ==="
stream nextPutAll: '=== Section 2: Basic ArbitraryPrecisionFloat operations ==='; cr.
(Smalltalk globals at: #PMArbitraryPrecisionFloat ifAbsent: [nil])
  ifNil: [stream nextPutAll: 'ERROR: PMArbitraryPrecisionFloat not found'; cr]
  ifNotNil: [:apfClass |
    "Test basic creation and conversion"
    [| x |
     x := apfClass new: 53.
     x mantissa: 1 exponent: 0 nBits: 53.
     stream nextPutAll: 'APF(1): class='; nextPutAll: x class name;
       nextPutAll: ' asFloat='; nextPutAll: x asFloat printString; cr]
    on: Error do: [:e |
      stream nextPutAll: 'APF(1) ERROR: '; nextPutAll: e class name;
        nextPutAll: ': '; nextPutAll: e messageText; cr].

    "Test pi computation"
    [| pi |
     pi := apfClass pi: 53.
     stream nextPutAll: 'APF pi(53): '; nextPutAll: pi asFloat printString; cr.
     stream nextPutAll: '  Float pi:  '; nextPutAll: Float pi printString; cr.
     stream nextPutAll: '  diff:      '; nextPutAll: (pi asFloat - Float pi) printString; cr]
    on: Error do: [:e |
      stream nextPutAll: 'APF pi ERROR: '; nextPutAll: e class name;
        nextPutAll: ': '; nextPutAll: e messageText; cr.
      [| ctx count |
       ctx := e signalerContext. count := 0.
       [ctx notNil and: [count < 15]] whileTrue: [
         stream nextPutAll: '    >> '; nextPutAll: ctx printString; cr.
         ctx := ctx sender. count := count + 1]] on: Error do: [:e2 |]].

    "Test ln(2)"
    [| x result |
     x := 2 asArbitraryPrecisionFloatNumBits: 53.
     result := x ln.
     stream nextPutAll: 'APF ln(2): '; nextPutAll: result asFloat printString; cr.
     stream nextPutAll: '  Float ln2: '; nextPutAll: 2.0 ln printString; cr.
     stream nextPutAll: '  diff:      '; nextPutAll: (result asFloat - 2.0 ln) printString; cr]
    on: Error do: [:e |
      stream nextPutAll: 'APF ln(2) ERROR: '; nextPutAll: e class name;
        nextPutAll: ': '; nextPutAll: e messageText; cr.
      [| ctx count |
       ctx := e signalerContext. count := 0.
       [ctx notNil and: [count < 15]] whileTrue: [
         stream nextPutAll: '    >> '; nextPutAll: ctx printString; cr.
         ctx := ctx sender. count := count + 1]] on: Error do: [:e2 |]].

    "Test exp(1)"
    [| x result |
     x := 1 asArbitraryPrecisionFloatNumBits: 53.
     result := x exp.
     stream nextPutAll: 'APF exp(1): '; nextPutAll: result asFloat printString; cr.
     stream nextPutAll: '  Float e:   '; nextPutAll: 1.0 exp printString; cr.
     stream nextPutAll: '  diff:      '; nextPutAll: (result asFloat - 1.0 exp) printString; cr]
    on: Error do: [:e |
      stream nextPutAll: 'APF exp(1) ERROR: '; nextPutAll: e class name;
        nextPutAll: ': '; nextPutAll: e messageText; cr.
      [| ctx count |
       ctx := e signalerContext. count := 0.
       [ctx notNil and: [count < 15]] whileTrue: [
         stream nextPutAll: '    >> '; nextPutAll: ctx printString; cr.
         ctx := ctx sender. count := count + 1]] on: Error do: [:e2 |]]].

stream cr.

"=== Section 3: LargeInteger asFloat precision test ==="
stream nextPutAll: '=== Section 3: LargeInteger asFloat precision ==='; cr.
{
  (1 << 53) + 1.
  (1 << 60) + (1 << 7) + 1.
  (1 << 100) + (1 << 47).
  (1 << 53) + 1 negated.
  1 << 53.
  (1 << 53) - 1
} do: [:n |
  [| f |
   f := n asFloat.
   stream nextPutAll: '  '; nextPutAll: n printString;
     nextPutAll: ' => '; nextPutAll: f printString; cr]
  on: Error do: [:e |
    stream nextPutAll: '  '; nextPutAll: n printString;
      nextPutAll: ' ERROR: '; nextPutAll: e class name;
      nextPutAll: ': '; nextPutAll: e messageText; cr]].

stream cr.

"=== Section 4: Fraction arithmetic stress ==="
stream nextPutAll: '=== Section 4: Fraction arithmetic ==='; cr.
{
  3/7. 22/7. 355/113. 1/3. (1 << 100) / ((1 << 100) - 1).
  1/1000000. 999999/1000000
} do: [:frac |
  [| result ok |
   result := frac * frac denominator / frac denominator.
   ok := result = frac.
   stream nextPutAll: '  '; nextPutAll: frac printString;
     nextPutAll: ' roundtrip: '; nextPutAll: (ok ifTrue: ['OK'] ifFalse: ['MISMATCH: ' , result printString]); cr]
  on: Error do: [:e |
    stream nextPutAll: '  '; nextPutAll: frac printString;
      nextPutAll: ' ERROR: '; nextPutAll: e class name;
      nextPutAll: ': '; nextPutAll: e messageText; cr]].

[| a b product quotient |
 a := (1 << 200) + 37.
 b := (1 << 150) - 19.
 product := a * b.
 quotient := product / b.
 stream nextPutAll: '  Large multiply/divide roundtrip: ';
   nextPutAll: (quotient = a ifTrue: ['OK'] ifFalse: ['MISMATCH']); cr]
on: Error do: [:e |
  stream nextPutAll: '  Large multiply/divide ERROR: '; nextPutAll: e class name;
    nextPutAll: ': '; nextPutAll: e messageText; cr].

stream cr.

"=== Section 5: anyBitFromTo: and bitShift: ==="
stream nextPutAll: '=== Section 5: Bit operations ==='; cr.
[| n |
 n := (1 << 200) + (1 << 100) + (1 << 50) + 1.
 stream nextPutAll: '  n = (1<<200)+(1<<100)+(1<<50)+1'; cr.
 stream nextPutAll: '  anyBitFrom:1 to:1 = '; nextPutAll: (n anyBitOfMagnitudeFrom: 1 to: 1) printString; cr.
 stream nextPutAll: '  anyBitFrom:2 to:49 = '; nextPutAll: (n anyBitOfMagnitudeFrom: 2 to: 49) printString; cr.
 stream nextPutAll: '  anyBitFrom:51 to:51 = '; nextPutAll: (n anyBitOfMagnitudeFrom: 51 to: 51) printString; cr.
 stream nextPutAll: '  anyBitFrom:52 to:99 = '; nextPutAll: (n anyBitOfMagnitudeFrom: 52 to: 99) printString; cr.
 stream nextPutAll: '  anyBitFrom:101 to:101 = '; nextPutAll: (n anyBitOfMagnitudeFrom: 101 to: 101) printString; cr.
 stream nextPutAll: '  bitShift:-50 = '; nextPutAll: (n bitShift: -50) printString; cr.
 stream nextPutAll: '  expected:    '; nextPutAll: ((1 << 150) + (1 << 50) + 1) printString; cr.
 stream nextPutAll: '  match: '; nextPutAll: ((n bitShift: -50) = ((1 << 150) + (1 << 50) + 1)) printString; cr]
on: Error do: [:e |
  stream nextPutAll: '  Bit ops ERROR: '; nextPutAll: e class name;
    nextPutAll: ': '; nextPutAll: e messageText; cr].

stream cr.

"=== Section 6: highBit ==="
stream nextPutAll: '=== Section 6: highBit ==='; cr.
{ 1. 2. 255. 256. (1<<52). (1<<53). (1<<53)+1. (1<<100). (1<<200) } do: [:n |
  [stream nextPutAll: '  '; nextPutAll: n printString;
     nextPutAll: ' highBit='; nextPutAll: n highBit printString; cr]
  on: Error do: [:e |
    stream nextPutAll: '  '; nextPutAll: n printString;
      nextPutAll: ' highBit ERROR: '; nextPutAll: e class name;
      nextPutAll: ': '; nextPutAll: e messageText; cr]].

stream cr.

"=== Section 7: quo: and rem: ==="
stream nextPutAll: '=== Section 7: quo: and rem: ==='; cr.
[| a b q r |
 a := (1 << 200) + 12345.
 b := (1 << 100) + 67.
 q := a quo: b.
 r := a rem: b.
 stream nextPutAll: '  (1<<200)+12345 quo: (1<<100)+67'; cr.
 stream nextPutAll: '  q*b+r = a? '; nextPutAll: ((q * b + r) = a) printString; cr.
 stream nextPutAll: '  r < b? '; nextPutAll: (r < b) printString; cr.
 stream nextPutAll: '  r >= 0? '; nextPutAll: (r >= 0) printString; cr]
on: Error do: [:e |
  stream nextPutAll: '  quo:/rem: ERROR: '; nextPutAll: e class name;
    nextPutAll: ': '; nextPutAll: e messageText; cr].

stream cr.
stream nextPutAll: '=== DIAGNOSTIC COMPLETE ==='; cr.
stream flush]
ensure: [stream close].

Smalltalk exitSuccess! !

!ArbPrecisionDiag class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self runDiagnostic! !

!ArbPrecisionDiag class methodsFor: 'startup'!
shutDown: quitting! !

SessionManager default
  register: (ClassSessionHandler forClassNamed: #ArbPrecisionDiag)
  inCategory: SessionManager default userCategory
  atPriority: 90!
