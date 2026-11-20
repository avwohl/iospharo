"StDebuggerContextPredicateTest>>testPrintDescription and testPrintPostMortemDescription
skipped themselves and expected '0'.  StDebuggerContextPredicate>>printDescription
answers the post-mortem prefix (from printPostMortemDescriptionOn:) followed by
'context printString'.  The '0' is a leftover from an older printDescription that
printed a number.  The tests now expect what printDescription is written to answer."
fix value: #StDebuggerContextPredicateTest value: #testPrintDescription
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testPrintDescription

	self assert: predicate printDescription equals: context printString'.


fix value: #StDebuggerContextPredicateTest value: #testPrintPostMortemDescription
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testPrintPostMortemDescription

	predicate postMortem: true.
	self
		assert: predicate printDescription
		equals: ''[Post-mortem] '' , context printString'.

"StDebuggerClientModelTest>>testComputeInitialTopContext was skipped with the remark
that it makes the production CI fail.  It passes on both VMs; skip removed."
fix value: #StDebuggerClientModelTest value: #testComputeInitialTopContext
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testComputeInitialTopContext

	| newSession dummyActionModel |
	dummyActionModel := StTestDebuggerProvider new
		                    debuggerWithMissingSubclassResponsibilityContext
		                    debuggerClientModel.
	self changeSession: dummyActionModel session.
	dummyActionModel clear.
	self
		assert: debugActionModel topContext
		identicalTo: session exception signalContext.

	dummyActionModel := StTestDebuggerProvider new
		                    debuggerWithMissingSubclassResponsibilityContext
		                    debuggerClientModel.
	newSession := dummyActionModel session.
	dummyActionModel clear.
	newSession restart: newSession interruptedContext sender.
	self changeSession: newSession.
	self
		assert: debugActionModel topContext
		identicalTo: session interruptedContext'.

"StDebuggerTest>>testGenerateAndProceed was skipped: 'it looks like my test is
executed several times with the same debugger'.  That is what happens: the
debugger session wraps the context that signalled StGenerateCode, and the
sender chain of that context is the chain of the test itself.  Proceeding the
session resumes the chain in a new process, which runs the rest of the test
method again (measured: the second run fails with 'StTestDebuggerProvider does
not understand #application:' and tearDown fails on the replaced debugger).
The test now creates the context in a process of its own.  Two more repairs:
the method source of the image has CR line ends and the expected sources in the
test had LF, and the expected generated code is the assert:equals: send that
StGenerateAssertEqualsCode>>generatedCodeFromDebugger: writes in place of see:
(the old expectation 'see: ... equals: 111' was never produced).  The test
also asserted nothing after proceeding although it computed
expectedSourceCode; it now checks the recompiled method."
fix value: #StDebuggerTest value: #testGenerateAndProceed
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testGenerateAndProceed

	| baseSourceCode expectedSourceCode adapter provider created createdSignal normalized |
	"The image keeps method sources with CR line ends; compare without that difference."
	normalized := [ :text | text withLineEndings: String cr ].
	baseSourceCode := ''methodThatGeneratesCode

	| asserter |
	asserter := TestAsserter new.
	
	asserter see: 42 + 66 + 3''.

	expectedSourceCode := ''methodThatGeneratesCode

	| asserter |
	asserter := TestAsserter new.
	
	asserter assert: 42 + 66 + 3 equals: 111''.

	"The debugger session wraps the context that signalled StGenerateCode.  Its
	sender chain must not be the chain of this test: proceeding the session
	resumes that chain in a new process, which would run the rest of this test
	method a second time (this is why the test used to be skipped).  So the
	context is created in a process of its own, and this method keeps the
	debugger in its own variable, which the resumed chain cannot reach."
	provider := StTestDebuggerProvider new.
	createdSignal := Semaphore new.
	[ created := provider debuggerWithGeneratingCodeContext.
	createdSignal signal ] forkAt: Processor userSchedulingPriority.
	(createdSignal wait: 10 seconds) ifTrue: [ self fail: ''debugger was not created'' ].
	debugger := created.
	debugger
		application: self debuggerClass currentApplication;
		initialize.
	"just things necessary to use debugger''s code presenter, which we need in this test"
	adapter := SpMorphicCodeAdapter new.
	adapter adapt: debugger code.
	debugger code adapter: adapter.
	debugger code adapter buildWidget.

	self assert: debugger canGenerateAndProceed.
	self assert: debugger exception isGeneratingCode.
	self
		assert: (normalized value: debugger interruptedContext sender sourceCode)
		equals: (normalized value: baseSourceCode).

	oldMethodToSourceDictionary
		at: debugger interruptedContext sender method
		put: baseSourceCode.

	debugger saveGeneratedCodeAndProceed.
	"StGenerateAssertEqualsCode>>generatedCodeFromDebugger: replaces the see: send
	by assert:...equals: with the value that was seen."
	self
		assert: (normalized value: (StTestDebuggerProvider >> #methodThatGeneratesCode) sourceCode)
		equals: (normalized value: expectedSourceCode)'.

"The five StDebuggerTest tests below were empty placeholders (self skip. self flag:
#DBG_TEST).  They now test StDebuggerInspector>>saveRawInspectionSelectionForContext:
and restoreRawInspectionSelectionForContext:, which no other test covers."
fix value: #StDebuggerTest value: #testSaveReceiverRawInspectionSelection
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testSaveReceiverRawInspectionSelection

	| context |
	"Paths 1 (self) and 5 (the temporary i of the block) can be read in the context of
	the session; the nodes 2 to 4 are temporaries of the method that created the
	session, which has returned, and a returned context cannot be read."
	dbg := self debuggerOn: session.
	context := dbg currentContext.

	dbg inspector getRawInspectorPresenterOrNil selectPath: #( 1 ).
	dbg inspector saveRawInspectionSelectionForContext: context.

	self
		assert: (dbg inspector rawInspectionSelectionCache
				 at: context receiver class -> context selector)
		equals: #( 1 ).
	self
		assert:
		(dbg inspector rawInspectionSelectionCache at: context receiver class)
		equals: #( 1 )'.


fix value: #StDebuggerTest value: #testSaveReceiverRawInspectionSelectionAfterContextChange
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testSaveReceiverRawInspectionSelectionAfterContextChange

	| context |
	dbg := self debuggerOn: session.
	dbg stepInto. "Still in the same context, the raw inspector is updated"
	context := dbg currentContext.

	dbg inspector getRawInspectorPresenterOrNil selectPath: #( 5 ).
	dbg stepInto. "We enter another context: the selection of the old one is saved"

	self deny: dbg currentContext identicalTo: context.
	self
		assert: (dbg inspector rawInspectionSelectionCache
				 at: context receiver class -> context selector)
		equals: #( 5 )'.


fix value: #StDebuggerTest value: #testRestoreReceiverRawInspectionSelection
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testRestoreReceiverRawInspectionSelection

	| context |
	dbg := self debuggerOn: session.
	context := dbg currentContext.
	self assert: dbg inspector getRawInspectorSelectedPath equals: #(  ).

	dbg inspector rawInspectionSelectionCache
		at: context receiver class -> context selector
		put: #( 5 ).
	dbg inspector restoreRawInspectionSelectionForContext: context.

	self assert: dbg inspector getRawInspectorSelectedPath equals: #( 5 )'.


fix value: #StDebuggerTest value: #testRestoreReceiverRawInspectionSelectionAfterContextChange
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testRestoreReceiverRawInspectionSelectionAfterContextChange

	| firstContext |
	dbg := self debuggerOn: session.
	dbg stepInto. "Still in the same context, the raw inspector is updated"
	firstContext := dbg currentContext.
	dbg inspector getRawInspectorPresenterOrNil selectPath: #( 5 ).

	dbg stepInto. "We enter another context"
	self deny: dbg currentContext identicalTo: firstContext.
	self deny: dbg inspector getRawInspectorSelectedPath equals: #( 5 ).

	"Back to the first context: its selection is restored"
	dbg stackTable selectItem: dbg stackTable items last.
	self assert: dbg currentContext identicalTo: firstContext.
	self assert: dbg inspector getRawInspectorSelectedPath equals: #( 5 )'.

"StDebuggerTest>>testReturnEnteredValue: StDebugger>>returnEnteredValue asks the user for an
expression with a modal request dialog.  The test gives the debugger an
application whose request answers the expression '0' and, in a second run,
nil (the user cancelled), the case that the flag DBG_HowToTestNilIfUserCancels
in returnEnteredValue asks for."
fix value: #StDebuggerTest value: #testReturnEnteredValue
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testReturnEnteredValue

	| requestClass applicationClass application context sender |
	requestClass := Object newAnonymousSubclass.
	requestClass addInstVarNamed: ''answer''.
	requestClass compile: ''answer: anObject answer := anObject''.
	requestClass compile: ''title: aString ^ self''.
	requestClass compile: ''openModal ^ answer''.
	applicationClass := StPharoApplication newAnonymousSubclass.
	applicationClass addInstVarNamed: ''request''.
	applicationClass compile: ''request: anObject request := anObject''.
	applicationClass compile: ''newRequest ^ request''.
	application := applicationClass new.

	dbg := self debugger
		       session: session;
		       application: application;
		       initialize;
		       yourself.
	dbg stepInto; stepInto. "We enter the squared method"
	context := dbg interruptedContext.
	sender := context sender. "a returned context has no sender any more"

	"The user cancels the request: nothing is returned"
	application request: (requestClass new answer: nil; yourself).
	dbg returnEnteredValue.
	self assert: dbg interruptedContext identicalTo: context.

	"The user enters an expression: its value is returned to the sender"
	application request: (requestClass new answer: ''0''; yourself).
	dbg returnEnteredValue.
	self assert: dbg interruptedContext identicalTo: sender.
	self assert: dbg interruptedContext top equals: 0'.
