"Write barrier for the typed integer stores into a ByteArray (2026-11-17).

The typed store primitives 615-627 fail on a read-only ByteArray on our VM.
The fallback of each accessor goes through ByteArray>>integerAt:put:size:signed:
to the named primitive primitiveFFIIntegerAtPut.  That primitive wrote the
bytes of the read-only object on our VM (fixed in the VM on 2026-11-17: the
primitive now fails).  Stock Cog writes the bytes in the primitives 615-627
themselves (docs/upstream has the Cog change).

W1. With the two VM changes the fallback ended in PrimitiveFailed.  A store
into a read-only object must signal ModificationForbidden, as at:put: does, so
that a handler can make the object writable and retry.  The retry selector is
the index-based accessor of the same size and sign.

W2, W3. WriteBarrierTest has tests for at:put:, doubleAt:put: and floatAt:put:
on a read-only ByteArray, and none for an integer store.  Two tests with the
form of testMutateByteArrayUsingDoubleAtPut."

fix value: #ByteArray value: #integerAt:put:size:signed:
	value: [ :source | (source includesSubstring: 'isReadOnlyObject') not ]
	value:
'integerAt: byteOffset put: value size: nBytes signed: aBoolean

	self isReadOnlyObject ifTrue: [
		| selectors |
		selectors := aBoolean
			ifTrue: [ #( #signedByteAt:put: #signedShortAt:put: nil #signedLongAt:put: nil nil nil #signedLongLongAt:put: ) ]
			ifFalse: [ #( #unsignedByteAt:put: #unsignedShortAt:put: nil #unsignedLongAt:put: nil nil nil #unsignedLongLongAt:put: ) ].
		^ self
			  modificationForbiddenFor: (selectors at: nBytes)
			  index: byteOffset
			  value: value ].

	^ FFIBackend current
		  integerOfObject: self
		  at: byteOffset
		  put: value
		  size: nBytes
		  signed: aBoolean'.

add value: #WriteBarrierTest value: #testMutateByteArrayUsingUnsignedLongAtPut value: 'tests - object'
	value:
'testMutateByteArrayUsingUnsignedLongAtPut
	| guineaPig |
	guineaPig := ByteArray new: 8.
	guineaPig beReadOnlyObject.

	self
		should: [ guineaPig unsignedLongAt: 1 put: 16r01020304 ]
		raise: ModificationForbidden.
	self assert: guineaPig equals: (ByteArray new: 8).

	[ guineaPig unsignedLongAt: 1 put: 16r01020304 ]
		on: ModificationForbidden
		do: [:modification |
			self assert: modification fieldIndex equals: 1.
			modification object beWritableObject.
			modification retryModification ].

	self assert: (guineaPig unsignedLongAt: 1) equals: 16r01020304'.

add value: #WriteBarrierTest value: #testMutateByteArrayUsingSignedShortAtPut value: 'tests - object'
	value:
'testMutateByteArrayUsingSignedShortAtPut
	| guineaPig |
	guineaPig := ByteArray new: 8.
	guineaPig beReadOnlyObject.

	self
		should: [ guineaPig signedShortAt: 3 put: -2 ]
		raise: ModificationForbidden.
	self assert: guineaPig equals: (ByteArray new: 8).

	[ guineaPig signedShortAt: 3 put: -2 ]
		on: ModificationForbidden
		do: [:modification |
			self assert: modification fieldIndex equals: 3.
			modification object beWritableObject.
			modification retryModification ].

	self assert: (guineaPig signedShortAt: 3) equals: -2'.
