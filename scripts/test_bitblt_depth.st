"BitBlt depth conversion tests.
 Tests that BitBlt primitive 96 handles cross-depth operations correctly.
 Would have caught the iPad world menu red X bug (8bit src to 1bit dest).
 See CLAUDE.md for how to inject and run."!

Object subclass: #BitBltDepthTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-BitBlt'!

!BitBltDepthTest class methodsFor: 'running'!
runAll
  "Run all depth conversion tests and write results."
  | stream pass fail |
  pass := 0. fail := 0.
  '/tmp/bitblt_depth_results.txt' asFileReference ensureDelete.
  stream := '/tmp/bitblt_depth_results.txt' asFileReference writeStream.
  [
    stream nextPutAll: '=== BitBlt Depth Conversion Tests ==='; cr; cr.

    "Test 8-to-1: The exact case that caused iPad world menu red X"
    (self test8to1pixPaintOn: stream) ifTrue: [pass := pass + 1] ifFalse: [fail := fail + 1].
    (self test8to1storeOn: stream) ifTrue: [pass := pass + 1] ifFalse: [fail := fail + 1].
    (self test8to1zeroPreservesDestOn: stream) ifTrue: [pass := pass + 1] ifFalse: [fail := fail + 1].

    "Test 16-to-1"
    (self test16to1pixPaintOn: stream) ifTrue: [pass := pass + 1] ifFalse: [fail := fail + 1].

    "Test 32-to-1"
    (self test32to1pixPaintOn: stream) ifTrue: [pass := pass + 1] ifFalse: [fail := fail + 1].

    "Test 1-to-1"
    (self test1to1storeOn: stream) ifTrue: [pass := pass + 1] ifFalse: [fail := fail + 1].

    "Test shadow canvas path (the real-world scenario)"
    (self testShadowCanvasStencilOn: stream) ifTrue: [pass := pass + 1] ifFalse: [fail := fail + 1].

    stream cr.
    stream nextPutAll: '=== RESULTS: '; print: pass; nextPutAll: ' passed, '; print: fail; nextPutAll: ' failed ==='; cr.
    stream flush.
    Transcript show: 'BitBlt depth tests: '; show: pass printString; show: ' passed, ';
      show: fail printString; show: ' failed'; cr]
  ensure: [stream close].
  ^ fail = 0! !

!BitBltDepthTest class methodsFor: 'helpers'!
makeForm: extent depth: depth allValue: wordVal
  "Create a Form and fill its bitmap directly (bypasses BitBlt).
   wordVal is the 32-bit word value to fill into every bitmap slot."
  | form |
  form := Form extent: extent depth: depth.
  form bits atAllPut: wordVal.
  ^ form! !

!BitBltDepthTest class methodsFor: 'tests'!
test8to1pixPaintOn: s
  "8-bit source with non-zero pixels to 1-bit dest, rule 25 (pixPaint).
   This is the exact case that caused the iPad world menu shadow errors."
  | src dst blt result |
  s nextPutAll: 'test8to1pixPaint: '.
  [
    "Fill 8-bit source with 0x80808080 (each byte = 128, non-zero)"
    src := self makeForm: 16@16 depth: 8 allValue: 16r80808080.
    "1-bit dest starts all zeros"
    dst := self makeForm: 16@16 depth: 1 allValue: 0.
    blt := BitBlt
      destForm: dst sourceForm: src
      fillColor: nil combinationRule: 25
      destOrigin: 0@0 sourceOrigin: 0@0
      extent: 16@16 clipRect: (0@0 extent: 16@16).
    blt copyBits.
    "After pixPaint with non-zero source, dest bits should be set"
    result := (dst pixelValueAt: 8@8) = 1.
    result
      ifTrue: [s nextPutAll: 'PASS'; cr]
      ifFalse: [s nextPutAll: 'FAIL (pixel at 8@8 = '; print: (dst pixelValueAt: 8@8); nextPutAll: ', expected 1)'; cr].
    s flush.
    ^ result]
  on: Error do: [:e |
    s nextPutAll: 'ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. s flush.
    ^ false]! !

!BitBltDepthTest class methodsFor: 'tests'!
test8to1storeOn: s
  "8-bit source to 1-bit dest, rule 3 (store). Non-zero pixels become 1."
  | src dst blt result |
  s nextPutAll: 'test8to1store: '.
  [
    src := self makeForm: 16@16 depth: 8 allValue: 16rFFFFFFFF.
    dst := self makeForm: 16@16 depth: 1 allValue: 0.
    blt := BitBlt
      destForm: dst sourceForm: src
      fillColor: nil combinationRule: 3
      destOrigin: 0@0 sourceOrigin: 0@0
      extent: 16@16 clipRect: (0@0 extent: 16@16).
    blt copyBits.
    result := (dst pixelValueAt: 8@8) = 1.
    result
      ifTrue: [s nextPutAll: 'PASS'; cr]
      ifFalse: [s nextPutAll: 'FAIL (pixel at 8@8 = '; print: (dst pixelValueAt: 8@8); nextPutAll: ', expected 1)'; cr].
    s flush.
    ^ result]
  on: Error do: [:e |
    s nextPutAll: 'ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. s flush.
    ^ false]! !

!BitBltDepthTest class methodsFor: 'tests'!
test8to1zeroPreservesDestOn: s
  "8-bit source with ZERO pixels to 1-bit dest, rule 25 (pixPaint).
   Zero source should NOT change dest (pixPaint: src=0 means keep dest)."
  | src dst blt result |
  s nextPutAll: 'test8to1zeroPreservesDest: '.
  [
    src := self makeForm: 16@16 depth: 8 allValue: 0.
    dst := self makeForm: 16@16 depth: 1 allValue: 16rFFFFFFFF.
    blt := BitBlt
      destForm: dst sourceForm: src
      fillColor: nil combinationRule: 25
      destOrigin: 0@0 sourceOrigin: 0@0
      extent: 16@16 clipRect: (0@0 extent: 16@16).
    blt copyBits.
    "pixPaint with zero source should preserve dest (still all 1s)"
    result := (dst pixelValueAt: 8@8) = 1.
    result
      ifTrue: [s nextPutAll: 'PASS'; cr]
      ifFalse: [s nextPutAll: 'FAIL (pixel at 8@8 = '; print: (dst pixelValueAt: 8@8); nextPutAll: ', expected 1)'; cr].
    s flush.
    ^ result]
  on: Error do: [:e |
    s nextPutAll: 'ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. s flush.
    ^ false]! !

!BitBltDepthTest class methodsFor: 'tests'!
test16to1pixPaintOn: s
  "16-bit source to 1-bit dest, rule 25 (pixPaint)."
  | src dst blt result |
  s nextPutAll: 'test16to1pixPaint: '.
  [
    "Fill 16-bit source: 0x7C007C00 = two non-zero 16-bit pixels per word"
    src := self makeForm: 16@16 depth: 16 allValue: 16r7C007C00.
    dst := self makeForm: 16@16 depth: 1 allValue: 0.
    blt := BitBlt
      destForm: dst sourceForm: src
      fillColor: nil combinationRule: 25
      destOrigin: 0@0 sourceOrigin: 0@0
      extent: 16@16 clipRect: (0@0 extent: 16@16).
    blt copyBits.
    result := (dst pixelValueAt: 8@8) = 1.
    result
      ifTrue: [s nextPutAll: 'PASS'; cr]
      ifFalse: [s nextPutAll: 'FAIL (pixel at 8@8 = '; print: (dst pixelValueAt: 8@8); nextPutAll: ', expected 1)'; cr].
    s flush.
    ^ result]
  on: Error do: [:e |
    s nextPutAll: 'ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. s flush.
    ^ false]! !

!BitBltDepthTest class methodsFor: 'tests'!
test32to1pixPaintOn: s
  "32-bit source to 1-bit dest, rule 25 (pixPaint). Already supported; regression test."
  | src dst blt result |
  s nextPutAll: 'test32to1pixPaint: '.
  [
    src := self makeForm: 16@16 depth: 32 allValue: 16rFF0000FF.
    dst := self makeForm: 16@16 depth: 1 allValue: 0.
    blt := BitBlt
      destForm: dst sourceForm: src
      fillColor: nil combinationRule: 25
      destOrigin: 0@0 sourceOrigin: 0@0
      extent: 16@16 clipRect: (0@0 extent: 16@16).
    blt copyBits.
    result := (dst pixelValueAt: 8@8) = 1.
    result
      ifTrue: [s nextPutAll: 'PASS'; cr]
      ifFalse: [s nextPutAll: 'FAIL (pixel at 8@8 = '; print: (dst pixelValueAt: 8@8); nextPutAll: ', expected 1)'; cr].
    s flush.
    ^ result]
  on: Error do: [:e |
    s nextPutAll: 'ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. s flush.
    ^ false]! !

!BitBltDepthTest class methodsFor: 'tests'!
test1to1storeOn: s
  "1-bit source to 1-bit dest, rule 3 (store). Basic mask copy."
  | src dst blt result |
  s nextPutAll: 'test1to1store: '.
  [
    src := self makeForm: 16@16 depth: 1 allValue: 16rFFFFFFFF.
    dst := self makeForm: 16@16 depth: 1 allValue: 0.
    blt := BitBlt
      destForm: dst sourceForm: src
      fillColor: nil combinationRule: 3
      destOrigin: 0@0 sourceOrigin: 0@0
      extent: 16@16 clipRect: (0@0 extent: 16@16).
    blt copyBits.
    result := (dst pixelValueAt: 8@8) = 1.
    result
      ifTrue: [s nextPutAll: 'PASS'; cr]
      ifFalse: [s nextPutAll: 'FAIL (pixel at 8@8 = '; print: (dst pixelValueAt: 8@8); nextPutAll: ', expected 1)'; cr].
    s flush.
    ^ result]
  on: Error do: [:e |
    s nextPutAll: 'ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. s flush.
    ^ false]! !

!BitBltDepthTest class methodsFor: 'tests'!
testShadowCanvasStencilOn: s
  "The real-world scenario: ShadowDrawingCanvas stencil operation.
   Creates a 1-bit shadow mask form, draws an 8-bit icon onto it via stencil.
   This is the exact code path that produced the iPad world menu red X errors."
  | shadowForm iconForm canvas result |
  s nextPutAll: 'testShadowCanvasStencil: '.
  [
    "Create a shadow mask (1-bit) like ShadowDrawingCanvas does"
    shadowForm := self makeForm: 32@32 depth: 1 allValue: 0.

    "Create an 8-bit icon with non-zero content (bypass BitBlt fill)"
    iconForm := self makeForm: 16@16 depth: 8 allValue: 16r80808080.

    "Use FormCanvas stencil:at:sourceRect:color: — the actual code path"
    canvas := FormCanvas on: shadowForm.
    canvas stencil: iconForm at: 4@4 sourceRect: (0@0 extent: 16@16) color: Color black.

    "Verify: pixels within the icon area should be set in the shadow mask"
    result := (shadowForm pixelValueAt: 8@8) = 1.
    result
      ifTrue: [s nextPutAll: 'PASS'; cr]
      ifFalse: [s nextPutAll: 'FAIL (shadow mask pixel at 8@8 = '; print: (shadowForm pixelValueAt: 8@8); nextPutAll: ', expected 1)'; cr].
    s flush.
    ^ result]
  on: Error do: [:e |
    s nextPutAll: 'ERROR: '; nextPutAll: e class name; nextPutAll: ' - '; nextPutAll: e messageText; cr. s flush.
    ^ false]! !

!BitBltDepthTest class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self runAll! !

!BitBltDepthTest class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup at priority 80 (before SUnit runner at 90)"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #BitBltDepthTest)
  inCategory: SessionManager default userCategory
  atPriority: 80!
