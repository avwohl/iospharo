"tomooda/ViennaTalk: ViennaConstrainedTypeTest>>testName fails the same way on
the two VMs.  ViennaConstrainedType>>name builds the name of the type from
'invariant printString'.  In Pharo 14 BlockClosure>>printOn: prints
'[ ] in Class>>#selector', not the source of the block, so the name is
'int inv [ ] in ViennaConstrainedTypeTest>>#testName' and not
'int inv [ :i | i < 0 ]'.  The name is shown to the user as the text of the
type, so the code must use the source text.  The repair uses the source of the
AST node of the block (BlockClosure>>sourceNode, sourceCode) for a block, and
keeps printString for the other invariants (a Symbol such as #odd prints as
#odd, as before).  The test and its expected text stay as they are."

fix value: #ViennaConstrainedType value: #name
	value: [ :source | source includesSubstring: ''' inv '' , invariant printString' ]
	value:
'name
	| text |
	text := invariant isBlock
		ifTrue: [ invariant sourceNode sourceCode ]
		ifFalse: [ '''' ].
	^ basetype name , '' inv '' , (text isEmpty
		ifTrue: [ invariant printString ]
		ifFalse: [ text ])'.
