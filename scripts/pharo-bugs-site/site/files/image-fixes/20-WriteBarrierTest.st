fix value: #TFFIBackend value: #'on:float64At:put:'
	value: [ :src | src includesSubstring: 'self isReadOnlyObject' ]
	value: 'on: anObject float64At: offset put: value

	value isFloat ifFalse: [
		^ anObject float64AtOffset: offset - 1 put: value asFloat ].

	anObject isReadOnlyObject ifTrue: [
		^ anObject
			  modificationForbiddenFor: #doubleAt:put:
			  index: offset
			  value: value ].

	^ self primitiveFailed'.
fix value: #TFFIBackend value: #'on:float32At:put:'
	value: [ :src | src includesSubstring: 'self isReadOnlyObject' ]
	value: 'on: anObject float32At: offset put: value

	value isFloat ifFalse: [
		^ anObject float32AtOffset: offset - 1 put: value asFloat ].

	anObject isReadOnlyObject ifTrue: [
		^ anObject
			  modificationForbiddenFor: #floatAt:put:
			  index: offset
			  value: value ].

	^ self primitiveFailed'.
fix value: #WriteBarrierTest value: #testMutateByteArrayUsingDoubleAtPut
	value: [ :src | src includesSubstring: 'expectedFailure' ]
	value: 'testMutateByteArrayUsingDoubleAtPut
	| guineaPig |
	guineaPig := ByteArray new: 8.
	guineaPig beReadOnlyObject.

	self
		should: [ guineaPig doubleAt: 1 put: (2 raisedTo: 65) asFloat ]
		raise: ModificationForbidden.

	[ guineaPig doubleAt: 1 put: (2 raisedTo: 65) asFloat ]
		on: ModificationForbidden
		do: [:modification |
			self assert: modification fieldIndex equals: 1.
			modification object beWritableObject.
			modification retryModification ].

	self assert: (guineaPig doubleAt: 1) equals: (2 raisedTo: 65) asFloat'.
fix value: #WriteBarrierTest value: #testMutateByteArrayUsingFloatAtPut
	value: [ :src | src includesSubstring: 'expectedFailure' ]
	value: 'testMutateByteArrayUsingFloatAtPut
	| guineaPig |
	guineaPig := ByteArray new: 5.
	guineaPig beReadOnlyObject.

	self
		should: [ guineaPig floatAt: 1 put: 1.0  ]
		raise: ModificationForbidden.

	[ guineaPig floatAt: 1 put: 1.0 ]
		on: ModificationForbidden
		do: [:modification |
			self assert: modification fieldIndex equals: 1.
			modification object beWritableObject.
			modification retryModification ].

	self assert: (guineaPig floatAt: 1) equals: 1.0'.
