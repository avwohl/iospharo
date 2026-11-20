"SpCodePresenterTest cluster: three tests that skip themselves.

 1. testDoBrowseClassNotFound: doBrowseClass evaluated the selected text with
    the interactive compiler.  An unknown class name made OCCodeReparator open
    a modal 'Unknown variable' choice menu ('Define new class', ...).  Browsing a
    class must never offer to define one.  Fix: SpCodePresenter gets
    evaluate:onCompileError:onError:permitUndeclared:, and doBrowseClass uses it
    and returns when the text names an undeclared variable.
 2. testEnvironment: doBrowseClass ignored the presenter environment (it only
    asked the class environment for Symbols; a class value was browsed whatever
    the environment).  Fix: doBrowseClass browses a class only when the
    presenter environment holds it.
 3. testDoBrowseMethodReferences: the skip said a dialogue blocks the test.  The
    test passes without the skip (measured on both VMs), so the skip is only
    removed."

(Smalltalk globals includesKey: #SpCodePresenter) ifTrue: [

"--- code under test ---"
add value: #SpCodePresenter value: #'evaluate:onCompileError:onError:permitUndeclared:' value: 'evaluating'
	value: (((SpCodePresenter >> #evaluate:onCompileError:onError:) sourceCode
		copyReplaceAll: 'evaluate: aString onCompileError: compileErrorBlock onError: errorBlock '
		with: 'evaluate: aString onCompileError: compileErrorBlock onError: errorBlock permitUndeclared: aBoolean')
		copyReplaceAll: 'source: aString;'
		with: 'source: aString;
			permitUndeclared: aBoolean;').

fix value: #SpCodePresenter value: #'evaluate:onCompileError:onError:'
	value: [ :src | (src includesSubstring: 'permitUndeclared') not ]
	value: 'evaluate: aString onCompileError: compileErrorBlock onError: errorBlock 
	"evaluate aString. 
	 evaluate compileErrorBlock if there are compilation errors. 
	 evaluate errorBlock is anything happens *during* evaluation (code compiled, but it does not 
	 executes.).  See #evaluate:onCompileError:onError:permitUndeclared: for the details."

	^ self
		evaluate: aString
		onCompileError: compileErrorBlock
		onError: errorBlock
		permitUndeclared: false'.

fix value: #SpCodePresenter value: #doBrowseClass
	value: [ :src | (src includesSubstring: 'permitUndeclared') not ]
	value: 'doBrowseClass
	| result |

	"An unknown name must not open the interactive menu of the compiler (it offers to define a class): there is nothing to browse."
	result := [ self 
		evaluate: self selectedTextOrLine
		onCompileError: [ ^ self ]
		onError: [ :e | ^ self ]
		permitUndeclared: true ]
			on: OCUndeclaredVariableWarning
			do: [ :warning | ^ self ].
	(result isKindOf: Symbol)
		ifTrue: [ 
				| classToBrowse |
				classToBrowse := self environment at: result ifAbsent: [ nil ].	
					classToBrowse ifNotNil: [ ^ self systemNavigation browse: classToBrowse ] ].
	"Only the classes of the environment of the presenter can be browsed"
	(result isKindOf: Behavior) ifTrue: [ | instanceSide |
		instanceSide := result instanceSide.
		(instanceSide isAnonymous not 
			and: [ (self environment at: instanceSide name ifAbsent: [ nil ]) ~~ instanceSide ])
				ifTrue: [ ^ self ] ].
	self systemNavigation browse: result'.

"--- tests ---"
fix value: #SpCodePresenterTest value: #testDoBrowseClassNotFound
	value: [ :src | src includesSubstring: 'self skip' ]
	value: 'testDoBrowseClassNotFound
	| navigation |
	
	navigation := SpCodeSystemNavigationMock new.
	presenter systemNavigation: navigation.
	self openInstance.
	
	presenter text: ''ClassThatDoesNotExits''.
	presenter doBrowseClass.
	
	self assert: navigation browseClass isNil.
	self assert: navigation messageList isNil'.

fix value: #SpCodePresenterTest value: #testDoBrowseMethodReferences
	value: [ :src | src includesSubstring: 'self skip' ]
	value: 'testDoBrowseMethodReferences

	| navigation |
	navigation := SpCodeSystemNavigationMock new.
	presenter systemNavigation: navigation.
	presenter environment add: SpCodePresenterTest binding.
	presenter environment add: SpCodeSystemNavigationMock binding.
	self openInstance.


	presenter text: ''Object''.
	presenter doBrowseMethodReferences.

	self assert: navigation messageList isNil.

	navigation reset.
	presenter text: ''SpCodeSystemNavigationMock''.
	presenter doBrowseMethodReferences.

	self assert: navigation messageList notEmpty.
	self assert: (navigation messageList includes: SpCodePresenterTest >> #testDoBrowseMethodReferences)'.

fix value: #SpCodePresenterTest value: #testEnvironment
	value: [ :src | src includesSubstring: 'self skip' ]
	value: 'testEnvironment
	| environment navigation |
	
	environment := self newEnvironment.
	navigation := SpCodeSystemNavigationMock new.
	
	presenter environment: environment.
	presenter systemNavigation: navigation.
	
	self openInstance.
	
	"we will find Object but not OrderedCollection"
	presenter text: ''Object''.
	presenter doBrowseClass.
	
	self assert: navigation browseClass equals: Object.
	
	navigation reset.
 	presenter text: ''OrderedCollection''.
	presenter doBrowseClass.
	
	self assert: navigation browseClass isNil'.
].
