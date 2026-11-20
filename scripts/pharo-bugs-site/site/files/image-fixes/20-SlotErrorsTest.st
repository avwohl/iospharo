fix value: #SlotErrorsTest value: #testCannotBeRecompiled value: [ :s | s includesSubstring: '<expectedFailure>' ] value: 'testCannotBeRecompiled
	"The new class builder (ClassBuilder replacement, Pharo 9+) can change the layout of a class that has subclasses: the subclasses are rebuilt with the new layout.  The old builder raised ''X cannot be recompiled''.  The test records that the new builder rebuilds the hierarchy and keeps the subclass in step."

	| superclass subclass |
	superclass := self make: [ :builder |
		builder
			superclass: Object;
			name: self aClassName ].

	self make: [ :builder |
		builder
			superclass: superclass;
			name: self anotherClassName ].

	superclass := self make: [ :builder |
		builder
			layoutClass: VariableLayout;
			name: self aClassName ].

	subclass := superclass subclasses detect: [ :each | each name = self anotherClassName ].
	self assert: superclass classLayout class equals: VariableLayout.
	self assert: subclass superclass equals: superclass.
	self assert: subclass classLayout class equals: VariableLayout.
	self assert: subclass isVariable'.
