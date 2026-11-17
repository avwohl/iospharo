"Beacon (pharo-project/pharo-beacon): MethodStackSignal>>currentStack calls
'each method asRingDefinition' on every context of the stack.  In Pharo 14
Context>>method answers the CompiledBlock for a block context (Context>>method
comment: use compiledCode, 'it can be CompiledMethod or a CompiledBlock'), and
CompiledBlock does not understand #asRingDefinition (only CompiledMethod has it,
Ring-Definitions-Core/CompiledMethod.extension.st).  Every stack that contains a
block frame (all test runs: TestCase>>runCase blocks) fails with
'CompiledBlock >> #asRingDefinition' DNU.  The signal documents that it stores
the method definitions of the stack, so a block frame maps to its home method:
CompiledBlock>>method (outerCode method) answers the CompiledMethod, and
CompiledMethod>>method answers self."

fix value: #MethodStackSignal value: #currentStack
	value: [ :source | source includesSubstring: 'each method asRingDefinition' ]
	value:
'currentStack
	^ self relevantStackWithoutSelf collect: [ :each | each homeMethod asRingDefinition ]'.
