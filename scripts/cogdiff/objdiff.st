"objdiff.st -- scenarios run on our VM and on Cog; diff the two answers.
Run: test_load_image P.image eval "$(cat objdiff.st)"; for Cog, arch -x86_64 Pharo --headless C.image eval ...
See docs/vm-compat-bugs.md, 2026-10-17."

| results run |
results := OrderedCollection new.
run := [:name :blk | results add: name -> ([ blk value ] on: Error do: [:e | 'ERR ', e class name ]) ].
(Object << #ObjDiffP slots: {#a. #b}; package: 'ObjDiff') install.
(Smalltalk at: #ObjDiffP) compile: 'a: x a := x'. (Smalltalk at: #ObjDiffP) compile: 'a ^ a'.
run value: #roArrayAtPut value: [ | a | a := Array new: 2. a beReadOnlyObject. a at: 1 put: 3. a ].
run value: #roArrayRetry value: [ | a | a := Array new: 2. a beReadOnlyObject. [ a at: 1 put: 3 ] on: ModificationForbidden do: [:e | { e class name. e fieldIndex. e newValue. e object == a. e messageText notNil } ] ].
run value: #roInstVar value: [ | p | p := (Smalltalk at: #ObjDiffP) new. p beReadOnlyObject. [ p a: 5 ] on: ModificationForbidden do: [:e | { e fieldIndex. e newValue. e retryModificationNoResume. p a } ] ].
run value: #roInstVarAtPut value: [ | p | p := (Smalltalk at: #ObjDiffP) new. p beReadOnlyObject. [ p instVarAt: 2 put: 9 ] on: ModificationForbidden do: [:e | { e fieldIndex. e newValue } ] ].
run value: #roString value: [ | s | s := 'abc' copy. s beReadOnlyObject. [ s at: 1 put: $z ] on: ModificationForbidden do: [:e | { e fieldIndex. e newValue } ] ].
run value: #roByteArray value: [ | s | s := #[1 2 3] copy. s beReadOnlyObject. [ s at: 1 put: 9 ] on: ModificationForbidden do: [:e | { e fieldIndex. e newValue } ] ].
run value: #roResume value: [ | a | a := Array new: 2. a beReadOnlyObject. [ a at: 1 put: 3 ] on: ModificationForbidden do: [:e | a beWritableObject. e retryModification ]. a ].
run value: #roReplace value: [ | a | a := Array new: 3. a beReadOnlyObject. [ a replaceFrom: 1 to: 2 with: #(7 8) startingAt: 1 ] on: ModificationForbidden do: [:e | e class name ] ].
run value: #roBecome value: [ | a | a := Array new: 1. a beReadOnlyObject. [ a become: Array new ] on: Error do: [:e | e messageText ] ].
run value: #literalArray value: [ #(1 2) isReadOnlyObject ].
run value: #literalString value: [ 'lit' isReadOnlyObject ].
run value: #symbolRO value: [ #sym isReadOnlyObject ].
run value: #smallIntRO value: [ 3 isReadOnlyObject ].
run value: #floatRO value: [ 1.5 isReadOnlyObject ].
run value: #boxedFloatRO value: [ 1.0e308 isReadOnlyObject ].
run value: #largeIntRO value: [ (2 raisedTo: 100) isReadOnlyObject ].
run value: #nilRO value: [ { nil isReadOnlyObject. true isReadOnlyObject } ].
run value: #copyOfRO value: [ | a | a := Array new: 2. a beReadOnlyObject. { a copy isReadOnlyObject. a shallowCopy isReadOnlyObject. a deepCopy isReadOnlyObject } ].
run value: #setRO value: [ | a | a := Array new: 1. { a setIsReadOnlyObject: true. a setIsReadOnlyObject: false. a isReadOnlyObject } ].
run value: #pin value: [ | o | o := Object new. { o isPinned. o pin. o isPinned. o unpin. o isPinned } ].
run value: #pinImmediate value: [ [ 3 pin ] on: Error do: [:e | e class name ] ].
run value: #pinnedBecome value: [ | o | o := Array new: 1. o pin. [ o become: (Array new: 1) ] on: Error do: [:e | e messageText ] ].
run value: #newContents value: [ { (Array new: 2). (String new: 2) asByteArray. (ByteArray new: 2). (WordArray new: 2) asArray. (Object new) basicSize } ].
run value: #basicNewBad value: [ [ Object basicNew: 2 ] on: Error do: [:e | e class name ] ].
run value: #arrayBasicNew value: [ (Array basicNew: 0) size ].
run value: #byteObjSlots value: [ { #[1 2 3] basicSize. 'abc' basicSize. (Array new: 3) basicSize. (2 raisedTo: 100) basicSize. 1.0e308 basicSize } ].
run value: #instVarNamed value: [ | p | p := (Smalltalk at: #ObjDiffP) new a: 4; yourself. { p instVarNamed: #a. p instVarAt: 1. [ p instVarAt: 3 ] on: Error do: [:e | e class name ] } ].
run value: #shallowCopyIdentity value: [ | p q | p := (Smalltalk at: #ObjDiffP) new a: 4; yourself. q := p shallowCopy. { q == p. q a. q class name } ].
run value: #classOfImmediates value: [ { 3 class. $a class. 1.5 class. nil class. true class } collect: [:c | c name ] ].
run value: #changeClass value: [ | p | p := (Smalltalk at: #ObjDiffP) new. [ p primitiveChangeClassTo: Object new ] on: Error do: [:e | e class name ] ].
run value: #adoptInstance value: [ | p | p := (Smalltalk at: #ObjDiffP) new. [ Object adoptInstance: p. p class name ] on: Error do: [:e | e class name ] ].
results asArray
