fix value: #ClyBrowserMorph value: #browseImplementorsOf:inNameResolver: value: [ :s | (s includesSubstring: 'spawned') not and: [ s includesSubstring: '^ #NormalImplementorBrowse' ] ] value: 'browseImplementorsOf: aSymbol inNameResolver: aNameResolver
	| classBinding classToBrowse spawned |
	aSymbol first isUppercase ifTrue: [
		aNameResolver 
			ifNil: [ classToBrowse := self class environment at: aSymbol.
						^ { ClyFullBrowserMorph openOnClass: classToBrowse.
						#BrowseClassWithoutNameResolver} ]
			ifNotNil: [ 
		classBinding := aNameResolver bindingOf: aSymbol.
	   
		classBinding ifNotNil: [
			classToBrowse := classBinding definingClass ifNil: [ classBinding value ].
			classToBrowse isClassOrTrait ifFalse: [ classToBrowse := classToBrowse class ].
			self spawnBrowser: ClyFullBrowserMorph withState: [ :browser |
				browser selectClass: classToBrowse].
			^ #NormalBrowseClass ] ] ].

	self
		spawnQueryBrowserOn: (ClyMessageImplementorsQuery of: aSymbol)
		withState: [ :browser | spawned := browser ].
	^ { spawned. #NormalImplementorBrowse }'.
fix value: #ClyBrowserMorph value: #spawnQueryBrowserOn:withState: value: [ :s | s includesSubstring: 'toolNamed: #messageList' ] value: 'spawnQueryBrowserOn: aQuery withState: navigationBlock
	| targetQuery |
	targetQuery := aQuery.
	aQuery isBoundToEnvironment ifFalse: [
		targetQuery := aQuery withScope: self defaultNavigationScope].

	targetQuery scope = self systemScope ifTrue: [
		"If query is empty in system (global) scope then it is empty in any other scope.
		In that case we do not need a query browser because it will be always empty and useless"
		targetQuery semiAsync hasEmptyResult ifTrue: [
			"For slow queries we will not wait and open a browser to indicate execution progress"
			^ InformativeNotification signal:  ''There are no '', targetQuery description ]  ].

	"The message list tool of the Smalltalk tools registry is the NewTools StMethodBrowser, which does not
	understand the Calypso protocol used here (on:systemScope:, queryScopes:, showResultOf:).  The Calypso
	query browser does."
	self 
		spawnBrowser: ClyQueryBrowserMorph
		withState: [ :browser |
			browser queryScopes: self allNavigationScopes.
			browser showResultOf: targetQuery.
			navigationBlock valueWithPossibleArgs: {browser} ]'.
fix value: #NavigationInteractionTest value: #testBrowseMethodViaImplementorActuallyBrowseTheClass value: [ :s | s includesSubstring: 'self skip' ] value: 'testBrowseMethodViaImplementorActuallyBrowseTheClass

	| browser res |
	"The browser must be built and opened: the method implementors query is scoped by the package view of the full browser."
	browser := ClyFullBrowserMorph openOnClass: Object.
	[ res := browser browseImplementorsOf: #add: inNameResolver: Object.
	self assert: res second equals: #NormalImplementorBrowse.
	self assert: res first isNotNil ] ensure: [
		res ifNotNil: [ res first ifNotNil: [ :spawned | spawned close ] ].
		browser close ]'.
