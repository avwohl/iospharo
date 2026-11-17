fix value: #ClyWrapQueryTest value: #testIsExecutedFromMultipleScopeWhenSubqueriesAreFromScopesOfDifferentObjects value: [ :s | s includesSubstring: '<expectedFailure>' ] value: 'testIsExecutedFromMultipleScopeWhenSubqueriesAreFromScopesOfDifferentObjects
	"A wrapper query wraps exactly one subquery (ClyWrapperQuery>>subqueries:), so the
	multi-subquery setup of the inherited composite test must be refused."
	| subqueriesBefore |
	subqueriesBefore := query subqueries.
	self
		should: [ query subqueries: {
		self subqueryFromScopeOf: self class.
		self subqueryFromScopeOf: self class superclass.
	} ]
		raise: Error
		whoseDescriptionIncludes: ''single subquery''
		description: ''A wrapper query must refuse several subqueries''.
	self assert: query subqueries equals: subqueriesBefore'.
fix value: #ClyWrapQueryTest value: #testIsExecutedFromSingleScopeWhenSubqueriesAreFromDifferentScopesOfSameObject value: [ :s | s includesSubstring: '<expectedFailure>' ] value: 'testIsExecutedFromSingleScopeWhenSubqueriesAreFromDifferentScopesOfSameObject
	"A wrapper query wraps exactly one subquery (ClyWrapperQuery>>subqueries:), so the
	multi-subquery setup of the inherited composite test must be refused."
	| subqueriesBefore |
	subqueriesBefore := query subqueries.
	self
		should: [ query subqueries: {
		self subqueryFromScope: ClyExampleScope of: self class.
		self subqueryFromScope: ClyExample2Scope of: self class.
	} ]
		raise: Error
		whoseDescriptionIncludes: ''single subquery''
		description: ''A wrapper query must refuse several subqueries''.
	self assert: query subqueries equals: subqueriesBefore'.
fix value: #ClyWrapQueryTest value: #testNotExecutedFromMultipleScopeWhenAllSubqueriesAreFromEmptyScope value: [ :s | s includesSubstring: '<expectedFailure>' ] value: 'testNotExecutedFromMultipleScopeWhenAllSubqueriesAreFromEmptyScope
	"A wrapper query wraps exactly one subquery (ClyWrapperQuery>>subqueries:), so the
	multi-subquery setup of the inherited composite test must be refused."
	| subqueriesBefore |
	subqueriesBefore := query subqueries.
	self
		should: [ query subqueries: {
		self subqueryFromScope: ClyExampleScope empty.
		self subqueryFromScope: ClyExample2Scope empty.
	} ]
		raise: Error
		whoseDescriptionIncludes: ''single subquery''
		description: ''A wrapper query must refuse several subqueries''.
	self assert: query subqueries equals: subqueriesBefore'.
fix value: #ClyWrapQueryTest value: #testNotExecutedFromMultipleScopeWhenSubqueriesAreFromDifferentScopesOfSameObject value: [ :s | s includesSubstring: '<expectedFailure>' ] value: 'testNotExecutedFromMultipleScopeWhenSubqueriesAreFromDifferentScopesOfSameObject
	"A wrapper query wraps exactly one subquery (ClyWrapperQuery>>subqueries:), so the
	multi-subquery setup of the inherited composite test must be refused."
	| subqueriesBefore |
	subqueriesBefore := query subqueries.
	self
		should: [ query subqueries: {
		self subqueryFromScope: ClyExampleScope of: self class.
		self subqueryFromScope: ClyExample2Scope of: self class.
	} ]
		raise: Error
		whoseDescriptionIncludes: ''single subquery''
		description: ''A wrapper query must refuse several subqueries''.
	self assert: query subqueries equals: subqueriesBefore'.
fix value: #ClyWrapQueryTest value: #testNotExecutedFromSingleScopeWhenAllSubqueriesAreFromEmptyScope value: [ :s | s includesSubstring: '<expectedFailure>' ] value: 'testNotExecutedFromSingleScopeWhenAllSubqueriesAreFromEmptyScope
	"A wrapper query wraps exactly one subquery (ClyWrapperQuery>>subqueries:), so the
	multi-subquery setup of the inherited composite test must be refused."
	| subqueriesBefore |
	subqueriesBefore := query subqueries.
	self
		should: [ query subqueries: {
		self subqueryFromScope: ClyExampleScope empty.
		self subqueryFromScope: ClyExample2Scope empty.
	} ]
		raise: Error
		whoseDescriptionIncludes: ''single subquery''
		description: ''A wrapper query must refuse several subqueries''.
	self assert: query subqueries equals: subqueriesBefore'.
fix value: #ClyWrapQueryTest value: #testNotExecutedFromSingleScopeWhenSubqueriesAreFromScopesOfDifferentObjects value: [ :s | s includesSubstring: '<expectedFailure>' ] value: 'testNotExecutedFromSingleScopeWhenSubqueriesAreFromScopesOfDifferentObjects
	"A wrapper query wraps exactly one subquery (ClyWrapperQuery>>subqueries:), so the
	multi-subquery setup of the inherited composite test must be refused."
	| subqueriesBefore |
	subqueriesBefore := query subqueries.
	self
		should: [ query subqueries: {
		self subqueryFromScopeOf: self class.
		self subqueryFromScopeOf: self class superclass.
	} ]
		raise: Error
		whoseDescriptionIncludes: ''single subquery''
		description: ''A wrapper query must refuse several subqueries''.
	self assert: query subqueries equals: subqueriesBefore'.
fix value: #ClyWrapQueryTest value: #testHasCompositeScopeFromSubqueries value: [ :s | s includesSubstring: '<expectedFailure>' ] value: 'testHasCompositeScopeFromSubqueries
	"A wrapper query wraps exactly one subquery, so its scope is the scope of that subquery,
	not a ClyCompositeScope (ClyWrapperQuery>>subqueries: refuses several subqueries)."
	| scope subscope |
	scope := query scope.
	subscope := query subqueries anyOne scope.
	self deny: scope class equals: ClyCompositeScope.
	self assert: scope equals: subscope'.
