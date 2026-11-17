"JavierLarre/pharo-test-analysis: 4 tests of TSDAssertionLessDetectorTest
(testHasAssertion, testHasConnectedAssertion, testHasConnectedAssertionTwice,
testIsAssertionLess) fail the same way on stock Cog and on our VM, with
'MessageNotUnderstood: RTDHelperVisitor >> #messages'.  No VM defect.

Cause: commit cbc92d1 of the package (removed unnecessary hierarchy) made
RTDHelperVisitor a subclass of Object.  Before, it was a subclass of
RTDMessageVisitor and inherited #messages.  Now it keeps its own
RTDMessageVisitor in the instance variable messageVisitor and forwards
#analyzeMethod:, #helpers and #helperSelectors, but not #messages.
TSDAssertionLessDetector>>hasNoAssertionSelectors still sends
'self methodVisitor messages does: ... includesMessage: ...' to the
RTDHelperVisitor, so each detection of an assertion-less test raises the
error.  The package code is wrong (an unfinished refactoring); the tests are
right.  The repair adds RTDHelperVisitor>>messages, which answers the messages
that the inner message visitor collected, the same object that the old
inherited accessor answered."

add value: #RTDHelperVisitor value: #messages value: 'accessing'
	value:
'messages

	^ messageVisitor messages'.
