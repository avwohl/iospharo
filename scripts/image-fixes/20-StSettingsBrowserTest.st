fix value: #StSettingsBrowserTest value: #testSimulateCategoryPopulation
	value: [ :s | s includesSubstring: 'self skip' ]
	value: 'testSimulateCategoryPopulation

	| firstCategory shownSettings |
	firstCategory := categoriesPresenter roots first.
	categoriesPresenter selectItem: firstCategory.
	self
		assert: detailsPresenter model
		identicalTo: firstCategory.
	"the details presenter replaced its placeholder label by the presenter of the category"
	shownSettings := detailsPresenter presenters first.
	self deny: (shownSettings isKindOf: SpLabelPresenter).
	self
		assert: shownSettings layout children notEmpty
		description: ''It tests that after a category selection has been made, the details presenter contains the settings of the category'''.
