"ffirange.st -- integer arguments of FFI callouts at and past the limits of each C type; run on our VM and on Cog; diff the two answers.
Run: pass the text of ffirange.st as the eval argument.
Each type gets the same list of values.  The answer for a value is the result of abs or llabs, or ERR and the error class
when the VM refuses the value.  A VM that does not check the range of an argument answers a truncated number where Cog fails.
2026-11-13: found on Windows, where long has 32 bits: labs: -12345678901 failed on Cog and answered 539222987 on our VM."

| L vals types r |
(Object << #FFIRangeLib package: 'FFIRange') install.
L := Smalltalk at: #FFIRangeLib.
L class compile: 'ffiLibraryName ^ LibC'.
types := #( #int8 #uint8 #int16 #uint16 #int32 #uint32 #char #uchar #short #ushort #int #uint #long #ulong #int64 #uint64 #longlong #ulonglong #size_t ).
types do: [:t | | wide |
	wide := #( #int64 #uint64 #longlong #ulonglong #size_t ) includes: t.
	L class compile: 'call_', t, ': x ^ self ffiCall: #(', (wide ifTrue: [ OSPlatform current isWindows ifTrue: ['longlong _abs64'] ifFalse: ['longlong llabs'] ] ifFalse: ['int abs']), '(', t, ' x))' ].
vals := { 0. 1. -1. 127. 128. -128. -129. 255. 256. 32767. 32768. -32768. -32769. 65535. 65536.
	(2 raisedTo: 31) - 1. 2 raisedTo: 31. (2 raisedTo: 31) negated. (2 raisedTo: 31) negated - 1. (2 raisedTo: 32) - 1. 2 raisedTo: 32.
	(2 raisedTo: 40) + 5. ((2 raisedTo: 40) + 5) negated. (2 raisedTo: 62) - 1. 2 raisedTo: 62. (2 raisedTo: 62) negated - 1.
	(2 raisedTo: 63) - 1. 2 raisedTo: 63. (2 raisedTo: 63) negated. (2 raisedTo: 63) negated - 1. (2 raisedTo: 64) - 1. 2 raisedTo: 64. (2 raisedTo: 64) negated.
	1.5. 3/4. nil. true. $a. 'x'. #(1) }.
r := OrderedCollection new.
types do: [:t |
	r add: t -> (vals collect: [:v |
		[ L perform: ('call_', t, ':') asSymbol with: v ] on: Error do: [:e | 'ERR ', e class name ] ]) ].
r asArray
