"SpRadioButtonInteractionTest: the presenter under test is a composable presenter
 (SpTestPresenterWithThreeRadioButtons).  A composable presenter has no adapter and
 hide/show on it only change its own visible state; the widget is the adapter of a
 presenter in its layout.  The two tests were skipped with this very remark.  They now
 hide/show the radio button presenter, which owns the widget, and check the widget."
fix value: #SpRadioButtonInteractionTest value: #testHidePresenterHidesWidget
 value: [ :src | src includesSubstring: 'self skip' ]
 value: 'testHidePresenterHidesWidget

	"A composable presenter has no adapter: hide the radio button presenter that owns the widget."
	self openInstance.
	self presenter radio1 hide.
	self deny: self presenter radio1 adapter isVisible'.
fix value: #SpRadioButtonInteractionTest value: #testShowPresenterShowsWidget
 value: [ :src | src includesSubstring: 'self skip' ]
 value: 'testShowPresenterShowsWidget

	"A composable presenter has no adapter: show the radio button presenter that owns the widget."
	self openInstance.
	self presenter radio1 hide.
	self presenter radio1 show.
	self assert: self presenter radio1 adapter isVisible'.
