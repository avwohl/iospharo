"Debugger-kernel-skips.

1. OnlineSourceDebugInfo>>shouldStopAt: stopped at every send, store, jump and
closure-creation bytecode, the ones that the compiler inserts included (the
comments of isSourceSend:, isSourceStore: and isSourceBlockCreation: say 'for
now').  DebugInfoTest has three tests of the intended behavior
(testNotStopAtAllButFirstBytecodeOfOptimizedSend, testNotStopAtConstantBlockCreation,
testNotStopAtFullBlockCreation); they said 'self skip' until shouldStopAt: no
longer stops at compiler-inserted stores and closure creations.  The fix:
shouldStopAt: stops at every return (the compiler inserts them, the debugger
must stop there) and at the FIRST bytecode of each message or assignment node
of the source.  Bytecodes that the compiler adds around such a node (the store
of the loop variable, the comparison, the increment and the jumps of an inlined
to:do:, the creation of a closure) are no stop points.  The three tests lose
the skip and their 'only valid when' comment; the assertions are unchanged.

The debugger (DebugSession>>stepToFirstInterestingBytecodeInContext:) is the
only sender of shouldStopAt:, and the Sindarin debugger (an external package,
SindarinDebuggerTest) steps over exactly the old stop points: with the strict
set 11 SindarinDebuggerTest tests that pass now would fail (testMoveToNode...
BlockHasBeenCreated, testSkipAssignment..., testSkipUpToNodeStopsOnReturnNodes
and others; their names say block creation is a step).  So the old stop set
stays available, under its own name shouldStepStopAt:, and DebugSession uses
that.  Moving DebugSession to the strict set needs the Sindarin package to be
changed first.

2. CodeSimulationTest>>testErrorCodeNotFound said 'self skip'.  It checks that
the simulation of a named primitive that does not exist gives the same error
code as the real primitive (#'not found').  The skip is removed.  It passes on
our VM with the VM change of the report (Primitives.cpp, primitiveExternalCall
and primitiveDoNamedPrimitiveWithArgs) and needs the Cog change of the report
to pass on stock Cog."

fix value: #OnlineSourceDebugInfo value: #shouldStopAt: value: [ :src | (src includesSubstring: 'isSourceBlockCreation:') ]
	value: 'shouldStopAt: aBytecodePC

	"Stop at all returns (the compiler inserts some, and the debugger has to stop there)
	 and at the first bytecode of each message or assignment node of the source.
	 The other bytecodes of the expansion of such a node are compiler-inserted and are
	 not stop points: the store of the loop variable, the comparison, the increment and
	 the jumps of an inlined to:do:, the closure creation, and the pushes of copied variables."
	| node |
	(self isSourceReturn: aBytecodePC) ifTrue: [ ^ true ].
	node := self privateSourceNodeForPC: aBytecodePC.
	node ifNil: [ ^ false ].
	(node isMessage or: [ node isAssignment ]) ifFalse: [ ^ false ].
	^ (bytecodeToASTCache firstBcOffsetForNode: node) = aBytecodePC'.

fix value: #DebugInfoTest value: #testNotStopAtAllButFirstBytecodeOfOptimizedSend value: [ :src | src includesSubstring: 'self skip' ]
	value: 'testNotStopAtAllButFirstBytecodeOfOptimizedSend

	| debugInfo methodToTest send todoPCs |
	methodToTest := self class >> #methodWithOptimizedSend.
	debugInfo := self newDebugInfoFor: methodToTest.

	"a to:do: send should stop at the to:do: point but not in the intermediate steps"
	send := methodToTest ast statements first.
	todoPCs := send methodNode pcsForNode: send.
	self assert: todoPCs size > 1.

	todoPCs allButFirst do: [ :otherPC |
		self deny: (debugInfo shouldStopAt: otherPC) ]'.

fix value: #DebugInfoTest value: #testNotStopAtConstantBlockCreation value: [ :src | src includesSubstring: 'self skip' ]
	value: 'testNotStopAtConstantBlockCreation

	| debugInfo methodToTest blockNode blockCreationPC |
	methodToTest := self class >> #methodWithConstantBlockReturn.
	debugInfo := self newDebugInfoFor: methodToTest.

	blockNode := methodToTest ast statements first.
	blockCreationPC := (blockNode methodNode pcsForNode: blockNode) first.

	self deny: (debugInfo shouldStopAt: blockCreationPC)'.

fix value: #DebugInfoTest value: #testNotStopAtFullBlockCreation value: [ :src | src includesSubstring: 'self skip' ]
	value: 'testNotStopAtFullBlockCreation

	| debugInfo methodToTest blockNode blockCreationPCs |
	methodToTest := self class >> #methodWithFullBlockReturn.
	debugInfo := self newDebugInfoFor: methodToTest.

	blockNode := methodToTest ast statements first.
	"A block may have many PCs: the creation of the block but also the creation of the temp verctor and the copied vars
	None of them are stop points."
	blockCreationPCs := blockNode methodNode pcsForNode: blockNode.
	self assert: blockCreationPCs notEmpty.

	blockCreationPCs do: [ :e | self deny: (debugInfo shouldStopAt: e) ]'.

fix value: #CodeSimulationTest value: #testErrorCodeNotFound value: [ :src | src includesSubstring: 'self skip' ]
	value: 'testErrorCodeNotFound
	| ctx result resultSimu |
	Smalltalk vm isRunningCog ifFalse: [ ^ self ].

	result := self veryBasicAt: 1.

	ctx := Context
		sender: nil
		receiver: nil
		method: Object >> #at:
		arguments: #(10).

	resultSimu := ctx
		push: nil;
		push: 500;
		doPrimitive: 117
			method: self class >> #veryBasicAt:
			receiver: self
			args: #(999).

	self assert: resultSimu isArray.
	self assert: Context primitiveFailToken first identicalTo: resultSimu first.
	self assert: result equals: resultSimu second'.

add value: #OnlineSourceDebugInfo value: #shouldStepStopAt: value: 'api'
	value: 'shouldStepStopAt: aBytecodePC

	"The stop points of the stepping of the debugger: every send, return, store, closure creation
	 and jump bytecode, the ones that the compiler inserts included.  This is the old behavior of
	 shouldStopAt:, which now answers the stop points of the source only.  The Sindarin debugger
	 steps over these stop points.  Merge this with shouldStopAt: when Sindarin is changed."

	^ (self isSourceSend: aBytecodePC) or: [
			  (self isSourceReturn: aBytecodePC) or: [
				  (self isSourceStore: aBytecodePC) or: [ (self isSourceBlockCreation: aBytecodePC) or: [ self isSourceJump: aBytecodePC ] ] ] ]'.

fix value: #DebugSession value: #stepToFirstInterestingBytecodeInContext: value: [ :src | src includesSubstring: 'debugInfo shouldStopAt: aContext pc' ]
	value: 'stepToFirstInterestingBytecodeInContext: aContext

	| bytecodeInterpreter context debugInfo |
	aContext isDead ifTrue: [
		self error: ''Cannot step into dead context'' ].

	debugInfo := aContext method debugInfo.
	bytecodeInterpreter := InstructionStream
		                       on: aContext method
		                       pc: aContext pc.
	[ debugInfo shouldStepStopAt: aContext pc ] whileFalse: [
			context := bytecodeInterpreter interpretNextInstructionFor:
				           aContext.
			context == aContext ifFalse: [ "Caused all non-send, non-store, non-return instructions
				that may activate a method. This is today the case of mustBeBoolean.
				
				If a mustBeBoolean has been activated, we need to change contexts"
				^ context ] ].

	^ aContext'.

add value: #OfflineSourceDebugInfo value: #shouldStepStopAt: value: 'accessing'
	value: 'shouldStepStopAt: aBytecodePC

	"The stop table of an offline debug info holds the stop points of the source only, see OnlineSourceDebugInfo>>shouldStopAt:"

	^ self shouldStopAt: aBytecodePC'.
