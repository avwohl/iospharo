fix value: #ClyAsyncQueryResultTest value: #testItemsChangedNotificationShouldResetItems
	value: [ :src | src includesSubstring: 'expectedFailure' ]
	value: 'testItemsChangedNotificationShouldResetItems
	"The inherited test expects itemsChanged to reset the items. An async result must not:
	ClyAsyncQueryResult>>initializeItems says it never resets existing items, so that the
	browser can swap the items when the new result is ready. itemsChanged only marks the
	result as needing a rebuild and keeps the items built so far."

	queryResult initializeItems.
	self assert: queryResult isBuilt.

	queryResult itemsChanged.

	self assert: queryResult isBuilt.
	self assert: queryResult items isNotNil.
	self assert: queryResult needsRebuild'.
