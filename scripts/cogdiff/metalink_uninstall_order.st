"metalink_uninstall_order.st -- shows the MetaLink uninstall defect of the
 Pharo 14 image (scripts/image-fixes/33-metalink-uninstall-order.st) on any VM.

   VMRUN_IMAGE=<image without fix 33> scripts/vmrun.sh eval cog  scripts/cogdiff/metalink_uninstall_order.st
   VMRUN_IMAGE=<image without fix 33> scripts/vmrun.sh eval ours scripts/cogdiff/metalink_uninstall_order.st

 The answer is a text.  The image must have the Reflectivity tests
 (ReflectivityExamples2).

 Measured 2026-11-17, Pharo 14 build 791 with no repair:
   PART A, stock Cog and our VM: P1 leaves 1 node in the mapper, and the
     uninstall of P3 gives KeyNotFound #methodWithTempVarAccess.
   PART B, 300 rounds: stock Cog 12 rounds with the KeyNotFound and 332
     uninstalls that left a node in the mapper; our VM 9 rounds and 368.

 PART A is deterministic.  It does not depend on identity hashes.  It replaces
 the IdentitySet 'nodes' of the link with a subclass whose elements all have the
 same home slot, and runs the three install and uninstall rounds of
 LinkInstallerTest>>testLinkOnTempVarForObject with the nodes installed one by one.
   round 1: node set with home slot 1 (the second node collides with the first).
   round 2: node set with home slot 'array size' (the nodes leave in order
            without a skip) -- this removes the method of the anonymous class.
   round 3: the same reads as round 1 again.
 Before the repair the log has, in this order:
   P1 uninstall: ok
   P1 nodes left in the mapper after uninstall: 1 (a correct uninstall leaves 0)
   P2 uninstall: ok
   P3 uninstall: KeyNotFound: ...#methodWithTempVarAccess not found in MethodDictionary
 After the repair (scripts/image-fixes entry for
 MetaLinkInstaller>>uninstallAll...) every 'mapper' line says 0 and there is no
 KeyNotFound.  If P1 shows 0 left in the mapper before the repair, the skip
 theory is wrong for this image (then read the 'P1 install' line for an error).

 PART B uses the natural identity hashes: 300 rounds of the full test body
 (read, write, all) with a fresh installer for each round, as setUp does.  It
 counts the uninstalls that left a node in the mapper ('skipped') and the
 failures.  The rates depend on the hash sequence of the VM and so differ from run
 to run on our VM.  Simulated with random hashes: about 16 percent of the
 two-node uninstalls skip a node, about 5 to 6 percent of the rounds end in a
 KeyNotFound (in the uninstall or in the install).  Stock Cog must show the same
 kind of counts; a count of 0 skipped in 300 rounds on one VM is the evidence
 against the theory for that VM.

 The script changes the global MetaLink installer and leaves it empty at the end."
| makeSet sorted link obj temp reads writes mapper log step stale result
  counts rounds skipped failedAt phase l o t opt note |
log := OrderedCollection new.

"A set whose elements all have the home slot that homeSource names."
makeSet := [ :homeSource | | setClass |
	setClass := IdentitySet newAnonymousSubclass.
	setClass compile: 'scanFor: anObject
	| index start |
	index := start := ', homeSource, '.
	[ | element |
		((element := array at: index) == nil or: [ element enclosedElement == anObject ])
			ifTrue: [ ^ index ].
		(index := index \\ array size + 1) = start ] whileFalse.
	self errorNoFreeSpace'.
	setClass new ].

sorted := [ :coll | (coll asSortedCollection: [ :a :b | a start <= b start ]) asArray ].

step := [ :name :aBlock |
	[ aBlock value. log add: name , ': ok' ]
		on: Error
		do: [ :e | log add: name , ': ' , e class name , ': ' , (e messageText ifNil: [ '' ]) asString ] ].

"---------------- PART A ----------------"
log add: '== PART A: forced layout of the node set'.
[ MetaLink instVarNamed: #linkInstaller put: nil.
link := MetaLink new.
obj := ReflectivityExamples2 new.
temp := (ReflectivityExamples2 >> #methodWithTempVarAccess) lookupVar: #temp.
reads := sorted value: (temp accessingNodesFor: #read).
writes := sorted value: (temp accessingNodesFor: #write).
log add: 'reads: ' , reads size printString , '  writes: ' , writes size printString , '  (expected 2 and 2)'.
mapper := MetaLink linkInstaller linkToNodesMapper.
stale := [ (mapper findNodesForObject: obj) size ].

link instVarNamed: #nodes put: (makeSet value: '1').
step value: 'P1 install' value: [ reads do: [ :n | n link: link forObject: obj ] ].
step value: 'P1 uninstall' value: [ link uninstall ].
log add: 'P1 nodes left in the mapper after uninstall: ' , stale value printString , ' (a correct uninstall leaves 0)'.

link instVarNamed: #nodes put: (makeSet value: 'array size').
step value: 'P2 install' value: [ writes do: [ :n | n link: link forObject: obj ] ].
step value: 'P2 uninstall' value: [ link uninstall ].
log add: 'P2 nodes left in the mapper after uninstall: ' , stale value printString , ' (0 expected; 1 is the old node of P1)'.

link instVarNamed: #nodes put: (makeSet value: 'array size').
step value: 'P3 install' value: [ reads do: [ :n | n link: link forObject: obj ] ].
step value: 'P3 uninstall' value: [ link uninstall ].
log add: 'P3 nodes left in the mapper after uninstall: ' , stale value printString ]
	on: Error
	do: [ :e | log add: 'PART A aborted: ' , e class name , ': ' , (e messageText ifNil: [ '' ]) asString ].
[ link instVarNamed: #nodes put: IdentitySet new ] on: Error do: [ :e | ].

"---------------- PART B ----------------"
log add: '== PART B: natural identity hashes, 300 rounds of the test body'.
counts := Dictionary new.
rounds := 300.
skipped := 0.
1 to: rounds do: [ :round |
	[ MetaLink linkInstaller unsubscribeFromAnnouncers ] on: Error do: [ :e | ].
	MetaLink instVarNamed: #linkInstaller put: nil.
	l := MetaLink new.
	o := ReflectivityExamples2 new.
	failedAt := nil.
	phase := ''.
	[ #(#read #write #all) do: [ :each |
			opt := each.
			phase := opt , ' install'.
			t := (ReflectivityExamples2 >> #methodWithTempVarAccess) lookupVar: #temp.
			l installOnVariable: t for: o option: opt instanceSpecific: false.
			phase := opt , ' uninstall'.
			l uninstall.
			(MetaLink linkInstaller linkToNodesMapper findNodesForObject: o) isEmpty
				ifFalse: [ skipped := skipped + 1.
					counts at: 'skipped, ' , opt put: (counts at: 'skipped, ' , opt ifAbsent: [ 0 ]) + 1 ] ] ]
		on: Error
		do: [ :e |
			failedAt := phase , ' ' , e class name , ' ' , (e messageText ifNil: [ '' ]) asString.
			"make the later rounds independent of this failed link"
			[ l instVarNamed: #nodes put: IdentitySet new ] on: Error do: [ :e2 | ] ].
	counts at: (failedAt ifNil: [ 'ok' ]) put: (counts at: (failedAt ifNil: [ 'ok' ]) ifAbsent: [ 0 ]) + 1 ].
counts keys asSortedCollection do: [ :k | log add: k , ' : ' , (counts at: k) printString ].
log add: 'uninstalls that left a node in the mapper: ' , skipped printString.
[ MetaLink instVarNamed: #linkInstaller put: nil ] on: Error do: [ :e | ].

String streamContents: [ :s | log do: [ :line | s nextPutAll: line; lf ] ]
