"utf8path.st -- file and directory names outside the ANSI code page: e-acute,
o-diaeresis, u-macron, a CJK character and an emoji.  The names are built from
code points so that this file stays ASCII.  The image passes every path to the
VM as UTF-8; the VM must use the wide Win32 API for them, as stock Cog does."

| results run d names nm sub |
d := (Smalltalk at: #FDIR) asFileReference. d ensureDeleteAll. d ensureCreateDirectory.
results := OrderedCollection new.
run := [:name :blk | results add: name -> ([ blk value ] on: Error do: [:e | 'ERR ', e class name ]) ].
nm := [:cp | 'n', (String with: (Character value: cp)), 'x' ].
names := #(233 246 363 20013 128512) collect: [:cp | nm value: cp ].
names := names copyWith: (nm value: 233), (nm value: 20013), (nm value: 128512).
run value: #nameSizes value: [ names collect: [:n | n size ] ].
run value: #mkdir value: [ names do: [:n | (d / ('D', n)) createDirectory ]. names collect: [:n | { (d / ('D', n)) exists. (d / ('D', n)) isDirectory. (d / ('D', n)) isFile } ] ].
run value: #writeRead value: [ names do: [:n | (d / ('D', n) / ('f', n, '.txt')) writeStreamDo: [:s | s nextPutAll: 'content ', n ] ].
    names collect: [:n | (d / ('D', n) / ('f', n, '.txt')) contents ] ].
run value: #exists value: [ names collect: [:n | { (d / ('D', n) / ('f', n, '.txt')) exists. (d / ('D', n) / ('f', n, '.txt')) isFile. (d / ('D', n) / ('g', n)) exists } ] ].
run value: #size value: [ names collect: [:n | (d / ('D', n) / ('f', n, '.txt')) size ] ].
run value: #binary value: [ names collect: [:n | (d / ('b', n)) binaryWriteStreamDo: [:s | s nextPutAll: #[1 2 3 250] ]. (d / ('b', n)) binaryReadStreamDo: [:s | s upToEnd ] ] ].
run value: #topChildren value: [ (d children collect: [:f | f basename ]) asSortedCollection asArray collect: [:n | n asByteArray ] ].
run value: #subChildren value: [ names collect: [:n | ((d / ('D', n)) children collect: [:f | f basename asByteArray ]) asSortedCollection: [:a :b | a asString <= b asString ] ] ].
run value: #entries value: [ ((d / ('D', (names at: 4))) entries collect: [:e | { e name asByteArray. e size. e isDirectory } ]) asArray ].
run value: #rename value: [ | a b |
    a := d / ('D', (names at: 1)) / ('f', (names at: 1), '.txt').
    b := 'r', (names at: 3), '.txt'.
    a renameTo: b.
    { a exists. (d / ('D', (names at: 1)) / b) exists. (d / ('D', (names at: 1)) / b) contents } ].
run value: #renameDir value: [ | a |
    a := d / ('D', (names at: 2)).
    a renameTo: 'R', (names at: 5).
    { a exists. (d / ('R', (names at: 5))) exists. (d / ('R', (names at: 5))) isDirectory. (d / ('R', (names at: 5))) children size } ].
run value: #copy value: [ (d / ('b', (names at: 3))) copyTo: (d / ('c', (names at: 4))). (d / ('c', (names at: 4))) binaryReadStreamDo: [:s | s upToEnd ] ].
run value: #copyDirFile value: [ (d / ('b', (names at: 5))) copyTo: (d / ('D', (names at: 3)) / ('c', (names at: 5))). (d / ('D', (names at: 3)) / ('c', (names at: 5))) size ].
run value: #delete value: [ (d / ('b', (names at: 1))) delete. { (d / ('b', (names at: 1))) exists. (d / ('b', (names at: 2))) exists } ].
run value: #deleteNonEmpty value: [ (d / ('D', (names at: 3))) delete ].
run value: #deleteDir value: [ (d / 'E', (names at: 4)) createDirectory. (d / 'E', (names at: 4)) delete. (d / 'E', (names at: 4)) exists ].
run value: #nested value: [ sub := d / ('N', (names at: 5)) / ('M', (names at: 4)) / ('K', (names at: 3)). sub ensureCreateDirectory.
    (sub / ('z', (names at: 1))) writeStreamDo: [:s | s nextPutAll: 'deep' ].
    { sub exists. (sub / ('z', (names at: 1))) contents. sub parent parent exists } ].
run value: #fileStreamPrims value: [ | f | f := File named: (d / ('raw', (names at: 5))) fullName. f writeStreamDo: [:s | s nextPutAll: #[65 66 67] ]. { f exists. f size. f readStreamDo: [:s | s next: 2 ] } ].
run value: #ensureDeleteAll value: [ (d / ('N', (names at: 5))) ensureDeleteAll. (d / ('N', (names at: 5))) exists ].
run value: #relativeAfterCwd value: [ d exists ].
run value: #parentPath value: [ (d / ('D', (names at: 4)) / 'q') parent basename asByteArray ].
d ensureDeleteAll.
results asArray
