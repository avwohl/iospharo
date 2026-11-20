"newfile_plugin_checks.st -- the NewFilePlugin primitives, one line for each
 check, to compare our VM with stock Cog.

   VMRUN_IMAGE=<image with the package ronsaldo/pharo-newfile> scripts/vmrun.sh eval cog  scripts/cogdiff/newfile_plugin_checks.st
   VMRUN_IMAGE=<the same image>                                scripts/vmrun.sh eval ours scripts/cogdiff/newfile_plugin_checks.st

 The image of row 50 of the package run has the package
 (<work directory> with KEEP_IMAGES=all).

 The checks are safe on the two VMs.  The checks of
 newfile_plugin_ours_checks.st are for our VM only: the stock plugin stops the
 VM there, or answers a mapping that the image then reads or writes outside
 its limits.

 The tests of the package reach 14 of the 20 primitives.  This script adds the
 directory primitives, closed handles, buffers that are not ByteArrays, and
 wrong arguments.

 Each line goes to the file nfp_progress.txt of the working directory when
 its check ends, so a VM that stops shows the last check that ended.

 Measured 2026-11-17 (stock Cog v12.0.6 and our VM, 36 lines): 33 lines are
 the same.  The 3 others:
   F4  read 3 bytes into a ByteArray of 2 bytes
         stock Cog: 3 (the plugin writes past the buffer)
         our VM:    ERROR PrimitiveFailed
   F11 read into an Array
         stock Cog: 2 (the plugin writes bytes into pointer slots)
         our VM:    ERROR PrimitiveFailed
   F12 read into a read-only ByteArray
         stock Cog: 2
         our VM:    ERROR PrimitiveFailed"
| log dir path check f v d buf names |
log := OrderedCollection new.
check := [ :name :aBlock | | line |
	line := name , ': ' , ([ aBlock value printString ]
		on: Error
		do: [ :e | 'ERROR ' , e class name ]).
	log add: line.
	(FileSystem workingDirectory / 'nfp_progress.txt') writeStreamDo: [ :s | s setToEnd; nextPutAll: line; lf ] ].
dir := 'nfp-checks-' , (Smalltalk vm class name) , '-' , Time millisecondClockValue printString.
path := [ :name | dir , '/' , name ].

check value: 'D1 create directory' value: [ NewDirectory create: dir ].
check value: 'D2 create directory again' value: [ NewDirectory create: dir ].
check value: 'D3 open absent directory' value: [ NewDirectory open: dir , '/absent' ].
check value: 'D4 contents of empty directory' value: [ (NewDirectory contentsOf: dir) asSortedCollection asArray ].
check value: 'D5 three files' value: [
	#( 'a.txt' 'b.txt' 'c.txt' ) do: [ :each | (NewFile openCreateAlways: (path value: each)) close ].
	(NewDirectory contentsOf: dir) asSortedCollection asArray ].
check value: 'D6 next, rewind, next' value: [
	d := NewDirectory open: dir.
	names := OrderedCollection new.
	names add: d next notNil.
	d rewind.
	names add: d contents size.
	d rewind.
	names add: d contents size.
	d close.
	names asArray ].
check value: 'D7 next after close' value: [ d next ].
check value: 'D8 rewind after close' value: [ d rewind. #done ].
check value: 'D9 close after close' value: [ d close. #done ].
check value: 'D10 remove directory that is not empty' value: [ NewDirectory removeEmpty: dir ].

check value: 'F1 write 4 bytes, size, tell' value: [
	f := NewFile openCreateAlways: (path value: 'f1.bin').
	{ f writeFrom: #[ 1 2 3 4 ]. f size. f tell } ].
check value: 'F2 read at offset 1' value: [
	buf := ByteArray new: 3.
	{ f readInto: buf at: 1. buf } ].
check value: 'F3 read past the end' value: [
	buf := ByteArray new: 3.
	{ f readInto: buf at: 100. buf } ].
check value: 'F4 read more than the buffer has' value: [
	buf := ByteArray new: 2.
	f readInto: buf offset: 0 withSize: 3 at: 0 ].
check value: 'F5 read with a negative buffer offset' value: [
	buf := ByteArray new: 4.
	f readInto: buf offset: -1 withSize: 2 at: 0 ].
check value: 'F6 truncate to 2, size' value: [ { f truncateToSize: 2. f size } ].
check value: 'F7 seek to 1, tell' value: [ f position: 1. f tell ].
check value: 'F8 write a WordArray, size' value: [
	f position: 0.
	{ f writeFrom: (WordArray with: 16r04030201 with: 16r08070605). f size } ].
check value: 'F9 read into a WordArray' value: [
	buf := WordArray new: 2.
	{ f readInto: buf at: 0. buf first printStringHex. buf last printStringHex } ].
check value: 'F10 read into a 3-element DoubleByteArray' value: [
	buf := DoubleByteArray new: 3.
	{ f readInto: buf at: 0. buf asArray } ].
check value: 'F11 read into an Array' value: [
	buf := Array new: 2.
	f readInto: buf at: 0 ].
check value: 'F12 read into a literal ByteArray' value: [
	buf := #[ 0 0 0 0 ].
	buf beReadOnlyObject.
	[ f readInto: buf at: 0 ] ensure: [ buf beWritableObject ] ].
check value: 'F13 close, then size, tell, truncate' value: [
	f close.
	{ f size. f tell. f truncateToSize: 1 } ].
check value: 'F14 read and write after close' value: [
	buf := ByteArray new: 2.
	{ f readInto: buf at: 0. f writeFrom: #[ 9 ] } ].
check value: 'F15 close after close' value: [ f close. #done ].

check value: 'O1 open absent file read-only' value: [ NewFile openReadOnly: (path value: 'absent.bin') ].
check value: 'O2 open existing file that is absent' value: [ NewFile openExisting: (path value: 'absent.bin') ].
check value: 'O3 create new on an existing file' value: [ NewFile openCreateNew: (path value: 'f1.bin') ].
check value: 'O4 delete absent file' value: [ NewFile deleteFile: (path value: 'absent.bin') ].

check value: 'M1 map, read through the view, unmap' value: [
	f := NewFile openCreateAlways: (path value: 'm1.bin').
	f writeFrom: #[ 11 22 33 44 ].
	v := f readOnlyMemoryMap.
	names := { v size. v address isNull. (v address unsignedByteAt: 2) }.
	v unmap.
	f close.
	names ].
check value: 'M2 map an empty file' value: [
	f := NewFile openCreateAlways: (path value: 'm2.bin').
	v := f readOnlyMemoryMap.
	f close.
	v ].
check value: 'M3 close, then unmap' value: [
	f := NewFile openCreateAlways: (path value: 'm3.bin').
	f writeFrom: #[ 1 2 3 4 ].
	v := f readOnlyMemoryMap.
	f close.
	v unmap.
	#done ].
check value: 'M4 two maps of one file, two unmaps' value: [
	| v2 |
	f := NewFile openCreateAlways: (path value: 'm4.bin').
	f writeFrom: #[ 5 6 7 8 ].
	v := f readOnlyMemoryMap.
	v2 := f readOnlyMemoryMap.
	names := { v address = v2 address. (v2 address unsignedByteAt: 4) }.
	v unmap. v2 unmap.
	f close.
	names ].

check value: 'Z1 delete the files' value: [
	(#( 'a.txt' 'b.txt' 'c.txt' 'f1.bin' 'm1.bin' 'm2.bin' 'm3.bin' 'm4.bin' )
		collect: [ :each | NewFile deleteFile: (path value: each) ]) asArray ].
check value: 'Z2 remove the empty directory' value: [ NewDirectory removeEmpty: dir ].
check value: 'Z3 remove the directory again' value: [ NewDirectory removeEmpty: dir ].

String streamContents: [ :s | log do: [ :each | s nextPutAll: each ] separatedBy: [ s lf ] ]
