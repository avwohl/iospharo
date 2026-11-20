fix value: #OCParserTest value: #testUnclosedTemporariesErrorNodeContainsRightValue value: [ :s | s includesSubstring: 'self skip' ] value: 'testUnclosedTemporariesErrorNodeContainsRightValue
	| tree |
	tree := self parseFaultyExpression: ''| faulty temporaries''.
	self assert: tree isEnglobingError.
	self assert: tree formattedCode equals: ''| faulty temporaries |'', String cr'.
fix value: #OCParserTest value: #testParseExpressionDontFreeze value: [ :s | s includesSubstring: '<expectedFailure>' ] value: 'testParseExpressionDontFreeze

	"This test checks that the image (and CI) does not freeze or crash on a code error.
	The parse error must be reported gracefully, as an OCCodeError."

	self should: [ self parserClass parseExpression: ''#'' ] raise: OCCodeError'.
