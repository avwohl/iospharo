"filepos64.st -- file positions past 2 GB; run on our VM and on Cog; diff the two answers.
Run: scripts/cogdiff/run-cogdiff.sh scripts/cogdiff/filepos64.st (the driver gives the directory).
2026-11-13: on Windows long has 32 bits, and our VM cut a position to 32 bits in fseek and ftell.
A Soil test seeks to object index 2^36 and expects the read there to answer nil."

| d f s r |
d := (Smalltalk at: #FDIR) asFileReference.
f := d / 'pos64.bin'.
r := OrderedCollection new.
s := f binaryWriteStream.
s nextPutAll: #[1 2 3 4 5].
s position: 68719476736 * 8. r add: s position.
s position: 4294967296 + 2. r add: s position.
s position: 3. r add: s position.
s close.
s := f binaryReadStream.
s position: 5000000000. r add: s position; add: s next; add: s size.
s position: 4294967296 + 1. r add: s position; add: s next.
s position: 1. r add: s next.
s close.
f delete.
r asArray
