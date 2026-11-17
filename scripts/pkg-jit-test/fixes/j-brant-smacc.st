"j-brant/SmaCC: the code generator and the tests of SmaCC use Pharo APIs that
Pharo 14 no longer has.  25 tests of SmaCCEndToEndTest end in an error, for
four causes.

1. SmaCCIncrementalParser class>>incrementalParserFrom: makes an anonymous
   subclass of the parser and copies the instance side and the class side
   methods into the subclass.  The class side method parserClasses reads the
   class-side instance variable parserClasses.  The anonymous subclass got the
   instance slots only, so the compile of that class side method fails with
   'Undeclared variable named parserClasses' (12 tests).  The repair adds the
   class slots of SmaCCIncrementalParser class to the builder.

2. SmaCCSmalltalkCodeGenerator>>splitIfTrueBranchFrom:usingName:into: and
   splitSequenceFrom:usingName:into: build a message node with a Symbol as
   selector.  OCMessageNode>>selector: of Pharo 14 wants an OCSelectorNode and
   sends #parent: to it (5 tests).  The two rewrite rules of the generator
   (addImplementationSpecificRewritesTo:, commonIfTrueCodeRewriter) build the
   same node in a pattern string, so they get the same repair.

3. SmaCCEndToEndTest uses 'Superclass subclass: #Name instanceVariableNames:
   classVariableNames: category:' as a message.  Pharo 14 handles that old
   form only when the compiler reads a class definition, not as a message
   (5 tests).  The repair adds a helper selector to Class that builds the
   class with the fluid builder, and the six test methods send the helper.

4. Symbol>>asClass does not exist in Pharo 14 (2 tests; a third use in the
   generator method defaultPackageName).  The tests use the lookupType:
   method of the test class.  defaultPackageName answers the name of the
   package of loose classes, #_UnpackagedPackage, as the old RPackage did."

add value: #Class value: #'smaccSubclass:instanceVariableNames:classVariableNames:category:' value: '*SmaCC_Tests' value:
'smaccSubclass: aName instanceVariableNames: aString classVariableNames: aClassVars category: aPackageName
	"The old class creation message that the tests of SmaCC send, built on the fluid class builder of Pharo 14."
	| builder |
	builder := self << aName.
	builder slots: aString asSlotCollection asArray.
	aClassVars isEmpty ifFalse: [ builder sharedVariablesFromString: aClassVars ].
	builder package: aPackageName.
	^ builder install'.

fix value: #'SmaCCIncrementalParser class' value: #incrementalParserFrom:
	value: [ :source | (source includesSubstring: 'classSlots:') not ]
	value:
'incrementalParserFrom: aGLRParserClass
	^ self parserClasses
		at: aGLRParserClass
		ifAbsentPut: [ | class |
			class := Smalltalk anonymousClassInstaller
					make: [ :builder | 
						builder
							installingEnvironment: aGLRParserClass environment;
							superclass: aGLRParserClass;
							layoutClass: aGLRParserClass classLayout class;
							slots: (self instVarNames collect: [ :each | InstanceVariableSlot named: each ]);
							classSlots: (self class instVarNames collect: [ :each | InstanceVariableSlot named: each ]) ].
			self selectors do: [ :each | class compile: (self sourceCodeAt: each) ].
			self class selectors
				do: [ :each | class class compile: (self class sourceCodeAt: each) ].
			class ]'.

fix value: #SmaCCSmalltalkCodeGenerator value: #defaultPackageName
	value: [ :source | source includesSubstring: 'asClass' ]
	value:
'defaultPackageName
	"Pharo 12 and later have no Package class>>defaultPackageName. The package of loose classes is _UnpackagedPackage."
	^ #''_UnpackagedPackage'''.

"The next entries change a few words of a method; the new source is the old source with those words replaced."
{
	{ 'SmaCCSmalltalkCodeGenerator'. #splitIfTrueBranchFrom:usingName:into:.
		{ 'selector: aSelector.' -> 'selector: (OCSelectorNode value: aSelector).' } }.
	{ 'SmaCCSmalltalkCodeGenerator'. #splitSequenceFrom:usingName:into:.
		{ 'selector: aSelector)).' -> 'selector: (OCSelectorNode value: aSelector))).' } }.
	{ 'SmaCCSmalltalkCodeGenerator'. #addImplementationSpecificRewritesTo:.
		{ 'selector: #ifTrue: arguments:' -> 'selector: (OCSelectorNode value: #ifTrue:) arguments:' } }.
	{ 'SmaCCSmalltalkCodeGenerator'. #commonIfTrueCodeRewriter.
		{ 'selector: #ifTrue: arguments:' -> 'selector: (OCSelectorNode value: #ifTrue:) arguments:' } }.
	{ 'SmaCCEndToEndTest'. #testCompileIntoNewPackage.
		{ '#Test123Parser asClass' -> '(self lookupType: #Test123Parser)' } }.
	{ 'SmaCCEndToEndTest'. #testRemoveVariablesFromNodes.
		{ '#FooIfNode asClass' -> '(self lookupType: #FooIfNode)' } }.
	{ 'SmaCCEndToEndTest'. #testExistingVariables. { 'subclass: #' -> 'smaccSubclass: #' } }.
	{ 'SmaCCEndToEndTest'. #testGLRHierarchy. { 'subclass: #' -> 'smaccSubclass: #' } }.
	{ 'SmaCCEndToEndTest'. #testHierarchy. { 'subclass: #' -> 'smaccSubclass: #' } }.
	{ 'SmaCCEndToEndTest'. #testMoveFromGLRHierarchy. { 'subclass: #' -> 'smaccSubclass: #' } }.
	{ 'SmaCCEndToEndTest'. #testMoveToGLRHierarchy. { 'subclass: #' -> 'smaccSubclass: #' } }.
	{ 'SmaCCEndToEndTest'. #testPriority. { 'subclass: #' -> 'smaccSubclass: #' } }
} do: [ :spec | | cls sel pairs source newSource |
	cls := resolve value: spec first.
	sel := spec second.
	pairs := spec third.
	(cls notNil and: [ cls includesSelector: sel ])
		ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', spec first, '>>', sel, ' (absent)'; lf; flush ]
		ifTrue: [
			source := (cls >> sel) sourceCode.
			(source includesSubstring: pairs first key)
				ifFalse: [ Stdio stdout nextPutAll: 'SKIP ', spec first, '>>', sel, ' (defect not present)'; lf; flush ]
				ifTrue: [
					newSource := source.
					pairs do: [ :pair | newSource := newSource copyReplaceAll: pair key with: pair value ].
					cls compile: newSource classified: (cls >> sel) protocolName.
					Stdio stdout nextPutAll: 'FIXED ', spec first, '>>', sel; lf; flush ] ] ].
