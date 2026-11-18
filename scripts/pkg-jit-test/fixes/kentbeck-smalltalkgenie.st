"KentBeck/SmalltalkGenie: two package methods use Pharo 13 APIs that Pharo 14 changed.

GenieServer>>returnError:by: sent Context>>debugStack:on:.  Pharo 14 has no
such method (the Deprecated14 package keeps only debugStackOn: and
printDebugStackOn:, both with no depth limit, and both deprecated for
printStackTraceOn:).  So the debug mode of a failed tool call raised
MessageNotUnderstood.  The repair prints at most 'stackSize' contexts by
walking the sender chain, as debugStack:on: did.

GenieServer>>handleTemporaryToInstanceVariable: used
RBTemporaryToInstanceVariableTransformation.  In Pharo 14 the class that is
tested and kept is ReTemporaryToInstanceVariableRefactoring (the old name is a
deprecated alias).  The old Transformation class stays in category
Model-Unused and has an old privateTransform: it removes the temporary from
every method of the class BEFORE it adds the instance variable, and it
compiles each method even when that method has no such temporary.  The
compile of 'promoteTemp ^ cache' then has an undeclared variable, which gives
OCCompilationError.  The Re class adds the instance variable first and skips
methods without the temporary.  The repair builds the Re class."

fix value: #GenieServer value: #returnError:by:
	value: [ :source | source includesSubstring: 'debugStack:' ]
	value:
'returnError: anError by: resultDict

	| errorInfo signalerContext |
	errorInfo := Dictionary new.
	errorInfo
		at: ''description'' put: ''Tool execution failed'';
		at: ''type'' put: anError class name asString;
		at: ''error_id'' put: UUID new asString.
	self exposeDebugErrors ifTrue: [
			errorInfo at: ''description'' put: anError description.
			signalerContext := anError signalerContext.
			errorInfo
				at: ''stack_trace'' put: (String streamContents: [ :stream |
							 | context remaining |
							 context := signalerContext.
							 remaining := self stackSize.
							 [ context notNil and: [ remaining > 0 ] ] whileTrue: [
									 context printOn: stream.
									 stream cr.
									 context := context sender.
									 remaining := remaining - 1 ] ]);
				at: ''receiver''
				put: (self receiverErrorInfoFrom: signalerContext receiver) ].
	resultDict at: ''error'' put: errorInfo.
	^ resultDict'.

fix value: #GenieServer value: #handleTemporaryToInstanceVariable:
	value: [ :source | source includesSubstring: 'RBTemporaryToInstanceVariableTransformation' ]
	value:
'handleTemporaryToInstanceVariable: request

	^ self returnResultDo: [
			  self
				  executeVariableRefactoring:
					  (ReTemporaryToInstanceVariableRefactoring
						   class: (self classFromRefactoringRequest: request)
						   selector: (request at: #method_name) asSymbol
						   variable: (request at: #variable_name))
				  request: request ]'.
