fix value: #OCRengineTest value: #testWhichUNodesDefinesBlockArg value: [ :s | s includesSubstring: 'self skip' ] value: 'testWhichUNodesDefinesBlockArg

	| ast |
	ast := OCParser parseMethod: ''whoDefines

[ :each | ^ each ]	
	''.
	self assert: (ast body children first body children first value whichUpNodeDefines: ''each'') class equals: OCBlockNode'.
