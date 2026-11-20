fix value: #ReflectivityOnStackTest value: #testExecuteWithWaitingWithLinkInstalledInOuterMethodAfterExecutionOfMethodAfterExecutionOfEmbeddedBlockChangeMethodManually value: [ :s | s includesSubstring: 'self skip' ] value: 'testExecuteWithWaitingWithLinkInstalledInOuterMethodAfterExecutionOfMethodAfterExecutionOfEmbeddedBlockChangeMethodManually
	| instance messageNode process |
	instance := ReflectivityExampleOnStack new.

	process := [ instance exampleMethodWaitingOnSemaphoreInEmbeddedBlock ]
		           fork.
	0.1 seconds wait.
	"If we install a link after we execute, we expect it to not matter for this test, as the new code will not be executed"
	messageNode := (ReflectivityExampleOnStack
	                >> #exampleMethodWaitingOnSemaphoreInEmbeddedBlock)
		               ast sendNodes last parent.
	link := MetaLink new
		        metaObject: self;
		        selector: #tagExec.
	messageNode link: link.

	self assert: messageNode hasMetalink.
	self
		assert: (ReflectivityExampleOnStack
			 >> #exampleMethodWaitingOnSemaphoreInEmbeddedBlock) class
		equals: ReflectiveMethod.
	self assert: tag isNil.

	"make sure to create the compiledMethod"
	(ReflectivityExampleOnStack
	 >> #exampleMethodWaitingOnSemaphoreInEmbeddedBlock)
		compileAndInstallCompiledMethod.

	(process findContextsForMethod: process suspendedContext homeMethod)
		do: [ :ctx |
			ctx method: ReflectivityExampleOnStack
				>> #exampleMethodWaitingOnSemaphoreInEmbeddedBlock ].

	"here check that the method is on the stack"

	self
		assert: process suspendedContext sender sender method selector
		equals: #exampleMethodWaitingOnSemaphoreInEmbeddedBlock.
	"process should be running"
	self deny: process isTerminated.

	instance continue.
	0.1 seconds wait.
	self assert: process isTerminated.

	"It got executed!"
	self assert: tag notNil'.
add value: #CompiledMethod value: #sourceNodeForPC: value: '*Reflectivity' value: 'sourceNodeForPC: aPC
	"After metalinks were installed the AST of this method describes the NEW bytecodes, but this method still has the OLD ones (it stays on the stack of suspended processes).  ReflectiveMethod>>compileAST saved the bytecode-to-AST cache of the OLD layout in the properties of this method: use it."

	| cache |
	cache := self propertyAt: self bcToASTCacheKey ifAbsent: [ nil ].
	^ cache
		  ifNil: [ super sourceNodeForPC: aPC ]
		  ifNotNil: [ cache nodeForPC: aPC ]'.
