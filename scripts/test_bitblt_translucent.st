"BitBlt translucent tests -- exercises rules 30/31 (alphaBlendConst/alphaPaintConst)
 and the copyBitsTranslucent: entry point (primitive 96 with argCount=1).
 These test the specific bugs fixed in Build 77:
  - primitiveCopyBits rejected argCount=1 (copyBitsTranslucent:)
  - Rules 30/31 were incorrectly routed to counting code path
  - Rules 30/31 were missing from all depth combination switches"!

TestCase subclass: #BitBltTranslucentTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-BitBlt'!

!BitBltTranslucentTest methodsFor: 'helpers'!
sourceFill: color extent: extent
  "Create a 32-bit Form filled with a single color."
  | form |
  form := Form extent: extent depth: 32.
  form fillColor: color.
  ^ form! !

!BitBltTranslucentTest methodsFor: 'helpers'!
bitBltFrom: srcForm to: dstForm rule: rule
  "Create a BitBlt configured for full-form copy."
  ^ (BitBlt
    destForm: dstForm
    sourceForm: srcForm
    halftoneForm: nil
    combinationRule: rule
    destOrigin: 0@0
    sourceOrigin: 0@0
    extent: srcForm extent
    clipRect: (0@0 extent: dstForm extent))! !

!BitBltTranslucentTest methodsFor: 'helpers'!
pixelAt: aPoint in: aForm
  "Read a single 32-bit pixel value."
  ^ aForm pixelValueAt: aPoint! !

!BitBltTranslucentTest methodsFor: 'tests - rule 30'!
testAlphaBlendConstFullOpacity
  "Rule 30 with alpha=255 should replace destination entirely."
  | src dst bb |
  src := self sourceFill: (Color r: 1.0 g: 0.0 b: 0.0) extent: 4@4.
  dst := self sourceFill: (Color r: 0.0 g: 0.0 b: 1.0) extent: 4@4.
  bb := self bitBltFrom: src to: dst rule: 30.
  bb copyBitsTranslucent: 255.
  "Dest should now be red (source replaces dest at full opacity)"
  self assert: (self pixelAt: 1@1 in: dst) equals: (self pixelAt: 1@1 in: src)! !

!BitBltTranslucentTest methodsFor: 'tests - rule 30'!
testAlphaBlendConstZeroOpacity
  "Rule 30 with alpha=0 should leave destination unchanged."
  | src dst bb origPixel |
  src := self sourceFill: (Color r: 1.0 g: 0.0 b: 0.0) extent: 4@4.
  dst := self sourceFill: (Color r: 0.0 g: 0.0 b: 1.0) extent: 4@4.
  origPixel := self pixelAt: 1@1 in: dst.
  bb := self bitBltFrom: src to: dst rule: 30.
  bb copyBitsTranslucent: 0.
  "Dest should be unchanged"
  self assert: (self pixelAt: 1@1 in: dst) equals: origPixel! !

!BitBltTranslucentTest methodsFor: 'tests - rule 30'!
testAlphaBlendConstHalfOpacity
  "Rule 30 with alpha=128 should blend source and dest."
  | src dst bb resultColor |
  src := self sourceFill: (Color r: 1.0 g: 0.0 b: 0.0) extent: 4@4.
  dst := self sourceFill: (Color r: 0.0 g: 0.0 b: 1.0) extent: 4@4.
  bb := self bitBltFrom: src to: dst rule: 30.
  bb copyBitsTranslucent: 128.
  "Result should be roughly purple (red + blue blended)"
  resultColor := dst colorAt: 1@1.
  self assert: resultColor red > 0.3.
  self assert: resultColor blue > 0.3.
  "Neither pure red nor pure blue"
  self deny: resultColor red > 0.9.
  self deny: resultColor blue > 0.9! !

!BitBltTranslucentTest methodsFor: 'tests - rule 30'!
testAlphaBlendConstDoesNotRaise
  "copyBitsTranslucent: with rule 30 must not raise an error.
   This was the original bug: primitive 96 rejected argCount=1."
  | src dst bb |
  src := self sourceFill: Color red extent: 10@10.
  dst := self sourceFill: Color blue extent: 10@10.
  bb := self bitBltFrom: src to: dst rule: 30.
  self shouldnt: [bb copyBitsTranslucent: 200] raise: Error! !

!BitBltTranslucentTest methodsFor: 'tests - rule 31'!
testAlphaPaintConstFullOpacity
  "Rule 31 with alpha=255 should replace dest where source is non-zero."
  | src dst bb |
  src := self sourceFill: (Color r: 0.0 g: 1.0 b: 0.0) extent: 4@4.
  dst := self sourceFill: (Color r: 0.0 g: 0.0 b: 1.0) extent: 4@4.
  bb := self bitBltFrom: src to: dst rule: 31.
  bb copyBitsTranslucent: 255.
  "Dest should be green"
  self assert: (self pixelAt: 1@1 in: dst) equals: (self pixelAt: 1@1 in: src)! !

!BitBltTranslucentTest methodsFor: 'tests - rule 31'!
testAlphaPaintConstSkipsZeroSource
  "Rule 31 with zero source pixels should leave dest unchanged.
   This is the key difference from rule 30 (paint vs blend)."
  | src dst bb origPixel |
  src := self sourceFill: Color transparent extent: 4@4.
  dst := self sourceFill: (Color r: 0.0 g: 0.0 b: 1.0) extent: 4@4.
  origPixel := self pixelAt: 1@1 in: dst.
  bb := self bitBltFrom: src to: dst rule: 31.
  bb copyBitsTranslucent: 255.
  "Dest should be unchanged -- zero source means skip"
  self assert: (self pixelAt: 1@1 in: dst) equals: origPixel! !

!BitBltTranslucentTest methodsFor: 'tests - rule 31'!
testAlphaPaintConstHalfOpacity
  "Rule 31 with alpha=128 should blend non-zero source pixels."
  | src dst bb resultColor |
  src := self sourceFill: (Color r: 0.0 g: 1.0 b: 0.0) extent: 4@4.
  dst := self sourceFill: (Color r: 1.0 g: 0.0 b: 0.0) extent: 4@4.
  bb := self bitBltFrom: src to: dst rule: 31.
  bb copyBitsTranslucent: 128.
  resultColor := dst colorAt: 1@1.
  "Should be yellowish (red + green blended)"
  self assert: resultColor red > 0.3.
  self assert: resultColor green > 0.3! !

!BitBltTranslucentTest methodsFor: 'tests - rule 31'!
testAlphaPaintConstDoesNotRaise
  "copyBitsTranslucent: with rule 31 must not raise an error."
  | src dst bb |
  src := self sourceFill: Color green extent: 10@10.
  dst := self sourceFill: Color blue extent: 10@10.
  bb := self bitBltFrom: src to: dst rule: 31.
  self shouldnt: [bb copyBitsTranslucent: 200] raise: Error! !

!BitBltTranslucentTest methodsFor: 'tests - fill'!
testAlphaBlendConstFill
  "Rule 30 fill (no source) with halftone word array should blend."
  | dst bb ht |
  dst := self sourceFill: Color blue extent: 4@4.
  ht := Bitmap new: 1.
  ht at: 1 put: 16rFFFF0000.
  bb := BitBlt
    destForm: dst
    sourceForm: nil
    halftoneForm: ht
    combinationRule: 30
    destOrigin: 0@0
    sourceOrigin: 0@0
    extent: 4@4
    clipRect: (0@0 extent: 4@4).
  self shouldnt: [bb copyBitsTranslucent: 128] raise: Error! !

!BitBltTranslucentTest methodsFor: 'tests - depth combos'!
testRule30With1BitSource
  "Rule 30 with 1-bit source to 32-bit dest."
  | src dst bb |
  src := Form extent: 4@4 depth: 1.
  src fillBlack.
  dst := self sourceFill: Color white extent: 4@4.
  bb := self bitBltFrom: src to: dst rule: 30.
  self shouldnt: [bb copyBitsTranslucent: 128] raise: Error! !

!BitBltTranslucentTest methodsFor: 'tests - depth combos'!
testRule31With1BitSource
  "Rule 31 with 1-bit source to 32-bit dest."
  | src dst bb |
  src := Form extent: 4@4 depth: 1.
  src fillBlack.
  dst := self sourceFill: Color white extent: 4@4.
  bb := self bitBltFrom: src to: dst rule: 31.
  self shouldnt: [bb copyBitsTranslucent: 128] raise: Error! !

!BitBltTranslucentTest methodsFor: 'tests - depth combos'!
testRule30With8BitSource
  "Rule 30 with 8-bit source to 32-bit dest."
  | src dst bb |
  src := Form extent: 4@4 depth: 8.
  src bits atAll: (1 to: src bits size) put: 16r80808080.
  dst := self sourceFill: Color white extent: 4@4.
  bb := self bitBltFrom: src to: dst rule: 30.
  self shouldnt: [bb copyBitsTranslucent: 128] raise: Error! !

!BitBltTranslucentTest methodsFor: 'tests - basic'!
testCopyBitsStillWorks
  "Plain copyBits (no alpha arg) must still work after our changes."
  | src dst bb |
  src := self sourceFill: Color red extent: 4@4.
  dst := self sourceFill: Color blue extent: 4@4.
  bb := self bitBltFrom: src to: dst rule: 3.
  self shouldnt: [bb copyBits] raise: Error.
  "Dest should now be red (rule 3 = store)"
  self assert: (self pixelAt: 1@1 in: dst) equals: (self pixelAt: 1@1 in: src)! !

!BitBltTranslucentTest methodsFor: 'tests - basic'!
testCopyBitsRule34AlphaBlendScaled
  "Rule 34 (alphaBlendScaled) via plain copyBits must still work."
  | src dst bb |
  src := self sourceFill: (Color red alpha: 0.5) extent: 4@4.
  dst := self sourceFill: Color blue extent: 4@4.
  bb := self bitBltFrom: src to: dst rule: 34.
  self shouldnt: [bb copyBits] raise: Error! !

!BitBltTranslucentTest methodsFor: 'tests - edge cases'!
testTranslucentWithClipping
  "copyBitsTranslucent: should handle clipping correctly."
  | src dst bb |
  src := self sourceFill: Color red extent: 10@10.
  dst := self sourceFill: Color blue extent: 4@4.
  bb := BitBlt
    destForm: dst
    sourceForm: src
    halftoneForm: nil
    combinationRule: 30
    destOrigin: -2 @ -2
    sourceOrigin: 0@0
    extent: 10@10
    clipRect: (0@0 extent: 4@4).
  self shouldnt: [bb copyBitsTranslucent: 200] raise: Error! !

!BitBltTranslucentTest methodsFor: 'tests - edge cases'!
testTranslucentZeroExtent
  "copyBitsTranslucent: with zero extent should succeed (no-op)."
  | src dst bb |
  src := self sourceFill: Color red extent: 4@4.
  dst := self sourceFill: Color blue extent: 4@4.
  bb := BitBlt
    destForm: dst
    sourceForm: src
    halftoneForm: nil
    combinationRule: 30
    destOrigin: 0@0
    sourceOrigin: 0@0
    extent: 0@0
    clipRect: (0@0 extent: 4@4).
  self shouldnt: [bb copyBitsTranslucent: 128] raise: Error! !
