"Pakarati/pharo-gsoc2025: DTTestCoverageTest>>testDTTestCoverageResultHas2Nodes
asserted three sub results of DTTestCoverageResult>>buildTreeForUI.  That
method builds two nodes (the percent node and the 'Uncovered methods' node);
the 'Covered' and 'Partially covered' trees are separate methods
(buildTreeForUICovered, buildTreeForUIPartiallyCovered) with their own tree
pragmas.  The code had two nodes in the first commit of the package, the test
name says 2, and the sibling tests read node 1 (percent) and node 2 (leaves).
So the number 3 in the assertion is the defect, on every VM and every Pharo
version.  The repair makes the assertion say 2."

fix value: #DTTestCoverageTest value: #testDTTestCoverageResultHas2Nodes
	value: [ :source | source includesSubstring: 'equals: 3' ]
	value:
'testDTTestCoverageResultHas2Nodes

	| resultTree |
	resultTree := (plugin runForConfiguration: pluginConfiguration) buildTreeForUI.
	self assert: resultTree subResults size equals: 2'.
