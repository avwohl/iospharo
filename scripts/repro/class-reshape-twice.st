"Regression repro: reshaping a class twice must keep its instances visible.

 ShiftClassInstaller migrates instances into `newClass basicNew` copies (the
 new class registers at its own class-table index X2), then forwards the old
 class to the new one with a hash-copying become, so the class's hash -- its
 class-table index -- becomes the old X1.  The migrated instances still carry
 X2.  Cog resolves that in allInstancesOf (ambiguous-class scan); before the
 fix our allInstances scanned X1 only, so the SECOND reshape missed the
 instance and left it one slot short (apptivegrid-soil
 SoilMigrationTest>>testSerializingObjectWithChangedShapeAfterReading:
 PrimitiveFailed instVarAt:).

 Run:  ./build-rel/test_load_image Pharo.image eval \
         ""'scripts/repro/class-reshape-twice.st' asFileReference fileIn""
 Pass: three=nil allInst=1.  Before the fix: three=ERR ... allInst=0."
| cls o r |
cls := (Object << #ZZReshapeTwice slots: { #one. #two }; package: 'ZZReshapeTwice') install.
o := cls new. o instVarNamed: #one put: 1; instVarNamed: #two put: 2.
cls removeSlot: (cls slotNamed: #two).
r := 'after remove: allInst=', cls allInstances size printString.
cls addSlot: #three asSlot.
r := r, ' | after add: three=',
  ([(o instVarNamed: #three) printString] on: Error do: [:e | 'ERR ', e messageText asString]),
  ' allInst=', cls allInstances size printString.
Stdio stdout nextPutAll: 'RESHAPE ', r; lf; flush.
cls removeFromSystem.
r
