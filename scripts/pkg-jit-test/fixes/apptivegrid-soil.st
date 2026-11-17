"apptivegrid-soil: 30 tests do not pass, all skipped.  Causes, by group:

1. SoilIndexedDictionaryTest>>testAReplayDoesNotAddAValueTheIndexHoldsAgain
   (BTree case).  SoilBTreePage>>cleanRemovedIn: dropped a removed entry by its
   key.  With duplicate keys the binary search for the key can land on the live
   entry next to the removed one, so the cleaner removed the live entry and
   kept the removed one.  The repair drops the removed entries themselves
   (as SoilSkipListPage>>cleanRemoved already does).  The test lost its
   BTree skip.

2. SoilIndexedDictionaryTest>>testConcurrentIsEmpty (both backends).  The
   iterator of a transaction answered #size from the header page of the index.
   The header counts the newest committed state, so a transaction that began
   before another commit saw the count of the other commit.  The repair adds
   SoilRestoringIndexIterator>>size: when the database moved past the read
   version of the iterator, the size is counted with the same visibility rule
   that #do: uses.  The test lost its skip (issue 1137 of the package).

3. SoilIndexedDictionaryTest>>testRemoveKeyWithTwoTransactions (both
   backends).  The test is stale.  Its comment says that the commit conflict
   cannot be detected yet.  Soil detects the conflict now
   (SoilIndexHasConcurrentChange, a subclass of SoilObjectHasConcurrentChange,
   see testRemovingAKeyAnotherTransactionRemovedIsAConflict).  The test is
   brought to the present design: the commit conflict is checked as before,
   and the size is read in a new transaction after the conflict.

4. SoilTest>>testRootSendingClass.  The cause is in the package: commit c8aee57a
   took SoilObjectProxy>>class out (the message says: called too often, breaks the inspector),
   so ProtoObject>>class answers SoilObjectProxy for a proxy, and a #= that
   compares #class (OrderedCollection>>remove: sends real = proxy) does not
   find its element.  The author skipped the test in commit abb46f0c.  The
   repair puts SoilObjectProxy>>class back (an owner decision to review) and
   uses a small class of the test package (SoilTestEqualsUsingClass) in place
   of the deprecated AST alias RBComment.
   corrected by the second reader: the first version claimed that the proxy
   override still existed.

5. Lint tests: SoilCleanCodeTest>>testNoUnusedClasses (and the copy that
   SoilResidentCleanCodeTest inherits), SoilSerializerCleanCodeTest>>
   testNoUnimplementedCalls and testNoUnsentMessages.  The skips say
   that the test is not critical.  SoilDatabaseMigrationV2 is found by
   SoilDatabaseMigration class>>migratesVersion: with #allSubclasses, so the
   lint test needs the isUsed override that the test comment names.
   SoilObjectVisitor has no sender, no subclass and no test: dead code, it is
   removed in the image.  The three tests lost the skip.

Also: SoilWindowsFileLockTest>>testErrorDescriptions and >>testWinOverlappedStructure
lose their isWindows skip (no call into kernel32).

Not repaired here (see the analysis text): the three #testCodeCoverage tests,
Soil-Core's #testNoUnsentMessages, the six SoilIndexIteratorTest cases that
cannot apply, and the eight SoilWindowsFileLockTest tests that call kernel32."

fix value: #SoilBTreePage value: #cleanRemovedIn:
	value: [ :source | source includesSubstring: 'self remove: removedItem key for: index' ]
	value:
'cleanRemovedIn: index
	"Drop the removed items themselves.  A removal by key takes any entry under
	the key when the keys are not unique, and that can be the live entry next to
	the removed one."
	items removeAllSuchThat: [ :each | each value isRemoved ].
	needWrite := true'.

add value: #SoilRestoringIndexIterator value: #size value: 'accessing' value:
'size
	"The header of the index counts the newest committed state.  A reader whose
	read version is older than the database version may see other entries, so it
	counts the entries that it sees, the way #do: does."
	| count item |
	(readVersion isNil or: [ journal isNil ]) ifTrue: [ ^ super size ].
	readVersion >= journal soil control databaseVersion ifTrue: [ ^ super size ].
	count := 0.
	currentPage := nil.
	[ (item := self basicNextAssociation) notNil ] whileTrue: [
		(self restoreItem: item) ifNotNil: [ count := count + 1 ] ].
	^ count'.

fix value: #SoilIndexedDictionaryTest value: #testAReplayDoesNotAddAValueTheIndexHoldsAgain
	value: [ :source | source includesSubstring: 'backend = SoilBTree ifTrue: [ ^ self skip ]' ]
	value:
'testAReplayDoesNotAddAValueTheIndexHoldsAgain
	"a replay applies journals whose changes are already on disk, so an add finds its
	pair on a page read back from disk - as a byte array, while the journal hands it a
	SoilObjectId. Compared as they were, every replayed add appended a second entry
	for its pair, and the index answered a value twice and a size too large.

	On a BTree the cleaning of removed entries (SoilBTreePage>>#cleanRemovedIn:) used
	to drop an entry by its key, and with duplicate keys that can be the live entry
	next to the removed one."
	| tx checkpoint values |
	tx := soil newTransaction.
	dict index allowDuplicateKeys.
	tx root: dict.
	tx commit.
	checkpoint := soil control lastCheckpoint.

	tx := soil newTransaction.
	tx root
		at: #testkey put: ''one'';
		at: #testkey put: ''two''.
	tx commit.
	tx := soil newTransaction.
	values := tx root at: #testkey.
	tx root removeKey: #testkey value: values first.
	tx commit.
	"back to a checkpoint before both commits - what a crash after them would have left"
	soil control checkpoint: checkpoint.
	self reopen.

	tx := soil newTransaction.
	values := tx root at: #testkey.
	self assert: values size equals: 1.
	self assert: values first equals: ''two''.
	self assert: tx root size equals: 1.
	tx abort'.

fix value: #SoilIndexedDictionaryTest value: #testConcurrentIsEmpty
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testConcurrentIsEmpty
	| tx1 tx2 tx3 |
	tx1 := soil newTransaction.
	tx1 root: dict.
	dict
		at: #one put: #onevalue.
	tx1 commit.
	tx2 := soil newTransaction.
	"After creating tx2 we open a concurrent transaction and remove the key of
	the dictionary, which should still be visible to tx2"
	tx3 := soil newTransaction.
	tx3 root removeKey: #one.
	tx3 commit.
	"invisible: tx2 still has the key"
	self deny: tx2 root isEmpty.
	self assert: tx2 root size equals: 1'.

fix value: #SoilIndexedDictionaryTest value: #testRemoveKeyWithTwoTransactions
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testRemoveKeyWithTwoTransactions
	"the second transaction sees the key at its read version and can remove it in
	its own view.  Its commit is a concurrent change: the first transaction removed
	the key and committed.  The conflict is an SoilIndexHasConcurrentChange, a
	SoilObjectHasConcurrentChange."
	| tx tx2 |
	tx := soil newTransaction.
	tx root: dict.
	dict at: #one put: #onevalue.
	dict at: #two put: #twovalue.
	tx commit.
	"we create two transactions"
	tx := soil newTransaction.
	tx2 := soil newTransaction.
	"remove the key"
	tx2 root removeKey: #two.
	tx2 commit.
	"check that we can still see in the first tr"
	self assert: (tx root at: #two) equals: #twovalue.
	"but removeKey: does not see it, we can remove it without error"
	tx root removeKey: #two ifAbsent: [ self fail ].
	"in its own view the first transaction holds one key now"
	self assert: tx root size equals: 1.
	"but commiting it will fail, as we have commited the remove in t2"
	self should: [ tx commit ] raise: SoilObjectHasConcurrentChange.
	"the first removal stands and the other key is untouched"
	tx := soil newTransaction.
	self assert: (tx root at: #one) equals: #onevalue.
	self assert: (tx root at: #two ifAbsent: [ nil ]) isNil.
	self assert: tx root size equals: 1.
	tx abort'.

(Smalltalk globals includesKey: #SoilTestEqualsUsingClass) ifFalse: [
	(Object << #SoilTestEqualsUsingClass
		slots: { #text };
		package: 'Soil-Core-Tests';
		yourself) install ].

add value: #SoilTestEqualsUsingClass value: #text value: 'accessing' value:
'text
	^ text'.

add value: #SoilTestEqualsUsingClass value: #text: value: 'accessing' value:
'text: aString
	text := aString'.

add value: #SoilTestEqualsUsingClass value: #= value: 'comparing' value:
'= anObject
	"compares the class by sending #class to the other object, as the AST nodes of
	the image do"
	self == anObject ifTrue: [ ^ true ].
	^ self class = anObject class and: [ text = anObject text ]'.

add value: #SoilTestEqualsUsingClass value: #hash value: 'comparing' value:
'hash
	^ text hash'.

add value: #SoilObjectProxy value: #class value: 'class membership' value:
'class
	"ProtoObject>>#class would answer SoilObjectProxy.  The proxy stands for its real
	object, so it answers the class of the real object (as the package did until
	commit c8aee57a, which took this method out)."
	^ self soilRealObject class'.

fix value: #SoilTest value: #testRootSendingClass
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testRootSendingClass
	"We have a root in a Dictionary and that object overrides #= that checks using #class.
	This tests that the proxy forwards #class correctly"

	| tx collection objectUsingClassinQueals |
	tx := soil newTransaction.
	collection := OrderedCollection new.
	tx root: collection.
	objectUsingClassinQueals := SoilTestEqualsUsingClass new
		text: ''Some sample text'';
		yourself.
	tx makeRoot: objectUsingClassinQueals.
	collection add: objectUsingClassinQueals.
	tx markDirty: objectUsingClassinQueals.
	tx markDirty: collection.
	tx commit.

	"now lets try to remove the object in a new transaction, note that the #remove uses #= which sends #class"
	tx := soil newTransaction.
	tx root remove: objectUsingClassinQueals.
	tx commit.
	tx := soil newTransaction.
	self assert: tx root isEmpty.
	tx abort'.

add value: #'SoilDatabaseMigration class' value: #isUsed value: 'testing' value:
'isUsed
	"the subclasses are found by #migratesVersion: with #allSubclasses, and no
	method names one of them"
	^ true'.

(Smalltalk globals at: #SoilObjectVisitor ifAbsent: [ nil ]) ifNotNil: [ :visitorClass |
	(visitorClass isUsed not)
		ifTrue: [
			visitorClass removeFromSystem.
			Stdio stdout nextPutAll: 'FIXED SoilObjectVisitor removed (no sender, no subclass, no test)'; lf; flush ]
		ifFalse: [
			Stdio stdout nextPutAll: 'SKIP SoilObjectVisitor (has a subclass or a reference)'; lf; flush ] ].

fix value: #SoilCleanCodeTest value: #testNoUnusedClasses
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testNoUnusedClasses
	"Fail if there are Classes that are not used. They should either be tested or deleted.
	(check how to override #isUsed for cases where classes are discovered reflectively)"
	| found  knownviolations |
	found := self packageToCheck definedClasses reject: [ :class | class isUsed].

	knownviolations := #(SoilSkipListDictionary SoilBTreeDictionary).
	found := found reject: [:class | knownviolations includes: class name  ].

	self
		assert: found isEmpty
		description: (''the following classes are unused: '', found asString)'.

fix value: #SoilSerializerCleanCodeTest value: #testNoUnimplementedCalls
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testNoUnimplementedCalls

	| remaining |
	"To tag selectors send that are not implemented that should not trigger this rule, use the pragma
	<ignoreNotImplementedSelectors: #(selector:to:be:ignored)>"

	remaining := self packageToCheck methods select: [ :meth |
		             | ignored |
		             ignored := meth allIgnoredNotImplementedSelectors.
						 meth messages anySatisfy: [ :m |
			             m isSelectorSymbol not and: [
				             (ignored includes: m) not ] ] ].

	self assert: remaining isEmpty description: (''the following methods send selectors that do not exist'', remaining asString)'.

fix value: #SoilSerializerCleanCodeTest value: #testNoUnsentMessages
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testNoUnsentMessages
	"Fail if there are methods implemented whose selectors is not sent anywhere in Pharo.
	Please add a test or remove the method!"

	| found methods validExceptions |
	found := self packageToCheck allUnsentMessages.
	methods := self packageToCheck methods select: [ :method | found includes: method selector ].
	methods := methods reject: [:method | method hasPragmaNamed: #inspectorPresentationOrder:title:
  ].
  validExceptions := { }.
	"No other exceptions beside the ones mentioned here should be allowed"
	 SystemVersion current major = 11 ifTrue: [
		validExceptions := {
		SoilMaterializer>>#nextCompiledBlock12: .
		SoilMaterializer>>#nextCompiledMethod12: }
	].

	methods := methods asOrderedCollection
								removeAll: validExceptions;
								yourself.


	self
		assert: methods isEmpty
		description: (''the following selectors are implemented, but never send'', methods asString)'.

fix value: #SoilWindowsFileLockTest value: #testErrorDescriptions
	value: [ :source | source includesSubstring: 'OSPlatform current isWindows ifFalse: [ ^ self skip ]' ]
	value:
'testErrorDescriptions
	"Test error description helper method.  The method only answers texts for numbers,
	so it runs on every platform."

	"Test known error codes"
	self assert: (SoilWindowsFileLock errorDescription: 5) equals: ''Access denied''.
	self assert: (SoilWindowsFileLock errorDescription: 6) equals: ''Invalid file handle''.
	self assert: (SoilWindowsFileLock errorDescription: 33) equals: ''Lock violation (region already locked)''.
	self assert: (SoilWindowsFileLock errorDescription: 158) equals: ''Region not locked''.

	"Test unknown error code"
	self assert: ((SoilWindowsFileLock errorDescription: 9999) includesSubstring: ''Unknown error'')'.

fix value: #SoilWindowsFileLockTest value: #testWinOverlappedStructure
	value: [ :source | source includesSubstring: 'OSPlatform current isWindows ifFalse: [ ^ self skip ]' ]
	value:
'testWinOverlappedStructure
	"Test WinOverlapped structure initialization and field access.  The structure is
	plain memory, no call into kernel32, so it runs on every platform."

	| overlapped |
	overlapped := SoilWinOverlappedStruct new.

	"Test field setters and getters"
	overlapped offset: 16r12345678.
	self assert: overlapped offset equals: 16r12345678.

	overlapped offsetHigh: 16rABCDEF00.
	self assert: overlapped offsetHigh equals: 16rABCDEF00.

	overlapped internal: 0.
	self assert: overlapped internal equals: 0.

	overlapped internalHigh: 0.
	self assert: overlapped internalHigh equals: 0.

	overlapped hEvent: ExternalAddress null.
	self assert: overlapped hEvent getHandle equals: ExternalAddress null'.
