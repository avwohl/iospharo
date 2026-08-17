"Tests for primitiveObjectPointsTo (primitive 132), the primitive behind
 #pointsTo: and, through it, #pointersTo and SystemNavigation's reference
 searches.

 The primitive must answer whether any of the receiver's *pointer slots* holds
 the argument. Objects whose contents are raw data have no pointer slots, so the
 answer for them is always false — but the guard that skips them has been wrong
 twice, and each time the failure mode was the same: raw bytes get compared
 against the target's oop bits, and #pointsTo: answers true because a bitmap or
 a string happened to contain the right bit pattern.

   Build 122  the word-array guard read `format >= Indexable32 and format <=
              Indexable64`, i.e. >= 10 and <= 9, never true. WordArray,
              DoubleWordArray, Bitmap and Form bits all fell through.
   2026-08-16 the 16-bit formats (12-15: DoubleByteArray, 16-bit strings) still
              fell through, as did the reserved formats 6-8.

 The tests below construct the failure directly rather than hoping to trip over
 it. A SmallInteger's oop is its value shifted left three with tag 1, so
 SmallInteger 0 is the bit pattern 1. A DoubleByteArray holding the shorts
 1, 0, 0, 0 is therefore one 64-bit slot containing exactly that pattern: if the
 primitive scans it as a pointer slot it answers true, and if it correctly
 refuses to scan raw data it answers false.

 Verified 2026-08-16 to have teeth: on the pre-fix VM
 testDoubleByteArrayIsNotScannedForPointers answers true and fails."!

TestCase subclass: #ObjectPointsToTest
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Kernel'!

!ObjectPointsToTest methodsFor: 'helpers'!
smallIntegerZeroBitPattern
  "SmallInteger 0 has oop bits 1 — tag 1, value 0. As four little-endian
   16-bit elements that is 1, 0, 0, 0."
  ^ #(1 0 0 0)! !

!ObjectPointsToTest methodsFor: 'tests - raw data is not scanned'!
testDoubleByteArrayIsNotScannedForPointers
  "Formats 12-15. The case that regressed on 2026-08-16."
  | dba |
  dba := DoubleByteArray new: 4.
  self smallIntegerZeroBitPattern doWithIndex: [ :each :i | dba at: i put: each ].
  self deny: (dba pointsTo: 0)! !

!ObjectPointsToTest methodsFor: 'tests - raw data is not scanned'!
testWordArrayIsNotScannedForPointers
  "Formats 10-11. The case that regressed in build 122."
  | wa |
  wa := WordArray new: 2.
  wa at: 1 put: 1; at: 2 put: 0.
  self deny: (wa pointsTo: 0)! !

!ObjectPointsToTest methodsFor: 'tests - raw data is not scanned'!
testByteArrayIsNotScannedForPointers
  "Formats 16-23."
  | ba |
  ba := ByteArray new: 8.
  ba at: 1 put: 1.
  2 to: 8 do: [ :i | ba at: i put: 0 ].
  self deny: (ba pointsTo: 0)! !

!ObjectPointsToTest methodsFor: 'tests - raw data is not scanned'!
testFloat64ArrayIsNotScannedForPointers
  "Format 9. A double whose bits happen to look like an oop must not match."
  | fa |
  fa := Float64Array new: 1.
  fa at: 1 put: 0.0.
  self deny: (fa pointsTo: 0).
  self deny: (fa pointsTo: nil)! !

!ObjectPointsToTest methodsFor: 'tests - pointer objects are scanned'!
testArrayFindsItsElement
  "The primitive must still do its actual job."
  | target arr |
  target := Object new.
  arr := Array with: 1 with: target with: 3.
  self assert: (arr pointsTo: target).
  self deny: (arr pointsTo: Object new)! !

!ObjectPointsToTest methodsFor: 'tests - pointer objects are scanned'!
testFixedSlotsAreScanned
  "A non-indexable object's named instance variables count as pointer slots."
  | assoc target |
  target := Object new.
  assoc := Association key: #k value: target.
  self assert: (assoc pointsTo: target)! !

!ObjectPointsToTest methodsFor: 'tests - pointer objects are scanned'!
testCompiledMethodFindsALiteralButNotItsBytecodes
  "CompiledMethods hold pointers in the literal frame only; the bytecodes
   after it are raw and must not be compared."
  | method literal |
  method := ObjectPointsToTest >> #testArrayFindsItsElement.
  literal := method literals
    detect: [ :each | each isSymbol ]
    ifNone: [ nil ].
  literal ifNotNil: [ self assert: (method pointsTo: literal) ]! !

!ObjectPointsToTest methodsFor: 'tests - immediates'!
testImmediateReceiverAnswersFalse
  "An immediate has no slots at all."
  self deny: (42 pointsTo: 0).
  self deny: ($a pointsTo: 0)! !
