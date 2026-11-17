fix value: #RGMCClassTest value: #testClassesWithTraits value: [ :s | s includesSubstring: 'self skip' ] value: 'testClassesWithTraits

	"classes with traits"

	Smalltalk allClasses
		select: [ :each | each isTrait not and: [each traits notEmpty or: [ each class traits notEmpty ]] ]
		thenDo: [ :each | self testClass: each ]'.

"The MC class definition stores only the slots a class defines itself (localSlots); slots that
 a trait provides are not part of it.  The test compared with instVarNames, which includes the
 slots from traits.  Compare with the local slots, as asClassDefinition exports them."
fix value: #RGMCTest value: #testClass: value: [ :s | s includesSubstring: 'equals: aClass instVarNames' ] value: 'testClass: aClass

	| mcDef def env|
	mcDef := 	aClass asClassDefinition.
	def := mcDef asRGDefinition.
	env := def environment.

	self assert: def name equals: aClass name.
	aClass superclass ifNotNil: [ "ProtoObject & co."
		self assert: def superclass name equals: aClass superclass name.].
	self assert: def category equals: aClass category.
	self assert: def layout class equals: (self layoutClassesMapping at: aClass classLayout class).

	def layout isPointerLayout ifTrue: [
		"Monticello keeps only the slots the class defines itself, not the ones from its traits"
		self assert: (def layout slots collect: [:each | each name]) asArray equals: (aClass localSlots collect: [:each | each name]) asArray.].
	self assert: (def metaclass layout slots collect: [:each | each name]) asArray equals: (aClass class localSlots collect: [:each | each name]) asArray.
	self assert: (def classVariables collect: [:each | each name]) equals: aClass classVarNames.
	self assert: (def sharedPoolNames asArray) equals: aClass sharedPoolNames asArray.

	"Monticello class definition still use only class categories, packages are unresolved"
	self assert: (def hasUnresolved: #package).
	self assert: (def hasResolved: #traitComposition).
	self compareTraitComposition: aClass traitComposition with: def traitComposition.
	self compareTraitComposition: aClass class traitComposition with: def metaclass traitComposition'.

"In a class with complex slots Monticello exports a plain variable as #y; the Ring definition kept the # in the slot name, for instance and class-side variables."
fix value: #MCClassDefinition value: #ensureRingDefinitionIn: value: [ :s | (s includesSubstring: 'RGInstanceVariableSlot named: each name parent: def layout') ] value: 'ensureRingDefinitionIn: anRGEnvironment

	^ anRGEnvironment backend definitionFor: self ifAbsentRegister: [
		| def metaclassDef |
		def := anRGEnvironment ensureClassNamed: self className asSymbol.
		metaclassDef := anRGEnvironment ensureClassNamed: (self className, '' class'') asSymbol.
		def metaclass: metaclassDef.
		def traitComposition: (self traitCompositionFrom: self traitComposition parent: def).
		metaclassDef traitComposition: (self traitCompositionFrom: self classTraitComposition parent: metaclassDef).
		(self superclassName = #nil)
			ifTrue: [ def superclass: def ]
			ifFalse: [ def superclass: (anRGEnvironment ensureClassNamed: superclassName ) ].
		def category: self category.
		def layout: (self ring2LayoutType parent: def).
		metaclassDef layout: (RGFixedLayout parent: metaclassDef).
		def comment: (
			(RGComment parent: def)
				content: self comment;
				author: (self authorForStamp: self commentStamp);
				time: (self timeForStamp: self commentStamp);
				yourself).
		self variables do: [ :each |
			(each class type = #instance) ifTrue: [
				(each name includes: $>)
					ifFalse: [ def layout addSlot: (RGInstanceVariableSlot named: (each name copyWithout: $#) trimBoth parent: def layout)]
					ifTrue: [
						| unknownSlot |
						unknownSlot := RGUnknownSlot named: ((each name copyUpTo: $=) copyWithout: $#) trimBoth parent: def layout.
						unknownSlot expression: (each name copyAfter: $>) trimBoth.
						def layout addSlot: unknownSlot]].
			(each class type = #class) ifTrue: [
				def addClassVariable: (RGClassVariable named: each name parent: def)].
			(each class type = #classInstance) ifTrue: [
				metaclassDef layout addSlot: (RGInstanceVariableSlot named: (each name copyWithout: $#) trimBoth parent: metaclassDef layout)].
			(each class type = #pool) ifTrue: [
				def addSharedPool: (RGPoolVariable named: each name parent: def)]. ].
		def]'.
