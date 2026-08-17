"BitBlt tests for depth-8 destinations.

 Depth-8 Form bits are MSB-first within each 32-bit word: column c lives at byte
 (c & ~3) + (3 - (c & 3)), not at byte c. The 8-to-8 handler in Primitives.cpp
 has always said so and done so. The 1-to-8, 2-to-8 and 4-to-8 handlers wrote
 their destination as plain little-endian bytes instead, which reversed every
 group of four pixels — pixel 0 landed where pixel 3 belonged, 1 where 2 did.

 It was easy to miss for two reasons. The primitive answered success, so nothing
 failed; and the copies also apply a legitimate palette map when the depths
 differ, so the wrong bytes did not look like a permutation of the input — they
 looked like unrelated numbers. Comparing pixel *values* invites exactly that
 confusion. Compare colours instead: a depth conversion is correct when the
 colour survives, whatever the index becomes.

 Verified 2026-08-16 to have teeth: testColour4to8IsPreserved and its siblings
 fail on the pre-fix VM and pass after."!

TestCase subclass: #BitBltLowDepthTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-BitBlt'!

!BitBltLowDepthTest methodsFor: 'helpers'!
sourceOfDepth: aDepth width: aWidth
  "A one-row form with a different palette index in every pixel."
  | form limit |
  form := Form extent: aWidth @ 1 depth: aDepth.
  limit := 1 bitShift: aDepth.
  0 to: aWidth - 1 do: [ :x | form pixelValueAt: x @ 0 put: x \\ limit ].
  ^ form! !

!BitBltLowDepthTest methodsFor: 'helpers'!
copy: srcForm onto: dstForm
  (BitBlt
    destForm: dstForm
    sourceForm: srcForm
    halftoneForm: nil
    combinationRule: 3
    destOrigin: 0 @ 0
    sourceOrigin: 0 @ 0
    extent: srcForm extent
    clipRect: (0 @ 0 extent: dstForm extent)) copyBits! !

!BitBltLowDepthTest methodsFor: 'helpers'!
assertColoursPreservedFrom: aDepth
  "Copy a depth-aDepth row onto a depth-8 form and require every pixel to keep
   its colour. Eight pixels is the minimum that spans two 32-bit words, which is
   what catches a per-word reversal — four would only catch it within one word."
  | src dst |
  src := self sourceOfDepth: aDepth width: 8.
  dst := Form extent: 8 @ 1 depth: 8.
  self copy: src onto: dst.
  0 to: 7 do: [ :x |
    self
      assert: (dst colorAt: x @ 0) = (src colorAt: x @ 0)
      description: 'pixel ', x printString, ' changed colour copying ',
                   aDepth printString, 'bpp to 8bpp' ]! !

!BitBltLowDepthTest methodsFor: 'tests - colour survives the depth change'!
testColour1to8IsPreserved
  self assertColoursPreservedFrom: 1! !

!BitBltLowDepthTest methodsFor: 'tests - colour survives the depth change'!
testColour2to8IsPreserved
  self assertColoursPreservedFrom: 2! !

!BitBltLowDepthTest methodsFor: 'tests - colour survives the depth change'!
testColour4to8IsPreserved
  self assertColoursPreservedFrom: 4! !

!BitBltLowDepthTest methodsFor: 'tests - colour survives the depth change'!
testColour8to8IsPreserved
  "The handler that was already right. Guards against a fix to the others
   breaking this one."
  self assertColoursPreservedFrom: 8! !

!BitBltLowDepthTest methodsFor: 'tests - pixel order'!
testPixelOrderIsNotReversedWithinWords
  "State the failure directly. Under the byte-order bug the first four pixels
   come back in the order 3, 2, 1, 0 and the next four as 7, 6, 5, 4, so a
   form whose colours are all distinct comes back visibly shuffled."
  | src dst forward reversed |
  src := self sourceOfDepth: 4 width: 8.
  dst := Form extent: 8 @ 1 depth: 8.
  self copy: src onto: dst.
  forward := (0 to: 7) collect: [ :x | src colorAt: x @ 0 ].
  reversed := (0 to: 7) collect: [ :x | dst colorAt: ((x // 4 * 4) + (3 - (x \\ 4))) @ 0 ].
  self
    deny: forward = reversed
    description: 'destination is the source reversed within each 32-bit word'! !

!BitBltLowDepthTest methodsFor: 'tests - layout invariant'!
testDepth8BitsAreMsbFirstWithinWords
  "The invariant the handlers must agree on. Pixels 1..8 written through
   pixelValueAt:put: must appear in the raw words as 16r01020304 16r05060708 —
   the first pixel in the *high* byte."
  | form |
  form := Form extent: 8 @ 1 depth: 8.
  1 to: 8 do: [ :i | form pixelValueAt: (i - 1) @ 0 put: i ].
  self assert: (form bits at: 1) equals: 16r01020304.
  self assert: (form bits at: 2) equals: 16r05060708! !

!BitBltLowDepthTest methodsFor: 'helpers'!
patternFor: aDepth at: x
  "A non-repeating pattern. A periodic one hides packing faults: alternating
   values survive a byte swap unchanged, so 1bpp and 2bpp appear to pass."
  ^ (x * 7 + 3) \\ (1 bitShift: (aDepth min: 8))! !

!BitBltLowDepthTest methodsFor: 'tests - negative depths'!
testNegatedDepthFormsReadBackWhatTheyWrote
  "A negative depth means non-MSB pixel order. Writing and reading a negated
   depth form must be self-consistent at every depth. This failed at 1, 2, 4,
   16 and 32 while succeeding at 8, because the primitive normalises a negative
   source depth to positive early (keeping the sign in a separate flag), so a
   negated-depth *source* was being handled by the plain positive path."
  #(1 2 4 8 16 32) do: [ :d | | form |
    form := Form extent: 32 @ 1 depth: d negated.
    0 to: 31 do: [ :x | form pixelValueAt: x @ 0 put: (self patternFor: d at: x) ].
    0 to: 31 do: [ :x |
      self
        assert: (form pixelValueAt: x @ 0) = (self patternFor: d at: x)
        description: 'depth -', d printString, ' pixel ', x printString ] ]! !

!BitBltLowDepthTest methodsFor: 'tests - negative depths'!
testConversionToNegatedDepthKeepsPixelValues
  "Copying a form onto one of negated depth must preserve every pixel."
  #(1 2 4 8 16 32) do: [ :d | | src rev |
    src := Form extent: 32 @ 1 depth: d.
    0 to: 31 do: [ :x | src pixelValueAt: x @ 0 put: (self patternFor: d at: x) ].
    rev := Form extent: 32 @ 1 depth: d negated.
    src displayOn: rev.
    0 to: 31 do: [ :x |
      self
        assert: (rev pixelValueAt: x @ 0) = (src pixelValueAt: x @ 0)
        description: 'depth ', d printString, ' to negated, pixel ', x printString ] ]! !

!BitBltLowDepthTest methodsFor: 'tests - negative depths'!
testConversionToNegatedDepthChangesBitsExceptAt32
  "The invariant PNGReadWriterTest states outright:
     original depth = 32
       ifTrue:  [ self assert: original bits equals: reversed bits ]
       ifFalse: [ self deny:   original bits equals: reversed bits ]
   At 32bpp the pixel is the whole word, so MSB order says nothing about it and
   the copy must leave the bits alone. Below 32 the packing genuinely differs."
  #(1 2 4 8 16 32) do: [ :d | | src rev |
    src := Form extent: 32 @ 1 depth: d.
    0 to: 31 do: [ :x | src pixelValueAt: x @ 0 put: (self patternFor: d at: x) ].
    rev := Form extent: 32 @ 1 depth: d negated.
    src displayOn: rev.
    d = 32
      ifTrue: [ self assert: src bits = rev bits
                  description: '32bpp copy to negated depth must not alter bits' ]
      ifFalse: [ self deny: src bits = rev bits
                  description: 'depth ', d printString, ' negated must repack' ] ]! !
