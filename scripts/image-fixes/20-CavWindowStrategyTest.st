fix value: #CavWindowStrategyTest value: #testExactReplaceStrategyDoesNotChangeExtentAndPosition
	value: [ :source | source includesSubstring: 'self skip.' ]
	value:
'testExactReplaceStrategyDoesNotChangeExtentAndPosition

	| strategy |
	
	"In setUp
	placeHolder := (CavWindowPlaceHolder new position: 101@101; extent: 100@200).
	"
	strategy := CavExactOnTopStrategy new.
	strategy placeHolder: placeHolder.
	strategy placePresenter: presenter.
	
	self assert: presenter extent equals: 100@200.
	self assert: presenter position equals: 101@101
	
	"we cannot test that if there was one window with the same presenter it got removed."'.
