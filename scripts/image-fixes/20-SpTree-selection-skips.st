fix value: #SpTreeAdapterSingleSelectionTest value: #testSelectPresenterItemSetsSelectedIndexInWidget value: [ :source | source includesSubstring: 'self skip' ] value: 'testSelectPresenterItemSetsSelectedIndexInWidget

	presenter selectItem: 20.
	self assert: (self adapter selectedPaths includes: #(2))'.
fix value: #SpTreeAdapterSingleSelectionTest value: #testUnselectPresenterItemUnselectsIndexInWidget value: [ :source | source includesSubstring: 'self skip' ] value: 'testUnselectPresenterItemUnselectsIndexInWidget

	presenter
		selectItem: 20;
		unselectItem: 20.
	self assert: self adapter selectedPaths isEmpty'.
fix value: #SpTreeTableAdapterSingleSelectionTest value: #testSelectPresenterItemSetsSelectedIndexInWidget value: [ :source | source includesSubstring: 'self skip' ] value: 'testSelectPresenterItemSetsSelectedIndexInWidget

	presenter selectItem: 20.
	self assert: (self adapter selectedPaths includes: #(2))'.
fix value: #SpTreeTableAdapterSingleSelectionTest value: #testUnselectPresenterItemUnselectsIndexInWidget value: [ :source | source includesSubstring: 'self skip' ] value: 'testUnselectPresenterItemUnselectsIndexInWidget

	presenter
		selectItem: 20;
		unselectItem: 20.
	self assert: self adapter selectedPaths isEmpty'.
fix value: #SpTreeAdapterMultipleSelectionTest value: #testSelectPresenterItemSetsSelectedIndexInWidget value: [ :source | source includesSubstring: 'self skip' ] value: 'testSelectPresenterItemSetsSelectedIndexInWidget

	presenter selectItem: 20.
	self assert: (self adapter selectedPaths includes: #(2))'.
fix value: #SpTreeTableAdapterMultipleSelectionTest value: #testSelectPresenterItemSetsSelectedIndexInWidget value: [ :source | source includesSubstring: 'self skip' ] value: 'testSelectPresenterItemSetsSelectedIndexInWidget

	presenter selectItem: 20.
	self assert: (self adapter selectedPaths includes: #(2))'.
