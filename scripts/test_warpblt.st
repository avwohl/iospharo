"WarpBlt tests -- exercises primitiveWarpBits (primitive 299, named
 BitBltPlugin.primitiveWarpBits) and its Smalltalk fallback.

 Pharo 13.1 ships no WarpBltTest of its own, so nothing in the stock suite
 covers this primitive. Two shipped bugs came out of that gap
 (docs/test-suite-gaps.md):

   Build 62  quad corner scan order wrong -- the left edge interpolated
             p1->p4 (horizontal) instead of p1->p2 (vertical), rotating all
             warped output 90 degrees. testOrientation* pin the orientation
             down by warping a form whose quadrants are four distinct
             colours: a rotation or a mirror moves at least two of them.

   Build 59  the Smalltalk fallback mixPix:sourceMap:destMap: averaged only
             R, G and B, dropping alpha. testSmoothedPreservesAlpha covers
             that path, which is the one taken whenever the primitive fails.

 testColorMapIsHonoured covers the 2026-08-16 change: primitiveWarpBits never
 read the receiver's colorMap and so produced unmapped output. It now fails to
 Smalltalk when a map is set. The test does not care which side does the work
 -- it asserts the mapped result, so it passes on a VM that implements mapped
 warping in the primitive and on one that falls back."!

TestCase subclass: #WarpBltTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-BitBlt'!

!WarpBltTest methodsFor: 'helpers'!
quadrantForm: extent
  "A 32-bit Form split into four solidly-coloured quadrants, so that any
   rotation or reflection of the output is detectable."
  | form half |
  form := Form extent: extent depth: 32.
  half := extent // 2.
  form fillColor: Color red.
  form fill: (half x @ 0 extent: half) fillColor: Color green.
  form fill: (0 @ half y extent: half) fillColor: Color blue.
  form fill: (half extent: half) fillColor: Color yellow.
  ^ form! !

!WarpBltTest methodsFor: 'helpers'!
warp: srcForm into: dstForm cellSize: n
  "Warp the whole source form onto the whole destination form."
  | warp |
  warp := WarpBlt toForm: dstForm.
  warp sourceForm: srcForm.
  warp colorMap: nil.
  warp cellSize: n.
  warp combinationRule: Form over.
  warp
    sourceQuad: (0 @ 0 corner: srcForm extent) innerCorners
    destRect: (0 @ 0 extent: dstForm extent).
  warp warpBits.
  ^ dstForm! !

!WarpBltTest methodsFor: 'helpers'!
colorAt: aPoint in: aForm
  ^ aForm colorAt: aPoint! !

!WarpBltTest methodsFor: 'helpers'!
assertQuadrantsOf: aForm
  "The four quadrants must still hold their original colours, in their
   original places. This is what catches a rotated or mirrored warp."
  | w h |
  w := aForm width. h := aForm height.
  self assert: (self colorAt: (w // 4) @ (h // 4) in: aForm) equals: Color red.
  self assert: (self colorAt: (w * 3 // 4) @ (h // 4) in: aForm) equals: Color green.
  self assert: (self colorAt: (w // 4) @ (h * 3 // 4) in: aForm) equals: Color blue.
  self assert: (self colorAt: (w * 3 // 4) @ (h * 3 // 4) in: aForm) equals: Color yellow! !

!WarpBltTest methodsFor: 'tests - orientation'!
testOrientationSameSize
  "A 1:1 warp must reproduce the source exactly, quadrant for quadrant."
  | src dst |
  src := self quadrantForm: 64 @ 64.
  dst := Form extent: 64 @ 64 depth: 32.
  self warp: src into: dst cellSize: 1.
  self assertQuadrantsOf: dst! !

!WarpBltTest methodsFor: 'tests - orientation'!
testOrientationMagnified
  "Magnifying must not rotate. This is the Build 62 bug: with the edges
   interpolated the wrong way the output came out turned a quarter turn,
   which swaps green and blue."
  | src dst |
  src := self quadrantForm: 32 @ 32.
  dst := Form extent: 128 @ 128 depth: 32.
  self warp: src into: dst cellSize: 1.
  self assertQuadrantsOf: dst! !

!WarpBltTest methodsFor: 'tests - orientation'!
testOrientationNonSquare
  "A non-square destination catches an x/y transposition that a square one
   cannot -- transposing a square warp still fills the form."
  | src dst |
  src := self quadrantForm: 32 @ 32.
  dst := Form extent: 128 @ 48 depth: 32.
  self warp: src into: dst cellSize: 1.
  self assertQuadrantsOf: dst! !

!WarpBltTest methodsFor: 'tests - orientation'!
testOrientationReduced
  "Reducing exercises the same interpolation with a step greater than one."
  | src dst |
  src := self quadrantForm: 128 @ 128.
  dst := Form extent: 32 @ 32 depth: 32.
  self warp: src into: dst cellSize: 1.
  self assertQuadrantsOf: dst! !

!WarpBltTest methodsFor: 'tests - smoothing'!
testSmoothedKeepsSolidColour
  "Smoothing averages a cell of source pixels. Well inside a solid region
   every sample is the same colour, so the average must be that colour --
   a smoothed warp must not tint or darken flat areas."
  | src dst |
  src := self quadrantForm: 64 @ 64.
  dst := Form extent: 64 @ 64 depth: 32.
  self warp: src into: dst cellSize: 2.
  self assertQuadrantsOf: dst! !

!WarpBltTest methodsFor: 'tests - smoothing'!
testSmoothedPreservesAlpha
  "Build 59: the Smalltalk fallback mixPix:sourceMap:destMap: summed only
   R, G and B, so every smoothed pixel came out with alpha 0. Warp an
   opaque form and require the result to still be opaque."
  | src dst pixel |
  src := Form extent: 64 @ 64 depth: 32.
  src fillColor: (Color r: 0.2 g: 0.6 b: 0.9).
  dst := Form extent: 32 @ 32 depth: 32.
  dst fillColor: Color transparent.
  self warp: src into: dst cellSize: 2.
  pixel := dst pixelValueAt: 16 @ 16.
  self assert: (pixel bitShift: -24) equals: 255! !

!WarpBltTest methodsFor: 'tests - colour map'!
testColorMapIsHonouredWhenSmoothing
  "A colorMap on the receiver maps each warped pixel through
   mixPix:sourceMap:destMap:. Our VM primitive does not implement mapping;
   since 2026-08-16 it fails when a map is set rather than quietly emitting
   unmapped pixels, so Smalltalk performs the mapped warp. Before that fix
   this answered red -- the source colour, straight through.

   cellSize must be > 1: Pharo's own fallback only consults colorMap on the
   smoothing path. At cellSize 1 it pokes source pixels across unmapped, so
   there is no mapped behaviour to assert there.

   colorMap must be set *after* cellSize:, because cellSize: overwrites it
   with (Color colorMapIfNeededFrom: 32 to: destForm depth) for any size > 1."
  | src dst warp map blue |
  src := Form extent: 32 @ 32 depth: 32.
  src fillColor: Color red.
  dst := Form extent: 32 @ 32 depth: 32.
  dst fillColor: Color black.

  "512 entries selects 3 bits per colour in mixPix:, so every averaged RGB
   lands somewhere in this table -- and every entry is blue."
  blue := Color blue pixelValueForDepth: 32.
  map := Bitmap new: 512 withAll: blue.

  warp := WarpBlt toForm: dst.
  warp sourceForm: src.
  warp cellSize: 2.
  warp colorMap: map.
  warp combinationRule: Form over.
  warp
    sourceQuad: (0 @ 0 corner: src extent) innerCorners
    destRect: (0 @ 0 extent: dst extent).
  warp warpBits.

  self
    assert: ((dst pixelValueAt: 16 @ 16) bitAnd: 16rFFFFFF)
    equals: (blue bitAnd: 16rFFFFFF)! !

!WarpBltTest methodsFor: 'tests - clipping'!
testClipRectIsRespected
  "Pixels outside the clip rectangle must be left alone."
  | src dst warp |
  src := Form extent: 32 @ 32 depth: 32.
  src fillColor: Color red.
  dst := Form extent: 64 @ 64 depth: 32.
  dst fillColor: Color black.

  warp := WarpBlt toForm: dst.
  warp sourceForm: src.
  warp colorMap: nil.
  warp cellSize: 1.
  warp combinationRule: Form over.
  warp clipRect: (0 @ 0 extent: 32 @ 32).
  warp
    sourceQuad: (0 @ 0 corner: src extent) innerCorners
    destRect: (0 @ 0 extent: 64 @ 64).
  warp warpBits.

  self assert: (self colorAt: 8 @ 8 in: dst) equals: Color red.
  self assert: (self colorAt: 48 @ 48 in: dst) equals: Color black! !
