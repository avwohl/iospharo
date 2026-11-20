"Refactoring-Core: a rename of a class loses the changes of its methods
(2026-11-17).  Found by the package run: Chanel,
ChanelTestCaseNameCleanerTest>>testRenameTestCaseEndingWithTestsUpdateReferences
(MessageNotUnderstood: receiver of includesSelector: is nil, in
ReAddMethodChange>>asUndoOperation).  The same on stock Cog and on our VM.

RBNamespace>>renameClass:to:around: adds the rename change to the changes of
the namespace, keeps the old value of 'changes', runs the block, and puts the
old value back.  The method never makes the rename change the current
'changes', so the code that puts the old value back has no effect.  The
sibling method perform:onlyWithChange: has the line
'changes := aCompositeRefactoryChange' at that place.  Without the line, the
method changes that the block records (the methods of the class that refer
to its own name) are siblings of the rename change and not its parts.
ReRenameClassChange>>executeNotifying: gives the new class name only to its
parts.  A sibling keeps the old name, and ReClassChange>>changeClass answers
nil for that name after the rename.

The repair adds the line.  The guard is the Pharo 14 form of the method with
no such line.

Two tests of the image count the changes of a class rename at the top level
of the model: RBTransformationsTest>>testRenameClassTransform (11) and
ReRenameClassCommandTest>>testRenameClassCommandSuccess (7).  The counts are
the rename change and the method changes as its siblings, which is the
defect.  ReRenameClassChange>>executeNotifying: runs the parts of the rename
change with the new class name, so the parts are where the method changes
belong.  With the repair the model has one change, the rename change, and
the method changes are its parts (10 and 6).  The two tests now check the
two numbers."

fix value: #RBNamespace value: #renameClass:to:around:
	value: [ :source | (source includesSubstring: 'oldChanges := changes.')
		and: [ (source includesSubstring: 'changes := change.') not ] ]
	value:
'renameClass: aRBClass to: aSymbol around: aBlock

	| change value dict oldChanges |
	change := changeFactory renameClass: aRBClass to: aSymbol.
	changes addChange: change.
	oldChanges := changes.
	"The changes that the block records are parts of the rename change, as in perform:onlyWithChange:.  The rename then gives them the new class name."
	changes := change.
	aBlock ensure: [ changes := oldChanges ].
	self flushCaches.

	dict := (newClasses includesKey: aRBClass name)
		        ifTrue: [ newClasses ]
		        ifFalse: [ changedClasses ].

	self markAsRemoved: aRBClass name.
	self unmarkAsRemoved: aSymbol.

	value := dict at: aRBClass name.
	dict removeKey: aRBClass name.
	dict at: aSymbol put: value.

	value first name: aSymbol.
	value last name: aSymbol.

	value first subclasses do: [ :each | each superclass: value first ].
	value last subclasses do: [ :each | each superclass: value last ].

	^ change'.

fix value: #RBTransformationsTest value: #testRenameClassTransform
	value: [ :source | source includesSubstring: 'transformation model changes changes size equals: 11.' ]
	value: ((RBTransformationsTest >> #testRenameClassTransform) sourceCode
		copyReplaceAll: 'self assert: transformation model changes changes size equals: 11.'
		with: '"The rename change is the one change of the model.  The changes of the methods are its parts, so that the rename gives them the new class name."
	self assert: transformation model changes changes size equals: 1.
	self assert: transformation model changes changes first changes size equals: 10.').

fix value: #ReRenameClassCommandTest value: #testRenameClassCommandSuccess
	value: [ :source | source includesSubstring: 'driver changes changes size equals: 7.' ]
	value: ((ReRenameClassCommandTest >> #testRenameClassCommandSuccess) sourceCode
		copyReplaceAll: 'self assert: driver changes changes size equals: 7.'
		with: '"The rename change is the one change.  The changes of the methods are its parts.  (One read of the changes: ReInteractionDriver>>changes runs the transformation at each call.)"
	self assert: (driver changes changes collect: [ :each | each changes size ]) asArray equals: #( 6 ).').
