"pinforward.st -- primitives that meet a pinned young object through an object slot.
Run: test_load_image P.image eval with the text of pinforward.st; for Cog, arch -x86_64 Pharo C.image eval ...
Pinning a young object makes a pinned copy in old space and turns the original into a forwarder
until the next scavenge.  A slot that held the original (Form bits, BitBlt halftone, an array
element) still names the forwarder.  Spur's contract: a primitive that meets the forwarder fails,
the VM follows the forwarders in the receiver and arguments, and retries.
Every case runs twice, with pin false and pin true.  The answers must agree, and must also equal
the answers of stock Cog.  The answer is one line per case: name, PLAIN-equals-PINNED, answer."

| pinIf run out forms |
out := WriteStream on: String new.
pinIf := [ :pin :obj | pin ifTrue: [ obj pinInMemory ]. obj ].
run := [ :name :blk | | plain pinned |
  plain := [ (blk value: false) printString ] on: Error do: [ :e | 'ERR ', e class name ].
  pinned := [ (blk value: true) printString ] on: Error do: [ :e | 'ERR ', e class name ].
  out nextPutAll: name; nextPutAll: ' '; nextPutAll: (plain = pinned ifTrue: [ 'SAME ' ] ifFalse: [ 'DIFF plain=', plain, ' pinned=' ]); nextPutAll: pinned; cr ].
forms := [ :depth | | f | f := Form extent: 8@8 depth: depth. f bits atAllPut: 0. f ].

"BitBlt family: the Form bits slot holds the forwarder"
run value: 'pixelValueAt32' value: [ :pin | | f |
  f := forms value: 32. f bits at: 1 put: 305419896. pinIf value: pin value: f bits.
  { f pixelValueAt: 0@0. f pixelValueAt: 3@3 } ].
run value: 'pixelValueAt8' value: [ :pin | | f |
  f := forms value: 8. f bits at: 1 put: 16r04030201. pinIf value: pin value: f bits.
  { f pixelValueAt: 0@0. f pixelValueAt: 1@0 } ].
run value: 'pixelValueAtPut32' value: [ :pin | | f |
  f := forms value: 32. pinIf value: pin value: f bits.
  f pixelValueAt: 2@2 put: 99. { f pixelValueAt: 2@2. f bits asArray copyFrom: 1 to: 20 } ].
run value: 'fillColor' value: [ :pin | | f |
  f := forms value: 32. pinIf value: pin value: f bits.
  f fillColor: Color red. { f bits first. f bits last. f pixelValueAt: 4@4 } ].
run value: 'copyBitsSrcDst' value: [ :pin | | s d |
  s := forms value: 32. d := forms value: 32. s fillColor: Color blue.
  pinIf value: pin value: s bits. pinIf value: pin value: d bits.
  d copy: (0@0 extent: 4@4) from: 0@0 in: s rule: Form over. { d pixelValueAt: 1@1. d pixelValueAt: 6@6 } ].
run value: 'copyBitsHalftone' value: [ :pin | | d bb ht |
  d := forms value: 32. ht := Form extent: 8@8 depth: 32. ht bits atAllPut: 7.
  pinIf value: pin value: d bits. pinIf value: pin value: ht bits.
  bb := BitBlt toForm: d. bb fillColor: ht. bb destRect: (0@0 extent: 8@8). bb combinationRule: 3. bb copyBits.
  d pixelValueAt: 5@5 ].
run value: 'warpBits' value: [ :pin | | s d w |
  s := forms value: 32. s fillColor: Color green. d := forms value: 32.
  pinIf value: pin value: s bits. pinIf value: pin value: d bits.
  w := WarpBlt toForm: d. w sourceForm: s. w combinationRule: 3. w cellSize: 1.
  w copyQuad: { 0@0. 0@8. 8@8. 8@0 } toRect: (0@0 extent: 8@8). { d pixelValueAt: 2@2. d pixelValueAt: 7@7 } ].
run value: 'asFormOfDepth' value: [ :pin | | f |
  f := forms value: 32. f fillColor: Color yellow. pinIf value: pin value: f bits.
  (f asFormOfDepth: 16) bits first ].
run value: 'formCopy' value: [ :pin | | f |
  f := forms value: 32. f fillColor: Color red. pinIf value: pin value: f bits.
  f deepCopy bits asArray first: 4 ].
run value: 'formAsByteArray' value: [ :pin | | f |
  f := forms value: 8. f bits at: 1 put: 16r04030201. pinIf value: pin value: f bits.
  (f colorAt: 1@0) printString ].

"Bitmap and ByteArray arguments"
run value: 'compressBitmap' value: [ :pin | | b h r |
  b := Bitmap new: 16. b atAllPut: 5. h := Array with: b. pinIf value: pin value: b.
  r := h first compressToByteArray. r asArray ].
run value: 'decompressBitmap' value: [ :pin | | src dst h |
  src := (Bitmap withAll: #(1 2 3 4 5 6 7 8)) compressToByteArray. dst := Bitmap new: 8. h := Array with: src with: dst.
  pinIf value: pin value: src. pinIf value: pin value: dst.
  h second decompress: h second fromByteArray: h first at: 1. h second asArray ].
run value: 'replaceFromWithStartingAt' value: [ :pin | | a b h |
  a := ByteArray new: 8. b := ByteArray withAll: #(1 2 3 4 5 6 7 8). h := Array with: a with: b.
  pinIf value: pin value: a. pinIf value: pin value: b.
  h first replaceFrom: 1 to: 8 with: h second startingAt: 1. h first asArray ].
run value: 'stringCompare' value: [ :pin | | a b h |
  a := 'abcdef' copy. b := 'abcdeg' copy. h := Array with: a with: b.
  pinIf value: pin value: a. pinIf value: pin value: b.
  { h first < h second. h first = h second. h first hash = a copy hash. h first compare: h second } ].
run value: 'stringIndexOf' value: [ :pin | | a h |
  a := 'hello world' copy. h := Array with: a. pinIf value: pin value: a.
  { h first indexOf: $w. h first asUppercase. h first asSymbol. h first findString: 'wor' startingAt: 1 } ].
run value: 'largeInteger' value: [ :pin | | a b h |
  a := 1 bitShift: 100. b := (1 bitShift: 90) + 7. h := Array with: a with: b.
  pinIf value: pin value: a. pinIf value: pin value: b.
  { h first + h second. h first * h second. h first // h second. h first \\ h second. h first < h second. h first bitAnd: h second } ].
run value: 'floatArray' value: [ :pin | | fa a b h |
  fa := Smalltalk globals at: #FloatArray ifAbsent: [ nil ].
  fa isNil ifTrue: [ 'FloatArray absent' ] ifFalse: [
  a := fa withAll: #(1.5 2.5 3.5). b := fa withAll: #(10.0 20.0 30.0). h := Array with: a with: b.
  pinIf value: pin value: a. pinIf value: pin value: b.
  { (h first + h second) asArray. (h first * h second) asArray. h first dot: h second. h first sum } ] ].
run value: 'boxedFloat' value: [ :pin | | a b h |
  a := 1.0e300 * 3. b := 2.5. h := Array with: a with: b. pinIf value: pin value: a. pinIf value: pin value: b.
  { h first + h second. h first * h second. h first < h second. h second sqrt } ].
run value: 'wordArrayAndSocketFree' value: [ :pin | | a h |
  a := WordArray withAll: #(5 4 3 2 1). h := Array with: a. pinIf value: pin value: a.
  { h first asArray reverse. h first copy asArray. (h first copyFrom: 2 to: 3) asArray. h first hash = a copy hash } ].

"String search and translate: body string and match table held in slots (CharacterScanner prim 103 is gone in Pharo 14)"
run value: 'findSubstring' value: [ :pin | | key body table h |
  key := 'wor' copy. body := 'hello world' copy. table := ByteArray new: 256.
  0 to: 255 do: [ :i | table at: i + 1 put: i ].
  h := Array with: key with: body with: table.
  pinIf value: pin value: key. pinIf value: pin value: body. pinIf value: pin value: table.
  { h second findSubstring: h first in: h second startingAt: 1 matchTable: h third.
    h second findString: h first startingAt: 1 caseSensitive: false } ].
run value: 'stringTranslate' value: [ :pin | | body table h |
  body := 'hello' copy. table := ByteArray new: 256. 0 to: 255 do: [ :i | table at: i + 1 put: i + 1 \\ 256 ].
  h := Array with: body with: table. pinIf value: pin value: body. pinIf value: pin value: table.
  ByteString translate: h first from: 1 to: 5 table: h second. h first ].

"Streams: the collection is held in an ivar of the stream (primitives 65 and 66 read it)"
run value: 'readStreamNext' value: [ :pin | | c st |
  c := ByteArray withAll: #(1 2 3 4 5 6). st := ReadStream on: c. pinIf value: pin value: c.
  { st next. st next. st next: 2. st atEnd. st upToEnd } ].
run value: 'readStreamString' value: [ :pin | | c st |
  c := 'hello world' copy. st := ReadStream on: c. pinIf value: pin value: c.
  { st next. st peek. st upTo: $o. st upToEnd } ].
run value: 'writeStreamNextPut' value: [ :pin | | c st |
  c := String new: 4. st := WriteStream on: c. pinIf value: pin value: c.
  st nextPut: $a; nextPut: $b; nextPutAll: 'cdefgh'. st contents ].
run value: 'writeStreamBytes' value: [ :pin | | c st |
  c := ByteArray new: 2. st := WriteStream on: c. pinIf value: pin value: c.
  1 to: 7 do: [ :i | st nextPut: i ]. st contents asArray ].
run value: 'orderedCollectionAdd' value: [ :pin | | oc arr |
  oc := OrderedCollection new: 4. arr := oc instVarNamed: #array. pinIf value: pin value: arr.
  1 to: 9 do: [ :i | oc add: i * i ]. { oc asArray. oc removeFirst. oc size } ].

"FFI: a ByteArray handed to C, held in an object slot"
run value: 'ffiMemCopy' value: [ :pin | | s d h |
  s := ByteArray withAll: #(9 8 7 6 5 4 3 2). d := ByteArray new: 8. h := Array with: s with: d.
  pinIf value: pin value: s. pinIf value: pin value: d.
  LibC uniqueInstance memCopy: h first to: h second size: 8. h second asArray ].
run value: 'ffiStructHandle' value: [ :pin | | s h v |
  s := ByteArray new: 16. h := Array with: s. pinIf value: pin value: s.
  h first unsignedLongAt: 1 put: 123456 bigEndian: false. h first doubleAt: 9 put: 2.5.
  { h first unsignedLongAt: 1 bigEndian: false. h first doubleAt: 9. h first asArray first: 4 } ].
run value: 'fileWriteRead' value: [ :pin | | bytes h ref r |
  bytes := ByteArray withAll: #(65 66 67 68). h := Array with: bytes. pinIf value: pin value: bytes.
  ref := FileSystem workingDirectory / 'pinforward.tmp'. ref ensureDelete.
  ref binaryWriteStreamDo: [ :s | s nextPutAll: h first ].
  r := ref binaryReadStreamDo: [ :s | s upToEnd ]. ref ensureDelete. r asArray ].

"Balloon: the work buffer is a word array held by the engine"
run value: 'balloonFill' value: [ :pin | | f eng |
  f := forms value: 32. eng := BalloonEngine new. eng bitBlt: (BitBlt toForm: f).
  eng clipRect: f boundingBox. eng destOffset: 0@0.
  eng drawRectangle: (1@1 corner: 3@3) fill: Color blue borderWidth: 0 borderColor: Color black transform: nil. eng flush.
  pinIf value: pin value: (eng instVarNamed: #workBuffer).
  eng drawRectangle: (1@1 corner: 7@7) fill: Color red borderWidth: 0 borderColor: Color black transform: nil. eng flush.
  { f pixelValueAt: 4@4. f pixelValueAt: 0@0 } ].

out contents
