"newfile_plugin_ours_checks.st -- NewFilePlugin checks for OUR VM only.

   VMRUN_IMAGE=<image with the package ronsaldo/pharo-newfile> scripts/vmrun.sh eval ours scripts/cogdiff/newfile_plugin_ours_checks.st

 Do not expect an answer from stock Cog for these checks:

 H1  A copy of the handle of a closed file, given to the close primitive.
     The stock plugin keeps a C pointer in the handle and frees the pointer
     a second time.  Measured 2026-11-17: stock Cog v12.0.6 stops with
     'Abort trap: 6' in primitiveClose:.  Our VM keeps an id in the handle
     that is never used again, and fails the primitive.
 M5  A second map after the file grew.  The stock plugin answers the old
     mapping, and the image gives the view the new size (read from the stock
     sources, not run: a read past the old mapping is outside the mapping).
     Our VM answers no mapping.
 M6  A read-write map after a read-only map of the same file.  The stock
     plugin answers the read-only mapping (read from the stock sources, not
     run).  Our VM answers no mapping.

 Measured 2026-11-17 on the Cog build with the change of
 docs/upstream/cog-newfile-plugin-buffer-and-handle-checks.patch: no stop of
 the VM; H1 {#sameValue->0. #rejected->0} (a token that is not live acts as a
 NULL handle, so the close does nothing); M5, M6 and M7 as our VM.

 Measured 2026-11-17 on our VM:
   H1 a copy of a closed handle against a new file, 20 rounds: {#sameValue->0. #rejected->20}
   M5 second map after the file grew: #(10 nil)
   M6 read-write map after a read-only map: #(false nil)
   M7 read-only map after a read-write map, then a write through the first view: #(true true 9)"
| log dir path check f v h g |
log := OrderedCollection new.
check := [ :name :aBlock | | line |
	line := name , ': ' , ([ aBlock value printString ]
		on: Error
		do: [ :e | 'ERROR ' , e class name ]).
	log add: line.
	(FileSystem workingDirectory / 'nfp_progress.txt') writeStreamDo: [ :s | s setToEnd; nextPutAll: line; lf ] ].
dir := 'nfp-ours-' , Time millisecondClockValue printString.
path := [ :name | dir , '/' , name ].
check value: 'D1 create directory' value: [ NewDirectory create: dir ].

check value: 'H1 a copy of a closed handle against a new file, 20 rounds' value: [
	| same rejected |
	same := 0. rejected := 0.
	1 to: 20 do: [ :i |
		f := NewFile openCreateAlways: (path value: 'h1a.bin').
		h := (f instVarNamed: #handle) copy.
		f close.
		g := NewFile openCreateAlways: (path value: 'h1b.bin').
		h = (g instVarNamed: #handle) ifTrue: [ same := same + 1 ].
		[ g primitiveClose: h ] on: Error do: [ :e | rejected := rejected + 1 ].
		g size = 0 ifFalse: [ same := same + 1000 ].
		g close ].
	{ #sameValue -> same. #rejected -> rejected } ].

check value: 'M5 second map after the file grew' value: [
	| v2 firstSize |
	f := NewFile openCreateAlways: (path value: 'm5.bin').
	f writeFrom: (ByteArray new: 10).
	v := f readWriteMemoryMap.
	firstSize := v size.
	f truncateToSize: 1000000.
	v2 := f readWriteMemoryMap.
	v unmap.
	f close.
	{ firstSize. v2 } ].
check value: 'M6 read-write map after a read-only map' value: [
	| v2 |
	f := NewFile openCreateAlways: (path value: 'm6.bin').
	f writeFrom: #[ 1 2 3 4 ].
	v := f readOnlyMemoryMap.
	v2 := f readWriteMemoryMap.
	v unmap.
	f close.
	{ v hasWritePermission. v2 } ].
check value: 'M7 read-only map after a read-write map, then a write through the first view' value: [
	| v2 r |
	f := NewFile openCreateAlways: (path value: 'm7.bin').
	f writeFrom: #[ 1 2 3 4 ].
	v := f readWriteMemoryMap.
	v2 := f readOnlyMemoryMap.
	v address unsignedByteAt: 1 put: 9.
	r := { v2 notNil. v2 address = v address. (v2 address unsignedByteAt: 1) }.
	v unmap. v2 unmap.
	f close.
	r ].
check value: 'Z1 delete the files' value: [
	(#( 'h1a.bin' 'h1b.bin' 'm5.bin' 'm6.bin' 'm7.bin' ) collect: [ :each | NewFile deleteFile: (path value: each) ]) asArray ].
check value: 'Z2 remove the empty directory' value: [ NewDirectory removeEmpty: dir ].

String streamContents: [ :s | log do: [ :each | s nextPutAll: each ] separatedBy: [ s lf ] ]
