"typed-stores-write-barrier.st -- typed stores into a read-only ByteArray.  The
 answer is a short text.  Stock Cog: the bytes change and no error shows (the
 last two lines of each store say 'no error' and a changed array).  Patched Cog:
 PrimitiveFailed for each store and the bytes stay zero."
| out try g |
out := WriteStream on: String new.
try := [ :block |
	[ block value. 'no error' ]
		on: Error
		do: [ :e | e class name ] ].
#( float64AtOffset:put: uint32AtOffset:put: int16AtOffset:put: uint8AtOffset:put: ) do: [ :selector |
	g := ByteArray new: 8.
	g beReadOnlyObject.
	out nextPutAll: selector; nextPutAll: ' -> '.
	out nextPutAll: (try value: [ g perform: selector with: 0 with: 3 ]); nextPutAll: ', bytes '.
	out print: g asArray; lf ].
out contents
