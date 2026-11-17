fix value: #FFICallbackParametersTest value: #testIdentityStruct
	value: [ :s | s includesSubstring: 'true ifTrue: [ ^ self skip. ]' ]
	value: 'testIdentityStruct

	| param result |
	callback := FFICallback
		signature: #(FFITestStructureSmallIntFloatStructure (FFITestStructureSmallIntFloatStructure a))
		block: [ :a |
			self assert: a x equals: 2.0.
			self assert: a y equals: 3.
			self assert: a w equals: 5.0.
			self assert: a z equals: 7.0.
			a x: 3.0.
			a y: 4.
			a w: 6.0.
			a z: 8.0.
			a
		].
	param := FFITestStructureSmallIntFloatStructure new
		x: 2.0;
		y: 3;
		w: 5.0;
		z: 7;
		yourself.
	result := self callCallback: callback withArgs: { param }.
	self deny: param equals: result.
	self assert: result x equals: 3.0.
	self assert: result y equals: 4.
	self assert: result w equals: 6.0.
	self assert: result z equals: 8.0.'.
fix value: #TFCallbacksTest value: #testCallbackNotRespectingLIFOOrderFailsReturn
	value: [ :s | s includesSubstring: 'self skip' ]
	value: 'testCallbackNotRespectingLIFOOrderFailsReturn

	| smain s1 c1 s2 c2 returned |
	smain := Semaphore new.
	returned := false.
	s1 := Semaphore new.
	c1 := self newTestCallbackDoing: [ smain signal. s1 wait ].
	s2 := Semaphore new.
	c2 := self newTestCallbackDoing: [ smain signal. s2 wait ].
	[ self callCallback: c1. returned := true ] fork.
	[ self callCallback: c2 ] fork.
	smain wait; wait.
	s1 signal.
	100 milliSecond wait.
	self deny: returned.
	s2 signal.
	100 milliSecond wait.
	self assert: returned'.
fix value: #TFUFFIFunctionCallTest value: #testLongCalloutDoesNotBlockImage
	value: [ :s | s includesSubstring: 'self skip' ]
	value: 'testLongCalloutDoesNotBlockImage

	"Tricky test.
	This test checks what a callout does to the other Pharo processes.
	A 1 second long callout runs in a separate Pharo process.  When that process starts (signals calloutEnter), the test process waits 0.3 seconds and then notes #waitingForCallout.
	With a worker runner the callout runs in another thread, so the image is not suspended: #waitingForCallout comes before #finishedCallout.
	TFSameThreadRunner runs the callout in the VM thread, so the whole image is suspended for the 1 second: #finishedCallout comes first.  The test checks the order that belongs to the runner."
	| calloutEnter calloutExit orderOfFinishing expectedFirst |
	expectedFirst := self ffiLibrary = TFTestLibraryUsingSameThreadRunner
		ifTrue: [ #finishedCallout ]
		ifFalse: [ #waitingForCallout ].
	"The first call of an ffiCall: method compiles the callout.  That takes tens of milliseconds, during which a higher priority process can preempt the callout process and let the test process run first.  Call once with 0 seconds, so the compile is done before the timed part."
	self longCallout: 0.
	calloutEnter := Semaphore new.
	calloutExit := Semaphore new.
	orderOfFinishing := OrderedCollection new.
	[
		calloutEnter signal.
		self longCallout: 1.
		orderOfFinishing add: #finishedCallout.
		calloutExit signal ] fork.
	calloutEnter wait.
	0.3 seconds wait.
	orderOfFinishing add: #waitingForCallout.
	calloutExit wait.

	self assert: orderOfFinishing first equals: expectedFirst'.
