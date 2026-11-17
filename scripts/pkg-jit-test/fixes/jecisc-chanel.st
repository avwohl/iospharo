"Chanel (jecisc/Chanel, master e5012fa).  Three defects, see jecisc-chanel.md.

1. ChanelAbstractCleanerTest: the test methods compile sources that use the
   variables test, test2, test3, test4, notTest and x without a declaration.
   Behavior>>compile:methodInstaller: of Pharo 14 sets permitUndeclared: false,
   so each compile fails with OCCompilationError.  The tests mean instance
   variables (testRemoveUselessInstanceVariableRead).  The classes and the
   traits that the test helpers create get the variables as slots.
2. OCMessageNode class>>superSendTo: (extension of Chanel) passes a Symbol
   to selector:, which takes an OCSelectorNode since Pharo 14.
3. RBNamespace>>renameClass:to:around: of Pharo 14 does not make the rename
   change the target of the changes that the block records."

fix value: #ChanelAbstractCleanerTest value: #createSubclassOf:named:
	value: [ :source | (source includesSubstring: 'slots:') not ]
	value:
'createSubclassOf: aClass named: aSymbol

	"Pharo 14 compiles without undeclared variables.  The test sources use these names as instance and class-side instance variables.  A subclass of a class of this helper has the variables from its superclass."
	| names |
	names := #(test test2 test3 test4 notTest x).
	^ aClass classInstaller make: [ :builder |
		  builder
			  superclass: aClass;
			  name: aSymbol;
			  slots: ((names reject: [ :each | aClass hasSlotNamed: each ]) collect: #asSlot);
			  classSlots: ((names reject: [ :each | aClass class hasSlotNamed: each ]) collect: #asSlot);
			  package: package name ]'.

fix value: #ChanelAbstractCleanerTest value: #createTraitNamed:
	value: [ :source | (source includesSubstring: 'slots:') not ]
	value:
'createTraitNamed: aSymbol

	"Pharo 14 compiles without undeclared variables.  A class that uses the trait gets the slots of the trait, so the class loses its own slots of the same names."
	| names trait |
	names := #(test test2 test3 test4 notTest x).
	trait := Trait classInstaller make: [ :builder |
		         builder
			         beTrait;
			         name: aSymbol;
			         slots: (names collect: #asSlot);
			         package: package name ].
	class ifNotNil: [
		class classInstaller update: class to: [ :builder |
			builder slots: (class localSlots reject: [ :each | names includes: each name ]) ] ].
	^ trait'.

fix value: #'OCMessageNode class' value: #superSendTo:
	value: [ :source | (source includesSubstring: 'OCSelectorNode') not ]
	value:
'superSendTo: aSelector
	^ self receiver: OCVariableNode superNode selector: (OCSelectorNode value: aSelector)'.

fix value: #RBNamespace value: #renameClass:to:around:
	value: [ :source | (source includesSubstring: 'changes := change.') not ]
	value:
'renameClass: aRBClass to: aSymbol around: aBlock

	| change value dict oldChanges |
	change := changeFactory renameClass: aRBClass to: aSymbol.
	changes addChange: change.
	oldChanges := changes.
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
