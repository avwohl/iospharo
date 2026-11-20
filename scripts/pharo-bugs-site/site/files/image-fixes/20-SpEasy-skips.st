"SpEasy-skips: the Easy presenters lacked the API that the shared Spec2 list and
 tree tests call, so ten test methods were overridden with 'self skip'.  Add the
 delegating API, then remove each 'self skip' override so the inherited test runs."
add value: #SpAbstractEasyListViewPresenter value: #whenSortingBlockChangedDo: value: 'api' value: 'whenSortingBlockChangedDo: aBlock
	"Inform when the sorting block changed.  See SpAbstractListPresenter>>#whenSortingBlockChangedDo:"

	contentView whenSortingBlockChangedDo: aBlock'.
add value: #SpAbstractEasyTreeListViewPresenter value: #pathIndexOf: value: 'private' value: 'pathIndexOf: anArrayOfElements

	^ contentView pathIndexOf: anArrayOfElements'.
add value: #SpAbstractEasyTreeListViewPresenter value: #pathItemOf: value: 'private' value: 'pathItemOf: aPath
	"answer an array of items following a path. 
	 e.g. #(1 2 3) = { item1. item2. item3 }"

	^ contentView pathItemOf: aPath'.
#( #(SpEasyColumnViewPresenterTest #(testDisableActivationDuring testSetSortingBlockBeforeItems testSortingBlock))
  #(SpEasyListViewPresenterTest #(testDisableActivationDuring))
  #(SpEasyTreeColumnViewPresenterTest #(testDisableActivationDuring testPathIndexOf testPathItemOf))
  #(SpEasyTreeListViewPresenterTest #(testDisableActivationDuring testPathIndexOf testPathItemOf))) do: [ :p |
	(Smalltalk globals at: p first ifAbsent: [ nil ]) ifNotNil: [ :c |
		p last do: [ :s |
			((c includesSelector: s) and: [ ((c >> s) sourceCode includesSubstring: 'self skip') and: [ (c >> s) sourceCode size < 200 ] ]) ifTrue: [
				c removeSelector: s.
				Stdio stdout nextPutAll: 'REMOVED ', c name, '>>', s; lf; flush ] ] ] ].
