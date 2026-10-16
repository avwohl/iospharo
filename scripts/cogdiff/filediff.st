"filediff.st -- file-system scenarios for a diff against Cog.  Replace
(Smalltalk at: #FDIR) with a fresh directory path per VM.  2026-10-17: all 23 agree."

| results run d |
d := (Smalltalk at: #FDIR) asFileReference. d ensureDeleteAll. d ensureCreateDirectory.
results := OrderedCollection new.
run := [:name :blk | results add: name -> ([ blk value ] on: Error do: [:e | 'ERR ', e class name ]) ].
run value: #writeRead value: [ (d / 'a.txt') writeStreamDo: [:s | s nextPutAll: 'hello'; cr; nextPutAll: 'world' ]. (d / 'a.txt') contents ].
run value: #size value: [ (d / 'a.txt') size ].
run value: #binary value: [ (d / 'b.bin') binaryWriteStreamDo: [:s | s nextPutAll: #[0 1 2 255 254] ]. (d / 'b.bin') binaryReadStreamDo: [:s | s upToEnd ] ].
run value: #position value: [ (d / 'b.bin') binaryReadStreamDo: [:s | s skip: 2. { s position. s next. s atEnd. s position: 4. s next. s atEnd. s next } ] ].
run value: #append value: [ (d / 'a.txt') writeStreamDo: [:s | s setToEnd. s nextPutAll: '!' ]. (d / 'a.txt') contents size ].
run value: #truncate value: [ (d / 'a.txt') writeStreamDo: [:s | s truncate. s nextPutAll: 'x' ]. (d / 'a.txt') contents ].
run value: #missingRead value: [ (d / 'nope.txt') contents ].
run value: #missingSize value: [ (d / 'nope.txt') size ].
run value: #missingDelete value: [ (d / 'nope.txt') delete ].
run value: #ensureDeleteMissing value: [ (d / 'nope.txt') ensureDelete. #ok ].
run value: #dirAsFile value: [ d contents ].
run value: #exists value: [ { (d / 'a.txt') exists. (d / 'nope') exists. d exists. d isDirectory. (d / 'a.txt') isFile } ].
run value: #rename value: [ (d / 'a.txt') renameTo: 'c.txt'. { (d / 'a.txt') exists. (d / 'c.txt') exists } ].
run value: #children value: [ (d children collect: [:f | f basename ]) asSortedCollection asArray ].
run value: #mkdirTwice value: [ (d / 'sub') createDirectory. (d / 'sub') createDirectory ].
run value: #deleteNonEmpty value: [ (d / 'sub' / 'f') writeStreamDo: [:s | s nextPut: $q ]. (d / 'sub') delete ].
run value: #copy value: [ (d / 'b.bin') copyTo: (d / 'b2.bin'). (d / 'b2.bin') binaryReadStreamDo: [:s | s upToEnd ] ].
run value: #utf8 value: [ (d / 'u.txt') writeStreamDo: [:s | s nextPutAll: 'é€😀' ]. { (d / 'u.txt') size. (d / 'u.txt') contents size. (d / 'u.txt') contents } ].
run value: #readPastEnd value: [ (d / 'b.bin') binaryReadStreamDo: [:s | s skip: 10. { s atEnd. s next. s position } ] ].
run value: #writeToDirPath value: [ d writeStreamDo: [:s | s nextPut: $x ] ].
run value: #emptyFile value: [ (d / 'e') writeStreamDo: [:s | ]. { (d / 'e') size. (d / 'e') contents } ].
run value: #bigFile value: [ (d / 'big') binaryWriteStreamDo: [:s | 1 to: 300000 do: [:i | s nextPut: i \\ 256 ] ]. (d / 'big') binaryReadStreamDo: [:s | s skip: 123456. { s next. s next. (d / 'big') size } ] ].
run value: #fileStreamPrims value: [ | f | f := File named: (d / 'raw') fullName. f writeStreamDo: [:s | s nextPutAll: #[65 66 67] ]. f readStreamDo: [:s | { s next: 2. s size } ] ].
d ensureDeleteAll.
results asArray
