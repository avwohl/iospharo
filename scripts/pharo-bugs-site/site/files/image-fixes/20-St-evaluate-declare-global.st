fix value: #StObjectContextPresenterTest value: #testDoEvaluateDeclareGlobal
 value: [ :s | s includesSubstring: 'self skip:' ]
 value: 'testDoEvaluateDeclareGlobal
	| value oldManager stubClass |
	self deny: (Smalltalk globals includesKey: #MyGlobalForTest).

	self openInstance.

	presenter evaluator text: ''MyGlobalForTest''.
	presenter whenActivatedDo: [ :selection | value := selection value ].

	"The compiler offers to repair the undeclared variable with a menu (OCCodeReparator>>openMenu). Answer the entry that declares a global."
	oldManager := UIManager default.
	stubClass := oldManager class newAnonymousSubclass.
	stubClass compile: ''chooseFrom: labels lines: lines title: title
	^ labels indexOf: ''''Declare new global variable'''' ifAbsent: [ 0 ]''.
	[ UIManager default: stubClass new.
	presenter doEvaluateAndGo ] ensure: [ UIManager default: oldManager ].

	self assert: value equals: nil.

	self assert: (Smalltalk globals includesKey: #MyGlobalForTest).
	Smalltalk globals removeKey: #MyGlobalForTest'.

fix value: #StPlaygroundPagePresenterTest value: #testDoEvaluateDeclareGlobal
 value: [ :s | s includesSubstring: 'self skip:' ]
 value: 'testDoEvaluateDeclareGlobal
	| value oldManager stubClass |
	self deny: (Smalltalk globals includesKey: #MyGlobalForTest).

	self openInstance.

	presenter contents: ''MyGlobalForTest''.

	presenter text selectionInterval: (1 to: 16).
	presenter whenActivatedDo: [ :selection | value := selection value ].

	"The compiler offers to repair the undeclared variable with a menu (OCCodeReparator>>openMenu). Answer the entry that declares a global."
	oldManager := UIManager default.
	stubClass := oldManager class newAnonymousSubclass.
	stubClass compile: ''chooseFrom: labels lines: lines title: title
	^ labels indexOf: ''''Declare new global variable'''' ifAbsent: [ 0 ]''.
	[ UIManager default: stubClass new.
	presenter doEvaluateAndGo ] ensure: [ UIManager default: oldManager ].

	self assert: value equals: nil.

	self assert: (Smalltalk globals includesKey: #MyGlobalForTest).
	Smalltalk globals removeKey: #MyGlobalForTest'.

fix value: #OCCodeReparator value: #openMenu
 value: [ :s | (s includesSubstring: 'declareGlobal') not and: [ s includesSubstring: 'labels add: ''Define new trait''.' ] ]
 value: ((OCCodeReparator includesSelector: #openMenu)
	ifTrue: [ (OCCodeReparator >> #openMenu) sourceCode
		copyReplaceAll: 'labels add: ''Define new trait''.'
		with: 'labels add: ''Declare new global variable''.
			actions add: [ self declareGlobal ].
			labels add: ''Define new trait''.' ]
	ifFalse: [ '' ]).
