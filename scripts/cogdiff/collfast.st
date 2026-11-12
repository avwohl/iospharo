"collfast.st -- hazard cases for the OrderedCollection>>add: and
WriteStream>>nextPut: paths.  Run on our VM and on Cog; diff the two answers.
Run: test_load_image P.image eval with the text of collfast.st; for Cog,
arch -x86_64 ./pharo C.image eval with the same text.
See docs/vm-compat-bugs.md, 2026-11-12, and docs/deferred.md.
The answer is one String, one line per scenario."

| results run ctr patch ocSrc wsSrc |
results := OrderedCollection new.
ctr := Array with: 0.
Smalltalk at: #CollFastCtr put: ctr.
run := [:name :blk | results add: name -> ([ blk value ] on: Error do: [:e | 'ERR ', e class name ]) ].

(OrderedCollection << #CollFastOC slots: {#extra}; package: 'CollFast') install.
(OrderedCollection << #CollFastOCOver slots: {}; package: 'CollFast') install.
(WriteStream << #CollFastWS slots: {#extra}; package: 'CollFast') install.
(WriteStream << #CollFastWSOver slots: {}; package: 'CollFast') install.
(Smalltalk at: #CollFastOCOver) compile: 'addLast: x
	(Smalltalk at: #CollFastCtr) at: 1 put: ((Smalltalk at: #CollFastCtr) at: 1) + 1.
	^ super addLast: x'.
(Smalltalk at: #CollFastWSOver) compile: 'nextPut: x
	(Smalltalk at: #CollFastCtr) at: 1 put: ((Smalltalk at: #CollFastCtr) at: 1) + 1.
	^ super nextPut: x'.

"Plain behaviour, growth, answers."
run value: #ocAddAnswer value: [ | oc | oc := OrderedCollection new. { oc add: 5. oc add: nil. (oc add: oc) == oc. oc size. (oc at: 3) == oc } ].
run value: #ocGrow value: [ | oc | oc := OrderedCollection new. 1 to: 1000 do: [:i | oc add: i * 2]. { oc size. oc first. oc last. (oc inject: 0 into: [:a :b | a + b]) } ].
run value: #ocAfterRemoveFirst value: [ | oc | oc := OrderedCollection new. 1 to: 10 do: [:i | oc add: i]. 1 to: 4 do: [:i | oc removeFirst]. oc add: 99. oc asArray ].
run value: #ocAddFirstMix value: [ | oc | oc := OrderedCollection new. 1 to: 30 do: [:i | oc addFirst: i. oc add: i negated]. { oc size. oc first. oc last. (oc at: 30) . (oc at: 31) } ].
run value: #ocSubclassIvar value: [ | oc | oc := (Smalltalk at: #CollFastOC) new. 1 to: 40 do: [:i | oc add: i]. { oc size. oc last. oc class name } ].
run value: #sortedAdd value: [ | s | s := SortedCollection new. #(5 3 9 1 7) do: [:i | s add: i]. s asArray ].
run value: #sortedAddBlock value: [ | s | s := SortedCollection sortBlock: [:a :b | a >= b]. 1 to: 20 do: [:i | s add: i \\ 7]. s asArray ].
run value: #wsArray value: [ | a | a := Array streamContents: [:s | 1 to: 300 do: [:i | s nextPut: i]]. { a size. a first. a last } ].
run value: #wsArrayAnswer value: [ | ws | ws := WriteStream on: (Array new: 2). { ws nextPut: 7. ws nextPut: 8. ws nextPut: 9. ws contents } ].
run value: #wsString value: [ String streamContents: [:s | s nextPut: $a; nextPut: $b; nextPut: $c ] ].
run value: #wsStringGrow value: [ (String streamContents: [:s | 1 to: 500 do: [:i | s nextPut: (Character value: 97 + (i \\ 26))]]) size ].
run value: #wsWide value: [ | s | s := String streamContents: [:ws | ws nextPut: $a. ws nextPut: (Character value: 16r100). ws nextPut: (Character value: 16r1F600). ws nextPut: $z ]. { s size. s class name. s asArray collect: [:c | c codePoint] } ].
run value: #wsWideOnWide value: [ | ws | ws := WriteStream on: (WideString new: 2). ws nextPut: (Character value: 233). ws nextPut: (Character value: 16r3B1). ws nextPut: $q. ws contents asArray collect: [:c | c codePoint] ].
run value: #wsLatin1 value: [ | ws | ws := WriteStream on: (String new: 2). ws nextPut: (Character value: 233). ws nextPut: (Character value: 255). ws contents asArray collect: [:c | c codePoint] ].
run value: #wsBytes value: [ | ws | ws := WriteStream on: (ByteArray new: 2). 1 to: 5 do: [:i | ws nextPut: i * 50]. ws contents asArray ].
run value: #wsBytesBad value: [ | ws | ws := WriteStream on: (ByteArray new: 2). ws nextPut: 300 ].
run value: #wsStringBad value: [ | ws | ws := WriteStream on: (String new: 2). ws nextPut: 3 ].
run value: #wsText value: [ | ws | ws := WriteStream on: (Text new: 2). ws nextPut: $a; nextPut: $b. ws contents string ].
run value: #wsWith value: [ | ws | ws := WriteStream with: #(1 2). ws nextPut: 3. ws contents ].
run value: #wsSubclassIvar value: [ | ws | ws := (Smalltalk at: #CollFastWS) on: Array new. 1 to: 20 do: [:i | ws nextPut: i]. { ws contents size. ws class name } ].
run value: #rwsPut value: [ | ws | ws := ReadWriteStream on: Array new. 1 to: 10 do: [:i | ws nextPut: i]. ws reset. { ws next. ws next. ws contents size } ].
run value: #rwsString value: [ | ws | ws := ReadWriteStream on: String new. ws nextPutAll: 'hello'; nextPut: $!. ws contents ].

"A subclass that overrides the selector must see every call."
run value: #ocOverride value: [ | oc n | oc := (Smalltalk at: #CollFastOCOver) new. ctr at: 1 put: 0. 1 to: 25 do: [:i | oc add: i]. n := ctr at: 1. { n. oc size. oc last } ].
run value: #wsOverride value: [ | ws n | ws := (Smalltalk at: #CollFastWSOver) on: Array new. ctr at: 1 put: 0. 1 to: 25 do: [:i | ws nextPut: i]. n := ctr at: 1. { n. ws contents size } ].

"Redefine the method in the real class; every call must run the new body."
ocSrc := (OrderedCollection >> #addLast:) sourceCode.
wsSrc := (WriteStream >> #nextPut:) sourceCode.
patch := [:cls :src |
	| lines |
	lines := src lines.
	cls compile: lines first, String lf, '(Smalltalk at: #CollFastCtr) at: 1 put: ((Smalltalk at: #CollFastCtr) at: 1) + 1.', String lf, (String lf join: lines allButFirst) ].
run value: #ocRedefined value: [ | oc n |
	patch value: OrderedCollection value: ocSrc.
	[ oc := OrderedCollection new. ctr at: 1 put: 0. 1 to: 25 do: [:i | oc add: i]. n := ctr at: 1 ]
		ensure: [ OrderedCollection compile: ocSrc ].
	{ n. oc size. oc last } ].
run value: #wsRedefined value: [ | ws n |
	patch value: WriteStream value: wsSrc.
	[ ws := WriteStream on: Array new. ctr at: 1 put: 0. 1 to: 25 do: [:i | ws nextPut: i]. n := ctr at: 1 ]
		ensure: [ WriteStream compile: wsSrc ].
	{ n. ws contents size } ].
run value: #ocRestored value: [ | oc n | oc := OrderedCollection new. ctr at: 1 put: 0. 1 to: 25 do: [:i | oc add: i]. { ctr at: 1. oc size } ].

"Read-only receivers and collections: Cog raises ModificationForbidden."
run value: #ocReadOnly value: [ | oc | oc := OrderedCollection new. oc add: 1. oc beReadOnlyObject. oc add: 2 ].
run value: #ocReadOnlyInfo value: [ | oc | oc := OrderedCollection new. oc add: 1. oc beReadOnlyObject.
	[ oc add: 2 ] on: ModificationForbidden do: [:e | { e class name. e fieldIndex. e newValue. e object == oc } ] ].
run value: #ocReadOnlyAfter value: [ | oc | oc := OrderedCollection new. oc add: 1. oc beReadOnlyObject.
	[ oc add: 2 ] on: ModificationForbidden do: [:e | e retryModificationNoResume ]. oc beWritableObject. { oc size. oc asArray } ].
run value: #ocArrayReadOnly value: [ | oc | oc := OrderedCollection new: 10. oc add: 1. (oc instVarAt: 1) beReadOnlyObject. oc add: 2 ].
run value: #ocArrayReadOnlyInfo value: [ | oc | oc := OrderedCollection new: 10. oc add: 1. (oc instVarAt: 1) beReadOnlyObject.
	[ oc add: 2 ] on: ModificationForbidden do: [:e | { e class name. e fieldIndex. e newValue. e object == (oc instVarAt: 1) } ] ].
run value: #ocArrayReadOnlySize value: [ | oc | oc := OrderedCollection new: 10. oc add: 1. (oc instVarAt: 1) beReadOnlyObject.
	[ oc add: 2 ] on: ModificationForbidden do: [:e | e return: oc size ] ].
run value: #wsReadOnly value: [ | ws | ws := WriteStream on: Array new. ws nextPut: 1. ws beReadOnlyObject. ws nextPut: 2 ].
run value: #wsReadOnlyInfo value: [ | ws | ws := WriteStream on: Array new. ws nextPut: 1. ws beReadOnlyObject.
	[ ws nextPut: 2 ] on: ModificationForbidden do: [:e | { e class name. e fieldIndex. e newValue. e object == ws } ] ].
run value: #wsCollReadOnly value: [ | ws | ws := WriteStream on: (Array new: 5). ws nextPut: 1. (ws instVarAt: 1) beReadOnlyObject. ws nextPut: 2 ].
run value: #wsCollReadOnlyInfo value: [ | ws | ws := WriteStream on: (Array new: 5). ws nextPut: 1. (ws instVarAt: 1) beReadOnlyObject.
	[ ws nextPut: 2 ] on: ModificationForbidden do: [:e | { e class name. e fieldIndex. e newValue. e object == (ws instVarAt: 1) } ] ].
run value: #wsCollReadOnlyPos value: [ | ws | ws := WriteStream on: (Array new: 5). ws nextPut: 1. (ws instVarAt: 1) beReadOnlyObject.
	[ ws nextPut: 2 ] on: ModificationForbidden do: [:e | e return: ws position ] ].
run value: #wsLiteralArray value: [ (WriteStream on: #(1 2 3)) nextPut: 9; contents ].
run value: #wsLiteralArrayRO value: [ #(1 2 3) isReadOnlyObject ].

"Young object stored into an old collection, then scavenges."
run value: #youngIntoOldOC value: [ | oc ok |
	oc := OrderedCollection new: 64. Smalltalk garbageCollect. Smalltalk garbageCollect.
	1 to: 20 do: [:i | oc add: (Array with: i with: i printString)].
	1 to: 6 do: [:r | Array new: 200000. 1 to: 5 do: [:k | oc add: (Array with: r with: k printString)]].
	ok := (1 to: 20) allSatisfy: [:i | (oc at: i) = (Array with: i with: i printString)].
	{ ok. oc size. oc last } ].
run value: #youngIntoOldWS value: [ | ws a ok |
	ws := WriteStream on: (Array new: 400). Smalltalk garbageCollect. Smalltalk garbageCollect.
	1 to: 6 do: [:r | Array new: 200000. 1 to: 40 do: [:k | ws nextPut: (Array with: r with: k printString)]].
	a := ws contents.
	ok := (1 to: 240) allSatisfy: [:i | | r k | r := (i - 1) // 40 + 1. k := (i - 1) \\ 40 + 1. (a at: i) = (Array with: r with: k printString)].
	{ ok. a size } ].
run value: #youngIntoOldRemembered value: [ | oc young |
	oc := OrderedCollection new: 16. Smalltalk garbageCollect. Smalltalk garbageCollect.
	young := Array with: 'young' copy.
	oc add: young. Array new: 300000. Array new: 300000.
	{ (oc at: 1) == young. (oc at: 1) first } ].

"become: forwarding the receiver."
run value: #ocBecomeForward value: [ | oc other |
	oc := OrderedCollection new. oc add: 1. other := OrderedCollection new. other add: 100.
	oc becomeForward: other. oc add: 2. { oc size. oc asArray. other size } ].
run value: #ocBecomeTwoWay value: [ | a b |
	a := OrderedCollection new. a add: 1. b := OrderedCollection new. b add: 7. b add: 8.
	a become: b. a add: 9. b add: 5. { a asArray. b asArray } ].
run value: #wsBecomeForward value: [ | ws other |
	ws := WriteStream on: (Array new: 4). ws nextPut: 1. other := WriteStream on: (Array new: 4). other nextPut: 100.
	ws becomeForward: other. ws nextPut: 2. { ws contents. other contents } ].
run value: #ocArrayBecome value: [ | oc big |
	oc := OrderedCollection new: 4. oc add: 1. big := Array new: 50. (oc instVarAt: 1) becomeForward: big. oc add: 2. oc add: 3. { oc size. oc asArray } ].

"Pinned objects."
run value: #ocPinnedArray value: [ | oc |
	oc := OrderedCollection new: 8. oc add: 1. (oc instVarAt: 1) pin. 2 to: 6 do: [:i | oc add: i]. oc asArray ].
run value: #ocPinnedArrayGrow value: [ | oc |
	oc := OrderedCollection new: 4. (oc instVarAt: 1) pin. 1 to: 40 do: [:i | oc add: i]. { oc size. oc last } ].
run value: #ocPinnedRcvr value: [ | oc | oc := OrderedCollection new. oc pin. 1 to: 30 do: [:i | oc add: i]. Smalltalk garbageCollectMost. { oc size. oc last. oc isPinned } ].
run value: #wsPinnedColl value: [ | ws | ws := WriteStream on: (Array new: 8). (ws instVarAt: 1) pin. 1 to: 20 do: [:i | ws nextPut: i]. { ws contents size. ws contents last } ].
run value: #wsPinnedRcvr value: [ | ws | ws := WriteStream on: Array new. ws pin. 1 to: 20 do: [:i | ws nextPut: i]. Smalltalk garbageCollectMost. { ws contents size. ws isPinned } ].

"Receiver state the layout does not expect."
run value: #ocNilArray value: [ | oc | oc := OrderedCollection new. oc instVarAt: 1 put: nil. [ oc add: 1 ] on: Error do: [:e | e class name ] ].
run value: #ocBadIndex value: [ | oc | oc := OrderedCollection new: 4. oc instVarAt: 3 put: 99. [ oc add: 1 ] on: Error do: [:e | e class name ] ].
run value: #wsPosPastLimit value: [ | ws | ws := WriteStream on: (Array new: 3). ws nextPut: 1; nextPut: 2; nextPut: 3; nextPut: 4. ws contents ].
run value: #wsSetToStart value: [ | ws | ws := WriteStream on: (Array new: 3). ws nextPut: 1; nextPut: 2. ws setToEnd. ws nextPut: 3. ws position: 1. ws nextPut: 8. ws contents ].
run value: #wsResetNext value: [ | ws | ws := WriteStream on: (String new). ws nextPutAll: 'abc'. ws reset. ws nextPut: $z. ws contents ].

String streamContents: [:s |
	results do: [:a | s nextPutAll: a key; nextPutAll: ' -> '; nextPutAll: a value printString; nextPut: Character lf ] ]
