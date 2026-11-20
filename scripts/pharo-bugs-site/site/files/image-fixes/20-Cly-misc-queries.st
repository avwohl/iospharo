fix value: #ClyConstantMethodQueryTest value: #testIsEmptyFromEmptyMethodScope
	value: [ :s | s includesSubstring: '<expectedFailure>' ]
	value: 'testIsEmptyFromEmptyMethodScope
	"Constant query does not depend on the scope. The inherited test expects an empty result from an empty method scope; this query always returns its own methods, so the result is not empty."
	query scope: (ClyMethodScope emptyIn: environment).

	self deny: query checkEmptyResult'.

fix value: #ClyUnknownQueryTest value: #testIsBoundToEnvironmentWhenScopeIs
	value: [ :s | s includesSubstring: '<expectedFailure>' ]
	value: 'testIsBoundToEnvironmentWhenScopeIs
	"Unknown query is never bound to the environment"
	self deny: query scope isBoundToEnvironment.
	self deny: query isBoundToEnvironment'.

fix value: #ClyRestUntaggedClassesQueryTest value: #testIsEmptyFromEmptyClassScope
	value: [ :s | s includesSubstring: '<expectedFailure>' ]
	value: 'testIsEmptyFromEmptyClassScope
	"This query supports only package scope, so the empty scope is a package scope"
	query scope: (ClyPackageScope emptyIn: environment).

	self assert: query checkEmptyResult'.

fix value: #ClyUnimplementedMethodsQueryTest value: #testIsEmptyFromEmptyMethodScope
	value: [ :s | s includesSubstring: '<expectedFailure>' ]
	value: 'testIsEmptyFromEmptyMethodScope
	"This query only supports class, package and global scopes
	because it analyses classes and only then methods.
	So the empty scope is a package scope"
	query scope: (ClyPackageScope emptyIn: environment).

	self assert: query checkEmptyResult'.

fix value: #ClyExtendingPackagesQueryTest value: #testFromClassWhichTraitIsExtendedButNotItself
	value: [ :s | s includesSubstring: '<expectedFailure>' ]
	value: 'testFromClassWhichTraitIsExtendedButNotItself
	"The methods of a class include the methods that come from its traits (ClyLocalClassScope>>methodsDo: uses Behavior>>methods).
	ClyClassWithTraits uses ClyTraitRoot1, which another package extends, so that package extends the methods of the class."

	self queryFromScope: ClyClassScope of: ClyClassWithTraits.
	self assert: resultItems asSet equals: { (ClyTraitRoot1 >> #extensionMethodFromRoot1Trait) package } asSet'.
