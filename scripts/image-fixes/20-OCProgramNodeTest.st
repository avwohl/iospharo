"OCAssignmentNode>>bestNodeFor: answered the value (the right side) when the interval starts in the ' := ' between the variable and the value, because the value is the only child the interval touches.  The interval lies in the assignment, so the assignment is the best node."
add value: #OCAssignmentNode value: #bestNodeFor: value: 'querying' value:
'bestNodeFor: anInterval

	| node |
	node := super bestNodeFor: anInterval.
	^ (node == value and: [ anInterval first >= self start and: [ anInterval first < value start ] ])
		ifTrue: [ self ]
		ifFalse: [ node ]'.

fix value: #OCProgramNodeTest value: #testBestNodeForReturnAStatementWhenIntervalInStatementWithoutLeftPart
	value: [ :source | source includesSubstring: '<expectedFailure>' ]
	value:
'testBestNodeForReturnAStatementWhenIntervalInStatementWithoutLeftPart
	"When the selection is not close to the variable but to the colon, the assignment is returned, not the literal (the right value)"
	| ast start body |
	body := 
''m
| a |
a := 1111111111111111.
''.
	
	start := ''m
| a |
'' size.
		
	ast := (OCParser parseMethod: body) bestNodeFor: (start + 2 to: start + 6).
	self assert: ast isAssignment.
	self assert: ast sourceCode equals: ''a := 1111111111111111''.'.
