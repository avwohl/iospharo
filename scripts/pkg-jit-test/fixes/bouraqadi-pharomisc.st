"PharoMisc (bouraqadi/PharoMisc, branch pharo12).  The code is for Pharo 12.
The tests that do not pass on Pharo 14 are the same on stock Cog and on our
VM.

M1. CodeMetrics (CmCodeAnalyzerTest, 5 errors: 'Attempt to read undeclared
variable' for the old organizer class).  CmCodeAnalyzer asked the old
organizer global for the category names and for the classes of a category.
Pharo 14 does not have that global.  'Smalltalk organization' answers a
PackageOrganizer, and a category is a package tag: PackageTag>>categoryName
is the package name, a hyphen and the tag name, or the package name for the
root tag.  The two methods now read the package tags and keep the match of
the category pattern.  The tests need a category with no class
(CodeMetrics-Tests-CategoryEmptyOnPurpose).  A Tonel repository cannot hold a
tag with no class, so setUp makes the tag and tearDown removes it when it is
still empty."

fix value: #CmCodeAnalyzer value: #initCategories
	value: [ :source | source includesSubstring: 'SystemOrganizer' ]
	value:
'initCategories
	"The categories are the names of the package tags (PackageTag>>categoryName).  The root tag of a package counts only when it holds a class."

	| all |
	all := OrderedCollection new.
	Smalltalk organization packages do: [ :package |
		package tags do: [ :tag |
			(tag isRoot not or: [ tag isEmpty not ])
				ifTrue: [ all add: tag categoryName asString ] ] ].
	^ categories := all select: [ :each | categoriesMatchString match: each ]'.

fix value: #CmCodeAnalyzer value: #addClassesInCategory:
	value: [ :source | source includesSubstring: 'SystemOrganizer' ]
	value:
'addClassesInCategory: aCategory

	Smalltalk organization packages do: [ :package |
		package tags do: [ :tag |
			tag categoryName asString = aCategory asString
				ifTrue: [ classes addAll: tag classes ] ] ]'.

fix value: #CmCodeAnalyzerTest value: #setUp
	value: [ :source | (source includesSubstring: 'CategoryEmptyOnPurpose') not ]
	value:
'setUp

	super setUp.
	Smalltalk organization ensureTag: #CategoryEmptyOnPurpose inPackage: #''CodeMetrics-Tests''.
	analyzer := CmCodeAnalyzer new'.

add value: #CmCodeAnalyzerTest value: #tearDown value: 'running'
	value:
'tearDown

	| package |
	package := Smalltalk organization packageNamed: #''CodeMetrics-Tests'' ifAbsent: [ nil ].
	package ifNotNil: [
		(package tagNamed: #CategoryEmptyOnPurpose ifAbsent: [ nil ]) ifNotNil: [ :tag |
			tag isEmpty ifTrue: [ package removeTag: tag ] ] ].
	super tearDown'.

"PharoMisc, LightweightObserver (append to the entries of this file).

G1. LoSubject class used the Pharo 12 hook ClassDescription>>addAndClassifySelector:withMethod:inProtocol: to wrap each method that writes an instance variable (wrapper method with the old selector sends the event, the user method is kept under basicSubjectMethod_<selector>).  Pharo 14 never sends that message: every installer ends in Behavior>>addSelectorSilently:withMethod: (OCMethodInstaller>>addAndClassifyMethod:into:), so no wrapper exists and no observer fires.  The hook now overrides addSelectorSilently:withMethod: in LoSubject class, updates methodDict and localMethodDict, and the old compiler calls (failBlock:, compile, putSource:withPreamble:) are replaced.  A wrapper answers the source pointer and the source of its basic method, so Behavior>>compile:from: keeps the user source through every recompilation.  LoSubject initialize at the end wraps the methods that were loaded before the hook existed.

G2. LoSubjectAutomaticCodeGenerationTest used ClassFactoryForTestCase>>silentlyNewSubclassOf:instanceVariableNames:classVariableNames: and newSubclassOf:..., the ST80 subclass:instanceVariableNames:... message and Class>>fileOutOn:.  Pharo 14 has silentlyMake:, make:, update:to: (with ClassFactoryForTestCase environment:) and the Fileouter class.  The tests keep every assertion; only the way to make or change the class and to file it out changes.

G3. Array>>asSubject sends becomeForward: to the receiver.  The receiver of LoArrayAutomaticEventGenerationTest>>testReset is a literal Array, and Pharo 14 compiles literals as read-only (beReadOnlyLiteral).  Primitive 72 refuses a read-only receiver with 'no modification' on Cog and on our VM, so the receiver is made writable first.

G4. LoSubjectSortedCollection got addLast: from a trait whose super is SortedCollection>>addLast:, which is shouldNotImplement, so the tests that append at the end fail.  SortedCollection>>addAll: adds with addNoSort: and sorts once, so add: is never sent and no event is dispatched.  addLast: now appends without sorting and sends the add event, and addAll: sends add: for each element.

G5. LoDicePresenter, LoDiceListPresenter and LoDiceAdderPresenter are Spec 1 classes (ComposablePresenter, DynamicComposablePresenter, SpecLayout).  Pharo 14 has only Spec 2, so the three classes are not created at load and LoDiceUiTest reads undeclared variables.  The classes are made again with Spec 2 and the observer logic is unchanged.  Lowest confidence: Spec 2 messages were not checked against sources."

"G1: source of a wrapper method comes from its basic method."
add value: #LoSubjectSupportMethod value: #basicMethodOrNil value: 'subject support' value:
'basicMethodOrNil
	"The basic method that a wrapper method calls.  Answers nil when the receiver is itself the basic method, or when there is none."

	| basicMethod |
	self methodClass ifNil: [ ^ nil ].
	basicMethod := self methodClass methodDict
		at: (LoSubject basicMethodSelectorFrom: self selector)
		ifAbsent: [ ^ nil ].
	^ basicMethod == self
		ifTrue: [ nil ]
		ifFalse: [ basicMethod ]'.

add value: #LoSubjectSupportMethod value: #sourcePointer value: 'source code management' value:
'sourcePointer
	"A wrapper method has no source of its own: it answers the source pointer of its basic method.  Behavior>>compile:from: copies this pointer to the new method, so the user source survives a recompilation."

	| pointer basicMethod |
	pointer := super sourcePointer.
	pointer = 0 ifFalse: [ ^ pointer ].
	basicMethod := self basicMethodOrNil.
	^ basicMethod
		ifNil: [ 0 ]
		ifNotNil: [ basicMethod sourcePointer ]'.

add value: #LoSubjectSupportMethod value: #sourceCodeOrNil value: 'source code management' value:
'sourceCodeOrNil
	"The source of a wrapper method is the source that the user wrote, which the basic method holds.  The #source property of the wrapper (set by the Pharo 14 compiler, OpalCompiler>>generateMethod) is the generated wrapper source and must not win: the file-out of a class reads sourceCode."

	| basicMethod |
	basicMethod := self basicMethodOrNil.
	^ basicMethod
		ifNil: [ super sourceCodeOrNil ]
		ifNotNil: [ basicMethod sourceCodeOrNil ]'.

add value: #LoSubjectSupportMethod value: #sourceCode value: 'source code management' value:
'sourceCode
	"Never decompile a wrapper method: its bytecode shows the generated code.  Without any source, the decompiled basic method is the code of the user."

	| basicMethod |
	^ self sourceCodeOrNil ifNil: [
		basicMethod := self basicMethodOrNil.
		basicMethod
			ifNil: [ super sourceCode ]
			ifNotNil: [ basicMethod sourceCode ] ]'.

"G1: the new hook.  Pharo 14 installs every compiled method with addSelectorSilently:withMethod: (OCMethodInstaller>>addAndClassifyMethod:into:)."
add value: #'LoSubject class' value: #addSelectorSilently:withMethod: value: 'accessing method dictionary' value:
'addSelectorSilently: selector withMethod: compiledMethod
	"Pharo 14 installs every compiled method with this message (OCMethodInstaller>>addAndClassifyMethod:into:).  The Pharo 12 hook, ClassDescription>>addAndClassifySelector:withMethod:inProtocol:, does not exist any more."

	(compiledMethod isKindOf: LoSubjectSupportMethod)
		ifTrue: [ ^ super addSelectorSilently: selector withMethod: compiledMethod ].
	compiledMethod isFromTrait
		ifTrue: [ ^ super addSelectorSilently: selector withMethod: compiledMethod ].
	^ self addAndClassifySelector: selector withMethod: compiledMethod inProtocol: nil'.

add value: #'LoSubject class' value: #restoreLocalMethodAt: value: 'subject support' value:
'restoreLocalMethodAt: selector
	"OCTraitMethodInstaller puts a method in the local method dictionary before the hook runs.  Put back what the method dictionary holds when the hook refuses the method."

	(self respondsTo: #localMethodDict) ifFalse: [ ^ self ].
	(self methodDict at: selector ifAbsent: [ nil ])
		ifNil: [ self localMethodDict removeKey: selector ifAbsent: [ nil ] ]
		ifNotNil: [ :method | self localMethodDict at: selector put: method ]'.

fix value: #'LoSubject class' value: #addAndClassifySelector:withMethod:inProtocol:
	value: [ :source | (source includesSubstring: 'restoreLocalMethodAt:') not ]
	value:
'addAndClassifySelector: selector withMethod: compiledMethod inProtocol: aProtocol
	(self isBasicMethodSelector: selector) ifTrue: [
		self restoreLocalMethodAt: selector.
		^ LoSubjectBasicMethodChangeForbiddenError signal ].
	self
		addObservingHooksFor: selector
		withMethod: compiledMethod
		inProtocol: aProtocol'.

fix value: #'LoSubject class' value: #addObservingHooksFor:withMethod:inProtocol:
	value: [ :source | (source includesSubstring: 'attachSourceOf:') ]
	value:
'addObservingHooksFor: selector withMethod: compiledMethod inProtocol: aProtocol
	| ivWriteNodes wrapperMethod basicSelector |
	ivWriteNodes := compiledMethod instanceVariableWriteNodes reject: [ :ivNode | ivNode name asSymbol = self dispatcherIvName ].
	basicSelector := self basicMethodSelectorFrom: selector.
	wrapperMethod := ivWriteNodes isEmpty
		ifTrue: [ nil ]
		ifFalse: [ self newWrapperMethod: selector forWritingIvs: (ivWriteNodes collect: [ :each | each name asSymbol ]) ].
	wrapperMethod ifNil: [
		(self includesLocalSelector: basicSelector) ifTrue: [ super removeSelector: basicSelector ].
		^ super addSelectorSilently: selector withMethod: compiledMethod ].
	{ wrapperMethod. compiledMethod } do: [ :each | LoSubjectSupportMethod adoptInstance: each ].
	super addSelectorSilently: selector withMethod: wrapperMethod.
	super addSelectorSilently: basicSelector withMethod: compiledMethod.
	(self respondsTo: #localMethodDict) ifTrue: [
		self localMethodDict at: selector put: wrapperMethod.
		self localMethodDict at: basicSelector put: compiledMethod ].
	self codeChangeAnnouncer suspendAllWhile: [
		self classify: basicSelector under: #''private-generated'' ]'.

fix value: #'LoSubject class' value: #newWrapperMethod:forWritingIvs:
	value: [ :source | source includesSubstring: 'failBlock:' ]
	value:
'newWrapperMethod: selector forWritingIvs: writtenIvs
	| wrapperMethodSource |
	wrapperMethodSource := self newWrapperMethodSource: selector forWriting: writtenIvs.
	^ [ self compiler
		source: wrapperMethodSource;
		compileOrFail ]
		on: Error
		do: [ :error | nil ]'.

fix value: #'LoSubject class' value: #attachSourceOf:to:
	value: [ :source | source includesSubstring: 'putSource:withPreamble:' ]
	value:
'attachSourceOf: compiledMethod to: wrapperMethod
	"Nothing to do: the source of a wrapper method is the source of its basic method, see LoSubjectSupportMethod>>sourcePointer and sourceCodeOrNil.  CompiledMethod>>putSource:withPreamble: does not exist in Pharo 14."'.

fix value: #'LoSubject class' value: #recompile:from:
	value: [ :source | source includesSubstring: 'addAndClassifySelector:' ]
	value:
'recompile: selector from: oldClass
	"The hook in #addSelectorSilently:withMethod: wraps the new method, as for any compilation."

	(self isBasicMethodSelector: selector) ifTrue: [ ^ self ].
	^ super recompile: selector from: oldClass'.

fix value: #'LoSubject class' value: #removeSelector:
	value: [ :source | (source includesSubstring: 'includesLocalSelector:') not ]
	value:
'removeSelector: selector
	| basicSelector |
	(self isBasicMethodSelector: selector) ifTrue: [
		^ LoSubjectBasicMethodChangeForbiddenError signal ].
	super removeSelector: selector.
	basicSelector := self basicMethodSelectorFrom: selector.
	(self includesLocalSelector: basicSelector) ifTrue: [ super removeSelector: basicSelector ]'.

"Pharo 14 Fileouter>>addFullClass: reads #localMethods, not #localSelectors."
add value: #'LoSubject class' value: #localMethods value: 'accessing method dictionary' value:
'localMethods
	"The generated basic methods are not part of the source of a subject class (compare #localSelectors)."

	^ self localSelectors collect: [ :each | self compiledMethodAt: each ]'.

"Wrap the methods of all the subject classes that were loaded before the hook existed (the package does the same in LoSubject class>>initialize).  The class is reached through Smalltalk globals: this file is compiled as one chunk."
(Smalltalk globals at: #LoSubject ifAbsent: [ nil ])
	ifNotNil: [ :subjectClass | subjectClass initialize ].

"G2: LoSubjectAutomaticCodeGenerationTest"
fix value: #LoSubjectAutomaticCodeGenerationTest value: #setUp
	value: [ :source | source includesSubstring: 'silentlyNewSubclassOf:' ]
	value:
'setUp
	super setUp.
	factory := ClassFactoryForTestCase environment: self class environment.
	class := factory silentlyMake: [ :aBuilder |
		aBuilder
			superclass: LoSubject;
			slots: { #a. #b } ]'.

fix value: #LoSubjectAutomaticCodeGenerationTest value: #testGeneratingSupportMethodsUponInheritance
	value: [ :source | source includesSubstring: 'newSubclassOf:' ]
	value:
'testGeneratingSupportMethodsUponInheritance
	| newClass |
	newClass := factory make: [ :aBuilder | aBuilder slots: { #x } ].
	newClass compile: ''x: anObject
		x := anObject''.
	self assertCollection: newClass localSelectors hasSameElements: #(x:).
	newClass := factory update: newClass to: [ :aBuilder | aBuilder superclass: LoSubject ].
	self assert: newClass superclass == LoSubject.
	self assertCollection: newClass basicLocalSelectors hasSameElements: #(x: basicSubjectMethod_x:)'.

fix value: #LoSubjectAutomaticCodeGenerationTest value: #testRemovingGeneratedSupportMethodsUponInheritance
	value: [ :source | source includesSubstring: 'newSubclassOf:' ]
	value:
'testRemovingGeneratedSupportMethodsUponInheritance
	| newClass |
	newClass := factory make: [ :aBuilder |
		aBuilder
			superclass: LoSubject;
			slots: { #x } ].
	newClass compile: ''x: anObject
		x := anObject''.
	self assertCollection: newClass basicLocalSelectors hasSameElements: #(x: basicSubjectMethod_x:).
	newClass := factory update: newClass to: [ :aBuilder | aBuilder superclass: Object ].
	self assert: newClass superclass == Object.
	self assertCollection: newClass localSelectors hasSameElements: #(x:)'.

fix value: #LoSubjectAutomaticCodeGenerationTest value: #testFileOutIngnoresGeneratedMethods
	value: [ :source | source includesSubstring: 'fileOutOn:' ]
	value:
'testFileOutIngnoresGeneratedMethods
	| fileOutStream |
	class compile: ''a: anObject
		a := anObject''.
	fileOutStream := String new writeStream.
	Fileouter new
		addFullClass: class;
		fileoutOnStream: fileOutStream.
	self deny: (fileOutStream contents includesSubstring: #basicSubjectMethod_a:)'.

"G3: LoArrayAutomaticEventGenerationTest>>testReset"
fix value: #Array value: #asSubject
	value: [ :source | (source includesSubstring: 'isReadOnlyObject') not ]
	value:
'asSubject
	| subject |
	subject := LoSubjectArray new: self size.
	self withIndexDo: [ :value :index | subject at: index put: value ].
	"A literal array is read-only in Pharo 14 (the compiler sends beReadOnlyLiteral), and the become primitive refuses a read-only receiver (no modification).  The identity of the receiver must change in place, so make the receiver writable first."
	self isReadOnlyObject ifTrue: [ self beWritableObject ].
	self becomeForward: subject'.

"G4: LoSortedCollectionAutomaticEventGenerationTest"
fix value: #LoSubjectSortedCollection value: #addLast:
	value: [ :source | source includesSubstring: 'super addLast:' ]
	value:
'addLast: addedObject
	"SortedCollection refuses #addLast: with ShouldNotImplement.  A subject sorted collection appends the element without sorting and sends the add event, as its tests expect."

	self addNoSort: addedObject.
	self dispatchEventAdded: addedObject at: self size.
	^ addedObject'.

add value: #LoSubjectSortedCollection value: #addAll: value: 'adding' value:
'addAll: aCollection
	"SortedCollection>>addAll: adds the elements with #addNoSort: and sorts once, so #add: is not sent and no event is dispatched.  Send #add: for each element."

	aCollection do: [ :each | self add: each ].
	^ aCollection'.

"G5: Spec 2 port of the three presenter classes (the classes do not exist after the load, so they are made here).  Spec 2 classes are reached through Smalltalk globals: this file is compiled as one chunk."
(Smalltalk globals at: #SpPresenter ifAbsent: [ nil ]) ifNotNil: [ :presenterClass |
	(Smalltalk globals includesKey: #LoDicePresenter) ifFalse: [
		(presenterClass << #LoDicePresenter)
			slots: { #diceFaceUpPresenter };
			tag: 'Example';
			package: 'LightweightObserver';
			install ].
	(Smalltalk globals includesKey: #LoDiceListPresenter) ifFalse: [
		(presenterClass << #LoDiceListPresenter)
			slots: { #dicePresenters };
			tag: 'Example';
			package: 'LightweightObserver';
			install ].
	(Smalltalk globals includesKey: #LoDiceAdderPresenter) ifFalse: [
		(presenterClass << #LoDiceAdderPresenter)
			slots: { #totalPresenter. #rollButton. #addDiceButton. #removeDiceButton. #diceGroupPresenter };
			tag: 'Example';
			package: 'LightweightObserver';
			install ] ].

"LoDicePresenter"
add value: #'LoDicePresenter class' value: #title value: 'specs' value: 'title
	^ ''Dice (Click to Roll)'''.
add value: #'LoDicePresenter class' value: #exampleBasic value: 'examples' value: 'exampleBasic
	<example>
	^ self new
		linkTo: LoDice new;
		open;
		yourself'.
add value: #'LoDicePresenter class' value: #example32FacesDice value: 'examples' value: 'example32FacesDice
	<example>
	^ self new
		linkTo: (LoDice of: 32);
		open;
		yourself'.
add value: #'LoDicePresenter class' value: #exampleAlphabetDice value: 'examples' value: 'exampleAlphabetDice
	<example>
	^ self new
		linkTo: (LoDice faces: Character alphabet asUppercase);
		open;
		yourself'.
add value: #LoDicePresenter value: #initializePresenters value: 'initialization' value: 'initializePresenters
	diceFaceUpPresenter := self newButton.
	self diceFaceUp: ''-''.
	self font: self defaultFont.
	self color: self defaultColor.
	self layout: (SpBoxLayout newTopToBottom
		add: diceFaceUpPresenter;
		yourself)'.
add value: #LoDicePresenter value: #initializeWindow: value: 'initialization' value: 'initializeWindow: aWindowPresenter
	aWindowPresenter
		title: self class title;
		initialExtent: 300 @ 200'.
add value: #LoDicePresenter value: #color: value: 'initialization' value: 'color: aColor
	(diceFaceUpPresenter respondsTo: #color:) ifTrue: [ diceFaceUpPresenter color: aColor ]'.
add value: #LoDicePresenter value: #font: value: 'initialization' value: 'font: aFont
	(diceFaceUpPresenter respondsTo: #font:) ifTrue: [ diceFaceUpPresenter font: aFont ]'.
add value: #LoDicePresenter value: #defaultColor value: 'queries' value: 'defaultColor
	^ Color yellow'.
add value: #LoDicePresenter value: #defaultFont value: 'queries' value: 'defaultFont
	^ LogicalFont familyName: StandardFonts defaultFont familyName pointSize: 42'.
add value: #LoDicePresenter value: #diceFaceUp: value: 'initialization' value: 'diceFaceUp: newValue
	diceFaceUpPresenter label: newValue asString'.
add value: #LoDicePresenter value: #diceFaceUpPresenter value: 'accessing' value: 'diceFaceUpPresenter
	^ diceFaceUpPresenter'.
add value: #LoDicePresenter value: #diceFaceUpString value: 'accessing' value: 'diceFaceUpString
	^ diceFaceUpPresenter label asString'.
add value: #LoDicePresenter value: #linkTo: value: 'initialization' value: 'linkTo: aDice
	self diceFaceUp: aDice faceUp.
	aDice afterChangeOf: #faceUp do: [ :newValue | self diceFaceUp: newValue ].
	diceFaceUpPresenter action: [ aDice roll ]'.

"LoDiceListPresenter"
add value: #'LoDiceListPresenter class' value: #exampleBasicDice value: 'example' value: 'exampleBasicDice
	<example>
	| presenter diceList |
	presenter := self new.
	diceList := LoDiceList withDiceCount: 3.
	presenter linkTo: diceList.
	presenter open.
	^ diceList'.
add value: #'LoDiceListPresenter class' value: #exampleProvidedDice value: 'example' value: 'exampleProvidedDice
	<example>
	| presenter diceList |
	diceList := LoDiceList with: {
		LoDice faces: Character alphabet asUppercase.
		LoDice of: 32 }.
	presenter := self new.
	presenter linkTo: diceList.
	presenter open.
	^ diceList'.
add value: #LoDiceListPresenter value: #initialize value: 'initialization' value: 'initialize
	dicePresenters := OrderedCollection new.
	super initialize'.
add value: #LoDiceListPresenter value: #initializePresenters value: 'initialization' value: 'initializePresenters
	self layout: SpBoxLayout newLeftToRight'.
add value: #LoDiceListPresenter value: #initializeWindow: value: 'initialization' value: 'initializeWindow: aWindowPresenter
	aWindowPresenter title: ''Dice Group'''.
add value: #LoDiceListPresenter value: #dicePresenters value: 'accessing' value: 'dicePresenters
	^ dicePresenters'.
add value: #LoDiceListPresenter value: #addPresenterForDice: value: 'protocol' value: 'addPresenterForDice: aDice
	| dicePresenter |
	dicePresenter := self instantiate: LoDicePresenter.
	dicePresenter linkTo: aDice.
	dicePresenters add: dicePresenter.
	self layout add: dicePresenter'.
add value: #LoDiceListPresenter value: #removePresenterAt: value: 'protocol' value: 'removePresenterAt: index
	self layout remove: (dicePresenters removeAt: index)'.
add value: #LoDiceListPresenter value: #linkTo: value: 'initialization' value: 'linkTo: aDiceList
	aDiceList dice do: [ :aDice | self addPresenterForDice: aDice ].
	aDiceList
		afterAddToCollection: #dice
		do: [ :newDice | self addPresenterForDice: newDice ].
	aDiceList
		afterRemoveFromCollection: #dice
		do: [ :oldDice :oldDiceIndex | self removePresenterAt: oldDiceIndex ]'.

"LoDiceAdderPresenter"
add value: #'LoDiceAdderPresenter class' value: #example value: 'examples' value: 'example
	<example>
	| presenter diceGroup |
	presenter := self new.
	diceGroup := LoDiceAdder withDiceCount: 3.
	presenter linkTo: diceGroup.
	presenter open.
	^ presenter'.
add value: #LoDiceAdderPresenter value: #initializePresenters value: 'initialization' value: 'initializePresenters
	diceGroupPresenter := self instantiate: LoDiceListPresenter.
	totalPresenter := self newButton.
	totalPresenter label: ''0''.
	self font: self majorFont of: totalPresenter.
	self color: Color green of: totalPresenter.
	addDiceButton := self newButton.
	addDiceButton label: ''+''.
	self font: self minorFont of: addDiceButton.
	removeDiceButton := self newButton.
	removeDiceButton label: ''-''.
	self font: self minorFont of: removeDiceButton.
	rollButton := self newButton.
	rollButton label: ''Roll Dice''.
	self font: self minorFont of: rollButton.
	self layout: (SpBoxLayout newTopToBottom
		add: diceGroupPresenter;
		add: (SpBoxLayout newLeftToRight
			add: addDiceButton;
			add: removeDiceButton;
			yourself) expand: false;
		add: totalPresenter;
		add: rollButton expand: false;
		yourself)'.
add value: #LoDiceAdderPresenter value: #initializeWindow: value: 'initialization' value: 'initializeWindow: aWindowPresenter
	aWindowPresenter
		title: ''Dice Game'';
		initialExtent: 360 @ 440'.
add value: #LoDiceAdderPresenter value: #font:of: value: 'initialization' value: 'font: aFont of: aButton
	(aButton respondsTo: #font:) ifTrue: [ aButton font: aFont ]'.
add value: #LoDiceAdderPresenter value: #color:of: value: 'initialization' value: 'color: aColor of: aButton
	(aButton respondsTo: #color:) ifTrue: [ aButton color: aColor ]'.
add value: #LoDiceAdderPresenter value: #majorFont value: 'initialization' value: 'majorFont
	^ LogicalFont familyName: StandardFonts defaultFont familyName pointSize: 42'.
add value: #LoDiceAdderPresenter value: #minorFont value: 'initialization' value: 'minorFont
	^ LogicalFont familyName: StandardFonts defaultFont familyName pointSize: 24'.
add value: #LoDiceAdderPresenter value: #addDiceButton value: 'accessing' value: 'addDiceButton
	^ addDiceButton'.
add value: #LoDiceAdderPresenter value: #removeDiceButton value: 'accessing' value: 'removeDiceButton
	^ removeDiceButton'.
add value: #LoDiceAdderPresenter value: #rollButton value: 'accessing' value: 'rollButton
	^ rollButton'.
add value: #LoDiceAdderPresenter value: #totalPresenter value: 'accessing' value: 'totalPresenter
	^ totalPresenter'.
add value: #LoDiceAdderPresenter value: #diceListPresenter value: 'accessing' value: 'diceListPresenter
	^ diceGroupPresenter'.
add value: #LoDiceAdderPresenter value: #dicePresenters value: 'accessing' value: 'dicePresenters
	^ self diceListPresenter dicePresenters'.
add value: #LoDiceAdderPresenter value: #totalString value: 'accessing' value: 'totalString
	^ self totalPresenter label asString'.
add value: #LoDiceAdderPresenter value: #totalValue: value: 'initialization' value: 'totalValue: newTotal
	^ totalPresenter label: newTotal asString'.
add value: #LoDiceAdderPresenter value: #linkTo: value: 'initialization' value: 'linkTo: aDiceGroup
	diceGroupPresenter linkTo: aDiceGroup.
	addDiceButton action: [ aDiceGroup addDice ].
	removeDiceButton action: [ aDiceGroup removeDice ].
	rollButton action: [ aDiceGroup roll ].
	aDiceGroup
		afterChangeOf: #total
		do: [ :newTotal | self totalValue: newTotal ].
	self totalValue: aDiceGroup total'.

"The tests click with #performAction.  Add it only if SpButtonPresenter has no such message (ask the image: SpButtonPresenter selectors)."
(Smalltalk globals at: #SpButtonPresenter ifAbsent: [ nil ]) ifNotNil: [ :buttonClass |
	(buttonClass canUnderstand: #performAction) ifFalse: [
		add value: #SpButtonPresenter value: #performAction value: '*LightweightObserver' value: 'performAction
	"Simulate a click: run the action block."

	^ (self respondsTo: #action)
		ifTrue: [ self action ifNotNil: [ :aBlock | aBlock cull: self ] ]
		ifFalse: [ self click ]' ] ].


"PharoMisc, SimpleMiddleware.

G7. SmSingleMiddlewareTest>>testCommunication (ConnectionClosed after 8 ms).  SmServer>>defaultPort is 54321, and the test uses the default.  On the test machine another program listens on 127.0.0.1:54321 (lsof, 2026-11-17).  SmServer listens on the wildcard address with the address reuse option, and macOS lets that bind go through.  The proxy connects to 127.0.0.1:54321, the other program gets the connection and closes it.  SmThreeMiddlewareTest uses other ports and passes.  The test now asks the system for a free port."

fix value: #SmSingleMiddlewareTest value: #setUp
	value: [ :source | (source includesSubstring: 'listenOn: 0') not ]
	value:
'setUp
	"The default port of SmServer (54321) can be in use by another program of the machine.  A program that listens on 127.0.0.1:54321 gets the connection of the proxy and closes it, so the test read ConnectionClosed.  The middleware of this test uses a free port that the system chooses."

	| probe freePort |
	super setUp.
	probe := Socket newTCP.
	[ probe listenOn: 0 backlogSize: 1.
	freePort := probe localPort ] ensure: [ probe closeAndDestroy ].
	middleware := SmMiddleware port: freePort'.

"PharoMisc, NetworkExtras.

G9. NeMulticastSocketTest>>testInterface (2 expected failures: NeLoopbackAddressMulticastSocketTest, NeRoutedAddressMulticastSocketTest).  The test was marked <expectedFailure> and ended with 'self fail: #todo'; the author never wrote the second half.  The package code under the test is also wrong.  NeMulticastSocket>>networkInterfaceIp: gives an NeIpV4Address to Socket>>setOption:value:, which sends asString.  NeIpV4Address>>asString answers the dotted text ('127.0.0.1', 9 bytes), and the plugin passes those 9 bytes to setsockopt(IP_MULTICAST_IF), which needs the 4 bytes of the address (struct in_addr).  The option is then set to the text bytes or refused, depending on the system.  The method now gives a plain ByteArray with the 4 bytes (ByteArray>>asString keeps the bytes; an NeIpV4Address is a ByteArray subclass, so asByteArray would answer the same object and the dotted asString would win again).  NeMulticastSocket>>networkInterface answered the raw {0. int} of getOption:, with the 4 bytes of the address as one host-order int; it now answers the NeIpV4Address (the system reads in_addr in memory order, so the first byte is the low byte on a little-endian machine, which is every machine of the supported systems).  The test uses the loopback interface (127.0.0.1) and not the address 10.89.2.24 of the network of the author."

fix value: #NeMulticastSocket value: #networkInterface
	value: [ :source | (source includesSubstring: 'NeIpV4Address') not ]
	value:
'networkInterface
	"The IP address of the interface that sends my multicast datagrams (option IP_MULTICAST_IF).  NeIpV4Address allNetworkInterfaces (0.0.0.0) means that the system chooses.  The system answers the 4 bytes of the address as one int in the byte order of the machine, first address byte in the low byte."

	| value |
	value := (self getOption: ''IP_MULTICAST_IF'') last.
	^ NeIpV4Address newFrom: ((0 to: 3) collect: [ :index | (value >> (8 * index)) bitAnd: 255 ])'.

fix value: #NeMulticastSocket value: #networkInterfaceIp:
	value: [ :source | (source includesSubstring: 'ByteArray') not ]
	value:
'networkInterfaceIp: newAddress
	"Socket>>setOption:value: sends asString to the value, and NeIpV4Address>>asString is the dotted text.  The option needs the 4 bytes of the address, so give a plain ByteArray."

	networkInterfaceIp := newAddress asIpAddress.
	self setOption: ''IP_MULTICAST_IF'' value: (ByteArray withAll: networkInterfaceIp)'.

fix value: #NeMulticastSocketTest value: #testInterface
	value: [ :source | source includesSubstring: 'todo' ]
	value:
'testInterface
	"The loopback interface exists on every machine; the test does not need the network of the author."

	| loopback |
	loopback := NeIpV4Address loopback.
	self assert: socket networkInterface equals: NeIpV4Address allNetworkInterfaces.
	socket networkInterfaceIp: loopback.
	self assert: socket networkInterface equals: loopback.
	self assert: socket networkInterfaceIp equals: loopback.
	socket networkInterfaceIp: ''127.0.0.1''.
	self assert: socket networkInterface equals: loopback'.

"PharoMisc, NetworkExtras: one socket listens on a port, and a sender does not bind (G10 to G13).

The tests of the multicast sockets made the sender as a second NeMulticastSocket on the port of the receiver.  The stock VM does not report the failed bind of the second socket (scripts/cogdiff/socket_bind_in_use.st), so the second socket stayed unbound there with no error, and the tests passed.  Our VM reports the error.  A first repair here (reuse options before the bind) made three sockets listen on one port, and the system then gave a unicast datagram to only one of them: testRestartAfterStop of the loopback variant failed on the two VMs."

"PharoMisc, NetworkExtras.

G10. A multicast socket that only sends.  NeMulticastSocket>>group:port: binds the port, joins the group, and registers the port.  A sender of a multicast datagram needs none of that, and NeUdpSocket>>send: needs only targetIp and targetPort (the group and the port).  A test that makes a second bound socket on the port of a receiver gets a different result on each system: a bind that fails and is not reported (stock VM), an error (our VM), or, with the reuse options, a split of the unicast datagrams of the loopback group between the sockets (the oldest socket gets the datagram on macOS, the hash of the addresses decides on Linux).  NeMulticastSocket class>>sendToGroup:port: makes a socket that does not bind, does not join, and does not register the port in the tracker; its close does not leave a group, and its destroy does not unregister the port of the receiver.  The method needs one new instance variable, isSendOnly."

(resolve value: 'NeMulticastSocket')
	ifNil: [ Stdio stdout nextPutAll: 'SKIP NeMulticastSocket instVar isSendOnly (class absent)'; lf; flush ]
	ifNotNil: [ :multicastClass |
		(multicastClass instVarNames includes: #isSendOnly)
			ifTrue: [ Stdio stdout nextPutAll: 'SKIP NeMulticastSocket instVar isSendOnly (already present)'; lf; flush ]
			ifFalse: [
				multicastClass addInstVarNamed: 'isSendOnly'.
				Stdio stdout nextPutAll: 'ADDED NeMulticastSocket instVar isSendOnly'; lf; flush ] ].

add value: #'NeMulticastSocket class' value: #sendToGroup:port: value: 'instance creation'
	value:
'sendToGroup: groupIp port: anInteger
	"A socket that only sends to the group and the port.  It does not bind the port, and it does not join the group."

	^ self new
		sendToGroup: groupIp port: anInteger;
		yourself'.

add value: #NeMulticastSocket value: #sendToGroup:port: value: 'initialize-destroy'
	value:
'sendToGroup: anIpAddress port: aPortNumber
	"The group IP address must be within the range: 224.0.0.0 - 239.255.255.255 (or the loopback address).
	I only send.  The system gives me a free port when I send, so I do not take the port of a receiver of the group.
	The port is not registered: the tracker keeps one entry for each socket that listens on a port, and I do not listen."

	self ensureValidPort: aPortNumber.
	isSendOnly := true.
	receptionPort := aPortNumber.
	self groupAddress: anIpAddress'.

add value: #NeMulticastSocket value: #isSendOnly value: 'testing'
	value:
'isSendOnly
	^ isSendOnly == true'.

add value: #NeMulticastSocket value: #unregisterUsedPort value: 'reception ports tracking'
	value:
'unregisterUsedPort
	"A socket that only sends did not register its port.  The port belongs to the receiver."

	self isSendOnly ifTrue: [ ^ self ].
	super unregisterUsedPort'.

fix value: #NeMulticastSocket value: #close
	value: [ :source | (source includesSubstring: 'isSendOnly') not ]
	value:
'close
	"A socket that only sends never joined the group."

	[ self isSendOnly ifFalse: [ self leaveGroup ] ] ensure: [
		super close ]'.

"PharoMisc, NetworkExtras.

G11. The tests of the multicast sockets and servers made the sender as a second NeMulticastSocket on the port of the receiver, with the hack 'unregisterUsedPort' to get round the tracker.  Two sockets bound to one port: see G10.  NeLoopbackAddressMulticastUdpServerTest>>testRestartAfterStop failed in the second round because of this: NeMulticastUdpServerTest>>startServer runs once in setUp and once more in each round of the test, and each run made a new bound sender; the sender of setUp (the oldest socket of the port after the first server is stopped) was never closed and took the datagram of the second round (a unicast datagram to the loopback address goes to the oldest socket on macOS).  The sender is now a socket that only sends (G10), and no test needs the hack.  Each test still checks what it checked: the receiver gets the datagram that a sender of the group sends, and datagrams to another group do not arrive."

add value: #NeUdpSocketTest value: #newMulticastSenderOnGroup:port: value: 'testing'
	value:
'newMulticastSenderOnGroup: groupIp port: port
	^ self createSocket: [ NeMulticastSocket sendToGroup: groupIp port: port ]'.

fix value: #NeMulticastUdpServerTest value: #startServer
	value: [ :source | source includesSubstring: 'unregisterUsedPort' ]
	value:
'startServer
	"The sender only sends.  It does not bind the port of the server: two sockets bound to one port split the datagrams between them in a way that depends on the system."

	server start.
	sender := self newMulticastSenderOnGroup: multicastGroup port: multicastPort'.

fix value: #NeMulticastSocketTest value: #testSendReceive
	value: [ :source | source includesSubstring: 'unregisterUsedPort' ]
	value:
'testSendReceive
	"The sender only sends, so the order of creation does not matter."

	| sender receiver |
	receiver := socket.
	sender := self
		          newMulticastSenderOnGroup: self groupIp
		          port: multicastPort.
	sender send: ''Hello''.
	self assert: (receiver receiveTimeout: 0.1) asString equals: ''Hello'''.

fix value: #NeRoutedAddressMulticastSocketTest value: #testSendReceiveOnDifferentGroups
	value: [ :source | source includesSubstring: 'unregisterUsedPort' ]
	value:
'testSendReceiveOnDifferentGroups

	| sender1 receiver1 receiver2 multicastPort2 otherGroupIp sender2 message1 message2 |
	receiver1 := socket.
	sender1 := self
		           newMulticastSenderOnGroup: self groupIp
		           port: multicastPort.
	multicastPort2 := NeUdpUsedPortsTracker freePortAtRandom.
	otherGroupIp := ''226.1.1.42''.
	self deny: otherGroupIp asIpAddress equals: self groupIp asIpAddress.
	receiver2 := self
		             newMulticastSocketOnGroup: otherGroupIp
		             port: multicastPort2.
	sender2 := self
		           newMulticastSenderOnGroup: otherGroupIp
		           port: multicastPort2.
	message1 := ''Hello111''.
	message2 := ''Hello222''.
	sender1 send: message1.
	sender2 send: message2.
	self
		assert: (receiver1 receiveTimeout: 0.1) asString
		equals: message1.
	self
		assert: (receiver2 receiveTimeout: 0.1) asString
		equals: message2'.

"PharoMisc, NetworkExtras.

G12. NeUdpLargeDataSender class>>multicastSocketGroup:port: made a NeMulticastSocket that binds the port, for a sender that only sends chunks.  A receiver of the group on the same port (same image: the tracker refuses the bind; another process of the host: the bind fails) cannot work with it.  No test in the package uses this method.  The sender socket now only sends (G10)."

fix value: #'NeUdpLargeDataSender class' value: #multicastSocketGroup:port:
	value: [ :source | (source includesSubstring: 'sendToGroup') not ]
	value:
'multicastSocketGroup: groupIp port: port
	| socket |
	socket := NeMulticastSocket sendToGroup: groupIp port: port.
	^ self socket: socket'.

"PharoMisc, NetworkExtras.

G13. NeUdpUnicastSocketTest>>testCloseWhileReceiving sent a datagram to the receiver and closed the receiver at once, and expected that the reader of the receiver gets no data and ConnectionClosed.  A datagram that is in the buffer of the socket before the close is data that the reader can read: NeUdpSocket>>receiveTimeout: and Socket>>waitForDataFor:ifClosed:ifTimedOut: test dataAvailable first and the connection state only when no data is there.  Which of the two comes first depended on the time that the VM needs to wake the reader (a VM that signals the semaphore of the socket at once lets the reader win).  The test is about a reader that waits and is woken by the close, so no datagram is sent.  The reader sets a flag before it waits, the test goes on only when the flag is set (the reader has a higher priority, so the reader is then blocked in the wait), and the wait of the test is shorter than the timeout of the reader, so the wake-up must come from the close and not from the timeout."

fix value: #NeUdpUnicastSocketTest value: #testCloseWhileReceiving
	value: [ :source | source includesSubstring: 'sender send:' ]
	value:
'testCloseWhileReceiving
	"A reader that waits for a datagram gets ConnectionClosed when the socket is closed.  No datagram is sent: a datagram that arrives before the close is read by the reader, so a test that sends one depends on the time that the VM needs to wake the reader."

	| isDataReceived isErrorSignaled isTimedOut isWaiting |
	isDataReceived := false.
	isErrorSignaled := false.
	isTimedOut := false.
	isWaiting := false.
	[
	[
	[
	isWaiting := true.
	receiver receiveTimeout: 2.
	isDataReceived := true ]
		on: ConnectionClosed
		do: [ isErrorSignaled := true ] ]
		on: ConnectionTimedOut
		do: [ isTimedOut := true ] ] forkAt: Processor userInterruptPriority.
	"The reader has a higher priority: it has run up to the wait."
	self assert: isWaiting.
	receiver closeAndDestroy.
	self
		waitUntil: [ isDataReceived | isErrorSignaled | isTimedOut ]
		timeout: 500 milliSeconds. "Shorter than the 2 seconds of the reader: the close must wake the reader."
	self deny: isDataReceived.
	self deny: isTimedOut.
	self assert: isErrorSignaled'.
