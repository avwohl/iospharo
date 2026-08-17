"Colour fidelity across every BitBlt depth conversion.

 Covers all 36 ordered pairs of the six pixel depths (1, 2, 4, 8, 16, 32). The
 hard part of such a test is deciding what 'correct' means when the target depth
 cannot represent the source colour. Naming an expected colour for each pair is a
 trap: a depth change also applies a palette map, and a lower target legitimately
 loses precision, so any table of expectations mostly encodes today's behaviour.

 So the invariant asserted here is position independence, which holds at every
 pair regardless of precision: converting a whole form must give each pixel the
 same value as converting that pixel on its own. Whatever a depth change does to
 a colour, it must not depend on where in the row the pixel sits. That is exactly
 the class of bug fixed below — wrong row strides and wrong within-word packing —
 and it needs no model of what the right colour is.

 Comparisons are on pixel VALUES throughout, and both sides go through the same
 conversion machinery. Comparing colours instead would drag in Form>>colorAt: and
 Form>>fillColor:, which disagree with BitBlt by design: colorAt: scales a 5-bit
 component by /31 while a depth change zero-fills it to n<<3, so white reads back
 as 1.0 one way and 248/255 the other. Both are correct in their own terms, and a
 test that mixes them fails on a perfectly good VM.

 Written 2026-08-16, after five separate defects in these paths were fixed in one
 sitting: 5-bit components widened by scaling instead of zero-fill, 16bpp rows
 strided by pixel width instead of the padded row, shift/mask colour maps ignored
 by the raw-order path, no implicit colour map built for a depth change, and alpha
 stashed in the unused top bit of a 1-5-5-5 pixel. Every one of them was a
 conversion between depths, and none was caught by the existing tests."!

TestCase subclass: #BitBltDepthMatrixTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-BitBlt'!

!BitBltDepthMatrixTest methodsFor: 'helpers'!
samplePixelValuesFor: aDepth
  "A non-repeating spread of pixel values for the depth. Periodic data is useless
   here: alternating values survive a reversal or a byte swap unchanged, so a
   packing fault at 1bpp or 2bpp would read back clean."
  | limit |
  limit := 1 bitShift: (aDepth min: 16).
  ^ (0 to: 32) collect: [ :x | (x * 7 + 3) \\ limit ]! !

!BitBltDepthMatrixTest methodsFor: 'helpers'!
sourceOfDepth: aDepth
  "33 pixels wide by 5 rows. Both dimensions matter. The width is deliberately
   odd, so a row of 16bpp pixels does not land on a whole number of 32-bit words —
   an even width makes the padded row and the pixel count identical and hides a
   stride error completely. And there have to be several rows, because a stride
   error is a per-row drift: row 0 always lands correctly however wrong the stride
   is, so a single-row form cannot show one."
  | values form |
  values := self samplePixelValuesFor: aDepth.
  form := Form extent: values size @ 5 depth: aDepth.
  0 to: 4 do: [ :y |
    1 to: values size do: [ :i |
      form pixelValueAt: (i - 1) @ y put: (((values at: i) + (y * 37)) \\ (1 bitShift: (aDepth min: 16))) ] ].
  ^ form! !

!BitBltDepthMatrixTest methodsFor: 'helpers'!
mismatchesFrom: srcDepth to: dstDepth
  "Convert a whole form, then convert each of its pixels alone, and require the
   two to agree. Answers human-readable descriptions, empty when they agree."
  | src converted bad |
  src := self sourceOfDepth: srcDepth.
  converted := src asFormOfDepth: dstDepth.
  bad := OrderedCollection new.
  0 to: src height - 1 do: [ :y |
    0 to: src width - 1 do: [ :x | | single expected actual |
      single := Form extent: 1 @ 1 depth: srcDepth.
      single pixelValueAt: 0 @ 0 put: (src pixelValueAt: x @ y).
      expected := (single asFormOfDepth: dstDepth) pixelValueAt: 0 @ 0.
      actual := converted pixelValueAt: x @ y.
      actual = expected ifFalse: [
        bad add: srcDepth printString, '->', dstDepth printString,
                 ' at', (x @ y) printString,
                 ' src=', ((src pixelValueAt: x @ y) printStringBase: 16),
                 ' alone=', (expected printStringBase: 16),
                 ' inForm=', (actual printStringBase: 16) ] ] ].
  ^ bad! !

!BitBltDepthMatrixTest methodsFor: 'helpers'!
assertAllTargetsFrom: srcDepth
  | bad |
  bad := OrderedCollection new.
  #(1 2 4 8 16 32) do: [ :dst | bad addAll: (self mismatchesFrom: srcDepth to: dst) ].
  self
    assert: bad isEmpty
    description: (String streamContents: [ :s |
      s nextPutAll: bad size printString, ' pixels convert differently in a row than alone, from depth ',
                    srcDepth printString; cr.
      (bad first: (8 min: bad size)) do: [ :line | s nextPutAll: '  ', line; cr ] ])! !

!BitBltDepthMatrixTest methodsFor: 'tests'!
testFromDepth1
  self assertAllTargetsFrom: 1! !

!BitBltDepthMatrixTest methodsFor: 'tests'!
testFromDepth2
  self assertAllTargetsFrom: 2! !

!BitBltDepthMatrixTest methodsFor: 'tests'!
testFromDepth4
  self assertAllTargetsFrom: 4! !

!BitBltDepthMatrixTest methodsFor: 'tests'!
testFromDepth8
  self assertAllTargetsFrom: 8! !

!BitBltDepthMatrixTest methodsFor: 'tests'!
testFromDepth16
  self assertAllTargetsFrom: 16! !

!BitBltDepthMatrixTest methodsFor: 'tests'!
testFromDepth32
  self assertAllTargetsFrom: 32! !

!BitBltDepthMatrixTest methodsFor: 'tests - regression'!
testTopBitOfSixteenBitPixelStaysClear
  "A 1-5-5-5 pixel has no alpha channel; bit 15 is unused and must stay clear.
   The 32-to-16 conversion used to stash the source's opacity there. Nothing in
   the VM read it back, so it went unnoticed until a PNG roundtrip — which has
   nowhere to store that bit — returned every pixel differing in exactly it."
  | src converted |
  src := Form extent: 8 @ 1 depth: 32.
  0 to: 7 do: [ :x |
    src fill: (x @ 0 extent: 1 @ 1) fillColor: (Color r: 1.0 g: (x / 7) b: 0.5) ].
  converted := src asFormOfDepth: 16.
  0 to: 7 do: [ :x |
    self
      assert: ((converted pixelValueAt: x @ 0) bitAnd: 16r8000) = 0
      description: 'bit 15 set in 16bpp pixel ', x printString ]! !

!BitBltDepthMatrixTest methodsFor: 'tests - regression'!
testFiveBitComponentsWidenByZeroFill
  "Widening 5 bits to 8 zero-fills, so a full component is 248 and not 255.
   BitBltSimulation>>rgbMap16To32: is three shifts, and rgbMap:from:to: states it:
   'Expand to more bits by zero-fill'. The golden PNGs in the image agree — they
   carry an sBIT chunk of 5 5 5 1 declaring the low bits padding."
  | src converted px |
  src := Form extent: 1 @ 1 depth: 16.
  src pixelValueAt: 0 @ 0 put: 16r7C00.  "red = 31, green = 0, blue = 0"
  converted := src asFormOfDepth: 32.
  px := converted pixelValueAt: 0 @ 0.
  self
    assert: ((px >> 16) bitAnd: 16rFF) = 248
    description: 'red widened to ', ((px >> 16) bitAnd: 16rFF) printString,
                 ', expected 248 (31 << 3)'! !

!BitBltDepthMatrixTest methodsFor: 'tests - regression'!
testNegatedDepthConvertsTheSameAsPositiveDepth
  "A negative depth changes only how pixels are PACKED into each 32-bit word,
   not what they are. So a depth d form and a depth -d form holding the same
   pixel values must convert to the same 32bpp form.

   This is the invariant the self-consistency tests in test_bitblt_lowdepth.st
   cannot check. They write a negated-depth form and read it back, which passes
   whenever the read and the write share an error — and they did: the primitive
   latches srcNeedsByteSwap for a negative source and then normalises srcDepth
   positive, so a negated-depth source arriving at the 32bpp handlers was
   packed MSB-first by code that never consults the flag. Every group of pixels
   in a word came out reversed, identically on the way in and on the way out.
   Depth -16 alone escaped, because that one handler compensates alone.

   Comparing the two forms against each other has no such blind spot: the
   positive path is independently exercised by every other test here."
  #( 1 2 4 8 16 32 ) do: [ :d |
    | positive negated limit |
    limit := 1 bitShift: (d min: 16).
    positive := Form extent: 32 @ 1 depth: d.
    negated := Form extent: 32 @ 1 depth: d negated.
    0 to: 31 do: [ :x |
      | v |
      v := (x * 7 + 3) \\ limit.
      positive pixelValueAt: x @ 0 put: v.
      negated pixelValueAt: x @ 0 put: v ].
    0 to: 31 do: [ :x |
      self
        assert: ((negated asFormOfDepth: 32) pixelValueAt: x @ 0)
             = ((positive asFormOfDepth: 32) pixelValueAt: x @ 0)
        description: 'depth ', d negated printString, ' converts differently ',
                     'from depth ', d printString, ' at x=', x printString ] ]! !

!BitBltDepthMatrixTest methodsFor: 'tests - regression'!
testOddWidthRowsDoNotDriftSideways
  "Form rows pad to whole 32-bit words, so at 16bpp a row of odd width owns one
   more pixel slot than it has pixels. Striding row to row by the width instead
   loses a pixel per row and walks the image sideways, a little further on each
   row — which is why this needs several rows and an odd width to show up.

   Checked against the zero-fill widening directly rather than through colours,
   for the reason given in the class comment."
  | src converted |
  src := Form extent: 33 @ 5 depth: 16.
  0 to: 4 do: [ :y |
    0 to: 32 do: [ :x |
      src pixelValueAt: x @ y put: ((x * 7) + (y * 131) + 1 bitAnd: 16r7FFF) ] ].
  converted := src asFormOfDepth: 32.
  0 to: 4 do: [ :y |
    0 to: 32 do: [ :x | | p expected |
      p := src pixelValueAt: x @ y.
      expected := ((p bitAnd: 16r1F) bitShift: 3)
                    bitOr: (((p bitAnd: 16r3E0) bitShift: 6)
                    bitOr: ((p bitAnd: 16r7C00) bitShift: 9)).
      self
        assert: ((converted pixelValueAt: x @ y) bitAnd: 16rFFFFFF) = expected
        description: 'pixel ', (x @ y) printString, ' drifted: expected ',
                     (expected printStringBase: 16), ' got ',
                     (((converted pixelValueAt: x @ y) bitAnd: 16rFFFFFF) printStringBase: 16) ] ]! !
