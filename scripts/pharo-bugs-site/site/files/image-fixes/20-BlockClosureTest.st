"BlockClosureTest: the two on:fork: tests skip themselves ('too brittle on CI').
 Remove the skip; keep every assertion."
fix value: #BlockClosureTest value: #testOnForkErrorTakesLessThanOneSecond
	value: [ :source | source includesSubstring: 'skip' ]
	value:
'testOnForkErrorTakesLessThanOneSecond
	"Test that if code runs with error, there is fork"

	| sema timeout |
	self flag: ''The following line makes the test pass under headless linux. Everywhere else this test works''.
	OSPlatform current isUnix ifTrue: [ (Delay forMilliseconds: 1) wait ].

	sema := Semaphore new.
	[ 1 / 0 ] on: Exception fork: [ sema signal ].

	timeout := sema waitTimeoutSeconds: 1.
	self assert: timeout identicalTo: false'.
fix value: #BlockClosureTest value: #testOnForkSplit
	value: [ :source | source includesSubstring: 'skip' ]
	value:
'testOnForkSplit
	"Test that when forking, the stack are split correctly (there is no any contexts referenced by both processes)"

	| sema timeout forkedContexts myContexts c |
	sema := Semaphore new.

	[ 1/0 ] on: Exception fork: [  | ctx |
		forkedContexts := IdentitySet new.
		ctx := thisContext.
		[ ctx isNotNil ] whileTrue: [ forkedContexts add: ctx. ctx := ctx sender ].
		sema signal ].

	timeout := (sema waitTimeoutSeconds: 1).
	self assert: timeout == false description: ''fork lasted more than one second''.

	myContexts := IdentitySet new.
	c := thisContext.
	[ c isNotNil ] whileTrue: [ myContexts add: c. c := c sender ].

	self assert: (myContexts noneSatisfy: [:b | forkedContexts includes: b ]) description: ''myContexts are not within forkedContexts''.
	self assert: (forkedContexts noneSatisfy: [:b | myContexts includes: b ]) description: ''forkedContexts are not within myContexts'''.
